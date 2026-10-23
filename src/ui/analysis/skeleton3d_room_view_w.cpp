#include "ui/analysis/skeleton3d_room_view_w.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QPainterPath>
#include <QQuaternion>
#include <QScrollArea>
#include <QSet>
#include <QSurfaceFormat>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector3D>
#include <QVector4D>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

#include "analysis/room_view_math.hpp"
#include "ui/analysis/subject_colors.hpp"
#include "video/camera_label.hpp"

namespace mosaic {

namespace {

const QColor kCameraColors[] = {
    QColor("#44cc44"), QColor("#4488ff"), QColor("#ffaa44"),
    QColor("#ff4488"), QColor("#44cccc"), QColor("#cc44cc"),
};

enum Preset { kView3D = 0, kViewTop = 1, kViewFront = 2, kViewSide = 3 };

// Sizes in millimetres, about a person's: thin enough to keep the pose
// readable, thick enough to shade.
constexpr float kLimbRadiusMm  = 24.0f;
constexpr float kJointRadiusMm = 34.0f;
constexpr float kHeadRadiusMm  = 95.0f;
constexpr float kShadowRadius  = 300.0f;
// How far back a trail reaches, and the grid's spacing.
constexpr double kTrailSeconds = 2.0;
constexpr float kGridStepMm    = 500.0f;
// A joint this close to the mouse (px) is the one hovered.
constexpr double kHoverRadiusPx    = 14.0;
constexpr float kPerspectiveFovDeg = 45.0f;

QVector3D qv(const room_view::Vec3& p) {
    return QVector3D(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]));
}

const Skeleton3DVec3& drawn_position(const Skeleton3DKeypoint& kp, bool smoothed) {
    return smoothed ? kp.positionRoomSmoothed : kp.positionRoom;
}

// Lit meshes: a soft key light, an ambient floor and a rim towards the
// viewer, so limbs read as round. GLSL without a version line runs on any
// OpenGL 2.1 context, software OpenGL included.
const char* kLitVertex   = R"(
attribute vec3 pos;
attribute vec3 nrm;
uniform mat4 mvp;
uniform mat3 nmat;
varying vec3 vN;
void main() {
    vN = normalize(nmat * nrm);
    gl_Position = mvp * vec4(pos, 1.0);
}
)";
const char* kLitFragment = R"(
uniform vec4 color;
uniform vec3 lightDir;
uniform vec3 toEye;
varying vec3 vN;
void main() {
    vec3 n = normalize(vN);
    float diff = max(dot(n, lightDir), 0.0);
    float rim = pow(1.0 - max(dot(n, toEye), 0.0), 2.0);
    vec3 c = color.rgb * (0.38 + 0.62 * diff) + vec3(0.22) * rim;
    gl_FragColor = vec4(c, color.a);
}
)";
// Unlit, per-vertex colour: the floor, grid, shadows, trails and frustums.
const char* kFlatVertex   = R"(
attribute vec3 pos;
attribute vec4 col;
uniform mat4 mvp;
varying vec4 vC;
void main() {
    vC = col;
    gl_Position = mvp * vec4(pos, 1.0);
}
)";
const char* kFlatFragment = R"(
varying vec4 vC;
void main() {
    gl_FragColor = vC;
}
)";

// Interleaved position + normal, as triangles.
std::vector<float> unit_sphere(int stacks = 14, int slices = 22) {
    std::vector<float> v;
    auto at = [](double th, double ph) {
        return QVector3D(static_cast<float>(std::sin(th) * std::cos(ph)),
                         static_cast<float>(std::sin(th) * std::sin(ph)),
                         static_cast<float>(std::cos(th)));
    };
    auto put = [&v](const QVector3D& p) {
        v.insert(v.end(), {p.x(), p.y(), p.z(), p.x(), p.y(), p.z()});
    };
    for (int i = 0; i < stacks; ++i) {
        const double t0 = M_PI * i / stacks, t1 = M_PI * (i + 1) / stacks;
        for (int j = 0; j < slices; ++j) {
            const double p0 = 2 * M_PI * j / slices, p1 = 2 * M_PI * (j + 1) / slices;
            put(at(t0, p0));
            put(at(t1, p0));
            put(at(t1, p1));
            put(at(t0, p0));
            put(at(t1, p1));
            put(at(t0, p1));
        }
    }
    return v;
}

// Radius 1, from z = 0 to z = 1, open ends (a joint covers each).
std::vector<float> unit_cylinder(int slices = 22) {
    std::vector<float> v;
    for (int j = 0; j < slices; ++j) {
        const float a0 = static_cast<float>(2 * M_PI * j / slices);
        const float a1 = static_cast<float>(2 * M_PI * (j + 1) / slices);
        const float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        const float quad[6][3] = {{c0, s0, 0}, {c1, s1, 0}, {c1, s1, 1},
                                  {c0, s0, 0}, {c1, s1, 1}, {c0, s0, 1}};
        for (const auto& q : quad) {
            v.insert(v.end(), {q[0], q[1], q[2], q[0], q[1], 0.0f});
        }
    }
    return v;
}

// Flat vertices: x y z r g b a.
void put_vertex(std::vector<float>& v, const QVector3D& p, const QColor& c) {
    v.insert(v.end(),
             {p.x(), p.y(), p.z(), static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
              static_cast<float>(c.blueF()), static_cast<float>(c.alphaF())});
}

// Column j of a row-major 3x3 rotation: the camera's x, y or z axis in the room.
QVector3D column(const std::array<double, 9>& m, int j) {
    return QVector3D(static_cast<float>(m[j]), static_cast<float>(m[3 + j]),
                     static_cast<float>(m[6 + j]));
}

QColor with_alpha(QColor c, double a) {
    c.setAlphaF(static_cast<float>(std::clamp(a, 0.0, 1.0)));
    return c;
}

// The text over the 3D scene, painted as an ordinary widget on top of it:
// QPainter into the OpenGL framebuffer itself proved unreliable.
class OverlayW : public QWidget {
   public:
    OverlayW(QWidget* parent, std::function<void(QPainter&)> paint)
        : QWidget(parent), paint_(std::move(paint)) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

   protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        paint_(p);
    }

   private:
    std::function<void(QPainter&)> paint_;
};

QString keypoint_label(const QString& name) {
    QString s = name;
    s.replace('_', ' ');
    return s;
}

} // namespace

struct Skeleton3DRoomViewW::Impl {
    Skeleton3DResult result;
    int64_t positionMs = 0;
    bool showSmoothed  = false;
    bool showTrails    = true;

    int idxLHip = -1;
    int idxRHip = -1;
    QVector<int> headIdx; // nose, eyes, ears: drawn as one head
    int dyadTrackA = -1;
    int dyadTrackB = -1;
    // The People panel: who is hidden, and whom the view follows (-1: nobody).
    QSet<int> hidden;
    int follow = -1;

    // The room's orientation for drawing (room_view_math.hpp): up, a forward
    // along the floor, and right = forward x up.
    QVector3D U{0, -1, 0}, F{0, 0, 1}, R{1, 0, 0};
    float floorH = 0.0f; // dot(point, U) of the floor
    QVector3D home{0, 0, 0};
    // The camera whose own frame is the room's (identity transform), named
    // in the info line so x, y and z can be read.
    QString frameCamera = "the reference camera";
    float extent        = 2000.0f; // radius of what there is to see, mm

    int preset        = kView3D;
    double yawDeg     = -35.0;
    double pitchDeg   = 28.0;
    double distanceMm = 5000.0;
    float orthoHalfH  = 2000.0f;
    QVector3D target{0, 0, 0};

    Qt::MouseButton dragButton = Qt::NoButton;
    QPoint lastMousePos;

    struct Hover {
        int trackId;
        QString name;
        Skeleton3DVec3 pos;
        QPointF screen;
    };
    std::optional<Hover> hover;

    // Last frame's matrices, for the overlay and hovering.
    QMatrix4x4 view, viewProj;

    // OpenGL resources.
    bool glReady = false;
    QString glProblem;
    QOpenGLShaderProgram lit, flat;
    QOpenGLBuffer sphereVbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer cylinderVbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer dynamicVbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject vao;
    int sphereCount   = 0;
    int cylinderCount = 0;

    QWidget* bar              = nullptr;
    QWidget* overlay          = nullptr;
    QWidget* peoplePanel      = nullptr; // top left: one tick per person, and Follow
    QScrollArea* peopleScroll = nullptr;
    int peopleListHeight      = 0;
    QButtonGroup* views       = nullptr;

    [[nodiscard]] bool flat_view() const { return preset != kView3D; }
    [[nodiscard]] bool shown(const Skeleton3DPerson& p) const {
        return !hidden.contains(p.trackId);
    }

    [[nodiscard]] QVector3D eye_direction() const {
        if (preset == kViewTop) {
            return U;
        }
        const double y = qDegreesToRadians(yawDeg), p = qDegreesToRadians(pitchDeg);
        return (F * static_cast<float>(-std::cos(y)) + R * static_cast<float>(std::sin(y))) *
                   static_cast<float>(std::cos(p)) +
               U * static_cast<float>(std::sin(p));
    }

    [[nodiscard]] QVector3D eye() const {
        return target + eye_direction() * static_cast<float>(distanceMm);
    }

    // The view and projection for a w x h widget. The scene and the text over
    // it both call this, so they always agree.
    void update_matrices(int w, int h) {
        const float aspect = static_cast<float>(w) / static_cast<float>(std::max(h, 1));
        view.setToIdentity();
        view.lookAt(eye(), target, preset == kViewTop ? F : U);
        QMatrix4x4 proj;
        const float far = static_cast<float>(distanceMm) + 4.0f * extent + 20000.0f;
        if (flat_view()) {
            // Depth both ways from the eye: nothing is cut away however far
            // the view is panned or zoomed.
            proj.ortho(-orthoHalfH * aspect, orthoHalfH * aspect, -orthoHalfH, orthoHalfH, -far,
                       far);
        } else {
            proj.perspective(kPerspectiveFovDeg, aspect, 20.0f, far);
        }
        viewProj = proj * view;
    }

    [[nodiscard]] const Skeleton3DFrame* current_frame() const {
        if (result.frames().isEmpty()) {
            return nullptr;
        }
        const int64_t ts = result.frames().first().timestampNs + positionMs * 1000000LL;
        return result.nearest_frame(ts);
    }

    [[nodiscard]] std::optional<QVector3D> hip_middle(const Skeleton3DPerson& p,
                                                      bool smoothed) const {
        if (idxLHip < 0 || idxRHip < 0 || idxLHip >= p.keypoints.size() ||
            idxRHip >= p.keypoints.size() || !p.keypoints[idxLHip].valid ||
            !p.keypoints[idxRHip].valid) {
            return std::nullopt;
        }
        return (qv(drawn_position(p.keypoints[idxLHip], smoothed)) +
                qv(drawn_position(p.keypoints[idxRHip], smoothed))) *
               0.5f;
    }

    // Where a person is, for their shadow: the hips, else the mean of what
    // was seen.
    [[nodiscard]] std::optional<QVector3D> body_centre(const Skeleton3DPerson& p) const {
        if (const auto h = hip_middle(p, showSmoothed)) {
            return h;
        }
        QVector3D sum;
        int n = 0;
        for (const auto& kp : p.keypoints) {
            if (kp.valid) {
                sum += qv(drawn_position(kp, showSmoothed));
                ++n;
            }
        }
        return n ? std::optional<QVector3D>(sum / static_cast<float>(n)) : std::nullopt;
    }

    [[nodiscard]] QVector3D on_floor(const QVector3D& p) const {
        return p - U * (QVector3D::dotProduct(p, U) - floorH);
    }

    void fit() {
        target     = home;
        distanceMm = std::max(extent * 2.4, 1500.0);
        orthoHalfH = std::max(extent * 1.15f, 800.0f);
    }

