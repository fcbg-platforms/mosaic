#pragma once

#if defined(MOSAIC_HAVE_OPENCV)
#include <opencv2/core.hpp>
#include <vector>

namespace mosaic {

// ── A plain chessboard with one corner square marked ────────────────────────
//
// A plain chessboard says nothing about which way round it is: a square board
// looks the same turned by 90 degrees, any board the same turned by 180, so
// two cameras can number its corners differently and disagree about where
// the board is. Room calibration needs every camera to agree.
//
// A board with one odd and one even side (9 x 8 squares, say) settles it by
// itself: one edge has black corner squares, the opposite edge white ones,
// and the corners are numbered from the black-cornered edge. Any other board
// (square, or both sides odd or both even) needs one corner square carrying a
// mark (a white disc on a black square is clearest): the corners are
// numbered from the marked corner, the same way in every camera.

/// Outcome of detect_marked_chessboard().
enum class MarkedBoardStatus {
    Found,          ///< corners found and put in the board's own order
    NoBoard,        ///< no chessboard of this size in the image
    OffImage,       ///< a corner square is outside the image: its mark or colour cannot be checked
    MarkUnclear,    ///< no corner square stands out clearly from the others
    ColoursUnclear, ///< odd x even board: black and white corners not told apart
};

/// A detected board, in the board's own order.
struct MarkedBoard {
    /// Inner corners, row by row from the marked corner: (squaresX - 1) per
    /// row, (squaresY - 1) rows.
    std::vector<cv::Point2f> corners;
    /// The matching board points, millimetres, z = 0. The origin is the
    /// board's outer corner at the marked square, x along the first row,
    /// y down the first column, z into the board (away from the camera).
    std::vector<cv::Point3f> object;
    /// How much the marked square differs from an unmarked square of its
    /// colour (grey levels), and the next clearest corner square's
    /// difference: the mark is accepted when it clearly stands out.
    double markContrast     = 0.0;
    double runnerUpContrast = 0.0;
    /// True on a board with one odd and one even side: oriented by its
    /// corner colours (no mark needed), the "marked" corner being the start
    /// of the black-cornered edge. colourDifference: how much darker that
    /// edge's corner squares are than the opposite edge's (grey levels).
    bool byColour           = false;
    double colourDifference = 0.0;
};

/// Finds a plain chessboard of @p squaresX x @p squaresY squares (so
/// (squaresX - 1) x (squaresY - 1) inner corners) in @p grey (8-bit, one
/// channel), and numbers its corners the same way in every camera (from the
/// black-cornered edge or the marked corner square, below).
///
/// One side odd, one even: the black-cornered edge comes first, the origin
/// at one of its two ends (always the same one for a given board), whatever
/// mark the board carries. Otherwise each corner square is compared with the
/// square diagonally inside it, which always has the same colour; the marked
/// one differs. On a board
/// whose sides differ in length only a half turn is possible, so the marked
/// corner ends up at the start or the end of the first row; on a square
/// board it is always at the start.
[[nodiscard]] MarkedBoardStatus detect_marked_chessboard(const cv::Mat& grey, int squaresX,
                                                         int squaresY, double squareMm,
                                                         MarkedBoard& out);

/// The inner corners of a plain chessboard (any order OpenCV gives), refined
/// to sub-pixel accuracy. Tries the more accurate sector-based detector
/// first, then the classic one. Empty when not found.
[[nodiscard]] std::vector<cv::Point2f> find_chessboard_corners(const cv::Mat& grey,
                                                               cv::Size innerCorners);

} // namespace mosaic

#endif
