#include "video/gige_action_command.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <cmath>
#include <future>
#include <limits>

#if defined(MOSAIC_HAVE_CAMERAS)
#  define NOMINMAX
#  include <pylon/PylonIncludes.h>
#  include <pylon/gige/GigETransportLayer.h>
#endif

namespace mosaic {

std::optional<uint32_t> ipv4_from_dotted(const QString& dotted) {
    const QStringList octets = dotted.split('.');
    if (octets.size() != 4) { return std::nullopt; }

    uint32_t value = 0;
    for (const QString& octet : octets) {
        bool    ok  = false;
        const int v = octet.toInt(&ok);
        if (!ok || v < 0 || v > 255) { return std::nullopt; }
        value = (value << 8) | static_cast<uint32_t>(v);
    }
    return value;
}

QString ipv4_to_dotted(uint32_t addr) {
    return QString("%1.%2.%3.%4")
        .arg((addr >> 24) & 0xFF)
        .arg((addr >> 16) & 0xFF)
        .arg((addr >> 8)  & 0xFF)
        .arg(addr & 0xFF);
}

uint32_t ipv4_broadcast_address(uint32_t ip, uint32_t mask) {
    return ip | ~mask;
}

double action_command_period_ms(const std::vector<double>& targetFps, double marginFactor) {
    double minFps = std::numeric_limits<double>::infinity();
    for (const double fps : targetFps) {
        if (fps > 0.0 && fps < minFps) { minFps = fps; }
    }
    if (!std::isfinite(minFps)) { minFps = k_default_action_fps; }
    if (!(marginFactor > 0.0 && marginFactor <= 1.0)) { marginFactor = 1.0; }
    return 1000.0 / (minFps * marginFactor);
}

bool is_achievable_fps_measurement_warmed_up(double secondsSinceGrabbingStarted,
                                              double warmupSeconds) {
    if (secondsSinceGrabbingStarted < 0.0) { return false; }
    if (!(warmupSeconds > 0.0)) { return true; }
    return secondsSinceGrabbingStarted >= warmupSeconds;
}

#if defined(MOSAIC_HAVE_CAMERAS)
struct ActionCommandSession::Impl {
    Pylon::ITransportLayer*     tl     = nullptr;
    Pylon::IGigETransportLayer* gigeTl = nullptr;

    Impl() {
        try {
            tl     = Pylon::CTlFactory::GetInstance().CreateTl(Pylon::BaslerGigEDeviceClass);
            gigeTl = dynamic_cast<Pylon::IGigETransportLayer*>(tl);
            if (!gigeTl) {
                log_error("[ActionCommand] GigE transport layer unavailable — "
                          "cannot issue action commands.");
            }
        } catch (const Pylon::GenericException& e) {
            log_error(QString("[ActionCommand] CreateTl(BaslerGigE) failed: %1")
                          .arg(QString::fromLocal8Bit(e.GetDescription())));
        }
    }

    ~Impl() {
        if (tl) { Pylon::CTlFactory::GetInstance().ReleaseTl(tl); }
    }
};

ActionCommandSession::ActionCommandSession() : d(std::make_unique<Impl>()) {}
ActionCommandSession::~ActionCommandSession() = default;

bool ActionCommandSession::is_valid() const { return d->gigeTl != nullptr; }

int ActionCommandSession::fire(const std::vector<ActionCommandTarget>& targets,
                                uint32_t groupKey, uint32_t groupMask) {
    if (targets.empty() || !d->gigeTl) { return 0; }

    // Fired concurrently, not in a sequential loop: Basler's own SDK header
    // documents IssueActionCommand() as reaching each target consecutively
    // (not simultaneously) when called back-to-back on one thread, with
    // real, cumulative per-call latency — for up to 6 cameras that bakes in
    // a fixed, camera-order-dependent skew every single tick, working
    // directly against the tight cross-camera simultaneity this feature
    // exists to provide. IssueActionCommand() is documented thread-safe, so
    // firing each target from its own short-lived thread and waiting for
    // all of them keeps one tick's wall-clock cost close to the SLOWEST
    // individual call instead of the sum of all of them. Each target is
    // captured BY VALUE into its own task — capturing the range-for loop
    // variable by reference would be a dangling-reference bug once the loop
    // moves on to the next target before that task has run.
    std::vector<std::future<bool>> results;
    results.reserve(targets.size());
    for (const auto& t : targets) {
        auto* gigeTl = d->gigeTl;
        results.push_back(std::async(std::launch::async, [gigeTl, t, groupKey, groupMask]() {
            try {
                gigeTl->IssueActionCommand(t.deviceKey, groupKey, groupMask,
                                           t.broadcastAddress.toStdString().c_str());
                return true;
            } catch (const Pylon::GenericException& e) {
                log_warning(QString("[ActionCommand] Camera %1 (%2): IssueActionCommand failed: %3")
                                .arg(t.cameraIndex).arg(t.broadcastAddress)
                                .arg(QString::fromLocal8Bit(e.GetDescription())));
                return false;
            }
        }));
    }

    int fired = 0;
    for (auto& f : results) {
        try {
            if (f.get()) { ++fired; }
        } catch (const std::exception& e) {
            log_warning(QString("[ActionCommand] Unexpected exception firing action command: %1")
                            .arg(QString::fromUtf8(e.what())));
        }
    }
    return fired;
}
#else
struct ActionCommandSession::Impl {};
ActionCommandSession::ActionCommandSession() : d(std::make_unique<Impl>()) {}
ActionCommandSession::~ActionCommandSession() = default;
bool ActionCommandSession::is_valid() const { return false; }
int ActionCommandSession::fire(const std::vector<ActionCommandTarget>& /*targets*/,
                                uint32_t /*groupKey*/, uint32_t /*groupMask*/) {
    return 0;
}
#endif

} // namespace mosaic