    void recompute_room() {
        const auto up = room_view::estimate_up(result);
        const auto fw = room_view::floor_forward(up);
        U             = qv(up);
        F             = qv(fw);
        R             = QVector3D::crossProduct(F, U).normalized();
        floorH        = static_cast<float>(room_view::floor_height(result, up));
        frameCamera   = "the reference camera";
        for (const auto& c : result.cameras()) {
            if (room_view::norm(c.positionRoom) < 1.0 &&
                (!c.hasRotation || std::abs(c.rotationRoom[0] + c.rotationRoom[4] +
                                            c.rotationRoom[8] - 3.0) < 1e-6)) {
                frameCamera = camera_label(c.index);
            }
        }

        // Look at the people (where they were on average, a metre above the
        // floor); without any, at the middle of the cameras.
        QVector3D sum;
        int n = 0;
        std::vector<QVector3D> pts;
        for (const auto& f : result.frames()) {
            for (const auto& p : f.people) {
                for (const auto& kp : p.keypoints) {
                    if (kp.valid) {
                        pts.push_back(qv(kp.positionRoom));
                        sum += pts.back();
                        ++n;
                    }
                }
            }
        }
        if (n == 0) {
            for (const auto& c : result.cameras()) {
                sum += qv(c.positionRoom);
                ++n;
            }
        }
        home = n ? on_floor(sum / static_cast<float>(n)) + U * 1000.0f : QVector3D();
        // Big enough for every camera and most of the people's room.
        float r       = 1500.0f;
        auto consider = [&](const QVector3D& p) {
            const QVector3D flatOff = on_floor(p) - on_floor(home);
            r                       = std::max(r, flatOff.length());
        };
        for (const auto& c : result.cameras()) {
            consider(qv(c.positionRoom));
        }
        std::sort(pts.begin(), pts.end(), [&](const QVector3D& a, const QVector3D& b) {
            return (on_floor(a) - on_floor(home)).lengthSquared() <
                   (on_floor(b) - on_floor(home)).lengthSquared();
        });
        if (!pts.empty()) {
            consider(pts[pts.size() * 95 / 100]); // stray triangulations do not count
        }
        extent = r;
    }
};

Skeleton3DRoomViewW::Skeleton3DRoomViewW(QWidget* parent)
    : QOpenGLWidget(parent), d(std::make_unique<Impl>()) {
    setMinimumSize(240, 180);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true); // hover readout
    QSurfaceFormat fmt = format();
    fmt.setSamples(4); // smooth edges
    fmt.setDepthBufferSize(24);
    setFormat(fmt);

    d->overlay = new OverlayW(this, [this](QPainter& p) { paint_overlay(p); });
    d->bar     = new QWidget(this);
    auto* lay  = new QHBoxLayout(d->bar);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    d->views = new QButtonGroup(this);
    const QString style =
        "QToolButton { background: rgba(20,20,44,200); color: #c0c0e0; border: 1px solid #34345a;"
        " border-radius: 4px; padding: 3px 8px; font-size: 11px; }"
        "QToolButton:checked { background: #3a5fc8; color: white; border-color: #5a7fe8; }"
        "QToolButton:hover { border-color: #7a8ff0; }";
    const struct {
        int id;
        const char* text;
        const char* tip;
    } buttons[] = {
        {kView3D, "3D", "Orbit: drag to turn, right-drag to pan, scroll to zoom."},
        {kViewTop, "Top", "Looking down on the floor, flat (a 2D map). Drag to pan."},
        {kViewFront, "Front",
         "Looking the way the room's reference camera does, flat. Drag to "
         "pan."},
        {kViewSide, "Side", "From the side, flat. Drag to pan."},
    };
    for (const auto& b : buttons) {
        auto* btn = new QToolButton(d->bar);
        btn->setText(b.text);
        btn->setToolTip(b.tip);
        btn->setCheckable(true);
        btn->setChecked(b.id == kView3D);
        btn->setStyleSheet(style);
        btn->setCursor(Qt::PointingHandCursor);
        d->views->addButton(btn, b.id);
        lay->addWidget(btn);
    }
    connect(d->views, &QButtonGroup::idClicked, this, [this](int id) { apply_preset(id); });
    lay->addSpacing(8);
    auto* trails = new QToolButton(d->bar);
    trails->setText("Trails");
    trails->setToolTip("Where each person was over the last 2 seconds.");
    trails->setCheckable(true);
    trails->setChecked(d->showTrails);
    trails->setStyleSheet(style);
    trails->setCursor(Qt::PointingHandCursor);
    connect(trails, &QToolButton::toggled, this, [this](bool on) {
        d->showTrails = on;
        refresh();
    });
    lay->addWidget(trails);
    d->bar->adjustSize();
}

Skeleton3DRoomViewW::~Skeleton3DRoomViewW() {
    // GL resources go with the context they were made in.
    makeCurrent();
    d->sphereVbo.destroy();
    d->cylinderVbo.destroy();
    d->dynamicVbo.destroy();
    d->vao.destroy();
    doneCurrent();
}

void Skeleton3DRoomViewW::set_result(const Skeleton3DResult& result) {
    d->result  = result;
    d->idxLHip = static_cast<int>(result.keypoint_names().indexOf("left_hip"));
    d->idxRHip = static_cast<int>(result.keypoint_names().indexOf("right_hip"));
    d->headIdx.clear();
    for (const char* n : {"nose", "left_eye", "right_eye", "left_ear", "right_ear"}) {
        const int i = static_cast<int>(result.keypoint_names().indexOf(n));
        if (i >= 0) {
            d->headIdx << i;
        }
    }
    d->hover.reset();
    d->hidden.clear();
    d->follow = -1;
    d->recompute_room();
    rebuild_people_panel();
    apply_preset(d->preset);
}

void Skeleton3DRoomViewW::set_position_ms(int64_t positionMs) {
    d->positionMs = positionMs;
    d->hover.reset(); // the joint under the mouse has moved
    follow_person();
    refresh();
}

// Keeps the followed person in the middle of the view, at the height the
// view looks at (so the camera glides along the floor, not up and down).
void Skeleton3DRoomViewW::follow_person() {
    if (d->follow < 0) {
        return;
    }
    if (const auto* frame = d->current_frame()) {
        for (const auto& p : frame->people) {
            if (p.trackId == d->follow) {
                if (const auto c = d->body_centre(p)) {
                    const float h = QVector3D::dotProduct(d->target, d->U);
                    d->target     = d->on_floor(*c) + d->U * (h - d->floorH);
                }
            }
        }
    }
}

