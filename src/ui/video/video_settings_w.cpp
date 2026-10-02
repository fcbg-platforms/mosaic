#include "ui/video/video_settings_w.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <iterator>

#include "ui/video/camera_card_w.hpp"
#include "utils/logger.hpp"
#include "video/camera_label.hpp"
#include "video/fps_readout.hpp"
#include "video/interview_mode.hpp"
#include "video/video_grabber.hpp"

namespace mosaic {

struct VideoSettingsW::Impl {
    // Encoding controls (need to swap CPU vs GPU sub-panels)
    QComboBox* codecCombo        = nullptr;
    QStackedWidget* qualityStack = nullptr; // index 0 = GPU bitrate, 1 = CPU CRF
    QComboBox* presetCombo       = nullptr;

    // Camera list
    QVBoxLayout* camerasLayout = nullptr;
    QVector<CameraCardW*> cards;
    QLabel* discoverStatusLbl = nullptr;
    QPushButton* discoverBtn  = nullptr;

    // ── Interview section ────────────────────────────────────────────────
    // Edits are staged in these widgets and reach VideoSettings::interview
    // only on Apply (or when the mode is switched on). Writing through on
    // every keystroke would make the settings describe a crop the open camera
    // does not have — and session_meta.json is written from the settings.
    QGroupBox* interviewBox        = nullptr;
    QCheckBox* interviewEnabled    = nullptr;
    QLabel* interviewStatus        = nullptr;
    QComboBox* interviewCamera     = nullptr;
    QComboBox* interviewPreset     = nullptr;
    QSpinBox* interviewWidth       = nullptr;
    QSpinBox* interviewHeight      = nullptr;
    QSpinBox* interviewOffsetX     = nullptr;
    QSpinBox* interviewOffsetY     = nullptr;
    QDoubleSpinBox* interviewFps   = nullptr;
    QDoubleSpinBox* interviewUpper = nullptr;
    QLabel* interviewExposureHint  = nullptr;
    QLabel* interviewLinkLoad      = nullptr;
    QLabel* interviewMeasured      = nullptr;
    QLabel* interviewCameraLimit   = nullptr;
    QPushButton* interviewUseMax   = nullptr;
    QToolButton* interviewToggle   = nullptr; // collapses/expands the section
    QWidget* interviewBody         = nullptr;
    QLabel* interviewHeaderStatus  = nullptr;
    double interviewCameraMaxFps   = -1.0;  // see set_interview_camera_max_fps()
    bool interviewWasActive        = false; // to open the section on off -> on only
    QPushButton* interviewApply    = nullptr;
    QPushButton* interviewRevert   = nullptr;
    // The interview camera's own measured rate, while interview mode is open;
    // -1 otherwise. See set_achievable_fps().
    double interviewMeasuredFps = -1.0;
    bool recordingLocked        = false;
    // Set while load_interview_fields() fills the widgets, so the fills do
    // not count as operator edits.
    bool loadingInterview = false;
};

namespace {

// Crop presets offered in the interview section, centred in the camera's
// frame. Sizes rather than offsets: the offsets follow from the frame.
struct CropPreset {
    const char* label;
    int width;
    int height;
};
// Ordered by height, because height is what sets this camera's rate: it reads
// its sensor row by row, and width hardly matters. Measured on room 11
// (2026-10-02, Camera 3, with the 10 ms transmission delay interview mode no
// longer uses): 1280×720 36.7 fps, 960×720 38.9, 1280×540 43.6. The camera's
// own figure for whatever is applied is shown under the presets.
constexpr CropPreset k_crop_presets[] = {
    {"Full frame", 0, 0}, // 0×0 = the camera's own configured size
    {"1280 × 720 (centre)", 1280, 720},
    {"1280 × 540 (centre, faster)", 1280, 540},
    {"960 × 540 (centre, face, faster)", 960, 540},
    {"640 × 480 (centre, fastest)", 640, 480},
};

} // namespace

// ── Constructor ────────────────────────────────────────────────────────────

VideoSettingsW::VideoSettingsW(VideoSettings& settings, QWidget* parent)
    : QWidget(parent), m_settings(settings), d(std::make_unique<Impl>()) {
    // Outer layout fills the tab
    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);

    // Scrollable content
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content    = new QWidget;
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(10, 10, 10, 10);
    contentLay->setSpacing(10);

    build_encoding_section(contentLay);
    build_interview_section(contentLay);
    build_cameras_section(contentLay);
    contentLay->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);

    // Pre-reserve so push_back never reallocates — keeps all CameraParameters&
    // references held by existing cards (CameraCardW) AND by any already-open
    // VideoGrabber (see VideoManager::open(), called before this widget is
    // constructed — Application::initialize() also reserves this same cap
    // for that reason) valid for the lifetime of this widget.
    m_settings.cameras.reserve(VideoSettings::kMaxCameras);

    // Create cards for cameras already in settings WITHOUT pushing them again.
    for (int i = 0; i < static_cast<int>(m_settings.cameras.size()); ++i) make_card(i);
    sync_interview_from_settings();
}

