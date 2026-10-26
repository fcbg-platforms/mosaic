#include <gtest/gtest.h>

#include <QSet>
#include <QStringList>

#include "analysis/analysis_plugins.hpp"

using mosaic::analysis_plugin_categories;
using mosaic::analysis_plugin_category_label;
using mosaic::analysis_plugin_for;
using mosaic::analysis_plugin_index_of;
using mosaic::analysis_plugins;
using mosaic::AnalysisPluginCategory;

// The anti-desync test. AnalysisTabW registers each controls page against a
// plugin id rather than appending it positionally, so this round-trip plus
// that keying is what makes "the picker's Nth entry shows the Nth page's
// controls" true by construction instead of by convention — the exact
// assumption that used to hold the combo and the stack together with nothing
// enforcing it.
TEST(AnalysisPlugins, IndexOfRoundTripsEveryIdToItsOwnPosition) {
    const auto& plugins = analysis_plugins();
    ASSERT_FALSE(plugins.isEmpty());
    for (int i = 0; i < plugins.size(); ++i) {
        EXPECT_EQ(analysis_plugin_index_of(plugins[i].id), i)
            << "id = " << plugins[i].id.toStdString();
    }
}

TEST(AnalysisPlugins, UnknownAndEmptyIdsResolveToMinusOne) {
    EXPECT_EQ(analysis_plugin_index_of("no_such_plugin"), -1);
    EXPECT_EQ(analysis_plugin_index_of(""), -1);
    EXPECT_EQ(analysis_plugin_for("no_such_plugin"), nullptr);
    EXPECT_EQ(analysis_plugin_for(""), nullptr);
}

TEST(AnalysisPlugins, LookupReturnsTheMatchingEntry) {
    const auto* pose = analysis_plugin_for("pose");
    ASSERT_NE(pose, nullptr);
    EXPECT_EQ(pose->id, "pose");
    EXPECT_FALSE(pose->label.isEmpty());
}

// Duplicate ids would make the id->page registration silently drop a page;
// duplicate labels would make two picker rows indistinguishable.
TEST(AnalysisPlugins, IdsAndLabelsAreUniqueAndNonEmpty) {
    QSet<QString> ids;
    QSet<QString> labels;
    for (const auto& plugin : analysis_plugins()) {
        EXPECT_FALSE(plugin.id.isEmpty());
        EXPECT_FALSE(plugin.label.isEmpty());
        EXPECT_FALSE(plugin.blurb.isEmpty()) << "id = " << plugin.id.toStdString();
        EXPECT_FALSE(ids.contains(plugin.id)) << "duplicate id " << plugin.id.toStdString();
        EXPECT_FALSE(labels.contains(plugin.label))
            << "duplicate label " << plugin.label.toStdString();
        ids.insert(plugin.id);
        labels.insert(plugin.label);
    }
}

// The failure this catches: someone adds an 11th plugin in a brand-new
// category and forgets analysis_plugin_categories(). The picker renders no
// heading for it, so the plugin vanishes entirely — with no error, and
// nothing else in the system would notice.
TEST(AnalysisPlugins, DisplayedCategoriesAndCategoriesInUseAreTheSameSet) {
    QSet<int> inUse;
    for (const auto& plugin : analysis_plugins()) {
        inUse.insert(static_cast<int>(plugin.category));
    }

    QSet<int> displayed;
    for (const auto category : analysis_plugin_categories()) {
        const int raw = static_cast<int>(category);
        EXPECT_FALSE(displayed.contains(raw)) << "category listed twice";
        displayed.insert(raw);
    }

    EXPECT_EQ(inUse, displayed);
}

TEST(AnalysisPlugins, EveryDisplayedCategoryHasANonEmptyLabel) {
    for (const auto category : analysis_plugin_categories()) {
        EXPECT_FALSE(analysis_plugin_category_label(category).isEmpty());
    }
}

// A tripwire, not a proof. run_analysis()'s dispatch, the is_*_plugin()
// predicates and the run-state lookup all live in ui/, which this test binary
// cannot link (Qt6::Widgets is deliberately not a dependency). Failing here
// forces whoever adds or renames a plugin to walk the whole checklist:
// registry -> controls page registration -> run_analysis() arm ->
// is_*_plugin() predicate -> run_state_for() arm.
TEST(AnalysisPlugins, RegistryMatchesTheKnownPluginIdSet) {
    QStringList ids;
    for (const auto& plugin : analysis_plugins()) {
        ids << plugin.id;
    }
    ids.sort();

    QStringList expected{
        "conversation", "diarize",     "expression", "eye_contact", "face_dynamics", "face_mask",
        "gaze2d",       "gaze_fusion", "hands",      "objects",     "overview",      "pose",
        "pose3d",       "rppg",        "social",     "sync_repair", "trigger_sync"};
    expected.sort();

    EXPECT_EQ(ids, expected);
}

// The picker's order is the order to run the analyses in: by category, then
// by place in the registry. Pinned, so a reorder is a deliberate change.
TEST(AnalysisPlugins, PickerListsTheAnalysesInTheOrderToRunThem) {
    QStringList shown;
    for (const auto category : analysis_plugin_categories()) {
        for (const auto& plugin : analysis_plugins()) {
            if (plugin.category == category) {
                shown << plugin.label;
            }
        }
    }
    const QStringList expected{"Frame Sync",
                               "EEG / Trigger Sync",
                               "Face Masking",
                               "Pose",
                               "Hands",
                               "3D Pose",
                               "Object Tracking",
                               "2D Gaze",
                               "Face Dynamics",
                               "Facial Expression",
                               "Eye Contact",
                               "3D Gaze",
                               "Heart Rate (experimental)",
                               "Speaker Diarization",
                               "Conversation Timing",
                               "Social Interaction",
                               "Session Overview"};
    EXPECT_EQ(shown, expected);
    // Pose first in the registry: the tab starts on it.
    EXPECT_EQ(analysis_plugins().first().id, "pose");
}

// The picker allots a fixed row height for label + blurb, so an over-long
// blurb would be silently elided away rather than wrapping.
TEST(AnalysisPlugins, BlurbsAreShortEnoughForOneRow) {
    for (const auto& plugin : analysis_plugins()) {
        EXPECT_LE(plugin.blurb.size(), 70) << "id = " << plugin.id.toStdString();
    }
}
