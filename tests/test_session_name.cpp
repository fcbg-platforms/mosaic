#include <gtest/gtest.h>

#include <QDateTime>
#include <QTimeZone>
#include <QVector>

#include "session/session_name.hpp"

using mosaic::advise_identity;
using mosaic::build_session_folder_name;
using mosaic::check_collision;
using mosaic::entity_prefix;
using mosaic::fits_path_budget;
using mosaic::IdentityAdvice;
using mosaic::IdentityIssue;
using mosaic::is_valid_label;
using mosaic::k_max_label_chars;
using mosaic::matching_session_folders;
using mosaic::next_run_index;
using mosaic::parse_session_folder_name;
using mosaic::sanitize_label;
using mosaic::session_entry_newer;
using mosaic::SessionIdentity;

namespace {

SessionIdentity make_id(const QString& sub, const QString& ses, const QString& task, int run = 0) {
    SessionIdentity id;
    id.subject = sub;
    id.session = ses;
    id.task    = task;
    id.run     = run;
    return id;
}

// A fixed instant, so every expectation below is exact rather than
// "whatever the clock said".
QDateTime fixed_when() { return QDateTime(QDate(2026, 9, 6), QTime(14, 30, 12)); }

const QString kLegacyFormat = "yyyy-MM-dd_hh-mm-ss";

} // namespace

// ── Labels ─────────────────────────────────────────────────────────────────

TEST(SessionLabel, AcceptsOnlyAlphanumerics) {
    EXPECT_TRUE(is_valid_label("P01"));
    EXPECT_TRUE(is_valid_label("rest"));
    EXPECT_TRUE(is_valid_label("9"));

    EXPECT_FALSE(is_valid_label(""));
    EXPECT_FALSE(is_valid_label("P-01")); // '-' separates key from value
    EXPECT_FALSE(is_valid_label("P_01")); // '_' separates entities
    EXPECT_FALSE(is_valid_label("P 01"));
    EXPECT_FALSE(is_valid_label("sub-P01")); // a whole entity, not a label
}

// Non-ASCII letters are real letters, but BIDS still forbids them; accepting
// them here would only move the failure into a filesystem path.
TEST(SessionLabel, RejectsNonAsciiLetters) {
    EXPECT_FALSE(is_valid_label(QString::fromUtf8("Pé")));
    EXPECT_FALSE(is_valid_label(QString::fromUtf8("Ру")));
}

TEST(SessionLabel, SanitizeStripsSeparatorsAndWhitespace) {
    EXPECT_EQ(sanitize_label("P-01"), "P01");
    EXPECT_EQ(sanitize_label("  rest  "), "rest");
    EXPECT_EQ(sanitize_label("task_2"), "task2");
    EXPECT_EQ(sanitize_label("***"), "");
    EXPECT_EQ(sanitize_label(""), "");
}

// The accent case is the one that matters: dropping the letter outright would
// turn two distinct participants into the same label.
TEST(SessionLabel, SanitizeKeepsTheBaseLetterOfAnAccentedCharacter) {
    EXPECT_EQ(sanitize_label(QString::fromUtf8("Müller")), "Muller");
    EXPECT_EQ(sanitize_label(QString::fromUtf8("Renée")), "Renee");
}

// BIDS treats "rest" and "Rest" as different labels. Folding case would hide
// that from an operator who typed one meaning the other.
TEST(SessionLabel, SanitizePreservesCase) { EXPECT_EQ(sanitize_label("ReSt"), "ReSt"); }

TEST(SessionLabel, SanitizeTruncatesToTheCap) {
    const QString long_input(k_max_label_chars + 10, u'a');
    EXPECT_EQ(sanitize_label(long_input).size(), k_max_label_chars);
}

TEST(SessionLabel, SanitizeIsIdempotent) {
    for (const QString& raw : {QString("P-01"), QString::fromUtf8("Müller"), QString("  x  "),
                               QString("***"), QString(k_max_label_chars + 5, u'z')}) {
        const QString once = sanitize_label(raw);
        EXPECT_EQ(sanitize_label(once), once) << "raw = " << raw.toStdString();
    }
}

// A label is about to become a path component, so traversal attempts must not
// survive it. These arrive via settings.json, which a user can hand-edit.
TEST(SessionLabel, SanitizeDefeatsPathTraversal) {
    EXPECT_EQ(sanitize_label("../../etc"), "etc");
    EXPECT_EQ(sanitize_label("C:\\Windows"), "CWindows");
    EXPECT_EQ(sanitize_label("a/b"), "ab");
}