VideoSettingsW::~VideoSettingsW() = default;

void VideoSettingsW::set_discover_enabled(bool enabled) {
    if (d->discoverBtn) {
        d->discoverBtn->setEnabled(enabled);
    }
}

void VideoSettingsW::set_action_command_capability(int cameraIndex, bool supported) {
    if (cameraIndex < 0 || cameraIndex >= d->cards.size()) {
        return;
    }
    d->cards[cameraIndex]->set_action_command_capability(supported);
}

void VideoSettingsW::set_achievable_fps(int cameraIndex, double fps) {
    if (cameraIndex < 0 || cameraIndex >= d->cards.size()) {
        return;
    }
    // In interview mode this camera is running the interview crop and rate,
    // so its measurement belongs to the interview section. Handing it to the
    // card as well would grade a 40 fps crop against the card's own 25 fps
    // room setting — a true number under a false comparison.
    if (m_settings.interview_active() && cameraIndex == m_settings.interview.cameraIndex) {
        d->interviewMeasuredFps = fps;
        refresh_interview_readouts();
        d->cards[cameraIndex]->set_achievable_fps(-1.0);
        return;
    }
    d->cards[cameraIndex]->set_achievable_fps(fps);
}

// ── Interview section ──────────────────────────────────────────────────────

void VideoSettingsW::build_interview_section(QVBoxLayout* parent) {
    // Collapsed by default: most sessions never touch it, and its dozen
    // controls would otherwise sit above the cameras every time. A header
    // button opens it; it also opens by itself while interview mode is on, so
    // the settings in effect are in view (sync_interview_from_settings()).
    auto* box       = new QGroupBox;
    d->interviewBox = box;
    auto* outer     = new QVBoxLayout(box);
    outer->setSpacing(6);

    auto* header       = new QHBoxLayout;
    d->interviewToggle = new QToolButton;
    d->interviewToggle->setText("Interview mode");
    d->interviewToggle->setCheckable(true);
    d->interviewToggle->setChecked(false);
    d->interviewToggle->setArrowType(Qt::RightArrow);
    d->interviewToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    d->interviewToggle->setAutoRaise(true);
    d->interviewToggle->setStyleSheet("QToolButton { font-weight: bold; border: none; }");
    d->interviewHeaderStatus = new QLabel;
    d->interviewHeaderStatus->setProperty("role", "muted");
    header->addWidget(d->interviewToggle);
    header->addStretch();
    header->addWidget(d->interviewHeaderStatus);
    outer->addLayout(header);

    d->interviewBody = new QWidget;
    d->interviewBody->setVisible(false);
    outer->addWidget(d->interviewBody);
    connect(d->interviewToggle, &QToolButton::toggled, this, [this](bool open) {
        d->interviewToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        d->interviewBody->setVisible(open);
    });

    auto* lay = new QVBoxLayout(d->interviewBody);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    auto* intro = new QLabel(
        "Record one camera only, cropped and at a higher frame rate. The other cameras "
        "are closed while it is on. Switch it here or with the toggle above the live "
        "feeds; the room configuration below is not changed.");
    intro->setProperty("role", "muted");
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto* toggleRow     = new QHBoxLayout;
    d->interviewEnabled = new QCheckBox("Interview mode on");
    d->interviewStatus  = new QLabel;
    d->interviewStatus->setProperty("role", "muted");
    d->interviewStatus->setWordWrap(true);
    toggleRow->addWidget(d->interviewEnabled);
    toggleRow->addStretch();
    toggleRow->addWidget(d->interviewStatus);
    lay->addLayout(toggleRow);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(10);

    d->interviewCamera = new QComboBox;
    d->interviewCamera->setToolTip(
        "The camera to record. Its own settings card still controls everything but the "
        "crop, frame rate and exposure limit set here.");
    form->addRow("Camera:", d->interviewCamera);

    d->interviewPreset = new QComboBox;
    for (const auto& p : k_crop_presets) {
        d->interviewPreset->addItem(p.label);
    }
    d->interviewPreset->addItem("Custom");
    d->interviewPreset->setToolTip(
        "A smaller crop is what makes a higher frame rate possible: the sensor reads out "
        "row by row, and every pixel crosses the camera's gigabit link.");
    form->addRow("Crop:", d->interviewPreset);

    auto make_spin = [](int lo, int hi, int step, const QString& suffix) {
        auto* s = new QSpinBox;
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setSuffix(suffix);
        s->setFixedWidth(92);
        return s;
    };
    d->interviewWidth   = make_spin(16, 4096, 16, " px");
    d->interviewHeight  = make_spin(16, 4096, 8, " px");
    d->interviewOffsetX = make_spin(0, 4096, 4, " px");
    d->interviewOffsetY = make_spin(0, 4096, 4, " px");

    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(d->interviewWidth);
    sizeRow->addWidget(new QLabel("×"));
    sizeRow->addWidget(d->interviewHeight);
    sizeRow->addStretch();
    form->addRow("Size:", sizeRow);

    auto* offsetRow = new QHBoxLayout;
    offsetRow->addWidget(d->interviewOffsetX);
    offsetRow->addWidget(new QLabel(","));
    offsetRow->addWidget(d->interviewOffsetY);
    auto* centreBtn = new QPushButton("Centre");
    centreBtn->setFixedHeight(24);
    centreBtn->setToolTip("Centre the crop in the camera's configured frame.");
    offsetRow->addWidget(centreBtn);
    offsetRow->addStretch();
    form->addRow("Offset:", offsetRow);

    d->interviewFps = new QDoubleSpinBox;
    d->interviewFps->setRange(1.0, 200.0);
    d->interviewFps->setDecimals(1);
    d->interviewFps->setSingleStep(5.0);
    d->interviewFps->setSuffix(" fps");
    d->interviewFps->setFixedWidth(110);
    d->interviewFps->setToolTip(
        "The rate to ask the camera for. Out-of-range values are clamped by the camera "
        "and logged; the rate it really reaches is shown below once it runs.");
    form->addRow("Frame rate:", d->interviewFps);

    d->interviewUpper = new QDoubleSpinBox;
    d->interviewUpper->setRange(100.0, 1'000'000.0);
    d->interviewUpper->setDecimals(0);
    d->interviewUpper->setSingleStep(1000.0);
    d->interviewUpper->setSuffix(" µs");
    d->interviewUpper->setFixedWidth(110);
    d->interviewUpper->setToolTip(
        "The longest exposure auto exposure may choose in this mode. A frame cannot be "
        "exposed for longer than the gap between frames, so the higher the frame rate, "
        "the lower this must be — at the cost of a darker image in dim light.");
    auto* upperRow = new QHBoxLayout;
    upperRow->addWidget(d->interviewUpper);
    upperRow->addStretch();
    form->addRow("Exposure limit:", upperRow);
    // On its own full-width line under the field, wrapping: beside the spin
    // box it ran off the edge of the settings panel (at most 520 px wide) and
    // could only be read by dragging the panel wider.
    d->interviewExposureHint = new QLabel;
    d->interviewExposureHint->setProperty("role", "muted");
    d->interviewExposureHint->setWordWrap(true);
    d->interviewExposureHint->setTextFormat(Qt::RichText);
    form->addRow(d->interviewExposureHint);

    lay->addLayout(form);

    d->interviewLinkLoad = new QLabel;
    d->interviewLinkLoad->setProperty("role", "muted");
    d->interviewLinkLoad->setWordWrap(true);
    d->interviewLinkLoad->setTextFormat(Qt::RichText);
    lay->addWidget(d->interviewLinkLoad);

    auto* limitRow          = new QHBoxLayout;
    d->interviewCameraLimit = new QLabel;
    d->interviewCameraLimit->setWordWrap(true);
    d->interviewCameraLimit->setTextFormat(Qt::RichText);
    d->interviewUseMax = new QPushButton;
    d->interviewUseMax->setFixedHeight(24);
    d->interviewUseMax->setVisible(false);
    d->interviewUseMax->setToolTip(
        "Ask for the rate this camera says it can deliver with this crop. Apply to use it.");
    connect(d->interviewUseMax, &QPushButton::clicked, this, [this] {
        if (d->interviewCameraMaxFps > 0) {
            // Rounded down, so the request never ends up a hair above the
            // camera's own figure and reads as "below the rate asked for".
            d->interviewFps->setValue(std::floor(d->interviewCameraMaxFps * 10.0) / 10.0);
        }
    });
    limitRow->addWidget(d->interviewCameraLimit, 1);
    limitRow->addWidget(d->interviewUseMax);
    lay->addLayout(limitRow);

    d->interviewMeasured = new QLabel;
    d->interviewMeasured->setWordWrap(true);
    d->interviewMeasured->setTextFormat(Qt::RichText);
    lay->addWidget(d->interviewMeasured);

    auto* btnRow       = new QHBoxLayout;
    d->interviewApply  = new QPushButton("Apply");
    d->interviewRevert = new QPushButton("Revert");
    d->interviewApply->setFixedHeight(26);
    d->interviewRevert->setFixedHeight(26);
    btnRow->addStretch();
    btnRow->addWidget(d->interviewRevert);
    btnRow->addWidget(d->interviewApply);
    lay->addLayout(btnRow);

    // ── Behaviour ───────────────────────────────────────────────────────
    const auto on_edit = [this] {
        if (d->loadingInterview) return;
        refresh_interview_readouts();
    };
    for (auto* s :
         {d->interviewWidth, d->interviewHeight, d->interviewOffsetX, d->interviewOffsetY}) {
        connect(s, qOverload<int>(&QSpinBox::valueChanged), this, on_edit);
    }
    // A hand-edited size no longer matches a preset; say so rather than leave
    // a preset name over a crop it does not describe.
    for (auto* s : {d->interviewWidth, d->interviewHeight}) {
        connect(s, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
            if (d->loadingInterview) return;
            const QSignalBlocker block(d->interviewPreset);
            d->interviewPreset->setCurrentIndex(d->interviewPreset->count() - 1); // Custom
        });
    }
    for (auto* s : {d->interviewFps, d->interviewUpper}) {
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), this, on_edit);
    }
    connect(d->interviewCamera, qOverload<int>(&QComboBox::currentIndexChanged), this, on_edit);

    connect(d->interviewPreset, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                if (d->loadingInterview || idx < 0 ||
                    idx >= static_cast<int>(std::size(k_crop_presets))) {
                    return; // "Custom": leave the fields as they are
                }
                const int cam = d->interviewCamera->currentData().toInt();
                const CameraParameters frame =
                    (cam >= 0 && cam < static_cast<int>(m_settings.cameras.size()))
                        ? m_settings.cameras[static_cast<size_t>(cam)]
                        : CameraParameters{};
                const auto& p       = k_crop_presets[idx];
                const int w         = p.width > 0 ? p.width : frame.width;
                const int h         = p.height > 0 ? p.height : frame.height;
                d->loadingInterview = true;
                d->interviewWidth->setValue(w);
                d->interviewHeight->setValue(h);
                d->interviewOffsetX->setValue(p.width > 0 ? centred_offset(frame.width, w) : 0);
                d->interviewOffsetY->setValue(p.height > 0 ? centred_offset(frame.height, h) : 0);
                d->loadingInterview = false;
                refresh_interview_readouts();
            });

    connect(centreBtn, &QPushButton::clicked, this, [this] {
        const int cam = d->interviewCamera->currentData().toInt();
        if (cam < 0 || cam >= static_cast<int>(m_settings.cameras.size())) return;
        const auto& frame = m_settings.cameras[static_cast<size_t>(cam)];
        d->interviewOffsetX->setValue(centred_offset(frame.width, d->interviewWidth->value()));
        d->interviewOffsetY->setValue(centred_offset(frame.height, d->interviewHeight->value()));
    });

    connect(d->interviewApply, &QPushButton::clicked, this, [this] {
        m_settings.interview = staged_interview();
        emit settings_changed();
        emit interview_settings_applied();
        refresh_interview_readouts();
    });
    connect(d->interviewRevert, &QPushButton::clicked, this,
            [this] { load_interview_fields(m_settings.interview); });

    // The checkbox asks; MainWindow decides. Put it straight back and let
    // sync_interview_from_settings() show the outcome, so a refused switch
    // can never leave it claiming a mode that is not in effect.
    connect(d->interviewEnabled, &QCheckBox::toggled, this, [this](bool on) {
        if (d->loadingInterview) return;
        {
            const QSignalBlocker block(d->interviewEnabled);
            d->interviewEnabled->setChecked(!on);
        }
        if (on) {
            // Open with what is on screen, not with whatever was last applied.
            m_settings.interview         = staged_interview();
            m_settings.interview.enabled = false; // MainWindow sets it on success
            emit settings_changed();
        }
        emit interview_mode_requested(on);
    });

    parent->addWidget(box);
}

