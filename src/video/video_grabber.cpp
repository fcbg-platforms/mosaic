#include "video/video_grabber.hpp"
#include "trigger/trigger_manager.hpp"
#include "video/gige_action_command.hpp"
#include "video/param_mapping.hpp"
#include "utils/logger.hpp"
#include "utils/timestamp.hpp"
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

#if defined(MOSAIC_HAVE_CAMERAS)
#  define NOMINMAX          // prevent Windows.h min/max macros breaking std::min
#  include <pylon/PylonIncludes.h>
#endif

namespace mosaic {

struct VideoGrabber::Impl {
    int                                      cameraIndex;
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
    const CameraParameters&                  params;
    RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer;
    TriggerManager*                          triggerMgr;  // not owned, may be null

    std::atomic<bool>    deviceOpen        {false};
    std::atomic<bool>    closeCalled       {false};
    std::atomic<bool>    pylonInitialized  {false};
    std::atomic<int64_t> frameCounter {0};
    std::atomic<int64_t> dropCounter  {0};
    std::atomic<double>  currentFps   {0.0};
    std::atomic<int64_t> lastFrameElapsedNs {-1};

    // Set true only once run_pylon_loop() has actually reached Pylon's
    // StartGrabbing() call (or immediately in the stub loop, which has no
    // equivalent setup step) — NOT merely once start_grabbing() has
    // returned, which only confirms QThread::start() scheduled the thread,
    // not that Pylon is actually listening for triggers yet. Reset false at
    // the start of every start_grabbing() call. See is_actually_grabbing().
    std::atomic<bool> actuallyGrabbing {false};

    // GigE Vision Action Command trigger state — see
    // action_command_ready()/action_device_key()/action_broadcast_address().
    // Written only in open() (main thread) before the grab thread starts, so
    // no synchronization is needed for reads afterward (plain bool/uint32_t/
    // QString, not atomics — same happens-before argument as tickFreqHz
    // below).
    bool     actionCommandReady{false};
    uint32_t actionDeviceKey{0};
    QString  actionBroadcastAddress;

    // Set by apply_live_params() (typically called from the GUI thread) and
    // consumed by the grab thread's own loop. Pylon's CInstantCamera/node
    // map is not documented as safe for concurrent access from a second
    // thread while RetrieveResult() is in-flight, so parameter writes are
    // deferred to — and only ever performed by — the grab thread itself.
    std::atomic<bool> liveApplyPending {false};

    // Same deferred-to-grab-thread pattern as liveApplyPending, but for a
    // one-shot full-resolution frame request (see
    // VideoGrabber::request_calibration_frame()).
    std::atomic<bool>     calibrationFramePending {false};
    // Echoed back verbatim on calibration_frame_ready() — see that signal's
    // doc comment. Written before calibrationFramePending is set, read after
    // calibrationFramePending is claimed via exchange(), so the token always
    // corresponds to the request that set the pending flag.
    std::atomic<uint64_t> calibrationFrameToken {0};

    // GevTimestampTickFrequency (Hz), read once in open() and reused by the
    // grab thread to convert each frame's chunk timestamp from device ticks
    // to nanoseconds. Written only in open() (main thread) before the grab
    // thread is started, so no synchronization is needed for the read in
    // run_pylon_loop() — QThread::start() establishes the happens-before edge.
    int64_t tickFreqHz = 0;

    // The camera's real ResultingFrameRate as measured at open() time — same
    // happens-before argument as tickFreqHz above (written once, main
    // thread, before the grab thread starts). -1.0 if it couldn't be read
    // (stub builds, or the node genuinely unavailable). See achievable_fps().
    double resultingFps = -1.0;

#if defined(MOSAIC_HAVE_CAMERAS)
    Pylon::CInstantCamera        camera;
    Pylon::CImageFormatConverter converter;  // converts any pixel format → BGR8
#endif

