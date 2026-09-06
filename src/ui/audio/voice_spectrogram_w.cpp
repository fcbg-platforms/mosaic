#include "ui/audio/voice_spectrogram_w.hpp"

#include <QFontMetrics>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <algorithm>
#include <cmath>
#include <vector>

#include "ui/audio/spectrogram_colormap.hpp"
#include "ui/audio/time_axis.hpp"

namespace mosaic {

namespace {

// Matches AudioWaveformW's background exactly; the two strips read as one
// surface split in half rather than as two unrelated panels.
constexpr QColor kBackground(0x08, 0x08, 0x16);

// Same height as the waveform's speaker strips, so a turn's marker lines up
// vertically across both widgets.
constexpr qreal kBandStripHeight = 9.0;

constexpr QColor kPitchColor(0x33, 0xe0, 0xff);
constexpr QColor kIntensityColor(0xff, 0xd2, 0x4a);
constexpr QColor kPlayheadColor(0xff, 0xdd, 0x55);
constexpr QColor kAxisTextColor(0x9a, 0x9a, 0xc8);

constexpr double kDefaultDisplayMaxHz = 5000.0;

// Intensity is drawn against a rolling window below its own peak rather than an
// absolute dB scale: absolute levels depend entirely on mic gain, so a fixed
// range would leave the contour pinned to the floor on a quiet recording and
// clipped on a loud one.
constexpr double kIntensitySpanDb = 50.0;

struct Track {
    double t0Ms = 0.0;
    double dtMs = 0.0;
    QVector<float> values;

    [[nodiscard]] bool valid() const { return dtMs > 0.0 && !values.isEmpty(); }
    [[nodiscard]] double time_of(int i) const { return t0Ms + i * dtMs; }
};

} // namespace

struct VoiceSpectrogramW::Impl {
    qint64 durationMs{0};
    qint64 playheadMs{0};

    // Colormap already applied, at the PNG's native resolution.
    QImage colored;
    double f0Hz{0.0};
    double f1Hz{0.0};
    double t0Ms{0.0};
    double t1Ms{0.0};
    double displayMaxHz{kDefaultDisplayMaxHz};

    // `colored` scaled to the current widget size. Rebuilt on resize only —
    // set_playhead_ms() fires on every player position update, and rescaling a
    // 4096-wide image per paint would cost 5-10 ms a tick.
    QPixmap cache;
    QSize cacheFor;
    // Part of the cache key, not just the size: set_displayed_max_frequency()
    // changes which rows are drawn, and a cache keyed on size alone would keep
    // serving the previous band while the frequency labels moved — the plot
    // would then actively misstate its own axis.
    double cacheForMaxHz{-1.0};

    Track pitch;
    double pitchFloorHz{60.0};
    double pitchCeilingHz{600.0};
    Track intensity;

    bool showPitch{true};
    bool showIntensity{true};

    std::vector<SpeakerBand> bands;
    std::function<void(qint64)> seekCb;
};

VoiceSpectrogramW::VoiceSpectrogramW(QWidget* parent)
    : QWidget(parent), d(std::make_unique<Impl>()) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setContentsMargins(0, 0, 0, 0); // alignment: see time_axis.hpp
}

VoiceSpectrogramW::~VoiceSpectrogramW() = default;

QSize VoiceSpectrogramW::sizeHint() const { return {400, 180}; }

// ── Inputs ─────────────────────────────────────────────────────────────────

void VoiceSpectrogramW::set_duration_ms(qint64 durationMs) {
    d->durationMs = std::max<qint64>(0, durationMs);
    update();
}

bool VoiceSpectrogramW::set_spectrogram(const QString& imagePath, double f0Hz, double f1Hz,
                                        double t0Ms, double t1Ms) {
    d->colored = QImage();
    d->cache   = QPixmap();
    d->f0Hz    = f0Hz;
    d->f1Hz    = f1Hz;
    d->t0Ms    = t0Ms;
    d->t1Ms    = t1Ms;

    QImage src(imagePath);
    if (src.isNull()) {
        update();
        return false;
    }
    // Defensive even though run_voice.py writes mode "L": a palette PNG loads
    // as Format_Indexed8 carrying the file's own colour table, and indexing our
    // colormap with those values would be meaningless. Converting first makes
    // the pixel value an intensity in both cases.
    src = src.convertToFormat(QImage::Format_Grayscale8);

    const auto table = spectrogram_color_table(SpectrogramColormap::Magma);
    d->colored       = QImage(src.width(), src.height(), QImage::Format_RGB32);
    for (int y = 0; y < src.height(); ++y) {
        // scanLine(), never bits() + y * width(): QImage rows are padded to a
        // 4-byte boundary, so an odd width makes the naive arithmetic drift a
        // few pixels further left on every row and shears the whole image.
        const uchar* in = src.constScanLine(y);
        auto* out       = reinterpret_cast<QRgb*>(d->colored.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const Rgb8 c = table[in[x]];
            out[x]       = qRgb(c.r, c.g, c.b);
        }
    }
    update();
    return true;
}

