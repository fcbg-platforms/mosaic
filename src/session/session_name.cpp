#include "session/session_name.hpp"

#include <QDir>
#include <QJsonValue>

namespace mosaic {

namespace {

// BIDS labels are ASCII-alphanumeric only. Deliberately not QChar::isLetterOrNumber(),
// which accepts Cyrillic, CJK and every other script — those are perfectly good
// characters that BIDS still does not allow in a label, and letting them through
// here would push the failure downstream into a path.
bool is_label_char(QChar c) {
    return (c >= u'0' && c <= u'9') || (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z');
}

// Entity keys this scheme understands. Anything else ends entity parsing rather
// than being skipped, so an unrecognised entity is preserved in the remainder
// instead of silently vanishing.
constexpr auto k_key_subject = "sub";
constexpr auto k_key_session = "ses";
constexpr auto k_key_task    = "task";
constexpr auto k_key_run     = "run";

QString format_run(int run) {
    // Width 2 is a minimum, not a limit: run 123 renders "run-123" rather than
    // being truncated to something that would collide with another run.
    return QString("run-%1").arg(run, 2, 10, QChar('0'));
}

} // namespace

// ── SessionIdentity ────────────────────────────────────────────────────────

bool SessionIdentity::has_entities() const {
    // Sanitize before deciding: a subject field holding only punctuation
    // contributes no entity, and must therefore not flip a recording out of
    // legacy timestamp-only naming into a BIDS name with nothing in it.
    return !sanitize_label(subject).isEmpty() || !sanitize_label(session).isEmpty() ||
           !sanitize_label(task).isEmpty();
}

bool names_a_subject(const SessionIdentity& id) { return !sanitize_label(id.subject).isEmpty(); }

QJsonObject SessionIdentity::to_json() const {
    // The sanitized values, not the raw ones — this records what was actually
    // used to name the folder, which is the only version that is true.
    QJsonObject obj{
        {"sub", sanitize_label(subject)},
        {"ses", sanitize_label(session)},
        {"task", sanitize_label(task)},
    };
    if (run > 0) {
        obj.insert("run", run);
    }
    return obj;
}

SessionIdentity SessionIdentity::from_json(const QJsonObject& obj) {
    SessionIdentity id;
    id.subject = sanitize_label(obj.value("sub").toString());
    id.session = sanitize_label(obj.value("ses").toString());
    id.task    = sanitize_label(obj.value("task").toString());
    id.run     = obj.value("run").toInt(0);
    // Notes are not carried in this object — see the header.
    return id;
}

// ── Labels ─────────────────────────────────────────────────────────────────

bool is_valid_label(const QString& label) {
    if (label.isEmpty() || label.size() > k_max_label_chars) {
        return false;
    }
    for (const QChar c : label) {
        if (!is_label_char(c)) {
            return false;
        }
    }
    return true;
}

QString sanitize_label(const QString& raw, int maxChars) {
    // NFKD splits an accented letter into its base plus a combining mark, so
    // dropping the marks below leaves the base letter behind. Without this,
    // "Müller" would sanitize to "Mller" — a different participant.
    const QString decomposed = raw.normalized(QString::NormalizationForm_KD);

    QString out;
    out.reserve(qMin(decomposed.size(), maxChars));
    for (const QChar c : decomposed) {
        if (is_label_char(c)) {
            out.append(c);
            if (out.size() >= maxChars) {
                break;
            }
        }
    }
    return out;
}

// ── Building ───────────────────────────────────────────────────────────────

QString entity_prefix(const SessionIdentity& id) {
    QStringList parts;
    // Canonical BIDS order. Not alphabetical, not field-declaration order —
    // BIDS fixes it, and a name whose entities are out of order is as wrong as
    // one with the values swapped.
    const QString sub  = sanitize_label(id.subject);
    const QString ses  = sanitize_label(id.session);
    const QString task = sanitize_label(id.task);

    if (!sub.isEmpty()) {
        parts << QString("%1-%2").arg(k_key_subject, sub);
    }
    if (!ses.isEmpty()) {
        parts << QString("%1-%2").arg(k_key_session, ses);
    }
    if (!task.isEmpty()) {
        parts << QString("%1-%2").arg(k_key_task, task);
    }
    if (parts.isEmpty()) {
        return {};
    }
    if (id.run > 0) {
        parts << format_run(id.run);
    }
    return parts.join('_');
}

QString build_session_folder_name(const SessionIdentity& id, const QDateTime& when,
                                  const QString& timestampFormat) {
    const QString prefix = entity_prefix(id);

    if (prefix.isEmpty()) {
        // No identity: reproduce exactly what this app produced before any of
        // this existed, including the literal "session" when the operator has
        // turned timestamps off. Pinned by a test — an existing deployment
        // that types nothing must not see its folder names change.
        return timestampFormat.isEmpty() ? QString("session") : when.toString(timestampFormat);
    }

    if (timestampFormat.isEmpty()) {
        // Timestamps off *and* an identity set is the one configuration where
        // the name is genuinely BIDS-valid, because run- is then the sole
        // disambiguator. Leave it alone rather than forcing a timestamp back in.
        return prefix;
    }
    return prefix + "_" + when.toString(k_bids_timestamp_format);
}

// ── Parsing ────────────────────────────────────────────────────────────────

ParsedSessionName parse_session_folder_name(const QString& basename) {
    ParsedSessionName out;
    const QStringList tokens = basename.split('_', Qt::KeepEmptyParts);

    int consumed = 0;
    for (const QString& token : tokens) {
        const qsizetype dash = token.indexOf('-');
        if (dash <= 0) {
            break; // not "key-value"
        }
        const QString key   = token.left(dash);
        const QString value = token.mid(dash + 1);

        if (key == QLatin1String(k_key_run)) {
            bool ok       = false;
            const int run = value.toInt(&ok);
            if (!ok || run <= 0) {
                break;
            }
            out.run = run;
        } else if (!is_valid_label(value)) {
            // A known key with an unusable value is not an entity. Stop rather
            // than accept it: the remainder is preserved verbatim below, so
            // nothing is lost, and a hand-mangled name degrades to "legacy"
            // instead of half-parsing into a wrong identity.
            break;
        } else if (key == QLatin1String(k_key_subject)) {
            out.subject = value;
        } else if (key == QLatin1String(k_key_session)) {
            out.session = value;
        } else if (key == QLatin1String(k_key_task)) {
            out.task = value;
        } else {
            break; // unknown entity — remainder, not discarded
        }
        ++consumed;
    }

    out.hasEntities = !out.subject.isEmpty() || !out.session.isEmpty() || !out.task.isEmpty();
    out.timestamp   = QStringList(tokens.mid(consumed)).join('_');
    return out;
}

// ── Collisions and run numbering ───────────────────────────────────────────

QStringList matching_session_folders(const QStringList& existingNames, const SessionIdentity& id) {
    if (!id.has_entities()) {
        // A legacy, entity-less recording has no identity to collide with. This
        // is what guarantees an operator who types nothing never sees a dialog.
        return {};
    }
    const QString sub  = sanitize_label(id.subject);
    const QString ses  = sanitize_label(id.session);
    const QString task = sanitize_label(id.task);

    QStringList out;
    for (const QString& name : existingNames) {
        const ParsedSessionName p = parse_session_folder_name(name);
        // Exact and case-sensitive, as BIDS defines labels: "rest" and "Rest"
        // really are different tasks, and quietly merging them would attribute
        // one recording's data to the other.
        if (p.hasEntities && p.subject == sub && p.session == ses && p.task == task) {
            out << name;
        }
    }
    return out;
}

int next_run_after(const QStringList& siblings) {
    int highest = 0;
    for (const QString& name : siblings) {
        highest = qMax(highest, parse_session_folder_name(name).run);
    }
    // max(highest)+1, never count+1 — see the header. The count term is only a
    // floor for the malformed case where siblings exist but carry no run index
    // at all, which would otherwise hand out a number already in use.
    return qMax(highest + 1, static_cast<int>(siblings.size()) + 1);
}

int next_run_index(const QStringList& existingNames, const SessionIdentity& id) {
    if (!id.has_entities()) {
        return 0;
    }
    return next_run_after(matching_session_folders(existingNames, id));
}

SessionCollision check_collision(const QStringList& existingNames, const SessionIdentity& id) {
    const QStringList siblings = matching_session_folders(existingNames, id);

    SessionCollision out;
    out.existingCount = static_cast<int>(siblings.size());
    // One match pass, not two. This used to call next_run_index(), which
    // re-derived the same sibling list — every folder name in the recordings
    // directory got split and parsed twice per call, and the identity preview
    // calls this on every keystroke.
    //
    // The has_entities() guard has to stay here rather than move into
    // next_run_after(): an entity-less identity reports run 0, meaning "not
    // applicable", and next_run_after({}) would answer 1 instead. Pinned by
    // SessionCollisions.AnEmptyIdentityNeverCollidesWithLegacyFolders.
    out.suggestedRun = id.has_entities() ? next_run_after(siblings) : 0;
    return out;
}

bool fits_path_budget(const QString& recordDir, const QString& folderName) {
    return recordDir.size() + 1 + folderName.size() <= k_session_path_budget;
}

// ── Advice ─────────────────────────────────────────────────────────────────

IdentityAdvice advise_identity(const SessionIdentity& raw, const QStringList& existingNames,
                               const QString& recordDir, const QDateTime& when,
                               const QString& timestampFormat) {
    IdentityAdvice out;

    SessionIdentity resolved = raw;
    // One match pass for the whole function: the run index, the collision count
    // and the budget measurement all come off this list. Any caller-supplied
    // resolved.run is overwritten rather than trusted — the same struct is used
    // as input and output by MonitorBridge, and a stale index would be measured
    // against the path budget and shown in the preview.
    const QStringList siblings = matching_session_folders(existingNames, resolved);
    if (resolved.has_entities()) {
        // Resolved before the budget check below, so that check measures the
        // name that will actually be created rather than one missing "_run-NN".
        resolved.run = next_run_after(siblings);
    } else {
        resolved.run = 0;
    }

    out.canRecord = names_a_subject(resolved);
    if (!out.canRecord) {
        // Collision deliberately left at its default. The siblings matched
        // above are those of a *different* combination — with the subject blank
        // and session/task carried over from the last participant,
        // matching_session_folders() finds the subject-less recordings a
        // trigger can still produce. Reporting "already has 1 recording, this
        // will be run-02" beside an empty Subject field quotes a run number for
        // a recording nobody is describing, and it changes the moment a subject
        // is typed.

        // No subject: say that and stop. Everything below reports problems with
        // a name that is obtainable, so it is all moot until this one is fixed.
        // folderName stays empty — see its doc comment.
        if (raw.subject.isEmpty()) {
            out.issue   = IdentityIssue::NoSubject;
            out.warning = QStringLiteral("A subject is required before recording.");
        } else {
            // Typed, but nothing survives sanitizing, e.g. "???". The generic
            // message would leave the operator staring at a filled field being
            // told to fill it in.
            out.issue   = IdentityIssue::SubjectUnusable;
            out.warning = QString(
                              "Subject \"%1\" has no letters or digits — a subject is "
                              "required before recording.")
                              .arg(raw.subject);
        }
        return out;
    }

    out.collision.existingCount = static_cast<int>(siblings.size());
    out.collision.suggestedRun  = resolved.run;
    out.folderName              = build_session_folder_name(resolved, when, timestampFormat);

    // Report the first problem only. A field-by-field breakdown is noise beside
    // a preview that already shows the result.
    struct Field {
        const char* name;
        const QString& text;
    };
    for (const Field f :
         {Field{"Subject", raw.subject}, Field{"Session", raw.session}, Field{"Task", raw.task}}) {
        if (f.text.isEmpty()) continue;
        const QString clean = sanitize_label(f.text);
        if (clean == f.text) continue;

        if (clean.isEmpty()) {
            // Reachable for Session and Task only — a subject that sanitizes to
            // nothing has already returned above.
            out.issue   = IdentityIssue::LabelDropped;
            out.warning = QString("%1 \"%2\" has no letters or digits and will be ignored.")
                              .arg(f.name, f.text);
        } else if (sanitize_label(f.text, f.text.size()) == f.text) {
            // Nothing was dropped for being the wrong kind of character — the
            // label is simply too long. Saying "only letters and digits are
            // allowed" here would be actively wrong, and would hide the real
            // danger: two long ids sharing a prefix collapse to one identity.
            out.issue   = IdentityIssue::LabelTruncated;
            out.warning = QString(
                              "%1 is longer than %2 characters and will be shortened "
                              "to \"%3\" — check it still identifies uniquely.")
                              .arg(f.name)
                              .arg(k_max_label_chars)
                              .arg(clean);
        } else {
            out.issue = IdentityIssue::LabelCoerced;
            out.warning =
                QString("%1 will be recorded as \"%2\" — only letters and digits are allowed.")
                    .arg(f.name, clean);
        }
        break;
    }

    // Folded into canRecord rather than merely reported: an over-budget folder
    // records perfectly well and then fails silently hours later, when an
    // analysis plugin cannot create its output inside it.
    //
    // This is the one place "report the first problem only" does not apply, and
    // the exception is the point. Every issue above is cosmetic — the recording
    // proceeds, the name is simply not the one that was typed — so naming one
    // and stopping is kind. This one *refuses*, and a refusal whose reason is
    // not on screen is exactly the failure this struct was reshaped to remove:
    // a dead Start button beside a sentence about a coerced hyphen says nothing
    // about path length. So it appends rather than defers, and it takes the
    // issue category — between a cosmetic note and a blocking one, the blocking
    // one is what the surfaces need to key off.
    if (!fits_path_budget(recordDir, out.folderName)) {
        out.canRecord = false;
        const QString tooLong =
            QString("This name is too long for %1 — shorten a label.").arg(recordDir);
        out.warning = out.warning.isEmpty() ? tooLong : out.warning + " " + tooLong;
        out.issue   = IdentityIssue::NameTooLong;
    }
    return out;
}

// ── Ordering ───────────────────────────────────────────────────────────────

bool session_entry_newer(const QDateTime& aStart, const QString& aName, const QDateTime& bStart,
                         const QString& bName) {
    if (aStart.isValid() != bStart.isValid()) {
        return aStart.isValid(); // damaged sessions sort last
    }
    if (aStart.isValid() && aStart != bStart) {
        return aStart > bStart; // newest first
    }
    return aName > bName; // total order, so std::sort stays well-defined
}

// ── Filesystem ─────────────────────────────────────────────────────────────

QStringList existing_session_names(const QString& recordDir) {
    const QDir dir(recordDir);
    if (!dir.exists()) {
        return {};
    }
    return dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

} // namespace mosaic