// ── Entity prefix ──────────────────────────────────────────────────────────

TEST(SessionEntityPrefix, EmitsCanonicalBidsOrder) {
    EXPECT_EQ(entity_prefix(make_id("P01", "pre", "rest")), "sub-P01_ses-pre_task-rest");
}

TEST(SessionEntityPrefix, SkipsAbsentEntitiesRatherThanEmittingEmptyValues) {
    EXPECT_EQ(entity_prefix(make_id("P01", "", "")), "sub-P01");
    EXPECT_EQ(entity_prefix(make_id("", "", "rest")), "task-rest");
    EXPECT_EQ(entity_prefix(make_id("P01", "", "rest")), "sub-P01_task-rest");
    EXPECT_EQ(entity_prefix(make_id("", "", "")), "");
}

TEST(SessionEntityPrefix, RunIsAppendedLastAndZeroPaddedToTwoDigits) {
    EXPECT_EQ(entity_prefix(make_id("P01", "pre", "rest", 2)), "sub-P01_ses-pre_task-rest_run-02");
    EXPECT_EQ(entity_prefix(make_id("P01", "", "", 12)), "sub-P01_run-12");
    // Width 2 is a minimum, not a limit — truncating here would let two runs
    // collide.
    EXPECT_EQ(entity_prefix(make_id("P01", "", "", 123)), "sub-P01_run-123");
}

// A run index without any entity to attach it to means nothing.
TEST(SessionEntityPrefix, RunAloneProducesNothing) {
    EXPECT_EQ(entity_prefix(make_id("", "", "", 3)), "");
}

TEST(SessionEntityPrefix, SanitizesItsInputSoRawUiTextIsSafe) {
    EXPECT_EQ(entity_prefix(make_id("P-01", "pre_1", "rest ")), "sub-P01_ses-pre1_task-rest");
}

// ── Folder names ───────────────────────────────────────────────────────────

// THE backward-compatibility pin. An existing deployment whose operator types
// nothing must keep producing byte-identical folder names.
TEST(SessionFolderName, EmptyIdentityReproducesTheLegacyTimestampExactly) {
    const QDateTime when = fixed_when();
    EXPECT_EQ(build_session_folder_name(SessionIdentity{}, when, kLegacyFormat),
              when.toString(kLegacyFormat));
    EXPECT_EQ(build_session_folder_name(SessionIdentity{}, when, kLegacyFormat),
              "2026-09-06_14-30-12");
}

TEST(SessionFolderName, EmptyIdentityAndNoTimestampIsStillTheLiteralSession) {
    EXPECT_EQ(build_session_folder_name(SessionIdentity{}, fixed_when(), ""), "session");
}

// The compact form is used instead of the operator's format because the
// default format's '_' and '-' are exactly BIDS' separators.
TEST(SessionFolderName, IdentityForcesTheCompactSeparatorFreeTimestamp) {
    EXPECT_EQ(
        build_session_folder_name(make_id("P01", "pre", "rest", 1), fixed_when(), kLegacyFormat),
        "sub-P01_ses-pre_task-rest_run-01_20260906T143012");
}

TEST(SessionFolderName, IdentityWithoutATimestampIsThePrefixAlone) {
    EXPECT_EQ(build_session_folder_name(make_id("P01", "pre", "rest", 2), fixed_when(), ""),
              "sub-P01_ses-pre_task-rest_run-02");
}

// ── Parsing ────────────────────────────────────────────────────────────────

TEST(SessionParse, RoundTripsEverythingBuildProduces) {
    for (const int run : {1, 2, 17}) {
        for (const auto& id : {make_id("P01", "pre", "rest", run), make_id("P01", "", "", run),
                               make_id("", "", "rest", run)}) {
            const QString name = build_session_folder_name(id, fixed_when(), kLegacyFormat);
            const auto p       = parse_session_folder_name(name);
            EXPECT_TRUE(p.hasEntities) << name.toStdString();
            EXPECT_EQ(p.subject, sanitize_label(id.subject)) << name.toStdString();
            EXPECT_EQ(p.session, sanitize_label(id.session)) << name.toStdString();
            EXPECT_EQ(p.task, sanitize_label(id.task)) << name.toStdString();
            EXPECT_EQ(p.run, run) << name.toStdString();
            EXPECT_EQ(p.timestamp, "20260906T143012") << name.toStdString();
        }
    }
}

