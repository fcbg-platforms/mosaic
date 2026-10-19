#include "calibration/room_bundle_adjust.hpp"

#if defined(MOSAIC_HAVE_OPENCV)

#include <algorithm>
#include <cmath>
#include <limits>
#include <opencv2/calib3d.hpp>

namespace mosaic::room_ba {

namespace {

// A rigid pose as OpenCV keeps it: Rodrigues vector and translation.
struct Pose {
    cv::Vec3d r;
    cv::Vec3d t;
};

Pose from_mat4(const Mat4& m) {
    const cv::Matx33d R(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    Pose p;
    cv::Rodrigues(R, p.r);
    p.t = cv::Vec3d(m[3], m[7], m[11]);
    return p;
}

Mat4 to_mat4(const Pose& p) {
    cv::Matx33d R;
    cv::Rodrigues(p.r, R);
    return {R(0, 0), R(0, 1), R(0, 2), p.t[0], R(1, 0), R(1, 1), R(1, 2), p.t[1],
            R(2, 0), R(2, 1), R(2, 2), p.t[2], 0.0,     0.0,     0.0,     1.0};
}

// Huber: squared error near zero, linear beyond kHuberPx, so a misdetected
// view pulls with a bounded force.
double huber_cost(double e) {
    return e <= kHuberPx ? 0.5 * e * e : kHuberPx * (e - 0.5 * kHuberPx);
}
double huber_weight(double e) { return e <= kHuberPx ? 1.0 : kHuberPx / e; }

cv::Matx33d m33(const cv::Mat& m) { return cv::Matx33d(m.ptr<double>()); }

} // namespace

Result refine(int cameraCount, int referenceIndex, const std::vector<bool>& resolved,
              const std::vector<Mat4>& cameraToRoom, const std::map<int, Lens>& lenses,
              const std::vector<Observation>& observations) {
    const auto nCam = static_cast<size_t>(std::max(0, cameraCount));
    Result out;
    out.cameraToRoom = cameraToRoom;
    out.rmsBeforePx.assign(nCam, -1.0);
    out.rmsAfterPx.assign(nCam, -1.0);
    out.movedMm.assign(nCam, 0.0);
    out.movedDeg.assign(nCam, 0.0);
    out.droppedPerCamera.assign(nCam, 0);
    if (cameraToRoom.size() < nCam || resolved.size() < nCam) {
        return out;
    }

    auto usable_camera = [&](int c) {
        return c >= 0 && c < cameraCount && resolved[static_cast<size_t>(c)] && lenses.count(c) > 0;
    };
    // Shots two or more resolved cameras saw: only they tie cameras together.
    std::map<int, int> camsPerShot;
    for (const auto& o : observations) {
        if (usable_camera(o.camera) && !o.object.empty() && o.object.size() == o.image.size()) {
            ++camsPerShot[o.shot];
        }
    }
    std::vector<const Observation*> used;
    for (const auto& o : observations) {
        if (usable_camera(o.camera) && !o.object.empty() && o.object.size() == o.image.size() &&
            camsPerShot[o.shot] >= 2) {
            used.push_back(&o);
        }
    }
    if (used.empty()) {
        return out;
    }

    // Unknowns: 6 per free camera (room to camera), 6 per shot (board to room).
    std::vector<int> camParam(nCam, -1);
    int n = 0;
    for (const auto* o : used) {
        const auto c = static_cast<size_t>(o->camera);
        if (o->camera != referenceIndex && camParam[c] < 0) {
            camParam[c] = n;
            n += 6;
        }
    }
    if (n == 0) {
        return out; // only the reference camera: nothing to refine
    }
    std::map<int, int> shotParam;
    for (const auto* o : used) {
        if (shotParam.count(o->shot) == 0) {
            shotParam[o->shot] = n;
            n += 6;
        }
    }

    std::vector<Pose> fixedW(nCam);
    for (size_t c = 0; c < nCam; ++c) {
        if (resolved[c]) {
            fixedW[c] = from_mat4(room_frame::invert(cameraToRoom[c]));
        }
    }
    std::vector<double> p(static_cast<size_t>(n), 0.0);
    auto put = [&](int at, const Pose& q) {
        for (int k = 0; k < 3; ++k) {
            p[static_cast<size_t>(at + k)]     = q.r[k];
            p[static_cast<size_t>(at + 3 + k)] = q.t[k];
        }
    };
    auto get = [](const std::vector<double>& v, int at) {
        Pose q;
        for (int k = 0; k < 3; ++k) {
            q.r[k] = v[static_cast<size_t>(at + k)];
            q.t[k] = v[static_cast<size_t>(at + 3 + k)];
        }
        return q;
    };
    for (size_t c = 0; c < nCam; ++c) {
        if (camParam[c] >= 0) {
            put(camParam[c], fixedW[c]);
        }
    }

    // Squared reprojection errors of one view, given the board and camera poses.
    auto view_errors = [&](const Observation& o, const Pose& b, const Pose& w) {
        cv::Vec3d r3, t3;
        cv::composeRT(b.r, b.t, w.r, w.t, r3, t3);
        const Lens& lens = lenses.at(o.camera);
        std::vector<cv::Point2f> proj;
        cv::projectPoints(o.object, r3, t3, lens.K, lens.dist, proj);
        std::vector<double> e(proj.size());
        for (size_t k = 0; k < proj.size(); ++k) {
            const cv::Point2f d = proj[k] - o.image[k];
            e[k] = std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y);
        }
        return e;
    };

