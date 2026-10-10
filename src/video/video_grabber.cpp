#include "video/video_grabber.hpp"

#include <QMutex>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>

#include "utils/logger.hpp"
#include "utils/timestamp.hpp"
#include "video/fps_readout.hpp"
#include "video/gige_action_command.hpp"
#include "video/gige_bandwidth.hpp"
#include "video/param_mapping.hpp"

#if defined(MOSAIC_HAVE_CAMERAS)
#define NOMINMAX // prevent Windows.h min/max macros breaking std::min
#include <pylon/PylonIncludes.h>
#endif

namespace mosaic {

#if defined(MOSAIC_HAVE_CAMERAS)
namespace {

// The SFNC 2.0 name of a float node if this camera has it, otherwise its
// SFNC 1.x "…Abs" spelling. Decided by node *existence* (GetNode), never by
// whether a read throws: a node that exists but is momentarily locked by an
// auto function would otherwise be misread as absent, and the write would go
// to a name that does not exist with a misleading warning. Same check the
// BlackLevel/BalanceRatio code below already uses.
const char* float_node_name(GenApi::INodeMap& cam, const char* sfnc2, const char* sfnc1) {
    return cam.GetNode(sfnc2) != nullptr ? sfnc2 : sfnc1;
}

} // namespace
#endif

struct VideoGrabber::Impl {
    int cameraIndex;
    // Bound once, for this grabber's entire lifetime, to an element of the
    // VideoSettings::cameras vector passed to VideoManager::open() (see
    // VideoSettings::kMaxCameras's doc comment in settings.hpp). Any
    // reallocation of that vector after this reference is taken — from
    // push_back()ing past capacity anywhere (VideoSettingsW::add_camera(),
    // discover_cameras(), or a future call site) — invalidates this
    // reference and produces a use-after-free the next time
    // apply_image_params() (below) reads d->params.*. If you add a new way
    // to grow VideoSettings::cameras while a VideoManager session may be
    // open, make sure it either respects kMaxCameras or reopens
    // VideoManager (which rebinds fresh references) before any control
    // edit can reach here.
    const CameraParameters& params;
    RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer;

    std::atomic<bool> deviceOpen{false};
    std::atomic<bool> closeCalled{false};
    std::atomic<bool> pylonInitialized{false};
    std::atomic<int64_t> frameCounter{0};
    std::atomic<int64_t> dropCounter{0};
    // Cumulative GVSP-incomplete-frame count — see incomplete_frames_total()'s
    // doc comment in video_grabber.hpp. Incremented alongside run_pylon_loop()'s
    // own local, periodically-reset warnCount; this one never resets.
    std::atomic<int64_t> incompleteFrameTotal{0};
    std::atomic<double> currentFps{0.0};
    std::atomic<int64_t> lastFrameElapsedNs{-1};

    // Set true only once run_pylon_loop() has actually reached Pylon's
    // StartGrabbing() call (or immediately in the stub loop, which has no
    // equivalent setup step) — NOT merely once start_grabbing() has
    // returned, which only confirms QThread::start() scheduled the thread.
    // Means "the stream grabber is armed", not "frames are arriving" — it is
    // stored before any frame has been received. Reset false at the start of
    // every start_grabbing() call. See is_actually_grabbing().
    std::atomic<bool> actuallyGrabbing{false};

    // GigE Vision Action Command trigger state — see
    // action_command_ready()/action_device_key()/action_broadcast_address().
    // Written in open(): on the main thread before the grab thread starts, and
    // again on the grab thread when it reopens a lost camera (see
    // reconnect_after_loss()) while the main thread may be reading — hence the
    // atomics. The broadcast address is a QString and cannot be atomic; a
    // reconnect only writes it if it changed, which it cannot do without the
    // camera moving to another subnet (logged when it does).
    std::atomic<bool> actionCommandReady{false};
    std::atomic<uint32_t> actionDeviceKey{0};
    QString actionBroadcastAddress;

    // The camera went away while grabbing (a pulled cable, a power cut) and the
    // grab thread is trying to reopen it — see reconnect_after_loss(). While
    // set, deviceOpen is false but the camera still belongs to this session:
    // start_grabbing() starts the thread anyway, so a camera lost during a
    // recording keeps being retried by the preview that follows.
    std::atomic<bool> deviceLost{false};
    // Successful reconnects since this grabber was created — see reconnects().
    std::atomic<int64_t> reconnectCount{0};
    // Set by reconnect_after_loss() around its open() attempts, so a camera
    // that is still unplugged does not log "Cannot open device" every 2 s.
    bool quietOpenFailure = false;

    // Set by apply_live_params() (typically called from the GUI thread) and
    // consumed by the grab thread's own loop. Pylon's CInstantCamera/node
    // map is not documented as safe for concurrent access from a second
    // thread while RetrieveResult() is in-flight, so parameter writes are
    // deferred to — and only ever performed by — the grab thread itself.
    std::atomic<bool> liveApplyPending{false};

    // Same deferred-to-grab-thread pattern as liveApplyPending, but for a
    // one-shot full-resolution frame request (see
    // VideoGrabber::request_calibration_frame()).
    std::atomic<bool> calibrationFramePending{false};
    // Echoed back verbatim on calibration_frame_ready() — see that signal's
    // doc comment. Written before calibrationFramePending is set, read after
    // calibrationFramePending is claimed via exchange(), so the token always
    // corresponds to the request that set the pending flag.
    std::atomic<uint64_t> calibrationFrameToken{0};

    // GevTimestampTickFrequency (Hz), read once in open() and reused by the
    // grab thread to convert each frame's chunk timestamp from device ticks
    // to nanoseconds. Written in open() on the main thread before the grab
    // thread is started (QThread::start() establishes the happens-before
    // edge), or on the grab thread itself when it reopens a lost camera; only
    // ever read by the grab thread, so no synchronization is needed.
    int64_t tickFreqHz = 0;

    // The frame size the camera actually delivers, read back from Width/Height
    // at the end of open() — -1 until then, or if the read failed. See
    // frame_width(). Atomic for the same reason as actionCommandReady: a
    // reconnect rewrites them on the grab thread.
    std::atomic<int> frameWidth{-1};
    std::atomic<int> frameHeight{-1};
    std::atomic<int> frameOffsetX{-1};
    std::atomic<int> frameOffsetY{-1};
    // The camera's own ResultingFrameRate as read at the end of open(): the
    // most it says it can deliver with the settings just applied. -1 when the
    // node was unavailable. Unlike resultingFps this is not held back until a
    // warm-up has passed — it is shown as "the camera's limit for this crop"
    // straight after a change, and refined by the measured rate later.
    std::atomic<double> openMaxFps{-1.0};
    // The pixel format the camera reported after open() configured it, empty
    // until then or when it could not be read. What is really on the wire:
    // the setting is only a request (see the PixelFormat write in open()).
    // A QString cannot be atomic, so guarded; see pixel_format().
    mutable QMutex pixelFormatMutex;
    QString pixelFormat;
    // Last exposure/gain logged by log_exposure_if_changed() — grab thread
    // only (refresh_achievable_fps() runs there after open()), so plain values.
    double loggedExposureUs = -1.0;
    double loggedGain       = -1.0;

    // The camera's real ResultingFrameRate. Atomic, unlike most of Impl:
    // refresh_achievable_fps() writes it from open() on the main thread *and*
    // from the grab thread (both the post-live-apply branch and the ~2s
    // periodic self-refresh in run_pylon_loop()), while achievable_fps() is
    // read from the main thread (camera_stats(), and the per-camera
    // achievable-rate readout in CameraCardW) *and* from the
    // ActionCommandTicker thread, which re-reads it periodically to
    // self-correct its firing period. -1.0 if never measured (stub builds, or
    // the node genuinely unavailable). See achievable_fps().
    std::atomic<double> resultingFps{-1.0};

    // The value most recently announced via achievable_fps_changed(). The
    // change-detection threshold must be measured against what the UI is
    // actually showing, not against the previous reading: resultingFps is
    // rewritten on every ~2s refresh whether or not a signal was sent, so
    // comparing against it lets a rate that drifts by less than
    // k_fps_change_epsilon per refresh wander arbitrarily far from the
    // displayed figure without ever emitting — leaving the card reporting a
    // long-dead number, and never raising its "below the configured rate"
    // warning even while refresh_achievable_fps()'s own log warning fires.
    std::atomic<double> lastEmittedFps{-1.0};

    // When this camera's grab thread most recently reached Pylon's
    // StartGrabbing() — nullopt if it has never actually started grabbing
    // yet (or was stopped and hasn't been restarted). Written only in
    // start_grabbing() (main thread, resets to nullopt) and run_pylon_loop()
    // (grab thread, sets the real timestamp) — the grab thread never runs
    // concurrently with those main-thread writes (QThread::start()/wait()
    // establish the happens-before edges), so a plain optional is safe here,
    // same reasoning as tickFreqHz/resultingFps above. Used by
    // refresh_achievable_fps() to gate on real elapsed acquisition time —
    // see that function's own doc comment for why.
    std::optional<SteadyClock::time_point> grabbingStartedAt;

#if defined(MOSAIC_HAVE_CAMERAS)
    Pylon::CInstantCamera camera;
    Pylon::CImageFormatConverter converter; // converts any pixel format → BGR8
#endif

