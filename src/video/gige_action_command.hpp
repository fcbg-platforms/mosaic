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
[[nodiscard]] double action_command_period_ms(const std::vector<double>& targetFps);

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
