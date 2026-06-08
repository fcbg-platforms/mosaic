#include "trigger/lsl_inlet.hpp"
#include "utils/logger.hpp"
#include "utils/timestamp.hpp"
#include <QThread>
#include <atomic>

#if defined(MOSAIC_HAVE_LSL)
#  include <lsl_cpp.h>
#endif

namespace mosaic {

// ── Background polling thread ──────────────────────────────────────────────

#if defined(MOSAIC_HAVE_LSL)
class LslPollThread : public QThread {
    Q_OBJECT
public:
    explicit LslPollThread(std::unique_ptr<lsl::stream_inlet> inlet,
                            const LslInletConfig&              config,
                            QObject*                           parent)
        : QThread(parent)
        , m_inlet(std::move(inlet))
        , m_config(config)
    {}

    std::atomic<int64_t> sampleCount{0};

signals:
    void marker_received(mosaic::TriggerEvent event);
    void stream_lost();

protected:
    void run() override {
        std::vector<std::string> sample(1);
        while (!isInterruptionRequested()) {
            try {
                const double ts = m_inlet->pull_sample(sample, 0.1);
                if (ts <= 0.0 || sample.empty()) { continue; }
                TriggerEvent ev;
                ev.timestampNs = elapsed_ns();
                ev.source      = "lsl";
                ev.label       = QString::fromStdString(sample[0]);
                ev.value       = 0.0;
                sampleCount.fetch_add(1);
                emit marker_received(std::move(ev));
            } catch (const lsl::lost_error&) {
                emit stream_lost();
                break;
            } catch (...) {}
        }
    }

private:
    std::unique_ptr<lsl::stream_inlet> m_inlet;
    const LslInletConfig&              m_config;
};
#endif

// ── LslInlet ───────────────────────────────────────────────────────────────

struct LslInlet::Impl {
    LslInletConfig config;
    bool           connected{false};

#if defined(MOSAIC_HAVE_LSL)
    std::unique_ptr<LslPollThread> thread;
#endif
};

LslInlet::LslInlet(const LslInletConfig& config, QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>())
{
    d->config = config;
}

LslInlet::~LslInlet() { disconnect(); }

bool LslInlet::connect(double resolveTimeoutSec) {
#if defined(MOSAIC_HAVE_LSL)
    if (!d->config.enabled) { return false; }
    if (d->connected)        { return true;  }

    try {
        log_info(QString("[LSL Inlet] Resolving stream '%1' (type: '%2')…")
                     .arg(d->config.streamName).arg(d->config.streamType));

        std::vector<lsl::stream_info> results =
            lsl::resolve_stream("name",
                                d->config.streamName.toStdString(),
                                1,
                                resolveTimeoutSec);

        if (results.empty()) {
            log_warning(QString("[LSL Inlet] Stream '%1' not found within %2 s.")
                            .arg(d->config.streamName).arg(resolveTimeoutSec));
            return false;
        }

        auto inlet = std::make_unique<lsl::stream_inlet>(results.front());
        d->thread  = std::make_unique<LslPollThread>(std::move(inlet), d->config, this);

        QObject::connect(d->thread.get(), &LslPollThread::marker_received,
                         this, &LslInlet::received, Qt::QueuedConnection);

        QObject::connect(d->thread.get(), &LslPollThread::stream_lost, this,
                         [this] {
                             d->connected = false;
                             emit connection_lost(d->config.streamName);
                             log_warning(QString("[LSL Inlet] Stream '%1' lost.")
                                             .arg(d->config.streamName));
                         }, Qt::QueuedConnection);

        d->thread->start();
        d->connected = true;

        log_info(QString("[LSL Inlet] Connected to stream '%1'.")
                     .arg(d->config.streamName));
        return true;

    } catch (const std::exception& e) {
        log_error(QString("[LSL Inlet] Connection error: %1").arg(e.what()));
        return false;
    }
#else
    (void)resolveTimeoutSec;
    log_info("[LSL Inlet] Built without LSL support — inlet disabled.");
    return false;
#endif
}

void LslInlet::disconnect() {
#if defined(MOSAIC_HAVE_LSL)
    if (d->thread) {
        d->thread->requestInterruption();
        d->thread->wait(3000);
        d->thread.reset();
    }
#endif
    d->connected = false;
}

bool    LslInlet::is_connected()     const { return d->connected; }
int64_t LslInlet::samples_received() const {
#if defined(MOSAIC_HAVE_LSL)
    return d->thread ? d->thread->sampleCount.load() : 0;
#else
    return 0;
#endif
}

} // namespace mosaic

// For classes with Q_OBJECT defined in a .cpp, automoc generates a separate
// .moc file that must be explicitly included.
#if defined(MOSAIC_HAVE_LSL)
#  include "lsl_inlet.moc"
#endif
