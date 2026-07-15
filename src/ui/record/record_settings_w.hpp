#pragma once
#include "core/settings.hpp"
#include <QVBoxLayout>
#include <QWidget>
#include <memory>

namespace mosaic {

// Record settings panel: output directory, channel toggles, file naming,
// and a live preview of the filenames that will be generated.

class RecordSettingsW : public QWidget {
    Q_OBJECT
public:
    explicit RecordSettingsW(RecordSettings& settings, QWidget* parent = nullptr);
    ~RecordSettingsW() override;

signals:
    void settings_changed();

private:
    void build_directory_section(QVBoxLayout* parent);
    void build_channels_section(QVBoxLayout* parent);
    void build_naming_section(QVBoxLayout* parent);
    void build_preview_section(QVBoxLayout* parent);
    void refresh_preview();

    RecordSettings& m_settings;

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
