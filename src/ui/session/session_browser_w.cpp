#include "ui/session/session_browser_w.hpp"
#include "ui/session/session_player_w.hpp"
#include "session/session_info.hpp"
#include <QAbstractScrollArea>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QTime>
#include <QTimeEdit>
#include <QCoreApplication>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

namespace mosaic {

// ── SessionRow — one entry in the left-panel list ─────────────────────────

class SessionRow : public QWidget {
public:
    using ClickCb = std::function<void(const QString&)>;

    explicit SessionRow(const SessionInfo& info,
                        ClickCb            onClick,
                        QWidget*           parent = nullptr)
        : QWidget(parent), m_info(info), m_onClick(std::move(onClick))
    {
        setFixedHeight(80);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }

    void set_selected(bool s) {
        if (m_selected == s) { return; }
        m_selected = s;
        update();
    }

    const QString& session_path() const { return m_info.path; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QColor bg = m_selected ? QColor("#181838")
                        : m_hovered  ? QColor("#0f0f28")
                                     : QColor("#0a0a22");
        p.fillRect(rect(), bg);

        if (m_selected) {
            p.fillRect(0, 0, 3, height(), QColor("#5580ff"));
        }

        // Date / time
        QFont titleFont;
        titleFont.setPixelSize(13);
        titleFont.setBold(true);
        p.setFont(titleFont);
        p.setPen(QColor("#d0d0f0"));

        const QString dateStr = m_info.startUtc.isValid()
            ? m_info.startUtc.toLocalTime().toString("yyyy-MM-dd   hh:mm:ss")
            : m_info.name;
        p.drawText(QRect(14, 10, width() - 18, 18), Qt::AlignLeft | Qt::AlignVCenter, dateStr);

        // Sub-line: @user · cameras · duration
        QFont subFont;
        subFont.setPixelSize(11);
        p.setFont(subFont);
        p.setPen(QColor("#5858a0"));

        QStringList subs;
        if (!m_info.recordedBy.isEmpty()) { subs << ("@" + m_info.recordedBy); }
        if (m_info.cameraCount > 0) { subs << (QString::number(m_info.cameraCount) + " cam"); }
        if (m_info.durationMs > 0)  { subs << m_info.format_duration(); }
        p.drawText(QRect(14, 32, width() - 18, 15),
                   Qt::AlignLeft | Qt::AlignVCenter, subs.join(" · "));

        // Analysis badges
        int bx = 14;
        const int by = 53;
        const int bh = 15;

        auto drawBadge = [&](const QString& label, QColor bgc, QColor fgc) {
            QFont bf;
            bf.setPixelSize(9);
            bf.setBold(true);
            p.setFont(bf);
            const int tw = p.fontMetrics().horizontalAdvance(label) + 10;
            const QRect br(bx, by, tw, bh);
            p.setBrush(bgc);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(br, 3, 3);
            p.setPen(fgc);
            p.drawText(br, Qt::AlignCenter, label);
            bx += tw + 4;
        };

        if (m_info.hasPoseAnalysis) {
            drawBadge("POSE",   QColor("#0a2a0a"), QColor("#44cc44"));
        }
        if (m_info.hasMotionAnalysis) {
            drawBadge("MOTION", QColor("#0a0a2a"), QColor("#4488ff"));
        }
        if (!m_info.annotations.isEmpty()) {
            drawBadge(QString("%1 notes").arg(m_info.annotations.size()),
                      QColor("#1a100a"), QColor("#cc8844"));
        }

        // Divider
        p.setPen(QColor("#10102a"));
        p.drawLine(0, height() - 1, width(), height() - 1);
    }

    void mousePressEvent(QMouseEvent*) override {
        if (m_onClick) { m_onClick(m_info.path); }
    }

    void enterEvent(QEnterEvent*) override { m_hovered = true;  update(); }
    void leaveEvent(QEvent*)      override { m_hovered = false; update(); }

private:
    SessionInfo m_info;
    ClickCb     m_onClick;
    bool        m_selected = false;
    bool        m_hovered  = false;
};

// ── Impl ──────────────────────────────────────────────────────────────────

struct SessionBrowserW::Impl {
    QString           recordsDir;
    AnalysisManager*  analysisMgr = nullptr;

    QList<SessionInfo>  sessions;
    int                 currentIndex = -1;

    // Panels stored so the constructor can give them to the splitter
    QWidget*     leftPanelWidget  = nullptr;
    QWidget*     rightPanelWidget = nullptr;

    // Left panel
    QLabel*      countLbl      = nullptr;
    QLineEdit*   searchBar     = nullptr;
    QWidget*     rowContainer  = nullptr;
    QVBoxLayout* rowLayout     = nullptr;
    QList<SessionRow*> rows;

