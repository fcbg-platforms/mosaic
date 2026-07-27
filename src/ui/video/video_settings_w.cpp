#include "ui/video/video_settings_w.hpp"
#include "ui/video/camera_card_w.hpp"
#include "utils/logger.hpp"
#include "video/video_grabber.hpp"
#include <algorithm>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace mosaic {

struct VideoSettingsW::Impl {
    // Encoding controls (need to swap CPU vs GPU sub-panels)
    QComboBox*     codecCombo    = nullptr;
    QStackedWidget* qualityStack = nullptr;  // index 0 = GPU bitrate, 1 = CPU CRF
    QComboBox*     presetCombo   = nullptr;

    // Camera list
    QVBoxLayout*   camerasLayout = nullptr;
    QVector<CameraCardW*> cards;
    QLabel*        discoverStatusLbl = nullptr;
    QPushButton*   discoverBtn       = nullptr;
};

// ── Constructor ────────────────────────────────────────────────────────────

VideoSettingsW::VideoSettingsW(VideoSettings& settings, QWidget* parent)
    : QWidget(parent), m_settings(settings), d(std::make_unique<Impl>())
{
    // Outer layout fills the tab
    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);

    // Scrollable content
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget;
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(10, 10, 10, 10);
    contentLay->setSpacing(10);

    build_encoding_section(contentLay);
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
    for (int i = 0; i < static_cast<int>(m_settings.cameras.size()); ++i)
        make_card(i);
}

VideoSettingsW::~VideoSettingsW() = default;

void VideoSettingsW::set_discover_enabled(bool enabled) {
    if (d->discoverBtn) { d->discoverBtn->setEnabled(enabled); }
}

void VideoSettingsW::set_action_command_capability(int cameraIndex, bool supported) {
    if (cameraIndex < 0 || cameraIndex >= d->cards.size()) { return; }
    d->cards[cameraIndex]->set_action_command_capability(supported);
}

// ── Encoding section ───────────────────────────────────────────────────────

