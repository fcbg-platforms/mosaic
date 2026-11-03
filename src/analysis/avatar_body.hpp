#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <cstdint>
#include <vector>

#include "analysis/avatar_result.hpp"

namespace mosaic {

/// @brief One person's 3D Avatar body (``avatar/person_N.bin``, layout in
/// analysis/avatar/io.py): the mesh at rest, its skinning and every tick's
/// bone transforms, posed for drawing by linear blend skinning. The 3D Pose
/// room view draws it; this class does the maths, so it is tested on its
/// own. See docs/math/avatar.rst.
class AvatarBody {
   public:
    /// A body part whose pose 3D Pose may not have measured at a tick: the
    /// lower legs and feet (knee and ankle), the forearms and hands (elbow
    /// and wrist). The rest of the body counts as measured.
    enum class Region : uint8_t { Body, LeftLowerLeg, RightLowerLeg, LeftForearm, RightForearm };

    /// Reads the file `path` described by `person` (avatar.json's counts);
    /// `boneLabels` (avatar.json's, in the file's bone order) say which bone
    /// is which limb. Invalid when the file is missing or its size is not
    /// the layout's.
    [[nodiscard]] static AvatarBody load(const QString& path, const AvatarResult::Person& person,
                                         const QStringList& boneLabels);

    [[nodiscard]] bool is_valid() const { return valid_; }
    [[nodiscard]] int vertex_count() const { return static_cast<int>(rest_.size() / 3); }
    [[nodiscard]] int tick_count() const { return static_cast<int>(timeMs_.size()); }
    [[nodiscard]] const std::vector<uint32_t>& faces() const { return faces_; }
    [[nodiscard]] const std::vector<Region>& regions() const { return regions_; }

    /// The tick nearest `timeMs` (session time), or -1 when the nearest is
    /// further than `maxGapMs` (the person is not in 3D then).
    [[nodiscard]] int nearest_tick(double timeMs, double maxGapMs = 100.0) const;

    /// Whether 3D Pose measured that region's joints at the tick.
    [[nodiscard]] bool measured(int tick, Region region) const;

    /// The body at `tick`, its vertices taken through `toScene` (row-major
    /// 3 x 4: the fit's frame, metres, to the drawing's): positions and unit
    /// normals, 3 floats per vertex each.
    void pose(int tick, const std::array<double, 12>& toScene, std::vector<float>& positions,
              std::vector<float>& normals) const;

   private:
    bool valid_ = false;
    int bones_  = 0;
    int k_      = 0;
    std::vector<float> rest_;     // V x 3
    std::vector<uint32_t> faces_; // F x 3
    std::vector<uint16_t> skinBones_;
    std::vector<float> skinWeights_;
    std::vector<double> timeMs_;
    std::vector<uint32_t> validBits_;
    std::vector<float> transforms_; // N x J x 7: x y z w, tx ty tz
    std::vector<Region> regions_;   // V
};

/// A session's 3D Avatar result and its people's bodies, for the 3D views.
struct SessionAvatars {
    AvatarResult result;
    QVector<AvatarBody> bodies; ///< bodies[i] is result.people()[i]'s
    /// Why there are none to show though a result exists (it is older than
    /// the 3D Pose result it was fitted to); empty otherwise.
    QString note;
};

/// Reads ``<session>/avatar/avatar.json`` and each person's file. Not when
/// ``skeleton3d.json`` is newer: the bodies were fitted to an earlier 3D
/// Pose result, whose ticks and people may no longer match (note says so).
[[nodiscard]] SessionAvatars load_session_avatars(const QString& sessionPath);

} // namespace mosaic
