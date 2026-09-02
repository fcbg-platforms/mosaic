#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QWidget>
#include <memory>
#include <optional>

#include "core/settings.hpp"

namespace mosaic {

// A collapsible card representing one camera's full configuration.
// The header is always visible; the body (4-tab form) animates open/closed.
//
// The card writes every change directly into the CameraParameters reference
// it was given, then emits params_changed() so the parent can react.

class CameraCardW : public QWidget {
    Q_OBJECT
   public:
    explicit CameraCardW(CameraParameters& params, int index, QWidget* parent = nullptr);
    ~CameraCardW() override;

    // Called by the backend when a camera with matching serial connects / disconnects.
    void set_connected(bool connected);

    // Called by VideoSettingsW after a sibling card is deleted to keep
    // the displayed number correct (always shows 1-based position).
    void set_index(int index);

    // Reload all controls from the current state of m_params.
    void refresh();

    // Updates the live "Action-command supported" readout in the HW Trigger
    // tab. std::nullopt = not yet probed this session (camera not opened, or
    // opened without Action1 selected — see
    // VideoGrabber::action_command_capability()).
    void set_action_command_capability(std::optional<bool> supported);

    // Updates the achievable-frame-rate readout beside the exposure controls.
    // `fps` <= 0 means the camera has no trustworthy measurement right now
    // (closed, or still inside its warm-up window) — the readout then falls
    // back to the exposure-imposed ceiling if exposure is manual, or says the
    // rate is measured once the camera runs. See compute_fps_readout().
    void set_achievable_fps(double fps);

   signals:
    void params_changed();

   private:
    void build_header();
    void build_body();
    void build_image_tab(QWidget* tab);
    void build_exposure_tab(QWidget* tab);
    void build_gain_tab(QWidget* tab);
    void build_advanced_tab(QWidget* tab);
    void build_hw_trigger_tab(QWidget* tab);
    void toggle_expanded();

    CameraParameters& m_params;
    int m_index;
    // Defaults to collapsed: camera settings are hardware-sensitive
    // (exposure/trigger/pixel-format) and pre-tuned for the room — starting
    // every card open invited accidental edits just from scrolling past it.
    // A user who wants to change a camera's settings can still expand any
    // card explicitly via its header arrow.
    bool m_expanded{false};

    QWidget* m_body{nullptr};
    QWidget* m_statusDot{nullptr};
    QWidget* m_expandBtn{nullptr};
    QLabel* m_nameLabel{nullptr};           // kept so set_index() can update it
    QLineEdit* m_serialEdit{nullptr};       // kept so refresh() can update it
    QLabel* m_actionCapabilityLbl{nullptr}; // kept so set_action_command_capability() can update it
    QLabel* m_achievableFpsLbl{nullptr};    // kept so set_achievable_fps() can update it
    // Last measured rate from VideoGrabber::achievable_fps(), <= 0 when there
    // is none. Retained so the readout can be recomputed when an exposure or
    // frame-rate control changes without waiting for the camera's next
    // measurement (see refresh_achievable_fps_label()).
    double m_achievableFps{-1.0};

    // Re-renders m_achievableFpsLbl from m_achievableFps and the current
    // exposure/frame-rate settings.
    void refresh_achievable_fps_label();
};

} // namespace mosaic
