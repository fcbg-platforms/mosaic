#include "ui/video/video_settings_w.hpp"
#include "ui/video/camera_card_w.hpp"
#include <QCheckBox>
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
    QSpinBox*      targetFpsSpin = nullptr;
    QCheckBox*     syncCk        = nullptr;

    // Camera list
    QVBoxLayout*   camerasLayout = nullptr;
    QVector<CameraCardW*> cards;
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
    // references held by existing cards valid for the lifetime of this widget.
    m_settings.cameras.reserve(32);

    // Create cards for cameras already in settings WITHOUT pushing them again.
    for (int i = 0; i < static_cast<int>(m_settings.cameras.size()); ++i)
        make_card(i);
}

VideoSettingsW::~VideoSettingsW() = default;

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
            for (const QString& p : {"ultrafast","superfast","veryfast","faster",
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

    // ── FPS synchronisation ──────────────────────────────────────────────
    auto* line = new QFrame; line->setFrameShape(QFrame::HLine);
    form->addWidget(line);

    auto* syncRow = new QHBoxLayout;
    d->syncCk = new QCheckBox("Synchronise camera frame rates");
    d->syncCk->setChecked(m_settings.syncFps);
    syncRow->addWidget(d->syncCk);
    form->addLayout(syncRow);

    auto* fpsRow = new QHBoxLayout;
    auto* fpsLbl = new QLabel("Target FPS:");
    fpsLbl->setFixedWidth(90);
    fpsLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    d->targetFpsSpin = new QSpinBox;
    d->targetFpsSpin->setRange(1, 200);
    d->targetFpsSpin->setValue(m_settings.targetFps);
    d->targetFpsSpin->setSuffix("  fps");
    d->targetFpsSpin->setEnabled(m_settings.syncFps);
    fpsRow->addWidget(fpsLbl);
    fpsRow->addWidget(d->targetFpsSpin);
    fpsRow->addStretch();
    form->addLayout(fpsRow);

    connect(d->syncCk, &QCheckBox::toggled, this, [this](bool v){
        m_settings.syncFps = v;
        d->targetFpsSpin->setEnabled(v);
        emit settings_changed();
    });
    connect(d->targetFpsSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v){
        m_settings.targetFps = v; emit settings_changed();
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

    auto* addBtn = new QPushButton("+ Add camera");
    addBtn->setFixedHeight(26);
    connect(addBtn, &QPushButton::clicked, this, [this]{
        add_camera({});  // position-based label is set by the card itself
    });
    headerRow->addWidget(addBtn);

    parent->addLayout(headerRow);

    // ── Camera cards list ─────────────────────────────────────────────────
    auto* cardsWidget = new QWidget;
    d->camerasLayout  = new QVBoxLayout(cardsWidget);
    d->camerasLayout->setContentsMargins(0, 0, 0, 0);
    d->camerasLayout->setSpacing(6);

    parent->addWidget(cardsWidget);
}

void VideoSettingsW::make_card(int index) {
    auto* card = new CameraCardW(m_settings.cameras[index], index, this);
    connect(card, &CameraCardW::params_changed,   this, &VideoSettingsW::settings_changed);
    connect(card, &CameraCardW::remove_requested, this, &VideoSettingsW::remove_camera);
    d->camerasLayout->addWidget(card);
    d->cards.append(card);
}

void VideoSettingsW::add_camera(CameraParameters params) {
    m_settings.cameras.push_back(std::move(params));
    make_card(static_cast<int>(m_settings.cameras.size()) - 1);
    emit settings_changed();
}

void VideoSettingsW::remove_camera(int index) {
    if (index < 0 || index >= d->cards.size()) return;

    auto* card = d->cards[index];
    d->camerasLayout->removeWidget(card);
    card->deleteLater();
    d->cards.remove(index);
    m_settings.cameras.erase(m_settings.cameras.begin() + index);

    // Renumber remaining cards so they always show their actual position.
    for (int i = index; i < d->cards.size(); ++i)
        d->cards[i]->set_index(i);

    emit settings_changed();
}

} // namespace mosaic