// Every folder recorded before this feature existed takes this path.
TEST(SessionParse, LegacyTimestampNameHasNoEntitiesAndIsPreservedVerbatim) {
    const auto p = parse_session_folder_name("2026-09-04_12-51-29");
    EXPECT_FALSE(p.hasEntities);
    EXPECT_EQ(p.run, 0);
    EXPECT_EQ(p.timestamp, "2026-09-04_12-51-29");
}

// An entity this scheme doesn't know must survive in the remainder rather than
// being silently dropped from the name.
TEST(SessionParse, UnknownEntityEndsParsingAndIsKept) {
    const auto p = parse_session_folder_name("sub-P01_ses-pre_acq-x_20260906T143012");
    EXPECT_TRUE(p.hasEntities);
    EXPECT_EQ(p.subject, "P01");
    EXPECT_EQ(p.session, "pre");
    EXPECT_EQ(p.task, "");
    EXPECT_EQ(p.timestamp, "acq-x_20260906T143012");
}

TEST(SessionParse, KeysAreCaseSensitive) {
    const auto p = parse_session_folder_name("SUB-P01_20260906T143012");
    EXPECT_FALSE(p.hasEntities);
    EXPECT_EQ(p.timestamp, "SUB-P01_20260906T143012");
}

// Total and non-throwing: any string is legal input.
TEST(SessionParse, HostileInputNeitherCrashesNorHalfParses) {
    for (const QString& name : {QString(""), QString("sub-"), QString("sub-P01_"),
                                QString("task-rest_run-"), QString("run-xx"), QString("run-0"),
                                QString("random_folder"), QString("-"), QString("_")}) {
        const auto p = parse_session_folder_name(name);
        EXPECT_GE(p.run, 0) << name.toStdString();
    }
    // "sub-P01_run-02" has a gap where ses/task would be — still valid.
    const auto gap = parse_session_folder_name("sub-P01_run-02");
    EXPECT_TRUE(gap.hasEntities);
    EXPECT_EQ(gap.subject, "P01");
    EXPECT_EQ(gap.run, 2);
}

// ── Collisions ─────────────────────────────────────────────────────────────

TEST(SessionCollisions, MatchesOnTheTripleIgnoringRunAndTimestamp) {
    const QStringList existing{
        "sub-P01_ses-pre_task-rest_run-01_20260906T100000",
        "sub-P01_ses-pre_task-rest_run-02_20260906T110000",
        "sub-P01_ses-pre_task-nback_run-01_20260906T120000", // different task
        "sub-P02_ses-pre_task-rest_run-01_20260906T130000",  // different subject
    };
    EXPECT_EQ(matching_session_folders(existing, make_id("P01", "pre", "rest")).size(), 2);
    EXPECT_EQ(matching_session_folders(existing, make_id("P01", "pre", "nback")).size(), 1);
    EXPECT_EQ(matching_session_folders(existing, make_id("P09", "pre", "rest")).size(), 0);
}

// The single most important negative test: an operator who types nothing must
// never be shown a collision dialog, however full the directory is.
TEST(SessionCollisions, AnEmptyIdentityNeverCollidesWithLegacyFolders) {
    const QStringList legacy{"2026-09-04_12-51-29", "2026-09-04_12-52-30", "session"};
    const auto report = check_collision(legacy, SessionIdentity{});
    EXPECT_FALSE(report.collides());
    EXPECT_EQ(report.existingCount, 0);
    EXPECT_EQ(report.suggestedRun, 0);
}

TEST(SessionCollisions, LegacyFoldersAreInvisibleToAnIdentifiedRecording) {
    const QStringList legacy{"2026-09-04_12-51-29", "session"};
    EXPECT_FALSE(check_collision(legacy, make_id("P01", "pre", "rest")).collides());
    EXPECT_EQ(next_run_index(legacy, make_id("P01", "pre", "rest")), 1);
}

TEST(SessionCollisions, ReportsCountAndTheNextRun) {
    const QStringList existing{
        "sub-P01_ses-pre_task-rest_run-01_20260906T100000",
        "sub-P01_ses-pre_task-rest_run-02_20260906T110000",
    };
    const auto report = check_collision(existing, make_id("P01", "pre", "rest"));
    EXPECT_TRUE(report.collides());
    EXPECT_EQ(report.existingCount, 2);
    EXPECT_EQ(report.suggestedRun, 3);
}

TEST(SessionRunIndex, FirstRecordingOfATripleIsRunOne) {
    EXPECT_EQ(next_run_index({}, make_id("P01", "pre", "rest")), 1);
}

