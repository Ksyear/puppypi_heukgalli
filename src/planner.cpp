#include "planner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace hg {

namespace {
std::string fmt_reason(size_t n) { return std::to_string(n) + "번 경유점이 장애물 안(또는 너무 가까움)"; }

double dist(const Pt& a, const Pt& b) { return std::hypot(a.first - b.first, a.second - b.second); }

// 점 p 와 선분 ab 사이 거리
double seg_dist(const Pt& p, const Pt& a, const Pt& b) {
    double dx = b.first - a.first, dy = b.second - a.second, l2 = dx * dx + dy * dy;
    double t = l2 > 1e-12 ? ((p.first - a.first) * dx + (p.second - a.second) * dy) / l2 : 0;
    t = std::max(0.0, std::min(1.0, t));
    return std::hypot(p.first - (a.first + t * dx), p.second - (a.second + t * dy));
}
}  // namespace

void CostGrid::build(const OccGrid* grid, const std::vector<Pt>& obstacles, double minx, double miny, double maxx, double maxy,
                     const PlanConfig& cfg) {
    cfg_ = cfg;
    res_ = cfg.res;
    ox_ = std::floor(minx / res_) * res_;
    oy_ = std::floor(miny / res_) * res_;
    w_ = std::max(1, static_cast<int>(std::ceil((maxx - ox_) / res_)) + 1);
    h_ = std::max(1, static_cast<int>(std::ceil((maxy - oy_) / res_)) + 1);
    w_ = std::min(w_, 600);  // 60 m 이상은 보지 않는다
    h_ = std::min(h_, 600);
    size_t n = static_cast<size_t>(w_) * h_;
    std::vector<uint8_t> occ(n, 0);
    unknown_.assign(n, grid ? 1 : 0);
    if (grid && grid->valid()) {
        // 지도 칸을 계획 칸에 모은다: 하나라도 점유면 점유, 하나라도 관측됐으면 아는 칸
        for (int gy = 0; gy < grid->h; ++gy)
            for (int gx = 0; gx < grid->w; ++gx) {
                int8_t v = grid->cells[static_cast<size_t>(gy) * grid->w + gx];
                if (v < 0) continue;
                double wx = grid->ox + (gx + 0.5) * grid->res, wy = grid->oy + (gy + 0.5) * grid->res;
                int cx, cy;
                if (!to_cell({wx, wy}, cx, cy)) continue;
                size_t i = static_cast<size_t>(cy) * w_ + cx;
                unknown_[i] = 0;
                if (v >= cfg.occupied) occ[i] = 1;
            }
    }
    for (const auto& p : obstacles) {
        int cx, cy;
        if (to_cell(p, cx, cy)) occ[static_cast<size_t>(cy) * w_ + cx] = 1;
    }
    // 장애물까지 거리: 두 번 훑는 chamfer 변환(칸 단위 → m)
    const float inf = 1e9f, a = 1.0f, d = 1.41421356f;
    std::vector<float> t(n, inf);
    for (size_t i = 0; i < n; ++i)
        if (occ[i]) t[i] = 0;
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            float& v = t[static_cast<size_t>(y) * w_ + x];
            if (x > 0) v = std::min(v, t[static_cast<size_t>(y) * w_ + x - 1] + a);
            if (y > 0) {
                v = std::min(v, t[static_cast<size_t>(y - 1) * w_ + x] + a);
                if (x > 0) v = std::min(v, t[static_cast<size_t>(y - 1) * w_ + x - 1] + d);
                if (x + 1 < w_) v = std::min(v, t[static_cast<size_t>(y - 1) * w_ + x + 1] + d);
            }
        }
    for (int y = h_ - 1; y >= 0; --y)
        for (int x = w_ - 1; x >= 0; --x) {
            float& v = t[static_cast<size_t>(y) * w_ + x];
            if (x + 1 < w_) v = std::min(v, t[static_cast<size_t>(y) * w_ + x + 1] + a);
            if (y + 1 < h_) {
                v = std::min(v, t[static_cast<size_t>(y + 1) * w_ + x] + a);
                if (x + 1 < w_) v = std::min(v, t[static_cast<size_t>(y + 1) * w_ + x + 1] + d);
                if (x > 0) v = std::min(v, t[static_cast<size_t>(y + 1) * w_ + x - 1] + d);
            }
        }
    dist_.resize(n);
    for (size_t i = 0; i < n; ++i) dist_[i] = t[i] >= inf ? 1e6f : t[i] * static_cast<float>(res_);
}

bool CostGrid::to_cell(const Pt& p, int& x, int& y) const {
    x = static_cast<int>(std::floor((p.first - ox_) / res_));
    y = static_cast<int>(std::floor((p.second - oy_) / res_));
    return inside(x, y);
}