void VideoSettingsW::rebuild_interview_camera_combo(int keep) {
    if (!d->interviewCamera) return;
    const QSignalBlocker block(d->interviewCamera);
    d->interviewCamera->clear();
    for (int i = 0; i < static_cast<int>(m_settings.cameras.size()); ++i) {
        const auto& cam = m_settings.cameras[static_cast<size_t>(i)];
        QString text    = camera_label(i);
        if (!cam.serialNumber.isEmpty()) {
            text += QString("  ·  %1").arg(cam.serialNumber);
        }
        d->interviewCamera->addItem(text, i);
    }
    // A persisted index past the end of the list (settings from a bigger rig)
    // is kept visible as such rather than silently replaced by camera 1:
    // VideoSettings::interview_active() already treats it as "off".
    if (keep >= static_cast<int>(m_settings.cameras.size())) {
        d->interviewCamera->addItem(QString("%1 (not configured)").arg(camera_label(keep)), keep);
    }
    d->interviewCamera->setCurrentIndex(d->interviewCamera->findData(keep));
}

void VideoSettingsW::load_interview_fields(const InterviewSettings& s) {
    d->loadingInterview = true;
    d->interviewCamera->setCurrentIndex(d->interviewCamera->findData(s.cameraIndex));
    d->interviewWidth->setValue(s.width);
    d->interviewHeight->setValue(s.height);
    d->interviewOffsetX->setValue(s.offsetX);
    d->interviewOffsetY->setValue(s.offsetY);
    d->interviewFps->setValue(s.fps);
    d->interviewUpper->setValue(s.exposureAutoUpperUs);
    // Show the matching preset when the crop is one, "Custom" otherwise.
    int preset = d->interviewPreset->count() - 1;
    for (int i = 0; i < static_cast<int>(std::size(k_crop_presets)); ++i) {
        if (k_crop_presets[i].width == s.width && k_crop_presets[i].height == s.height) {
            preset = i;
            break;
        }
    }
    d->interviewPreset->setCurrentIndex(preset);
    d->loadingInterview = false;
    refresh_interview_readouts();
}

