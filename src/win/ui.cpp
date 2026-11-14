#include "ui.hpp"  // sock.hpp(winsock2) 가 windows.h 보다 먼저 들어온다

#include "catalog.hpp"
#include "imgui.h"
#include "theme.hpp"
#include "util.hpp"
#include "widgets.hpp"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

namespace hg {

const char* state_label(const std::string& st) {
    if (st == "idle") return "대기(중립 확인됨)";
    if (st == "active") return "주행 중";
    if (st == "awaiting_neutral") return "중립 확인 중";
    if (st == "timeout") return "시간 초과 → 중립 필요";
    if (st == "emergency_stop") return "비상정지";
    if (st == "fault") return "로봇 오류";
    return "응답 없음";
}

const char* reason_label(const std::string& r) {
    if (r == "not_owner") return "다른 조종기가 제어 중";
    if (r == "neutral_required") return "중립 먼저 필요";
    if (r == "emergency_stop") return "비상정지 중";
    if (r == "out_of_order") return "순서 어긋남";
    if (r == "invalid_packet") return "잘못된 패킷";
    if (r == "fault") return "로봇 오류";
    return r.c_str();
}

const char* path_label(const std::string& st) {
    if (st == "idle") return "경로 없음";
    if (st == "rotate") return "제자리 회전";
    if (st == "drive") return "이동 중";
    if (st == "blocked") return "정지(센서 확인)";
    if (st == "done") return "도착";
    if (st == "cancelled") return "취소됨";
    return "-";
}

std::string time_tag() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d_%H%M%S", &tm);
    return b;
}

// ---------------- 설정 ----------------

bool Settings::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        Json j = Json::parse(ss.str());
        last_host = j["last_host"].as_string();
        single_stick = j["single_stick"].as_bool(single_stick);
        tilt_enabled = j["tilt_enabled"].as_bool(tilt_enabled);
        tilt_invert_forward = j["tilt_invert_forward"].as_bool(tilt_invert_forward);
        tilt_invert_turn = j["tilt_invert_turn"].as_bool(tilt_invert_turn);
        max_speed = static_cast<float>(j["max_speed"].as_number(max_speed));
        deadzone = static_cast<float>(j["deadzone"].as_number(deadzone));
        camera = j["camera"].as_bool(camera);
        sniff = j["sniff"].as_bool(sniff);
    } catch (const std::exception&) {
        return false;
    }
    max_speed = std::max(0.1f, std::min(1.0f, max_speed));
    deadzone = std::max(0.05f, std::min(0.4f, deadzone));
    return true;
}

bool Settings::save(const std::string& path) const {
    Json j = Json::object();
    j.set("last_host", last_host).set("single_stick", single_stick).set("tilt_enabled", tilt_enabled);
    j.set("tilt_invert_forward", tilt_invert_forward).set("tilt_invert_turn", tilt_invert_turn);
    j.set("max_speed", static_cast<double>(max_speed)).set("deadzone", static_cast<double>(deadzone));
    j.set("camera", camera).set("sniff", sniff);
    std::ofstream f(path);
    if (!f) return false;
    f << j.dump() << "\n";
    return true;
}

// ---------------- 앱 ----------------

App::App(ID3D11Device* dev, ID3D11DeviceContext* ctx, void* hwnd, std::string exe_dir, float dpi_scale)
    : dev_(dev), ctx_(ctx), hwnd_(hwnd), exe_dir_(std::move(exe_dir)), s_(dpi_scale),
      cam_dec_([this](std::string& j, double& rx, uint64_t& seq) { return robot_.camera_frame(j, rx, seq); }, 30) {
    cfg_.load(exe_dir_ + "\\config.json");
    robot_.v2().set_speed_scale(cfg_.max_speed);
    robot_.set_camera_enabled(cfg_.camera);
    robot_.set_packet_capture(cfg_.sniff);
    pad_.deadzone = cfg_.deadzone;
    tilt_drive_.invert_forward = cfg_.tilt_invert_forward;
    tilt_drive_.invert_turn = cfg_.tilt_invert_turn;
    tilt_.start(hwnd_);
    cam_dec_.start();
    ip_entry_ = cfg_.last_host;
    if (!cfg_.last_host.empty()) connect_to(cfg_.last_host);
}

App::~App() {
    pad_.stop_rumble();
    cam_dec_.stop();
    tilt_.stop();
    robot_.disconnect();  // 중립 + disconnect
    save_settings();
}

void App::save_settings() { cfg_.save(exe_dir_ + "\\config.json"); }

bool App::select_screen(const std::string& name) {
    static const std::pair<const char*, Screen> names[] = {
        {"drive", Screen::Drive}, {"map", Screen::Map}, {"robot", Screen::Robot}, {"log", Screen::Log}, {"connect", Screen::Connect}};
    for (const auto& n : names)
        if (name == n.first) {
            screen_ = n.second;
            return true;
        }
    return false;
}