    // Each shot's board starts where the camera that explains the whole
    // shot best puts it (one camera may have solved the board as its mirror
    // image; an average would mix the two).
    for (const auto& [shot, at] : shotParam) {
        double best = std::numeric_limits<double>::max();
        Pose bestPose;
        for (const auto* cand : used) {
            if (cand->shot != shot) {
                continue;
            }
            const Pose b = from_mat4(room_frame::compose(
                cameraToRoom[static_cast<size_t>(cand->camera)], cand->boardToCam));
            double cost  = 0.0;
            for (const auto* o : used) {
                if (o->shot == shot) {
                    for (double e : view_errors(*o, b, fixedW[static_cast<size_t>(o->camera)])) {
                        cost += huber_cost(e);
                    }
                }
            }
            if (cost < best) {
                best     = cost;
                bestPose = b;
            }
        }
        put(at, bestPose);
    }

    auto cam_pose = [&](const std::vector<double>& v, int c) {
        const int at = camParam[static_cast<size_t>(c)];
        return at >= 0 ? get(v, at) : fixedW[static_cast<size_t>(c)];
    };

    // The robust cost at v; with A and g, also the Gauss-Newton normal
    // equations (A = J'WJ, g = J'We), Huber weights from the current errors.
    auto evaluate = [&](const std::vector<double>& v, cv::Mat* A, cv::Mat* g) {
        if (A != nullptr) {
            *A = cv::Mat::zeros(n, n, CV_64F);
            *g = cv::Mat::zeros(n, 1, CV_64F);
        }
        double cost = 0.0;
        for (const auto* o : used) {
            const Pose b    = get(v, shotParam.at(o->shot));
            const Pose w    = cam_pose(v, o->camera);
            const Lens& len = lenses.at(o->camera);
            cv::Mat r3, t3, dr3dr1, dr3dt1, dr3dr2, dr3dt2, dt3dr1, dt3dt1, dt3dr2, dt3dt2;
            cv::composeRT(b.r, b.t, w.r, w.t, r3, t3, dr3dr1, dr3dt1, dr3dr2, dr3dt2, dt3dr1,
                          dt3dt1, dt3dr2, dt3dt2);
            std::vector<cv::Point2f> proj;
            cv::Mat jac;
            cv::projectPoints(o->object, r3, t3, len.K, len.dist, proj, jac);

            const int bAt = shotParam.at(o->shot);
            const int wAt = camParam[static_cast<size_t>(o->camera)];
            for (size_t k = 0; k < proj.size(); ++k) {
                const double ex = static_cast<double>(proj[k].x) - o->image[k].x;
                const double ey = static_cast<double>(proj[k].y) - o->image[k].y;
                const double e  = std::sqrt(ex * ex + ey * ey);
                cost += huber_cost(e);
                if (A == nullptr) {
                    continue;
                }
                const double wt = huber_weight(e);
                // d(pixel)/d(r3, t3), then the chain rule to the board's and
                // the camera's own parameters.
                const auto* j0 = jac.ptr<double>(static_cast<int>(2 * k));
                const auto* j1 = jac.ptr<double>(static_cast<int>(2 * k + 1));
                const cv::Matx23d Jr(j0[0], j0[1], j0[2], j1[0], j1[1], j1[2]);
                const cv::Matx23d Jt(j0[3], j0[4], j0[5], j1[3], j1[4], j1[5]);
                cv::Matx<double, 2, 12> J;
                const cv::Matx23d dbr = Jr * m33(dr3dr1) + Jt * m33(dt3dr1);
                const cv::Matx23d dbt = Jr * m33(dr3dt1) + Jt * m33(dt3dt1);
                const cv::Matx23d dwr = Jr * m33(dr3dr2) + Jt * m33(dt3dr2);
                const cv::Matx23d dwt = Jr * m33(dr3dt2) + Jt * m33(dt3dt2);
                for (int row = 0; row < 2; ++row) {
                    for (int col = 0; col < 3; ++col) {
                        J(row, col)     = dbr(row, col);
                        J(row, 3 + col) = dbt(row, col);
                        J(row, 6 + col) = dwr(row, col);
                        J(row, 9 + col) = dwt(row, col);
                    }
                }
                const int cols[12]  = {bAt, bAt + 1, bAt + 2, bAt + 3, bAt + 4, bAt + 5,
                                       wAt, wAt + 1, wAt + 2, wAt + 3, wAt + 4, wAt + 5};
                const int nCols     = wAt >= 0 ? 12 : 6; // the reference camera is fixed
                const double res[2] = {ex, ey};
                for (int a = 0; a < nCols; ++a) {
                    double ga = 0.0;
                    for (int row = 0; row < 2; ++row) {
                        ga += J(row, a) * res[row];
                    }
                    g->at<double>(cols[a]) += wt * ga;
                    for (int bcol = 0; bcol < nCols; ++bcol) {
                        double s = 0.0;
                        for (int row = 0; row < 2; ++row) {
                            s += J(row, a) * J(row, bcol);
                        }
                        A->at<double>(cols[a], cols[bcol]) += wt * s;
                    }
                }
            }
        }
        return cost;
    };

