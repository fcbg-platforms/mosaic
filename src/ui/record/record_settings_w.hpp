#pragma once
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

#include "core/settings.hpp"

namespace mosaic {

// Record settings panel: output directory, channel toggles, how a recording
// starts (countdown delay, preview hiding), file naming, and a live preview
// of the filenames that will be generated.

class RecordSettingsW : public QWidget {
    Q_OBJECT
   public:
    // isAdmin: per-user recording access control (item 27) — a non-admin's
    // directory field is locked read-only (still visible, this profile's
    // own recording folder, just not retargetable) since directory-per-
    // user is the actual access-control boundary; freely allowing it to be
    // changed would let a non-admin point their own recordings at another
    // user's folder, silently defeating that boundary. Admin profiles keep
    // full editing, unchanged from before this feature existed.
    explicit RecordSettingsW(RecordSettings& settings, bool isAdmin = false,
                             QWidget* parent = nullptr);
    ~RecordSettingsW() override;

   signals:
    void settings_changed();

   private:
    void build_directory_section(QVBoxLayout* parent);
    void build_channels_section(QVBoxLayout* parent);
    void build_start_section(QVBoxLayout* parent);
    void build_naming_section(QVBoxLayout* parent);
    void build_preview_section(QVBoxLayout* parent);
    void refresh_preview();

    RecordSettings& m_settings;
    bool m_isAdmin = false;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