InterviewSettings VideoSettingsW::staged_interview() const {
    InterviewSettings s   = m_settings.interview; // keeps `enabled`
    s.cameraIndex         = d->interviewCamera->currentData().toInt();
    s.width               = d->interviewWidth->value();
    s.height              = d->interviewHeight->value();
    s.offsetX             = d->interviewOffsetX->value();
    s.offsetY             = d->interviewOffsetY->value();
    s.fps                 = d->interviewFps->value();
    s.exposureAutoUpperUs = d->interviewUpper->value();
    return s;
}

void VideoSettingsW::sync_interview_from_settings() {
    if (!d->interviewEnabled) return;
    rebuild_interview_camera_combo(m_settings.interview.cameraIndex);
    {
        const QSignalBlocker block(d->interviewEnabled);
        d->interviewEnabled->setChecked(m_settings.interview_active());
    }
    const bool active = m_settings.interview_active();
    if (!active) {
        d->interviewMeasuredFps  = -1.0;
        d->interviewCameraMaxFps = -1.0;
    } else if (!d->interviewWasActive && d->interviewToggle) {
        // Opened when the mode turns on, to show the settings in effect — and
        // only then, so an operator who collapses it again is not overruled
        // by every later reopen or refresh.
        d->interviewToggle->setChecked(true);
    }
    d->interviewWasActive = active;
    load_interview_fields(m_settings.interview);
}