// The rule that keeps two recordings from ever sharing an identity: deleting a
// middle run must not cause its number to be handed out again.
TEST(SessionRunIndex, TakesOnePastTheHighestNotOnePastTheCount) {
    const QStringList withGap{
        "sub-P01_ses-pre_task-rest_run-01_20260906T100000",
        "sub-P01_ses-pre_task-rest_run-03_20260906T120000", // run-02 was deleted
    };
    EXPECT_EQ(next_run_index(withGap, make_id("P01", "pre", "rest")), 4);
}

TEST(SessionRunIndex, RollsOverPastTwoDigitsCorrectly) {
    const QStringList existing{"sub-P01_ses-pre_task-rest_run-09_20260906T100000"};
    EXPECT_EQ(next_run_index(existing, make_id("P01", "pre", "rest")), 10);
}

// A sibling with no run index at all is malformed, but must still not have its
// implied number reissued.
TEST(SessionRunIndex, SiblingsWithoutARunStillOccupyANumber) {
    const QStringList existing{"sub-P01_ses-pre_task-rest_20260906T100000"};
    EXPECT_EQ(next_run_index(existing, make_id("P01", "pre", "rest")), 2);
}

TEST(SessionRunIndex, AnEmptyIdentityGetsNoRunAtAll) {
    EXPECT_EQ(next_run_index({"2026-09-04_12-51-29"}, SessionIdentity{}), 0);
}

// ── Path budget ────────────────────────────────────────────────────────────

TEST(SessionPathBudget, AcceptsATypicalNameAndRejectsAPathologicalOne) {
    EXPECT_TRUE(fits_path_budget("./recordings/virginie",
                                 "sub-P01_ses-pre_task-rest_run-01_20260906T143012"));

    const QString deep(120, u'd');
    const QString longName(60, u'n');
    EXPECT_FALSE(fits_path_budget(deep, longName));
}

TEST(SessionPathBudget, BoundaryIsInclusive) {
    const QString dir(100, u'd');
    // dir + '/' + name == exactly the budget.
    EXPECT_TRUE(fits_path_budget(dir, QString(mosaic::k_session_path_budget - 101, u'n')));
    EXPECT_FALSE(fits_path_budget(dir, QString(mosaic::k_session_path_budget - 100, u'n')));
}

// ── Ordering ───────────────────────────────────────────────────────────────

TEST(SessionOrder, NewestFirst) {
    const QDateTime older(QDate(2026, 9, 4), QTime(12, 0, 0), QTimeZone::utc());
    const QDateTime newer(QDate(2026, 9, 6), QTime(12, 0, 0), QTimeZone::utc());
    EXPECT_TRUE(session_entry_newer(newer, "b", older, "a"));
    EXPECT_FALSE(session_entry_newer(older, "a", newer, "b"));
}

// A damaged session (no parseable session_start_utc) is buried rather than
// interleaved at an arbitrary point.
TEST(SessionOrder, SessionsWithoutAStartTimeSortLast) {
    const QDateTime valid(QDate(2026, 9, 4), QTime(12, 0, 0), QTimeZone::utc());
    const QDateTime invalid;
    EXPECT_TRUE(session_entry_newer(valid, "a", invalid, "z"));
    EXPECT_FALSE(session_entry_newer(invalid, "z", valid, "a"));
}

TEST(SessionOrder, TwoUndatedSessionsFallBackToNameDescending) {
    const QDateTime invalid;
    EXPECT_TRUE(session_entry_newer(invalid, "b", invalid, "a"));
    EXPECT_FALSE(session_entry_newer(invalid, "a", invalid, "b"));
}

// std::sort with an inconsistent comparator is undefined behaviour, not merely
// a wrong order — so pin the ordering axioms rather than trusting them.
TEST(SessionOrder, ComparatorIsIrreflexiveAndAntisymmetric) {
    struct Entry {
        QDateTime start;
        QString name;
    };
    const QDateTime t1(QDate(2026, 9, 4), QTime(12, 0, 0), QTimeZone::utc());
    const QDateTime t2(QDate(2026, 9, 6), QTime(12, 0, 0), QTimeZone::utc());
    const QVector<Entry> entries{
        {t1, "a"}, {t2, "b"}, {QDateTime{}, "c"}, {t1, "d"}, {QDateTime{}, "a"}};

    for (const auto& x : entries) {
        EXPECT_FALSE(session_entry_newer(x.start, x.name, x.start, x.name));
        for (const auto& y : entries) {
            const bool xy   = session_entry_newer(x.start, x.name, y.start, y.name);
            const bool yx   = session_entry_newer(y.start, y.name, x.start, x.name);
            const bool same = (x.start == y.start) && (x.name == y.name);
            if (!same) {
                EXPECT_NE(xy, yx) << x.name.toStdString() << " vs " << y.name.toStdString();
            }
        }
    }
}

