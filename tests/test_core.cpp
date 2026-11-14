// 코어 단위 시험(외부 프레임워크 없음). 맥과 흑갈리(Windows) 모두에서 돈다: make test
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/discovery.hpp"
#include "../src/json.hpp"
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

int main() {
    test_json();
    test_ack();
    test_drive_decision();
    test_discovery();
    test_map_and_path();
    test_parsers();
    test_util();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