    explicit Impl(int idx,
                  const CameraParameters& p,
                  RingBuffer<std::shared_ptr<VideoFrame>>& buf,
                  TriggerManager* tm)
        : cameraIndex(idx), params(p), frameBuffer(buf), triggerMgr(tm) {}
};

VideoGrabber::VideoGrabber(int                                      cameraIndex,
                            const CameraParameters&                  params,
                            RingBuffer<std::shared_ptr<VideoFrame>>& frameBuffer,
                            TriggerManager*                          triggerMgr,
                            QObject*                                 parent)
    : QThread(parent)
    , d(std::make_unique<Impl>(cameraIndex, params, frameBuffer, triggerMgr))
{}

VideoGrabber::~VideoGrabber() { stop_grabbing(); close(); }

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
        Pylon::PylonInitialize();
        d->pylonInitialized.store(true);  // must call PylonTerminate() in close()

        if (d->params.serialNumber.isEmpty()) {
            d->camera.Attach(Pylon::CTlFactory::GetInstance().CreateFirstDevice());
        } else {
            Pylon::CDeviceInfo info;
            info.SetSerialNumber(d->params.serialNumber.toStdString().c_str());
            d->camera.Attach(Pylon::CTlFactory::GetInstance().CreateDevice(info));
        }
        d->camera.Open();

    } catch (const Pylon::GenericException& e) {
        log_error(QString("[Camera %1] Cannot open device (serial %2): %3")
                      .arg(d->cameraIndex)
                      .arg(d->params.serialNumber)
                      .arg(QString::fromLocal8Bit(e.GetDescription())));
        return false;
    }

    // Phase 2: configure camera parameters.  Each param is tried independently
    // so a bad setting (e.g. gain out of range) logs a warning instead of
    // killing the entire camera.
    {
        auto& cam = d->camera.GetNodeMap();
        using Pylon::CIntegerParameter;
        using Pylon::CFloatParameter;
        using Pylon::CEnumParameter;
        using Pylon::CBooleanParameter;

        const int idx = d->cameraIndex;
        auto try_set = [&](const char* name, auto setter) {
            try { setter(); }
            catch (const Pylon::GenericException& e) {
                log_warning(QString("[Camera %1] Skipping '%2': %3")
                                .arg(idx).arg(name)
                                .arg(QString::fromLocal8Bit(e.GetDescription())));
            }
            // Broadened beyond Pylon::GenericException so a node write that
            // throws something else (a different exception type, std::bad_alloc,
            // ...) degrades to a skipped parameter — same as any other bad
            // setting — instead of escaping uncaught up through run_pylon_loop()
            // and QThread::run(), which is fatal (std::terminate()).
            catch (const std::exception& e) {
                log_warning(QString("[Camera %1] Skipping '%2': unexpected exception: %3")
                                .arg(idx).arg(name).arg(QString::fromUtf8(e.what())));
            }
            catch (...) {
                log_warning(QString("[Camera %1] Skipping '%2': unknown non-standard exception")
                                .arg(idx).arg(name));
            }
        };

        try_set("Width",   [&]{ CIntegerParameter(cam,"Width").SetValue(d->params.width); });
        try_set("Height",  [&]{ CIntegerParameter(cam,"Height").SetValue(d->params.height); });
        try_set("OffsetX", [&]{ CIntegerParameter(cam,"OffsetX").SetValue(d->params.offsetX); });
        try_set("OffsetY", [&]{ CIntegerParameter(cam,"OffsetY").SetValue(d->params.offsetY); });
        try_set("ReverseX",[&]{ CBooleanParameter(cam,"ReverseX").SetValue(d->params.reverseX); });
        try_set("ReverseY",[&]{ CBooleanParameter(cam,"ReverseY").SetValue(d->params.reverseY); });
        // PixelFormat: GigE cameras output raw sensor formats (BayerRG8, YUV…),
        // not BGR8.  Try the configured value; if it fails try the SFNC 1.x
        // packed equivalent.  CImageFormatConverter converts whatever the camera
        // delivers to BGR8packed, so any accepted format is fine.
        try_set("PixelFormat", [&]{
            auto pfp = CEnumParameter(cam, "PixelFormat");
            try {
                pfp.SetValue(d->params.pixelFormat.toStdString().c_str());
            } catch (const Pylon::GenericException&) {
                // SFNC 1.x packed variants
                static const std::vector<std::string> fallbacks = {
                    "BGR8Packed", "BayerRG8", "YCbCr422_8"
                };
                bool set = false;
                for (const auto& fb : fallbacks) {
                    try { pfp.SetValue(fb.c_str()); set = true; break; }
                    catch (...) {}
                }
                if (set) {
                    log_info(QString("[Camera %1] PixelFormat '%2' not supported; accepted alternative")
                                 .arg(d->cameraIndex).arg(d->params.pixelFormat));
                }
            }
        });

        if (d->params.specifyFps) {
            // AcquisitionFrameRateEnable is a *boolean* parameter (SFNC 2.0).
            // Some firmware versions don't have it — the rate is always settable.
            try_set("AcquisitionFrameRateEnable", [&]{
                CBooleanParameter(cam, "AcquisitionFrameRateEnable").SetValue(true);
            });
            // SFNC 2.0: AcquisitionFrameRate (float)
            // SFNC 1.x: AcquisitionFrameRateAbs (float) — same unit, different name
            try_set("AcquisitionFrameRate", [&]{
                try {
                    CFloatParameter(cam, "AcquisitionFrameRate").SetValue(d->params.fps);
                } catch (const Pylon::GenericException&) {
                    CFloatParameter(cam, "AcquisitionFrameRateAbs").SetValue(d->params.fps);
                }
            });
        }

        // Hardware trigger input (external TTL pulse on a GPIO line, or a
        // GigE Vision Action Command broadcast, drives frame acquisition
        // instead of the camera free-running at AcquisitionFrameRate). Off
        // by default — CameraParameters::hwTriggerEnabled starts false, so
        // this only takes effect if a user explicitly opts in via the HW
        // Trigger tab. A camera left triggered with no signal/command ever
        // arriving simply produces zero frames, so this must stay opt-in
        // rather than auto-detected.
        const bool wantsActionCommand = d->params.hwTriggerEnabled
                                      && d->params.hwTriggerSource == "Action1";
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
                CIntegerParameter(cam, "ActionSelector").SetToMinimum(); // selects "Action1" (index 0)
                CIntegerParameter(cam, "ActionDeviceKey")
                    .SetValue(static_cast<int64_t>(d->cameraIndex) + 1);
                CIntegerParameter(cam, "ActionGroupKey").SetValue(static_cast<int64_t>(k_action_group_key));
                CIntegerParameter(cam, "ActionGroupMask").SetValue(static_cast<int64_t>(k_action_group_mask));

                // Resolve this camera's own broadcast address at runtime from
                // what it reports (GevCurrentIPAddress/GevCurrentSubnetMask),
                // rather than hardcoding the ops setup script's static
                // per-NIC subnet list — each camera lives on its own
                // isolated /24.
                const auto ip   = static_cast<uint32_t>(CIntegerParameter(cam, "GevCurrentIPAddress").GetValue());
                const auto mask = static_cast<uint32_t>(CIntegerParameter(cam, "GevCurrentSubnetMask").GetValue());
                d->actionDeviceKey        = static_cast<uint32_t>(d->cameraIndex) + 1;
                d->actionBroadcastAddress = ipv4_to_dotted(ipv4_broadcast_address(ip, mask));
                d->actionCommandReady     = true;
                log_info(QString("[Camera %1] Action-command trigger ready — deviceKey=%2 broadcast=%3")
                             .arg(d->cameraIndex).arg(d->actionDeviceKey).arg(d->actionBroadcastAddress));
            } catch (const Pylon::GenericException& e) {
                log_warning(QString("[Camera %1] This firmware does not support GigE Vision Action "
                                    "Commands (%2) — falling back to free-run for this session. "
                                    "Switch 'Trigger source' to Line1 or Software instead.")
                                .arg(d->cameraIndex).arg(QString::fromLocal8Bit(e.GetDescription())));
            }
            emit action_command_capability(d->cameraIndex, d->actionCommandReady);
        }

        // Falls back to free-run if Action1 was requested but unsupported
        // on this firmware — never leave TriggerMode=On with nothing that
        // will ever trigger it. Deliberately does not mutate
        // d->params.hwTriggerEnabled/hwTriggerSource (the user's saved
        // settings) — a firmware/config change is re-probed fresh on the
        // next open(), not silently downgraded and stuck.
        const bool effectiveHwTrigger = d->params.hwTriggerEnabled
                                      && (!wantsActionCommand || d->actionCommandReady);

        // Every trigger source (Line1/Software/Action1) uses the same
        // per-frame "FrameStart" selector — see the note in the Action1
        // probe above for why Action1 no longer gets a special
        // "AcquisitionStart" case.
        try_set("TriggerSelector", [&]{
            CEnumParameter(cam, "TriggerSelector").SetValue("FrameStart");
        });
        try_set("TriggerMode", [&]{
            CEnumParameter(cam, "TriggerMode")
                .SetValue(effectiveHwTrigger ? "On" : "Off");
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
            try_set("AcquisitionMode", [&]{
                CEnumParameter(cam, "AcquisitionMode").SetValue("Continuous");
            });

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
                                .arg(idx).arg(d->params.hwTriggerSource)
                                .arg(QString::fromLocal8Bit(e.GetDescription())));
                try_set("TriggerMode", [&]{
                    CEnumParameter(cam, "TriggerMode").SetValue("Off");
                });
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
            try_set("TriggerDelay", [&]{
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
        try_set("ChunkModeActive", [&]{
            CBooleanParameter(cam, "ChunkModeActive").SetValue(true);
            CEnumParameter(cam, "ChunkSelector").SetValue("Timestamp");
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
        try_set("GevSCPSPacketSize", [&]{
            auto p = CIntegerParameter(cam, "GevSCPSPacketSize");
            p.SetValue(std::clamp(static_cast<int64_t>(1500), p.GetMin(), p.GetMax()));
        });
        // Keep zero inter-packet delay (burst mode). Experiments showed that
        // any positive SCPD value interacts poorly with the I350 interrupt
        // coalescing on this machine and worsened loss on some cameras.
        // Burst (SCPD=0) leaves the full 23ms gap between frames for the NIC
        // to finish processing, which consistently performs better here.
        try_set("GevSCPD", [&]{
            auto p = CIntegerParameter(cam, "GevSCPD");
            p.SetValue(std::clamp(static_cast<int64_t>(0), p.GetMin(), p.GetMax()));
        });
        // Stagger frame transmission across cameras so they don't all burst
        // onto the same I350-T4 card simultaneously.  Each camera waits
        // (index × 5 ms) after frame readout before transmitting.
        // At 125 MHz tick frequency: 5 ms = 625 000 ticks.
        try_set("GevSCFTD", [&]{
            auto p = CIntegerParameter(cam, "GevSCFTD");
            const int64_t delayTicks = static_cast<int64_t>(d->cameraIndex) * 625000LL;
            p.SetValue(std::clamp(delayTicks, p.GetMin(), p.GetMax()));
        });
    }

    // Exposure, gain, gamma, black level, white balance, auto-exposure
    // target, and digital shift — shared with apply_live_params() so a
    // later UI edit doesn't require a full reopen to take effect.
    apply_image_params();

    d->deviceOpen.store(true);

    // Diagnostic: log actual GigE transport settings (best-effort, non-fatal).
    {
        auto& cam2 = d->camera.GetNodeMap();
        auto safe_i = [&](const char* name) -> int64_t {
            try { return Pylon::CIntegerParameter(cam2, name).GetValue(); } catch (...) { return -1; }
        };
        auto safe_f = [&](const char* name) -> double {
            try { return Pylon::CFloatParameter(cam2, name).GetValue(); } catch (...) { return -1.0; }
        };
        // SFNC 2.0: ResultingFrameRate — SFNC 1.x (e.g. ace-classic GigE
        // cameras like the acA1920-25gc): ResultingFrameRateAbs. Same
        // dual-name pattern as AcquisitionFrameRate/ExposureTime above;
        // without this fallback this always silently returned -1 here.
        auto safe_f_fallback = [&](const char* primary, const char* fallback) -> double {
            try { return Pylon::CFloatParameter(cam2, primary).GetValue(); }
            catch (const Pylon::GenericException&) {
                try { return Pylon::CFloatParameter(cam2, fallback).GetValue(); }
                catch (...) { return -1.0; }
            }
        };
        const int64_t scftd = safe_i("GevSCFTD");
        const int64_t scbwa = safe_i("GevSCBWA");
        const int64_t pktSz = safe_i("GevSCPSPacketSize");
        const int64_t scpd  = safe_i("GevSCPD");
        const int64_t freq  = safe_i("GevTimestampTickFrequency");
        d->tickFreqHz = freq;
        const double  rfps  = safe_f_fallback("ResultingFrameRate", "ResultingFrameRateAbs");
        d->resultingFps = rfps; // see achievable_fps()
        log_info(QString("[Camera %1] GigE pkt=%2B scpd=%3 scftd=%4 scbwa=%5 resultFPS=%6 tickFreq=%7Hz")
                     .arg(d->cameraIndex)
                     .arg(pktSz).arg(scpd).arg(scftd).arg(scbwa)
                     .arg(rfps).arg(freq));
        if (scftd > 0 && freq > 0) {
            log_warning(QString("[Camera %1] GevSCFTD=%2 ticks = %3 ms — camera delays frame transmission")
                            .arg(d->cameraIndex).arg(scftd)
                            .arg(scftd * 1000.0 / freq, 0, 'f', 2));
        }
        // The requested AcquisitionFrameRate is not guaranteed to be
        // achievable — exposure time, ROI, and GigE bandwidth all cap the
        // real delivered rate, and Pylon accepts an unachievable request
        // without error (it just silently under-delivers). Surface a real
        // mismatch here instead of leaving it silent — this is a likely
        // contributor to cross-camera frame-count differences (see
        // SyncManifest / docs on synchronization).
        if (d->params.specifyFps && rfps > 0.0 && rfps < d->params.fps * 0.9) {
            log_warning(QString("[Camera %1] Requested %2 fps but the camera can only sustain "
                                "~%3 fps at the current exposure/ROI/bandwidth settings — "
                                "lower the exposure time or the configured frame rate to match.")
                            .arg(d->cameraIndex)
                            .arg(d->params.fps, 0, 'f', 1)
                            .arg(rfps, 0, 'f', 1));
        }
    }

    // Read back actual values for the log message.
    log_info(QString("[Camera %1] reading back dimensions from node map").arg(d->cameraIndex));
    try {
        auto& cam = d->camera.GetNodeMap();
        log_info(QString("[Camera %1] GetNodeMap ok").arg(d->cameraIndex));
        const int    w   = static_cast<int>(Pylon::CIntegerParameter(cam,"Width").GetValue());
        log_info(QString("[Camera %1] Width=%2").arg(d->cameraIndex).arg(w));
        const int    h   = static_cast<int>(Pylon::CIntegerParameter(cam,"Height").GetValue());
        const double fps = d->params.specifyFps
            ? d->params.fps
            : Pylon::CFloatParameter(cam,"ResultingFrameRate").GetValue();
        log_info(QString("[Camera %1] Opened: %2\xd7%3 @ %4 fps (serial: %5)")
                     .arg(d->cameraIndex).arg(w).arg(h).arg(fps)
                     .arg(d->params.serialNumber));
        emit opened(d->cameraIndex, w, h, fps);
    } catch (const Pylon::GenericException& e) {
        log_warning(QString("[Camera %1] Pylon exception reading dimensions: %2")
                        .arg(d->cameraIndex)
                        .arg(QString::fromLocal8Bit(e.GetDescription())));
        log_info(QString("[Camera %1] Opened (serial: %2)")
                     .arg(d->cameraIndex).arg(d->params.serialNumber));
        emit opened(d->cameraIndex, d->params.width, d->params.height, d->params.fps);
    } catch (const std::exception& e) {
        log_error(QString("[Camera %1] std::exception reading dimensions: %2")
                      .arg(d->cameraIndex).arg(QString::fromLocal8Bit(e.what())));
        emit opened(d->cameraIndex, d->params.width, d->params.height, d->params.fps);
    } catch (...) {
        log_error(QString("[Camera %1] unknown exception reading dimensions").arg(d->cameraIndex));
        emit opened(d->cameraIndex, d->params.width, d->params.height, d->params.fps);
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
    emit opened(d->cameraIndex, d->params.width, d->params.height, d->params.fps);
    return true;
#endif
}

// Writes the image-processing subset of CameraParameters (exposure, gain,
// gamma, black level, white balance, auto-exposure target, digital shift)
// onto the currently-open camera. Called once from open() and again from
// apply_live_params() whenever the UI edits one of these fields — this is
// the single place that subset of node-writes exists.
void VideoGrabber::apply_image_params() {
#if defined(MOSAIC_HAVE_CAMERAS)
    if (!d->camera.IsOpen()) return;

    auto& cam = d->camera.GetNodeMap();
    using Pylon::CIntegerParameter;
    using Pylon::CFloatParameter;
    using Pylon::CEnumParameter;
    const int idx = d->cameraIndex;
    auto try_set = [&](const char* name, auto setter) {
        try { setter(); }
        catch (const Pylon::GenericException& e) {
            log_warning(QString("[Camera %1] Skipping '%2': %3")
                            .arg(idx).arg(name)
                            .arg(QString::fromLocal8Bit(e.GetDescription())));
        }
    };

    try_set("ExposureAuto", [&]{
        CEnumParameter(cam, "ExposureAuto")
            .SetValue(d->params.exposureAuto.toStdString().c_str());
    });
    if (d->params.exposureAuto == "Off") {
        // SFNC 2.0: ExposureTime (float, µs)
        // SFNC 1.x: ExposureTimeAbs (float, µs) — same unit, different name
        try_set("ExposureTime", [&]{
            try {
                CFloatParameter(cam, "ExposureTime").SetValue(d->params.exposureTimeUs);
            } catch (const Pylon::GenericException&) {
                CFloatParameter(cam, "ExposureTimeAbs").SetValue(d->params.exposureTimeUs);
            }
        });
    }

    try_set("GainAuto", [&]{
        CEnumParameter(cam, "GainAuto")
            .SetValue(d->params.gainAuto.toStdString().c_str());
    });
    if (d->params.gainAuto == "Off") {
        // SFNC 2.0: Gain (float, dB) — clamp to camera's valid range.
        // SFNC 1.x: GainRaw (integer, device units) — skip if Gain not present,
        //           since converting dB→raw requires knowing the camera's scale.
        try_set("Gain", [&]{
            auto p = CFloatParameter(cam, "Gain");
            p.SetValue(std::clamp(d->params.gainDb, p.GetMin(), p.GetMax()));
        });
    }

    try_set("Gamma", [&]{ CFloatParameter(cam, "Gamma").SetValue(d->params.gamma); });

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
    try_set("BlackLevel", [&]{
        if (cam.GetNode("BlackLevel") != nullptr) {
            CFloatParameter(cam, "BlackLevel").SetValue(d->params.blackLevel);
        } else {
            auto p = CIntegerParameter(cam, "BlackLevelRaw");
            p.SetValue(round_clamp_to_int_range(d->params.blackLevel, p.GetMin(), p.GetMax()));
        }
    });

    try_set("BalanceWhiteAuto", [&]{
        CEnumParameter(cam, "BalanceWhiteAuto")
            .SetValue(d->params.balanceWhiteAuto.toStdString().c_str());
    });

    // AutoTargetBrightness: the SFNC 2.0 float node (0.0-1.0) isn't present
    // on this camera generation — it exposes the same concept as
    // AutoTargetValue, an integer in device brightness units (confirmed
    // range ~50-205 on a real unit, but read live rather than hardcoded in
    // case it varies by model). Check node existence first, same rationale
    // as BlackLevel above.
    try_set("AutoTargetBrightness", [&]{
        if (cam.GetNode("AutoTargetBrightness") != nullptr) {
            CFloatParameter(cam, "AutoTargetBrightness").SetValue(d->params.autoTargetBrightness);
        } else {
            auto p = CIntegerParameter(cam, "AutoTargetValue");
            p.SetValue(map_normalized_to_int_range(d->params.autoTargetBrightness, p.GetMin(), p.GetMax()));
        }
    });

    try_set("DigitalShift", [&]{
        auto p = CIntegerParameter(cam, "DigitalShift");
        p.SetValue(std::clamp(static_cast<int64_t>(d->params.digitalShift), p.GetMin(), p.GetMax()));
    });

    // Saturation, Contrast, Brightness, and TestPattern have no GenICam node
    // on this ace-classic camera generation at all (confirmed against a real
    // unit — this hardware has no on-camera ISP for those). They stay
    // UI-only; testPattern is intentionally UI-only regardless of hardware
    // generation — it only drives the stub/no-camera preview generator, per
    // CameraParameters' own doc comment.
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
                 .arg(false).arg(false).arg(false)
#endif
             );
#if defined(MOSAIC_HAVE_CAMERAS)
    try {
        if (d->camera.IsGrabbing())             { d->camera.StopGrabbing();  log_info(QString("[Camera %1] StopGrabbing done").arg(d->cameraIndex)); }
        if (d->camera.IsOpen())                 { d->camera.Close();         log_info(QString("[Camera %1] Close done").arg(d->cameraIndex)); }
        if (d->camera.IsPylonDeviceAttached())  { d->camera.DestroyDevice(); log_info(QString("[Camera %1] DestroyDevice done").arg(d->cameraIndex)); }
    } catch (...) { log_error(QString("[Camera %1] close() exception caught").arg(d->cameraIndex)); }

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
    emit closed(d->cameraIndex);
}

// ── Grab control ───────────────────────────────────────────────────────────

void VideoGrabber::start_grabbing() {
    if (!d->deviceOpen.load()) { return; }
    d->frameCounter.store(0);
    d->dropCounter.store(0);
    d->lastFrameElapsedNs.store(-1);
    d->actuallyGrabbing.store(false);
    QThread::start();
}

void VideoGrabber::stop_grabbing() {
    if (!isRunning()) return;
    requestInterruption();
    if (!wait(5000)) {
        log_warning(QString("[Camera %1] stop_grabbing: thread did not finish in 5 s — forcing terminate")
                        .arg(d->cameraIndex));
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
        } catch (...) {}

        d->camera.StartGrabbing(Pylon::GrabStrategy_LatestImageOnly,
                                Pylon::GrabLoop_ProvidedByUser);
        d->actuallyGrabbing.store(true);

        Pylon::CGrabResultPtr  result;
        Pylon::CPylonImage     convertedImage;
        auto lastFpsTime    = SteadyClock::now();
        auto lastWarnTime   = SteadyClock::now();
        int64_t fpsCount    = 0;
        int64_t warnCount   = 0;
        int previewSkip     = 0;

        while (!isInterruptionRequested()) {
            if (d->liveApplyPending.exchange(false)) {
                apply_image_params();
            }
            if (!d->camera.RetrieveResult(100, result, Pylon::TimeoutHandling_Return)) {
                continue;
            }
            if (!result->GrabSucceeded()) {
                ++warnCount;
                const auto now = SteadyClock::now();
                if (std::chrono::duration<double>(now - lastWarnTime).count() >= 5.0) {
                    log_warning(QString("[Camera %1] %2 incomplete frame(s) in last 5 s "
                                        "(GigE packet loss — check NIC jumbo frames and switch bandwidth)")
                                    .arg(d->cameraIndex).arg(warnCount));
                    warnCount    = 0;
                    lastWarnTime = now;
                }
                continue;
            }

            const int64_t frameId = d->frameCounter.fetch_add(1) + 1;
            const int64_t tsNs    = elapsed_ns();
            const int64_t wallNs  = wall_clock_ns();
            if (d->triggerMgr) {
                d->triggerMgr->publish_frame_marker(d->cameraIndex, frameId, tsNs);
            }
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
                    const int64_t ticks = Pylon::CIntegerParameter(chunkNodeMap, "ChunkTimestamp").GetValue();
                    hwTsNs = static_cast<int64_t>(
                        static_cast<double>(ticks) * (1e9 / static_cast<double>(d->tickFreqHz)));
                } catch (const Pylon::GenericException&) {
                    // Chunk data unavailable for this frame — leave hwTsNs at 0.
                }
            }

            // Convert from the camera's native pixel format (BayerRG, YUV, Mono, …)
            // to BGR8packed so the QImage and VideoFrame always carry BGR data.
            d->converter.Convert(convertedImage, result);

            const int    w   = static_cast<int>(convertedImage.GetWidth());
            const int    h   = static_cast<int>(convertedImage.GetHeight());
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
                QImage full(frame->data.data(), w, h, static_cast<int>(stride), QImage::Format_BGR888);
                const QImage scaled = (w > 640 || h > 360)
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
        emit grab_error(d->cameraIndex,
                        QString::fromLocal8Bit(e.GetDescription()));
    }
    // Broadened beyond Pylon::GenericException: an uncaught exception of any
    // other type escaping this function escapes QThread::run() itself, which
    // is fatal to the whole process (std::terminate()) — report and let the
    // thread exit cleanly instead, same rationale as try_set()'s own
    // broadened catch above.
    catch (const std::exception& e) {
        emit grab_error(d->cameraIndex,
                        QString("Unexpected exception: %1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        emit grab_error(d->cameraIndex, "Unknown non-standard exception in grab loop.");
    }
}
#else
void VideoGrabber::run_pylon_loop() { run_stub_loop(); }
#endif

void VideoGrabber::run_stub_loop() {
    d->actuallyGrabbing.store(true);
    const double fps   = d->params.fps > 0.0 ? d->params.fps : 30.0;
    const int    width  = d->params.width  > 0 ? d->params.width  : 1920;
    const int    height = d->params.height > 0 ? d->params.height : 1080;
    // Convert floating-point period to the steady_clock's native duration type.
    const SteadyClock::duration period =
        std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(1.0 / fps));

    // Pre-allocate a reusable test-pattern buffer (BGR colour bars).
    std::vector<uint8_t> pattern(static_cast<size_t>(width * height * 3));
    {
        // Seven ITU colour bars: white, yellow, cyan, green, magenta, red, blue.
        // Stored as {B, G, R} to match BGR8 layout.
        const std::array<std::array<uint8_t, 3>, 7> bars = {{
            {255,255,255}, {  0,255,255}, {255,255,  0},
            {  0,255,  0}, {255,  0,255}, {  0,  0,255}, {  0,  0,255}
        }};
        const int barW = width / 7;
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const int bar = std::min(col / barW, 6);
                const std::size_t offset = static_cast<std::size_t>((row * width + col) * 3);
                pattern[offset + 0] = bars[static_cast<std::size_t>(bar)][0];
                pattern[offset + 1] = bars[static_cast<std::size_t>(bar)][1];
                pattern[offset + 2] = bars[static_cast<std::size_t>(bar)][2];
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
        frame->data        = pattern;   // copy of pre-built pattern

        d->lastFrameElapsedNs.store(frame->elapsedNs);
        if (d->triggerMgr) {
            d->triggerMgr->publish_frame_marker(d->cameraIndex, frameId, frame->elapsedNs);
        }

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

bool    VideoGrabber::is_open()        const { return d->deviceOpen.load(); }
int64_t VideoGrabber::frames_grabbed() const { return d->frameCounter.load(); }
int64_t VideoGrabber::frames_dropped() const { return d->dropCounter.load(); }
double  VideoGrabber::current_fps()    const { return d->currentFps.load(); }
int64_t VideoGrabber::last_frame_elapsed_ns() const { return d->lastFrameElapsedNs.load(); }

bool     VideoGrabber::action_command_ready()     const { return d->actionCommandReady; }
uint32_t VideoGrabber::action_device_key()        const { return d->actionDeviceKey; }
QString  VideoGrabber::action_broadcast_address() const { return d->actionBroadcastAddress; }

double VideoGrabber::configured_fps() const { return d->params.fps; }
double VideoGrabber::achievable_fps() const { return d->resultingFps; }

} // namespace mosaic