void App::set_focus(bool active) {
    focus_ = active;
    if (!active) robot_.v2().set_drive(0, 0, false);  // 포커스를 잃으면 즉시 중립
}

void App::connect_to(const std::string& host) {
    std::string err;
    if (robot_.connect(host, &err)) {
        cfg_.last_host = host;
        save_settings();
        screen_ = Screen::Drive;
    } else {
        toast_ = "연결 실패: " + err;
        toast_until_ = now_s() + 4;
    }
}

void App::estop_all() {
    robot_.emergency_stop_all();
    pad_.rumble(Rumble::Long);
}

void App::frame(double now) {
    pad_.poll(now);
    if (pad_.state().connected != prev_pad_) {
        prev_pad_ = pad_.state().connected;
        robot_.events().add(prev_pad_ ? "게임패드 연결됨(XInput)" : "게임패드 없음 — Armoury Crate 에서 게임패드 모드인지 확인");
    }
    RobotSnapshot s = robot_.snapshot();
    handle_input(now, s);
    feedback(now, s);
    update_textures();
    draw(now, s);
}

void App::update_textures() {
    int w = 0, h = 0;
    double rx = 0;
    uint64_t seq = 0;
    if (cam_dec_.take(scratch_, w, h, rx, seq) && cam_tex_.update(dev_, ctx_, scratch_.data(), w, h)) {
        cam_tex_.rx_time = rx;
        cam_tex_.seq = seq;
    }
}

// ---------------- 입력 ----------------

void App::handle_input(double now, const RobotSnapshot& s) {
    const PadState& p = pad_.state();
    double forward = 0, turn = 0;
    bool held = false;
    drive_source_ = "-";

    bool tilt_mode = cfg_.tilt_enabled && tilt_.available() && p.down(kRB);
    if (tilt_mode) {
        float x, y, z;
        if (tilt_.gravity(x, y, z)) {
            if (p.hit(kRB)) tilt_drive_.calibrate(x, y, z);  // 쥐는 순간의 자세가 기준(0)
            tilt_drive_.map(x, y, z, forward, turn);
            held = true;
            drive_source_ = "기울기(RB)";
        }
    } else if (p.connected) {
        forward = p.ly;
        turn = cfg_.single_stick ? -p.lx : -p.rx;  // 스틱 오른쪽 = 우회전 = turn 음수
        held = p.rt >= 0.5f;
        if (held) {
            // 트리거를 더 깊게 쥘수록 빠르게(0.5 → 50 %, 1.0 → 100 %), LT 정밀 모드 ×0.4
            double depth = 0.5 + (p.rt - 0.5) * 1.0;
            double mult = depth * (p.lt >= 0.5f ? 0.4 : 1.0);
            forward *= mult;
            turn *= mult;
            drive_source_ = p.lt >= 0.5f ? "패드(정밀)" : "패드";
        }
    }
    // 패드가 없을 때 개발·시연용 키보드(--kbdrive 로 켰을 때만): Shift 가 데드맨, W/S/A/D 로 방향
    if (kbdrive_ && !p.connected && focus_ && !ImGui::GetIO().WantTextInput) {
        auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        double kf = (key('W') ? 1 : 0) - (key('S') ? 1 : 0);
        double kt = (key('A') ? 1 : 0) - (key('D') ? 1 : 0);
        if (key(VK_SHIFT) && (kf != 0 || kt != 0)) {
            forward = kf * 0.6;
            turn = kt * 0.6;
            held = true;
            drive_source_ = "키보드";
        }
    }
    // 화면을 보지 않을 때(포커스 없음)·연결 화면에서는 운전하지 않는다
    if (!focus_ || screen_ == Screen::Connect) held = false;
    held_ = held;
    in_forward_ = held ? forward : 0;
    in_turn_ = held ? turn : 0;
    if (s.connected) robot_.v2().set_drive(forward, turn, held);

    if (!p.connected) return;
    // 버튼(운전 중에는 몸을 움직이는 버튼을 무시)
    if (p.hit(kB)) estop_all();
    if (p.hit(kX)) robot_.cancel_path();
    if (p.hit(kY)) hud_layer_ = (hud_layer_ + 1) % 3;
    if (p.hit(kA) && !held) robot_.go_home();
    if (p.hit(kLeftStick)) {
        speed_full_ = !speed_full_;
        robot_.v2().set_speed_scale(speed_full_ ? 1.0 : cfg_.max_speed);
        toast_ = speed_full_ ? "최고 속도 100 %" : "최고 속도 " + std::to_string(int(cfg_.max_speed * 100 + 0.5)) + " %";
        toast_until_ = now + 2;
    }
    int np = static_cast<int>(pose_presets().size()), na = static_cast<int>(action_groups().size());
    // 자세는 고른 뒤 0.3 초 동안 더 바꾸지 않으면 보낸다(빠르게 넘길 때 중간 자세를 다 보내지 않게)
    if (p.hit(kDpadUp)) {
        pose_index_ = (pose_index_ + np - 1) % np;
        pose_publish_at_ = now + 0.3;
    }
    if (p.hit(kDpadDown)) {
        pose_index_ = (pose_index_ + 1) % np;
        pose_publish_at_ = now + 0.3;
    }
    if (pose_publish_at_ > 0 && now >= pose_publish_at_) {
        pose_publish_at_ = 0;
        if (!held) robot_.set_pose(pose_presets()[pose_index_].name);
    }
    if (p.hit(kDpadLeft)) action_index_ = (action_index_ + na - 1) % na;
    if (p.hit(kDpadRight)) action_index_ = (action_index_ + 1) % na;
    if (p.hit(kLB) && !held) {
        const ActionGroup& a = action_groups()[action_index_];
        if (a.caution && !(caution_action_ == a.name && now < caution_armed_until_)) {
            caution_action_ = a.name;
            caution_armed_until_ = now + 2.0;
            toast_ = std::string("'") + a.label + "' 은 큰 동작입니다. 2초 안에 LB 를 한 번 더";
            toast_until_ = now + 2;
        } else {
            caution_action_.clear();
            robot_.run_action(a.name);
            pad_.rumble(Rumble::Tick);
        }
    }
    if (p.hit(kRB) && !cfg_.tilt_enabled) {
        CreateDirectoryA((exe_dir_ + "\\shots").c_str(), nullptr);
        std::string path = exe_dir_ + "\\shots\\cam_" + time_tag() + ".jpg";
        std::string err;
        toast_ = robot_.save_snapshot_jpeg(path, &err) ? "사진 저장: shots\\cam_" + time_tag() + ".jpg" : "사진 실패: " + err;
        toast_until_ = now + 3;
        pad_.rumble(Rumble::Tick);
    }
    if (p.hit(kView)) {
        static const Screen order[] = {Screen::Drive, Screen::Map, Screen::Robot, Screen::Log};
        int i = 0;
        for (int k = 0; k < 4; ++k)
            if (order[k] == screen_) i = k;
        screen_ = order[(i + 1) % 4];
    }
    if (p.hit(kMenu)) screen_ = screen_ == Screen::Connect ? Screen::Drive : Screen::Connect;
    // View + Menu 를 1.5 초 → 종료
    if (p.down(kView) && p.down(kMenu)) {
        if (view_menu_hold_ == 0) view_menu_hold_ = now;
        if (now - view_menu_hold_ > 1.5) quit_ = true;
    } else {
        view_menu_hold_ = 0;
    }
}

