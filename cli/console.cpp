// 텍스트 클라이언트: 같은 코어로 로봇(또는 가짜 로봇)을 다룬다. 통합 시험 스크립트와 측정에 쓴다.
//   console --host 127.0.0.1                 대화형
//   console --host 127.0.0.1 --script f.txt   스크립트(expect 실패 시 종료 코드 1)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>

#include "catalog.hpp"
#include "discovery.hpp"
#include "robot.hpp"
#include "sock.hpp"
#include "util.hpp"

using namespace hg;

static void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

static void print_status(Robot& r) {
    RobotSnapshot s = r.snapshot();
    const V2Snapshot& v = s.v2;
    std::printf("robot %s  v2 state=%s reason=%s estop=%d owner=%d linked=%d  rtt %.1f/%.1f ms  loss %.1f%%  tx %llu rx %llu\n",
                s.host.c_str(), v.state.c_str(), v.reason.c_str(), v.estop, v.owner, v.linked, v.rtt_last_ms, v.rtt_avg_ms,
                v.loss_pct, static_cast<unsigned long long>(v.tx), static_cast<unsigned long long>(v.rx));
    std::printf("rosbridge %s  http %d  map %s (%.2f, %.2f, %.0f°)  path %s %d/%d rem %.2f  scan %zu beams  camera %s %.1f fps\n",
                s.rb_status.c_str(), s.http_ok, s.map.valid ? "ok" : "-", s.map.robot_x, s.map.robot_y,
                s.map.robot_yaw * 180 / 3.14159265, s.path.valid ? s.path.state.c_str() : "-", s.path.index, s.path.count,
                s.path.remaining_m, s.scan.ranges.size(), r.camera_status().c_str(), r.camera_fps());
    if (s.arbiter.is_object()) std::printf("arbiter %s\n", s.arbiter.dump().c_str());
    if (!s.last_result.empty()) std::printf("last %s\n", s.last_result.c_str());
}

// 조건이 참이 될 때까지 기다린다(최대 timeout)
template <class F>
static bool wait_for(F cond, double timeout) {
    double end = now_s() + timeout;
    while (now_s() < end) {
        if (cond()) return true;
        sleep_s(0.02);
    }
    return cond();
}

static void help() {
    std::puts(
        "명령: discover | connect <ip> | disconnect | status | speed <0.1-1>\n"
        "      drive <forward -1..1> <turn -1..1> <초>   (데드맨을 쥔 채 초 동안 운전)\n"
        "      estop | clear | home | pose <이름> | action <이름> | poses | actions\n"
        "      path x,y [x,y ...] | cancel | savemap | resetmap | topics | snap <파일.jpg>\n"
        "      wait <초> | expect v2 <state> [초] | expect path <state> [초] | expect estop <0|1> [초]\n"
        "      expect result <문구> [초] | rtt <초> | sniff on|off | log | quit\n"
        "      go x,y [x,y ...] (장애물 회피) | nav | http <경로> | expect nav <phase> | expect replans <n> | expect map");
}

