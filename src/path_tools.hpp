// 지도 좌표 변환과 손가락 스케치 → 로봇 경유점(최대 20개) 단순화.
#pragma once
#include <utility>
#include <vector>

namespace hg {

using Pt = std::pair<double, double>;  // 지도 좌표 m (x 앞, y 왼쪽) 또는 이미지 픽셀(u, v)

// /api/map 이 주는 지도 이미지 기하(로봇 문서 18 §4): u = (x - left) * ppm, v = h - (y - bottom) * ppm
struct MapGeometry {
    int width_px = 0, height_px = 0;
    double left_m = 0, bottom_m = 0, px_per_m = 0;
    bool valid() const { return width_px > 0 && height_px > 0 && px_per_m > 0; }
    Pt to_pixel(const Pt& xy) const;
    Pt to_map(const Pt& uv) const;
};

// Ramer–Douglas–Peucker 단순화(끝점 유지)
std::vector<Pt> rdp_simplify(const std::vector<Pt>& pts, double epsilon);
// 가까운 점 제거(첫 점·끝점 유지)
std::vector<Pt> drop_close(const std::vector<Pt>& pts, double min_gap);
// 스케치를 로봇이 받는 경로로: 간격 정리 → RDP(ε) → max_points 넘으면 ε 를 키워 다시
std::vector<Pt> sketch_to_path(const std::vector<Pt>& stroke_m, double epsilon = 0.08, double min_gap = 0.15,
                               size_t max_points = 20);
double polyline_length(const std::vector<Pt>& pts);

}  // namespace hg
