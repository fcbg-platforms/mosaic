
.. _program_listing_file_src_video_gige_action_command.hpp:

Program Listing for File gige_action_command.hpp
================================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_video_gige_action_command.hpp>` (``src\video\gige_action_command.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QString>
   #include <cstdint>
   #include <memory>
   #include <optional>
   #include <vector>
   
   namespace mosaic {
   
   // IPv4 helpers. Pure — no Pylon/Qt-beyond-QString dependency — so they are
   // directly unit-testable under MOSAIC_ENABLE_CAMERAS=OFF. Both the 32-bit
   // "ip"/"mask" representation and the dotted-quad strings use the natural
   // left-to-right reading order of an address (byte0<<24 | byte1<<16 |
   // byte2<<8 | byte3), matching what the GevCurrentIPAddress/
   // GevCurrentSubnetMask GenICam integer nodes report on Basler GigE cameras.
   
   // Parses e.g. "192.168.3.42" into its 32-bit representation. Returns
   // std::nullopt for anything that isn't exactly 4 dot-separated octets each
   // in [0,255] — fails closed rather than producing a garbage broadcast
   // address from a malformed node readback.
   [[nodiscard]] std::optional<uint32_t> ipv4_from_dotted(const QString& dotted);
   
   // Inverse of ipv4_from_dotted — formats back to dotted-quad notation, as
   // required by IGigETransportLayer::IssueActionCommand's broadcastAddress
   // string parameter.
   [[nodiscard]] QString ipv4_to_dotted(uint32_t addr);
   
   // Computes the subnet broadcast address: ip | ~mask.
   [[nodiscard]] uint32_t ipv4_broadcast_address(uint32_t ip, uint32_t mask);
   
   // One camera's resolved GigE Vision Action Command target, computed once in
   // VideoGrabber::open() and consumed by VideoManager's start sequencing.
   struct ActionCommandTarget {
       int      cameraIndex = -1;   // VideoGrabber::cameraIndex, for logging only
       uint32_t deviceKey   = 0;
       QString  broadcastAddress;   // dotted-quad, this camera's own subnet broadcast
   };
   
   // MOSAIC-wide constants, shared between VideoGrabber::open() (writes
   // ActionGroupKey/ActionGroupMask onto the camera) and ActionCommandSession::
   // fire() (passes the same values to IssueActionCommand). Not user-
   // configurable — a device key only needs to be unique per camera
   // (cameraIndex+1 already guarantees that) and the group key/mask only need
   // to be internally consistent between what's written to the cameras and
   // what's broadcast; there is no scenario in a single-application,
   // single-room deployment where a user would need to change them.
   inline constexpr uint32_t k_action_group_key  = 0x00000001;
   inline constexpr uint32_t k_action_group_mask = 0xFFFFFFFF; // Pylon::AllGroupMask
   
   // Default firing rate (frames/second) used by action_command_period_ms()
   // when no usable per-camera rate is available. Matches CameraParameters'
   // own documented default acquisition rate for this camera generation.
   inline constexpr double k_default_action_fps = 25.0;
   
   // Safety margin applied to the slowest camera's own measured/configured
   // rate before deriving the shared trigger period — see action_command_
   // period_ms()'s doc comment for why triggering AT a camera's exact ceiling
   // (marginFactor=1.0) isn't safe in practice: a camera's own reported
   // achievable rate (e.g. Basler's ResultingFrameRate) is the rate it can
   // sustain with perfect timing, but the trigger ticker fires from software
   // on a separate thread with real (if small) jitter — any trigger that
   // lands even slightly before the camera has finished its previous
   // exposure/readout cycle is simply missed, not queued. Confirmed on real
   // room-11 hardware 2026-07-27: a camera triggered at exactly its own
   // resultFPS ceiling (0% margin) missed a stable ~39-40% of triggers across
   // several independent runs, on a link independently confirmed not to be a
   // bad cable — backing off the trigger rate below that ceiling is the fix,
   // not a hardware change. 0.85 (14% slower than the ceiling) is a starting
   // point, not a measured-optimal value.
   inline constexpr double k_default_action_margin = 0.85;
   
   // Pure: computes the shared firing period (milliseconds) for a group of
   // cameras being triggered by continuous per-frame Action Commands — the
   // interval between consecutive ActionCommandSession::fire() calls. Uses the
   // MINIMUM of the given per-camera configured frame rates, defensively, so
   // no participating camera is ever triggered faster than its own configured
   // target even if configs differ across cameras — a mismatched config
   // should degrade to the slowest camera's pace, not risk swamping it with
   // triggers it can't keep up with (the same GigE-bandwidth packet-loss
   // failure mode already seen when a requested free-run fps exceeds a
   // camera's achievable rate, just self-inflicted by our own trigger cadence
   // this time). Non-positive values in targetFps are ignored. Returns the
   // period for k_default_action_fps if targetFps is empty or every value is
   // non-positive.
   //
   // marginFactor scales the resolved rate down before computing the period
   // (period = 1000 / (minFps * marginFactor)), so the slowest camera is
   // triggered somewhat below its own ceiling rather than exactly at it — see
   // k_default_action_margin's doc comment above for why this is needed.
   // Values outside (0, 1] are treated as 1.0 (no margin) rather than
   // producing a negative/infinite period from a caller mistake.
   [[nodiscard]] double action_command_period_ms(
       const std::vector<double>& targetFps,
       double                     marginFactor = k_default_action_margin);
   
   // Default minimum time a camera must have been actively grabbing before a
   // freshly-read ResultingFrameRate/ResultingFrameRateAbs is trusted at all —
   // gating on real elapsed time rather than the reading's own magnitude.
   //
   // A magnitude-based approach (reject anything "suspiciously low" relative
   // to the camera's own exposure ceiling) was tried first and reverted after
   // real room-11 data (2026-08-05) contradicted its core assumption. The
   // first version (rejecting readings taken right at open(), well below what
   // exposure alone predicted, e.g. ~2.9 fps against a ~20 fps figure) correctly
   // caught a real bug — but a later, larger comparison across all 6 cameras
   // over one real ~20s recording found several genuinely stable, REPEATED
   // readings sitting well below what even a loosened margin assumed was
   // physically possible: auto-exposure converging near its own configured
   // ceiling in a dim room can legitimately leave real achievable fps far
   // lower than any "1/exposure, with some margin" estimate predicts, and no
   // fixed margin generalizes across however bright or dim the room actually
   // is. Waiting for real elapsed acquisition time instead, then trusting
   // whatever the camera reports once warmed up — however low — needs no
   // per-scene-lighting-tuned constant and can't mistake "real, if
   // disappointing" for "premature". 3.0s is a starting point (this camera
   // generation's "Once" auto-exposure convergence time was never directly
   // measured in isolation), not a measured-optimal value — the periodic
   // self-refresh in VideoGrabber::run_pylon_loop() and the periodic
   // self-correction in VideoManager's ActionCommandTicker::run() mean an
   // under-estimate here still self-corrects within a few more cycles, just
   // not on the very first check.
   inline constexpr double k_default_fps_warmup_seconds = 3.0;
   
   // Pure: true once at least warmupSeconds have elapsed since this camera
   // started grabbing. secondsSinceGrabbingStarted < 0 means "not grabbing yet
   // at all" (e.g. open()'s own one-time diagnostic reading, taken before
   // start_grabbing() is ever called) and is always rejected, regardless of
   // warmupSeconds.
   [[nodiscard]] bool is_achievable_fps_measurement_warmed_up(
       double secondsSinceGrabbingStarted,
       double warmupSeconds = k_default_fps_warmup_seconds);
   
   // Owns the GigE transport layer handle for the lifetime of a continuous,
   // per-frame Action-Command-triggered recording/preview session, so
   // IssueActionCommand() is a lightweight per-tick call rather than paying
   // CTlFactory::CreateTl()/ReleaseTl() cost on every single frame (up to
   // dozens of times a second for the whole session). Not copyable — the
   // underlying transport-layer handle is a single owned resource.
   //
   // Every method is safe to call from stub builds (MOSAIC_HAVE_CAMERAS not
   // defined) — is_valid() is simply always false there, and fire() always
   // returns 0.
   class ActionCommandSession {
   public:
       ActionCommandSession();
       ~ActionCommandSession();
       ActionCommandSession(const ActionCommandSession&)            = delete;
       ActionCommandSession& operator=(const ActionCommandSession&) = delete;
   
       // True if the GigE transport layer was successfully obtained — false
       // means fire() will always no-op (stub build, or no GigE transport
       // layer available on this machine).
       [[nodiscard]] bool is_valid() const;
   
       // Issues one IssueActionCommand() per target, back-to-back, in the
       // order given, with timeoutMs=0 (fire-and-forget — no acknowledgement
       // wait). Each camera lives on its own isolated /24 subnet (dedicated
       // NIC per camera, no shared switch — see the room-11 network setup),
       // so there is no single broadcast that reaches all of them; one call
       // per target's own broadcastAddress is unavoidable and intentional.
       //
       // Best-effort per target: a failure issuing to one camera's subnet is
       // logged and does NOT abort the remaining targets — a partial fire is
       // still strictly better than none.
       //
       // Returns the number of IssueActionCommand() calls that did not throw.
       // Always 0 if targets is empty or !is_valid().
       int fire(const std::vector<ActionCommandTarget>& targets,
                uint32_t groupKey, uint32_t groupMask);
   
   private:
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