    // Right panel
    QStackedWidget* rightStack  = nullptr;

    // Detail header
    QLabel*      nameLbl        = nullptr;
    QLabel*      metaLbl        = nullptr;
    QLabel*      filesLbl       = nullptr;
    QPushButton* playBtn        = nullptr;

    // Analysis section
    QPushButton* runPoseBtn     = nullptr;
    QPushButton* runMotionBtn   = nullptr;
    QLabel*      analysisFilesLbl = nullptr;
    QTextEdit*   analysisLog    = nullptr;
    QLabel*      analysisStatus = nullptr;

    // Annotation section
    QTableWidget* annotTable   = nullptr;
    QTimeEdit*    annotTime    = nullptr;
    QComboBox*    annotCat     = nullptr;
    QLineEdit*    annotNote    = nullptr;
    QPushButton*  addAnnotBtn  = nullptr;

    // Running analysis process
    QProcess*    proc          = nullptr;

    SessionInfo* current() {
        if (currentIndex < 0 || currentIndex >= sessions.size()) { return nullptr; }
        return &sessions[currentIndex];
    }
};

// ── Helpers ────────────────────────────────────────────────────────────────

static QLabel* section_header(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet(
        "QLabel { color: #4466cc; font-size: 11px; font-weight: bold;"
        "  border-bottom: 1px solid #1a1a44; padding-bottom: 3px;"
        "  margin-top: 8px; margin-bottom: 4px; }");
    return lbl;
}

static QLabel* chip_label(const QString& text,
                           const QString& bg = "#12122a",
                           const QString& fg = "#8888b0") {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet(
        QString("QLabel { background: %1; color: %2; border-radius: 4px;"
                " padding: 2px 8px; font-size: 11px; }").arg(bg, fg));
    return lbl;
}

// ── Constructor ────────────────────────────────────────────────────────────

SessionBrowserW::SessionBrowserW(const QString&   recordsDir,
                                  AnalysisManager* analysisMgr,
                                  QWidget*         parent)
    : QDialog(parent)
    , d(std::make_unique<Impl>())
{
    d->recordsDir  = recordsDir;
    d->analysisMgr = analysisMgr;

    setWindowTitle("Session Browser & Annotator");
    resize(1160, 730);
    setMinimumSize(900, 600);

    setStyleSheet(
        "SessionBrowserW, QDialog {"
        "  background: #070718; color: #c0c0e0; }"
        "QScrollArea, QAbstractScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: #0a0a20; width: 6px; }"
        "QScrollBar::handle:vertical { background: #303060; border-radius: 3px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QLineEdit { background: #10102a; border: 1px solid #252550;"
        "  border-radius: 4px; padding: 4px 8px; color: #c0c0e0; }"
        "QPushButton { background: #12123a; border: 1px solid #252555;"
        "  border-radius: 4px; color: #c0c0e0; padding: 4px 12px; }"
        "QPushButton:hover { background: #1a1a50; border-color: #4466cc; }"
        "QPushButton:pressed { background: #0a0a30; }"
        "QPushButton:checked { background: #1a2a5a; border-color: #5580ff; }"
        "QTableWidget { background: #0c0c22; gridline-color: #181838;"
        "  border: 1px solid #1a1a44; border-radius: 4px; }"
        "QHeaderView::section { background: #0f0f2a; color: #6666a0;"
        "  border: none; border-bottom: 1px solid #1a1a44; padding: 4px 8px;"
        "  font-size: 11px; }"
        "QComboBox { background: #10102a; border: 1px solid #252550;"
        "  border-radius: 4px; padding: 3px 8px; color: #c0c0e0; }"
        "QComboBox QAbstractItemView { background: #10102a; color: #c0c0e0;"
        "  selection-background-color: #1a2050; }"
        "QTextEdit { background: #0a0a1e; border: 1px solid #1a1a44;"
        "  border-radius: 4px; color: #8090b0; font-family: monospace;"
        "  font-size: 11px; }"
        "QTimeEdit { background: #10102a; border: 1px solid #252550;"
        "  border-radius: 4px; padding: 3px 8px; color: #c0c0e0; }"
    );

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Splitter holds left list + right detail ──────────────────────────
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(1);
    splitter->setStyleSheet("QSplitter::handle { background: #181838; }");

    build_left_panel();
    build_right_panel();

    splitter->addWidget(d->leftPanelWidget);
    splitter->addWidget(d->rightPanelWidget);
    splitter->setSizes({300, 860});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    root->addWidget(splitter);

    rebuild_session_list();
}

SessionBrowserW::~SessionBrowserW() {
    if (d->proc && d->proc->state() != QProcess::NotRunning) {
        d->proc->terminate();
        d->proc->waitForFinished(3000);
    }
}

// ── Left panel ─────────────────────────────────────────────────────────────

void SessionBrowserW::build_left_panel() {
    auto* leftWidget = new QWidget;
    d->leftPanelWidget = leftWidget;
    leftWidget->setFixedWidth(300);
    leftWidget->setStyleSheet("background: #08081e;");

    auto* vbox = new QVBoxLayout(leftWidget);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // ── Header ──────────────────────────────────────────────────────────
    auto* header = new QWidget;
    header->setStyleSheet("background: #0a0a28; border-bottom: 1px solid #151538;");
    auto* headerBox = new QVBoxLayout(header);
    headerBox->setContentsMargins(12, 10, 12, 10);
    headerBox->setSpacing(6);

    auto* titleRow = new QHBoxLayout;
    auto* titleLbl = new QLabel("Sessions");
    titleLbl->setStyleSheet("font-size: 14px; font-weight: bold; color: #d0d0ff;");
    d->countLbl = new QLabel("0");
    d->countLbl->setStyleSheet(
        "background: #12122a; border-radius: 8px; padding: 1px 7px;"
        "font-size: 11px; color: #6666a0;");
    titleRow->addWidget(titleLbl);
    titleRow->addStretch();
    titleRow->addWidget(d->countLbl);

    d->searchBar = new QLineEdit;
    d->searchBar->setPlaceholderText("Filter sessions…");
    d->searchBar->setClearButtonEnabled(true);
    connect(d->searchBar, &QLineEdit::textChanged,
            this, &SessionBrowserW::apply_filter);

    headerBox->addLayout(titleRow);
    headerBox->addWidget(d->searchBar);
    vbox->addWidget(header);

    // ── Scrollable session list ──────────────────────────────────────────
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    d->rowContainer = new QWidget;
    d->rowContainer->setStyleSheet("background: #08081e;");
    d->rowLayout = new QVBoxLayout(d->rowContainer);
    d->rowLayout->setContentsMargins(0, 0, 0, 0);
    d->rowLayout->setSpacing(0);
    d->rowLayout->addStretch();

    scroll->setWidget(d->rowContainer);
    vbox->addWidget(scroll, 1);

    // ── Footer buttons ───────────────────────────────────────────────────
    auto* footer = new QWidget;
    footer->setStyleSheet("background: #0a0a28; border-top: 1px solid #151538;");
    auto* footerBox = new QHBoxLayout(footer);
    footerBox->setContentsMargins(10, 8, 10, 8);
    footerBox->setSpacing(6);

    auto* openFolderBtn = new QPushButton("📁 Open folder");
    auto* refreshBtn    = new QPushButton("⟳ Refresh");
    footerBox->addWidget(openFolderBtn);
    footerBox->addStretch();
    footerBox->addWidget(refreshBtn);

    connect(openFolderBtn, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(d->recordsDir));
    });
    connect(refreshBtn, &QPushButton::clicked, this, &SessionBrowserW::rebuild_session_list);

    vbox->addWidget(footer);
}