void VideoSettingsW::set_interview_camera_max_fps(double fps) {
    d->interviewCameraMaxFps = fps;
    refresh_interview_readouts();
}

void VideoSettingsW::set_recording_locked(bool locked) {
    d->recordingLocked = locked;
    if (d->interviewBox) {
        // The settings lock, not the header: opening the section to see what
        // is in effect stays possible during a recording.
        (d->interviewBody ? d->interviewBody : static_cast<QWidget*>(d->interviewBox))
            ->setEnabled(!locked);
        d->interviewBox->setToolTip(locked ? "Locked while recording — switching or changing "
                                             "interview mode reopens the cameras."
                                           : QString());
    }
    refresh_interview_readouts();
}

void VideoSettingsW::refresh_interview_readouts() {
    if (!d->interviewMeasured) return;
    const InterviewSettings staged = staged_interview();
    const bool active              = m_settings.interview_active();
    const bool dirty               = staged.cameraIndex != m_settings.interview.cameraIndex ||
                       staged.width != m_settings.interview.width ||
                       staged.height != m_settings.interview.height ||
                       staged.offsetX != m_settings.interview.offsetX ||
                       staged.offsetY != m_settings.interview.offsetY ||
                       staged.fps != m_settings.interview.fps ||
                       staged.exposureAutoUpperUs != m_settings.interview.exposureAutoUpperUs;
    d->interviewApply->setEnabled(dirty);
    d->interviewRevert->setEnabled(dirty);
    d->interviewApply->setText(dirty && active ? "Apply && reopen" : "Apply");

    d->interviewStatus->setText(
        active ? QString("On — %1 only").arg(camera_label(m_settings.interview.cameraIndex))
               : "Off — recording every camera");
    if (d->interviewHeaderStatus) {
        d->interviewHeaderStatus->setText(
            active ? QString("On · %1").arg(camera_label(m_settings.interview.cameraIndex))
                   : "Off");
    }

    // Exposure limit vs rate. Only the necessary condition is stated: an
    // exposure longer than a frame period cannot reach the rate. Short enough
    // is NOT sufficient — on this camera the rate turned out to be set by
    // sensor readout (crop height), and exposure from 2 to 20 ms changed
    // nothing. This hint used to say "allows N fps", which was false.
    const double maxExposure = max_exposure_us_for_fps(staged.fps);
    if (staged.exposureAutoUpperUs > maxExposure) {
        d->interviewExposureHint->setText(
            QString("<font color='#ddaa44'>longer than one frame at %1 fps — %2 µs or "
                    "less is needed</font>")
                .arg(staged.fps, 0, 'f', 1)
                .arg(maxExposure, 0, 'f', 0));
    } else {
        d->interviewExposureHint->setText("shorter is darker in dim light");
    }

    // Crop vs the camera's frame, and what the link has to carry.
    const int cam = staged.cameraIndex;
    QStringList lines;
    if (cam >= 0 && cam < static_cast<int>(m_settings.cameras.size())) {
        const auto& frame = m_settings.cameras[static_cast<size_t>(cam)];
        if (!crop_fits(frame.width, frame.height, staged.width, staged.height, staged.offsetX,
                       staged.offsetY)) {
            lines << QString(
                         "<font color='#ddaa44'>The crop runs outside %1's %2×%3 frame — "
                         "the camera will shrink or shift it.</font>")
                         .arg(camera_label(cam))
                         .arg(frame.width)
                         .arg(frame.height);
        }
    }
    const double mbps = stream_mb_per_s(staged.width, staged.height, staged.fps);
    const double load = mbps / k_gige_link_mb_per_s;
    const QString loadText = QString("Link: ≈ %1 MB/s of the camera's %2 MB/s gigabit link (%3%).")
                                 .arg(mbps, 0, 'f', 0)
                                 .arg(k_gige_link_mb_per_s, 0, 'f', 0)
                                 .arg(load * 100.0, 0, 'f', 0);
    lines << (load > 0.9 ? QString("<font color='#ddaa44'>%1 Too close to the link's capacity "
                                   "— expect lost frames; shrink the crop or the rate.</font>")
                               .arg(loadText)
                         : loadText);
    d->interviewLinkLoad->setText(lines.join("<br>"));

    // The camera's own limit for the applied crop, known at once.
    d->interviewUseMax->setVisible(false);
    if (!active || d->interviewCameraMaxFps <= 0) {
        d->interviewCameraLimit->setText(
            active ? QString()
                   : "<span style='color:#7878a0'>The camera's own limit for a crop is shown "
                     "here once interview mode is on.</span>");
        d->interviewCameraLimit->setVisible(!active);
    } else {
        d->interviewCameraLimit->setVisible(true);
        const double asked = m_settings.interview.fps;
        const double cap   = d->interviewCameraMaxFps;
        // ResultingFrameRate is the lesser of the rate asked for and what the
        // camera can do, so a figure below the request is the camera's limit.
        QString t;
        if (asked > cap * 1.01) {
            t = QString(
                    "<font color='#ddaa44'>Camera's maximum for this crop: <b>%1 fps</b> — "
                    "less than the %2 fps asked for. It reads the sensor row by row, so "
                    "fewer rows (a shorter crop) raise it; width and exposure hardly "
                    "matter.</font>")
                    .arg(cap, 0, 'f', 1)
                    .arg(asked, 0, 'f', 1);
            d->interviewUseMax->setText(
                QString("Use %1 fps").arg(std::floor(cap * 10.0) / 10.0, 0, 'f', 1));
            d->interviewUseMax->setVisible(true);
        } else {
            t = QString("The camera can deliver the %1 fps asked for with this crop.")
                    .arg(asked, 0, 'f', 1);
        }
        if (dirty) {
            t += "<br><span style='color:#7878a0'>(for the crop applied now — Apply to see "
                 "the new one's)</span>";
        }
        d->interviewCameraLimit->setText(t);
    }

    // What the camera actually reaches — only knowable while it runs.
    if (!active) {
        d->interviewMeasured->setText(
            "<span style='color:#7878a0'>The rate the camera really reaches is shown here "
            "once interview mode is on.</span>");
        return;
    }
    const CameraParameters& src =
        m_settings.cameras[static_cast<size_t>(m_settings.interview.cameraIndex)];
    const FpsReadout r = compute_fps_readout(
        d->interviewMeasuredFps, true, m_settings.interview.fps, src.exposureAuto == "Off",
        src.exposureTimeUs, m_settings.interview.exposureAutoUpperUs);
    QString text;
    switch (r.kind) {
        case FpsReadoutKind::Measured:
            text = QString("Camera reports <b>%1 fps</b>").arg(r.fps, 0, 'f', 1);
            if (r.belowConfigured) {
                text = QString(
                           "<font color='#ddaa44'>%1 — below the %2 fps asked for. Fewer "
                           "crop rows raise the ceiling.</font>")
                           .arg(text)
                           .arg(m_settings.interview.fps, 0, 'f', 1);
            } else if (r.limitedBy == FpsLimit::ConfiguredRate) {
                text += QString(" — running at the %1 fps asked for.")
                            .arg(m_settings.interview.fps, 0, 'f', 1);
            }
            break;
        case FpsReadoutKind::ExposureCeiling:
            text = QString("At most %1 fps at the camera's manual exposure.").arg(r.fps, 0, 'f', 1);
            break;
        case FpsReadoutKind::ExposureLimitFloor:
        case FpsReadoutKind::AwaitingMeasurement:
            text =
                "<span style='color:#7878a0'>Measuring — the camera reports its real rate "
                "a few seconds after it opens.</span>";
            break;
    }
    d->interviewMeasured->setText(text);
}

