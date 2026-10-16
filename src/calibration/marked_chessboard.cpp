#include "calibration/marked_chessboard.hpp"

#if defined(MOSAIC_HAVE_OPENCV)

#include <algorithm>
#include <array>
#include <cmath>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

namespace mosaic {

namespace {

// The mark must differ from an unmarked square of the same colour by at
// least this much (grey levels, mean plus spread), and by this factor more
// than any other corner square does.
constexpr double kMinMarkContrast = 18.0;
constexpr double kMarkMargin      = 2.0;

struct Patch {
    double mean   = 0.0;
    double spread = 0.0;
};

// Grey levels inside one square of the board: a lattice over its middle
// 60%, mapped into the image by the grid-to-image homography. False when any
// sample falls outside the image.
bool sample_square(const cv::Mat& grey, const cv::Mat& H, double gx, double gy, Patch& out) {
    constexpr int kN = 9;
    std::vector<cv::Point2f> grid;
    grid.reserve(kN * kN);
    for (int i = 0; i < kN; ++i) {
        for (int j = 0; j < kN; ++j) {
            grid.emplace_back(static_cast<float>(gx + 0.2 + 0.6 * j / (kN - 1)),
                              static_cast<float>(gy + 0.2 + 0.6 * i / (kN - 1)));
        }
    }
    std::vector<cv::Point2f> img;
    cv::perspectiveTransform(grid, img, H);
    double sum = 0.0, sum2 = 0.0;
    for (const auto& p : img) {
        if (p.x < 1.0f || p.y < 1.0f || p.x > grey.cols - 2.0f || p.y > grey.rows - 2.0f) {
            return false;
        }
        // Bilinear: the lattice falls between pixels.
        const int x0 = static_cast<int>(p.x), y0 = static_cast<int>(p.y);
        const double fx = p.x - x0, fy = p.y - y0;
        const double v = (1 - fx) * (1 - fy) * grey.at<uchar>(y0, x0) +
                         fx * (1 - fy) * grey.at<uchar>(y0, x0 + 1) +
                         (1 - fx) * fy * grey.at<uchar>(y0 + 1, x0) +
                         fx * fy * grey.at<uchar>(y0 + 1, x0 + 1);
        sum += v;
        sum2 += v * v;
    }
    const double n = static_cast<double>(img.size());
    out.mean       = sum / n;
    out.spread     = std::sqrt(std::max(0.0, sum2 / n - out.mean * out.mean));
    return true;
}

} // namespace

std::vector<cv::Point2f> find_chessboard_corners(const cv::Mat& grey, cv::Size innerCorners) {
    // A quick look first, on a smaller copy: most frames show no board, and
    // the accurate search below takes most of a second on a full-resolution
    // frame (this runs on the GUI thread, once a second during capture).
    const double scale = std::min(1.0, 960.0 / std::max(grey.cols, grey.rows));
    cv::Mat small;
    if (scale < 1.0) {
        cv::resize(grey, small, cv::Size(), scale, scale, cv::INTER_AREA);
    } else {
        small = grey;
    }
    std::vector<cv::Point2f> rough;
    const bool seen = cv::findChessboardCorners(
        small, innerCorners, rough,
        cv::CALIB_CB_ADAPTIVE_THRESH | cv::CALIB_CB_NORMALIZE_IMAGE | cv::CALIB_CB_FAST_CHECK);
    if (!seen) {
        return {};
    }

    std::vector<cv::Point2f> corners;
    // The sector-based detector is more accurate and copes better with blur,
    // uneven light and boards seen at a slant. Only near where the board was
    // seen: its bounding box, with a margin of two squares' worth.
    cv::Rect box = cv::boundingRect(rough);
    const int margin =
        static_cast<int>(2.0 * std::max(box.width / std::max(1, innerCorners.width - 1),
                                        box.height / std::max(1, innerCorners.height - 1)));
    box = cv::Rect(box.x - margin, box.y - margin, box.width + 2 * margin, box.height + 2 * margin);
    cv::Rect roi(static_cast<int>(box.x / scale), static_cast<int>(box.y / scale),
                 static_cast<int>(box.width / scale), static_cast<int>(box.height / scale));
    roi &= cv::Rect(0, 0, grey.cols, grey.rows);
    if (cv::findChessboardCornersSB(grey(roi), innerCorners, corners,
                                    cv::CALIB_CB_NORMALIZE_IMAGE | cv::CALIB_CB_ACCURACY)) {
        for (auto& c : corners) {
            c.x += static_cast<float>(roi.x);
            c.y += static_cast<float>(roi.y);
        }
        return corners;
    }
    // Else the classic corners, refined on the full image.
    corners.clear();
    for (const auto& c : rough) {
        corners.emplace_back(c.x / static_cast<float>(scale), c.y / static_cast<float>(scale));
    }
    const int win = std::max(5, static_cast<int>(std::round(5.0 / scale)));
    cv::cornerSubPix(grey, corners, cv::Size(win, win), cv::Size(-1, -1),
                     cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 40, 0.001));
    return corners;
}

