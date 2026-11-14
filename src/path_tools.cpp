#include "path_tools.hpp"

#include <algorithm>
#include <cmath>

namespace hg {

Pt MapGeometry::to_pixel(const Pt& xy) const {
    return {(xy.first - left_m) * px_per_m, height_px - (xy.second - bottom_m) * px_per_m};
}

Pt MapGeometry::to_map(const Pt& uv) const {
    return {uv.first / px_per_m + left_m, (height_px - uv.second) / px_per_m + bottom_m};
}

static double seg_dist(const Pt& p, const Pt& a, const Pt& b) {
    double dx = b.first - a.first, dy = b.second - a.second;
    double len2 = dx * dx + dy * dy;
    if (len2 <= 1e-12) return std::hypot(p.first - a.first, p.second - a.second);
    double t = ((p.first - a.first) * dx + (p.second - a.second) * dy) / len2;
    t = std::max(0.0, std::min(1.0, t));
    return std::hypot(p.first - (a.first + t * dx), p.second - (a.second + t * dy));
}

static void rdp(const std::vector<Pt>& pts, size_t a, size_t b, double eps, std::vector<bool>& keep) {
    if (b <= a + 1) return;
    double best = -1;
    size_t idx = a;
    for (size_t i = a + 1; i < b; ++i) {
        double d = seg_dist(pts[i], pts[a], pts[b]);
        if (d > best) {
            best = d;
            idx = i;
        }
    }
    if (best > eps) {
        keep[idx] = true;
        rdp(pts, a, idx, eps, keep);
        rdp(pts, idx, b, eps, keep);
    }
}

std::vector<Pt> rdp_simplify(const std::vector<Pt>& pts, double epsilon) {
    if (pts.size() <= 2) return pts;
    std::vector<bool> keep(pts.size(), false);
    keep.front() = keep.back() = true;
    rdp(pts, 0, pts.size() - 1, epsilon, keep);
    std::vector<Pt> out;
    for (size_t i = 0; i < pts.size(); ++i)
        if (keep[i]) out.push_back(pts[i]);
    return out;
}

std::vector<Pt> drop_close(const std::vector<Pt>& pts, double min_gap) {
    if (pts.size() <= 2) return pts;
    std::vector<Pt> out{pts.front()};
    for (size_t i = 1; i + 1 < pts.size(); ++i)
        if (std::hypot(pts[i].first - out.back().first, pts[i].second - out.back().second) >= min_gap) out.push_back(pts[i]);
    // 끝점은 항상 유지. 바로 앞 점과 너무 가까우면 앞 점을 끝점으로 바꾼다
    if (out.size() > 1 && std::hypot(pts.back().first - out.back().first, pts.back().second - out.back().second) < min_gap)
        out.back() = pts.back();
    else
        out.push_back(pts.back());
    return out;
}

std::vector<Pt> sketch_to_path(const std::vector<Pt>& stroke_m, double epsilon, double min_gap, size_t max_points) {
    if (stroke_m.empty()) return {};
    std::vector<Pt> base = drop_close(stroke_m, min_gap);
    if (base.size() == 1) return base;
    double eps = epsilon;
    std::vector<Pt> out = rdp_simplify(base, eps);
    for (int i = 0; i < 40 && out.size() > max_points; ++i) {
        eps *= 1.4;
        out = rdp_simplify(base, eps);
    }
    if (out.size() > max_points) {  // 그래도 많으면(지그재그) 균등 추출
        std::vector<Pt> pick;
        for (size_t i = 0; i < max_points; ++i) pick.push_back(out[i * (out.size() - 1) / (max_points - 1)]);
        out = pick;
    }
    return out;
}

double polyline_length(const std::vector<Pt>& pts) {
    double s = 0;
    for (size_t i = 1; i < pts.size(); ++i) s += std::hypot(pts[i].first - pts[i - 1].first, pts[i].second - pts[i - 1].second);
    return s;
}

}  // namespace hg