void VoiceSpectrogramW::set_pitch_track(double t0Ms, double dtMs, const QVector<float>& hz,
                                        double floorHz, double ceilingHz) {
    d->pitch          = {t0Ms, dtMs, hz};
    d->pitchFloorHz   = std::max(1.0, floorHz);
    d->pitchCeilingHz = std::max(d->pitchFloorHz + 1.0, ceilingHz);
    update();
}

void VoiceSpectrogramW::set_intensity_track(double t0Ms, double dtMs, const QVector<float>& db) {
    d->intensity = {t0Ms, dtMs, db};
    update();
}

void VoiceSpectrogramW::set_speaker_bands(const QVector<SpeakerBand>& bands) {
    d->bands.assign(bands.begin(), bands.end());
    update();
}

void VoiceSpectrogramW::set_playhead_ms(qint64 ms) {
    d->playheadMs = std::clamp<qint64>(ms, 0, d->durationMs);
    update();
}

void VoiceSpectrogramW::set_seek_callback(std::function<void(qint64)> cb) {
    d->seekCb = std::move(cb);
}

void VoiceSpectrogramW::set_displayed_max_frequency(double hz) {
    d->displayMaxHz = std::max(100.0, hz);
    update();
}

void VoiceSpectrogramW::set_show_pitch(bool on) {
    d->showPitch = on;
    update();
}

void VoiceSpectrogramW::set_show_intensity(bool on) {
    d->showIntensity = on;
    update();
}

void VoiceSpectrogramW::clear() {
    d->colored   = QImage();
    d->cache     = QPixmap();
    d->pitch     = {};
    d->intensity = {};
    d->bands.clear();
    d->durationMs = 0;
    d->playheadMs = 0;
    update();
}

bool VoiceSpectrogramW::has_data() const {
    return !d->colored.isNull() || d->pitch.valid() || d->intensity.valid();
}

// ── Paint ──────────────────────────────────────────────────────────────────

void VoiceSpectrogramW::resizeEvent(QResizeEvent* event) {
    d->cache = QPixmap(); // invalidate; rebuilt lazily on the next paint
    QWidget::resizeEvent(event);
}

