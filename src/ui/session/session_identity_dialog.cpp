#include "ui/session/session_identity_dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
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

} // namespace

SessionIdentityDialog::SessionIdentityDialog(const QString& subject, const QString& session,
                                             const QString& task, AdviseFn advise, QWidget* parent)
    : QDialog(parent), m_advise(std::move(advise)) {
    setWindowTitle("Name this recording");
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

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_start       = buttons->addButton("Start recording", QDialogButtonBox::AcceptRole);
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
