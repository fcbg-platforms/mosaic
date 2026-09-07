#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace mosaic {

// Names a recording after the participant, session and task that produced it,
// instead of only the wall-clock moment it happened to start.
//
// The scheme is BIDS-*inspired*, deliberately not BIDS-valid: one flat folder
// per recording, named with BIDS entities in BIDS order, with a trailing
// timestamp that BIDS has no entity for. That trade was made knowingly — the
// timestamp guarantees uniqueness and keeps folders sorting chronologically in
// a file manager, which matters more day to day than passing bids-validator. A
// real BIDS export, if it is ever wanted, is a converter over this, not a
// change to it.
//
// Everything here is pure and QtCore-only, with one clearly-marked exception
// (existing_session_names). That is not incidental tidiness: mosaic_tests links
// only Qt6::Core and Qt6::Network, and CI runs ctest but never pytest, so logic
// that lives anywhere else — in a widget, in the QML bridge, in
// session_info.hpp, which pulls QtGui in via QColor — cannot be covered at all.
// Naming decides where data lands, so it is exactly the wrong thing to leave
// untested.

// Longest a single entity label may be. Labels are bounded because the folder
// name is only the *first* component of every path underneath it: analysis
// plugins write into anonymized/, pose/, synced/ with long generated basenames
// of their own, and Windows still caps a path at 260 characters. Fixed
// overhead here is 38 characters ("sub-" + "_ses-" + "_task-" + "_run-NN" +
// "_yyyyMMddTHHmmss"), so three 24-character labels give a ~110-character
// folder name and leave usable headroom. The cap alone is not the guard,
// though — see fits_path_budget().
inline constexpr int k_max_label_chars = 24;

// Budget for recordDir + '/' + folderName, leaving ~110 characters of the
// Windows MAX_PATH for the deepest generated child a plugin may write.
inline constexpr int k_session_path_budget = 150;

// The timestamp appended after the entities. Deliberately *not*
// RecordSettings::timestampFormat, whose default "yyyy-MM-dd_hh-mm-ss"
// contains both '_' and '-' — the two characters BIDS reserves to separate
// entities from each other and keys from values. Using it would make the
// folder name unparseable by anything, including parse_session_folder_name()
// below. The 'T' is quoted because Qt would otherwise be free to read a bare
// letter as a format specifier.
//
// If you are here because you want the operator's chosen format honoured: it
// still is, exactly, for recordings with no identity set. See
// build_session_folder_name().
inline constexpr auto k_bids_timestamp_format = "yyyyMMdd'T'HHmmss";

/// Who and what a recording is of.
///
/// Every field is optional *to this module*: an entirely empty identity is
/// legal here and falls back to the timestamp-only naming this app used before
/// any of this existed. That fallback still has to exist — a StartRecording
/// trigger can fire with nobody at the keyboard, and an awkwardly-named
/// recording beats no recording at all.
///
/// What is *not* optional is starting a recording by hand without a subject;
/// see names_a_subject(). The permissiveness here and the requirement there
/// are deliberately separate: this module says what a name can be, the UI says
/// what an operator may do.
struct SessionIdentity {
    QString subject; ///< BIDS "sub" label.
    QString session; ///< BIDS "ses" label.
    QString task;    ///< BIDS "task" label.

    /// Free text from the operator. Never enters a path, never sanitized as a
    /// label, and stored beside the session as notes.txt rather than inside
    /// session_meta.json — see RecordManager::start().
    QString notes;

    /// 1-based; 0 means "not yet resolved", which is what the UI holds until
    /// build_session_path() scans the recordings directory for siblings.
    int run = 0;

    /// True when no entity is set, i.e. this recording gets the legacy
    /// timestamp-only folder name. Notes deliberately do not count: a note is
    /// not an identity, and attaching one must not change where data lands.
    [[nodiscard]] bool has_entities() const;