Pt CostGrid::to_world(int x, int y) const { return {ox_ + (x + 0.5) * res_, oy_ + (y + 0.5) * res_}; }

bool CostGrid::blocked(int x, int y) const {
    return !inside(x, y) || dist_[static_cast<size_t>(y) * w_ + x] < cfg_.inflate;
}

float CostGrid::step_cost(int x, int y) const {
    size_t i = static_cast<size_t>(y) * w_ + x;
    float c = 1.0f, dd = dist_[i], band = static_cast<float>(cfg_.inflate + cfg_.prefer);
    if (dd < band) c += 4.0f * (band - dd) / static_cast<float>(cfg_.prefer);  // 벽 가까이는 비싸게
    if (unknown_[i]) c += static_cast<float>(cfg_.unknown_cost);
    return c;
}

float CostGrid::clearance(const Pt& p) const {
    int x, y;
    if (!to_cell(p, x, y)) return 0;
    return dist_[static_cast<size_t>(y) * w_ + x];
}

bool CostGrid::segment_free(const Pt& a, const Pt& b, double ignore_near, const Pt* near) const {
    double len = dist(a, b);
    int steps = std::max(1, static_cast<int>(std::ceil(len / (res_ * 0.5))));
    for (int i = 0; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps;
        Pt p{a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t};
        if (near && dist(p, *near) < ignore_near) continue;  // 출발점 주변(이미 벽 옆에 서 있는 경우)은 봐준다
        int x, y;
        if (!to_cell(p, x, y) || blocked(x, y)) return false;
    }
    return true;
}

// 8방향 A*. start 가 막힌 칸이면(벽 옆에 서 있음) 가장 가까운 빈 칸까지는 허용
static bool astar(const CostGrid& g, int sx, int sy, int gx, int gy, std::vector<std::pair<int, int>>& out) {
    int w = g.width(), h = g.height();
    size_t n = static_cast<size_t>(w) * h;
    std::vector<float> cost(n, std::numeric_limits<float>::infinity());
    std::vector<int> parent(n, -1);
    std::vector<uint8_t> closed(n, 0);
    using Node = std::pair<float, int>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    auto idx = [w](int x, int y) { return y * w + x; };
    auto heur = [&](int x, int y) { return std::hypot(static_cast<float>(x - gx), static_cast<float>(y - gy)); };
    int s = idx(sx, sy), goal = idx(gx, gy);
    cost[s] = 0;
    open.push({heur(sx, sy), s});
    static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!open.empty()) {
        int cur = open.top().second;
        open.pop();
        if (closed[cur]) continue;
        closed[cur] = 1;
        if (cur == goal) break;
        int cx = cur % w, cy = cur / w;
        bool cur_blocked = g.blocked(cx, cy);
        for (int k = 0; k < 8; ++k) {
            int nx = cx + dx[k], ny = cy + dy[k];
            if (!g.inside(nx, ny)) continue;
            // 막힌 칸 안에서 시작했으면 막힌 칸끼리 이동은 허용(밖으로 빠져나오기), 빈 칸에서 막힌 칸으로는 못 들어감
            if (g.blocked(nx, ny) && !(cur_blocked && g.blocked(cx, cy))) continue;
            if (k >= 4 && (g.blocked(cx + dx[k], cy) || g.blocked(cx, cy + dy[k])) && !cur_blocked) continue;  // 모서리 끼기 금지
            int ni = idx(nx, ny);
            float c = cost[cur] + (k < 4 ? 1.0f : 1.41421356f) * g.step_cost(nx, ny);
            if (c < cost[ni]) {
                cost[ni] = c;
                parent[ni] = cur;
                open.push({c + heur(nx, ny), ni});
            }
        }
    }
    if (parent[goal] < 0 && goal != s) return false;
    out.clear();
    for (int c = goal; c >= 0; c = parent[c]) {
        out.emplace_back(c % w, c / w);
        if (c == s) break;
    }
    std::reverse(out.begin(), out.end());
    return true;
}