// 0 = 계속, 1 = expect 실패, 2 = 종료
static int run_line(Robot& r, const std::string& raw) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') return 0;
    auto a = split_ws(line);
    const std::string& c = a[0];
    if (c == "help") {
        help();
    } else if (c == "quit" || c == "exit") {
        return 2;
    } else if (c == "discover") {
        auto found = discover_robots(1500);
        std::printf("찾은 로봇 %zu대\n", found.size());
        for (const auto& f : found) std::printf("  %s  %s  id %s  port %u\n", f.ip.c_str(), f.name.c_str(), f.id.c_str(), f.control_port);
    } else if (c == "connect" && a.size() >= 2) {
        std::string err;
        if (!r.connect(a[1], &err)) std::printf("연결 실패: %s\n", err.c_str());
    } else if (c == "disconnect") {
        r.disconnect();
    } else if (c == "status") {
        print_status(r);
    } else if (c == "speed" && a.size() >= 2) {
        r.v2().set_speed_scale(std::atof(a[1].c_str()));
    } else if (c == "drive" && a.size() >= 4) {
        double f = std::atof(a[1].c_str()), t = std::atof(a[2].c_str()), dur = std::atof(a[3].c_str());
        double end = now_s() + dur;
        while (now_s() < end) {
            r.v2().set_drive(f, t, true);
            sleep_s(0.02);
        }
        r.v2().set_drive(0, 0, false);
    } else if (c == "estop") {
        r.emergency_stop_all();
    } else if (c == "clear") {
        r.clear_emergency_all();
    } else if (c == "home") {
        r.go_home();
    } else if (c == "pose" && a.size() >= 2) {
        r.set_pose(a[1]);
    } else if (c == "action" && a.size() >= 2) {
        r.run_action(a[1]);
    } else if (c == "poses") {
        for (const auto& p : pose_presets()) std::printf("  %-16s %s\n", p.name, p.label);
    } else if (c == "actions") {
        for (const auto& g : action_groups()) std::printf("  %-20s %s%s\n", g.name, g.label, g.caution ? "  (주의)" : "");
    } else if (c == "path" && a.size() >= 2) {
        std::vector<Pt> pts;
        for (size_t i = 1; i < a.size(); ++i) {
            auto xy = split(a[i], ',');
            if (xy.size() == 2) pts.emplace_back(std::atof(xy[0].c_str()), std::atof(xy[1].c_str()));
        }
        std::string err;
        if (!r.send_path(pts, &err)) std::printf("경로 실패: %s\n", err.c_str());
    } else if (c == "go" && a.size() >= 2) {
        // 장애물을 피하는 경로(앱이 계획하고, 가는 중에 막히면 다시 계획)
        std::vector<Pt> pts;
        for (size_t i = 1; i < a.size(); ++i) {
            auto xy = split(a[i], ',');
            if (xy.size() == 2) pts.emplace_back(std::atof(xy[0].c_str()), std::atof(xy[1].c_str()));
        }
        std::string err;
        if (!r.go_path(pts, &err)) std::printf("회피 경로 실패: %s\n", err.c_str());
        else {
            NavStatus n = r.snapshot().nav;
            std::printf("보냄 %zu점%s:", n.planned.size(), n.detoured ? " (우회)" : "");
            for (const auto& p : n.planned) std::printf(" (%.2f,%.2f)", p.first, p.second);
            std::printf("\n");
        }
    } else if (c == "nav") {
        NavStatus n = r.snapshot().nav;
        std::printf("nav %s · %s · 우회 %d회 · 경로 %zu점\n", n.phase.c_str(), n.message.c_str(), n.replans, n.planned.size());
    } else if (c == "http" && a.size() >= 2) {
        HttpResponse h = http_get(r.host(), 8090, a[1], 1500);
        std::printf("%d %s\n", h.status, h.body.substr(0, 300).c_str());
    } else if (c == "cancel") {
        r.cancel_path();
    } else if (c == "savemap") {
        r.save_map();
    } else if (c == "resetmap") {
        r.reset_map();
    } else if (c == "topics") {
        r.list_topics();
    } else if (c == "snap" && a.size() >= 2) {
        std::string err;
        std::printf("%s\n", r.save_snapshot_jpeg(a[1], &err) ? "저장함" : err.c_str());
    } else if (c == "wait" && a.size() >= 2) {
        sleep_s(std::atof(a[1].c_str()));
    } else if (c == "sniff" && a.size() >= 2) {
        r.set_packet_capture(a[1] == "on");
    } else if (c == "log") {
        for (const auto& l : r.events().tail(30)) std::printf("%s\n", l.c_str());
        if (r.packet_capture())
            for (const auto& l : r.packets().tail(10)) std::printf("%s\n", l.c_str());
    } else if (c == "rtt" && a.size() >= 2) {
        // 지정 시간 동안 RTT 표본을 모아 중앙값·p95 를 보고(같은 기기 단조 시계)
        std::vector<double> xs;
        double end = now_s() + std::atof(a[1].c_str());
        double last = -1;
        uint64_t last_rx = 0;
        while (now_s() < end) {
            V2Snapshot v = r.v2().snapshot();
            if (v.rx != last_rx && v.rtt_last_ms >= 0 && v.rtt_last_ms != last) xs.push_back(v.rtt_last_ms);
            last_rx = v.rx;
            last = v.rtt_last_ms;
            sleep_s(0.01);
        }
        std::sort(xs.begin(), xs.end());
        if (xs.empty()) {
            std::puts("RTT 표본 없음");
        } else {
            std::printf("RTT n=%zu  median %.2f ms  p95 %.2f ms  max %.2f ms\n", xs.size(), xs[xs.size() / 2],
                        xs[std::min(xs.size() - 1, xs.size() * 95 / 100)], xs.back());
        }
    } else if (c == "expect" && a.size() >= 3) {
        // expect <무엇> <기대값 여러 단어 가능> [초]: 마지막 토큰이 숫자면 시간 제한. map 은 기대값이 없다(expect map [초])
        double tmo = 3.0;
        size_t last = a.size();
        char* end = nullptr;
        double v = std::strtod(a.back().c_str(), &end);
        size_t min_tokens = a[1] == "map" ? 3 : 4;
        if (a.size() >= min_tokens && end && *end == '\0') {
            tmo = v;
            --last;
        }
        const std::string& what = a[1];
        std::string want = a[2];
        for (size_t i = 3; i < last; ++i) want += " " + a[i];
        bool ok = false;
        if (what == "v2") ok = wait_for([&] { return r.v2().snapshot().state == want; }, tmo);
        else if (what == "path") ok = wait_for([&] { return r.snapshot().path.state == want; }, tmo);
        else if (what == "estop") ok = wait_for([&] { return r.snapshot().v2.estop == (want == "1"); }, tmo);
        else if (what == "result")  // "서기 실패 …" 에도 '서기' 가 들어 있으므로 실패 문구는 통과시키지 않는다
            ok = wait_for([&] {
                std::string res = r.snapshot().last_result;
                return res.find(want) != std::string::npos && res.find("실패") == std::string::npos;
            }, tmo);
        else if (what == "nav") ok = wait_for([&] { return r.snapshot().nav.phase == want; }, tmo);
        else if (what == "replans") ok = wait_for([&] { return r.snapshot().nav.replans >= std::atoi(want.c_str()); }, tmo);
        else if (what == "map") ok = wait_for([&] { return r.snapshot().grid != nullptr; }, tmo);
        else if (what == "http") {  // expect http <경로> <포함할 문구...>
            auto sp = want.find(' ');
            std::string path = want.substr(0, sp), needle = sp == std::string::npos ? "" : want.substr(sp + 1);
            ok = wait_for([&] { return http_get(r.host(), 8090, path, 1000).body.find(needle) != std::string::npos; }, tmo);
        }
        std::printf("%s expect %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), want.c_str());
        if (!ok) {
            print_status(r);
            return 1;
        }
    } else {
        std::printf("모르는 명령: %s (help)\n", line.c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    sock_init();
    std::string host, script;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--host" && i + 1 < argc) host = argv[++i];
        else if (s == "--script" && i + 1 < argc) script = argv[++i];
        else {
            std::puts("사용법: console [--host <ip>] [--script <파일>]");
            return 2;
        }
    }
    Robot robot;
    if (!host.empty()) {
        std::string err;
        if (!robot.connect(host, &err)) {
            std::printf("연결 실패: %s\n", err.c_str());
            return 1;
        }
    }
    int failures = 0;
    if (!script.empty()) {
        std::ifstream f(script);
        if (!f) {
            std::printf("스크립트를 열 수 없음: %s\n", script.c_str());
            return 2;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (!trim(line).empty() && trim(line)[0] != '#') std::printf("> %s\n", trim(line).c_str());
            int rc = run_line(robot, line);
            if (rc == 1) ++failures;
            if (rc == 2) break;
        }
        robot.disconnect();
        std::printf("스크립트 끝: expect 실패 %d개\n", failures);
        return failures ? 1 : 0;
    }
    help();
    std::string line;
    while (std::printf("heukgalli> "), std::fflush(stdout), std::getline(std::cin, line))
        if (run_line(robot, line) == 2) break;
    robot.disconnect();
    return 0;
}
