// 코어 단위 시험(외부 프레임워크 없음). 맥과 흑갈리(Windows) 모두에서 돈다: make test
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/discovery.hpp"
#include "../src/image_decode.hpp"
#include "../src/json.hpp"
#include "../src/lidar.hpp"
#include "../src/planner.hpp"
#include "../src/path_tools.hpp"
#include "../src/robot.hpp"
#include "../src/util.hpp"
#include "../src/v2client.hpp"

using namespace hg;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            ++g_pass;                                                     \
        } else {                                                          \
            ++g_fail;                                                     \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

static void test_json() {
    Json j = Json::parse(R"({"a":1,"b":-2.5,"c":"x\"yé😀","d":[true,false,null],"e":{}})");
    CHECK(j["a"].is_int() && j["a"].as_int() == 1);
    CHECK_NEAR(j["b"].as_number(), -2.5, 1e-12);
    CHECK(j["c"].as_string() == "x\"y\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(j["d"].size() == 3 && j["d"][0].as_bool() && j["d"][2].is_null());
    CHECK(j["missing"].is_null() && j["e"].is_object());
    // 출력: 정수는 정수로, 실수는 실수로(로봇은 sequence 에 실수를 거부한다)
    Json o = Json::object();
    o.set("sequence", static_cast<int64_t>(9007199254740991LL)).set("forward", 0.0).set("turn", -0.25);
    CHECK(o.dump() == R"({"sequence":9007199254740991,"forward":0.0,"turn":-0.25})");
    bool threw = false;
    try {
        Json::parse("{\"a\":1,}");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        Json::parse(std::string(100, '[') + std::string(100, ']'));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);  // 깊이 제한
    CHECK(Json::parse(R"("\n\t")").as_string() == "\n\t");
}

static void test_ack() {
    V2Ack a;
    CHECK(parse_v2_ack(R"({"protocol":2,"type":"ack","client_id":"d8b27f10-09a8-4c21-b649-1e7058d46b72","sequence":124,"accepted":true,"reason":"ok","emergency_stop":false,"state":"idle"})", a));
    CHECK(a.sequence == 124 && a.accepted && a.state == "idle" && a.reason == "ok" && !a.emergency_stop);
    // invalid_packet 은 client_id/sequence 가 null/-1 일 수 있다
    CHECK(parse_v2_ack(R"({"protocol":2,"type":"ack","client_id":null,"sequence":-1,"accepted":false,"reason":"invalid_packet","emergency_stop":false,"state":"awaiting_neutral"})", a));
    CHECK(a.client_id.empty() && a.sequence == -1 && !a.accepted);
    // 9번째 필드가 있으면 거부
    CHECK(!parse_v2_ack(R"({"protocol":2,"type":"ack","client_id":"x","sequence":1,"accepted":true,"reason":"ok","emergency_stop":false,"state":"idle","extra":1})", a));
    CHECK(!parse_v2_ack(R"({"protocol":1,"type":"ack","client_id":"x","sequence":1,"accepted":true,"reason":"ok","emergency_stop":false,"state":"idle"})", a));
    CHECK(!parse_v2_ack("not json", a));
}

static DriveInput base_input() {
    DriveInput in;
    in.input_fresh = true;
    in.held = true;
    in.forward = 0.5;
    in.ack_fresh = true;
    in.state = "idle";
    return in;
}

static void test_drive_decision() {
    DriveInput in = base_input();
    CHECK(decide_drive(in).active);
    in.state = "awaiting_neutral";
    CHECK(!decide_drive(in).active);  // 중립 확인 전
    in = base_input();
    in.held = false;
    CHECK(!decide_drive(in).active);  // 데드맨 놓음
    in = base_input();
    in.input_fresh = false;
    CHECK(!decide_drive(in).active);  // 화면 멈춤
    in = base_input();
    in.ack_fresh = false;
    CHECK(!decide_drive(in).active);  // ACK 끊김
    in = base_input();
    in.ack_estop = true;
    CHECK(!decide_drive(in).active);
    in = base_input();
    in.estop_requested = true;
    CHECK(!decide_drive(in).active);
    in = base_input();
    in.state = "timeout";
    CHECK(!decide_drive(in).active);
    in.state = "active";
    CHECK(decide_drive(in).active);  // 타임아웃 뒤 중립 확인되면 자동 재개
    // 재프레스: 해제 직후 쥐고 있으면 움직이지 않고, 한 번 놓으면 풀린다
    in = base_input();
    in.release_required = true;
    DriveDecision d = decide_drive(in);
    CHECK(!d.active && d.release_required);
    in.held = false;
    d = decide_drive(in);
    CHECK(!d.active && !d.release_required);
    in.held = true;
    in.release_required = d.release_required;
    CHECK(decide_drive(in).active);
    // 두 축 0 이면 중립
    in = base_input();
    in.forward = 0;
    in.turn = 0;
    CHECK(!decide_drive(in).active);
}

static void test_discovery() {
    std::string nonce(32, 'a');
    FoundRobot r;
    CHECK(parse_discovery_reply("PUPPYPI_HERE/1|" + nonce + "|0123456789abcdef0123456789abcdef|puppy%20pi|5001|2", nonce, r));
    CHECK(r.name == "puppy pi" && r.control_port == 5001 && r.id == "0123456789abcdef0123456789abcdef");
    CHECK(!parse_discovery_reply("PUPPYPI_HERE/1|" + std::string(32, 'b') + "|0123456789abcdef0123456789abcdef|x|5001|2", nonce, r));
    CHECK(!parse_discovery_reply("PUPPYPI_HERE/1|" + nonce + "|XYZ|x|5001|2", nonce, r));
    CHECK(!parse_discovery_reply("PUPPYPI_HERE/1|" + nonce + "|0123456789abcdef0123456789abcdef|x|0|2", nonce, r));
    CHECK(url_decode("%ED%95%9C%EA%B8%80") == "\xED\x95\x9C\xEA\xB8\x80");
}

static void test_map_and_path() {
    // 로봇 문서 18 §4 의 식과 예제 기하. (문서 예제의 v_px 186 은 식으로 계산한 183.6 과 2.4 px 다르다 — 예시 값으로 본다)
    MapGeometry g{486, 369, -3.05, -3.10, 60.0};
    Pt uv = g.to_pixel({0.80, -0.01});
    CHECK_NEAR(uv.first, 231.0, 1e-9);
    CHECK_NEAR(uv.second, 183.6, 1e-9);
    Pt back = g.to_map(uv);
    CHECK_NEAR(back.first, 0.80, 1e-9);
    CHECK_NEAR(back.second, -0.01, 1e-9);

    // 직선에 잡음이 섞인 스케치 → 끝점 2개
    std::vector<Pt> stroke;
    for (int i = 0; i <= 100; ++i) stroke.emplace_back(i * 0.03, (i % 2 ? 0.01 : -0.01));
    auto p = sketch_to_path(stroke);
    CHECK(p.size() == 2);
    CHECK_NEAR(p.front().first, 0.0, 1e-9);
    CHECK_NEAR(p.back().first, 3.0, 1e-9);
    // L 자 → 3점
    stroke.clear();
    for (int i = 0; i <= 30; ++i) stroke.emplace_back(i * 0.1, 0.0);
    for (int i = 1; i <= 10; ++i) stroke.emplace_back(3.0, i * 0.1);
    p = sketch_to_path(stroke);
    CHECK(p.size() == 3);
    CHECK_NEAR(p[1].first, 3.0, 1e-9);
    CHECK_NEAR(p[1].second, 0.0, 1e-9);
    // 지그재그 많이 → 20점 이하
    stroke.clear();
    for (int i = 0; i < 400; ++i) stroke.emplace_back(i * 0.05, (i % 2) * 0.6);
    p = sketch_to_path(stroke);
    CHECK(p.size() <= 20 && p.size() >= 2);
    CHECK_NEAR(p.back().first, 399 * 0.05, 1e-9);
    // 한 점
    CHECK(sketch_to_path({{1.0, 2.0}}).size() == 1);
    CHECK_NEAR(polyline_length({{0, 0}, {3, 4}}), 5.0, 1e-12);
}

static void test_parsers() {
    MapInfo mi;
    CHECK(parse_map_info(Json::parse(R"({"media":"puppypi-media/1","width_px":486,"height_px":369,"left_m":-3.05,"bottom_m":-3.1,"px_per_m":60.0,"image_seq":42,"age_s":0.38,"robot":{"x_m":0.8,"y_m":-0.01,"yaw_rad":1.49,"u_px":231,"v_px":186}})"), mi));
    CHECK(mi.image_seq == 42 && mi.geo.width_px == 486);
    CHECK_NEAR(mi.robot_yaw, 1.49, 1e-12);
    CHECK(!parse_map_info(Json::parse(R"({"error":"no map yet"})"), mi));
    PathStatus ps;
    CHECK(parse_path_status(Json::parse(R"({"path":{"state":"drive","waiting":false,"reason":"","request_client_id":"abc","request_sequence":7,"points":[[1.0,0.0],[1.0,1.0]],"index":1,"count":2,"remaining_m":0.8,"blocked_count":0,"elapsed_s":3.2}})"), ps));
    CHECK(ps.state == "drive" && ps.request_sequence == 7 && ps.points.size() == 2 && ps.index == 1);
    CHECK_NEAR(ps.remaining_m, 0.8, 1e-12);
    ScanData sd;
    CHECK(parse_scan(Json::parse(R"({"angle_min":-3.14159,"angle_increment":0.01396,"range_max":12.0,"ranges":[1.0,null,2.5]})"), sd));
    CHECK(sd.ranges.size() == 3 && sd.ranges[1] < 0);
}

static void test_util() {
    std::string u = uuid4();
    CHECK(u.size() == 36 && u[8] == '-' && u[14] == '4');
    CHECK(base64_encode("hello") == "aGVsbG8=");
    CHECK(base64_encode("") == "");
    CHECK(split("a|b||c", '|').size() == 4);
    CHECK(trim("  x y \n") == "x y");
}


static void test_lidar() {
    // 3×2 격자 메시지(rosbridge publish 형식)
    std::string msg = R"({"op":"publish","topic":"/map","msg":{"header":{"frame_id":"map"},"info":{"resolution":0.05,"width":3,"height":2,"origin":{"position":{"x":-15.0,"y":-15.0,"z":0.0},"orientation":{"x":0,"y":0,"z":0,"w":1}}},"data":[-1,0,100, 0 ,-1,  55]}})";
    OccGrid g;
    CHECK(parse_occupancy_grid(msg, g));
    CHECK(g.w == 3 && g.h == 2 && g.cells.size() == 6 && g.cells[2] == 100 && g.cells[5] == 55);
    CHECK_NEAR(g.ox, -15.0, 1e-12);
    CHECK(g.x0 == 0 && g.x1 == 2 && g.y0 == 0 && g.y1 == 1);
    CHECK(cell_at(g, -15.0 + 0.11, -15.0 + 0.01) == 100 && cell_at(g, -15.0 + 0.01, -15.0 + 0.06) == 0 && cell_at(g, 0, 0) == -2);
    CHECK(!parse_occupancy_grid(R"({"msg":{"info":{"width":3,"height":3,"resolution":0.05},"data":[0,0]}})", g));  // 크기 불일치
    CHECK_NEAR(yaw_from_quaternion(0, 0, std::sin(0.6), std::cos(0.6)), 1.2, 1e-9);
    Pose2D pose{1.0, 2.0, 3.14159265358979 / 2};
    auto pts = scan_to_map({1.0f, -1.0f, 0.0f}, 0.0, 0.1, 12.0, pose);  // 정면 1 m → 지도 (1, 3)
    CHECK(pts.size() == 1);
    CHECK_NEAR(pts[0].first, 1.0, 1e-6);
    CHECK_NEAR(pts[0].second, 3.0, 1e-6);
    std::string out;
    CHECK(base64_decode("aGVsbG8=", out) && out == "hello");
    CHECK(!base64_decode("a$b", out));
}

static double min_clearance(const std::vector<Pt>& path, const Pt& start, const std::vector<Pt>& obs) {
    double best = 1e9;
    std::vector<Pt> all{start};
    all.insert(all.end(), path.begin(), path.end());
    for (size_t i = 1; i < all.size(); ++i)
        for (int k = 0; k <= 50; ++k) {
            double t = k / 50.0;
            Pt p{all[i - 1].first + (all[i].first - all[i - 1].first) * t, all[i - 1].second + (all[i].second - all[i - 1].second) * t};
            for (const auto& o : obs) best = std::min(best, std::hypot(p.first - o.first, p.second - o.second));
        }
    return best;
}

static void test_planner() {
    PlanConfig cfg;
    // 1) 막힌 것 없음 → 그린 그대로
    PlanResult r = plan_path(nullptr, {}, {0, 0}, {{2, 0}, {2, 1}}, cfg);
    CHECK(r.ok && !r.detoured && r.path.size() == 2);
    // 2) 직선 위에 장애물(0.4 m 폭 벽 조각) → 돌아감, 장애물에서 inflate 이상 떨어짐
    std::vector<Pt> obs;
    for (double y = -0.2; y <= 0.2001; y += 0.05) obs.push_back({1.0, y});
    r = plan_path(nullptr, obs, {0, 0}, {{2, 0}}, cfg);
    CHECK(r.ok && r.detoured && r.path.size() >= 2 && r.path.size() <= 20);
    CHECK(min_clearance(r.path, {0, 0}, obs) >= cfg.inflate - 0.06);
    CHECK_NEAR(r.path.back().first, 2.0, 1e-9);
    // 3) 긴 벽에 틈 하나(0.8 m) → 틈으로 지나감
    obs.clear();
    for (double y = -3; y <= 3.0001; y += 0.05)
        if (std::fabs(y - 1.5) > 0.4) obs.push_back({1.0, y});
    r = plan_path(nullptr, obs, {0, 0}, {{2, 0}}, cfg);
    CHECK(r.ok && r.detoured);
    CHECK(min_clearance(r.path, {0, 0}, obs) >= cfg.inflate - 0.06);
    // 4) 완전히 막힌 벽(계획 범위보다 넓게) → 실패와 이유
    obs.clear();
    for (double y = -6; y <= 6.0001; y += 0.05) obs.push_back({1.0, y});
    r = plan_path(nullptr, obs, {0, 0}, {{2, 0}}, cfg);
    CHECK(!r.ok && !r.reason.empty());
    // 5) 경유점이 장애물 안 → 실패
    r = plan_path(nullptr, {{2.0, 0.0}}, {0, 0}, {{2.05, 0}}, cfg);
    CHECK(!r.ok);
    // 6) 점유 격자 지도로 계획: 벽(점유 100)을 피한다
    OccGrid g;
    g.w = 100; g.h = 100; g.res = 0.05; g.ox = -1; g.oy = -2.5;
    g.cells.assign(10000, 0);
    for (int y = 0; y < 100; ++y)
        for (int x = 39; x <= 41; ++x)
            if (y < 30 || y > 46) g.cells[y * 100 + x] = 100;  // x≈1 m 벽, 틈 y∈[-1.0,-0.2]
    r = plan_path(&g, {}, {0, 0}, {{2, 0}}, cfg);
    CHECK(r.ok && r.detoured);
    // 7) 남은 경로 감시
    std::vector<Pt> hits;
    CHECK(path_blocked_by({{1.0, 0.05}}, {0, 0}, {{2, 0}}, 0, 0.2, 1.5, &hits) && hits.size() == 1);
    CHECK(!path_blocked_by({{1.0, 0.5}}, {0, 0}, {{2, 0}}, 0, 0.2, 1.5));
    CHECK(!path_blocked_by({{1.8, 0.0}}, {0, 0}, {{2, 0}}, 0, 0.2, 1.5));  // 앞쪽 1.5 m 밖
    // 8) 막힘 판정: 4번 중 3번, 재계획 뒤 1.5 s 쿨다운, 한 번 튄 값은 무시
    BlockTrigger t;
    CHECK(!t.push(true, 10.0) && !t.push(false, 10.1) && !t.push(true, 10.2));
    CHECK(t.push(true, 10.3));           // 4번 중 3번
    t.replanned(10.3);
    CHECK(!t.push(true, 10.4) && !t.push(true, 10.5) && !t.push(true, 10.6));  // 쿨다운 중
    CHECK(t.push(true, 11.9));           // 쿨다운 끝, 여전히 막힘
    t.reset();
    CHECK(!t.push(true, 20) && !t.push(false, 20.1) && !t.push(false, 20.2) && !t.push(true, 20.3) && !t.push(false, 20.4));
    // 9) 남은 사용자 경유점: 로봇이 index 번째 점으로 가는 중
    std::vector<Pt> goals{{1, 0}, {2, 0}, {3, 0}};
    std::vector<int> at{0, 2, 4};
    CHECK(remaining_goals(goals, at, 0).size() == 3);
    CHECK(remaining_goals(goals, at, 1).size() == 2 && remaining_goals(goals, at, 1)[0].first == 2);
    CHECK(remaining_goals(goals, at, 5).size() == 1 && remaining_goals(goals, at, 5)[0].first == 3);  // 끝 무렵
    // 10) 얇은 벽(x=0, 길이 2 m)에 붙어 출발해도 벽을 뚫고 가지 않는다(전에는 0.10 m 안에서 직선이 벽을 통과했다)
    obs.clear();
    for (double y = -1.0; y <= 1.0001; y += 0.02) obs.push_back({0.0, y});
    auto crosses_wall = [](const Pt& start, const std::vector<Pt>& path) {
        Pt prev = start;
        for (const auto& p : path) {
            if ((prev.first < 0) != (p.first < 0)) {
                double y = prev.second + (0 - prev.first) / (p.first - prev.first) * (p.second - prev.second);
                if (y > -1.0 && y < 1.0) return true;
            }
            prev = p;
        }
        return false;
    };
    for (double d : {0.05, 0.09, 0.10, 0.15}) {
        Pt s{-d, 0.0};
        r = plan_path(nullptr, obs, s, {{0.6, 0.0}}, cfg);
        CHECK(r.ok && !crosses_wall(s, r.path));
    }
}

int main() {
    test_json();
    test_ack();
    test_drive_decision();
    test_discovery();
    test_map_and_path();
    test_parsers();
    test_util();
    test_lidar();
    test_planner();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