    /// Entities only — notes are excluded on purpose. This goes into
    /// session_meta.json, which is written once and never rewritten; notes
    /// have to stay editable after the fact, so they cannot live there.
    [[nodiscard]] QJsonObject to_json() const;

    /// Re-sanitizes every label. Values reaching this have been round-tripped
    /// through settings.json, which a user can hand-edit, so a label arriving
    /// as "../../etc" must not be able to reach a path.
    [[nodiscard]] static SessionIdentity from_json(const QJsonObject& obj);
};

/// Whether this identity is attributable to a participant, i.e. whether the
/// folder it produces will carry a "sub-" entity.
///
/// A subject is the one label that has to be there. Session and task describe
/// what was done and can be reconstructed later from a lab notebook or the
/// recording itself; only "sub-" says *who it was of*, and that is the one
/// thing no amount of after-the-fact archaeology recovers once a dozen
/// timestamp-named folders have piled up.
///
/// Judged on the sanitized label, not the raw text: a subject field holding
/// "!!!" is three characters the operator can see and zero characters that
/// survive into the folder name, and a subject that names nothing is no
/// subject. Consistent with has_entities(), which sanitizes for the same reason.
[[nodiscard]] bool names_a_subject(const SessionIdentity& id);

// ── Labels ─────────────────────────────────────────────────────────────────

/// BIDS labels are strictly alphanumeric: '-' separates a key from its value
/// and '_' separates entities, so a label containing either would silently
/// change the name's structure rather than just look odd.
[[nodiscard]] bool is_valid_label(const QString& label);

/// Best-effort coercion of operator input into a valid label.
///
/// Normalizes to NFKD and drops combining marks first, so "Müller" becomes
/// "Muller" rather than "Mller" — dropping the accented letter entirely would
/// quietly merge two different participants. Case is preserved: BIDS treats
/// "rest" and "Rest" as different labels, and silently folding case would hide
/// that from an operator who typed one and meant the other.
///
/// Idempotent by construction: sanitize(sanitize(x)) == sanitize(x).
[[nodiscard]] QString sanitize_label(const QString& raw, int maxChars = k_max_label_chars);

// ── Building ───────────────────────────────────────────────────────────────

/// The entity portion alone, e.g. "sub-P01_ses-pre_task-rest_run-02".
/// Empty when the identity has no entities. Entities appear in canonical BIDS
/// order (sub, ses, task, run); any absent one is skipped rather than emitted
/// with an empty value.
[[nodiscard]] QString entity_prefix(const SessionIdentity& id);

/// The full folder basename.
///
/// @param timestampFormat  The operator's configured format, or an empty
///                         string for "no timestamp" (RecordSettings::
///                         addTimestamp == false). Honoured verbatim only when
///                         the identity is empty; otherwise the compact
///                         k_bids_timestamp_format is used instead, for the
///                         separator reason documented on that constant.
///
/// With no identity and no format this returns "session", which is what this
/// app has always produced in that configuration. That exact fallback is
/// pinned by a test: an existing deployment that types nothing must see
/// byte-identical behaviour.
[[nodiscard]] QString build_session_folder_name(const SessionIdentity& id, const QDateTime& when,
                                                const QString& timestampFormat);

// ── Parsing ────────────────────────────────────────────────────────────────

/// What a folder basename decomposes into. A legacy timestamp-only folder
/// yields hasEntities == false with the whole name preserved in `timestamp`,
/// which is how every consumer keeps working across the naming change.
struct ParsedSessionName {
    bool hasEntities = false;
    QString subject;
    QString session;
    QString task;
    int run = 0;
    QString timestamp; ///< Trailing remainder, verbatim; may be empty.
};

/// Total and non-throwing: any string is a valid input, including "sub-",
/// "run-xx" and "". Unrecognised leading tokens end entity parsing, so an
/// unknown BIDS entity lands in `timestamp` rather than being silently
/// discarded.
[[nodiscard]] ParsedSessionName parse_session_folder_name(const QString& basename);