    // Per camera RMS (and in total) against the room model at v.
    auto rms_at = [&](const std::vector<double>& v, std::vector<double>& perCam, double& total,
                      int* outliers) {
        std::vector<double> sq(nCam, 0.0);
        std::vector<int> cnt(nCam, 0);
        double allSq = 0.0;
        int all      = 0;
        for (const auto* o : used) {
            const auto c = static_cast<size_t>(o->camera);
            for (double e :
                 view_errors(*o, get(v, shotParam.at(o->shot)), cam_pose(v, o->camera))) {
                sq[c] += e * e;
                ++cnt[c];
                allSq += e * e;
                ++all;
                if (outliers != nullptr && e > kOutlierPx) {
                    ++*outliers;
                }
            }
        }
        for (size_t c = 0; c < nCam; ++c) {
            perCam[c] = cnt[c] > 0 ? std::sqrt(sq[c] / cnt[c]) : -1.0;
        }
        total = all > 0 ? std::sqrt(allSq / all) : -1.0;
        return all;
    };
    out.shotsUsed                = static_cast<int>(shotParam.size());
    const std::vector<double> p0 = p;

    // Levenberg-Marquardt from the current p, on the views in `used`.
    auto run_lm = [&]() {
        cv::Mat A, g;
        double cost         = evaluate(p, &A, &g);
        double lambda       = 1e-3;
        constexpr int kIter = 100;
        for (int it = 0; it < kIter; ++it) {
            ++out.iterations;
            cv::Mat H = A.clone();
            for (int i = 0; i < n; ++i) {
                H.at<double>(i, i) += lambda * H.at<double>(i, i) + 1e-12;
            }
            cv::Mat step;
            if (!cv::solve(H, -g, step, cv::DECOMP_CHOLESKY)) {
                lambda *= 10.0;
                if (lambda > 1e10) {
                    break;
                }
                continue;
            }
            std::vector<double> trial = p;
            for (int i = 0; i < n; ++i) {
                trial[static_cast<size_t>(i)] += step.at<double>(i);
            }
            const double trialCost = evaluate(trial, nullptr, nullptr);
            if (trialCost < cost) {
                const bool tiny = cost - trialCost < 1e-10 * cost;
                p               = std::move(trial);
                cost            = evaluate(p, &A, &g);
                lambda          = std::max(lambda / 10.0, 1e-9);
                if (tiny) {
                    break;
                }
            } else {
                lambda *= 10.0;
                if (lambda > 1e10) {
                    break; // no step lowers the cost: converged
                }
            }
        }
    };
    run_lm();

