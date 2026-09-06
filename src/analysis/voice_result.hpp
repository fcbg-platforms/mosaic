#pragma once
#include <QString>
#include <QVector>
#include <cstdint>

namespace mosaic {

/// Schema version this build understands. A file declaring anything else is
/// rejected outright rather than parsed on a hope: the fields here describe how
/// to interpret an image's pixels, and quietly misreading them produces a
/// picture that looks entirely plausible and is wrong.
inline constexpr int k_voice_schema_version = 1;

/// How to read the spectrogram PNG that accompanies a .voice.json.
struct VoiceSpectrogramMeta {
    /// Absolute path, resolved next to the JSON. Empty when the file declared
    /// no image, which is legal — tracks alone are still worth drawing.
    QString imagePath;
    int width  = 0;
    int height = 0;

    /// Time span the image covers. Deliberately separate from the clip's own
    /// duration: parselmouth, the WAV header and QMediaPlayer can disagree by
    /// tens of milliseconds, so the image is drawn into *its* slice of the
    /// widget's timeline rather than being allowed to define that timeline.
    double t0Ms = 0.0;
    double t1Ms = 0.0;

    double f0Hz = 0.0;
    double f1Hz = 0.0;

    /// The dB values that image intensities 0 and 255 correspond to. Kept so a
    /// readout or colour bar can name real units later.
    double dbMin = 0.0;
    double dbMax = 0.0;

    [[nodiscard]] bool has_image() const { return !imagePath.isEmpty() && width > 0 && height > 0; }
};

/// A uniformly-sampled curve: value i is at t0Ms + i * dtMs.
///
/// Uniform rather than (time, value) pairs because Praat's own output already
/// is, so nothing is lost, the file is half the size, and lookup is arithmetic
/// instead of a search.
struct VoiceTrack {
    double t0Ms = 0.0;
    double dtMs = 0.0;
    QVector<float> values;

    /// dtMs > 0 is load-bearing, not cosmetic: every lookup divides by it.
    [[nodiscard]] bool is_valid() const { return dtMs > 0.0 && !values.isEmpty(); }

    /// Nearest sample to `ms`, or `fallback` outside the track's span.
    [[nodiscard]] float at(double ms, float fallback) const;
};

/// Parses a "<name>.voice.json" written by analysis/run_voice.py — the
/// spectrogram, pitch track and intensity contour drawn beneath the waveform
/// on the Speaker Diarization page.
///
/// QtCore-only on purpose. The PNG is located but never loaded here, so this
/// stays free of QtGui and can be compiled into mosaic_tests, which links only
/// Qt6::Core and Qt6::Network. Everything that decides how the image is
/// *interpreted* therefore has test coverage; only the drawing does not.
class VoiceResult {
   public:
    VoiceResult() = default;

    /// Returns a default-constructed (is_valid() == false) result if the file
    /// is missing, malformed, or declares a schema this build cannot read.
    static VoiceResult load(const QString& jsonPath);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] const QString& source_audio() const { return sourceAudio_; }
    [[nodiscard]] double duration_ms() const { return durationMs_; }
    [[nodiscard]] const VoiceSpectrogramMeta& spectrogram() const { return spectrogram_; }

    /// Hz per frame; 0 means unvoiced, which is a value and not an absence —
    /// the widget must break its line there rather than interpolate across it.
    [[nodiscard]] const VoiceTrack& pitch() const { return pitch_; }
    [[nodiscard]] double pitch_floor_hz() const { return pitchFloorHz_; }
    [[nodiscard]] double pitch_ceiling_hz() const { return pitchCeilingHz_; }

    [[nodiscard]] const VoiceTrack& intensity() const { return intensity_; }

   private:
    bool valid_ = false;
    QString sourceAudio_;
    double durationMs_ = 0.0;
    VoiceSpectrogramMeta spectrogram_;
    VoiceTrack pitch_;
    double pitchFloorHz_   = 0.0;
    double pitchCeilingHz_ = 0.0;
    VoiceTrack intensity_;
};

} // namespace mosaic