// ── Right panel ────────────────────────────────────────────────────────────

void SessionBrowserW::build_right_panel() {
    auto* rightWidget = new QWidget;
    d->rightPanelWidget = rightWidget;
    rightWidget->setStyleSheet("background: #070718;");
    auto* rightBox = new QVBoxLayout(rightWidget);
    rightBox->setContentsMargins(0, 0, 0, 0);
    rightBox->setSpacing(0);

    d->rightStack = new QStackedWidget;

    // ── Page 0: no selection ────────────────────────────────────────────
    auto* noSelPage = new QWidget;
    auto* nsCtr = new QVBoxLayout(noSelPage);
    nsCtr->setAlignment(Qt::AlignCenter);
    auto* nsLbl = new QLabel("Select a session to view details");
    nsLbl->setStyleSheet("color: #3a3a70; font-size: 14px;");
    nsCtr->addWidget(nsLbl, 0, Qt::AlignCenter);
    d->rightStack->addWidget(noSelPage);

    // ── Page 1: session detail (scrollable) ─────────────────────────────
    auto* detailScroll = new QScrollArea;
    detailScroll->setWidgetResizable(true);
    detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* detailPage = new QWidget;
    auto* detailBox  = new QVBoxLayout(detailPage);
    detailBox->setContentsMargins(24, 20, 24, 24);
    detailBox->setSpacing(0);

    // ── Session title bar ────────────────────────────────────────────────
    auto* titleBar = new QHBoxLayout;
    d->nameLbl = new QLabel("—");
    d->nameLbl->setStyleSheet("font-size: 17px; font-weight: bold; color: #e0e0ff;");
    d->nameLbl->setWordWrap(true);

    auto* openBtn = new QPushButton("📁");
    openBtn->setFixedSize(30, 28);
    openBtn->setToolTip("Open session folder");
    connect(openBtn, &QPushButton::clicked, this, [this] {
        if (auto* info = d->current()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(info->path));
        }
    });

    d->playBtn = new QPushButton("▶  Play");
    d->playBtn->setFixedHeight(28);
    d->playBtn->setMinimumWidth(80);
    d->playBtn->setToolTip("Open synchronised multi-camera player");
    d->playBtn->setStyleSheet(
        "QPushButton { background: #0a1a2a; border: 1px solid #1a4a6a;"
        "  color: #44aaff; border-radius: 4px; padding: 3px 14px; font-weight: bold; }"
        "QPushButton:hover  { background: #0f2a3a; border-color: #44aaff; }"
        "QPushButton:pressed{ background: #071218; }");
    connect(d->playBtn, &QPushButton::clicked, this, [this] {
        auto* info = d->current();
        if (!info) { return; }
        auto* player = new SessionPlayerW(*info, this);
        player->setAttribute(Qt::WA_DeleteOnClose);
        player->show();
    });

    auto* closeBtn = new QPushButton("✕");
    closeBtn->setFixedSize(30, 28);
    closeBtn->setToolTip("Close detail");
    connect(closeBtn, &QPushButton::clicked, this, [this] {
        d->currentIndex = -1;
        for (auto* row : d->rows) { row->set_selected(false); }
        d->rightStack->setCurrentIndex(0);
    });

    titleBar->addWidget(d->nameLbl, 1);
    titleBar->addWidget(d->playBtn);
    titleBar->addWidget(openBtn);
    titleBar->addWidget(closeBtn);
    detailBox->addLayout(titleBar);

    // ── Overview metadata ────────────────────────────────────────────────
    d->metaLbl = new QLabel;
    d->metaLbl->setStyleSheet("color: #6060a0; font-size: 12px; margin: 6px 0 10px 0;");
    d->metaLbl->setWordWrap(true);
    detailBox->addWidget(d->metaLbl);

    // ── Chip row: cameras, mics, duration, codec ─────────────────────────
    // (populated in populate_detail via a stored layout pointer)
    auto* chipsRow = new QHBoxLayout;
    chipsRow->setSpacing(6);
    // Chips are rebuilt in populate_detail — we add a container widget
    auto* chipsWidget = new QWidget;
    chipsWidget->setObjectName("chipsWidget");
    auto* chipsInner  = new QHBoxLayout(chipsWidget);
    chipsInner->setContentsMargins(0, 0, 0, 0);
    chipsInner->setSpacing(6);
    chipsRow->addWidget(chipsWidget);
    chipsRow->addStretch();
    detailBox->addLayout(chipsRow);

    auto* sep1 = new QFrame;
    sep1->setFrameShape(QFrame::HLine);
    sep1->setStyleSheet("color: #181838; margin: 12px 0 4px 0;");
    detailBox->addWidget(sep1);

    // ── Files section ───────────────────────────────────────────────────
    detailBox->addWidget(section_header("Files"));
    d->filesLbl = new QLabel;
    d->filesLbl->setStyleSheet("color: #7070a0; font-size: 12px; font-family: monospace; margin-bottom: 6px;");
    d->filesLbl->setWordWrap(true);
    detailBox->addWidget(d->filesLbl);

    // ── Analysis section ────────────────────────────────────────────────
    auto* sep2 = new QFrame;
    sep2->setFrameShape(QFrame::HLine);
    sep2->setStyleSheet("color: #181838; margin: 6px 0 4px 0;");
    detailBox->addWidget(sep2);

    detailBox->addWidget(section_header("Analysis"));

    auto* analysisRow = new QHBoxLayout;
    d->runPoseBtn   = new QPushButton("▶  Run Pose (YOLOv8)");
    d->runMotionBtn = new QPushButton("▶  Run Motion (MOG2)");
    d->runPoseBtn->setStyleSheet(
        "QPushButton { background: #0a2a0a; border: 1px solid #1a4a1a;"
        "  color: #44cc44; border-radius: 4px; padding: 5px 14px; }"
        "QPushButton:hover { background: #0f3a0f; border-color: #44cc44; }"
        "QPushButton:disabled { color: #336633; border-color: #0f2a0f; }");
    d->runMotionBtn->setStyleSheet(
        "QPushButton { background: #0a0a2a; border: 1px solid #1a1a4a;"
        "  color: #4488ff; border-radius: 4px; padding: 5px 14px; }"
        "QPushButton:hover { background: #0f0f3a; border-color: #4488ff; }"
        "QPushButton:disabled { color: #334488; border-color: #0f0f2a; }");
    connect(d->runPoseBtn,   &QPushButton::clicked, this, &SessionBrowserW::run_pose_analysis);
    connect(d->runMotionBtn, &QPushButton::clicked, this, &SessionBrowserW::run_motion_analysis);
    analysisRow->addWidget(d->runPoseBtn);
    analysisRow->addWidget(d->runMotionBtn);
    analysisRow->addStretch();
    detailBox->addLayout(analysisRow);

    d->analysisFilesLbl = new QLabel;
    d->analysisFilesLbl->setStyleSheet("color: #446644; font-size: 11px;"
                                       " font-family: monospace; margin: 4px 0;");
    d->analysisFilesLbl->setWordWrap(true);
    detailBox->addWidget(d->analysisFilesLbl);

    d->analysisStatus = new QLabel;
    d->analysisStatus->setStyleSheet("color: #6688aa; font-size: 11px; margin: 2px 0;");
    detailBox->addWidget(d->analysisStatus);

    d->analysisLog = new QTextEdit;
    d->analysisLog->setReadOnly(true);
    d->analysisLog->setFixedHeight(90);
    d->analysisLog->hide();
    detailBox->addWidget(d->analysisLog);

    // ── Annotations section ─────────────────────────────────────────────
    auto* sep3 = new QFrame;
    sep3->setFrameShape(QFrame::HLine);
    sep3->setStyleSheet("color: #181838; margin: 12px 0 4px 0;");
    detailBox->addWidget(sep3);

    detailBox->addWidget(section_header("Annotations"));

    // Table
    d->annotTable = new QTableWidget(0, 4);
    d->annotTable->setHorizontalHeaderLabels({"Time", "Category", "Notes", ""});
    d->annotTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    d->annotTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    d->annotTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    d->annotTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    d->annotTable->horizontalHeader()->resizeSection(3, 34);
    d->annotTable->verticalHeader()->setVisible(false);
    d->annotTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->annotTable->setEditTriggers(QAbstractItemView::DoubleClicked);
    d->annotTable->setMinimumHeight(130);
    d->annotTable->setMaximumHeight(280);
    d->annotTable->setAlternatingRowColors(false);
    d->annotTable->setShowGrid(false);
    d->annotTable->setFrameShape(QFrame::NoFrame);
    d->annotTable->setStyleSheet(
        "QTableWidget { background: #0a0a20; border: 1px solid #181840; border-radius: 4px; }"
        "QTableWidget::item { padding: 3px 8px; border-bottom: 1px solid #12122a; }"
        "QTableWidget::item:selected { background: #1a1a44; }");
    detailBox->addWidget(d->annotTable);

    // Add-annotation form
    auto* addRow = new QHBoxLayout;
    addRow->setSpacing(6);

    d->annotTime = new QTimeEdit(QTime(0, 0, 0));
    d->annotTime->setDisplayFormat("HH:mm:ss");
    d->annotTime->setFixedWidth(90);
    d->annotTime->setToolTip("Annotation timestamp (HH:MM:SS from session start)");

    d->annotCat = new QComboBox;
    for (const auto& cat : AnnotationCategory::defaults()) {
        d->annotCat->addItem(cat.name);
    }
    d->annotCat->setFixedWidth(130);

    d->annotNote = new QLineEdit;
    d->annotNote->setPlaceholderText("Note (optional)…");
    connect(d->annotNote, &QLineEdit::returnPressed,
            this, &SessionBrowserW::add_annotation);

    d->addAnnotBtn = new QPushButton("+ Add");
    d->addAnnotBtn->setStyleSheet(
        "QPushButton { background: #0a1a3a; border: 1px solid #1a2a5a;"
        "  color: #5588ff; border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background: #0f2050; border-color: #5588ff; }");
    connect(d->addAnnotBtn, &QPushButton::clicked,
            this, &SessionBrowserW::add_annotation);

    addRow->addWidget(new QLabel("Time:"));
    addRow->addWidget(d->annotTime);
    addRow->addWidget(new QLabel("Category:"));
    addRow->addWidget(d->annotCat);
    addRow->addWidget(d->annotNote, 1);
    addRow->addWidget(d->addAnnotBtn);
    detailBox->addLayout(addRow);

    // Bottom action buttons
    auto* actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 10, 0, 0);

    auto* saveBtn   = new QPushButton("💾  Save annotations");
    auto* exportBtn = new QPushButton("📄  Export CSV");
    connect(saveBtn,   &QPushButton::clicked, this, &SessionBrowserW::save_annotations);
    connect(exportBtn, &QPushButton::clicked, this, &SessionBrowserW::export_annot_csv);
    actionRow->addWidget(saveBtn);
    actionRow->addWidget(exportBtn);
    actionRow->addStretch();

    auto* closeAllBtn = new QPushButton("Close");
    connect(closeAllBtn, &QPushButton::clicked, this, &QDialog::accept);
    actionRow->addWidget(closeAllBtn);
    detailBox->addLayout(actionRow);

    detailBox->addStretch();
    detailScroll->setWidget(detailPage);
    d->rightStack->addWidget(detailScroll);

    rightBox->addWidget(d->rightStack);
}