// One tick per person (with the colour they are drawn in and how long they
// were in 3D) and a Follow choice, rebuilt for each result.
void Skeleton3DRoomViewW::rebuild_people_panel() {
    delete d->peoplePanel;
    d->peoplePanel = new QWidget(this);
    d->peoplePanel->setObjectName("peoplePanel");
    d->peoplePanel->setStyleSheet(
        "#peoplePanel { background: rgba(14,14,32,215); border: 1px solid #34345a; "
        "border-radius: 6px; } QLabel, QCheckBox { color: #d0d0ec; font-size: 12px; } "
        "QComboBox { font-size: 12px; }");
    auto* lay = new QVBoxLayout(d->peoplePanel);
    lay->setContentsMargins(8, 6, 8, 6);
    lay->setSpacing(2);
    auto* title = new QLabel("<b>People</b>");
    lay->addWidget(title);

    QMap<int, int> ticks;
    for (const auto& f : d->result.frames()) {
        for (const auto& p : f.people) {
            ticks[p.trackId] += 1;
        }
    }
    const auto& frames = d->result.frames();
    const double step =
        frames.size() > 1
            ? static_cast<double>(frames.last().timestampNs - frames.first().timestampNs) / 1e9 /
                  (frames.size() - 1)
            : 0.0;
    // The ticks scroll when there are many (results from before people were
    // joined can have one per track fragment).
    auto* list    = new QWidget;
    auto* listLay = new QVBoxLayout(list);
    listLay->setContentsMargins(0, 0, 0, 0);
    listLay->setSpacing(2);
    auto* scroll = new QScrollArea;
    scroll->setWidget(list);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(
        "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
    auto* follow = new QComboBox;
    follow->addItem("Follow nobody", -1);
    for (auto it = ticks.begin(); it != ticks.end(); ++it) {
        const int id = it.key();
        auto* box =
            new QCheckBox(QString("Person %1  ·  %2 s").arg(id).arg(it.value() * step, 0, 'f', 1));
        box->setChecked(!d->hidden.contains(id));
        const QColor c = subject_color(id - 1);
        box->setStyleSheet(QString("QCheckBox { color: %1; font-weight: 600; }").arg(c.name()));
        box->setToolTip("Show or hide this person in the room view.");
        connect(box, &QCheckBox::toggled, this, [this, id](bool on) {
            if (on) {
                d->hidden.remove(id);
            } else {
                d->hidden.insert(id);
            }
            refresh();
        });
        listLay->addWidget(box);
        follow->addItem(QString("Follow Person %1").arg(id), id);
    }
    if (ticks.isEmpty()) {
        listLay->addWidget(new QLabel("Nobody in 3D"));
    }
    list->adjustSize();
    scroll->setMinimumWidth(list->sizeHint().width() + 16);
    d->peopleListHeight = list->sizeHint().height();
    d->peopleScroll     = scroll;
    lay->addWidget(scroll);
    follow->setToolTip("Keep this person in the middle of the view as the video plays.");
    follow->setCurrentIndex(std::max(0, follow->findData(d->follow)));
    connect(follow, &QComboBox::currentIndexChanged, this, [this, follow](int) {
        d->follow = follow->currentData().toInt();
        follow_person();
        refresh();
    });
    lay->addWidget(follow);
    place_people_panel();
    d->peoplePanel->show();
}

// Top left, at most half the view's height (its list scrolls beyond that),
// and below the view buttons when the view is too narrow for both side by
// side.
void Skeleton3DRoomViewW::place_people_panel() {
    if (!d->peoplePanel || !d->peopleScroll) {
        return;
    }
    d->peopleScroll->setFixedHeight(
        std::min(d->peopleListHeight + 4, std::max(40, height() / 2 - 70)));
    d->peoplePanel->adjustSize();
    const bool clash = 8 + d->peoplePanel->width() + 8 > width() - d->bar->width() - 8;
    d->peoplePanel->move(8, clash ? d->bar->y() + d->bar->height() + 8 : 8);
}

void Skeleton3DRoomViewW::set_show_smoothed(bool showSmoothed) {
    d->showSmoothed = showSmoothed;
    refresh();
}

void Skeleton3DRoomViewW::set_dyad_tracks(int trackIdA, int trackIdB) {
    d->dyadTrackA = trackIdA;
    d->dyadTrackB = trackIdB;
    refresh();
}

void Skeleton3DRoomViewW::apply_preset(int preset) {
    d->preset = preset;
    switch (preset) {
        case kViewFront:
            d->yawDeg   = 0.0;
            d->pitchDeg = 0.0;
            break;
        case kViewSide:
            d->yawDeg   = 90.0;
            d->pitchDeg = 0.0;
            break;
        case kViewTop:
            d->yawDeg   = 0.0;
            d->pitchDeg = 90.0;
            break;
        default:
            d->yawDeg   = -35.0;
            d->pitchDeg = 28.0;
            break;
    }
    d->fit();
    if (auto* b = d->views->button(preset)) {
        b->setChecked(true);
    }
    d->hover.reset();
    refresh();
}

// ── Interaction ──────────────────────────────────────────────────────────

void Skeleton3DRoomViewW::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton ||
        event->button() == Qt::MiddleButton) {
        d->dragButton   = event->button();
        d->lastMousePos = event->pos();
        d->hover.reset();
        refresh();
    }
}