// 화면을 보지 않아도 손으로 알 수 있게: 상태 변화 → 진동
void App::feedback(double now, const RobotSnapshot& s) {
    const V2Snapshot& v = s.v2;
    if (v.estop && !prev_estop_) pad_.rumble(Rumble::Long);
    prev_estop_ = v.estop;
    if (s.connected && prev_linked_ && !v.linked) pad_.rumble(Rumble::Double);
    prev_linked_ = v.linked;
    // 운전하려는데 로봇이 거부하면(다른 조종기·중립 필요 등) 1 초에 한 번
    std::string rej = held_ && v.linked && !v.accepted ? v.reason : "";
    if (!rej.empty() && (rej != prev_reject_ || now - last_reject_rumble_ > 1.0)) {
        pad_.rumble(Rumble::Double);
        last_reject_rumble_ = now;
    }
    prev_reject_ = rej;
    if (s.path.valid && s.path.state != prev_path_state_) {
        if (s.path.state == "done") pad_.rumble(Rumble::Done);
        if (s.path.state == "cancelled" || s.path.state == "blocked") pad_.rumble(Rumble::Double);
        prev_path_state_ = s.path.state;
    }
    if (s.last_result != prev_result_) {
        if (!prev_result_.empty() || !s.last_result.empty()) {
            toast_ = s.last_result;
            toast_until_ = now + 3;
        }
        prev_result_ = s.last_result;
    }
    if (v.rx != rtt_last_rx_ && v.rtt_last_ms >= 0) {
        rtt_hist_.push_back(static_cast<float>(v.rtt_last_ms));
        if (rtt_hist_.size() > 600) rtt_hist_.erase(rtt_hist_.begin(), rtt_hist_.begin() + 100);
    }
    rtt_last_rx_ = v.rx;
}

}  // namespace hg