    explicit Impl(int idx, const CameraParameters& p, RingBuffer<std::shared_ptr<VideoFrame>>& buf)
        : cameraIndex(idx), params(p), frameBuffer(buf) {}
};

VideoGrabber::VideoGrabber(int cameraIndex, const CameraParameters& params,
                           RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer, QObject* parent)
    : QThread(parent), d(std::make_unique<Impl>(cameraIndex, params, frameBuffer)) {}

VideoGrabber::~VideoGrabber() {
    stop_grabbing();
    close();
}

// ── Device discovery ──────────────────────────────────────────────────────

QVector<DiscoveredCamera> VideoGrabber::enumerate_devices() {
    QVector<DiscoveredCamera> out;
#if defined(MOSAIC_HAVE_CAMERAS)
    // Every PylonInitialize() must be matched by a PylonTerminate() — see the
    // identical discipline (and its rationale: stale device/socket state
    // corrupting the next open session) in VideoGrabber::open()/close(). This
    // was previously missing here, permanently leaking one reference for the
    // life of the process and leaving Pylon's environment never fully torn
    // down between camera open/close cycles.
    Pylon::PylonInitialize();
    try {
        Pylon::DeviceInfoList_t deviceList;
        Pylon::CTlFactory::GetInstance().EnumerateDevices(deviceList);
        out.reserve(static_cast<int>(deviceList.size()));
        for (const auto& info : deviceList) {
            DiscoveredCamera cam;
            cam.serialNumber = QString::fromLocal8Bit(info.GetSerialNumber().c_str());
            cam.modelName    = QString::fromLocal8Bit(info.GetModelName().c_str());
            // "IpAddress" is a GigE-transport-specific info property — not
            // every transport layer (USB3, CameraLink, ...) has one, so this
            // is a best-effort lookup via the generic property accessor
            // rather than a GigE-typed device-info subclass.
            Pylon::String_t ip;
            if (info.GetPropertyValue("IpAddress", ip)) {
                cam.ipAddress = QString::fromLocal8Bit(ip.c_str());
            }
            out.append(cam);
        }
    } catch (const Pylon::GenericException& e) {
        log_error(QString("[VideoGrabber] enumerate_devices: %1")
                      .arg(QString::fromLocal8Bit(e.GetDescription())));
    }
    Pylon::PylonTerminate();
#endif
    return out;
}

// ── Device management ──────────────────────────────────────────────────────

bool VideoGrabber::open() {
    if (d->deviceOpen.load()) {
        return true;
    }

#if defined(MOSAIC_HAVE_CAMERAS)
    // Phase 1: attach and open the physical device — hard failure if this fails.
    try {
        // Once per grabber, not once per open(): a reconnect calls open()
        // again without close(), and must not take a second reference that
        // close() would never release.
        if (!d->pylonInitialized.exchange(true)) {
            Pylon::PylonInitialize(); // balanced by PylonTerminate() in close()
        }

        if (d->params.serialNumber.isEmpty()) {
            d->camera.Attach(Pylon::CTlFactory::GetInstance().CreateFirstDevice());
        } else {
            Pylon::CDeviceInfo info;
            info.SetSerialNumber(d->params.serialNumber.toStdString().c_str());
            d->camera.Attach(Pylon::CTlFactory::GetInstance().CreateDevice(info));
        }
        d->camera.Open();

    } catch (const Pylon::GenericException& e) {
        if (!d->quietOpenFailure) {
            log_error(QString("[Camera %1] Cannot open device (serial %2): %3")
                          .arg(d->cameraIndex)
                          .arg(d->params.serialNumber)
                          .arg(QString::fromLocal8Bit(e.GetDescription())));
        }
        return false;
    }

    // Phase 2: configure camera parameters.  Each param is tried independently
    // so a bad setting (e.g. gain out of range) logs a warning instead of
    // killing the entire camera.
    {
        auto& cam = d->camera.GetNodeMap();
        using Pylon::CBooleanParameter;
        using Pylon::CEnumParameter;
        using Pylon::CFloatParameter;
        using Pylon::CIntegerParameter;

        const int idx = d->cameraIndex;
        auto try_set  = [&](const char* name, auto setter) {
            try {
                setter();
            } catch (const Pylon::GenericException& e) {
                log_warning(QString("[Camera %1] Skipping '%2': %3")
                                 .arg(idx)
                                 .arg(name)
                                 .arg(QString::fromLocal8Bit(e.GetDescription())));
            }
            // Broadened beyond Pylon::GenericException so a node write that
            // throws something else (a different exception type, std::bad_alloc,
            // ...) degrades to a skipped parameter — same as any other bad
            // setting — instead of escaping uncaught up through run_pylon_loop()
            // and QThread::run(), which is fatal (std::terminate()).
            catch (const std::exception& e) {
                log_warning(QString("[Camera %1] Skipping '%2': unexpected exception: %3")
                                 .arg(idx)
                                 .arg(name)
                                 .arg(QString::fromUtf8(e.what())));
            } catch (...) {
                log_warning(QString("[Camera %1] Skipping '%2': unknown non-standard exception")
                                 .arg(idx)
                                 .arg(name));
            }
        };

        // Region of interest. Offsets go to zero first, then the size, then
        // the offsets — not size-then-offset. A camera bounds Width by
        // (sensor width - OffsetX), so narrowing to a crop and later widening
        // again had the wider Width rejected against the old, still-applied
        // offset; the camera silently kept the crop while the encoder was
        // configured for the full frame. Interview mode switches between a
        // crop and the full frame on every toggle, which made that the normal
        // path rather than a corner case.
        //
        // Each value is clamped to the node's range and rounded down to its
        // increment (this generation wants multiples of 4 or 8), and any
        // adjustment is logged: a silently different crop is the same failure
        // class as the silently ignored pixel format.
        auto write_roi = [&](const char* name, int64_t want) {
            try_set(name, [&] {
                CIntegerParameter p(cam, name);
                const int64_t lo  = p.GetMin();
                const int64_t inc = std::max<int64_t>(1, p.GetInc());
                int64_t v         = std::clamp(want, lo, p.GetMax());
                v                 = lo + ((v - lo) / inc) * inc;
                if (v != want) {
                    log_warning(QString("[Camera %1] %2 %3 is not one this camera accepts "
                                        "(range %4-%5, step %6); using %7.")
                                    .arg(idx)
                                    .arg(name)
                                    .arg(want)
                                    .arg(lo)
                                    .arg(p.GetMax())
                                    .arg(inc)
                                    .arg(v));
                }
                p.SetValue(v);
            });
        };
        try_set("OffsetX", [&] { CIntegerParameter(cam, "OffsetX").SetValue(0); });
        try_set("OffsetY", [&] { CIntegerParameter(cam, "OffsetY").SetValue(0); });
        write_roi("Width", d->params.width);
        write_roi("Height", d->params.height);
        write_roi("OffsetX", d->params.offsetX);
        write_roi("OffsetY", d->params.offsetY);
        try_set("ReverseX",
                [&] { CBooleanParameter(cam, "ReverseX").SetValue(d->params.reverseX); });
        try_set("ReverseY",
                [&] { CBooleanParameter(cam, "ReverseY").SetValue(d->params.reverseY); });
        // PixelFormat: GigE cameras output raw sensor formats (BayerRG8, YUV…),
        // not BGR8.  Try the configured value; if it fails try the SFNC 1.x
        // packed equivalent.  CImageFormatConverter converts whatever the camera
        // delivers to BGR8packed, so any accepted format is fine.
        try_set("PixelFormat", [&] {
            auto pfp = CEnumParameter(cam, "PixelFormat");
            try {
                pfp.SetValue(d->params.pixelFormat.toStdString().c_str());
            } catch (const Pylon::GenericException&) {
                // SFNC 1.x packed variants
                static const std::vector<std::string> fallbacks = {"BGR8Packed", "BayerRG8",
                                                                   "YCbCr422_8"};
                bool set                                        = false;
                for (const auto& fb : fallbacks) {
                    try {
                        pfp.SetValue(fb.c_str());
                        set = true;
                        break;
                    } catch (...) {
                    }
                }
                // Either way the format actually in use is read back and
                // logged at the end of open(); this only says the request
                // itself did not land.
                log_info(QString("[Camera %1] PixelFormat '%2' is not one this camera accepts; "
                                 "%3")
                             .arg(d->cameraIndex)
                             .arg(d->params.pixelFormat)
                             .arg(set ? QStringLiteral("a fallback was accepted.")
                                      : QStringLiteral("no fallback was accepted either, so it "
                                                       "keeps the format it was in.")));
            }
        });

        if (d->params.specifyFps) {
            // AcquisitionFrameRateEnable is a *boolean* parameter (SFNC 2.0).
            // Some firmware versions don't have it — the rate is always settable.
            try_set("AcquisitionFrameRateEnable",
                    [&] { CBooleanParameter(cam, "AcquisitionFrameRateEnable").SetValue(true); });
            // SFNC 2.0: AcquisitionFrameRate (float)
            // SFNC 1.x: AcquisitionFrameRateAbs (float) — same unit, different name
            // Clamped to the node's range, like every neighbouring numeric
            // write. It used to be the one unclamped write in this file, so an
            // out-of-range rate threw and the camera kept its previous rate.
            // Clamping must not make that silent instead, so say when it bites.
            try_set("AcquisitionFrameRate", [&] {
                CFloatParameter p(
                    cam, float_node_name(cam, "AcquisitionFrameRate", "AcquisitionFrameRateAbs"));
                const double applied = std::clamp(d->params.fps, p.GetMin(), p.GetMax());
                if (applied != d->params.fps) {
                    log_warning(QString("[Camera %1] Frame rate %2 fps is outside what this "
                                        "camera accepts (%3-%4); using %5 fps.")
                                    .arg(d->cameraIndex)
                                    .arg(d->params.fps)
                                    .arg(p.GetMin())
                                    .arg(p.GetMax())
                                    .arg(applied));
                }
                p.SetValue(applied);
            });
        }

        // Hardware trigger input (external TTL pulse on a GPIO line, or a
        // GigE Vision Action Command broadcast, drives frame acquisition
        // instead of the camera free-running at AcquisitionFrameRate).
        //
        // On by default: CameraParameters::hwTriggerEnabled starts *true* with
        // hwTriggerSource "Action1", so room 11's fleet is synchronised out of
        // the box (see that field's doc comment). This comment used to claim
        // the opposite. A camera left triggered with no signal or command ever
        // arriving simply produces zero frames, which is why the source is
        // never auto-detected — it is always what the HW Trigger tab says.
        const bool wantsActionCommand =
            d->params.hwTriggerEnabled && d->params.hwTriggerSource == "Action1";
        d->actionCommandReady = false;

        if (wantsActionCommand) {
            // GigE Vision Action Command: this camera is triggered by a
            // broadcast IssueActionCommand() (see gige_action_command.hpp)
            // instead of a wired signal. ActionSelector/ActionDeviceKey/
            // ActionGroupKey/ActionGroupMask are present on this camera
            // generation per Pylon's own pylon/gige/ActionTriggerConfiguration.h
            // sample, but genuinely unconfirmed on this specific firmware
            // until probed live — fall back to free-run (TriggerMode stays
            // Off below) if unsupported, rather than leaving the camera
            // parked forever waiting for a command that will never arrive.
            //
            // TriggerSelector itself is NOT written here — it's the same
            // "FrameStart" every other trigger source uses (see the shared
            // write below). Real room-11 testing 2026-07-20 proved this
            // camera generation rejects TriggerSource=Action1 while
            // TriggerSelector=AcquisitionStart (a one-shot "start
            // continuous acquisition" selector this code used to select
            // specifically for Action1) — the camera ends up armed
            // (TriggerMode=On) but never actually listening for the action
            // command, a silent black-screen hang. Basler's own reference
            // (CActionTriggerConfiguration::ApplyConfiguration()) only ever
            // pairs TriggerSource=Action1 with TriggerSelector=FrameStart,
            // which means Action Commands must be fired once PER FRAME,
            // continuously, for the whole recording — see
            // VideoManager::ActionCommandTicker.
            try {
                // ActionGroupKey/ActionGroupMask are selector-scoped ("Selected
                // by: ActionSelector" per GenICam SFNC) — the selector must be
                // set first, or the writes below land on whichever action
                // index the device happened to have selected already.
                CIntegerParameter(cam, "ActionSelector")
                    .SetToMinimum(); // selects "Action1" (index 0)
                CIntegerParameter(cam, "ActionDeviceKey")
                    .SetValue(static_cast<int64_t>(d->cameraIndex) + 1);
                CIntegerParameter(cam, "ActionGroupKey")
                    .SetValue(static_cast<int64_t>(k_action_group_key));
                CIntegerParameter(cam, "ActionGroupMask")
                    .SetValue(static_cast<int64_t>(k_action_group_mask));

                // Resolve this camera's own broadcast address at runtime from
                // what it reports (GevCurrentIPAddress/GevCurrentSubnetMask),
                // rather than hardcoding the ops setup script's static
                // per-NIC subnet list — each camera lives on its own
                // isolated /24.
                const auto ip =
                    static_cast<uint32_t>(CIntegerParameter(cam, "GevCurrentIPAddress").GetValue());
                const auto mask = static_cast<uint32_t>(
                    CIntegerParameter(cam, "GevCurrentSubnetMask").GetValue());
                d->actionDeviceKey      = static_cast<uint32_t>(d->cameraIndex) + 1;
                const QString broadcast = ipv4_to_dotted(ipv4_broadcast_address(ip, mask));
                if (broadcast != d->actionBroadcastAddress) {
                    if (d->deviceLost.load()) {
                        // Reconnected on another subnet: the running Action
                        // Command ticker still broadcasts to the old one, so
                        // this camera gets no triggers until it is reopened.
                        log_warning(QString("[Camera %1] Came back on a different subnet "
                                            "(%2, was %3); it will not be triggered until "
                                            "the cameras are reopened.")
                                        .arg(d->cameraIndex)
                                        .arg(broadcast)
                                        .arg(d->actionBroadcastAddress));
                    }
                    d->actionBroadcastAddress = broadcast;
                }
                d->actionCommandReady = true;
                log_info(
                    QString("[Camera %1] Action-command trigger ready — deviceKey=%2 broadcast=%3")
                        .arg(d->cameraIndex)
                        .arg(d->actionDeviceKey.load())
                        .arg(broadcast));
            } catch (const Pylon::GenericException& e) {
                log_warning(QString("[Camera %1] This firmware does not support GigE Vision Action "
                                    "Commands (%2) — falling back to free-run for this session. "
                                    "Switch 'Trigger source' to Line1 or Software instead.")
                                .arg(d->cameraIndex)
                                .arg(QString::fromLocal8Bit(e.GetDescription())));
            }
            emit action_command_capability(d->cameraIndex, d->actionCommandReady);
        }

        // Falls back to free-run if Action1 was requested but unsupported
        // on this firmware — never leave TriggerMode=On with nothing that
        // will ever trigger it. Deliberately does not mutate
        // d->params.hwTriggerEnabled/hwTriggerSource (the user's saved
        // settings) — a firmware/config change is re-probed fresh on the
        // next open(), not silently downgraded and stuck.
        const bool effectiveHwTrigger =
            d->params.hwTriggerEnabled && (!wantsActionCommand || d->actionCommandReady);

        // Every trigger source (Line1/Software/Action1) uses the same
        // per-frame "FrameStart" selector — see the note in the Action1
        // probe above for why Action1 no longer gets a special
        // "AcquisitionStart" case.
        try_set("TriggerSelector",
                [&] { CEnumParameter(cam, "TriggerSelector").SetValue("FrameStart"); });
        try_set("TriggerMode", [&] {
            CEnumParameter(cam, "TriggerMode").SetValue(effectiveHwTrigger ? "On" : "Off");
        });
        if (effectiveHwTrigger) {
            // Basler's own reference Action Command configuration (pylon/gige/
            // ActionTriggerConfiguration.h — CActionTriggerConfiguration::
            // ApplyConfiguration()) explicitly sets AcquisitionMode=Continuous
            // whenever it configures any trigger, and fails closed if it isn't
            // writable, treating it as a hard requirement rather than assuming
            // the camera's current value is already right. Mosaic never wrote
            // this node before (free-run/Line1/Software all worked purely
            // because the camera's power-on default happens to be Continuous)
            // — make the same requirement explicit here for every hardware-
            // triggered source, not just Action1, rather than silently
            // depending on that default forever.
            try_set("AcquisitionMode",
                    [&] { CEnumParameter(cam, "AcquisitionMode").SetValue("Continuous"); });

            // Deliberately NOT routed through try_set(): a failed TriggerSource
            // write must not be silently skipped like a cosmetic parameter.
            // TriggerMode is already "On" at this point — if TriggerSource
            // fails to land on the value we actually want, the camera keeps
            // whatever TriggerSource it had before and ends up armed-and-
            // waiting-forever for a signal that will never arrive — a silent
            // black-screen hang, not just an imprecise setting (this is
            // exactly what happened with the old TriggerSelector=
            // AcquisitionStart design on real room-11 hardware, 2026-07-20 —
            // see the Action1 probe's comment above). We must force back to
            // free-run rather than leave that half-armed state in place.
            try {
                CEnumParameter(cam, "TriggerSource")
                    .SetValue(d->params.hwTriggerSource.toStdString().c_str());
            } catch (const Pylon::GenericException& e) {
                log_warning(QString("[Camera %1] TriggerSource='%2' rejected (%3) — forcing "
                                    "TriggerMode back to Off to avoid an armed-but-never-"
                                    "triggered camera (no frames would ever arrive).")
                                .arg(idx)
                                .arg(d->params.hwTriggerSource)
                                .arg(QString::fromLocal8Bit(e.GetDescription())));
                try_set("TriggerMode", [&] { CEnumParameter(cam, "TriggerMode").SetValue("Off"); });
                // The Action1 probe above only checked the Action* nodes —
                // it never confirmed TriggerSource=Action1 itself would be
                // accepted. If THAT write is what just failed, actionCommandReady
                // is now a lie: VideoManager::arm_and_fire_action_commands()
                // reads it to decide who gets continuous Action Command
                // broadcasts, and this camera is no longer listening for them
                // (TriggerMode just forced to Off above). Reset it and tell
                // the UI, rather than leaving a stale "SUPPORTED" label next
                // to a camera that will never actually trigger.
                if (wantsActionCommand && d->actionCommandReady) {
                    d->actionCommandReady = false;
                    emit action_command_capability(d->cameraIndex, false);
                }
            }
            // SFNC 2.0: TriggerDelay (float, µs)
            // SFNC 1.x: TriggerDelayAbs (float, µs) — same dual-name pattern
            // as ExposureTime/ExposureTimeAbs (see apply_image_params()).
            try_set("TriggerDelay", [&] {
                try {
                    CFloatParameter(cam, "TriggerDelay").SetValue(d->params.hwTriggerDelayUs);
                } catch (const Pylon::GenericException&) {
                    CFloatParameter(cam, "TriggerDelayAbs").SetValue(d->params.hwTriggerDelayUs);
                }
            });
        }

        // Enable the per-frame hardware timestamp chunk (GevTimestamp) so the
        // grab loop can attach a camera-clock timestamp to each VideoFrame,
        // independent of host-side scheduling/network jitter. Non-fatal if
        // this firmware/SDK doesn't support chunk data — hwTimestampNs then
        // stays 0 and only the software elapsed_ns/wall_ns stamps are used.
        try_set("ChunkModeActive", [&] {
            CBooleanParameter(cam, "ChunkModeActive").SetValue(true);
            CEnumParameter(cam, "ChunkSelector").SetValue("Timestamp");
            CBooleanParameter(cam, "ChunkEnable").SetValue(true);
        });
        // And each frame's own exposure time. With auto exposure it changes
        // from frame to frame, and it is part of the delay between a trigger
        // and the frame arriving: Frame Sync Repair subtracts it so cameras
        // exposing for different times still line up on the right tick.
        // Separate from the timestamp chunk, so a firmware without it keeps
        // the timestamps; exposure_us is then left empty.
        try_set("ChunkExposureTime", [&] {
            CEnumParameter(cam, "ChunkSelector").SetValue("ExposureTime");
            CBooleanParameter(cam, "ChunkEnable").SetValue(true);
        });

        // 1500-byte packets: safe default that works regardless of whether
        // jumbo frames are usable end-to-end. Tried raising this to 8192 on
        // 2026-07-15 (the NIC advanced properties report "Jumbo Packet:
        // 9014 Bytes" as configured on all 6 camera NICs) — live-tested
        // against real hardware and it was a severe regression: packet
        // error counts exceeded received-packet counts on every camera
        // (not just one), i.e. the path end-to-end (switch and/or camera)
        // does not actually support 8192-byte GVSP packets even though the
        // NIC-side jumbo frame setting suggested it should. Do not raise
        // this without re-verifying against real hardware first — see
        // [[pylon-genicam-quirks]] memory for the general lesson.
        try_set("GevSCPSPacketSize", [&] {
            auto p = CIntegerParameter(cam, "GevSCPSPacketSize");
            p.SetValue(std::clamp(static_cast<int64_t>(1500), p.GetMin(), p.GetMax()));
        });
        // Keep zero inter-packet delay (burst mode). Experiments showed that
        // any positive SCPD value interacts poorly with the I350 interrupt
        // coalescing on this machine and worsened loss on some cameras.
        // Burst (SCPD=0) leaves the full 23ms gap between frames for the NIC
        // to finish processing, which consistently performs better here.
        try_set("GevSCPD", [&] {
            auto p = CIntegerParameter(cam, "GevSCPD");
            p.SetValue(std::clamp(static_cast<int64_t>(0), p.GetMin(), p.GetMax()));
        });
        // No frame transmission delay (GevSCFTD = 0), written explicitly
        // because a camera keeps the last value written across opens.
        //
        // Cameras used to be staggered by (index × 5 ms) so they would not all
        // send at once onto the same I350-T4 card. Measured on room 11
        // (2026-10-02, full frame, 25 fps asked): the delay adds straight onto
        // each camera's frame time — readout 38.3 ms + delay + 0.5 ms — so the
        // six cameras could manage 25.0 / 22.8 / 20.5 / 18.6 / 17.0 / 15.7 fps,
        // and Action1 triggering paces the whole group at 85% of the slowest:
        // ~13.3 fps (14.45 when Camera 6 was not opening — the ceiling this rig
        // showed for months). Without it every camera reports 25 and the group
        // records at 21.25 fps, with no incomplete (packet-loss) frames on any
        // camera over a 450-frame recording. At 25 fps a full frame leaves ~1.7
        // ms between readouts, so no stagger worth having fits anyway.
        try_set("GevSCFTD", [&] {
            auto p = CIntegerParameter(cam, "GevSCFTD");
            p.SetValue(std::clamp(static_cast<int64_t>(0), p.GetMin(), p.GetMax()));
        });
    }

    // Exposure, gain, gamma, black level, white balance, auto-exposure
    // target, and digital shift — shared with apply_live_params() so a
    // later UI edit doesn't require a full reopen to take effect.
    apply_image_params();

    d->deviceOpen.store(true);

    // Diagnostic: log actual GigE transport settings (best-effort, non-fatal).
    {
        auto& cam2  = d->camera.GetNodeMap();
        auto safe_i = [&](const char* name) -> int64_t {
            try {
                return Pylon::CIntegerParameter(cam2, name).GetValue();
            } catch (...) {
                return -1;
            }
        };
        auto safe_f = [&](const char* name) -> double {
            try {
                return Pylon::CFloatParameter(cam2, name).GetValue();
            } catch (...) {
                return -1.0;
            }
        };
        // SFNC 2.0: ResultingFrameRate — SFNC 1.x (e.g. ace-classic GigE
        // cameras like the acA1920-25gc): ResultingFrameRateAbs. Same
        // dual-name pattern as AcquisitionFrameRate/ExposureTime above;
        // without this fallback this always silently returned -1 here.
        auto safe_f_fallback = [&](const char* primary, const char* fallback) -> double {
            try {
                return Pylon::CFloatParameter(cam2, primary).GetValue();
            } catch (const Pylon::GenericException&) {
                try {
                    return Pylon::CFloatParameter(cam2, fallback).GetValue();
                } catch (...) {
                    return -1.0;
                }
            }
        };
        const int64_t scftd = safe_i("GevSCFTD");
        const int64_t scbwa = safe_i("GevSCBWA");
        const int64_t pktSz = safe_i("GevSCPSPacketSize");
        const int64_t scpd  = safe_i("GevSCPD");
        const int64_t freq  = safe_i("GevTimestampTickFrequency");
        d->tickFreqHz       = freq;
        const double rfps   = safe_f_fallback("ResultingFrameRate", "ResultingFrameRateAbs");
        d->openMaxFps       = rfps;
        // Sensor readout time, where the camera exposes it (ace GigE:
        // ReadoutTimeAbs, µs). Logged because this camera's rate turned out to
        // be bound by readout (plus, until it was removed, the transmission
        // stagger), not exposure or bandwidth — see the GevSCFTD write above.
        const double readoutUs = safe_f("ReadoutTimeAbs");
        log_info(QString("[Camera %1] GigE pkt=%2B scpd=%3 scftd=%4 scbwa=%5 resultFPS=%6 "
                         "exposureAutoLimits=%7-%8us tickFreq=%9Hz readout=%10us")
                     .arg(d->cameraIndex)
                     .arg(pktSz)
                     .arg(scpd)
                     .arg(scftd)
                     .arg(scbwa)
                     .arg(rfps)
                     .arg(d->params.exposureAutoLowerUs)
                     .arg(d->params.exposureAutoUpperUs)
                     .arg(freq)
                     .arg(readoutUs, 0, 'f', 0));
        if (scftd > 0 && freq > 0) {
            log_warning(
                QString("[Camera %1] GevSCFTD=%2 ticks = %3 ms — camera delays frame transmission")
                    .arg(d->cameraIndex)
                    .arg(scftd)
                    .arg(scftd * 1000.0 / freq, 0, 'f', 2));
        }
    }
    // Stores d->resultingFps + warns if the configured fps isn't achievable
    // — shared with apply_live_params()'s live-apply path (via
    // run_pylon_loop()), see refresh_achievable_fps()'s own doc comment for
    // why a one-time-at-open() measurement isn't enough.
    refresh_achievable_fps();

    // Read back actual values for the log message.
    log_info(QString("[Camera %1] reading back dimensions from node map").arg(d->cameraIndex));
    try {
        auto& cam = d->camera.GetNodeMap();
        log_info(QString("[Camera %1] GetNodeMap ok").arg(d->cameraIndex));
        const int w = static_cast<int>(Pylon::CIntegerParameter(cam, "Width").GetValue());
        log_info(QString("[Camera %1] Width=%2").arg(d->cameraIndex).arg(w));
        const int h = static_cast<int>(Pylon::CIntegerParameter(cam, "Height").GetValue());
        // Not ResultingFrameRate read again here: this camera generation
        // only has ResultingFrameRateAbs, and the throw skipped everything
        // below. open() already read it, with the fallback, into openMaxFps.
        const double fps = d->params.specifyFps ? d->params.fps : d->openMaxFps.load();
        d->frameWidth    = w;
        d->frameHeight   = h;
        try {
            d->frameOffsetX = static_cast<int>(Pylon::CIntegerParameter(cam, "OffsetX").GetValue());
            d->frameOffsetY = static_cast<int>(Pylon::CIntegerParameter(cam, "OffsetY").GetValue());
        } catch (...) {
            // Offsets are informational (session metadata); the size, which
            // the encoder needs, was read above.
            d->frameOffsetX = -1;
            d->frameOffsetY = -1;
        }
        if (w != d->params.width || h != d->params.height) {
            // Not fatal — VideoManager::start() sizes the encoder from what is
            // read back here, not from the settings — but the recording will
            // not have the dimensions the operator asked for, so say so.
            log_warning(QString("[Camera %1] Delivers %2\xd7%3, not the configured %4\xd7%5 — "
                                "recording at the camera's size.")
                            .arg(d->cameraIndex)
                            .arg(w)
                            .arg(h)
                            .arg(d->params.width)
                            .arg(d->params.height));
        }
        log_pixel_format_and_bandwidth(w, h);
        log_info(QString("[Camera %1] Opened: %2\xd7%3 @ %4 fps (serial: %5)")
                     .arg(d->cameraIndex)
                     .arg(w)
                     .arg(h)
                     .arg(fps)
                     .arg(d->params.serialNumber));
    } catch (const Pylon::GenericException& e) {
        log_warning(QString("[Camera %1] Pylon exception reading dimensions: %2")
                        .arg(d->cameraIndex)
                        .arg(QString::fromLocal8Bit(e.GetDescription())));
        log_info(QString("[Camera %1] Opened (serial: %2)")
                     .arg(d->cameraIndex)
                     .arg(d->params.serialNumber));
    } catch (const std::exception& e) {
        log_error(QString("[Camera %1] std::exception reading dimensions: %2")
                      .arg(d->cameraIndex)
                      .arg(QString::fromLocal8Bit(e.what())));
    } catch (...) {
        log_error(QString("[Camera %1] unknown exception reading dimensions").arg(d->cameraIndex));
    }

    return true;

#else
    // Stub: always succeeds.
    d->deviceOpen.store(true);
    log_info(QString("[Camera %1] Stub opened (%2×%3 @ %4 fps)")
                 .arg(d->cameraIndex)
                 .arg(d->params.width)
                 .arg(d->params.height)
                 .arg(d->params.fps));
    return true;
#endif
}

#if defined(MOSAIC_HAVE_CAMERAS)
// Reads back the pixel format the camera is really using and says, once, what
// that costs on the wire at this size and rate. Judged against the read-back
// format, never the configured one: the two differ whenever the camera
// rejected the request, and a check against the request can report an
// overload that does not exist or miss one that does (see gige_bandwidth.hpp).
void VideoGrabber::log_pixel_format_and_bandwidth(int width, int height) {
    QString actual;
    try {
        actual = QString::fromLatin1(
            Pylon::CEnumParameter(d->camera.GetNodeMap(), "PixelFormat").GetValue().c_str());
    } catch (const Pylon::GenericException&) {
        // Left empty: reported as unknown, and nothing below is judged.
    }
    {
        QMutexLocker lock(&d->pixelFormatMutex);
        d->pixelFormat = actual;
    }
    if (actual.isEmpty()) {
        log_warning(QString("[Camera %1] Could not read the pixel format back; its bandwidth "
                            "cannot be judged.")
                        .arg(d->cameraIndex));
        return;
    }

    // The rate asked for when it is fixed, else what the camera says it can
    // do. Not capped by the camera's own figure: on this generation that
    // already accounts for the link (it throttles to fit), so capping would
    // hide the very shortfall this is meant to report.
    const double fps        = d->params.specifyFps ? d->params.fps : d->openMaxFps.load();
    const QString requested = same_pixel_format(actual, d->params.pixelFormat)
                                  ? QString()
                                  : QString(" (requested %1, which the camera did not accept)")
                                        .arg(d->params.pixelFormat);
    const double needed     = required_bytes_per_second(width, height, fps, actual);
    if (needed <= 0.0) {
        log_info(QString("[Camera %1] Pixel format %2%3; bandwidth unknown for this format.")
                     .arg(d->cameraIndex)
                     .arg(actual)
                     .arg(requested));
        return;
    }
    const double share = needed / k_gige_line_rate_bytes_per_sec;
    log_info(QString("[Camera %1] Pixel format %2%3: %4\xd7%5 @ %6 fps = %7 MB/s, %8% of a "
                     "gigabit link.")
                 .arg(d->cameraIndex)
                 .arg(actual)
                 .arg(requested)
                 .arg(width)
                 .arg(height)
                 .arg(fps, 0, 'f', 1)
                 .arg(needed / 1e6, 0, 'f', 1)
                 .arg(100.0 * share, 0, 'f', 0));
    if (share > k_gige_link_warn_utilisation) {
        // No format advice: which formats a camera accepts varies by model and
        // firmware, and a rate or crop change works on every one of them.
        log_warning(QString("[Camera %1] This stream needs %2% of a gigabit link, so it will "
                            "lose frames or fall short of %3 fps. At %4\xd7%5 in %6 the link "
                            "carries about %7 fps; lower the frame rate or use a smaller crop.")
                        .arg(d->cameraIndex)
                        .arg(100.0 * share, 0, 'f', 0)
                        .arg(fps, 0, 'f', 1)
                        .arg(width)
                        .arg(height)
                        .arg(actual)
                        .arg(k_gige_link_warn_utilisation * max_fps_for_link(width, height, actual),
                             0, 'f', 1));
    }
}
#endif

// Writes the image-processing subset of CameraParameters (exposure, gain,
// gamma, black level, white balance, auto-exposure target, digital shift)
// onto the currently-open camera. Called once from open() and again from
// apply_live_params() whenever the UI edits one of these fields — this is
// the single place that subset of node-writes exists.
void VideoGrabber::apply_image_params() {
#if defined(MOSAIC_HAVE_CAMERAS)
    if (!d->camera.IsOpen()) return;

    auto& cam = d->camera.GetNodeMap();
    using Pylon::CEnumParameter;
    using Pylon::CFloatParameter;
    using Pylon::CIntegerParameter;
    const int idx = d->cameraIndex;
    auto try_set  = [&](const char* name, auto setter) {
        try {
            setter();
        } catch (const Pylon::GenericException& e) {
            log_warning(QString("[Camera %1] Skipping '%2': %3")
                             .arg(idx)
                             .arg(name)
                             .arg(QString::fromLocal8Bit(e.GetDescription())));
        }
    };

    // Auto-exposure limits — written BEFORE ExposureAuto so a "Once"
    // convergence happens inside them rather than outside and then being
    // clamped on the next apply.
    //
    // These two settings existed, were persisted, and were editable in the
    // camera card for months without ever being written to the camera. Nothing
    // bounded auto exposure, so the camera picked whatever the room's light
    // suggested — on this rig ~69 ms, which caps acquisition at 1e6/69200 =
    // 14.5 fps. That, not GigE bandwidth and not the pixel format, is why a rig
    // configured for 25 fps recorded at 14.45 for months. A sensor cannot
    // produce frames faster than it exposes them (see fps_readout.cpp), so an
    // unbounded exposure is an unbounded cap on frame rate.
    //
    // Written in whichever order keeps the pair valid at every step, never by
    // widening first. The two limits constrain each other inside the camera,
    // so raising `lower` past the *currently active* `upper` is rejected — but
    // the obvious workaround, widening `upper` to its maximum and then setting
    // both, leaves auto exposure fully unbounded if the final write fails, and
    // on a live re-apply mid-recording it opens a window in which Continuous
    // auto can pick an exposure far past the limit. So: if the new lower would
    // exceed the camera's current upper, raise upper first; otherwise lower
    // first. Each step leaves a valid, bounded pair.
    //
    // The pair is also forced consistent here — both spinboxes range 10..1e6
    // independently, so lower > upper is enterable — rather than letting the
    // camera reject one of them silently.
    try_set("AutoExposureTimeLimits", [&] {
        CFloatParameter lowerNode(cam, float_node_name(cam, "AutoExposureTimeLowerLimit",
                                                       "AutoExposureTimeAbsLowerLimit"));
        CFloatParameter upperNode(cam, float_node_name(cam, "AutoExposureTimeUpperLimit",
                                                       "AutoExposureTimeAbsUpperLimit"));
        const double upper =
            std::clamp(d->params.exposureAutoUpperUs, upperNode.GetMin(), upperNode.GetMax());
        const double lower = std::clamp(std::min(d->params.exposureAutoLowerUs, upper),
                                        lowerNode.GetMin(), lowerNode.GetMax());
        if (lower > upperNode.GetValue()) {
            upperNode.SetValue(upper);
            lowerNode.SetValue(lower);
        } else {
            lowerNode.SetValue(lower);
            upperNode.SetValue(upper);
        }
    });

    // Start auto exposure inside its limits. The limits only bound what auto
    // exposure *chooses*; they do not move an exposure already outside them.
    // And "Once" stops as soon as the image reaches its target brightness —
    // so with a bright scene it stopped immediately and left the exposure
    // where it was. Measured on room 11 (2026-10-02): limit 1000 µs, exposure
    // stayed at 19985 µs through two reopens (costing 49 fps instead of 51.6),
    // then dropped to 1015 µs on a third. Moving the current exposure into the
    // limits first makes the limit hold from the first open.
    if (d->params.exposureAuto != "Off") {
        try_set("ExposureIntoAutoLimits", [&] {
            CFloatParameter exposure(cam, float_node_name(cam, "ExposureTime", "ExposureTimeAbs"));
            const double upper  = d->params.exposureAutoUpperUs;
            const double lower  = std::min(d->params.exposureAutoLowerUs, upper);
            const double now    = exposure.GetValue();
            const double target = std::clamp(now, lower, upper);
            if (target != now) {
                // ExposureTime is writable only with auto exposure off; it is
                // set back to the configured mode just below.
                CEnumParameter(cam, "ExposureAuto").SetValue("Off");
                exposure.SetValue(std::clamp(target, exposure.GetMin(), exposure.GetMax()));
                log_info(QString("[Camera %1] Exposure %2 us was outside the auto limits "
                                 "(%3-%4 us) — starting auto exposure from %5 us.")
                             .arg(d->cameraIndex)
                             .arg(now, 0, 'f', 0)
                             .arg(lower, 0, 'f', 0)
                             .arg(upper, 0, 'f', 0)
                             .arg(target, 0, 'f', 0));
            }
        });
    }

    try_set("ExposureAuto", [&] {
        CEnumParameter(cam, "ExposureAuto").SetValue(d->params.exposureAuto.toStdString().c_str());
    });
    if (d->params.exposureAuto == "Off") {
        // SFNC 2.0: ExposureTime (float, µs)
        // SFNC 1.x: ExposureTimeAbs (float, µs) — same unit, different name
        try_set("ExposureTime", [&] {
            try {
                CFloatParameter(cam, "ExposureTime").SetValue(d->params.exposureTimeUs);
            } catch (const Pylon::GenericException&) {
                CFloatParameter(cam, "ExposureTimeAbs").SetValue(d->params.exposureTimeUs);
            }
        });
    }

    try_set("GainAuto", [&] {
        CEnumParameter(cam, "GainAuto").SetValue(d->params.gainAuto.toStdString().c_str());
    });
    if (d->params.gainAuto == "Off") {
        // SFNC 2.0: Gain (float, dB) — clamp to camera's valid range.
        // SFNC 1.x: GainRaw (integer, device units) — skip if Gain not present,
        //           since converting dB→raw requires knowing the camera's scale.
        try_set("Gain", [&] {
            auto p = CFloatParameter(cam, "Gain");
            p.SetValue(std::clamp(d->params.gainDb, p.GetMin(), p.GetMax()));
        });
    }

    try_set("Gamma", [&] { CFloatParameter(cam, "Gamma").SetValue(d->params.gamma); });

    // BlackLevel: the SFNC 2.0 float node isn't present on this camera
    // generation (confirmed against a real acA1920-25gc) — fall back to the
    // SFNC 1.x BlackLevelRaw integer node (device-specific range, typically
    // 0-63; UI range matches). Check node *existence* first (GetNode()
    // returns null) rather than distinguishing "node absent" from "value
    // out of range" by catching GenericException from SetValue() — the
    // float node may exist but reject an out-of-range value on some other
    // camera model, and that's a real error to surface via try_set's own
    // catch, not something that should silently fall through to a node
    // that doesn't exist either.
    try_set("BlackLevel", [&] {
        if (cam.GetNode("BlackLevel") != nullptr) {
            CFloatParameter(cam, "BlackLevel").SetValue(d->params.blackLevel);
        } else {
            auto p = CIntegerParameter(cam, "BlackLevelRaw");
            p.SetValue(round_clamp_to_int_range(d->params.blackLevel, p.GetMin(), p.GetMax()));
        }
    });

    try_set("BalanceWhiteAuto", [&] {
        CEnumParameter(cam, "BalanceWhiteAuto")
            .SetValue(d->params.balanceWhiteAuto.toStdString().c_str());
    });

    // Manual Red/Blue balance ratios — only meaningful once BalanceWhiteAuto
    // is "Off" (see settings.hpp doc comment on balanceRatioRed/Blue: "Once"
    // re-converges fresh on every camera open with no readback anywhere in
    // this codebase, so a fixed manual ratio is the only way to get a
    // reproducible color across sessions). Same SFNC-2.0-float-then-SFNC-1.x
    // BalanceRatioAbs-fallback pattern as BlackLevel/AutoTargetBrightness
    // above. Green is left untouched — stays at the camera's own fixed
    // reference value, matching standard Basler convention where only
    // Red/Blue are adjustable relative to Green.
    if (d->params.balanceWhiteAuto == "Off") {
        auto write_balance_ratio = [&](const char* channel, double value) {
            CEnumParameter(cam, "BalanceRatioSelector").SetValue(channel);
            if (cam.GetNode("BalanceRatio") != nullptr) {
                CFloatParameter(cam, "BalanceRatio").SetValue(value);
            } else {
                auto p = CIntegerParameter(cam, "BalanceRatioAbs");
                p.SetValue(round_clamp_to_int_range(value, p.GetMin(), p.GetMax()));
            }
        };
        try_set("BalanceRatio(Red)",
                [&] { write_balance_ratio("Red", d->params.balanceRatioRed); });
        try_set("BalanceRatio(Blue)",
                [&] { write_balance_ratio("Blue", d->params.balanceRatioBlue); });
    }

    // AutoTargetBrightness: the SFNC 2.0 float node (0.0-1.0) isn't present
    // on this camera generation — it exposes the same concept as
    // AutoTargetValue, an integer in device brightness units (confirmed
    // range ~50-205 on a real unit, but read live rather than hardcoded in
    // case it varies by model). Check node existence first, same rationale
    // as BlackLevel above.
    try_set("AutoTargetBrightness", [&] {
        if (cam.GetNode("AutoTargetBrightness") != nullptr) {
            CFloatParameter(cam, "AutoTargetBrightness").SetValue(d->params.autoTargetBrightness);
        } else {
            auto p = CIntegerParameter(cam, "AutoTargetValue");
            p.SetValue(map_normalized_to_int_range(d->params.autoTargetBrightness, p.GetMin(),
                                                   p.GetMax()));
        }
    });

    try_set("DigitalShift", [&] {
        auto p = CIntegerParameter(cam, "DigitalShift");
        p.SetValue(
            std::clamp(static_cast<int64_t>(d->params.digitalShift), p.GetMin(), p.GetMax()));
    });

    // Saturation, Contrast, Brightness, and TestPattern have no GenICam node
    // on this ace-classic camera generation at all (confirmed against a real
    // unit — this hardware has no on-camera ISP for those). They stay
    // UI-only; testPattern is intentionally UI-only regardless of hardware
    // generation — it only drives the stub/no-camera preview generator, per
    // CameraParameters' own doc comment.
#endif
}

// Re-reads the camera's real ResultingFrameRate (SFNC 2.0) / ResultingFrameRateAbs
// (SFNC 1.x) into d->resultingFps and re-emits the "requested fps not
// achievable" warning if it's now short of the configured rate. Called once
// from open(), and again from run_pylon_loop()'s live-apply branch and its
// own periodic self-refresh, right after apply_image_params() re-applies
// exposure/gain/etc: a parameter that affects the real achievable rate
// (most notably exposure time) can change via apply_live_params() without a
// full close+reopen, and leaving d->resultingFps stale from open() time
// would feed VideoManager::arm_and_fire_action_commands() a too-optimistic
// value — letting the shared Action Command trigger period run faster than
// this camera can now actually sustain, reproducing the packet-loss/
// dropped-frame failure mode this project already guards against for
// over-requested free-run fps, just self-inflicted by a stale live-apply
// instead.
//
// Gated on real elapsed acquisition time (is_achievable_fps_measurement_
// warmed_up()), not on the reading's own magnitude — a magnitude-based
// floor (rejecting anything "suspiciously low" relative to the camera's
// exposure ceiling) was tried first and reverted after real room-11 data
// (2026-08-05) contradicted its core assumption: comparing readings taken
// seconds apart, across all 6 cameras, over a real ~20s recording showed
// several genuinely stable, repeated, real readings sitting well below
// what that floor assumed was physically possible — auto-exposure
// converging near its own configured ceiling in a dim room can legitimately
// leave real achievable fps far lower than a naive "1/exposure, with some
// margin" estimate predicts, and no fixed margin generalizes across
// however bright or dim the room actually is. Waiting for real elapsed
// time instead, then trusting whatever the camera reports once warmed up
// — however low — needs no per-scene-lighting-tuned constant and can't
// mistake "real, if disappointing" for "premature".
void VideoGrabber::refresh_achievable_fps() {
#if defined(MOSAIC_HAVE_CAMERAS)
    if (!d->camera.IsOpen()) return;
    auto& cam            = d->camera.GetNodeMap();
    auto safe_f_fallback = [&](const char* primary, const char* fallback) -> double {
        try {
            return Pylon::CFloatParameter(cam, primary).GetValue();
        } catch (const Pylon::GenericException&) {
            try {
                return Pylon::CFloatParameter(cam, fallback).GetValue();
            } catch (...) {
                return -1.0;
            }
        }
    };
    const double rfps = safe_f_fallback("ResultingFrameRate", "ResultingFrameRateAbs");

    const double secondsSinceGrabbingStarted =
        d->grabbingStartedAt.has_value()
            ? std::chrono::duration<double>(SteadyClock::now() - *d->grabbingStartedAt).count()
            : -1.0;
    if (rfps <= 0.0 || !is_achievable_fps_measurement_warmed_up(secondsSinceGrabbingStarted)) {
        // Leaves d->resultingFps untouched (still -1 if never yet had a
        // trustworthy reading) rather than overwriting it with one taken
        // before real acquisition has had time to settle —
        // achievable_fps()'s callers already correctly fall back to
        // configured_fps() in that case.
        return;
    }
    d->resultingFps.store(rfps); // see achievable_fps()

    // What auto exposure and auto gain actually settled on. Logged because
    // they compensate for each other: cap the exposure and auto gain raises
    // the gain to reach the same target brightness, so the image looks
    // unchanged (only noisier) and an exposure limit appears to "do nothing".
    // Only when a value moves by more than 5%, so a settled camera logs once.
    {
        const double exposureUs = safe_f_fallback("ExposureTime", "ExposureTimeAbs");
        double gain             = safe_f_fallback("Gain", "GainAbs");
        bool gainIsRaw          = false;
        if (gain < 0.0) {
            try {
                gain = static_cast<double>(Pylon::CIntegerParameter(cam, "GainRaw").GetValue());
                gainIsRaw = true;
            } catch (...) {
                gain = -1.0;
            }
        }
        const auto moved = [](double was, double now) {
            return now >= 0.0 && (was < 0.0 || std::abs(now - was) > 0.05 * std::max(1.0, was));
        };
        if (moved(d->loggedExposureUs, exposureUs) || moved(d->loggedGain, gain)) {
            d->loggedExposureUs = exposureUs;
            d->loggedGain       = gain;
            log_info(QString("[Camera %1] Auto settled: exposure %2 us (limit %3 us), gain %4%5")
                         .arg(d->cameraIndex)
                         .arg(exposureUs, 0, 'f', 0)
                         .arg(d->params.exposureAutoUpperUs, 0, 'f', 0)
                         .arg(gain, 0, 'f', gainIsRaw ? 0 : 2)
                         .arg(gainIsRaw ? " (raw)" : " dB"));
        }
    }

    // Only announce a genuine change: this runs every ~2s per camera, and a
    // stable camera's reading jitters in the third decimal place, which would
    // otherwise repaint every card's readout twice a second for no reason.
    // Compared against the last *emitted* value rather than the last stored
    // one, so the displayed figure can never be more than one epsilon stale —
    // see Impl::lastEmittedFps.
    const double lastEmitted = d->lastEmittedFps.load();
    if (lastEmitted <= 0.0 || std::abs(rfps - lastEmitted) >= k_fps_change_epsilon) {
        d->lastEmittedFps.store(rfps);
        emit achievable_fps_changed(d->cameraIndex, rfps);
    }

    // Same mismatch check as open()'s own diagnostic block — surfaced again
    // here so a live parameter change (typically exposure time) that makes
    // the configured fps newly unachievable is flagged right away, not only
    // discovered indirectly via dropped frames during a later recording.
    // k_fps_shortfall_factor is shared with compute_fps_readout(), so this
    // warning and the camera card's own amber "can't sustain" readout can
    // never disagree about what counts as a shortfall.
    if (d->params.specifyFps && rfps > 0.0 && rfps < d->params.fps * k_fps_shortfall_factor) {
        log_warning(QString("[Camera %1] Requested %2 fps but the camera can only sustain "
                            "~%3 fps at the current exposure/ROI/bandwidth settings — "
                            "lower the exposure time or the configured frame rate to match.")
                        .arg(d->cameraIndex)
                        .arg(d->params.fps, 0, 'f', 1)
                        .arg(rfps, 0, 'f', 1));
    }
#endif
}

void VideoGrabber::apply_live_params() {
#if defined(MOSAIC_HAVE_CAMERAS)
    if (!d->deviceOpen.load()) return;
    // Do not touch d->camera's node map from this (caller's) thread — see
    // the liveApplyPending comment in Impl. The grab thread's own loop
    // (run_pylon_loop()) picks this up and calls apply_image_params()
    // itself, so all node-map access stays on one thread.
    d->liveApplyPending.store(true);
#endif
}

void VideoGrabber::request_calibration_frame(uint64_t token) {
    d->calibrationFrameToken.store(token);
    d->calibrationFramePending.store(true);
}

void VideoGrabber::close() {
    // Guard against the destructor calling close() a second time after
    // VideoManager::close() already called it explicitly.
    if (d->closeCalled.exchange(true)) return;

    log_info(QString("[Camera %1] close(): grabbing=%2 open=%3 attached=%4")
                 .arg(d->cameraIndex)
#if defined(MOSAIC_HAVE_CAMERAS)
                 .arg(d->camera.IsGrabbing())
                 .arg(d->camera.IsOpen())
                 .arg(d->camera.IsPylonDeviceAttached())
#else
                 .arg(false)
                 .arg(false)
                 .arg(false)
#endif
    );
#if defined(MOSAIC_HAVE_CAMERAS)
    release_device(false);

    // Balance the PylonInitialize() call from open().  When the last camera
    // calls this, the ref count hits 0 and Pylon fully resets its internal
    // transport-layer state, preventing stale device/socket state from
    // corrupting the next open session.
    if (d->pylonInitialized.exchange(false)) {
        Pylon::PylonTerminate();
        log_info(QString("[Camera %1] PylonTerminate done").arg(d->cameraIndex));
    }
#endif

    d->deviceOpen.store(false);
    d->deviceLost.store(false);
}

#if defined(MOSAIC_HAVE_CAMERAS)
// Stops, closes and detaches the Pylon device, leaving Pylon itself
// initialised. Shared by close() and reconnect_after_loss(), which must free a
// removed device before it can attach to the camera again. Each step is
// guarded, so this is safe on a device that is gone, half-open or never
// attached. `quiet` drops the per-step log lines, for the retry loop.
void VideoGrabber::release_device(bool quiet) {
    try {
        if (d->camera.IsGrabbing()) {
            d->camera.StopGrabbing();
            if (!quiet) log_info(QString("[Camera %1] StopGrabbing done").arg(d->cameraIndex));
        }
        if (d->camera.IsOpen()) {
            d->camera.Close();
            if (!quiet) log_info(QString("[Camera %1] Close done").arg(d->cameraIndex));
        }
        if (d->camera.IsPylonDeviceAttached()) {
            d->camera.DestroyDevice();
            if (!quiet) log_info(QString("[Camera %1] DestroyDevice done").arg(d->cameraIndex));
        }
    } catch (...) {
        if (!quiet) {
            log_error(
                QString("[Camera %1] exception while releasing the device").arg(d->cameraIndex));
        }
    }
}
#endif

// ── Grab control ───────────────────────────────────────────────────────────

void VideoGrabber::start_grabbing() {
    // A lost camera still starts: its thread begins by trying to reopen it
    // (see run_pylon_loop()), so one lost during a recording keeps being
    // retried in the preview that follows.
    if (!d->deviceOpen.load() && !d->deviceLost.load()) {
        return;
    }
    d->frameCounter.store(0);
    d->dropCounter.store(0);
    d->lastFrameElapsedNs.store(-1);
    d->actuallyGrabbing.store(false);
    d->grabbingStartedAt.reset();
    QThread::start();
}

void VideoGrabber::stop_grabbing() {
    if (!isRunning()) return;
    requestInterruption();
    // A reconnecting thread may be inside open(), which cannot be interrupted
    // and, on a link that drops again while it configures the camera, can
    // spend several GigE control timeouts there. Killing it mid-call would
    // leave the device in an unknown state for close(), so it gets longer.
    const int waitMs = d->deviceLost.load() ? 20000 : 5000;
    if (!wait(waitMs)) {
        log_warning(QString("[Camera %1] stop_grabbing: thread did not finish in %2 s — forcing "
                            "terminate")
                        .arg(d->cameraIndex)
                        .arg(waitMs / 1000));
        terminate();
        wait();
    }
}

// ── Run loop ───────────────────────────────────────────────────────────────

void VideoGrabber::run() {
#if defined(MOSAIC_HAVE_CAMERAS)
    run_pylon_loop();
#else
    run_stub_loop();
#endif
}

#if defined(MOSAIC_HAVE_CAMERAS)
void VideoGrabber::run_pylon_loop() {
    // A camera that drops out (a pulled cable, a power cut) is reopened by
    // this thread, in place, rather than left for dead until the operator
    // reopens every camera. In place matters during a recording: the same
    // ring buffer, encoder and video file carry on, frame_id keeps counting,
    // and the gap shows up as a gap in the timestamps (and as MISSING in Frame
    // Sync Repair's synced/ videos). The Action Command ticker keeps
    // broadcasting to the camera's subnet throughout, so it is triggered
    // again the moment it is armed. All Pylon access stays on this thread.
    //
    // A camera that fails again soon after coming back (a loose connector,
    // a fault that a reopen does not fix) is retried less and less often, up
    // to k_max_retry_s, so it cannot flood the log.
    constexpr double k_min_retry_s = 2.0;
    constexpr double k_max_retry_s = 30.0;
    constexpr double k_stable_s    = 10.0;
    double retryS                  = k_min_retry_s;

    // Lost before this thread started (during the recording that just ended).
    if (d->deviceLost.load() && !reconnect_after_loss(retryS)) {
        return;
    }
    auto runningSince = SteadyClock::now();
    while (grab_until_stopped()) {
        const double ranS =
            std::chrono::duration<double>(SteadyClock::now() - runningSince).count();
        retryS = ranS < k_stable_s ? std::min(retryS * 2.0, k_max_retry_s) : k_min_retry_s;
        if (!reconnect_after_loss(retryS)) {
            return; // stopped while the camera was away
        }
        runningSince = SteadyClock::now();
    }
}

bool VideoGrabber::reconnect_after_loss(double retryS) {
    d->deviceLost.store(true);
    d->deviceOpen.store(false);
    d->actuallyGrabbing.store(false);
    d->currentFps.store(0.0);
    // The reopened camera's rate must pass the warm-up gate again, not inherit
    // the previous stretch's start time (see refresh_achievable_fps()).
    d->grabbingStartedAt.reset();
    release_device(true);

    log_warning(QString("[Camera %1] Lost; trying to reopen it every %2 s. Once it is back its "
                        "frames continue in the same video, and the gap is marked MISSING by "
                        "Frame Sync Repair.")
                    .arg(d->cameraIndex)
                    .arg(retryS, 0, 'f', 0));
    const auto lostAt  = SteadyClock::now();
    auto lastReport    = lostAt;
    const auto retryMs = static_cast<int>(retryS * 1000.0);
    int attempts       = 0;
    while (!isInterruptionRequested()) {
        // Interruptible wait, so stop_grabbing() is never held up by it.
        for (int waited = 0; waited < retryMs && !isInterruptionRequested(); waited += 100) {
            QThread::msleep(100);
        }
        if (isInterruptionRequested()) {
            break;
        }
        ++attempts;
        d->quietOpenFailure = true;
        const bool ok       = open();
        d->quietOpenFailure = false;
        const auto now      = SteadyClock::now();
        const double awayS  = std::chrono::duration<double>(now - lostAt).count();
        if (ok) {
            d->reconnectCount.fetch_add(1);
            d->deviceLost.store(false);
            log_info(QString("[Camera %1] Reconnected after %2 s (%3 attempt(s)).")
                         .arg(d->cameraIndex)
                         .arg(awayS, 0, 'f', 1)
                         .arg(attempts));
            return true;
        }
        release_device(true); // open() can fail after attaching
        if (std::chrono::duration<double>(now - lastReport).count() >= 30.0) {
            lastReport = now;
            log_warning(QString("[Camera %1] Still not reachable after %2 s; still trying.")
                            .arg(d->cameraIndex)
                            .arg(awayS, 0, 'f', 0));
        }
    }
    log_info(QString("[Camera %1] Stopped while lost; it will be retried when grabbing starts "
                     "again.")
                 .arg(d->cameraIndex));
    return false;
}

bool VideoGrabber::grab_until_stopped() {
    bool grabFailed = false;
    try {
        // Always output BGR8packed so Qt can display any camera pixel format.
        d->converter.OutputPixelFormat  = Pylon::PixelType_BGR8packed;
        d->converter.OutputBitAlignment = Pylon::OutputBitAlignment_MsbAligned;

        // More grab buffers reduce incomplete-frame errors when the processing
        // thread falls behind the camera (e.g. during Bayer demosaicing or
        // when multiple cameras share a GigE switch).
        d->camera.MaxNumBuffer = 30;

        // Tune the stream grabber for GigE reliability: allow packet resend
        // and a larger receive window so the driver can reorder late packets.
        try {
            auto& sgn = d->camera.GetStreamGrabberNodeMap();
            Pylon::CIntegerParameter(sgn, "MaximumNumberResendRequests").SetValue(100);
            Pylon::CIntegerParameter(sgn, "ReceiveWindowSize").SetValue(64);
        } catch (...) {
        }

        d->camera.StartGrabbing(Pylon::GrabStrategy_LatestImageOnly,
                                Pylon::GrabLoop_ProvidedByUser);
        d->grabbingStartedAt = SteadyClock::now();
        d->actuallyGrabbing.store(true);

        Pylon::CGrabResultPtr result;
        Pylon::CPylonImage convertedImage;
        auto lastFpsTime    = SteadyClock::now();
        auto lastWarnTime   = SteadyClock::now();
        auto lastFpsRefresh = SteadyClock::now();
        int64_t fpsCount    = 0;
        int64_t warnCount   = 0;
        int previewSkip     = 0;

        while (!isInterruptionRequested()) {
            if (d->liveApplyPending.exchange(false)) {
                apply_image_params();
                refresh_achievable_fps();
            }
            // Periodic self-refresh, independent of any UI edit: a reading
            // taken right at open()/StartGrabbing() is always rejected by
            // refresh_achievable_fps()'s warm-up gate as premature — real
            // acquisition needs to actually run for a while first. Without
            // this, a camera would never get a second chance to report a
            // trustworthy reading until the user happened to touch a
            // live-apply-triggering UI control — leaving VideoManager::
            // arm_and_fire_action_commands() (and its own periodic
            // self-correction, see ActionCommandTicker::run()) permanently
            // stuck trusting configured_fps() instead of ever discovering
            // this camera's real ceiling.
            {
                const auto nowForFpsRefresh = SteadyClock::now();
                if (std::chrono::duration<double>(nowForFpsRefresh - lastFpsRefresh).count() >=
                    2.0) {
                    lastFpsRefresh = nowForFpsRefresh;
                    refresh_achievable_fps();
                }
            }
            if (!d->camera.RetrieveResult(100, result, Pylon::TimeoutHandling_Return)) {
                // Whether a removed GigE device makes RetrieveResult() throw
                // or just time out depends on the Pylon version; ask, so a
                // lost camera is always handed to reconnect_after_loss().
                if (d->camera.IsCameraDeviceRemoved()) {
                    throw RUNTIME_EXCEPTION("The camera device has been physically removed.");
                }
                continue;
            }
            if (!result->GrabSucceeded()) {
                ++warnCount;
                d->incompleteFrameTotal.fetch_add(1, std::memory_order_relaxed);
                const auto now = SteadyClock::now();
                if (std::chrono::duration<double>(now - lastWarnTime).count() >= 5.0) {
                    log_warning(
                        QString("[Camera %1] %2 incomplete frame(s) in last 5 s "
                                "(GigE packet loss — check NIC jumbo frames and switch bandwidth)")
                            .arg(d->cameraIndex)
                            .arg(warnCount));
                    warnCount    = 0;
                    lastWarnTime = now;
                }
                continue;
            }

            const int64_t frameId = d->frameCounter.fetch_add(1) + 1;
            const int64_t tsNs    = elapsed_ns();
            const int64_t wallNs  = wall_clock_ns();
            d->lastFrameElapsedNs.store(tsNs);

            // Camera-hardware timestamp (GevTimestamp chunk), converted from
            // device ticks to nanoseconds. Read via the grab result's chunk
            // data node map — the generic (non-camera-model-typed) accessor,
            // valid as long as ChunkModeActive was successfully enabled in
            // open(). Left at 0 if chunk data isn't present on this frame.
            int64_t hwTsNs = 0;
            if (d->tickFreqHz > 0) {
                try {
                    auto& chunkNodeMap = result->GetChunkDataNodeMap();
                    const int64_t ticks =
                        Pylon::CIntegerParameter(chunkNodeMap, "ChunkTimestamp").GetValue();
                    hwTsNs = static_cast<int64_t>(static_cast<double>(ticks) *
                                                  (1e9 / static_cast<double>(d->tickFreqHz)));
                } catch (const Pylon::GenericException&) {
                    // Chunk data unavailable for this frame — leave hwTsNs at 0.
                }
            }
            // The frame's exposure chunk, µs. Checked for rather than read
            // and caught: on a firmware without it this runs for every frame.
            double exposureUs = -1.0;
            try {
                auto& chunkNodeMap = result->GetChunkDataNodeMap();
                auto* node         = chunkNodeMap.GetNode("ChunkExposureTime");
                if (node != nullptr && GenApi::IsReadable(node)) {
                    exposureUs = Pylon::CFloatParameter(node).GetValue();
                }
            } catch (const Pylon::GenericException&) {
                // Unavailable for this frame: left unknown.
            }

            // Convert from the camera's native pixel format (BayerRG, YUV, Mono, …)
            // to BGR8packed so the QImage and VideoFrame always carry BGR data.
            d->converter.Convert(convertedImage, result);

            const int w = static_cast<int>(convertedImage.GetWidth());
            const int h = static_cast<int>(convertedImage.GetHeight());
            // Query the converter's actual row stride rather than assuming a
            // tightly-packed w*3 buffer — if the output ever has row padding
            // (alignment padding is a real possibility for some pixel/output
            // format combinations), copying w*3 bytes/row here would read
            // each row from the wrong offset, producing a diagonally-sheared,
            // rainbow-striped image (a classic symptom of a stride mismatch).
            size_t stride = 0;
            if (!convertedImage.GetStride(stride) || stride == 0) {
                stride = static_cast<size_t>(w) * 3;
            }
            const size_t sz = convertedImage.GetImageSize();

            auto frame           = std::make_shared<VideoFrame>();
            frame->cameraIndex   = d->cameraIndex;
            frame->frameId       = frameId;
            frame->elapsedNs     = tsNs;
            frame->wallClockNs   = wallNs;
            frame->hwTimestampNs = hwTsNs;
            frame->exposureUs    = exposureUs;
            frame->width         = w;
            frame->height        = h;
            frame->stride        = static_cast<int>(stride);
            frame->data.resize(sz);
            std::memcpy(frame->data.data(), convertedImage.GetBuffer(), sz);

            // One-shot full-resolution capture for room calibration — see
            // request_calibration_frame(). Checked before the throttled/
            // downscaled preview below so a pending request is served on the
            // very next frame regardless of the preview's own skip counter.
            if (d->calibrationFramePending.exchange(false)) {
                const QImage full(frame->data.data(), w, h, static_cast<int>(stride),
                                  QImage::Format_BGR888);
                emit calibration_frame_ready(d->cameraIndex, full.copy(),
                                             d->calibrationFrameToken.load());
            }

            // Throttled preview at ~half the grab rate for live QML display.
            // Scale down to 640×360 max — the UI panels are small and a 6 MB
            // texture upload per camera per frame makes Qt's render thread the
            // bottleneck.  FastTransformation is ~6× faster than SmoothTransformation
            // and imperceptible at preview size.
            if (++previewSkip >= 2) {
                previewSkip = 0;
                QImage full(frame->data.data(), w, h, static_cast<int>(stride),
                            QImage::Format_BGR888);
                const QImage scaled =
                    (w > 640 || h > 360)
                        ? full.scaled(640, 360, Qt::KeepAspectRatio, Qt::FastTransformation)
                        : full.copy();
                emit preview_frame(d->cameraIndex, scaled);
            }

            if (!d->frameBuffer.push(std::move(frame))) {
                d->dropCounter.fetch_add(1);
                emit frame_dropped(d->cameraIndex, frameId);
            }

            // Rolling FPS estimate over 1-second windows.
            ++fpsCount;
            const auto now = SteadyClock::now();
            const auto dt  = std::chrono::duration<double>(now - lastFpsTime).count();
            if (dt >= 1.0) {
                d->currentFps.store(static_cast<double>(fpsCount) / dt);
                fpsCount    = 0;
                lastFpsTime = now;
            }
        }

        d->camera.StopGrabbing();

    } catch (const Pylon::GenericException& e) {
        emit grab_error(d->cameraIndex, QString::fromLocal8Bit(e.GetDescription()));
        grabFailed = true;
    }
    // Broadened beyond Pylon::GenericException: an uncaught exception of any
    // other type escaping this function escapes QThread::run() itself, which
    // is fatal to the whole process (std::terminate()) — report and let the
    // thread exit cleanly instead, same rationale as try_set()'s own
    // broadened catch above.
    catch (const std::exception& e) {
        emit grab_error(d->cameraIndex,
                        QString("Unexpected exception: %1").arg(QString::fromUtf8(e.what())));
        grabFailed = true;
    } catch (...) {
        emit grab_error(d->cameraIndex, "Unknown non-standard exception in grab loop.");
        grabFailed = true;
    }

    if (grabFailed) {
        // This camera has stopped acquiring mid-session (a pulled cable is the
        // realistic case), so nothing refreshes its rate until it is back.
        // VideoManager::close()'s retraction only covers an orderly shutdown,
        // which this isn't — without this the card would go on presenting the
        // last good reading as a live figure indefinitely.
        //
        // Deliberately retracts only what is *displayed*: resultingFps itself
        // is left alone, because ActionCommandTicker re-reads achievable_fps()
        // to pick the group's shared firing period and falling back to
        // configured_fps() there caused >80% trigger loss across the whole
        // group in real room-11 testing. A dead camera's last known ceiling is
        // still a better input to that calculation than an optimistic
        // configured value.
        d->lastEmittedFps.store(-1.0);
        emit achievable_fps_changed(d->cameraIndex, -1.0);
    }
    // Stopped on request is not a failure, even if the request landed just as
    // the camera went away: the caller is shutting down.
    return grabFailed && !isInterruptionRequested();
}
#else
void VideoGrabber::run_pylon_loop() { run_stub_loop(); }
#endif

void VideoGrabber::run_stub_loop() {
    d->actuallyGrabbing.store(true);
    const double fps = d->params.fps > 0.0 ? d->params.fps : 30.0;
    const int width  = d->params.width > 0 ? d->params.width : 1920;
    const int height = d->params.height > 0 ? d->params.height : 1080;
    // Convert floating-point period to the steady_clock's native duration type.
    const SteadyClock::duration period =
        std::chrono::duration_cast<SteadyClock::duration>(std::chrono::duration<double>(1.0 / fps));

    // Pre-allocate a reusable test-pattern buffer (BGR colour bars).
    std::vector<uint8_t> pattern(static_cast<size_t>(width * height * 3));
    {
        // Seven ITU colour bars: white, yellow, cyan, green, magenta, red, blue.
        // Stored as {B, G, R} to match BGR8 layout.
        const std::array<std::array<uint8_t, 3>, 7> bars = {{{255, 255, 255},
                                                             {0, 255, 255},
                                                             {255, 255, 0},
                                                             {0, 255, 0},
                                                             {255, 0, 255},
                                                             {0, 0, 255},
                                                             {0, 0, 255}}};
        const int barW                                   = width / 7;
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const int bar            = std::min(col / barW, 6);
                const std::size_t offset = static_cast<std::size_t>((row * width + col) * 3);
                pattern[offset + 0]      = bars[static_cast<std::size_t>(bar)][0];
                pattern[offset + 1]      = bars[static_cast<std::size_t>(bar)][1];
                pattern[offset + 2]      = bars[static_cast<std::size_t>(bar)][2];
            }
        }
    }

    auto nextFrame   = SteadyClock::now();
    auto lastFpsTime = nextFrame;
    int64_t fpsCount = 0;

    while (!isInterruptionRequested()) {
        const auto now = SteadyClock::now();
        if (now < nextFrame) {
            QThread::msleep(1);
            continue;
        }
        nextFrame += period;

        const int64_t frameId = d->frameCounter.fetch_add(1) + 1;

        // Embed frame counter as a white rectangle in top-left so we can
        // verify frame ordering during testing.
        auto frame         = std::make_shared<VideoFrame>();
        frame->cameraIndex = d->cameraIndex;
        frame->frameId     = frameId;
        frame->elapsedNs   = elapsed_ns();
        frame->wallClockNs = wall_clock_ns();
        frame->width       = width;
        frame->height      = height;
        frame->stride      = width * 3;
        frame->data        = pattern; // copy of pre-built pattern

        d->lastFrameElapsedNs.store(frame->elapsedNs);

        // Stamp a small white rectangle top-left so frame ordering is visible.
        const int stampH = std::min(20, height);
        const int stampW = std::min(80, width);
        for (int row = 0; row < stampH; ++row) {
            for (int col = 0; col < stampW; ++col) {
                const std::size_t off = static_cast<std::size_t>((row * width + col) * 3);
                frame->data[off] = frame->data[off + 1] = frame->data[off + 2] = 255;
            }
        }

        // One-shot full-resolution capture for room calibration (stub
        // build — see request_calibration_frame() and its Pylon-loop
        // counterpart above).
        if (d->calibrationFramePending.exchange(false)) {
            const QImage full(frame->data.data(), width, height, width * 3, QImage::Format_BGR888);
            emit calibration_frame_ready(d->cameraIndex, full.copy(),
                                         d->calibrationFrameToken.load());
        }

        // Emit a throttled preview (~15 fps) for live QML display.
        if (frameId % 2 == 0) {
            QImage preview(frame->data.data(), width, height, width * 3, QImage::Format_BGR888);
            emit preview_frame(d->cameraIndex, preview.copy());
        }

        if (!d->frameBuffer.push(std::move(frame))) {
            d->dropCounter.fetch_add(1);
            emit frame_dropped(d->cameraIndex, frameId);
        }

        ++fpsCount;
        const double elapsed = std::chrono::duration<double>(now - lastFpsTime).count();
        if (elapsed >= 1.0) {
            d->currentFps.store(static_cast<double>(fpsCount) / elapsed);
            fpsCount    = 0;
            lastFpsTime = now;
        }
    }
}