void VoiceSpectrogramW::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    const QRectF rc = rect();
    p.fillRect(rc, kBackground);

    if (!has_data() || d->durationMs <= 0) {
        p.setPen(QColor(0x60, 0x60, 0xa0));
        p.drawText(rc, Qt::AlignCenter, "No acoustic analysis yet — click ▶ Acoustics");
        return;
    }

    // ── Spectrogram ────────────────────────────────────────────────────────
    if (!d->colored.isNull()) {
        // Everything here is inside the cache guard on purpose. paintEvent runs
        // on every playhead tick, and both the crop and the scale allocate —
        // the crop alone is a multi-megabyte deep copy at full resolution. Only
        // a resize or a band change may pay for them.
        if (d->cache.isNull() || d->cacheFor != size() || d->cacheForMaxHz != d->displayMaxHz) {
            // Rows are stored highest frequency first, so the band
            // [f0Hz, displayMaxHz] is the *bottom* slice of the image.
            const double span  = std::max(d->f1Hz - d->f0Hz, 1e-6);
            const double shown = std::clamp(d->displayMaxHz - d->f0Hz, 1e-6, span);
            const int keepRows =
                std::max(1, static_cast<int>(std::lround(d->colored.height() * shown / span)));
            const QImage banded =
                d->colored.copy(0, d->colored.height() - keepRows, d->colored.width(), keepRows);

            d->cache = QPixmap::fromImage(
                banded.scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            d->cacheFor      = size();
            d->cacheForMaxHz = d->displayMaxHz;
        }
        // Into the image's own slice of the widget's timeline, never the whole
        // widget: the clip's duration and the analysed span can differ.
        const double x0 = time_to_x(static_cast<int64_t>(d->t0Ms), d->durationMs, rc.width());
        const double x1 = time_to_x(static_cast<int64_t>(d->t1Ms), d->durationMs, rc.width());
        p.drawPixmap(QRectF(x0, 0, std::max(x1 - x0, 1.0), rc.height()), d->cache,
                     QRectF(d->cache.rect()));
    }

    // ── Speaker bands: strips only ─────────────────────────────────────────
    // No full-height wash here, unlike the waveform. A wash would tint the
    // spectrogram's own colours and make the magma scale unreadable — the very
    // thing this widget exists to show.
    for (const auto& band : d->bands) {
        if (!band.color.isValid()) {
            continue;
        }
        const double x0 = time_to_x(band.startMs, d->durationMs, rc.width());
        const double x1 = time_to_x(band.endMs, d->durationMs, rc.width());
        if (x1 <= x0) {
            continue;
        }
        p.fillRect(QRectF(x0, 0, x1 - x0, kBandStripHeight), band.color);
        QColor edge = band.color;
        edge.setAlpha(90);
        p.setPen(QPen(edge, 1));
        p.drawLine(QPointF(x0, 0), QPointF(x0, rc.height()));
    }

    // ── Intensity ──────────────────────────────────────────────────────────
    if (d->showIntensity && d->intensity.valid()) {
        double peak = -1e9;
        for (const float v : d->intensity.values) {
            peak = std::max(peak, static_cast<double>(v));
        }
        const double floorDb = peak - kIntensitySpanDb;

        QPainterPath path;
        bool started = false;
        for (int i = 0; i < d->intensity.values.size(); ++i) {
            const double x =
                time_to_x(static_cast<int64_t>(d->intensity.time_of(i)), d->durationMs, rc.width());
            const double norm =
                std::clamp((d->intensity.values[i] - floorDb) / kIntensitySpanDb, 0.0, 1.0);
            const QPointF pt(x, rc.height() * (1.0 - norm));
            if (started) {
                path.lineTo(pt);
            } else {
                path.moveTo(pt);
                started = true;
            }
        }
        QColor c = kIntensityColor;
        c.setAlpha(170);
        p.setPen(QPen(c, 1.4));
        p.drawPath(path);
    }

    // ── Pitch ──────────────────────────────────────────────────────────────
    if (d->showPitch && d->pitch.valid()) {
        // Logarithmic. A male and a female voice in the same recording span
        // roughly 80-350 Hz; on a linear axis the lower contour is squashed
        // into the bottom fifth and the two are not comparable.
        const double logFloor = std::log(d->pitchFloorHz);
        const double logSpan  = std::max(std::log(d->pitchCeilingHz) - logFloor, 1e-6);

        QPainterPath path;
        bool inRun = false;
        for (int i = 0; i < d->pitch.values.size(); ++i) {
            const float hz = d->pitch.values[i];
            if (hz <= 0.0f) {
                // Unvoiced. Break the path rather than letting it run: a single
                // continuous line draws long diagonals across every silence,
                // which is the classic wrong-looking pitch plot.
                inRun = false;
                continue;
            }
            const double x =
                time_to_x(static_cast<int64_t>(d->pitch.time_of(i)), d->durationMs, rc.width());
            const double norm = std::clamp((std::log(hz) - logFloor) / logSpan, 0.0, 1.0);
            const QPointF pt(x, rc.height() * (1.0 - norm));
            if (inRun) {
                path.lineTo(pt);
            } else {
                path.moveTo(pt);
                inRun = true;
            }
        }
        QColor c = kPitchColor;
        c.setAlpha(230);
        p.setPen(QPen(c, 2.0));
        p.drawPath(path);
    }

    // ── Playhead ───────────────────────────────────────────────────────────
    {
        const double x = time_to_x(d->playheadMs, d->durationMs, rc.width());
        p.setPen(QPen(kPlayheadColor, 2));
        p.drawLine(QPointF(x, 0), QPointF(x, rc.height()));
    }

    // ── Frequency labels, overlaid ─────────────────────────────────────────
    // Overlaid rather than in a left gutter: a gutter here and not on the
    // waveform above would shift this widget's time origin relative to it, and
    // the two strips would quietly disagree about where a moment is.
    if (!d->colored.isNull()) {
        QFont axisFont = p.font();
        axisFont.setPixelSize(8);
        p.setFont(axisFont);
        const QFontMetrics fm(axisFont);

        // The drawn band is clamped to what was analysed, so a recording whose
        // ceiling is below displayMaxHz fills the height with 0..f1Hz. Labelling
        // against displayMaxHz there would put every gridline at the wrong
        // frequency.
        const double axisTopHz = std::min(d->displayMaxHz, d->f1Hz);
        for (double hz = 1000.0; hz < axisTopHz; hz += 1000.0) {
            const double y = rc.height() * (1.0 - hz / axisTopHz);
            p.setPen(QPen(QColor(0xff, 0xff, 0xff, 28), 1, Qt::DotLine));
            p.drawLine(QPointF(0, y), QPointF(rc.width(), y));

            const QString label = QString("%1k").arg(hz / 1000.0, 0, 'g', 2);
            const int tw        = fm.horizontalAdvance(label);
            p.fillRect(QRectF(2, y - 11, tw + 6, 12), QColor(0, 0, 0, 130));
            p.setPen(kAxisTextColor);
            p.drawText(QRectF(4, y - 11, tw + 2, 12), Qt::AlignVCenter | Qt::AlignLeft, label);
        }
    }
}

void VoiceSpectrogramW::mousePressEvent(QMouseEvent* event) {
    if (!d->seekCb || d->durationMs <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    d->seekCb(x_to_time(event->pos().x(), d->durationMs, width()));
    QWidget::mousePressEvent(event);
}

} // namespace mosaic
