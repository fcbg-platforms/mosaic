#include "ui/audio/audio_settings_w.hpp"
#include "ui/audio/audio_waveform_w.hpp"
#include "ui/audio/microphone_card_w.hpp"
#include <QComboBox>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVector>

namespace mosaic {

struct AudioSettingsW::Impl {
    AudioManager*   audioMgr     = nullptr;
    QComboBox*      codecCombo   = nullptr;
    QVBoxLayout*    microphonesLay = nullptr;
    AudioWaveformW* waveform     = nullptr;
    QVector<MicrophoneCardW*> cards;
};

AudioSettingsW::AudioSettingsW(AudioSettings& settings,
                                AudioManager* audioMgr,
                                QWidget* parent)
    : QWidget(parent), m_settings(settings), d(std::make_unique<Impl>())
{
    d->audioMgr = audioMgr;

    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);
    outerLay->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content    = new QWidget;
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(10, 10, 10, 10);
    contentLay->setSpacing(10);

    build_encoding_section(contentLay);
    build_microphones_section(contentLay);
    build_waveform_section(contentLay);
    contentLay->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);

    m_settings.microphones.reserve(16);

    // Create cards for existing microphones WITHOUT pushing them again.
    for (int i = 0; i < static_cast<int>(m_settings.microphones.size()); ++i) {
        auto* card = new MicrophoneCardW(m_settings.microphones[i], i, this);
        connect(card, &MicrophoneCardW::params_changed,   this, &AudioSettingsW::settings_changed);
        connect(card, &MicrophoneCardW::remove_requested, this, &AudioSettingsW::remove_microphone);
        d->microphonesLay->addWidget(card);
        d->cards.append(card);
    }

    // Route AudioManager level signals to mic cards AND the waveform widget.
    if (d->audioMgr) {
        connect(d->audioMgr, &AudioManager::level_rms_changed, this,
                [this](int micIndex, float rms) {
            if (micIndex >= 0 && micIndex < d->cards.size()) {
                d->cards[micIndex]->set_level(rms);
            }
            if (d->waveform) {
                d->waveform->push_level(micIndex, rms);
            }
        }, Qt::QueuedConnection);
    }
}

AudioSettingsW::~AudioSettingsW() = default;

// ── Encoding section ───────────────────────────────────────────────────────

void AudioSettingsW::build_encoding_section(QVBoxLayout* parent) {
    auto* box  = new QGroupBox("Encoding");
    auto* form = new QVBoxLayout(box);
    form->setSpacing(8);

    auto* row = new QHBoxLayout;
    auto* lbl = new QLabel("Codec:");
    lbl->setFixedWidth(80);
    lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    d->codecCombo = new QComboBox;
    d->codecCombo->addItem("PCM 16-bit  (lossless, large)",  "pcm_s16le");
    d->codecCombo->addItem("PCM 24-bit  (lossless, large)",  "pcm_s24le");
    d->codecCombo->addItem("FLAC        (lossless, small)",   "flac");
    d->codecCombo->addItem("AAC         (lossy, very small)", "aac");
    d->codecCombo->setCurrentIndex(
        d->codecCombo->findData(m_settings.codec));

    row->addWidget(lbl);
    row->addWidget(d->codecCombo, 1);
    form->addLayout(row);

    // Recommendation hint
    auto* hint = new QLabel("Tip: PCM 16-bit is recommended for research — "
                             "no quality loss, compatible with all analysis tools.");
    hint->setWordWrap(true);
    hint->setProperty("role", "muted");
    form->addWidget(hint);

    connect(d->codecCombo, &QComboBox::currentIndexChanged, this, [this](int idx){
        m_settings.codec = d->codecCombo->itemData(idx).toString();
        emit settings_changed();
    });

    parent->addWidget(box);
}

// ── Microphones section ────────────────────────────────────────────────────

void AudioSettingsW::build_microphones_section(QVBoxLayout* parent) {
    auto* headerRow = new QHBoxLayout;

    auto* titleLbl = new QLabel("Microphones");
    titleLbl->setProperty("role", "section");
    headerRow->addWidget(titleLbl);
    headerRow->addStretch();

    auto* addBtn = new QPushButton("+ Add microphone");
    addBtn->setFixedHeight(26);
    connect(addBtn, &QPushButton::clicked, this, [this]{
        add_microphone({});  // position-based label is set by the card itself
    });
    headerRow->addWidget(addBtn);

    parent->addLayout(headerRow);

    auto* cardsWidget = new QWidget;
    d->microphonesLay = new QVBoxLayout(cardsWidget);
    d->microphonesLay->setContentsMargins(0, 0, 0, 0);
    d->microphonesLay->setSpacing(6);
    parent->addWidget(cardsWidget);
}

void AudioSettingsW::add_microphone(MicrophoneParameters params) {
    m_settings.microphones.push_back(std::move(params));
    const int idx = static_cast<int>(m_settings.microphones.size()) - 1;

    auto* card = new MicrophoneCardW(m_settings.microphones[idx], idx, this);
    connect(card, &MicrophoneCardW::params_changed,   this, &AudioSettingsW::settings_changed);
    connect(card, &MicrophoneCardW::remove_requested, this, &AudioSettingsW::remove_microphone);

    d->microphonesLay->addWidget(card);
    d->cards.append(card);

    if (d->waveform) { d->waveform->set_channel_count(d->cards.size()); }
    emit settings_changed();
}

void AudioSettingsW::remove_microphone(int index) {
    if (index < 0 || index >= d->cards.size()) { return; }

    auto* card = d->cards[index];
    d->microphonesLay->removeWidget(card);
    card->deleteLater();
    d->cards.remove(index);
    m_settings.microphones.erase(m_settings.microphones.begin() + index);

    for (int i = index; i < d->cards.size(); ++i) {
        d->cards[i]->set_index(i);
    }

    if (d->waveform) { d->waveform->set_channel_count(qMax(1, d->cards.size())); }
    emit settings_changed();
}

// ── Waveform section ───────────────────────────────────────────────────────

void AudioSettingsW::build_waveform_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Live Waveform Monitor");
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(4, 6, 4, 6);

    d->waveform = new AudioWaveformW;
    d->waveform->set_channel_count(
        qMax(1, static_cast<int>(m_settings.microphones.size())));

    auto* hint = new QLabel("Rolling 8-second RMS level per channel.  "
                             "Levels update during recording.");
    hint->setWordWrap(true);
    hint->setProperty("role", "muted");

    lay->addWidget(d->waveform);
    lay->addWidget(hint);
    parent->addWidget(box);
}

} // namespace mosaic
