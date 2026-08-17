#include <gtest/gtest.h>

#include "core/recording_access_control.hpp"

using mosaic::default_record_directory_for;
using mosaic::is_legacy_shared_record_directory;
using mosaic::legacy_shared_record_directory;
using mosaic::resolve_migration_target;
using mosaic::unassigned_record_directory;

TEST(RecordingAccessControl, LegacySharedDirectoryIsTheExpectedConstant) {
    EXPECT_EQ(legacy_shared_record_directory(), "./recordings");
}

TEST(RecordingAccessControl, DefaultRecordDirectoryForBuildsPerUserSubfolder) {
    EXPECT_EQ(default_record_directory_for("alice"), "./recordings/alice");
    EXPECT_EQ(default_record_directory_for("bob"), "./recordings/bob");
}

TEST(RecordingAccessControl, IsLegacySharedRecordDirectoryOnlyMatchesExactDefault) {
    EXPECT_TRUE(is_legacy_shared_record_directory("./recordings"));
    // A customized directory — even one that merely starts with the legacy
    // default — must never be mistaken for "still unmodified", or the
    // migration logic would silently overwrite a deliberate user choice.
    EXPECT_FALSE(is_legacy_shared_record_directory("./recordings/alice"));
    EXPECT_FALSE(is_legacy_shared_record_directory("./recordings2"));
    EXPECT_FALSE(is_legacy_shared_record_directory("D:/custom/path"));
    EXPECT_FALSE(is_legacy_shared_record_directory(""));
}

TEST(RecordingAccessControl, ResolveMigrationTargetGoesToKnownUsersOwnDirectory) {
    const QSet<QString> known{"alice", "bob"};
    EXPECT_EQ(resolve_migration_target("alice", known), default_record_directory_for("alice"));
    EXPECT_EQ(resolve_migration_target("bob", known), default_record_directory_for("bob"));
}

TEST(RecordingAccessControl, ResolveMigrationTargetFallsBackToUnassignedForUnknownUser) {
    const QSet<QString> known{"alice", "bob"};
    EXPECT_EQ(resolve_migration_target("someone_deleted", known), unassigned_record_directory());
}

TEST(RecordingAccessControl, ResolveMigrationTargetFallsBackToUnassignedForEmptyRecordedBy) {
    const QSet<QString> known{"alice", "bob"};
    EXPECT_EQ(resolve_migration_target("", known), unassigned_record_directory());
}

TEST(RecordingAccessControl, ResolveMigrationTargetHandlesEmptyKnownUsernames) {
    // No registered profiles at all (e.g. a fresh/corrupted profiles.json)
    // must still resolve to the safe fallback, never crash.
    EXPECT_EQ(resolve_migration_target("alice", {}), unassigned_record_directory());
}
