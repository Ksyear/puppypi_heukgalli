// LiDAR 지도 위 경로 계획: 그린 선이 장애물을 지나면 로봇 몸 크기만큼 여유를 두고 돌아가는 경로를 만든다.
// 로봇은 장애물 회피가 없고 경유점(지도 좌표, 최대 20개)을 차례로 따라가기만 하므로, 회피는 앱이 맡는다.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lidar.hpp"
#include "path_tools.hpp"

namespace hg {

struct PlanConfig {
    double res = 0.10;          // 계획 격자(m)
    double inflate = 0.20;      // 장애물을 이만큼 부풀린다(로봇 0.226×0.149 m + 여유, 제자리 회전 반지름)
    double prefer = 0.35;       // 벽에서 inflate+prefer 안쪽은 비용을 더 매겨 가운데로 지나가게
    double unknown_cost = 0.6;  // 모르는 칸(-1)도 지나갈 수 있지만 조금 비싸게
    int occupied = 65;          // 이 값 이상이면 점유(map_server 와 같은 기준)
    double margin = 3.0;        // 출발·경유점 범위 밖으로 이만큼 더 본다(m)
    size_t max_points = 20;     // 로봇이 받는 경유점 수
};

struct PlanResult {
    bool ok = false;
    bool detoured = false;      // 그린 선을 그대로 쓰지 못하고 돌아감
    std::vector<Pt> path;       // 로봇에 보낼 경유점(출발점 제외)
    std::vector<int> goal_at;   // 사용자 경유점 i 가 path 의 몇 번째인지
    std::string reason;         // 실패 이유(화면 표시용)
};

class CostGrid {
public:
    // 점유 격자(없으면 nullptr) + 실시간 장애물 점들로, bounds 범위의 계획 격자를 만든다
    void build(const OccGrid* grid, const std::vector<Pt>& obstacles, double minx, double miny, double maxx, double maxy,
               const PlanConfig& cfg);
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }
    bool blocked(int x, int y) const;
    float step_cost(int x, int y) const;
    bool to_cell(const Pt& p, int& x, int& y) const;
    Pt to_world(int x, int y) const;
    float clearance(const Pt& p) const;            // 가장 가까운 장애물까지 거리(m), 범위 밖이면 0
    bool segment_free(const Pt& a, const Pt& b, double ignore_near = 0, const Pt* near = nullptr) const;
    int width() const { return w_; }
    int height() const { return h_; }

private:
    int w_ = 0, h_ = 0;
    double ox_ = 0, oy_ = 0, res_ = 0.1;
    PlanConfig cfg_;
    std::vector<float> dist_;     // 장애물까지 거리(m)
    std::vector<uint8_t> unknown_;
};

// start 에서 waypoints 를 차례로 지나는 경로. 구간이 막혀 있으면 A* 로 우회하고, 결과를 max_points 이하로 줄인다
PlanResult plan_path(const OccGrid* grid, const std::vector<Pt>& obstacles, const Pt& start, const std::vector<Pt>& waypoints,
                     const PlanConfig& cfg = PlanConfig());

// 남은 경로(start → path[from..])의 앞쪽 lookahead 거리 안에 장애물 점이 radius 안으로 들어왔는지
bool path_blocked_by(const std::vector<Pt>& obstacles, const Pt& start, const std::vector<Pt>& path, size_t from, double radius,
                     double lookahead, std::vector<Pt>* hits = nullptr);

// 가는 중 막힘 판정: 최근 window 번 스캔 중 need 번 이상 막히면(그리고 직전 재계획 뒤 cooldown 이 지났으면) 재계획
class BlockTrigger {
public:
    explicit BlockTrigger(int window = 4, int need = 3, double cooldown = 1.5) : window_(window), need_(need), cooldown_(cooldown) {}
    bool push(bool hit, double now);  // true = 지금 재계획할 때
    void reset() { hits_.clear(); }
    void replanned(double now) { last_ = now; hits_.clear(); }
    int count() const;

private:
    int window_, need_;
    double cooldown_, last_ = -1e9;
    std::vector<bool> hits_;
};

// 로봇이 지금 index 번째 점을 향해 가는 중일 때, 아직 지나지 않은 사용자 경유점(goal_at[i] >= index)
std::vector<Pt> remaining_goals(const std::vector<Pt>& goals, const std::vector<int>& goal_at, int index);

}  // namespace hg
