#include "ui/record/record_settings_w.hpp"

#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include "utils/timestamp.hpp"

namespace mosaic {

struct RecordSettingsW::Impl {
    // Directory
    QLineEdit* dirEdit = nullptr;

    // Channels
    QCheckBox* videoChk   = nullptr;
    QCheckBox* audioChk   = nullptr;
    QCheckBox* triggerChk = nullptr;

    // Starting a recording
    QSpinBox* delaySpin        = nullptr;
    QCheckBox* hidePreviewsChk = nullptr;

    // Naming
    QLineEdit* videoBase   = nullptr;
    QLineEdit* audioBase   = nullptr;
    QLineEdit* triggerBase = nullptr;
    QLineEdit* separator   = nullptr;
    QCheckBox* tsChk       = nullptr;
    QLineEdit* tsFormat    = nullptr;

    // Preview
    QTextEdit* preview = nullptr;
};

RecordSettingsW::RecordSettingsW(RecordSettings& settings, bool isAdmin, QWidget* parent)
    : QWidget(parent), m_settings(settings), m_isAdmin(isAdmin), d(std::make_unique<Impl>()) {
    auto* outerLay = new QVBoxLayout(this);
    outerLay->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content    = new QWidget;
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(10, 10, 10, 10);
    contentLay->setSpacing(10);

    build_directory_section(contentLay);
    build_channels_section(contentLay);
    build_start_section(contentLay);
    build_naming_section(contentLay);
    build_preview_section(contentLay);
    contentLay->addStretch();

    scroll->setWidget(content);
    outerLay->addWidget(scroll);

    refresh_preview();
}

RecordSettingsW::~RecordSettingsW() = default;

// ── Directory ──────────────────────────────────────────────────────────────

void RecordSettingsW::build_directory_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Output Directory");
    auto* lay = new QVBoxLayout(box);
    lay->setSpacing(6);

    // Path row: text field + browse button
    auto* pathRow = new QHBoxLayout;
    d->dirEdit    = new QLineEdit(m_settings.directory);
    d->dirEdit->setPlaceholderText("./recordings");
    pathRow->addWidget(d->dirEdit, 1);

    auto* browseBtn = new QToolButton;
    browseBtn->setText("📁");
    browseBtn->setToolTip("Choose directory…");
    browseBtn->setStyleSheet(
        "QToolButton { font-size: 14px; padding: 2px 6px;"
        " background: #1a1a3a; border: 1px solid #353565;"
        " border-radius: 4px; }"
        "QToolButton:hover { background: #22224a; }");
    connect(browseBtn, &QToolButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, "Select recording directory",
                                                              m_settings.directory);
        if (!dir.isEmpty()) {
            m_settings.directory = dir;
            d->dirEdit->setText(dir);
            refresh_preview();
            emit settings_changed();
        }
    });
    pathRow->addWidget(browseBtn);
    lay->addLayout(pathRow);

    // Per-user recording access control (item 27): a non-admin's recording
    // directory is the actual access boundary, not just a default — freely
    // allowing it to be retargeted (e.g. pointed at another user's folder)
    // would silently defeat that boundary. Read-only (not merely disabled)
    // so the current path stays visible/selectable/copyable, just not
    // editable; the browse button is disabled outright since there's
    // nothing meaningful to browse to. Admins keep full editing.
    if (!m_isAdmin) {
        d->dirEdit->setReadOnly(true);
        d->dirEdit->setToolTip(
            "This is your own recording folder and can't be changed — "
            "per-user recordings are kept separate. An admin can adjust "
            "this if needed.");
        browseBtn->setEnabled(false);
        browseBtn->setToolTip(d->dirEdit->toolTip());
    }

    // Open directory shortcut
    auto* openBtn = new QPushButton("Open recordings folder");
    openBtn->setProperty("flat", true);
    connect(openBtn, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_settings.directory)); });
    lay->addWidget(openBtn, 0, Qt::AlignLeft);

    connect(d->dirEdit, &QLineEdit::textEdited, this, [this](const QString& v) {
        m_settings.directory = v;
        refresh_preview();
        emit settings_changed();
    });

    parent->addWidget(box);
}

