#pragma once
#include <QString>
#include <QVector>
#include <optional>

namespace mosaic {

/// @brief What analysis/run_clap_check.py found (``<session>/sync_check/
/// clap_check.json``, schema mosaic-clap-check-v1): per camera and
/// microphone, how far the audio is placed from the video, measured on
/// claps heard and seen. See docs/math/clap_check.rst.
class ClapCheckResult {
   public:
    struct Pair {
        int camera     = -1;
        double frameMs = 40.0;
        QString audioFile;
        int matched = 0;
        std::optional<double> medianOffsetMs; ///< audio minus video, ms
        std::optional<double> spreadMs;
        QString verdict; ///< "ok", "check", "off" or "none"
    };

    [[nodiscard]] static ClapCheckResult load(const QString& path);

    [[nodiscard]] bool is_valid() const { return valid_; }
    /// The worst verdict over every camera and microphone.
    [[nodiscard]] const QString& verdict() const { return verdict_; }
    [[nodiscard]] const QVector<Pair>& pairs() const { return pairs_; }
    /// One line per camera and microphone, for the status line.
    [[nodiscard]] QString summary() const;

   private:
    bool valid_ = false;
    QString verdict_;
    QVector<Pair> pairs_;
};

} // namespace mosaic