// ── SessionIdentity JSON ───────────────────────────────────────────────────

TEST(SessionIdentityJson, RoundTripsEntitiesAndRun) {
    const auto id   = make_id("P01", "pre", "rest", 3);
    const auto back = SessionIdentity::from_json(id.to_json());
    EXPECT_EQ(back.subject, "P01");
    EXPECT_EQ(back.session, "pre");
    EXPECT_EQ(back.task, "rest");
    EXPECT_EQ(back.run, 3);
}

TEST(SessionIdentityJson, EmptyObjectYieldsAnEmptyIdentity) {
    const auto id = SessionIdentity::from_json({});
    EXPECT_FALSE(id.has_entities());
    EXPECT_EQ(id.run, 0);
}

// settings.json is hand-editable, so a label arriving from it must be
// re-sanitized before it can reach a path.
TEST(SessionIdentityJson, ResanitizesOnLoad) {
    QJsonObject obj{{"sub", "../../etc"}, {"ses", "p re"}, {"task", "re-st"}};
    const auto id = SessionIdentity::from_json(obj);
    EXPECT_EQ(id.subject, "etc");
    EXPECT_EQ(id.session, "pre");
    EXPECT_EQ(id.task, "rest");
}

TEST(SessionIdentityJson, NotesAreNotCarried) {
    SessionIdentity id = make_id("P01", "", "");
    id.notes           = "participant arrived late";
    EXPECT_FALSE(id.to_json().contains("notes"));
}

// has_entities() must ignore notes: attaching a note is not an identity, and
// must not move a recording out of legacy naming.
TEST(SessionIdentityJson, NotesAloneDoNotConstituteAnIdentity) {
    SessionIdentity id;
    id.notes = "some prose";
    EXPECT_FALSE(id.has_entities());
    EXPECT_EQ(build_session_folder_name(id, fixed_when(), kLegacyFormat), "2026-09-06_14-30-12");
}

// Punctuation-only input sanitizes away to nothing, and must not flip the
// recording into a BIDS name with no entities in it.
TEST(SessionIdentityJson, PunctuationOnlyLabelsAreNotEntities) {
    EXPECT_FALSE(make_id("---", "", "").has_entities());
    EXPECT_EQ(build_session_folder_name(make_id("---", "", ""), fixed_when(), kLegacyFormat),
              "2026-09-06_14-30-12");
}

// ── names_a_subject ────────────────────────────────────────────────────────
//
// The rule behind the greyed-out Record button. It is deliberately *not*
// has_entities(): the two answer different questions, and the gap between them
// is exactly the case this guard exists for — a session or task typed with no
// subject produces a perfectly well-formed folder name that nobody can trace
// back to a participant.

TEST(NamesASubject, AnOrdinarySubjectQualifies) {
    EXPECT_TRUE(names_a_subject(make_id("P01", "", "")));
    EXPECT_TRUE(names_a_subject(make_id("P01", "pre", "rest")));
}

TEST(NamesASubject, NothingTypedAtAllDoesNot) { EXPECT_FALSE(names_a_subject(SessionIdentity{})); }

// The whole reason this is a separate predicate. Both of these have entities,
// so has_entities() says yes and a folder name is produced — but neither names
// a participant, which is the one thing that cannot be recovered afterwards.
TEST(NamesASubject, SessionAndTaskWithoutASubjectDoNot) {
    const auto id = make_id("", "pre", "rest");
    EXPECT_TRUE(id.has_entities());
    EXPECT_FALSE(names_a_subject(id));
    EXPECT_EQ(build_session_folder_name(id, fixed_when(), kLegacyFormat),
              "ses-pre_task-rest_20260906T143012");
}

// Judged after sanitizing, like has_entities(). A field the operator can see
// text in but that contributes nothing to the folder name is not a subject —
// this is the case where a naive isEmpty() check on the raw text would let an
// unattributable recording straight through.
TEST(NamesASubject, PunctuationOnlySubjectDoesNot) {
    EXPECT_FALSE(names_a_subject(make_id("---", "", "")));
    EXPECT_FALSE(names_a_subject(make_id("???", "pre", "rest")));
}