// ── Channels ───────────────────────────────────────────────────────────────

void RecordSettingsW::build_channels_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Record Channels");
    auto* lay = new QVBoxLayout(box);
    lay->setSpacing(6);

    auto make_ch = [&](const QString& label, bool checked) {
        auto* ck = new QCheckBox(label);
        ck->setChecked(checked);
        lay->addWidget(ck);
        return ck;
    };

    d->videoChk   = make_ch("Video  (camera files)", m_settings.enableVideo);
    d->audioChk   = make_ch("Audio  (microphone files)", m_settings.enableAudio);
    d->triggerChk = make_ch("Triggers  (event log file)", m_settings.enableTrigger);

    connect(d->videoChk, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.enableVideo = v;
        refresh_preview();
        emit settings_changed();
    });
    connect(d->audioChk, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.enableAudio = v;
        refresh_preview();
        emit settings_changed();
    });
    connect(d->triggerChk, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.enableTrigger = v;
        refresh_preview();
        emit settings_changed();
    });

    parent->addWidget(box);
}

// ── Starting a recording ───────────────────────────────────────────────────

void RecordSettingsW::build_start_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Starting a Recording");
    auto* lay = new QVBoxLayout(box);
    lay->setSpacing(6);

    auto* delayRow   = new QHBoxLayout;
    auto* delayLabel = new QLabel("Countdown:");
    delayLabel->setFixedWidth(110);
    delayLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    d->delaySpin = new QSpinBox;
    d->delaySpin->setRange(0, RecordSettings::kMaxStartDelaySec);
    d->delaySpin->setSuffix(" s");
    d->delaySpin->setSpecialValueText("Off (start immediately)");
    d->delaySpin->setValue(m_settings.startDelaySec);
    // setValue() clamps to the range but doesn't write back, so a hand-edited
    // settings.json holding an out-of-range value would leave the spinbox and
    // the stored setting disagreeing. Take the clamped value as authoritative.
    m_settings.startDelaySec = d->delaySpin->value();
    d->delaySpin->setToolTip(
        "Seconds to count down on the monitor after clicking Record, before capture "
        "actually begins — so the subject and experimenter aren't still looking at the "
        "screen at t=0. The delay happens entirely before recording starts, so session "
        "timestamps are unaffected. Click Record again during the countdown to cancel.");

    delayRow->addWidget(delayLabel);
    delayRow->addWidget(d->delaySpin);
    delayRow->addStretch();
    lay->addLayout(delayRow);

    d->hidePreviewsChk = new QCheckBox("Hide camera previews while recording");
    d->hidePreviewsChk->setChecked(m_settings.hidePreviewsWhileRecording);
    d->hidePreviewsChk->setToolTip(
        "Blanks the live camera grid in the monitor from the moment the countdown starts "
        "until recording stops, so the video doesn't distract the subject. A compact "
        "per-camera liveness strip replaces it; per-camera frame rates stay available in "
        "the Perf tab.");
    lay->addWidget(d->hidePreviewsChk);

    connect(d->delaySpin, &QSpinBox::valueChanged, this, [this](int v) {
        m_settings.startDelaySec = v;
        emit settings_changed();
    });
    connect(d->hidePreviewsChk, &QCheckBox::toggled, this, [this](bool v) {
        m_settings.hidePreviewsWhileRecording = v;
        emit settings_changed();
    });

    parent->addWidget(box);
}

// ── Naming ─────────────────────────────────────────────────────────────────