// ── Accessors ──────────────────────────────────────────────────────────────

bool VideoGrabber::is_actually_grabbing() const { return d->actuallyGrabbing.load(); }

bool VideoGrabber::is_open() const { return d->deviceOpen.load(); }
bool VideoGrabber::is_reconnecting() const { return d->deviceLost.load(); }
int64_t VideoGrabber::reconnects() const { return d->reconnectCount.load(); }
int64_t VideoGrabber::frames_grabbed() const { return d->frameCounter.load(); }
int64_t VideoGrabber::frames_dropped() const { return d->dropCounter.load(); }
int64_t VideoGrabber::incomplete_frames_total() const { return d->incompleteFrameTotal.load(); }
double VideoGrabber::current_fps() const { return d->currentFps.load(); }
int64_t VideoGrabber::last_frame_elapsed_ns() const { return d->lastFrameElapsedNs.load(); }

bool VideoGrabber::action_command_ready() const { return d->actionCommandReady; }
uint32_t VideoGrabber::action_device_key() const { return d->actionDeviceKey; }
QString VideoGrabber::action_broadcast_address() const { return d->actionBroadcastAddress; }

double VideoGrabber::configured_fps() const { return d->params.fps; }
int VideoGrabber::frame_width() const { return d->frameWidth; }
int VideoGrabber::frame_height() const { return d->frameHeight; }
double VideoGrabber::camera_max_fps() const { return d->openMaxFps; }
int VideoGrabber::frame_offset_x() const { return d->frameOffsetX; }
QString VideoGrabber::pixel_format() const {
    QMutexLocker lock(&d->pixelFormatMutex);
    return d->pixelFormat;
}
int VideoGrabber::frame_offset_y() const { return d->frameOffsetY; }
double VideoGrabber::achievable_fps() const { return d->resultingFps; }

} // namespace mosaic
