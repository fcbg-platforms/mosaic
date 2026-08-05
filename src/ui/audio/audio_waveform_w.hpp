#pragma once
#include <QColor>
#include <QPair>
#include <QVector>
#include <QWidget>
#include <functional>
#include <memory>

namespace mosaic {

/// @brief Rolling bipolar-waveform display for multiple audio channels.
///
/// Paints a scrolling time-series of signed min/max envelope pairs, one
/// colored trace per microphone channel — real excursions both above and
/// below the center line, like a zoomed-out audio-editor waveform, not a
/// rectified RMS meter.  No external plotting library is needed — everything
/// is drawn with QPainter on a QWidget.
///
/// @par Usage
/// @code{.cpp}
/// auto* wave = new AudioWaveformW;
/// wave->set_channel_count(2);
///
/// // Feed from AudioManager (10-20× per second, already on main thread):
/// connect(audioMgr, &AudioManager::envelope_changed,
///         wave,     &AudioWaveformW::push_envelope);
/// @endcode
///
/// @par Visual layout
/// @code
/// ┌──────────────────────────────────────────────────────────┐
/// │ Ch 0 ──╱╲────╱╲╲───────────────────────────────────      │
/// │ Ch 1 ────╲╱────╲╱╲───────────────────────────────       │
/// │           3s ago  2s ago  1s ago                 now     │
/// └──────────────────────────────────────────────────────────┘
/// @endcode
///
/// History window: 8 seconds at 20 samples/s = 160 samples per channel.
class AudioWaveformW : public QWidget {
    Q_OBJECT
public:
    // Vertical-amplitude display scale, applied at paint time (not baked
    // into stored samples) so dragging the scale control re-renders the
    // entire existing history immediately. View-only — never affects the
    // recorded audio.
    static constexpr float kMinScale     = 1.0f;
    static constexpr float kMaxScale     = 20.0f;
    static constexpr float kDefaultScale = 6.0f;

    explicit AudioWaveformW(QWidget* parent = nullptr);
    ~AudioWaveformW() override;

    /// Set how many channels to display (1–8).  Extra push_envelope() calls
    /// for higher indices are silently ignored; missing channels draw flat.
    void set_channel_count(int count);

    /// @returns Current channel count.
    [[nodiscard]] int channel_count() const;

    /// Set the display amplitude scale (clamped to [kMinScale, kMaxScale]);
    /// re-renders the whole visible history at the new scale immediately.
    void set_scale(float scale);

    /// @returns Current display amplitude scale.
    [[nodiscard]] float scale() const;

    /// Switches into static mode and displays a whole pre-recorded clip's
    /// envelope across the full widget width, instead of the live rolling
    /// history used by the Audio settings tab — used by the Analysis tab's
    /// Speaker Diarization results view. @p envelope is one (min, max) pair
    /// per display column, each in [-1, 1], spanning the entire clip; @p
    /// durationMs is the clip's total duration, used by set_playhead_ms() to
    /// place the moving playhead line.
    void set_static_envelope(const QVector<QPair<float, float>>& envelope, qint64 durationMs);

    /// Moves the static-mode playhead line (ignored in live mode, i.e.
    /// before set_static_envelope() has been called).
    void set_playhead_ms(qint64 ms);

    /// Clears static-mode data and returns to live rolling mode.
    void clear_static_envelope();

    /// One speaker's contiguous talking turn, in the same clip-relative
    /// millisecond timeline as set_static_envelope()'s durationMs. Plain,
    /// analysis-decoupled struct — the caller (e.g. AnalysisTabW) converts
    /// from its own TranscriptSegment type; this widget never depends on
    /// analysis-specific headers. `speaker` == empty string means "no
    /// speaker attributed" (a gap/unlabeled stretch) and is never drawn as
    /// a band.
    struct SpeakerBand {
        qint64  startMs = 0;
        qint64  endMs   = 0;
        QString speaker;
    };

    /// Static mode only (ignored before set_static_envelope() has been
    /// called, or after clear_static_envelope()). Colors are assigned
    /// internally — stable for the lifetime of this call, in order of each
    /// speaker's first appearance across @p bands — see speaker_color()/
    /// speaker_legend() for callers that need the same mapping (e.g.
    /// coloring the transcript table to match). Passing an empty vector
    /// clears any previously-set bands.
    void set_speaker_bands(const QVector<SpeakerBand>& bands);

    /// @returns The color assigned to `speaker` by the most recent
    /// set_speaker_bands() call, or a neutral gray if `speaker` is empty or
    /// wasn't present in that call.
    [[nodiscard]] QColor speaker_color(const QString& speaker) const;

    /// @returns The (speaker, color) pairs actually present after the most
    /// recent set_speaker_bands() call, ordered by first appearance — for
    /// building a legend row. Never includes the empty/"no speaker" entry.
    [[nodiscard]] QVector<QPair<QString, QColor>> speaker_legend() const;

    /// Click-to-seek: @p cb is invoked with the clip-relative millisecond
    /// position under the mouse on click. Active only in static mode —
    /// seeking a live rolling view has no meaning, so this is a no-op
    /// there.
    void set_seek_callback(std::function<void(qint64)> cb);

public slots:
    /// Append one (min, max) envelope pair for @p channelIndex (0-based),
    /// each in the raw, unscaled range [-1, 1] — display gain is applied in
    /// paintEvent() via scale(), not here. Safe to call from any thread;
    /// internally dispatches via invokeMethod if needed.
    void push_envelope(int channelIndex, float minSample, float maxSample);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