// ── Encoding section ───────────────────────────────────────────────────────

void VideoSettingsW::build_encoding_section(QVBoxLayout* parent) {
    auto* box  = new QGroupBox("Encoding");
    auto* form = new QVBoxLayout(box);
    form->setSpacing(8);

    // ── Codec ───────────────────────────────────────────────────────────
    auto* codecRow = new QHBoxLayout;
    auto* codecLbl = new QLabel("Codec:");
    codecLbl->setFixedWidth(90);
    codecLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    d->codecCombo = new QComboBox;
    d->codecCombo->addItem("H.264  —  NVIDIA GPU", "h264_nvenc");
    d->codecCombo->addItem("H.265  —  NVIDIA GPU", "hevc_nvenc");
    d->codecCombo->addItem("H.264  —  CPU (libx264)", "libx264");
    d->codecCombo->addItem("H.265  —  CPU (libx265)", "libx265");
    d->codecCombo->setCurrentIndex(d->codecCombo->findData(m_settings.codec));
    codecRow->addWidget(codecLbl);
    codecRow->addWidget(d->codecCombo, 1);
    form->addLayout(codecRow);

    // ── Preset ──────────────────────────────────────────────────────────
    auto* presetRow = new QHBoxLayout;
    auto* presetLbl = new QLabel("Preset:");
    presetLbl->setFixedWidth(90);
    presetLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    d->presetCombo = new QComboBox;
    presetRow->addWidget(presetLbl);
    presetRow->addWidget(d->presetCombo, 1);
    form->addLayout(presetRow);

    // ── Quality stack: GPU (bitrate) vs CPU (CRF) ────────────────────────
    d->qualityStack = new QStackedWidget;

    // GPU panel — bitrate
    {
        auto* panel = new QWidget;
        auto* row   = new QHBoxLayout(panel);
        row->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel("Bitrate:");
        lbl->setFixedWidth(90);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* spin = new QSpinBox;
        spin->setRange(500, 400'000);
        spin->setSingleStep(500);
        spin->setValue(m_settings.bitrate);
        spin->setSuffix("  kbit/s");
        spin->setFixedWidth(130);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
            m_settings.bitrate = v;
            emit settings_changed();
        });
        row->addWidget(lbl);
        row->addWidget(spin);
        row->addStretch();
        d->qualityStack->addWidget(panel); // index 0
    }

    // CPU panel — CRF
    {
        auto* panel = new QWidget;
        auto* row   = new QHBoxLayout(panel);
        row->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel("CRF:");
        lbl->setFixedWidth(90);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* spin = new QSpinBox;
        spin->setRange(0, 51);
        spin->setValue(m_settings.crf);
        auto* hint = new QLabel("(17 = best, 28 = smaller)");
        hint->setProperty("role", "muted");
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
            m_settings.crf = v;
            emit settings_changed();
        });
        row->addWidget(lbl);
        row->addWidget(spin);
        row->addSpacing(6);
        row->addWidget(hint);
        row->addStretch();
        d->qualityStack->addWidget(panel); // index 1
    }

    form->addWidget(d->qualityStack);

    // ── Sync to codec selection ──────────────────────────────────────────
    auto refresh_codec = [this](int idx) {
        const QString codec = d->codecCombo->itemData(idx).toString();
        const bool isGpu    = codec.contains("nvenc");
        m_settings.codec    = codec;

        // Swap preset items
        d->presetCombo->blockSignals(true);
        d->presetCombo->clear();
        if (isGpu) {
            for (int i = 1; i <= 7; ++i)
                d->presetCombo->addItem(QString("P%1  (%2)")
                                            .arg(i)
                                            .arg(i <= 2   ? "fastest"
                                                 : i <= 4 ? "balanced"
                                                          : "quality"),
                                        QString("p%1").arg(i));
            d->qualityStack->setCurrentIndex(0); // bitrate panel
        } else {
            for (const QString p : {"ultrafast", "superfast", "veryfast", "faster", "fast",
                                    "medium", "slow", "slower", "veryslow"})
                d->presetCombo->addItem(p, p);
            d->qualityStack->setCurrentIndex(1); // CRF panel
        }
        d->presetCombo->setCurrentText(m_settings.preset);
        d->presetCombo->blockSignals(false);
        emit settings_changed();
    };

    connect(d->codecCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, refresh_codec);
    refresh_codec(d->codecCombo->currentIndex()); // initialise

    connect(d->presetCombo, &QComboBox::currentTextChanged, this, [this](const QString& v) {
        m_settings.preset = v;
        emit settings_changed();
    });

    parent->addWidget(box);
}