// Truncation must not be able to empty a subject that had real characters in
// it, or a long id would silently become unrecordable.
TEST(NamesASubject, AnOverlongSubjectStillQualifies) {
    const QString longSub(k_max_label_chars * 3, QChar('P'));
    EXPECT_TRUE(names_a_subject(make_id(longSub, "", "")));
}

// Notes are not an identity, here for the same reason they are not in
// has_entities(): attaching prose to a recording says nothing about who it is of.
TEST(NamesASubject, NotesDoNotStandInForASubject) {
    SessionIdentity id;
    id.notes = "P01, arrived late";
    EXPECT_FALSE(names_a_subject(id));
}

// ── advise_identity ────────────────────────────────────────────────────────
//
// The single judgement both UI surfaces render — the inline bar in the monitor
// and the dialog that opens when Record is pressed without a usable identity.
// These assert on the IdentityIssue category rather than on the prose, so
// rewording a message does not break them but a misclassification does.

namespace {

const QString kDir = "recordings/lab";

IdentityAdvice advise(const SessionIdentity& id, const QStringList& existing = {},
                      const QString& dir = kDir) {
    return advise_identity(id, existing, dir, fixed_when(), kLegacyFormat);
}

} // namespace

TEST(AdviseIdentity, AnOrdinaryIdentityIsRecordableAndNamed) {
    const auto a = advise(make_id("P01", "pre", "rest"));
    EXPECT_TRUE(a.canRecord);
    EXPECT_EQ(a.issue, IdentityIssue::None);
    EXPECT_TRUE(a.warning.isEmpty());
    EXPECT_EQ(a.folderName, "sub-P01_ses-pre_task-rest_run-01_20260906T143012");
}

TEST(AdviseIdentity, NoSubjectIsNotRecordableAndOffersNoName) {
    const auto a = advise(SessionIdentity{});
    EXPECT_FALSE(a.canRecord);
    EXPECT_EQ(a.issue, IdentityIssue::NoSubject);
    // Empty, not the timestamp fallback: an accurate preview of a folder the
    // operator cannot create is worse than no preview at all.
    EXPECT_TRUE(a.folderName.isEmpty());
}

// An over-long name is the *other* way canRecord goes false, and there the name
// must survive — it is the thing the operator has to read in order to shorten
// it. Pins the distinction folderPreview() relies on to tell the two apart.
TEST(AdviseIdentity, AnOverBudgetNameIsStillShown) {
    const QString deepDir(mosaic::k_session_path_budget - 20, QChar('d'));
    const auto a = advise(make_id("P01", "pre", "rest"), {}, deepDir);
    EXPECT_FALSE(a.canRecord);
    EXPECT_FALSE(a.folderName.isEmpty());
}

// A subject-less identity must not quote a run number. With session and task
// carried over from the last participant, the sibling match finds the
// subject-less folders a trigger can still produce — a different combination
// entirely from the one the dialog is asking about, and the count changes the
// moment a subject is typed.
TEST(AdviseIdentity, NoSubjectReportsNoCollision) {
    const QStringList existing{
        "ses-pre_task-rest_run-01_20260906T100000",
        "ses-pre_task-rest_run-02_20260906T110000",
    };
    // The siblings really do match, so this is not vacuous.
    EXPECT_EQ(matching_session_folders(existing, make_id("", "pre", "rest")).size(), 2);

    const auto a = advise(make_id("", "pre", "rest"), existing);
    EXPECT_FALSE(a.canRecord);
    EXPECT_FALSE(a.collision.collides());
    EXPECT_EQ(a.collision.existingCount, 0);
    EXPECT_EQ(a.collision.suggestedRun, 0);
}

// Session and task are not a substitute. Without this the operator can produce
// "ses-pre_task-rest_...", a well-formed folder naming no participant.
TEST(AdviseIdentity, SessionAndTaskDoNotRescueAMissingSubject) {
    const auto a = advise(make_id("", "pre", "rest"));
    EXPECT_FALSE(a.canRecord);
    EXPECT_EQ(a.issue, IdentityIssue::NoSubject);
    EXPECT_TRUE(a.folderName.isEmpty());
}

TEST(AdviseIdentity, ASubjectThatSanitizesAwayIsItsOwnCase) {
    const auto a = advise(make_id("???", "", ""));
    EXPECT_FALSE(a.canRecord);
    EXPECT_EQ(a.issue, IdentityIssue::SubjectUnusable);
    // The raw text comes back so the message can name what was typed, rather
    // than telling the operator to fill in a field they can see is full.
    EXPECT_TRUE(a.warning.contains("???"));
}

