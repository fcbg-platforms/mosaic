#include <gtest/gtest.h>

#include <QDateTime>
#include <QTimeZone>
#include <QVector>

#include "session/session_name.hpp"

using mosaic::build_session_folder_name;
using mosaic::check_collision;
using mosaic::entity_prefix;
using mosaic::fits_path_budget;
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