// ── Session list management ────────────────────────────────────────────────

void SessionBrowserW::rebuild_session_list() {
    d->sessions = SessionInfo::list_all(d->recordsDir);

    // Remove old rows
    for (auto* row : d->rows) { row->deleteLater(); }
    d->rows.clear();

    // Remove stretch
    while (d->rowLayout->count() > 0) {
        auto* item = d->rowLayout->takeAt(0);
        if (item->widget()) { item->widget()->hide(); }
        delete item;
    }

    const QString filter = d->searchBar ? d->searchBar->text().trimmed().toLower() : QString();

    for (int i = 0; i < d->sessions.size(); ++i) {
        const auto& info = d->sessions[i];
        if (!filter.isEmpty()) {
            const QString hay = (info.name + info.recordedBy).toLower();
            if (!hay.contains(filter)) { continue; }
        }

        auto* row = new SessionRow(info, [this](const QString& path) {
            select_session(path);
        }, d->rowContainer);

        d->rowLayout->addWidget(row);
        d->rows.append(row);
    }

    d->rowLayout->addStretch();

    if (d->countLbl) {
        d->countLbl->setText(QString::number(d->sessions.size()));
    }

    // Re-select current if still present
    if (d->currentIndex >= 0 && d->currentIndex < d->sessions.size()) {
        const QString path = d->sessions[d->currentIndex].path;
        for (auto* row : d->rows) {
            if (row->session_path() == path) { row->set_selected(true); }
        }
    } else {
        d->currentIndex = -1;
        if (d->rightStack) { d->rightStack->setCurrentIndex(0); }
    }
}

