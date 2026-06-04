#pragma once
#include "core/settings.hpp"
#include <QLabel>
#include <QWidget>
#include <memory>

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

signals:
    void params_changed();
    void remove_requested(int index);

private:
    void build_header();
    void build_body();
    void build_image_tab(QWidget* tab);
    void build_exposure_tab(QWidget* tab);
    void build_gain_tab(QWidget* tab);
    void build_advanced_tab(QWidget* tab);
    void toggle_expanded();

    CameraParameters& m_params;
    int  m_index;
    bool m_expanded{true};

    QWidget* m_body{nullptr};
    QWidget* m_statusDot{nullptr};
    QWidget* m_expandBtn{nullptr};
    QLabel*  m_nameLabel{nullptr};  // kept so set_index() can update it
};

} // namespace mosaic
