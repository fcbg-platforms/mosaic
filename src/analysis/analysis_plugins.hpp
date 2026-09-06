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

/// Groups the plugins by what they are *for*. The picker shows these as
/// headings, because a flat list of ten put data-prep utilities (frame-sync
/// repair, EEG/trigger alignment) beside real analyses with no visual
/// distinction between them.
enum class AnalysisPluginCategory {
    BodyMotion,
    FaceGaze,
    Audio,
    Privacy,
    DataPrep,
};

struct AnalysisPluginDesc {
    /// Stable identifier. Also AnalysisTabW::Impl::currentPlugin, the key for
    /// run_analysis()'s dispatch, and the value compared by every
    /// is_*_plugin() predicate. Never shown to the user; never reorder-
    /// sensitive.
    QString id;
    /// Short enough to fit the picker without eliding at its default width.
    QString label;
    /// One line, shown under the label and as the row's tooltip.
    QString blurb;
    AnalysisPluginCategory category;
};

/// Every plugin, in the order the controls stack pages are built.
///
/// Order is preserved from the original combo purely so the stack's page
/// registration stays a straight one-line-per-plugin edit; nothing depends on
/// it any more. Display grouping is applied at render time from `category`.
///
/// The first entry is load-bearing: AnalysisTabW seeds its current plugin
/// from it at construction, and the tab's startup visibility pass only lands
/// correctly for a plugin whose visible widgets are the ones already visible
/// at construction time. That is Pose today. See AnalysisTabW's constructor.
[[nodiscard]] inline const QVector<AnalysisPluginDesc>& analysis_plugins() {
    static const QVector<AnalysisPluginDesc> kPlugins = {
        {"pose", "Pose", "Per-person 2D skeletons and keypoint kinematics.",
         AnalysisPluginCategory::BodyMotion},
        {"face_mask", "Face Masking", "Writes an anonymized copy; never touches the original.",
         AnalysisPluginCategory::Privacy},
        {"diarize", "Speaker Diarization", "Transcribes each mic and labels who is speaking.",
         AnalysisPluginCategory::Audio},
        {"expression", "Facial Expression", "Per-face emotion labels from facial landmarks.",
         AnalysisPluginCategory::FaceGaze},
        {"gaze_fusion", "Gaze Fusion", "Combines cameras into one 3D gaze ray. Needs calibration.",
         AnalysisPluginCategory::FaceGaze},
        {"pose3d", "3D Pose Reconstruction",
         "Triangulates 2D poses into 3D. Needs Pose and calibration.",
         AnalysisPluginCategory::BodyMotion},
        {"trigger_sync", "EEG / Trigger Sync", "Maps trigger events onto each camera's frames.",
         AnalysisPluginCategory::DataPrep},
        // "experimental" stays in the label, not only the blurb: this
        // plugin's whole safety framing depends on that caveat being
        // impossible to miss (see Impl::rppgDisclaimerLbl).
        {"rppg", "Heart Rate (experimental)", "Estimates pulse from facial colour change (rPPG).",
         AnalysisPluginCategory::FaceGaze},
        {"gaze2d", "2D Gaze", "Per-face gaze direction in image space. No calibration needed.",
         AnalysisPluginCategory::FaceGaze},
        {"sync_repair", "Frame Sync Repair", "Equalizes camera frame counts into synced/.",
         AnalysisPluginCategory::DataPrep},
    };
    return kPlugins;
}

/// Categories in picker display order. A category missing here renders no
/// heading, and every plugin in it silently disappears from the picker — so
/// tests/test_analysis_plugins.cpp asserts this list and the categories
/// actually in use are the same set.
[[nodiscard]] inline QVector<AnalysisPluginCategory> analysis_plugin_categories() {
    return {
        AnalysisPluginCategory::BodyMotion, AnalysisPluginCategory::FaceGaze,
        AnalysisPluginCategory::Audio,      AnalysisPluginCategory::Privacy,
        AnalysisPluginCategory::DataPrep,
    };
}

[[nodiscard]] inline QString analysis_plugin_category_label(AnalysisPluginCategory category) {
    switch (category) {
        case AnalysisPluginCategory::BodyMotion:
            return "Body & motion";
        case AnalysisPluginCategory::FaceGaze:
            return "Face & gaze";
        case AnalysisPluginCategory::Audio:
            return "Audio";
        case AnalysisPluginCategory::Privacy:
            return "Privacy";
        case AnalysisPluginCategory::DataPrep:
            return "Data prep & utilities";
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
