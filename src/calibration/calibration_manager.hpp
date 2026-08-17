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
///         [](int viewIdx, bool found, QImage preview) {
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
    };

    explicit CalibrationManager(QObject* parent = nullptr);
    ~CalibrationManager() override;

    /// @returns @c true when OpenCV is compiled in (@c MOSAIC_HAVE_OPENCV is defined).
    [[nodiscard]] static bool is_available();

    /// @brief Sets the checkerboard geometry used for detection and calibration.
    ///
    /// Call before the first feed_frame().  Changing the spec after capturing
    /// views requires clear_views() to discard incompatible data.
    ///
    /// @param spec  Board geometry (cols, rows, square size in mm).
    void set_board(const BoardSpec& spec);

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
    /// @returns      @c true if a complete checkerboard was detected and the
    ///               view was accepted into the calibration set.
    bool feed_frame(const VideoFrame& frame);

    /// @returns The number of successfully detected views accumulated so far.
    ///          At least 10 views are recommended before calling calibrate().
    [[nodiscard]] int view_count() const;

    /// @brief Clears all accumulated views and the last calibration result.
    void clear_views();

    /// @brief Runs @c cv::calibrateCamera() on a background thread.
    ///
    /// Emits calibration_started(), calibration_progress(), and
    /// calibration_done() in sequence.  The caller must wait for
    /// calibration_done() before reading result().
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
    /// @param viewIndex  Running index of this detection attempt.
    /// @param found      @c true if all corners were detected and the view was accepted.
    /// @param preview    BGR frame with detected corners drawn on it (RGB for Qt).
    void corners_detected(int viewIndex, bool found, QImage preview);

    /// Emitted when calibrate() starts its background thread.
    void calibration_started();

    /// @param percentage  Rough completion percentage (0–100).
    void calibration_progress(int percentage);

    /// Emitted when calibration finishes.
    /// @param rmsError  Mean reprojection error in pixels.  Negative on failure.
    /// @param success   @c true if calibration converged successfully.
    void calibration_done(double rmsError, bool success);

   private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mosaic
