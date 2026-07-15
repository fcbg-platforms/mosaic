#pragma once
#include "core/settings.hpp"
#include <QWidget>
#include <memory>

namespace mosaic {

// A collapsible settings card for one parallel port trigger source.
// Follows the same card pattern used by KeyboardCardW and MicrophoneCardW.

class ParallelPortCardW : public QWidget {
    Q_OBJECT
public:
    explicit ParallelPortCardW(ParallelPortConfig& config,
                                int                index,
                                QWidget*           parent = nullptr);
    ~ParallelPortCardW() override;

    void set_index(int index);

signals:
    void config_changed();
    void remove_requested(int index);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