// ── Collisions and run numbering ───────────────────────────────────────────
//
// These take the directory listing rather than a path. Keeping the filesystem
// out of them is what makes the run-numbering rules — which decide whether two
// recordings can overwrite each other — directly testable.

/// Folders in `existingNames` recording the same subject/session/task, ignoring
/// their run index and timestamp. Comparison is exact and case-sensitive,
/// matching BIDS. Always empty for an identity with no entities, so a
/// directory full of legacy timestamp folders can never register a collision.
[[nodiscard]] QStringList matching_session_folders(const QStringList& existingNames,
                                                   const SessionIdentity& id);

/// The run index a new recording of `id` should take: 1 when it is the first,
/// otherwise one past the highest run already present.
///
/// Deliberately `max(highest) + 1`, never `count + 1`. If run-02 of three is
/// deleted, count+1 would hand run-03's number to a different recording, so
/// two distinct sessions would share an identity — the precise failure this
/// whole feature exists to prevent. Returns 0 for an identity with no
/// entities, which emits no run entity at all.
[[nodiscard]] int next_run_index(const QStringList& existingNames, const SessionIdentity& id);

/// What the operator needs to be told before a repeat recording is created.
struct SessionCollision {
    int existingCount = 0; ///< Recordings already sharing this subject/session/task.
    int suggestedRun  = 0; ///< The run index next_run_index() would assign.

    [[nodiscard]] bool collides() const { return existingCount > 0; }
};

[[nodiscard]] SessionCollision check_collision(const QStringList& existingNames,
                                               const SessionIdentity& id);

/// next_run_index() once the sibling list is already in hand.
///
/// Split out so a caller that needs both the count and the index — every
/// caller, in practice — matches the directory once instead of twice. Assumes
/// its argument really is a matched sibling list; it does no filtering of its
/// own and an empty list means "run 1", not "not applicable".
[[nodiscard]] int next_run_after(const QStringList& siblings);

/// Whether recordDir + '/' + folderName leaves a plugin room to write inside
/// the resulting session. Checked before a recording starts, so an over-long
/// combination fails while it is still just text in a field — rather than
/// hours later, when an analysis plugin cannot create its output file.
[[nodiscard]] bool fits_path_budget(const QString& recordDir, const QString& folderName);

// ── Advice ─────────────────────────────────────────────────────────────────

/// What, if anything, is wrong with an identity — the classification behind
/// IdentityAdvice::warning.
///
/// The prose is carried too, so the two surfaces cannot word the same problem
/// differently, but the surfaces need the *category* for the one thing a
/// sentence cannot give them: the dialog opened specifically to ask for a
/// subject must not then print "A subject is required before recording." at the
/// operator, which is a tautology inside a box whose only field is Subject. It
/// suppresses NoSubject and renders every other issue verbatim.
enum class IdentityIssue {
    None,
    NoSubject,       ///< Subject field empty.
    SubjectUnusable, ///< Subject typed, but nothing survives sanitizing ("???").
    LabelDropped,    ///< A session/task label sanitizes away entirely.
    LabelTruncated,  ///< Legal characters, but longer than k_max_label_chars.
    LabelCoerced,    ///< Illegal characters removed, something usable remains.
    NameTooLong,     ///< The whole folder name overruns the path budget.
};