void SessionBrowserW::apply_filter(const QString& text) {
    const QString lower = text.trimmed().toLower();
    for (auto* row : d->rows) {
        const bool show = lower.isEmpty()
            || row->session_path().toLower().contains(lower);
        row->setVisible(show);
    }
}

// ── Session selection & detail ─────────────────────────────────────────────

void SessionBrowserW::select_session(const QString& path) {
    // Find in sessions list
    int idx = -1;
    for (int i = 0; i < d->sessions.size(); ++i) {
        if (d->sessions[i].path == path) { idx = i; break; }
    }
    if (idx < 0) { return; }

    d->currentIndex = idx;
    for (auto* row : d->rows) {
        row->set_selected(row->session_path() == path);
    }

    // Reload annotations from disk in case they changed
    d->sessions[idx].load_annotations();
    populate_detail();
    d->rightStack->setCurrentIndex(1);
}

void SessionBrowserW::populate_detail() {
    auto* info = d->current();
    if (!info) { return; }

    // Title
    d->nameLbl->setText(info->startUtc.isValid()
        ? info->startUtc.toLocalTime().toString("yyyy-MM-dd  hh:mm:ss")
        : info->name);

    // Meta line
    QStringList meta;
    if (!info->recordedBy.isEmpty()) { meta << ("Recorded by @" + info->recordedBy); }
    if (!info->mosaicVersion.isEmpty()) { meta << ("MOSAIC " + info->mosaicVersion); }
    if (!info->videoCodec.isEmpty() || !info->audioCodec.isEmpty()) {
        meta << (info->videoCodec + "/" + info->audioCodec);
    }
    d->metaLbl->setText(meta.join("  ·  "));

    // Chips: rebuild
    auto* chipsWidget = d->nameLbl->parentWidget()->findChild<QWidget*>("chipsWidget");
    if (chipsWidget) {
        auto* layout = qobject_cast<QHBoxLayout*>(chipsWidget->layout());
        while (layout && layout->count()) {
            auto* item = layout->takeAt(0);
            if (item->widget()) { item->widget()->deleteLater(); }
            delete item;
        }
        if (layout) {
            if (info->cameraCount > 0) {
                layout->addWidget(chip_label(
                    QString("📷  %1 camera%2")
                        .arg(info->cameraCount)
                        .arg(info->cameraCount > 1 ? "s" : "")));
            }
            if (info->micCount > 0) {
                layout->addWidget(chip_label(
                    QString("🎤  %1 mic%2")
                        .arg(info->micCount)
                        .arg(info->micCount > 1 ? "s" : "")));
            }
            if (info->durationMs > 0) {
                layout->addWidget(chip_label("⏱  " + info->format_duration()));
            }
            layout->addStretch();
        }
    }

    // Files — SessionInfo stores video/audio/analysis entries as paths
    // relative to the session dir (e.g. "video/video_0.mp4") so callers can
    // join them onto info->path directly; display only the bare filename.
    QStringList fileLines;
    for (const auto& fn : info->videoFiles) { fileLines << ("  🎬  " + QFileInfo(fn).fileName()); }
    for (const auto& fn : info->audioFiles) { fileLines << ("  🔊  " + QFileInfo(fn).fileName()); }
    fileLines << ("  📁  " + info->path);
    d->filesLbl->setText(fileLines.join("\n"));

    // Analysis files
    if (info->analysisFiles.isEmpty()) {
        d->analysisFilesLbl->setText("No analysis results yet.");
    } else {
        QStringList alines;
        for (const auto& fn : info->analysisFiles) {
            const QFileInfo fi(info->path + "/" + fn);
            alines << QString("  ✓  %1  (%2 KB)")
                .arg(fi.fileName())
                .arg(fi.size() / 1024);
        }
        d->analysisFilesLbl->setText(alines.join("\n"));
    }

    d->analysisLog->clear();
    d->analysisLog->hide();
    d->analysisStatus->clear();

    rebuild_annot_table();
}