void Skeleton3DRoomViewW::mouseMoveEvent(QMouseEvent* event) {
    if (d->dragButton == Qt::NoButton) {
        // Hover: the nearest joint of the frame on show, if close enough.
        std::optional<Impl::Hover> best;
        double bestD                 = kHoverRadiusPx;
        const Skeleton3DFrame* frame = d->current_frame();
        if (frame && d->glReady) {
            for (const auto& p : frame->people) {
                if (!d->shown(p)) {
                    continue;
                }
                for (int i = 0; i < p.keypoints.size(); ++i) {
                    if (!p.keypoints[i].valid) {
                        continue;
                    }
                    const auto& pos      = drawn_position(p.keypoints[i], d->showSmoothed);
                    const QVector4D clip = d->viewProj * QVector4D(qv(pos), 1.0f);
                    if (clip.w() <= 0.0f) {
                        continue;
                    }
                    const QPointF s((clip.x() / clip.w() * 0.5 + 0.5) * width(),
                                    (0.5 - clip.y() / clip.w() * 0.5) * height());
                    const double dist =
                        std::hypot(s.x() - event->position().x(), s.y() - event->position().y());
                    if (dist < bestD) {
                        bestD              = dist;
                        const QString name = i < d->result.keypoint_names().size()
                                                 ? keypoint_label(d->result.keypoint_names()[i])
                                                 : QString("keypoint %1").arg(i);
                        best               = Impl::Hover{p.trackId, name, pos, s};
                    }
                }
            }
        }
        const bool changed = best.has_value() != d->hover.has_value() ||
                             (best && d->hover && best->screen != d->hover->screen);
        d->hover = best;
        if (changed) {
            refresh();
        }
        return;
    }
    const QPoint delta = event->pos() - d->lastMousePos;
    d->lastMousePos    = event->pos();
    const bool pan     = d->dragButton != Qt::LeftButton || d->flat_view();
    if (pan) {
        // Move the target with the mouse, in the plane facing the viewer.
        const float perPx     = d->flat_view()
                                    ? 2.0f * d->orthoHalfH / std::max(height(), 1)
                                    : 2.0f * static_cast<float>(d->distanceMm) *
                                      std::tan(qDegreesToRadians(kPerspectiveFovDeg / 2)) /
                                      std::max(height(), 1);
        const QVector3D right = d->view.row(0).toVector3D();
        const QVector3D up    = d->view.row(1).toVector3D();
        d->target +=
            (-right * static_cast<float>(delta.x()) + up * static_cast<float>(delta.y())) * perPx;
    } else {
        d->yawDeg += delta.x() * 0.4;
        d->pitchDeg = std::clamp(d->pitchDeg + delta.y() * 0.4, -10.0, 89.0);
    }
    refresh();
}

void Skeleton3DRoomViewW::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == d->dragButton) {
        d->dragButton = Qt::NoButton;
    }
}

void Skeleton3DRoomViewW::wheelEvent(QWheelEvent* event) {
    const double factor = std::pow(0.999, event->angleDelta().y());
    // A flat view zooms by its width alone; moving its eye closer would cut
    // away what lies between.
    if (d->flat_view()) {
        d->orthoHalfH = std::clamp(d->orthoHalfH * static_cast<float>(factor), 150.0f, 50000.0f);
    } else {
        d->distanceMm = std::clamp(d->distanceMm * factor, 300.0, 100000.0);
    }
    d->hover.reset();
    refresh();
}

void Skeleton3DRoomViewW::mouseDoubleClickEvent(QMouseEvent*) { apply_preset(d->preset); }

void Skeleton3DRoomViewW::leaveEvent(QEvent*) {
    if (d->hover) {
        d->hover.reset();
        refresh();
    }
}

// ── OpenGL ───────────────────────────────────────────────────────────────

void Skeleton3DRoomViewW::initializeGL() {
    initializeOpenGLFunctions();
    d->glReady = false;
    // A new context (the widget moved to another window) starts over.
    d->lit.removeAllShaders();
    d->flat.removeAllShaders();
    auto build = [this](QOpenGLShaderProgram& prog, const char* vs, const char* fs,
                        const QStringList& attributes) {
        bool ok = prog.addShaderFromSourceCode(QOpenGLShader::Vertex, vs) &&
                  prog.addShaderFromSourceCode(QOpenGLShader::Fragment, fs);
        for (int i = 0; i < attributes.size(); ++i) {
            prog.bindAttributeLocation(attributes[i], i);
        }
        ok = ok && prog.link();
        if (!ok) {
            d->glProblem = prog.log();
        }
        return ok;
    };
    if (!build(d->lit, kLitVertex, kLitFragment, {"pos", "nrm"}) ||
        !build(d->flat, kFlatVertex, kFlatFragment, {"pos", "col"})) {
        return;
    }
    d->vao.create(); // absent on plain OpenGL 2.1, where none is needed
    auto upload = [](QOpenGLBuffer& vbo, const std::vector<float>& v) {
        vbo.create();
        vbo.bind();
        vbo.allocate(v.data(), static_cast<int>(v.size() * sizeof(float)));
        vbo.release();
        return static_cast<int>(v.size() / 6);
    };
    d->sphereCount   = upload(d->sphereVbo, unit_sphere());
    d->cylinderCount = upload(d->cylinderVbo, unit_cylinder());
    d->dynamicVbo.setUsagePattern(QOpenGLBuffer::StreamDraw);
    d->dynamicVbo.create();
    d->glReady = true;
}

// Placed on every resize, not in resizeGL(), so the buttons and any message
// sit right even when OpenGL never starts.
void Skeleton3DRoomViewW::resizeEvent(QResizeEvent* e) {
    QOpenGLWidget::resizeEvent(e);
    d->overlay->setGeometry(rect());
    d->bar->adjustSize();
    d->bar->move(width() - d->bar->width() - 8, 8);
    place_people_panel();
}

// The scene and its text redraw together. Asking the overlay to repaint from
// inside paintGL() would repaint the scene under it again, without end.
void Skeleton3DRoomViewW::refresh() {
    update();
    d->overlay->update();
}

