#pragma once
#include <QMap>
#include <QString>
#include <QVector>
#include <array>

namespace mosaic {

/// @brief What analysis/run_avatar.py made (``<session>/avatar/avatar.json``,
/// schema mosaic-avatar-v1): a realistic body (Anny) fitted to each 3D Pose
/// person, its shape, how well it fits, and where its file is. The file
/// (mesh, skinning, every tick's bone transforms) is described here but read
/// by the 3D view; its layout is in analysis/avatar/io.py. See
/// docs/math/avatar.rst.
class AvatarResult {
   public:
    struct Person {
        int personId = -1;
        QString file; ///< session-relative
        int vertices         = 0;
        int faces            = 0;
        int weightsPerVertex = 0;
        int ticks            = 0;
        int boneCount        = 0;
        qint64 bytes         = 0;
        double seconds{};
        /// Anny's shape parameters, each 0 to 1 (gender, age, muscle,
        /// weight, height, proportions).
        QMap<QString, double> phenotype;
        /// How far the body's keypoints are from 3D Pose's (mm): the median
        /// and the 90th percentile, over all joints and per joint; NaN where
        /// not measured.
        double medianErrorMm{};
        double p90ErrorMm{};
        QMap<QString, double> perJointErrorMm;
        QMap<QString, double> perJointP90ErrorMm;
    };

    [[nodiscard]] static AvatarResult load(const QString& path);

    [[nodiscard]] bool is_valid() const { return valid_; }
    /// From the fit's frame (metres, z up) to the room's (mm), row-major:
    /// room = to_room * p.
    [[nodiscard]] const std::array<double, 9>& to_room() const { return toRoom_; }
    [[nodiscard]] const QVector<Person>& people() const { return people_; }
    /// The 3D Pose people that could not be fitted (too few joints, or an error).
    [[nodiscard]] const QVector<int>& failed() const { return failed_; }

   private:
    bool valid_ = false;
    std::array<double, 9> toRoom_{};
    QVector<Person> people_;
    QVector<int> failed_;
};

} // namespace mosaic
