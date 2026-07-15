#pragma once
#include <QWidget>
#include <memory>

namespace mosaic {

/// @brief Rolling RMS-history waveform display for multiple audio channels.
///
/// Paints a scrolling time-series of normalised RMS levels, one colored
/// trace per microphone channel.  No external plotting library is needed —
/// everything is drawn with QPainter on a QWidget.
///
/// @par Usage
/// @code{.cpp}
/// auto* wave = new AudioWaveformW;
/// wave->set_channel_count(2);
///
/// // Feed from AudioManager (10-20× per second, already on main thread):
/// connect(audioMgr, &AudioManager::level_rms_changed,
///         wave,     &AudioWaveformW::push_level);
/// @endcode
///
/// @par Visual layout
/// @code
/// ┌──────────────────────────────────────────────────────────┐
/// │ Ch 0 ──────────╮────────────────────────────────────     │
/// │ Ch 1 ─────╮────╮──────╮─────────────────────────────    │
/// │           3s ago  2s ago  1s ago                 now     │
/// └──────────────────────────────────────────────────────────┘
/// @endcode
///
/// History window: 8 seconds at 20 samples/s = 160 samples per channel.
class AudioWaveformW : public QWidget {
    Q_OBJECT
public:
    explicit AudioWaveformW(QWidget* parent = nullptr);
    ~AudioWaveformW() override;

    /// Set how many channels to display (1–8).  Extra push_level() calls for
    /// higher indices are silently ignored; missing channels draw flat.
    void set_channel_count(int count);

    /// @returns Current channel count.
    [[nodiscard]] int channel_count() const;

public slots:
    /// Append one RMS sample for @p channelIndex (0-based) in the range [0, 1].
    /// Safe to call from any thread; internally dispatches via invokeMethod if needed.
    void push_level(int channelIndex, float rms);

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