void Skeleton3DRoomViewW::paintGL() {
    glClearColor(0.035f, 0.035f, 0.075f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!d->glReady || !d->result.is_valid()) {
        return;
    }
    d->vao.bind();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    // Colour blends; the framebuffer's own alpha stays opaque, or Qt shows
    // the widget as see-through where translucent things were drawn.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);

    // ── Camera ──
    d->update_matrices(width(), height());
    const QVector3D eye   = d->eye();
    const QVector3D toEye = (eye - d->target).normalized();
    const QVector3D light = (d->U * 0.85f + d->R * 0.35f - d->F * 0.4f).normalized();

    // ── Drawing helpers ──
    auto draw_flat = [&](GLenum mode, const std::vector<float>& v) {
        if (v.empty()) {
            return;
        }
        d->flat.bind();
        d->flat.setUniformValue("mvp", d->viewProj);
        d->dynamicVbo.bind();
        d->dynamicVbo.allocate(v.data(), static_cast<int>(v.size() * sizeof(float)));
        d->flat.enableAttributeArray(0);
        d->flat.enableAttributeArray(1);
        d->flat.setAttributeBuffer(0, GL_FLOAT, 0, 3, 7 * sizeof(float));
        d->flat.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 4, 7 * sizeof(float));
        glDrawArrays(mode, 0, static_cast<GLsizei>(v.size() / 7));
        d->flat.disableAttributeArray(0);
        d->flat.disableAttributeArray(1);
        d->dynamicVbo.release();
    };
    auto draw_mesh = [&](QOpenGLBuffer& vbo, int count, const QMatrix4x4& model, const QColor& c) {
        d->lit.bind();
        d->lit.setUniformValue("mvp", d->viewProj * model);
        d->lit.setUniformValue("nmat", model.normalMatrix());
        d->lit.setUniformValue("color", c);
        d->lit.setUniformValue("lightDir", light);
        d->lit.setUniformValue("toEye", toEye);
        vbo.bind();
        d->lit.enableAttributeArray(0);
        d->lit.enableAttributeArray(1);
        d->lit.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * sizeof(float));
        d->lit.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
        glDrawArrays(GL_TRIANGLES, 0, count);
        d->lit.disableAttributeArray(0);
        d->lit.disableAttributeArray(1);
        vbo.release();
    };
    auto sphere = [&](const QVector3D& at, float radius, const QColor& c) {
        QMatrix4x4 m;
        m.translate(at);
        m.scale(radius);
        draw_mesh(d->sphereVbo, d->sphereCount, m, c);
    };
    auto limb = [&](const QVector3D& a, const QVector3D& b, float radius, const QColor& c) {
        const QVector3D ab = b - a;
        const float len    = ab.length();
        if (len < 1.0f) {
            return;
        }
        QMatrix4x4 m;
        m.translate(a);
        m.rotate(QQuaternion::rotationTo(QVector3D(0, 0, 1), ab / len));
        m.scale(radius, radius, len);
        draw_mesh(d->cylinderVbo, d->cylinderCount, m, c);
    };
    auto floor_pt = [&](float a, float b, float lift = 0.0f) {
        return d->R * a + d->F * b + d->U * (d->floorH + lift);
    };

    // ── Floor: a dark plate, then a grid every 0.5 m, brighter each metre ──
    const float cr   = QVector3D::dotProduct(d->home, d->R);
    const float cf   = QVector3D::dotProduct(d->home, d->F);
    const float c0r  = std::round(cr / 1000.0f) * 1000.0f;
    const float c0f  = std::round(cf / 1000.0f) * 1000.0f;
    const float half = std::ceil((d->extent + 1000.0f) / 1000.0f) * 1000.0f;
    glDepthMask(GL_FALSE);
    {
        std::vector<float> plate;
        const QColor pc(26, 28, 52, 235);
        const QVector3D p00 = floor_pt(c0r - half, c0f - half, -2.0f);
        const QVector3D p10 = floor_pt(c0r + half, c0f - half, -2.0f);
        const QVector3D p11 = floor_pt(c0r + half, c0f + half, -2.0f);
        const QVector3D p01 = floor_pt(c0r - half, c0f + half, -2.0f);
        for (const auto& p : {p00, p10, p11, p00, p11, p01}) {
            put_vertex(plate, p, pc);
        }
        draw_flat(GL_TRIANGLES, plate);
        std::vector<float> grid;
        for (float g = -half; g <= half + 1.0f; g += kGridStepMm) {
            const bool metre = std::fmod(std::abs(g), 1000.0f) < 1.0f;
            const QColor gc  = metre ? QColor(90, 96, 150, 150) : QColor(70, 74, 120, 70);
            put_vertex(grid, floor_pt(c0r + g, c0f - half), gc);
            put_vertex(grid, floor_pt(c0r + g, c0f + half), gc);
            put_vertex(grid, floor_pt(c0r - half, c0f + g), gc);
            put_vertex(grid, floor_pt(c0r + half, c0f + g), gc);
        }
        draw_flat(GL_LINES, grid);
    }

    const Skeleton3DFrame* frame = d->current_frame();

    // ── Shadows: a soft dark disc on the floor under each person ──
    if (frame) {
        std::vector<float> shadows;
        for (const auto& p : frame->people) {
            const auto c = d->shown(p) ? d->body_centre(p) : std::nullopt;
            if (!c) {
                continue;
            }
            const QVector3D o  = d->on_floor(*c) + d->U * 1.0f;
            const int segments = 28;
            for (int i = 0; i < segments; ++i) {
                const float a0 = static_cast<float>(2 * M_PI * i / segments);
                const float a1 = static_cast<float>(2 * M_PI * (i + 1) / segments);
                put_vertex(shadows, o, QColor(0, 0, 0, 120));
                put_vertex(shadows, o + (d->R * std::cos(a0) + d->F * std::sin(a0)) * kShadowRadius,
                           QColor(0, 0, 0, 0));
                put_vertex(shadows, o + (d->R * std::cos(a1) + d->F * std::sin(a1)) * kShadowRadius,
                           QColor(0, 0, 0, 0));
            }
        }
        draw_flat(GL_TRIANGLES, shadows);
    }

    // ── Trails: where each person was over the last seconds, fading ──
    if (frame && d->showTrails) {
        const auto& frames = d->result.frames();
        const int now      = static_cast<int>(frame - frames.constData());
        const int64_t from = frame->timestampNs - static_cast<int64_t>(kTrailSeconds * 1e9);
        std::vector<float> trails;
        for (const auto& p : frame->people) {
            if (!d->shown(p)) {
                continue;
            }
            // The hips only: a centre of whatever happens to be seen jumps
            // about as keypoints come and go, and tangles the trail.
            std::optional<QVector3D> next = d->hip_middle(p, d->showSmoothed);
            double nextAge                = 0.0;
            for (int k = now - 1; k >= 0 && frames[k].timestampNs >= from; --k) {
                std::optional<QVector3D> here;
                for (const auto& q : frames[k].people) {
                    if (q.trackId == p.trackId) {
                        here = d->hip_middle(q, d->showSmoothed);
                    }
                }
                const double age = (frame->timestampNs - frames[k].timestampNs) / 1e9;
                // Never across a gap: a person joined from two tracks
                // (analysis/pose3d/people.py) can reappear metres away.
                if (here && next && age - nextAge <= 0.25) {
                    const QColor c = subject_color(p.trackId - 1);
                    put_vertex(trails, *next, with_alpha(c, 0.9 * (1.0 - nextAge / kTrailSeconds)));
                    put_vertex(trails, *here, with_alpha(c, 0.9 * (1.0 - age / kTrailSeconds)));
                    // Its trace on the floor too, fainter: the path walked.
                    put_vertex(trails, d->on_floor(*next) + d->U * 2.0f,
                               with_alpha(c, 0.45 * (1.0 - nextAge / kTrailSeconds)));
                    put_vertex(trails, d->on_floor(*here) + d->U * 2.0f,
                               with_alpha(c, 0.45 * (1.0 - age / kTrailSeconds)));
                }
                if (here) {
                    next    = here;
                    nextAge = age;
                }
            }
        }
        glLineWidth(2.0f);
        draw_flat(GL_LINES, trails);
        glLineWidth(1.0f);
    }

    // ── Cameras: a body and what each sees (a pyramid 0.45 m deep) ──
    {
        std::vector<float> frusta, faces;
        for (const auto& cam : d->result.cameras()) {
            const QColor col  = kCameraColors[((cam.index % 6) + 6) % 6];
            const QVector3D o = qv(cam.positionRoom);
            if (!cam.hasRotation) {
                continue;
            }
            const QVector3D x = column(cam.rotationRoom, 0);
            const QVector3D y = column(cam.rotationRoom, 1);
            const QVector3D z = column(cam.rotationRoom, 2);
            const float depth = 450.0f;
            const float tx =
                depth * static_cast<float>(std::tan(qDegreesToRadians(cam.fovDeg[0] / 2.0)));
            const float ty =
                depth * static_cast<float>(std::tan(qDegreesToRadians(cam.fovDeg[1] / 2.0)));
            const QVector3D c[4] = {
                o + z * depth - x * tx - y * ty, o + z * depth + x * tx - y * ty,
                o + z * depth + x * tx + y * ty, o + z * depth - x * tx + y * ty};
            for (int i = 0; i < 4; ++i) {
                put_vertex(frusta, o, with_alpha(col, 0.85));
                put_vertex(frusta, c[i], with_alpha(col, 0.85));
                put_vertex(frusta, c[i], with_alpha(col, 0.85));
                put_vertex(frusta, c[(i + 1) % 4], with_alpha(col, 0.85));
            }
            for (const int i : {0, 1, 2, 0, 2, 3}) {
                put_vertex(faces, c[i], with_alpha(col, 0.14));
            }
        }
        draw_flat(GL_TRIANGLES, faces);
        draw_flat(GL_LINES, frusta);
    }
    glDepthMask(GL_TRUE);
    for (const auto& cam : d->result.cameras()) {
        sphere(qv(cam.positionRoom), 45.0f, kCameraColors[((cam.index % 6) + 6) % 6]);
    }

    // ── People ──
    if (frame) {
        const auto& edges = d->result.skeleton_edges();
        for (const auto& p : frame->people) {
            if (!d->shown(p)) {
                continue;
            }
            const QColor base  = subject_color(p.trackId - 1);
            const QColor limbC = base.darker(115);
            const QColor joint = base.lighter(125);
            auto at = [&](int i) { return qv(drawn_position(p.keypoints[i], d->showSmoothed)); };
            auto is_head = [&](int i) { return d->headIdx.contains(i); };
            for (const auto& e : edges) {
                const int a = e.first, b = e.second;
                if (a < 0 || b < 0 || a >= p.keypoints.size() || b >= p.keypoints.size() ||
                    !p.keypoints[a].valid || !p.keypoints[b].valid || (is_head(a) && is_head(b))) {
                    continue;
                }
                limb(at(a), at(b), kLimbRadiusMm, limbC);
            }
            QVector3D headSum;
            int headN = 0;
            for (int i = 0; i < p.keypoints.size(); ++i) {
                if (!p.keypoints[i].valid) {
                    continue;
                }
                if (is_head(i)) {
                    headSum += at(i);
                    ++headN;
                    continue;
                }
                sphere(at(i), kJointRadiusMm, joint);
            }
            // The face's points as one head, a little behind them (the
            // keypoints sit on the face's surface).
            if (headN) {
                sphere(headSum / static_cast<float>(headN), kHeadRadiusMm, with_alpha(joint, 0.92));
            }
        }
    }

    d->vao.release();
}