// Pins the loop order: whichever field is wrong first is the one reported, so
// the message cannot flip between fields as unrelated text is edited.
TEST(AdviseIdentity, ReportsTheFirstProblemOnly) {
    const QString longSub(k_max_label_chars + 6, QChar('P'));
    const auto a = advise(make_id(longSub, "", "re st"));
    EXPECT_EQ(a.issue, IdentityIssue::LabelTruncated);
    EXPECT_TRUE(a.warning.contains("Subject"));
    EXPECT_FALSE(a.warning.contains("Task"));
}

// The subtle one. sanitize_label(raw, raw.size()) == raw is what distinguishes
// "too long" from "contains characters BIDS forbids" — get it backwards and a
// perfectly legal long id is reported as containing illegal characters.
TEST(AdviseIdentity, TruncationAndIllegalCharactersAreDifferentIssues) {
    const QString longButLegal(k_max_label_chars + 6, QChar('P'));
    EXPECT_EQ(advise(make_id(longButLegal, "", "")).issue, IdentityIssue::LabelTruncated);
    EXPECT_EQ(advise(make_id("P-01", "", "")).issue, IdentityIssue::LabelCoerced);
    // Dropped entirely is reachable for session/task only — a subject that
    // sanitizes away is SubjectUnusable, above.
    EXPECT_EQ(advise(make_id("P01", "---", "")).issue, IdentityIssue::LabelDropped);

    // None of the three blocks recording: they are cosmetic, and the preview
    // already shows what will really be created.
    EXPECT_TRUE(advise(make_id(longButLegal, "", "")).canRecord);
    EXPECT_TRUE(advise(make_id("P-01", "", "")).canRecord);
    EXPECT_TRUE(advise(make_id("P01", "---", "")).canRecord);
}

// The bug this struct was reshaped to prevent: an over-budget name used to be
// refused at click time while the button stayed green and the warning line was
// busy reporting something else. canRecord has to mean "pressing Start will
// actually arm".
TEST(AdviseIdentity, AnOverBudgetNameIsNotRecordable) {
    const QString deepDir(mosaic::k_session_path_budget - 20, QChar('d'));
    const auto a = advise(make_id("P01", "pre", "rest"), {}, deepDir);
    EXPECT_FALSE(a.canRecord);
    EXPECT_EQ(a.issue, IdentityIssue::NameTooLong);
}

// A cosmetic label note must never be the only thing shown beside a disabled
// Start button. "Report the first problem only" is right for cosmetic issues
// and wrong for a refusal: an operator told about a coerced hyphen, with no
// mention of path length, has been given a dead button and a red herring.
TEST(AdviseIdentity, ALabelWarningDoesNotMaskAnOverBudgetRefusal) {
    const QString deepDir(mosaic::k_session_path_budget - 20, QChar('d'));
    const auto a = advise(make_id("P-01", "pre", "rest"), {}, deepDir);
    EXPECT_FALSE(a.canRecord);
    // Both are stated: the label coercion the operator can see, and the reason
    // the button is dead, which they cannot.
    EXPECT_TRUE(a.warning.contains("only letters and digits"));
    EXPECT_TRUE(a.warning.contains("too long"));
    // The blocking issue wins the category, because that is what the surfaces
    // key off when deciding whether to suppress or highlight a message.
    EXPECT_EQ(a.issue, IdentityIssue::NameTooLong);
}

// The budget is measured against the name that will actually be created, which
// is the whole reason the run index is resolved before the measurement. A
// directory sized to fit run-01 but not run-100 must reject the latter.
TEST(AdviseIdentity, TheBudgetIsMeasuredWithTheRunIndexResolved) {
    const auto id = make_id("P01", "pre", "rest");
    const QString atRun1 =
        build_session_folder_name(make_id("P01", "pre", "rest", 1), fixed_when(), kLegacyFormat);
    // Exactly enough room for the run-01 form and nothing more.
    const QString dir(mosaic::k_session_path_budget - atRun1.size() - 1, QChar('d'));
    EXPECT_TRUE(advise(id, {}, dir).canRecord);

    // 99 siblings push it to run-100, one character longer, over the edge.
    QStringList many;
    for (int i = 1; i <= 99; ++i) {
        many << QString("sub-P01_ses-pre_task-rest_run-%1_20260906T10%2")
                    .arg(i, 2, 10, QChar('0'))
                    .arg(i, 4, 10, QChar('0'));
    }
    const auto crowded = advise(id, many, dir);
    EXPECT_EQ(crowded.collision.suggestedRun, 100);
    EXPECT_FALSE(crowded.canRecord);
}