    // A view whose corners all sit far off (a misdetection, the board
    // solved as its mirror image, a camera nudged mid-way) pulls as one:
    // the per-corner weighting cannot stop 50 corners pulling together. So
    // the worst view, when it fits far worse than the rest, is left out and
    // the refinement run again, one view at a time (a bad view first drags
    // its shot's board, so the other cameras' views of that shot look bad
    // too until it is gone). At most a tenth of the views (none with fewer
    // than 10), and never one whose loss would cut a camera or a board loose.
    const size_t maxDrops = used.size() / 10;
    // Every shot keeps a view, every refined camera keeps two views and stays
    // tied to the reference camera through shared shots; else the problem
    // would have directions nothing constrains.
    auto still_tied = [&](const std::vector<const Observation*>& v) {
        std::map<int, std::vector<int>> camsOfShot;
        std::map<int, int> views;
        for (const auto* o : v) {
            camsOfShot[o->shot].push_back(o->camera);
            ++views[o->camera];
        }
        if (camsOfShot.size() != shotParam.size()) {
            return false;
        }
        std::vector<bool> reached(nCam, false);
        if (referenceIndex >= 0 && static_cast<size_t>(referenceIndex) < nCam) {
            reached[static_cast<size_t>(referenceIndex)] = true;
        }
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& [shot, cams] : camsOfShot) {
                (void)shot;
                if (cams.size() < 2) {
                    continue;
                }
                const bool any = std::any_of(cams.begin(), cams.end(), [&](int c) {
                    return reached[static_cast<size_t>(c)];
                });
                for (int c : cams) {
                    if (any && !reached[static_cast<size_t>(c)]) {
                        reached[static_cast<size_t>(c)] = true;
                        grew                            = true;
                    }
                }
            }
        }
        for (size_t c = 0; c < nCam; ++c) {
            if (camParam[c] >= 0 && (!reached[c] || views[static_cast<int>(c)] < 2)) {
                return false;
            }
        }
        return true;
    };
    while (static_cast<size_t>(out.droppedViews) < maxDrops) {
        std::vector<double> viewRms;
        for (const auto* o : used) {
            double sq    = 0.0;
            const auto e = view_errors(*o, get(p, shotParam.at(o->shot)), cam_pose(p, o->camera));
            for (double x : e) {
                sq += x * x;
            }
            viewRms.push_back(std::sqrt(sq / static_cast<double>(e.size())));
        }
        std::vector<double> sorted = viewRms;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(sorted.size() / 2),
                         sorted.end());
        const double limit = std::max(kBadViewPx, 4.0 * sorted[sorted.size() / 2]);
        // The worst view over the limit that can go.
        std::vector<size_t> order(used.size());
        for (size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) { return viewRms[a] > viewRms[b]; });
        size_t worst = used.size();
        for (size_t i : order) {
            if (viewRms[i] <= limit) {
                break;
            }
            std::vector<const Observation*> rest = used;
            rest.erase(rest.begin() + static_cast<long>(i));
            if (still_tied(rest)) {
                worst = i;
                break;
            }
        }
        if (worst == used.size()) {
            break;
        }
        ++out.droppedPerCamera[static_cast<size_t>(used[worst]->camera)];
        used.erase(used.begin() + static_cast<long>(worst));
        ++out.droppedViews;
        run_lm();
    }

    // Before and after, on the same views.
    out.cornersUsed    = rms_at(p0, out.rmsBeforePx, out.totalRmsBeforePx, nullptr);
    const double cost0 = evaluate(p0, nullptr, nullptr);
    const double cost  = evaluate(p, nullptr, nullptr);

    std::vector<double> rmsAfter(nCam, -1.0);
    double totalAfter = -1.0;
    int outliers      = 0;
    rms_at(p, rmsAfter, totalAfter, &outliers);

    std::vector<Mat4> refined = cameraToRoom;
    bool plausible            = true;
    for (size_t c = 0; c < nCam; ++c) {
        if (camParam[c] < 0) {
            continue;
        }
        refined[c] = room_frame::invert(to_mat4(get(p, camParam[c])));
        out.movedMm[c] =
            std::hypot(refined[c][3] - cameraToRoom[c][3], refined[c][7] - cameraToRoom[c][7],
                       refined[c][11] - cameraToRoom[c][11]);
        out.movedDeg[c] = room_frame::rotation_angle_deg(refined[c], cameraToRoom[c]);
        plausible &= out.movedMm[c] <= kMaxMoveMm && out.movedDeg[c] <= kMaxMoveDeg;
    }
    if (cost < cost0 && plausible) {
        out.applied         = true;
        out.cameraToRoom    = refined;
        out.rmsAfterPx      = rmsAfter;
        out.totalRmsAfterPx = totalAfter;
        out.outlierCorners  = outliers;
    } else {
        out.rmsAfterPx      = out.rmsBeforePx;
        out.totalRmsAfterPx = out.totalRmsBeforePx;
    }
    return out;
}

} // namespace mosaic::room_ba

#endif