MarkedBoardStatus detect_marked_chessboard(const cv::Mat& grey, int squaresX, int squaresY,
                                           double squareMm, MarkedBoard& out) {
    out            = MarkedBoard{};
    const int cols = squaresX - 1;
    const int rows = squaresY - 1;
    if (cols < 2 || rows < 2) {
        return MarkedBoardStatus::NoBoard;
    }
    std::vector<cv::Point2f> found = find_chessboard_corners(grey, cv::Size(cols, rows));
    if (found.size() != static_cast<size_t>(cols * rows)) {
        return MarkedBoardStatus::NoBoard;
    }
    const int n = cols * rows;
    auto at     = [&](int r, int c) { return found[static_cast<size_t>(r * cols + c)]; };

    // Handedness: seen from the front, the first row runs to the right of the
    // first column (as x does from y in an image). The detector can return
    // the mirror image of that order; then reverse every row.
    const cv::Point2f e1 = at(0, cols - 1) - at(0, 0);
    const cv::Point2f e2 = at(rows - 1, 0) - at(0, 0);
    if (e1.x * e2.y - e1.y * e2.x < 0.0f) {
        std::vector<cv::Point2f> flipped(found.size());
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                flipped[static_cast<size_t>(r * cols + c)] = at(r, cols - 1 - c);
            }
        }
        found.swap(flipped);
    }

    // Grid units (one square = 1, inner corner (r, c) at (c, r)) to image.
    std::vector<cv::Point2f> gridPts;
    gridPts.reserve(static_cast<size_t>(n));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            gridPts.emplace_back(static_cast<float>(c), static_cast<float>(r));
        }
    }
    const cv::Mat H = cv::findHomography(gridPts, found, 0);
    if (H.empty()) {
        return MarkedBoardStatus::NoBoard;
    }

    // The four corner squares (top-left of each, in grid units), the square
    // diagonally inside each (same colour), and the inner corner each touches.
    struct Corner {
        double gx, gy, refX, refY;
        int cornerIndex;
    };
    const std::array<Corner, 4> cornersOfBoard = {{
        {-1.0, -1.0, 0.0, 0.0, 0},                               // first row, start
        {cols - 1.0, -1.0, cols - 2.0, 0.0, cols - 1},           // first row, end
        {cols - 1.0, rows - 1.0, cols - 2.0, rows - 2.0, n - 1}, // last row, end
        {-1.0, rows - 1.0, 0.0, rows - 2.0, (rows - 1) * cols},  // last row, start
    }};
    std::array<double, 4> contrast{};
    for (size_t k = 0; k < cornersOfBoard.size(); ++k) {
        const Corner& cb = cornersOfBoard[k];
        Patch square, reference;
        if (!sample_square(grey, H, cb.gx, cb.gy, square) ||
            !sample_square(grey, H, cb.refX, cb.refY, reference)) {
            return MarkedBoardStatus::OffImage;
        }
        contrast[k] =
            std::abs(square.mean - reference.mean) + std::abs(square.spread - reference.spread);
    }
    std::array<size_t, 4> order = {0, 1, 2, 3};
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return contrast[a] > contrast[b]; });
    out.markContrast     = contrast[order[0]];
    out.runnerUpContrast = contrast[order[1]];
    if (out.markContrast < kMinMarkContrast ||
        out.markContrast < kMarkMargin * out.runnerUpContrast) {
        return MarkedBoardStatus::MarkUnclear;
    }
    const int marked = cornersOfBoard[order[0]].cornerIndex;

    // Renumber so the marked corner comes first. Turns only (a mirror would
    // undo the handedness fixed above): a half turn on any board, quarter
    // turns on a square one. perm[i] = the old index of new corner i.
    std::vector<std::vector<int>> turns;
    std::vector<int> id(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        id[static_cast<size_t>(i)] = i;
    }
    turns.push_back(id);
    std::vector<int> half(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        half[static_cast<size_t>(i)] = n - 1 - i;
    }
    turns.push_back(half);
    if (cols == rows) {
        // A quarter turn: new (r, c) is old (cols - 1 - c, r).
        auto quarter = [&](const std::vector<int>& p) {
            std::vector<int> q(static_cast<size_t>(n));
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    q[static_cast<size_t>(r * cols + c)] =
                        p[static_cast<size_t>((cols - 1 - c) * cols + r)];
                }
            }
            return q;
        };
        turns.push_back(quarter(id));
        turns.push_back(quarter(half));
    }
    const std::vector<int>* chosen = nullptr;
    for (const auto& p : turns) {
        if (p[0] == marked) {
            chosen = &p;
            break;
        }
    }
    if (chosen == nullptr) {
        // Sides of different length, mark at the end of a row: put it at the
        // end of the first row.
        for (const auto& p : turns) {
            if (p[static_cast<size_t>(cols - 1)] == marked) {
                chosen = &p;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        return MarkedBoardStatus::MarkUnclear;
    }

    out.corners.resize(static_cast<size_t>(n));
    out.object.resize(static_cast<size_t>(n));
    const auto s = static_cast<float>(squareMm);
    for (int i = 0; i < n; ++i) {
        out.corners[static_cast<size_t>(i)] = found[static_cast<size_t>((*chosen)[i])];
        const int r                         = i / cols;
        const int c                         = i % cols;
        // From the board's outer corner: the first inner corner is one
        // square in, as for a ChArUco board.
        out.object[static_cast<size_t>(i)] = cv::Point3f((c + 1) * s, (r + 1) * s, 0.0f);
    }
    return MarkedBoardStatus::Found;
}

} // namespace mosaic

#endif
