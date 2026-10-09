#include "ui/session/session_identity_dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace mosaic {

namespace {

// Matching SessionHealthDialog's helpers, which set the house style for a
// dialog in this app: muted captions, monospace values.
QLabel* caption_label(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet("color: #7070a0; font-size: 11px;");
    return lbl;
}

QLineEdit* identity_field(const QString& value, const QString& placeholder) {
    auto* edit = new QLineEdit(value);
    edit->setPlaceholderText(placeholder);
    edit->setStyleSheet(
        "QLineEdit { background: #09091a; color: #c8c8e0; border: 1px solid #1e1e40; "
        "border-radius: 4px; padding: 4px 6px; font-size: 12px; } "
        "QLineEdit:focus { border-color: #4a4a90; }");
    return edit;
}

// One row of the pre-flight checks: a coloured dot, the finding, and a muted
// explanation under it. Red and amber match the warning colours used across
// the app (#ddaa44 is the identity warning's own amber).
QWidget* preflight_row(const PreflightItem& item) {
    const char* colour = item.level == PreflightLevel::Fail   ? "#ff6655"
                         : item.level == PreflightLevel::Warn ? "#ddaa44"
                                                              : "#44cc66";
    auto* row          = new QWidget;
    auto* lay          = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    auto* dot = new QLabel("●");
    dot->setStyleSheet(QString("color: %1; font-size: 12px;").arg(colour));
    dot->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    dot->setFixedWidth(14);
    lay->addWidget(dot);

    auto* text = new QLabel(
        item.detail.isEmpty() ? QString("<b>%1</b>").arg(item.title.toHtmlEscaped())
                              : QString("<b>%1</b><br><span style='color:#7070a0'>%2</span>")
                                    .arg(item.title.toHtmlEscaped(), item.detail.toHtmlEscaped()));
    text->setTextFormat(Qt::RichText);
    text->setWordWrap(true);
    text->setStyleSheet(QString("color: %1; font-size: 11px;")
                            .arg(item.level == PreflightLevel::Ok ? "#a0a0c0" : "#d8d8f0"));
    lay->addWidget(text, 1);
    return row;
}

} // namespace

SessionIdentityDialog::SessionIdentityDialog(const QString& subject, const QString& session,
                                             const QString& task, AdviseFn advise,
                                             const PreflightReport& preflight, QWidget* parent)
    : QDialog(parent),
      m_advise(std::move(advise)),
      m_preflightProblems(preflight.needs_attention()) {
    setWindowTitle(m_preflightProblems ? "Before recording" : "Name this recording");
    setModal(true);

    auto* root = new QVBoxLayout(this);
    root->setSpacing(10);

    root->addWidget(caption_label(
        "These name the folder this recording is saved in. Only the subject is required."));

    m_subject = identity_field(subject, "e.g. P01");
    m_session = identity_field(session, "optional — e.g. pre");
    m_task    = identity_field(task, "optional — e.g. rest");

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(7);
    // The asterisk is the only thing marking subject as different from the
    // other two; the warning line deliberately stays quiet about a merely empty
    // subject, because "a subject is required" inside a box whose first field
    // is Subject tells the operator nothing they cannot see.
    form->addRow(caption_label("Subject *"), m_subject);
    form->addRow(caption_label("Session"), m_session);
    form->addRow(caption_label("Task"), m_task);
    root->addLayout(form);

    m_preview = new QLabel;
    m_preview->setStyleSheet("color: #5a5a80; font-size: 11px; font-family: monospace;");
    m_preview->setWordWrap(true);
    root->addWidget(m_preview);

    m_warning = new QLabel;
    m_warning->setStyleSheet("color: #ddaa44; font-size: 11px;");
    m_warning->setWordWrap(true);
    root->addWidget(m_warning);

    // ── Pre-flight checks ────────────────────────────────────────────────
    // Everything that was checked, problems first. The rows that passed stay
    // in view on purpose: "3 cameras ready" next to "Camera 6 is not open"
    // tells the operator the rest of the rig is fine, which is half of
    // deciding whether to record anyway.
    if (!preflight.items.isEmpty()) {
        auto* rule = new QFrame;
        rule->setFrameShape(QFrame::HLine);
        rule->setStyleSheet("color: #1a1a35;");
        root->addWidget(rule);
        root->addWidget(caption_label(m_preflightProblems ? "Checks — fix these, or record anyway:"
                                                          : "Checks"));
        for (const auto& item : preflight.items) {
            root->addWidget(preflight_row(item));
        }
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_start       = buttons->addButton(m_preflightProblems ? "Record anyway" : "Start recording",
                                 QDialogButtonBox::AcceptRole);
    m_start->setDefault(true);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (auto* edit : {m_subject, m_session, m_task}) {
        connect(edit, &QLineEdit::textChanged, this, &SessionIdentityDialog::refresh);
    }

    refresh();
    m_subject->setFocus();
    // Whatever was prefilled is the most likely thing to be replaced, since the
    // usual reason this dialog opens on a filled subject is the next
    // participant. Selecting it makes that one keystroke.
    m_subject->selectAll();
    setMinimumWidth(420);
}

QString SessionIdentityDialog::subject() const { return m_subject->text(); }
QString SessionIdentityDialog::session() const { return m_session->text(); }
QString SessionIdentityDialog::task() const { return m_task->text(); }

void SessionIdentityDialog::refresh() {
    SessionIdentity id;
    id.subject = m_subject->text();
    id.session = m_session->text();
    id.task    = m_task->text();
    // Notes are deliberately absent: this dialog does not own them, and
    // MonitorBridge::confirmIdentityAndStart() takes three strings rather than
    // an identity for the same reason.

    const IdentityAdvice advice = m_advise(id);

    m_preview->setText(advice.folderName.isEmpty() ? QStringLiteral("▸  —")
                                                   : QString("▸  %1").arg(advice.folderName));

    // NoSubject is the one issue this dialog suppresses — see the Subject *
    // comment above. Every other issue is shown verbatim, so the wording an
    // operator sees here is the wording they saw in the monitor's inline bar.
    QString text;
    if (advice.issue != IdentityIssue::None && advice.issue != IdentityIssue::NoSubject) {
        text = QString("⚠  %1").arg(advice.warning);
    }
    // A repeat is not a warning about the labels, so it is worded here rather
    // than in advise_identity(): it is the answer to "has this been done
    // before", and it is reassurance as much as caution — nothing is
    // overwritten either way, the run number is all that changes.
    if (advice.collision.collides()) {
        const QString repeat =
            QString(
                "This subject/session/task already has %1 recording%2 — this one will be "
                "run-%3. Nothing is overwritten.")
                .arg(advice.collision.existingCount)
                .arg(advice.collision.existingCount == 1 ? "" : "s")
                .arg(advice.collision.suggestedRun, 2, 10, QChar('0'));
        text = text.isEmpty() ? repeat : text + "\n" + repeat;
    }
    m_warning->setText(text);
    m_warning->setVisible(!text.isEmpty());

    m_start->setEnabled(advice.canRecord);
}

} // namespace mosaic
