#pragma once
#include <QString>
#include <QVector>

namespace mosaic {

/// @brief The canonical registry of Analysis-tab plugins: id, display label,
/// one-line description and category, in one place.
///
/// This exists to make a specific class of bug impossible. The Analysis tab
/// used to hold two manually-maintained parallel sequences — a QComboBox's
/// addItem() calls and a QStackedWidget's addWidget() calls — coupled only by
/// the assumption that the Nth item and the Nth page belonged together.
/// Nothing enforced it, and getting it wrong showed the wrong plugin's
/// controls with no error at all. Identity is now the id string everywhere
/// (it already was for the run dispatch and the is_*_plugin() predicates);
/// pages are registered against that id rather than appended positionally.
///
/// Deliberately QtCore-only and header-only, matching pose_models.hpp: the
/// test target links Qt6::Core and Qt6::Network but not Qt6::Widgets, so
/// anything living in ui/ cannot be unit-tested. Keeping the registry here
/// is what lets tests/test_analysis_plugins.cpp guard it.

/// Groups the plugins in the order they are run: data preparation first,
/// then what each person does (body, then face and gaze), the audio, and
/// last what combines the others. The picker shows these as numbered
/// headings, so the order to work in can be read off the list.
enum class AnalysisPluginCategory {
    Prepare,
    BodyMotion,
    FaceGaze,
    Audio,
    Together,
};

struct AnalysisPluginDesc {
    /// Stable identifier. Also AnalysisTabW::Impl::currentPlugin, the key for
    /// run_analysis()'s dispatch, and the value compared by every
    /// is_*_plugin() predicate. Never shown to the user; never reorder-
    /// sensitive.
    QString id;
    /// Short enough to fit the picker without eliding at its default width.
    QString label;
    /// One line, shown under the label and as the row's tooltip; says what
    /// must run first ("Needs") or helps when it has ("Best after").
    QString blurb;
    AnalysisPluginCategory category;
};

/// Every plugin. Within a category the picker lists them in this order,
/// which is the order to run them in (an analysis after the ones it uses).
///
/// The first entry is load-bearing: AnalysisTabW seeds its current plugin
/// from it at construction, and the tab's startup visibility pass only lands
/// correctly for a plugin whose visible widgets are the ones already visible
/// at construction time. That is Pose. See AnalysisTabW's constructor.
[[nodiscard]] inline const QVector<AnalysisPluginDesc>& analysis_plugins() {
    static const QVector<AnalysisPluginDesc> kPlugins = {
        {"pose", "Pose", "Per-person 2D skeletons and keypoint kinematics.",
         AnalysisPluginCategory::BodyMotion},
        {"hands", "Hands", "21 points per hand, left and right, for close-up cameras. Needs Pose.",
         AnalysisPluginCategory::BodyMotion},
        {"pose3d", "3D Pose", "Skeletons in 3D. Needs Pose and the room calibration.",
         AnalysisPluginCategory::BodyMotion},
        {"objects", "Object Tracking",
         "Toys, cups, tablets found by name (SAM 3), and who touches them.",
         AnalysisPluginCategory::BodyMotion},
        {"sync_repair", "Frame Sync", "Lines the cameras up frame by frame; exports synced videos.",
         AnalysisPluginCategory::Prepare},
        {"trigger_sync", "EEG / Trigger Sync", "Maps trigger events onto each camera's frames.",
         AnalysisPluginCategory::Prepare},
        {"face_mask", "Face Masking", "Writes an anonymized copy; never touches the original.",
         AnalysisPluginCategory::Prepare},
        {"gaze2d", "2D Gaze", "Per-face gaze direction in image space. No calibration needed.",
         AnalysisPluginCategory::FaceGaze},
        {"face_dynamics", "Face Dynamics",
         "Blinks, smiles, brow raises, nods. Best in interview mode.",
         AnalysisPluginCategory::FaceGaze},
        {"expression", "Facial Expression", "Per-face emotion labels from facial landmarks.",
         AnalysisPluginCategory::FaceGaze},
        {"eye_contact", "Eye Contact",
         "Looking at the partner or away. Best after Conversation Timing.",
         AnalysisPluginCategory::FaceGaze},
        {"gaze_fusion", "3D Gaze", "Who looks at whom, in 3D. Needs the room calibration.",
         AnalysisPluginCategory::FaceGaze},
        // "experimental" stays in the label, not only the blurb: this
        // plugin's whole safety framing depends on that caveat being
        // impossible to miss (see Impl::rppgDisclaimerLbl).
        {"rppg", "Heart Rate (experimental)", "Estimates pulse from facial colour change (rPPG).",
         AnalysisPluginCategory::FaceGaze},
        {"diarize", "Speaker Diarization", "Transcribes each mic and labels who is speaking.",
         AnalysisPluginCategory::Audio},
        {"conversation", "Conversation Timing",
         "Turns, response times, overlaps. Best after Speaker Diarization.",
         AnalysisPluginCategory::Audio},
        {"social", "Social Interaction",
         "Synchrony and mimicry. Needs Face Dynamics, Pose, Heart Rate.",
         AnalysisPluginCategory::Together},
        {"overview", "Session Overview",
         "Speaking, eye contact, smile, head and heart rate on one timeline.",
         AnalysisPluginCategory::Together},
    };
    return kPlugins;
}

/// Categories in picker display order. A category missing here renders no
/// heading, and every plugin in it silently disappears from the picker — so
/// tests/test_analysis_plugins.cpp asserts this list and the categories
/// actually in use are the same set.
[[nodiscard]] inline QVector<AnalysisPluginCategory> analysis_plugin_categories() {
    return {
        AnalysisPluginCategory::Prepare,  AnalysisPluginCategory::BodyMotion,
        AnalysisPluginCategory::FaceGaze, AnalysisPluginCategory::Audio,
        AnalysisPluginCategory::Together,
    };
}

[[nodiscard]] inline QString analysis_plugin_category_label(AnalysisPluginCategory category) {
    switch (category) {
        case AnalysisPluginCategory::Prepare:
            return "1 · Prepare (run first)";
        case AnalysisPluginCategory::BodyMotion:
            return "2 · Body & motion";
        case AnalysisPluginCategory::FaceGaze:
            return "3 · Face & gaze";
        case AnalysisPluginCategory::Audio:
            return "4 · Audio";
        case AnalysisPluginCategory::Together:
            return "5 · Together (run last)";
    }
    return {};
}

/// Position of `id` in analysis_plugins(), or -1 if there is no such plugin.
/// -1 is a normal answer, not an error: an empty id (nothing selected yet) and
/// an unknown one both land here, and callers are expected to bail rather than
/// index with it.
[[nodiscard]] inline int analysis_plugin_index_of(const QString& id) {
    const auto& plugins = analysis_plugins();
    for (int i = 0; i < plugins.size(); ++i) {
        if (plugins[i].id == id) {
            return i;
        }
    }
    return -1;
}

/// The registry entry for `id`, or nullptr if unknown.
[[nodiscard]] inline const AnalysisPluginDesc* analysis_plugin_for(const QString& id) {
    const int index = analysis_plugin_index_of(id);
    return index < 0 ? nullptr : &analysis_plugins()[index];
}

} // namespace mosaic