// ── Cameras section ────────────────────────────────────────────────────────

void VideoSettingsW::build_cameras_section(QVBoxLayout* parent) {
    // ── Header row: "Cameras" title + "Add Camera" button ────────────────
    auto* headerRow = new QHBoxLayout;

    auto* titleLbl = new QLabel("Cameras");
    titleLbl->setProperty("role", "section");
    headerRow->addWidget(titleLbl);
    headerRow->addStretch();

    auto* discoverBtn = new QPushButton("Discover cameras");
    discoverBtn->setFixedHeight(26);
    discoverBtn->setToolTip(
        "Scan for physically-connected Basler cameras and add a card "
        "for each one found, with its serial number already filled in.");
    connect(discoverBtn, &QPushButton::clicked, this, &VideoSettingsW::discover_cameras);
    headerRow->addWidget(discoverBtn);
    d->discoverBtn = discoverBtn;

    auto* addBtn = new QPushButton("+ Add camera");
    addBtn->setFixedHeight(26);
    connect(addBtn, &QPushButton::clicked, this, [this] {
        add_camera({}); // position-based label is set by the card itself
    });
    headerRow->addWidget(addBtn);

    parent->addLayout(headerRow);

    d->discoverStatusLbl = new QLabel;
    d->discoverStatusLbl->setProperty("role", "muted");
    d->discoverStatusLbl->setWordWrap(true);
    d->discoverStatusLbl->hide();
    parent->addWidget(d->discoverStatusLbl);

    // ── Camera cards list ─────────────────────────────────────────────────
    auto* cardsWidget = new QWidget;
    d->camerasLayout  = new QVBoxLayout(cardsWidget);
    d->camerasLayout->setContentsMargins(0, 0, 0, 0);
    d->camerasLayout->setSpacing(6);

    parent->addWidget(cardsWidget);
}

