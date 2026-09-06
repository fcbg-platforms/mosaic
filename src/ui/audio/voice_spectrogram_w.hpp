#pragma once
#include <QColor>
#include <QVector>
#include <QWidget>
#include <functional>
#include <memory>

namespace mosaic {

/// Spectrogram with the pitch track and intensity contour drawn over it, sized
/// and positioned to sit directly beneath AudioWaveformW showing the same clip.
///
/// Pure QPainter, matching this project's split: Qt Charts for post-hoc
/// whole-dataset line plots, raw QPainter for anything raster or streaming (see
/// AudioWaveformW and RealtimeTraceW). Qt Charts has no heatmap series at all,
/// so there was never a choice here.
///
/// **Alignment with the waveform above is the constraint everything else bends
/// around.** Both widgets map time to x through mosaic::time_to_x() with the
/// same duration, and both paint edge-to-edge over rect() with no gutter — a
/// gutter on one and not the other would shift the time origin between them and
/// they would disagree about where a moment is while each looked perfectly
/// reasonable. Frequency labels are therefore overlaid on the plot rather than
/// inset beside it.
class VoiceSpectrogramW : public QWidget {
    Q_OBJECT

   public:
    /// One speaker turn.
    ///
    /// Carries a resolved colour rather than a speaker name, deliberately: the
    /// colour comes from AudioWaveformW::speaker_color(), so the two strips
    /// agree by construction instead of by two independent palette assignments
    /// happening to produce the same answer. An invalid QColor means "no
    /// speaker attributed" and draws nothing — the same honesty the waveform
    /// applies, since a colour there would assert an attribution the data does
    /// not contain.
    struct SpeakerBand {
        qint64 startMs = 0;
        qint64 endMs   = 0;
        QColor color;
    };

    explicit VoiceSpectrogramW(QWidget* parent = nullptr);
    ~VoiceSpectrogramW() override;

    /// The x-axis span, in the same clip-relative timeline the waveform above
    /// was given. Callers **must** pass the identical value to both; that is
    /// what makes the two align pixel-for-pixel.
    void set_duration_ms(qint64 durationMs);

    /// Loads an 8-bit greyscale PNG (row 0 = f1Hz) and bakes the colormap in
    /// once. The image is drawn into the sub-rect spanning [t0Ms, t1Ms] of
    /// set_duration_ms()'s span — it never defines the axis itself, because
    /// the WAV header, QMediaPlayer and Praat can disagree about a clip's
    /// length by tens of milliseconds.
    ///
    /// Returns false if the file is missing or unreadable; the widget still
    /// renders tracks and bands over a plain background, since a missing image
    /// is no reason to discard data that loaded fine.
    bool set_spectrogram(const QString& imagePath, double f0Hz, double f1Hz, double t0Ms,
                         double t1Ms);

    /// 0 Hz entries mean unvoiced and break the line rather than being plotted.
    void set_pitch_track(double t0Ms, double dtMs, const QVector<float>& hz, double floorHz,
                         double ceilingHz);
    void set_intensity_track(double t0Ms, double dtMs, const QVector<float>& db);

    void set_speaker_bands(const QVector<SpeakerBand>& bands);
    void set_playhead_ms(qint64 ms);
    void set_seek_callback(std::function<void(qint64)> cb);

    /// Crops the drawn band without re-running analysis — the stored image
    /// always covers 0..f1Hz, this only selects which rows are shown. Defaults
    /// to 5 kHz: at this widget's height the full 8 kHz puts a low male voice's
    /// harmonics about two pixels apart, which moirés under smooth scaling.
    void set_displayed_max_frequency(double hz);

    void set_show_pitch(bool on);
    void set_show_intensity(bool on);

    void clear();
    [[nodiscard]] bool has_data() const;

   protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    /// Constant regardless of what is loaded, so the surrounding column does
    /// not jump when a sidecar finishes loading.
    [[nodiscard]] QSize sizeHint() const override;

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