void VideoSettingsW::build_encoding_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Encoding");
    auto* form = new QVBoxLayout(box);
    form->setSpacing(8);

    // ── Codec ───────────────────────────────────────────────────────────
    auto* codecRow = new QHBoxLayout;
    auto* codecLbl = new QLabel("Codec:");
    codecLbl->setFixedWidth(90);
    codecLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    d->codecCombo = new QComboBox;
    d->codecCombo->addItem("H.264  —  NVIDIA GPU",  "h264_nvenc");
    d->codecCombo->addItem("H.265  —  NVIDIA GPU",  "hevc_nvenc");
    d->codecCombo->addItem("H.264  —  CPU (libx264)","libx264");
    d->codecCombo->addItem("H.265  —  CPU (libx265)","libx265");
    d->codecCombo->setCurrentIndex(
        d->codecCombo->findData(m_settings.codec));
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
        row->setContentsMargins(0,0,0,0);
        auto* lbl   = new QLabel("Bitrate:");
        lbl->setFixedWidth(90);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* spin  = new QSpinBox;
        spin->setRange(500, 400'000);
        spin->setSingleStep(500);
        spin->setValue(m_settings.bitrate);
        spin->setSuffix("  kbit/s");
        spin->setFixedWidth(130);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v){
            m_settings.bitrate = v; emit settings_changed();
        });
        row->addWidget(lbl);
        row->addWidget(spin);
        row->addStretch();
        d->qualityStack->addWidget(panel);   // index 0
    }

    // CPU panel — CRF
    {
        auto* panel = new QWidget;
        auto* row   = new QHBoxLayout(panel);
        row->setContentsMargins(0,0,0,0);
        auto* lbl   = new QLabel("CRF:");
        lbl->setFixedWidth(90);
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* spin  = new QSpinBox;
        spin->setRange(0, 51);
        spin->setValue(m_settings.crf);
        auto* hint = new QLabel("(17 = best, 28 = smaller)");
        hint->setProperty("role", "muted");
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v){
            m_settings.crf = v; emit settings_changed();
        });
        row->addWidget(lbl);
        row->addWidget(spin);
        row->addSpacing(6);
        row->addWidget(hint);
        row->addStretch();
        d->qualityStack->addWidget(panel);   // index 1
    }

    form->addWidget(d->qualityStack);

    // ── Sync to codec selection ──────────────────────────────────────────
    auto refresh_codec = [this](int idx) {
        const QString codec = d->codecCombo->itemData(idx).toString();
        const bool isGpu = codec.contains("nvenc");
        m_settings.codec = codec;

        // Swap preset items
        d->presetCombo->blockSignals(true);
        d->presetCombo->clear();
        if (isGpu) {
            for (int i = 1; i <= 7; ++i)
                d->presetCombo->addItem(QString("P%1  (%2)").arg(i)
                    .arg(i <= 2 ? "fastest" : i <= 4 ? "balanced" : "quality"),
                    QString("p%1").arg(i));
            d->qualityStack->setCurrentIndex(0);  // bitrate panel
        } else {
            for (const QString p : {"ultrafast","superfast","veryfast","faster",
                                    "fast","medium","slow","slower","veryslow"})
                d->presetCombo->addItem(p, p);
            d->qualityStack->setCurrentIndex(1);  // CRF panel
        }
        d->presetCombo->setCurrentText(m_settings.preset);
        d->presetCombo->blockSignals(false);
        emit settings_changed();
    };

    connect(d->codecCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, refresh_codec);
    refresh_codec(d->codecCombo->currentIndex());  // initialise

    connect(d->presetCombo, &QComboBox::currentTextChanged, this, [this](const QString& v){
        m_settings.preset = v; emit settings_changed();
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
    discoverBtn->setToolTip("Scan for physically-connected Basler cameras and add a card "
                            "for each one found, with its serial number already filled in.");
    connect(discoverBtn, &QPushButton::clicked, this, &VideoSettingsW::discover_cameras);
    headerRow->addWidget(discoverBtn);
    d->discoverBtn = discoverBtn;

    auto* addBtn = new QPushButton("+ Add camera");
    addBtn->setFixedHeight(26);
    connect(addBtn, &QPushButton::clicked, this, [this]{
        add_camera({});  // position-based label is set by the card itself
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
    int  added   = 0;
    bool atCap   = false;
    for (const auto& cam : found) {
        if (cam.serialNumber.isEmpty()) { continue; }
        const bool alreadyPresent = std::any_of(
            m_settings.cameras.cbegin(), m_settings.cameras.cend(),
            [&](const CameraParameters& existing) {
                return existing.serialNumber == cam.serialNumber;
            });
        if (alreadyPresent) { continue; }

        // Refuse past kMaxCameras rather than push_back()ing past the
        // capacity every already-open VideoGrabber/CameraCardW's
        // CameraParameters& reference depends on staying stable — see
        // VideoSettings::kMaxCameras's doc comment (settings.hpp).
        if (m_settings.cameras.size() >= static_cast<size_t>(VideoSettings::kMaxCameras)) {
            log_error(QString("[VideoSettingsW] discover_cameras: already at the %1-camera "
                               "limit — not adding any more.").arg(VideoSettings::kMaxCameras));
            atCap = true;
            break;
        }

        CameraParameters params;
        params.serialNumber = cam.serialNumber;
        if (!cam.modelName.isEmpty()) { params.friendlyName = cam.modelName; }
        m_settings.cameras.push_back(std::move(params));
        make_card(static_cast<int>(m_settings.cameras.size()) - 1);
        ++added;
    }

    if (added > 0) {
        emit settings_changed();
        emit cameras_list_changed();
    }

    if (d->discoverStatusLbl) {
        d->discoverStatusLbl->setText(found.isEmpty()
            ? "No cameras found — check network cabling/power, or that this build was "
              "compiled with camera support."
            : QString("Found %1 camera(s); added %2 new card(s) (%3 already configured).%4")
                  .arg(found.size()).arg(added).arg(found.size() - added)
                  .arg(atCap ? QString(" Reached the %1-camera limit.")
                                   .arg(VideoSettings::kMaxCameras)
                             : QString()));
        d->discoverStatusLbl->show();
    }
}

void VideoSettingsW::make_card(int index) {
    auto* card = new CameraCardW(m_settings.cameras[index], index, this);
    connect(card, &CameraCardW::params_changed,   this, &VideoSettingsW::settings_changed);
    connect(card, &CameraCardW::params_changed,   this, [this, index]{
        emit camera_params_changed(index);
    });
    connect(card, &CameraCardW::remove_requested, this, &VideoSettingsW::remove_camera);
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
                           "refusing to add another.").arg(VideoSettings::kMaxCameras));
        return;
    }
    m_settings.cameras.push_back(std::move(params));
    make_card(static_cast<int>(m_settings.cameras.size()) - 1);
    emit settings_changed();
    emit cameras_list_changed();
}

void VideoSettingsW::remove_camera(int index) {
    if (index < 0 || index >= d->cards.size()) return;

    // The card at `index` emitted remove_requested, which called us via a
    // DirectConnection — it is still on the call stack.  Deleting it now would
    // destroy the object mid-signal-emission and crash.  Use deleteLater() so
    // Qt defers the destruction until after the call stack unwinds.
    d->camerasLayout->removeWidget(d->cards[index]);
    d->cards[index]->deleteLater();

    // Cards AFTER `index` are not on the call stack, but their CameraParameters&
    // refs become stale after the vector erase below.  Delete them immediately
    // so no queued paint/update event can dereference the dangling refs.
    for (int i = d->cards.size() - 1; i > index; --i) {
        d->camerasLayout->removeWidget(d->cards[i]);
        delete d->cards[i];
    }
    d->cards.resize(index);

    m_settings.cameras.erase(m_settings.cameras.begin() + index);

    // Recreate cards for every camera that follows the removed slot.
    for (int i = index; i < static_cast<int>(m_settings.cameras.size()); ++i)
        make_card(i);

    emit settings_changed();
    emit cameras_list_changed();
}

} // namespace mosaic