PlanResult plan_path(const OccGrid* grid, const std::vector<Pt>& obstacles, const Pt& start, const std::vector<Pt>& waypoints,
                     const PlanConfig& cfg) {
    PlanResult r;
    if (waypoints.empty()) {
        r.reason = "경유점 없음";
        return r;
    }
    double minx = start.first, maxx = start.first, miny = start.second, maxy = start.second;
    for (const auto& p : waypoints) {
        minx = std::min(minx, p.first);
        maxx = std::max(maxx, p.first);
        miny = std::min(miny, p.second);
        maxy = std::max(maxy, p.second);
    }
    CostGrid g;
    g.build(grid, obstacles, minx - cfg.margin, miny - cfg.margin, maxx + cfg.margin, maxy + cfg.margin, cfg);
    const double start_slack = cfg.inflate + 0.05;  // 벽 옆에서 출발하는 경우를 봐준다

    std::vector<Pt> full;  // 출발점 포함
    full.push_back(start);
    for (size_t wi = 0; wi < waypoints.size(); ++wi) {
        const Pt& a = full.back();
        const Pt& b = waypoints[wi];
        int bx, by;
        if (!g.to_cell(b, bx, by)) {
            r.reason = "경유점이 계획 범위 밖";
            return r;
        }
        if (g.blocked(bx, by)) {
            r.reason = fmt_reason(wi + 1);
            return r;
        }
        bool first_leg = wi == 0;
        if (g.segment_free(a, b, first_leg ? start_slack : 0, first_leg ? &start : nullptr)) {
            full.push_back(b);
            continue;
        }
        // 막혔다: A* 로 우회
        int ax, ay;
        g.to_cell(a, ax, ay);
        std::vector<std::pair<int, int>> cells;
        if (!astar(g, ax, ay, bx, by, cells)) {
            r.reason = "우회 경로 없음(" + std::to_string(wi + 1) + "번 경유점까지 지나갈 틈이 없음)";
            return r;
        }
        r.detoured = true;
        // 보이는 데까지 건너뛰며 꺾이는 점만 남긴다(line of sight)
        std::vector<Pt> pts;
        for (const auto& c : cells) pts.push_back(g.to_world(c.first, c.second));
        pts.front() = a;
        pts.back() = b;
        size_t i = 0;
        while (i + 1 < pts.size()) {
            size_t j = pts.size() - 1;
            while (j > i + 1 && !g.segment_free(pts[i], pts[j], first_leg && i == 0 ? start_slack : 0, first_leg && i == 0 ? &start : nullptr)) --j;
            full.push_back(pts[j]);
            i = j;
        }
    }
    // 너무 가까운 점 정리(사용자 경유점은 유지하지 않아도 됨: 로봇은 순서대로 지나가기만 한다)
    std::vector<Pt> path(full.begin() + 1, full.end());
    std::vector<Pt> clean;
    for (const auto& p : path)
        if (clean.empty() ? dist(p, start) > 0.05 : dist(p, clean.back()) > 0.05) clean.push_back(p);
    if (clean.empty()) clean.push_back(waypoints.back());
    // 20 개 넘으면: 장애물을 다시 지나지 않는 범위에서 가운데 점을 하나씩 뺀다
    while (clean.size() > cfg.max_points) {
        size_t best = 0;
        double best_cost = 1e18;
        for (size_t k = 0; k + 1 < clean.size(); ++k) {
            const Pt& prev = k == 0 ? start : clean[k - 1];
            if (!g.segment_free(prev, clean[k + 1])) continue;
            double c = seg_dist(clean[k], prev, clean[k + 1]);
            if (c < best_cost) {
                best_cost = c;
                best = k;
            }
        }
        if (best_cost >= 1e18) {
            r.reason = "경유점이 20개를 넘음(선을 더 단순하게 그려 주세요)";
            return r;
        }
        clean.erase(clean.begin() + static_cast<long>(best));
    }
    // 사용자 경유점 위치(가장 가까운 점)
    for (const auto& wp : waypoints) {
        int bi = 0;
        double bd = 1e18;
        for (size_t k = 0; k < clean.size(); ++k)
            if (dist(clean[k], wp) < bd) {
                bd = dist(clean[k], wp);
                bi = static_cast<int>(k);
            }
        r.goal_at.push_back(bi);
    }
    r.path = clean;
    r.ok = true;
    return r;
}

bool path_blocked_by(const std::vector<Pt>& obstacles, const Pt& start, const std::vector<Pt>& path, size_t from, double radius,
                     double lookahead, std::vector<Pt>* hits) {
    if (from >= path.size()) return false;
    std::vector<Pt> segs{start};
    double acc = 0;
    for (size_t i = from; i < path.size() && acc < lookahead; ++i) {
        double l = dist(segs.back(), path[i]);
        if (acc + l > lookahead && l > 1e-9) {  // 마지막 구간은 lookahead 지점에서 자른다
            double t = (lookahead - acc) / l;
            const Pt& a = segs.back();
            segs.push_back({a.first + (path[i].first - a.first) * t, a.second + (path[i].second - a.second) * t});
            break;
        }
        acc += l;
        segs.push_back(path[i]);
    }
    bool any = false;
    for (const auto& o : obstacles) {
        if (dist(o, start) < 0.12) continue;  // 로봇 몸 자체의 반사
        for (size_t i = 1; i < segs.size(); ++i)
            if (seg_dist(o, segs[i - 1], segs[i]) < radius) {
                any = true;
                if (hits) hits->push_back(o);
                break;
            }
    }
    return any;
}

}  // namespace hg