void VideoSettingsW::discover_cameras() {
    const QVector<DiscoveredCamera> found = VideoGrabber::enumerate_devices();

    // Add every new camera first and emit settings_changed()/cameras_list_changed()
    // only once at the end, instead of calling add_camera() per device (which
    // each emit cameras_list_changed() on their own). MainWindow reacts to that
    // signal by closing and reopening every configured camera against real
    // hardware — doing that once per discovered device would mean N redundant,
    // increasingly-large close/reopen cycles all queued and run back-to-back on
    // the GUI thread for one click, easily adding up to a many-seconds freeze
    // with several real cameras.
    int added  = 0;
    bool atCap = false;
    for (const auto& cam : found) {
        if (cam.serialNumber.isEmpty()) {
            continue;
        }
        const bool alreadyPresent =
            std::any_of(m_settings.cameras.cbegin(), m_settings.cameras.cend(),
                        [&](const CameraParameters& existing) {
                            return existing.serialNumber == cam.serialNumber;
                        });
        if (alreadyPresent) {
            continue;
        }

        // Refuse past kMaxCameras rather than push_back()ing past the
        // capacity every already-open VideoGrabber/CameraCardW's
        // CameraParameters& reference depends on staying stable — see
        // VideoSettings::kMaxCameras's doc comment (settings.hpp).
        if (m_settings.cameras.size() >= static_cast<size_t>(VideoSettings::kMaxCameras)) {
            log_error(QString("[VideoSettingsW] discover_cameras: already at the %1-camera "
                              "limit — not adding any more.")
                          .arg(VideoSettings::kMaxCameras));
            atCap = true;
            break;
        }

        CameraParameters params;
        params.serialNumber = cam.serialNumber;
        if (!cam.modelName.isEmpty()) {
            params.friendlyName = cam.modelName;
        }
        m_settings.cameras.push_back(std::move(params));
        make_card(static_cast<int>(m_settings.cameras.size()) - 1);
        ++added;
    }

    if (added > 0) {
        rebuild_interview_camera_combo(d->interviewCamera->currentData().toInt());
        emit settings_changed();
        emit cameras_list_changed();
    }

    if (d->discoverStatusLbl) {
        d->discoverStatusLbl->setText(
            found.isEmpty()
                ? "No cameras found — check network cabling/power, or that this build was "
                  "compiled with camera support."
                : QString("Found %1 camera(s); added %2 new card(s) (%3 already configured).%4")
                      .arg(found.size())
                      .arg(added)
                      .arg(found.size() - added)
                      .arg(atCap ? QString(" Reached the %1-camera limit.")
                                       .arg(VideoSettings::kMaxCameras)
                                 : QString()));
        d->discoverStatusLbl->show();
    }
}

void VideoSettingsW::make_card(int index) {
    auto* card = new CameraCardW(m_settings.cameras[index], index, this);
    connect(card, &CameraCardW::params_changed, this, &VideoSettingsW::settings_changed);
    connect(card, &CameraCardW::params_changed, this,
            [this, index] { emit camera_params_changed(index); });
    d->camerasLayout->addWidget(card);
    d->cards.append(card);
}

void VideoSettingsW::add_camera(CameraParameters params) {
    // Refuse past kMaxCameras rather than push_back()ing past the capacity
    // every already-open VideoGrabber/CameraCardW's CameraParameters&
    // reference depends on staying stable — see VideoSettings::kMaxCameras's
    // doc comment (settings.hpp).
    if (m_settings.cameras.size() >= static_cast<size_t>(VideoSettings::kMaxCameras)) {
        log_error(QString("[VideoSettingsW] add_camera: already at the %1-camera limit — "
                          "refusing to add another.")
                      .arg(VideoSettings::kMaxCameras));
        return;
    }
    m_settings.cameras.push_back(std::move(params));
    make_card(static_cast<int>(m_settings.cameras.size()) - 1);
    rebuild_interview_camera_combo(d->interviewCamera->currentData().toInt());
    emit settings_changed();
    emit cameras_list_changed();
}

} // namespace mosaic