// ── Annotation table ────────────────────────────────────────────────────────

void SessionBrowserW::rebuild_annot_table() {
    auto* info = d->current();
    if (!info) { return; }

    d->annotTable->setRowCount(0);

    for (int row = 0; row < info->annotations.size(); ++row) {
        const auto& ann = info->annotations[row];
        const QColor catColor = AnnotationCategory::color_for(ann.category);
        const QColor rowBg    = catColor.darker(350);

        d->annotTable->insertRow(row);

        auto makeItem = [&](const QString& text) -> QTableWidgetItem* {
            auto* item = new QTableWidgetItem(text);
            item->setBackground(rowBg);
            item->setForeground(catColor.lighter(140));
            return item;
        };

        auto* timeItem = makeItem(SessionInfo::ms_to_hms(ann.timestampMs));
        timeItem->setFlags(timeItem->flags() & ~Qt::ItemIsEditable);
        d->annotTable->setItem(row, 0, timeItem);
        d->annotTable->setItem(row, 1, makeItem(ann.category));
        d->annotTable->setItem(row, 2, makeItem(ann.note));

        auto* delBtn = new QPushButton("✕");
        delBtn->setFlat(true);
        delBtn->setStyleSheet("color: #884444; font-size: 11px;");
        delBtn->setCursor(Qt::PointingHandCursor);
        const int r = row;
        connect(delBtn, &QPushButton::clicked, this,
                [this, r] { delete_annotation(r); });
        d->annotTable->setCellWidget(row, 3, delBtn);

        d->annotTable->setRowHeight(row, 26);
    }
}

