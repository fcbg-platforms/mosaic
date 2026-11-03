#include "analysis/avatar_body.hpp"

#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mosaic {

namespace {

// COCO keypoints (3D Pose's order) of the joints at each region's ends.
constexpr int kLeftElbow = 7, kRightElbow = 8, kLeftWrist = 9, kRightWrist = 10;
constexpr int kLeftKnee = 13, kRightKnee = 14, kLeftAnkle = 15, kRightAnkle = 16;

AvatarBody::Region region_of_bone(const QString& name) {
    using R          = AvatarBody::Region;
    const bool left  = name.endsWith(".L");
    const bool right = name.endsWith(".R");
    const bool lowLeg =
        name.startsWith("lowerleg") || name.startsWith("foot") || name.startsWith("toe");
    const bool forearm = name.startsWith("lowerarm") || name.startsWith("wrist") ||
                         name.startsWith("finger") || name.startsWith("metacarpal");
    if (lowLeg && left) {
        return R::LeftLowerLeg;
    }
    if (lowLeg && right) {
        return R::RightLowerLeg;
    }
    if (forearm && left) {
        return R::LeftForearm;
    }
    if (forearm && right) {
        return R::RightForearm;
    }
    return R::Body;
}

template <typename T>
bool take(const QByteArray& data, qsizetype& pos, std::vector<T>& out, qsizetype count) {
    // Checked by division: a count from a damaged avatar.json must not
    // overflow the multiplication.
    if (count < 0 || count > (data.size() - pos) / static_cast<qsizetype>(sizeof(T))) {
        return false;
    }
    const qsizetype bytes = count * static_cast<qsizetype>(sizeof(T));
    out.resize(static_cast<std::size_t>(count));
    std::memcpy(out.data(), data.constData() + pos, static_cast<std::size_t>(bytes));
    pos += bytes;
    return true;
}

} // namespace

AvatarBody AvatarBody::load(const QString& path, const AvatarResult::Person& person,
                            const QStringList& boneLabels) {
    AvatarBody b;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return b;
    }
    const QByteArray data = f.readAll();
    const qsizetype v = person.vertices, fc = person.faces, k = person.weightsPerVertex;
    const qsizetype n = person.ticks, j = person.boneCount;
    // Each count bounded by the file's size before any product of them.
    const qsizetype most = data.size();
    if (v <= 0 || fc < 0 || k <= 0 || n < 0 || j <= 0 || v > most || fc > most || k > most ||
        n > most || j > most) {
        return b;
    }
    qsizetype pos = 0;
    // The layout of analysis/avatar/io.py, in its order.
    const bool ok = take(data, pos, b.rest_, v * 3) && take(data, pos, b.faces_, fc * 3) &&
                    take(data, pos, b.skinBones_, v * k) &&
                    take(data, pos, b.skinWeights_, v * k) && take(data, pos, b.timeMs_, n) &&
                    take(data, pos, b.validBits_, n) && take(data, pos, b.transforms_, n * j * 7);
    if (!ok || pos != data.size()) {
        return AvatarBody();
    }
    for (const auto bone : b.skinBones_) {
        if (bone >= j) {
            return AvatarBody();
        }
    }
    for (const auto idx : b.faces_) {
        if (idx >= static_cast<uint32_t>(v)) {
            return AvatarBody();
        }
    }
    b.bones_ = static_cast<int>(j);
    b.k_     = static_cast<int>(k);
    // Each vertex belongs to the region of the bone that moves it most.
    b.regions_.assign(static_cast<std::size_t>(v), Region::Body);
    for (qsizetype i = 0; i < v; ++i) {
        int best = 0;
        for (int c = 1; c < k; ++c) {
            if (b.skinWeights_[i * k + c] > b.skinWeights_[i * k + best]) {
                best = c;
            }
        }
        const int bone = b.skinBones_[i * k + best];
        if (bone < boneLabels.size()) {
            b.regions_[i] = region_of_bone(boneLabels[bone]);
        }
    }
    b.valid_ = true;
    return b;
}

int AvatarBody::nearest_tick(double timeMs, double maxGapMs) const {
    if (timeMs_.empty()) {
        return -1;
    }
    const auto it = std::lower_bound(timeMs_.begin(), timeMs_.end(), timeMs);
    auto best     = it == timeMs_.end() ? std::prev(it) : it;
    if (it != timeMs_.begin() && std::abs(*std::prev(it) - timeMs) <= std::abs(*best - timeMs)) {
        best = std::prev(it);
    }
    return std::abs(*best - timeMs) <= maxGapMs ? static_cast<int>(best - timeMs_.begin()) : -1;
}

