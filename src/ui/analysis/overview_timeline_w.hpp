#pragma once
#include <QWidget>
#include <functional>

#include "analysis/session_overview.hpp"

namespace mosaic {

/// The Session Overview: one camera's analyses as lanes on one time axis
/// (who speaks, eye contact, smile, head movement, heart rate), with the
/// playhead across all of them. A click seeks the video; hovering shows the
/// time and each lane's value there. A lane whose analysis was not run says
/// so instead of staying blank.
class OverviewTimelineW : public QWidget {
   public:
    using SeekCb = std::function<void(qint64 ms)>;

    explicit OverviewTimelineW(QWidget* parent = nullptr);

    void set_overview(const SessionOverview& overview);
    void set_playhead_ms(qint64 ms);
    void set_seek_callback(SeekCb cb) { seek_ = std::move(cb); }

   protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;

   private:
    [[nodiscard]] QRectF plot_rect() const;
    [[nodiscard]] double x_of(double ms) const;
    [[nodiscard]] double ms_of(double x) const;

    SessionOverview overview_;
    qint64 playheadMs_ = 0;
    double hoverX_     = -1.0;
    SeekCb seek_;
};

} // namespace mosaic