void SessionBrowserW::add_annotation() {
    auto* info = d->current();
    if (!info) { return; }

    Annotation ann;
    const QTime t = d->annotTime->time();
    ann.timestampMs = static_cast<int64_t>(
        (t.hour() * 3600 + t.minute() * 60 + t.second()) * 1000LL);
    ann.category = d->annotCat->currentText();
    ann.note     = d->annotNote->text().trimmed();

    // Insert sorted by timestamp
    int pos = 0;
    while (pos < info->annotations.size() &&
           info->annotations[pos].timestampMs <= ann.timestampMs) {
        ++pos;
    }
    info->annotations.insert(pos, ann);
    info->save_annotations();

    d->annotNote->clear();
    rebuild_annot_table();
}

void SessionBrowserW::delete_annotation(int row) {
    auto* info = d->current();
    if (!info || row < 0 || row >= info->annotations.size()) { return; }
    info->annotations.removeAt(row);
    info->save_annotations();
    rebuild_annot_table();
}

void SessionBrowserW::save_annotations() {
    if (auto* info = d->current()) {
        // Sync any in-table edits back to the model before saving
        for (int row = 0; row < d->annotTable->rowCount(); ++row) {
            if (row >= info->annotations.size()) { break; }
            auto& ann = info->annotations[row];
            if (auto* item = d->annotTable->item(row, 1)) {
                ann.category = item->text();
            }
            if (auto* item = d->annotTable->item(row, 2)) {
                ann.note = item->text();
            }
        }
        info->save_annotations();
        d->analysisStatus->setText("Annotations saved.");
        QTimer::singleShot(2000, d->analysisStatus, [this] {
            d->analysisStatus->clear();
        });
    }
}

