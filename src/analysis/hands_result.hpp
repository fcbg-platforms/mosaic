#pragma once
#include <QMap>
#include <QPair>
#include <QString>
#include <QVector>

namespace mosaic {

/// @brief What analysis/run_hands.py found in one camera's video
/// (``<session>/hands/video_N.hands.json``, schema mosaic-hands-v1): the
/// model used, and each hand found, by person (Pose subject id) and side,
/// with when it was seen and its kinematics. Every frame's 21 points are in
/// a separate file, not read here: the annotated video shows them. The
/// kinematics per frame are in a CSV, read by load_kinematics(). Also reads
/// the hands two or more cameras saw, in the room (``hands/hands3d.json``,
/// schema mosaic-hands3d-v1): is_3d(), each hand with its number and the
/// summaries in millimetres too, and how well each camera agreed. See
/// docs/math/hands.rst.
class HandsResult {
   public:
    /// A hand's kinematics over the recording; speeds in hand lengths per
    /// second, aperture and opening in palm lengths. NaN where not measured.
    struct Kinematics {
        bool valid = false;
        double measuredS{};
        double wristPathHl{};
        double wristSpeedMedian{};
        double wristSpeedP95{};
        double gripApertureMedian{};
        double gripApertureP95{};
        double handOpeningMedian{};
        // 3D only (NaN otherwise): in millimetres.
        double palmMm{};
        double wristSpeedMedianMm{}; ///< mm/s
        double wristSpeedP95Mm{};    ///< mm/s
        double gripApertureMedianMm{};
    };

    /// A hand's movements (reaches, grasps, any move) in a few numbers:
    /// medians over its movements; NaN where none had the value.
    struct Movements {
        bool valid = false;
        int count  = 0;
        int cutOff = 0; ///< left out: the measurement starts or ends during them
        double durationS{};
        double peakSpeed{};       ///< hand lengths per second
        double timeToPeakPct{};   ///< of the movement
        double movementUnits{};   ///< speed peaks per movement
        double sparc{};           ///< smoothness: closer to 0 is smoother
        double straightness{};    ///< straight-line distance over path
        double maxGripAperture{}; ///< palm lengths
    };

    /// One movement (a row of ``video_N.hands.movements.csv``).
    struct Movement {
        int hand      = -1; ///< 3D: the hand's number
        int subjectId = -1;
        QString side;
        double startMs{};
        double endMs{};
        double peakSpeed{};
    };

    /// How a person used their two hands (both measured).
    struct Person {
        int subjectId = -1;
        /// (right - left) / (right + left) movements: -1 all left, 1 all right.
        double lateralityIndex{};
        double bothMovingS{};
        /// The share of the time either hand moved that both did.
        double bothMovingShare{};
    };

    /// One hand's kinematics per analysed frame (the CSV's rows for it):
    /// each column by name, NaN where the CSV has no value.
    struct Series {
        int hand      = -1; ///< 3D: the hand's number
        int subjectId = -1;
        QString side;
        QVector<double> timeMs;
        QMap<QString, QVector<double>> columns;
    };

    /// How well a pair of cameras agreed on the hands (3D), in the room: how
    /// far their lines of sight through the same hand points miss each other
    /// (median, mm), the steady part of that (both cameras' room calibration
    /// together: a pair cannot say which is off) and the scatter around it
    /// (the hand model); how far apart in time their frames were (a moving
    /// hand adds to the gap); and the steady part from frames taken at once,
    /// NaN when too few were.
    struct Agreement {
        int cameraA = -1;
        int cameraB = -1;
        int hands   = 0;
        double gapMm{};
        double offsetMm{};
        double scatterMm{};
        double frameGapMs{};
        double offsetSameMomentMm{};
    };

    struct Hand {
        /// The Pose subject id, or -1: a hand no person's wrist was near, or
        /// on a person Pose did not track. In 3D, on the reference camera.
        int subjectId = -1;
        int number    = -1; ///< 3D: the hand's number
        /// "left" or "right" (the person's own), or "unknown".
        QString side;
        double seenMs{};
        double medianSizePx{};
        double meanScore{};
        QVector<QPair<double, double>> seenSpans; ///< session elapsed ms
        Kinematics kinematics;
        Movements movements;

        /// "S3 left hand", "? hand", or in 3D "S3 left hand 2".
        [[nodiscard]] QString display_name() const {
            const QString who = subjectId < 0 ? QStringLiteral("?") : QString("S%1").arg(subjectId);
            const QString name = side == QLatin1String("unknown")
                                     ? who + " hand"
                                     : QString("%1 %2 hand").arg(who, side);
            return number < 0 ? name : QString("%1 %2").arg(name).arg(number);
        }
    };

    [[nodiscard]] static HandsResult load(const QString& path);

    /// The kinematics CSV (``video_N.hands.kinematics.csv``), one Series per
    /// hand in the order they first appear; empty when it cannot be read.
    [[nodiscard]] static QVector<Series> load_kinematics(const QString& path);

    /// The movements CSV (``video_N.hands.movements.csv``), in its order;
    /// empty when it cannot be read.
    [[nodiscard]] static QVector<Movement> load_movements(const QString& path);

    [[nodiscard]] bool is_valid() const { return valid_; }
    /// The hands in the room (hands3d.json), in millimetres.
    [[nodiscard]] bool is_3d() const { return is3d_; }
    [[nodiscard]] const QVector<Agreement>& agreement() const { return agreement_; }
    /// "rtmw" or "mediapipe".
    [[nodiscard]] QString model() const { return model_; }
    [[nodiscard]] int stride() const { return stride_; }
    /// The video's frame rate.
    [[nodiscard]] double fps() const { return fps_; }
    [[nodiscard]] double video_start_ms() const { return videoStartMs_; }
    [[nodiscard]] QString annotated_video() const { return annotatedVideo_; }
    /// Session-relative path of the kinematics CSV; empty for a result from
    /// before kinematics.
    [[nodiscard]] QString kinematics_file() const { return kinematicsFile_; }
    /// Session-relative path of the movements CSV; empty before movements.
    [[nodiscard]] QString movements_file() const { return movementsFile_; }
    [[nodiscard]] const QVector<Person>& people() const { return people_; }
    [[nodiscard]] const QVector<Hand>& hands() const { return hands_; }

   private:
    bool valid_ = false;
    bool is3d_  = false;
    QVector<Agreement> agreement_;
    QString model_;
    int stride_ = 1;
    double fps_{};
    double videoStartMs_{};
    QString annotatedVideo_;
    QString kinematicsFile_;
    QString movementsFile_;
    QVector<Person> people_;
    QVector<Hand> hands_;
};

} // namespace mosaic
