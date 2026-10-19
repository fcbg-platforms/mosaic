#pragma once
#include <QImage>
#include <QObject>
#include <memory>

#include "core/settings.hpp"
#include "video/video_frame.hpp"

namespace mosaic {

/// @brief Runs a full checkerboard calibration pipeline using OpenCV.
///
/// Checkerboard calibration determines the intrinsic camera parameters
/// (focal length, principal point, and lens distortion coefficients) needed
/// to undistort frames and to triangulate 3-D positions across cameras.
///
/// Requires @c MOSAIC_HAVE_OPENCV — is_available() returns @c false otherwise.
///
/// @par Workflow
/// @code{.cpp}
/// CalibrationManager cal;
/// cal.set_board({9, 6, 25.0});          // 9×6 inner corners, 25 mm squares
///
/// connect(&cal, &CalibrationManager::corners_detected, this,
///         [](int viewCount, bool accepted, QImage preview, QString note) {
///             // update a preview label in the UI
///         });
/// connect(&cal, &CalibrationManager::calibration_done, this,
///         [&](double rms, bool ok) {
///             if (ok) settings.video.cameras[0].calibration = cal.result();
///         });
///
/// // Feed frames as they arrive from VideoGrabber:
/// cal.feed_frame(frame);   // repeat until view_count() >= 10
///
/// cal.calibrate();         // async — emits calibration_done() when done
/// @endcode
///
/// @see CalibrationData, VideoFrame, CalibrationW
class CalibrationManager : public QObject {
    Q_OBJECT
   public:
    /// @brief Checkerboard geometry parameters.
    struct BoardSpec {
        int cols            = 9;    ///< Number of *inner* corner columns.
        int rows            = 6;    ///< Number of *inner* corner rows.
        double squareSizeMm = 25.0; ///< Physical side length of one square, in mm.

        bool operator==(const BoardSpec&) const = default;
    };

    explicit CalibrationManager(QObject* parent = nullptr);
    ~CalibrationManager() override;

    /// @returns @c true when OpenCV is compiled in (@c MOSAIC_HAVE_OPENCV is defined).
    [[nodiscard]] static bool is_available();

    /// @brief Sets the checkerboard geometry used for detection and calibration.
    ///
    /// Call before the first feed_frame(). Every accepted view stores its
    /// corners' 3-D positions in the board's own size, so views captured with
    /// one board cannot be mixed with another: a spec that differs from the
    /// current one discards the views captured so far (a finished calibration
    /// result is kept; the intrinsics do not depend on the board).
    ///
    /// @param spec  Board geometry (cols, rows, square size in mm).
    /// @returns     How many captured views were discarded (0 if the spec is
    ///              unchanged or there were none).
    int set_board(const BoardSpec& spec);

    /// @brief Runs corner detection on a single frame.
    ///
    /// This is the main ingestion method.  Call it each time a new frame
    /// arrives from VideoGrabber (or whenever the user clicks "Capture").
    /// The method is synchronous and may take a few milliseconds per frame.
    ///
    /// Emits corners_detected() with a preview image regardless of whether
    /// corners were found.
    ///
    /// @param frame  A valid VideoFrame in BGR8 format.
    /// A found board is still not used when the frame's size differs from the
    /// views so far, or when the board has not moved since an accepted view
    /// (a near copy adds nothing and over-weights that pose); the note in
    /// corners_detected() says which.
    ///
    /// @returns      @c true if a complete checkerboard was detected and the
    ///               view was accepted into the calibration set.
    bool feed_frame(const VideoFrame& frame);

    /// @returns The share of the image (0..1, an 8 x 6 grid) where the
    ///          accepted views put at least one corner. The lens' distortion
    ///          is only measured where corners were seen: aim for most of it,
    ///          edges and corners included.
    [[nodiscard]] double coverage() const;

    /// @returns How many views the last calibration used, and how many it
    ///          left out because they fit far worse than the rest.
    [[nodiscard]] int used_view_count() const;
    [[nodiscard]] int dropped_view_count() const;
    /// Each candidate lens model's error on views left out of its fit,
    /// the chosen one marked (calibration/lens_model.hpp); empty before a
    /// calibration.
    [[nodiscard]] QString lens_model_report() const;

    /// @returns The number of successfully detected views accumulated so far.
    ///          At least 10 views are recommended before calling calibrate().
    [[nodiscard]] int view_count() const;

    /// @brief Clears all accumulated views and the last calibration result.
    void clear_views();

    /// @brief Runs @c cv::calibrateCamera() on a background thread.
    ///
    /// Emits calibration_started() and then calibration_done().  The caller must wait for
    /// calibration_done() before reading result(). The thread works on a copy
    /// of the views (frames may keep arriving); views fitting far worse than
    /// the rest (over max(1 px, 3 x the median)) are left out and the
    /// calibration run once more, when there are at least 8 views and at
    /// least 6 remain. A call while one is running is ignored.
    ///
    /// Requires at least 5 accepted views (10+ recommended).
    void calibrate();

    /// @returns The most recent calibration result.  Only valid after
    ///          calibration_done() has been emitted with @c success = @c true.
    [[nodiscard]] CalibrationData result() const;

    /// @returns @c true if a valid calibration result is available.
    [[nodiscard]] bool has_result() const;

   signals:
    /// Emitted after each call to feed_frame().
    /// @param viewCount  Views accepted so far.
    /// @param accepted   @c true if this frame's view was accepted.
    /// @param preview    The frame with any detected corners drawn on it (RGB).
    /// @param note       What happened to this frame, in words.
    void corners_detected(int viewCount, bool accepted, QImage preview, QString note);

    /// Emitted when calibrate() starts its background thread.
    void calibration_started();

    /// Emitted when calibration finishes.
    /// @param rmsError  Mean reprojection error in pixels.  Negative on failure.
    /// @param success   @c true if calibration converged successfully.
    void calibration_done(double rmsError, bool success);

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
