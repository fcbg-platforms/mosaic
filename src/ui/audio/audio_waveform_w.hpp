#pragma once
#include <QWidget>
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

public slots:
    /// Append one (min, max) envelope pair for @p channelIndex (0-based),
    /// each in the raw, unscaled range [-1, 1] — display gain is applied in
    /// paintEvent() via scale(), not here. Safe to call from any thread;
    /// internally dispatches via invokeMethod if needed.
    void push_envelope(int channelIndex, float minSample, float maxSample);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