// A duplicate is a question, not a refusal — nothing is overwritten either way.
TEST(AdviseIdentity, ARepeatedCombinationStillRecords) {
    const QStringList existing{
        "sub-P01_ses-pre_task-rest_run-01_20260906T100000",
        "sub-P01_ses-pre_task-rest_run-02_20260906T110000",
    };
    const auto a = advise(make_id("P01", "pre", "rest"), existing);
    EXPECT_TRUE(a.canRecord);
    EXPECT_TRUE(a.collision.collides());
    EXPECT_EQ(a.collision.existingCount, 2);
    EXPECT_EQ(a.collision.suggestedRun, 3);
    EXPECT_TRUE(a.folderName.contains("run-03"));
}

// max(highest)+1 end to end, not count+1: deleting a middle run must never
// cause its number to be handed to a second recording.
TEST(AdviseIdentity, ADeletedMiddleRunIsNeverReissued) {
    const QStringList existing{
        "sub-P01_ses-pre_task-rest_run-01_20260906T100000",
        "sub-P01_ses-pre_task-rest_run-03_20260906T120000",
    };
    const auto a = advise(make_id("P01", "pre", "rest"), existing);
    EXPECT_EQ(a.collision.existingCount, 2);
    EXPECT_EQ(a.collision.suggestedRun, 4);
}

// MonitorBridge feeds this struct back in as input, so a run index left over
// from a previous pass has to be overwritten — otherwise a stale number would
// be measured against the path budget and shown in the preview.
TEST(AdviseIdentity, AStaleRunIndexOnTheInputIsOverwritten) {
    const auto a = advise(make_id("P01", "pre", "rest", 99));
    EXPECT_EQ(a.collision.suggestedRun, 1);
    EXPECT_TRUE(a.folderName.contains("run-01"));
    EXPECT_FALSE(a.folderName.contains("run-99"));
}

// Pure: no clock, no filesystem, no hidden state. Guards against someone
// reaching for QDateTime::currentDateTime() inside, which would make every
// expectation above non-deterministic.
TEST(AdviseIdentity, IsPureInItsArguments) {
    const auto id = make_id("P01", "pre", "rest");
    const auto a  = advise(id);
    const auto b  = advise(id);
    EXPECT_EQ(a.folderName, b.folderName);
    EXPECT_EQ(a.warning, b.warning);
    EXPECT_EQ(a.canRecord, b.canRecord);

    const auto later = advise_identity(id, {}, kDir, fixed_when().addSecs(3600), kLegacyFormat);
    EXPECT_NE(a.folderName, later.folderName);
}

// The legacy timestamp-only fallback and the subject requirement must not
// fight. The fallback stays reachable by a trigger, so the naming rule is
// untouched; it simply is not recordable by hand.
TEST(AdviseIdentity, TheLegacyFallbackSurvivesButIsNotRecordableByHand) {
    const auto a = advise_identity(SessionIdentity{}, {}, kDir, fixed_when(), QString());
    EXPECT_FALSE(a.canRecord);
    EXPECT_EQ(build_session_folder_name(SessionIdentity{}, fixed_when(), QString()), "session");

    // Timestamps off with an identity set: the prefix alone, still recordable.
    const auto named =
        advise_identity(make_id("P01", "pre", "rest"), {}, kDir, fixed_when(), QString());
    EXPECT_TRUE(named.canRecord);
    EXPECT_EQ(named.folderName, "sub-P01_ses-pre_task-rest_run-01");
}

// One match pass, not three. check_collision() used to re-derive the sibling
// list that next_run_index() had already built, and advise_identity() calls
// both — so the count and the index have to stay consistent now they come off
// a single scan.
TEST(AdviseIdentity, TheCountAndTheRunIndexComeFromOneScan) {
    const QStringList existing{
        "sub-P01_ses-pre_task-rest_run-01_20260906T100000",
        "sub-P01_ses-pre_task-nback_run-01_20260906T110000", // different task
        "2026-09-04_12-51-29",                               // legacy, invisible
    };
    const auto a = advise(make_id("P01", "pre", "rest"), existing);
    EXPECT_EQ(a.collision.existingCount, 1);
    EXPECT_EQ(a.collision.suggestedRun, 2);
    EXPECT_EQ(check_collision(existing, make_id("P01", "pre", "rest")).suggestedRun,
              a.collision.suggestedRun);
}