// ── Text over the scene ──────────────────────────────────────────────────

void Skeleton3DRoomViewW::paint_overlay(QPainter& p) {
    d->update_matrices(width(), height());
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    if (!d->glReady && !d->glProblem.isEmpty()) {
        p.fillRect(rect(), QColor("#0a0a1a"));
        p.setPen(QColor("#c08080"));
        p.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
                   "The 3D view needs OpenGL 2.1, which this PC's graphics driver did not "
                   "provide.\n\n" +
                       d->glProblem.left(300));
        return;
    }
    if (!d->result.is_valid()) {
        p.setPen(QColor("#5a5a80"));
        p.drawText(rect(), Qt::AlignCenter,
                   "No 3D pose result loaded.\nRun 3D Pose Reconstruction for this session.");
        return;
    }
    const auto project = [&](const QVector3D& w) -> std::optional<QPointF> {
        const QVector4D clip = d->viewProj * QVector4D(w, 1.0f);
        if (clip.w() <= 0.01f) {
            return std::nullopt;
        }
        return QPointF((clip.x() / clip.w() * 0.5 + 0.5) * width(),
                       (0.5 - clip.y() / clip.w() * 0.5) * height());
    };
    auto pill = [&p](const QPointF& at, const QString& text, const QColor& fg, bool centred) {
        const QFontMetrics fm(p.font());
        QRectF r = fm.boundingRect(text).adjusted(-7, -3, 7, 3);
        if (centred) {
            r.moveCenter(at);
        } else {
            r.moveBottomLeft(at);
        }
        p.setPen(QPen(fg, 1.5));
        p.setBrush(QColor(10, 10, 26, 220));
        p.drawRoundedRect(r, 5, 5);
        p.setPen(fg);
        p.drawText(r, Qt::AlignCenter, text);
    };

    // Camera names: bold, and larger in a larger view (12 to 18 px).
    QFont label = p.font();
    label.setPixelSize(std::clamp(height() / 38, 12, 18));
    label.setBold(true);
    p.setFont(label);
    for (const auto& cam : d->result.cameras()) {
        if (const auto s = project(qv(cam.positionRoom))) {
            pill(*s + QPointF(10, -8), camera_label(cam.index),
                 kCameraColors[((cam.index % 6) + 6) % 6], false);
        }
    }

    // People: a name above each head; the dyad line and its distance.
    const Skeleton3DFrame* frame = d->current_frame();
    if (frame) {
        for (const auto& person : frame->people) {
            if (!d->shown(person)) {
                continue;
            }
            QVector3D top;
            float best = -1e30f;
            for (const auto& kp : person.keypoints) {
                if (!kp.valid) {
                    continue;
                }
                const QVector3D w = qv(drawn_position(kp, d->showSmoothed));
                const float h     = QVector3D::dotProduct(w, d->U);
                if (h > best) {
                    best = h;
                    top  = w;
                }
            }
            if (best > -1e29f) {
                if (const auto s = project(top + d->U * 200.0f)) {
                    pill(*s, QString("Person %1").arg(person.trackId),
                         subject_color(person.trackId - 1), true);
                }
            }
        }
        if (d->dyadTrackA >= 0 && d->dyadTrackB >= 0 && d->dyadTrackA != d->dyadTrackB) {
            const Skeleton3DPerson *a = nullptr, *b = nullptr;
            for (const auto& q : frame->people) {
                if (d->shown(q)) {
                    a = q.trackId == d->dyadTrackA ? &q : a;
                    b = q.trackId == d->dyadTrackB ? &q : b;
                }
            }
            const auto ha = a ? d->hip_middle(*a, d->showSmoothed) : std::nullopt;
            const auto hb = b ? d->hip_middle(*b, d->showSmoothed) : std::nullopt;
            // The distance printed is from raw positions, as Dyad Analysis.
            const auto ra = a ? d->hip_middle(*a, false) : std::nullopt;
            const auto rb = b ? d->hip_middle(*b, false) : std::nullopt;
            const auto pa = ha ? project(*ha) : std::nullopt;
            const auto pb = hb ? project(*hb) : std::nullopt;
            if (pa && pb && ra && rb) {
                p.setPen(QPen(QColor(255, 255, 255, 200), 2, Qt::DashLine));
                p.drawLine(*pa, *pb);
                pill((*pa + *pb) * 0.5,
                     QString("%1 m").arg((*ra - *rb).length() / 1000.0, 0, 'f', 2),
                     QColor(255, 255, 255, 235), true);
            }
        }
    }

    // The hovered joint: a ring and its room coordinates.
    if (d->hover) {
        const auto& h = *d->hover;
        p.setPen(QPen(QColor(255, 255, 255, 230), 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(h.screen, 8, 8);
        const QString text = QString("Person %1 · %2\nx %3   y %4   z %5 m")
                                 .arg(h.trackId)
                                 .arg(h.name)
                                 .arg(h.pos[0] / 1000.0, 0, 'f', 2)
                                 .arg(h.pos[1] / 1000.0, 0, 'f', 2)
                                 .arg(h.pos[2] / 1000.0, 0, 'f', 2);
        const QFontMetrics fm(p.font());
        QRectF r =
            fm.boundingRect(QRect(0, 0, 400, 100), Qt::AlignLeft, text).adjusted(-6, -4, 6, 4);
        r.moveTopLeft(h.screen + QPointF(12, 10));
        if (r.right() > width() - 4) {
            r.moveRight(h.screen.x() - 12);
        }
        if (r.bottom() > height() - 4) {
            r.moveBottom(h.screen.y() - 10);
        }
        p.setPen(QColor(120, 140, 230));
        p.setBrush(QColor(12, 12, 30, 225));
        p.drawRoundedRect(r, 5, 5);
        p.setPen(QColor(235, 235, 250));
        p.drawText(r.adjusted(6, 4, -6, -4), Qt::AlignLeft, text);
    }

    // The room axes, as the view turns them (bottom right), so x, y and z
    // in the hover readout can be found.
    const QPointF o(width() - 44.0, height() - 40.0);
    struct Axis {
        QVector3D dir;
        QColor col;
        const char* name;
    };
    const Axis axes[] = {{{1, 0, 0}, QColor("#ff6666"), "x"},
                         {{0, 1, 0}, QColor("#66dd66"), "y"},
                         {{0, 0, 1}, QColor("#6699ff"), "z"}};
    for (const auto& a : axes) {
        const QVector3D v = d->view.mapVector(a.dir);
        const QPointF tip = o + QPointF(v.x(), -v.y()) * 26.0;
        p.setPen(QPen(a.col, 2));
        p.drawLine(o, tip);
        p.drawText(tip + QPointF(v.x() >= 0 ? 3 : -10, v.y() >= 0 ? -3 : 11), a.name);
    }

    QFont plain = p.font();
    plain.setBold(false);
    plain.setPixelSize(12);
    p.setFont(plain);
    p.setPen(QColor("#7070a0"));
    const QString info =
        (frame ? QString("tick %1  ·  %2 person(s)").arg(frame->tick).arg(frame->people.size())
               : QString("no data at this position")) +
        "  ·  grid 0.5 m  ·  x y z: " + d->frameCamera + "'s frame, metres";
    p.drawText(rect().adjusted(8, 4, -90, -6), Qt::AlignBottom | Qt::AlignLeft, info);
}

} // namespace mosaic
