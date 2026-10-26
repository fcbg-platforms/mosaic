#pragma once
#include <QPair>
#include <QString>
#include <QVector>

namespace mosaic {

/// @brief What analysis/run_hands.py found in one camera's video
/// (``<session>/hands/video_N.hands.json``, schema mosaic-hands-v1): the
/// model used, and each hand found, by person (Pose subject id) and side,
/// with when it was seen. Every frame's 21 points are in a separate file,
/// not read here: the annotated video shows them. See docs/math/hands.rst.
class HandsResult {
   public:
    struct Hand {
        /// The Pose subject id, or -1: a hand no person's wrist was near, or
        /// on a person Pose did not track.
        int subjectId = -1;
        /// "left" or "right" (the person's own), or "unknown".
        QString side;
        double seenMs{};
        double medianSizePx{};
        double meanScore{};
        QVector<QPair<double, double>> seenSpans; ///< session elapsed ms

        /// "S3 left hand", or "? hand".
        [[nodiscard]] QString display_name() const {
            const QString who = subjectId < 0 ? QStringLiteral("?") : QString("S%1").arg(subjectId);
            return side == QLatin1String("unknown") ? who + " hand"
                                                    : QString("%1 %2 hand").arg(who, side);
        }
    };

    [[nodiscard]] static HandsResult load(const QString& path);

    [[nodiscard]] bool is_valid() const { return valid_; }
    /// "rtmw" or "mediapipe".
    [[nodiscard]] QString model() const { return model_; }
    [[nodiscard]] int stride() const { return stride_; }
    [[nodiscard]] double video_start_ms() const { return videoStartMs_; }
    [[nodiscard]] QString annotated_video() const { return annotatedVideo_; }
    [[nodiscard]] const QVector<Hand>& hands() const { return hands_; }

   private:
    bool valid_ = false;
    QString model_;
    int stride_ = 1;
    double videoStartMs_{};
    QString annotatedVideo_;
    QVector<Hand> hands_;
};

} // namespace mosaic
