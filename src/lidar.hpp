// LiDAR 지도 데이터: 점유 격자(/map), 로봇 자세(/lidar_map/pose), 스캔 → 지도 좌표 변환.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hg {

// nav_msgs/OccupancyGrid. cells[y * w + x], -1 모름, 0..100 점유 확률. (x, y) 칸의 왼쪽 아래 = origin + (x, y) * res
struct OccGrid {
    int w = 0, h = 0;
    double res = 0.05, ox = 0, oy = 0;
    std::vector<int8_t> cells;
    // 한 번이라도 관측된 칸의 범위(지도를 그릴 때 이 부분만 확대해 보여 준다). 없으면 x1 < x0
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    bool valid() const { return w > 0 && h > 0 && res > 0 && cells.size() == static_cast<size_t>(w) * h; }
};

// rosbridge 가 보낸 /map 메시지(JSON 전체 문자열)에서 격자를 읽는다. 큰 data 배열은 일반 JSON 해석기를 거치지 않고 직접 읽는다.
bool parse_occupancy_grid(const std::string& message_json, OccGrid& out);

struct Pose2D {
    double x = 0, y = 0, yaw = 0;
};
// geometry_msgs/PoseStamped 의 JSON(msg 부분)에서 평면 자세
class Json;
bool parse_pose_stamped(const Json& msg, Pose2D& out);
double yaw_from_quaternion(double qx, double qy, double qz, double qw);

// 스캔 한 개를 지도 좌표 점들로(레이저 프레임 = 로봇 몸, 회전 없음). range_max 이상·0.05 m 미만은 버린다
std::vector<std::pair<float, float>> scan_to_map(const std::vector<float>& ranges, double angle_min, double angle_inc,
                                                 double range_max, const Pose2D& pose);

}  // namespace hg