bool AvatarBody::measured(int tick, Region region) const {
    if (tick < 0 || tick >= tick_count()) {
        return false;
    }
    const uint32_t bits = validBits_[static_cast<std::size_t>(tick)];
    const auto has      = [bits](int a, int b) { return ((bits >> a) & 1U) && ((bits >> b) & 1U); };
    switch (region) {
        case Region::LeftLowerLeg:
            return has(kLeftKnee, kLeftAnkle);
        case Region::RightLowerLeg:
            return has(kRightKnee, kRightAnkle);
        case Region::LeftForearm:
            return has(kLeftElbow, kLeftWrist);
        case Region::RightForearm:
            return has(kRightElbow, kRightWrist);
        default:
            return true;
    }
}

void AvatarBody::pose(int tick, const std::array<double, 12>& toScene,
                      std::vector<float>& positions, std::vector<float>& normals) const {
    const int v = vertex_count();
    positions.assign(static_cast<std::size_t>(v) * 3, 0.0f);
    normals.assign(static_cast<std::size_t>(v) * 3, 0.0f);
    if (!valid_ || tick < 0 || tick >= tick_count()) {
        return;
    }
    // Each bone's transform, then the frame's: one 3 x 4 matrix per bone
    // taking a vertex at rest straight into the drawing.
    std::vector<double> m(static_cast<std::size_t>(bones_) * 12);
    const float* tr = transforms_.data() + static_cast<std::size_t>(tick) * bones_ * 7;
    for (int b = 0; b < bones_; ++b) {
        const double x = tr[7 * b], y = tr[7 * b + 1], z = tr[7 * b + 2], w = tr[7 * b + 3];
        const double r[9] = {
            1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
        const double t[3] = {tr[7 * b + 4], tr[7 * b + 5], tr[7 * b + 6]};
        double* out       = m.data() + 12 * b;
        for (int row = 0; row < 3; ++row) {
            const double* s = toScene.data() + 4 * row;
            for (int col = 0; col < 3; ++col) {
                out[4 * row + col] = s[0] * r[col] + s[1] * r[3 + col] + s[2] * r[6 + col];
            }
            out[4 * row + 3] = s[0] * t[0] + s[1] * t[1] + s[2] * t[2] + s[3];
        }
    }
    for (int i = 0; i < v; ++i) {
        const float* p = rest_.data() + 3 * i;
        double acc[3]  = {0, 0, 0};
        for (int c = 0; c < k_; ++c) {
            const float wgt = skinWeights_[static_cast<std::size_t>(i) * k_ + c];
            if (wgt <= 0.0f) {
                continue;
            }
            const double* mm = m.data() + 12 * skinBones_[static_cast<std::size_t>(i) * k_ + c];
            for (int row = 0; row < 3; ++row) {
                acc[row] += wgt * (mm[4 * row] * p[0] + mm[4 * row + 1] * p[1] +
                                   mm[4 * row + 2] * p[2] + mm[4 * row + 3]);
            }
        }
        for (int row = 0; row < 3; ++row) {
            positions[3 * i + row] = static_cast<float>(acc[row]);
        }
    }
    // Normals: each vertex the area-weighted sum of its triangles'.
    for (std::size_t f = 0; f + 2 < faces_.size(); f += 3) {
        const float* a   = positions.data() + 3 * faces_[f];
        const float* b   = positions.data() + 3 * faces_[f + 1];
        const float* c   = positions.data() + 3 * faces_[f + 2];
        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2],
                            u[0] * w[1] - u[1] * w[0]};
        for (int corner = 0; corner < 3; ++corner) {
            float* nn = normals.data() + 3 * faces_[f + corner];
            nn[0] += n[0];
            nn[1] += n[1];
            nn[2] += n[2];
        }
    }
    for (int i = 0; i < v; ++i) {
        float* nn       = normals.data() + 3 * i;
        const float len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (len > 0.0f) {
            nn[0] /= len;
            nn[1] /= len;
            nn[2] /= len;
        }
    }
}

QString session_avatars_stamp(const QString& sessionPath) {
    const auto stamp = [](const QString& path) {
        return QFileInfo(path).lastModified().toString(Qt::ISODateWithMs);
    };
    return sessionPath + "|" + stamp(sessionPath + "/avatar/avatar.json") + "|" +
           stamp(sessionPath + "/skeleton3d.json");
}

SessionAvatars load_session_avatars(const QString& sessionPath) {
    SessionAvatars out;
    const QFileInfo json(sessionPath + "/avatar/avatar.json");
    if (!json.exists()) {
        return out;
    }
    const QFileInfo pose(sessionPath + "/skeleton3d.json");
    if (pose.exists() && pose.lastModified() > json.lastModified()) {
        out.note = "The avatars are older than this 3D Pose result: run 3D Avatar again.";
        return out;
    }
    out.result = AvatarResult::load(json.filePath());
    if (out.result.is_valid() && out.result.bone_labels().isEmpty()) {
        out.note = "Run 3D Avatar again to see which limbs were estimated.";
    }
    for (const auto& p : out.result.people()) {
        out.bodies << AvatarBody::load(sessionPath + "/" + p.file, p, out.result.bone_labels());
    }
    return out;
}

} // namespace mosaic