void SessionBrowserW::export_annot_csv() {
    auto* info = d->current();
    if (!info || info->annotations.isEmpty()) { return; }

    const QString dst = QFileDialog::getSaveFileName(
        this, "Export Annotations",
        info->path + "/annotations.csv",
        "CSV files (*.csv)");
    if (dst.isEmpty()) { return; }

    QFile f(dst);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { return; }

    QTextStream ts(&f);
    ts << "timestamp_ms,timestamp_hms,category,note\n";
    for (const auto& ann : info->annotations) {
        const QString escaped = QString(ann.note).replace('"', "\"\"");
        ts << ann.timestampMs << ","
           << SessionInfo::ms_to_hms(ann.timestampMs) << ","
           << ann.category << ","
           << "\"" << escaped << "\"\n";
    }
    d->analysisStatus->setText("Exported → " + dst);
}

// ── Analysis process management ────────────────────────────────────────────

void SessionBrowserW::run_pose_analysis() {
    auto* info = d->current();
    if (!info) { return; }

    if (d->analysisMgr) {
        d->analysisMgr->analyze_session(info->path);
        d->analysisStatus->setText("Pose analysis queued. Check Perf tab for progress.");
        return;
    }
    // Fallback: run directly
    const QString python = find_python();
    if (python.isEmpty()) {
        d->analysisStatus->setText("Python not found. Set up the analysis environment first.");
        return;
    }
    const QString script =
        QDir(QCoreApplication::applicationDirPath()).filePath("analysis/run_pose.py");
    if (!QFile::exists(script)) {
        d->analysisStatus->setText("run_pose.py not found: " + script);
        return;
    }
    launch_analysis(python, {script, "--session", info->path, "--out-format", "json"});
}

void SessionBrowserW::run_motion_analysis() {
    auto* info = d->current();
    if (!info) { return; }

    const QString python = find_python();
    if (python.isEmpty()) {
        d->analysisStatus->setText("Python not found. Set up the analysis environment first.");
        return;
    }

    // Try to find run_motion.py relative to app or project root
    QStringList candidateDirs = {
        QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationDirPath() + "/../../..",
        QDir::currentPath(),
    };
    QString script;
    for (const auto& base : candidateDirs) {
        const QString candidate = QDir(base).filePath("analysis/run_motion.py");
        if (QFile::exists(candidate)) { script = candidate; break; }
    }
    if (script.isEmpty()) {
        d->analysisStatus->setText("run_motion.py not found. Check analysis/ directory.");
        return;
    }

    launch_analysis(python, {script, "--session", info->path, "--out-format", "csv"});
}

void SessionBrowserW::launch_analysis(const QString& exe, const QStringList& args) {
    if (d->proc && d->proc->state() != QProcess::NotRunning) {
        d->analysisStatus->setText("Analysis already running…");
        return;
    }

    d->runPoseBtn->setEnabled(false);
    d->runMotionBtn->setEnabled(false);
    d->analysisLog->clear();
    d->analysisLog->show();
    d->analysisStatus->setText("Running…");

    if (!d->proc) {
        d->proc = new QProcess(this);
        d->proc->setProcessChannelMode(QProcess::MergedChannels);
    }

    connect(d->proc, &QProcess::readyRead, this, [this] {
        const QString out = d->proc->readAll().trimmed();
        if (!out.isEmpty()) {
            d->analysisLog->append(out);
        }
    });

    connect(d->proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
        d->runPoseBtn->setEnabled(true);
        d->runMotionBtn->setEnabled(true);
        const QString msg = (code == 0) ? "Analysis complete." : "Analysis failed.";
        d->analysisStatus->setText(msg);
        // Refresh analysis files list
        if (auto* info = d->current()) {
            *info = SessionInfo::load(info->path);
            if (info->analysisFiles.isEmpty()) {
                d->analysisFilesLbl->setText("No analysis results yet.");
            } else {
                QStringList bareNames;
                for (const auto& fn : info->analysisFiles) {
                    bareNames << QFileInfo(fn).fileName();
                }
                d->analysisFilesLbl->setText(bareNames.join("  "));
            }
        }
        // Refresh the session row badge
        rebuild_session_list();
    });

    d->proc->start(exe, args);
}

// ── Python discovery ────────────────────────────────────────────────────────

QString SessionBrowserW::find_python() const {
    const QStringList candidateBases = {
        QCoreApplication::applicationDirPath(),
        QCoreApplication::applicationDirPath() + "/../../..",
        QDir::currentPath(),
    };
    for (const auto& base : candidateBases) {
        const QString venvPy = QDir(base).filePath("analysis/.venv/bin/python3");
        if (QFile::exists(venvPy)) { return venvPy; }
        const QString venvPyAlt = QDir(base).filePath("analysis/.venv/bin/python");
        if (QFile::exists(venvPyAlt)) { return venvPyAlt; }
    }
    // Fall back to system python
    for (const auto& name : {"python3", "python"}) {
        QProcess probe;
        probe.start(name, {"--version"});
        if (probe.waitForFinished(2000) && probe.exitCode() == 0) { return name; }
    }
    return {};
}

} // namespace mosaic