/// Everything the UI needs to say about one identity, decided in a single pass.
///
/// This exists because there are two surfaces asking the same question — the
/// inline bar in the monitor and the dialog that opens when Record is clicked
/// without a subject — and they must never give different answers about the
/// same three labels. Phrasing the warnings in the UI layer would mean phrasing
/// them twice, which is exactly how the two drift apart. (The usual argument
/// for keeping strings out of a domain module is localization; this app has no
/// translation layer, so it does not apply.)
///
/// The second reason it lives here rather than in MonitorBridge, where this
/// logic started: that class pulls QtGui in via QImage, and mosaic_tests links
/// only Qt6::Core and Qt6::Network. Logic deciding where recordings land is the
/// wrong thing to leave untestable.
struct IdentityAdvice {
    /// The folder that would be created. **Empty exactly when the identity names
    /// no subject** — build_session_folder_name() would happily return a name
    /// there (the bare timestamp, or "ses-…_task-…" when only those two are
    /// filled), and showing an accurate preview of a folder the operator cannot
    /// create is worse than showing none.
    ///
    /// Deliberately *not* empty for an over-long name, which is the other way
    /// canRecord goes false: there the name is exactly what the operator needs
    /// to see in order to shorten it.
    QString folderName;

    /// One line about the *labels*, empty when there is nothing to say: what
    /// got dropped or shortened, that a subject is still missing, or that the
    /// name is too long for the recordings directory. The first problem only —
    /// a field-by-field breakdown is noise next to a preview showing the result.
    QString warning;

    /// Whether a recording may be started by hand under this identity: it names
    /// a subject (see names_a_subject()) *and* the resulting path leaves room
    /// for the files written inside it (see fits_path_budget()). One answer
    /// rather than two, because every caller wants the same thing from it —
    /// whether to enable a button. Which of the two failed is in `warning`.
    bool canRecord = false;

    /// What history says about this combination. Deliberately data rather than
    /// prose: a collision is a question with buttons attached ("record as
    /// run-02?"), not a warning, so the surface asking it does the phrasing.
    SessionCollision collision;

    /// The category `warning` describes. IdentityIssue::None exactly when
    /// `warning` is empty.
    IdentityIssue issue = IdentityIssue::None;
};

/// Judge one identity: what it would create, what is wrong with it, and whether
/// it may be recorded.
///
/// @param existingNames  One directory listing, from existing_session_names().
///                       Taken as a parameter rather than read here so this
///                       stays pure and so a caller that needs several answers
///                       pays for one scan, not three.
/// @param timestampFormat  As build_session_folder_name() takes it: the
///                         operator's configured format, or empty for "no
///                         timestamp".
[[nodiscard]] IdentityAdvice advise_identity(const SessionIdentity& raw,
                                             const QStringList& existingNames,
                                             const QString& recordDir, const QDateTime& when,
                                             const QString& timestampFormat);

// ── Ordering ───────────────────────────────────────────────────────────────

/// "Newest first", for SessionInfo::list_all() and the two places that merge
/// several directories' worth of sessions together.
///
/// Sessions were previously ordered by folder name reversed, which was only
/// ever *accidentally* chronological: "yyyy-MM-dd_hh-mm-ss" happens to sort
/// lexicographically the same way it sorts in time. A "sub-" prefix destroys
/// that coincidence and would silently reorder both browsers by subject, so
/// ordering now uses the recorded start time, which is what "newest first"
/// always meant.
///
/// Takes the two fields it compares rather than a SessionInfo, because
/// session_info.hpp must include this header to call it — passing the struct
/// would close that into an include cycle.
///
/// A session whose start time is missing or unparseable sorts last: it is a
/// damaged session, and burying it beats interleaving it at an arbitrary
/// point. Ties break on name descending so the order is total and stable —
/// std::sort with an inconsistent comparator is undefined behaviour, not just
/// a wrong order.
[[nodiscard]] bool session_entry_newer(const QDateTime& aStart, const QString& aName,
                                       const QDateTime& bStart, const QString& bName);

// ── Filesystem ─────────────────────────────────────────────────────────────

/// Every immediate subdirectory of `recordDir`. The one impure function here,
/// kept trivial so everything above it stays testable.
///
/// Deliberately does *not* require session_meta.json, unlike
/// SessionInfo::list_all(): a half-created folder left by a crashed start
/// still occupies its name, so it must still count against run numbering.
[[nodiscard]] QStringList existing_session_names(const QString& recordDir);

} // namespace mosaic
