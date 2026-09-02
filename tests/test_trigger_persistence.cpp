#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

#include "core/settings.hpp"

using mosaic::KeyTriggerConfig;
using mosaic::TriggerSettings;

namespace {

// A keyboard-trigger entry as written by a build that predates
// KeyTriggerConfig::code — i.e. no "code" key at all.
QJsonObject legacy_key_trigger(const QString& name, const QString& keySeq) {
    return QJsonObject{{"name", name}, {"key_seq", keySeq}, {"enabled", true}, {"action", "Log"}};
}

} // namespace

// Upgrade path. Every entry in an old settings.json lacks "code", so without
// special handling they would all load with the struct default and write an
// indistinguishable column of 1s to trigger.csv — exactly the failure the
// add-button's next-free-code prefill exists to prevent, just arriving via a
// different route.
TEST(TriggerPersistence, LegacyTriggersWithoutCodeGetSequentialCodes) {
    QJsonArray keys;
    keys.append(legacy_key_trigger("Experiment start", "F9"));
    keys.append(legacy_key_trigger("Trial onset", "F11"));
    keys.append(legacy_key_trigger("Note", "F12"));

    const auto loaded = TriggerSettings::from_json(QJsonObject{{"keyboard_triggers", keys}});
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->keyboardTriggers.size(), 3u);

    EXPECT_EQ(loaded->keyboardTriggers[0].code, 1);
    EXPECT_EQ(loaded->keyboardTriggers[1].code, 2);
    EXPECT_EQ(loaded->keyboardTriggers[2].code, 3);

    // The rest of the entry must survive untouched.
    EXPECT_EQ(loaded->keyboardTriggers[1].name, "Trial onset");
    EXPECT_EQ(loaded->keyboardTriggers[1].keySeq, "F11");
}

// An explicit code always wins — the sequential assignment above is strictly a
// fallback for entries that have none, never an override.
TEST(TriggerPersistence, ExplicitCodeIsPreservedAndNotRenumbered) {
    QJsonArray keys;
    QJsonObject withCode = legacy_key_trigger("Marker", "F5");
    withCode["code"]     = 77;
    keys.append(withCode);
    keys.append(legacy_key_trigger("No code here", "F6"));

    const auto loaded = TriggerSettings::from_json(QJsonObject{{"keyboard_triggers", keys}});
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->keyboardTriggers.size(), 2u);

    EXPECT_EQ(loaded->keyboardTriggers[0].code, 77);
    // The second entry has no code of its own and takes its position-derived
    // fallback, which may collide with an explicit code elsewhere — acceptable,
    // since the user can see and edit both, and the alternative (renumbering
    // around explicit codes) would silently move values the user chose.
    EXPECT_EQ(loaded->keyboardTriggers[1].code, 2);
}

TEST(TriggerPersistence, CodeRoundTripsThroughToJson) {
    KeyTriggerConfig c;
    c.name   = "Trial onset";
    c.keySeq = "F11";
    c.code   = 42;

    const auto parsed = KeyTriggerConfig::from_json(c.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->code, 42);
    EXPECT_EQ(parsed->name, "Trial onset");
    EXPECT_EQ(parsed->keySeq, "F11");
}