void RecordSettingsW::build_naming_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("File Naming");
    auto* lay = new QVBoxLayout(box);
    lay->setSpacing(8);

    // Helper: labelled text row
    auto make_row = [&](const QString& lbl, QLineEdit* edit) {
        auto* row = new QHBoxLayout;
        auto* l   = new QLabel(lbl);
        l->setFixedWidth(110);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(l);
        row->addWidget(edit, 1);
        lay->addLayout(row);
    };

    d->videoBase   = new QLineEdit(m_settings.videoBasename);
    d->audioBase   = new QLineEdit(m_settings.audioBasename);
    d->triggerBase = new QLineEdit(m_settings.triggerBasename);
    d->separator   = new QLineEdit(m_settings.separator);
    d->separator->setMaximumWidth(50);

    make_row("Video name:", d->videoBase);
    make_row("Audio name:", d->audioBase);
    make_row("Trigger name:", d->triggerBase);

    auto* sepRow = new QHBoxLayout;
    auto* sepLbl = new QLabel("Separator:");
    sepLbl->setFixedWidth(110);
    sepLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sepRow->addWidget(sepLbl);
    sepRow->addWidget(d->separator);
    sepRow->addStretch();
    lay->addLayout(sepRow);

    // Timestamp line
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    lay->addWidget(line);

    d->tsChk = new QCheckBox("Add timestamp to filename");
    d->tsChk->setChecked(m_settings.addTimestamp);
    lay->addWidget(d->tsChk);

    auto* fmtRow = new QHBoxLayout;
    auto* fmtLbl = new QLabel("Format:");
    fmtLbl->setFixedWidth(110);
    fmtLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    d->tsFormat = new QLineEdit(m_settings.timestampFormat);
    d->tsFormat->setEnabled(m_settings.addTimestamp);
    fmtRow->addWidget(fmtLbl);
    fmtRow->addWidget(d->tsFormat, 1);
    lay->addLayout(fmtRow);

    // Format hint
    auto* hint = new QLabel(
        "Qt date format: yyyy=year  MM=month  dd=day  "
        "hh=hour  mm=min  ss=sec");
    hint->setWordWrap(true);
    hint->setProperty("role", "muted");
    lay->addWidget(hint);

    // Wire everything
    auto refresh = [this] {
        refresh_preview();
        emit settings_changed();
    };
    connect(d->videoBase, &QLineEdit::textEdited, this, [this, refresh](const QString& v) {
        m_settings.videoBasename = v;
        refresh();
    });
    connect(d->audioBase, &QLineEdit::textEdited, this, [this, refresh](const QString& v) {
        m_settings.audioBasename = v;
        refresh();
    });
    connect(d->triggerBase, &QLineEdit::textEdited, this, [this, refresh](const QString& v) {
        m_settings.triggerBasename = v;
        refresh();
    });
    connect(d->separator, &QLineEdit::textEdited, this, [this, refresh](const QString& v) {
        m_settings.separator = v;
        refresh();
    });
    connect(d->tsChk, &QCheckBox::toggled, this, [this, refresh](bool v) {
        m_settings.addTimestamp = v;
        d->tsFormat->setEnabled(v);
        refresh();
    });
    connect(d->tsFormat, &QLineEdit::textEdited, this, [this, refresh](const QString& v) {
        m_settings.timestampFormat = v;
        refresh();
    });

    parent->addWidget(box);
}

// ── Preview ────────────────────────────────────────────────────────────────

void RecordSettingsW::build_preview_section(QVBoxLayout* parent) {
    auto* box = new QGroupBox("Generated Filenames  (example)");
    auto* lay = new QVBoxLayout(box);

    d->preview = new QTextEdit;
    d->preview->setReadOnly(true);
    d->preview->setMaximumHeight(110);
    d->preview->setStyleSheet(
        "QTextEdit { background: #0a0a17; color: #44aa66; font-family: monospace;"
        " font-size: 11px; border: 1px solid #1e1e3a; border-radius: 4px; padding: 4px; }");
    lay->addWidget(d->preview);

    parent->addWidget(box);
}

void RecordSettingsW::refresh_preview() {
    const QString ts = m_settings.addTimestamp
                           ? m_settings.separator +
                                 QDateTime::currentDateTime().toString(m_settings.timestampFormat)
                           : QString();

    const QString base = m_settings.directory.isEmpty() ? "./recordings" : m_settings.directory;

    QStringList lines;
    lines << "Session folder:  " + base + "/";

    if (m_settings.enableVideo) lines << "  " + m_settings.videoBasename + ts + ".mp4";
    if (m_settings.enableAudio) lines << "  " + m_settings.audioBasename + ts + ".wav";
    if (m_settings.enableTrigger) lines << "  " + m_settings.triggerBasename + ts + ".csv";

    if (lines.size() == 1) lines << "  (no channels enabled)";

    d->preview->setPlainText(lines.join("\n"));
}

} // namespace mosaic
