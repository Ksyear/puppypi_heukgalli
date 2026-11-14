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

namespace {
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
}  // namespace

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
      cam_dec_([this](std::string& j, double& rx, uint64_t& seq) { return robot_.camera_frame(j, rx, seq); }, 30),
      map_dec_([this](std::string& j, double& rx, uint64_t& seq) { return robot_.map_frame(j, rx, seq); }, 4) {
    cfg_.load(exe_dir_ + "\\config.json");
    robot_.v2().set_speed_scale(cfg_.max_speed);
    robot_.set_camera_enabled(cfg_.camera);
    robot_.set_packet_capture(cfg_.sniff);
    pad_.deadzone = cfg_.deadzone;
    tilt_drive_.invert_forward = cfg_.tilt_invert_forward;
    tilt_drive_.invert_turn = cfg_.tilt_invert_turn;
    tilt_.start(hwnd_);
    cam_dec_.start();
    map_dec_.start();
    ip_entry_ = cfg_.last_host;
    if (!cfg_.last_host.empty()) connect_to(cfg_.last_host);
}

App::~App() {
    pad_.stop_rumble();
    cam_dec_.stop();
    map_dec_.stop();
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
    if (map_dec_.take(scratch_, w, h, rx, seq) && map_tex_.update(dev_, ctx_, scratch_.data(), w, h)) {
        map_tex_.rx_time = rx;
        map_tex_.seq = seq;
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

// ---------------- 화면 (docs/DESIGN_PLAN_v1.md) ----------------

namespace {
using theme::F;
using theme::T;
namespace sz = theme::sz;
namespace ic = theme::icon;
using w::Pad;
using w::Tone;

// 주행 상태 캡슐의 내용: 색 + 램프 모양 + 짧은 글자(세 겹으로 같은 뜻)
struct Capsule {
    Tone tone;
    bool filled;
    const char* lamp;
    std::string text;
};

Tone toast_tone(const std::string& t) {
    if (t.find("실패") != std::string::npos || t.find("비상") != std::string::npos || t.find("거부") != std::string::npos)
        return Tone::Danger;
    if (t.find("큰 동작") != std::string::npos || t.find("확인") != std::string::npos) return Tone::Warn;
    return Tone::Info;
}

Tone event_tone(const std::string& t) {
    if (t.find("실패") != std::string::npos || t.find("끊") != std::string::npos || t.find("비상") != std::string::npos ||
        t.find("없음") != std::string::npos)
        return Tone::Danger;
    if (t.find("거부") != std::string::npos || t.find("취소") != std::string::npos || t.find("중립") != std::string::npos)
        return Tone::Warn;
    return Tone::Info;
}
}  // namespace

static Capsule capsule_for(const RobotSnapshot& s, bool held, bool focus, bool connect_screen, bool pad, const std::string& source,
                           int speed_pct) {
    const V2Snapshot& v = s.v2;
    if (!s.connected) return {Tone::Muted, false, "○", "연결 안 됨"};
    if (v.estop || s.robot_estop_requested) return {Tone::Danger, true, "✕", "비상정지"};
    if (!v.linked) return {Tone::Danger, true, "✕", "응답 없음"};
    if (v.state == "fault") return {Tone::Danger, true, "✕", "로봇 오류"};
    if (connect_screen) return {Tone::Info, false, "○", "연결 화면 · 운전 안 함"};
    if (!focus) return {Tone::Muted, false, "○", "운전 안 함 · 창 포커스 없음"};
    if (held && v.release_required) return {Tone::Warn, true, "◐", "RT를 놓았다 다시 쥐세요"};
    if (held && v.cmd_active) return {Tone::Ok, true, "●", "주행 중 · " + source + fmt(" · %d %%", speed_pct)};
    if (held) {
        if (!v.accepted && !v.reason.empty() && v.reason != "ok") return {Tone::Warn, true, "◐", std::string("운전 대기 · ") + reason_label(v.reason)};
        return {Tone::Warn, true, "◐", std::string("운전 대기 · ") + state_label(v.state)};
    }
    if (v.state == "awaiting_neutral" || v.state == "timeout") return {Tone::Warn, false, "◐", "중립 확인 중"};
    if (!pad) return {Tone::Info, false, "○", "패드 없음 · 게임패드 모드 확인"};
    return {Tone::Info, false, "○", "대기 · RT를 쥐면 운전"};
}

void App::draw(double now, const RobotSnapshot& s) {
    w::set_scale(s_);
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    float top = 44 * s_, bottom = 28 * s_, rail_l = 80 * s_, rail_r = 128 * s_;
    ImDrawList* bg = ImGui::GetWindowDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), T.bg0);

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::BeginChild("status", ImVec2(W, top), 0, ImGuiWindowFlags_NoScrollbar);
    draw_status_bar(now, s);
    ImGui::EndChild();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * s_, 10 * s_));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(T.bg1));
    ImGui::SetCursorPos(ImVec2(0, top));
    ImGui::BeginChild("left", ImVec2(rail_l, H - top - bottom), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    draw_left_rail(s);
    ImGui::EndChild();
    ImGui::SetCursorPos(ImVec2(W - rail_r, top));
    ImGui::BeginChild("right", ImVec2(rail_r, H - top - bottom), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    draw_right_rail(now, s);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::SetCursorPos(ImVec2(rail_l, top));
    bool scroll = screen_ == Screen::Robot || screen_ == Screen::Log || screen_ == Screen::Connect;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, scroll ? ImVec2(16 * s_, 14 * s_) : ImVec2(0, 0));
    ImGui::BeginChild("center", ImVec2(W - rail_l - rail_r, H - top - bottom), scroll ? ImGuiChildFlags_AlwaysUseWindowPadding : 0,
                      scroll ? 0 : ImGuiWindowFlags_NoScrollbar);
    switch (screen_) {
        case Screen::Drive: draw_drive(now, s); break;
        case Screen::Map: draw_map(now, s); break;
        case Screen::Robot: draw_robot(s); break;
        case Screen::Log: draw_log(now, s); break;
        case Screen::Connect: draw_connect(s); break;
    }
    // 토스트: 가운데 위. 120 ms 올라오며 나타나고 끝 200 ms 동안 사라진다
    if (toast_ != toast_seen_) {
        toast_seen_ = toast_;
        toast_start_ = now;
    }
    if (!toast_.empty() && now < toast_until_) {
        float in = static_cast<float>(std::min(1.0, (now - toast_start_) / 0.12));
        float out = static_cast<float>(std::min(1.0, (toast_until_ - now) / 0.2));
        ImVec2 cp = ImGui::GetWindowPos();
        w::toast(ImGui::GetForegroundDrawList(), ImVec2(cp.x + ImGui::GetWindowWidth() / 2, cp.y + 16 * s_), toast_.c_str(),
                 toast_tone(toast_), std::min(in, out), 8 * (1 - in));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SetCursorPos(ImVec2(0, H - bottom));
    ImGui::BeginChild("events", ImVec2(W, bottom), 0, ImGuiWindowFlags_NoScrollbar);
    draw_events();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

void App::draw_status_bar(double now, const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth(), Ht = ImGui::GetWindowHeight();
    const V2Snapshot& v = s.v2;
    bool estop = v.estop || s.robot_estop_requested;
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + Ht), estop ? T.danger_deep : T.bg1);
    dl->AddLine(ImVec2(o.x, o.y + Ht - 1), ImVec2(o.x + W, o.y + Ht - 1), T.line);
    float h = 28 * s_, y = o.y + (Ht - h) / 2, x = o.x + 12 * s_, gap = 8 * s_;
    // 1) 연결 칩(점 + 주소). 끊긴 직후 1.6 초 깜빡임
    bool linked = s.connected && v.linked;
    Tone ct = !s.connected ? Tone::Muted : (linked ? Tone::Ok : Tone::Danger);
    if (!linked && s.connected && std::fmod(now, 0.8) < 0.4) ct = Tone::Warn;
    std::string host = privacy_ ? std::string("로봇") : s.host;
    x += w::chip(dl, ImVec2(x, y), h, "●", s.connected ? host.c_str() : "연결 안 됨", ct, false) + gap;
    // 2) 주행 상태 캡슐: 상태띠에서 유일하게 채운 요소
    Capsule c = capsule_for(s, held_, focus_, screen_ == Screen::Connect, pad_.state().connected, drive_source_,
                            static_cast<int>(robot_.v2().speed_scale() * 100 + 0.5));
    float cw = std::max(260 * s_, w::text_size(F.bold, sz::button, c.text.c_str()).x + 70 * s_);
    w::drive_capsule(dl, ImVec2(x, o.y + (Ht - 34 * s_) / 2), ImVec2(cw, 34 * s_), c.tone, c.filled, c.lamp, c.text.c_str());
    x += cw + gap;
    if (s.connected) {
        std::string rtt = v.linked ? fmt("RTT %.0f ms · 유실 %.0f%%", v.rtt_avg_ms, v.loss_pct) : "RTT —";
        Tone rt = !v.linked ? Tone::Danger : (v.rtt_avg_ms > 100 || v.loss_pct > 5 ? Tone::Warn : Tone::Neutral);
        x += w::chip(dl, ImVec2(x, y), h, ic::wifi, rtt.c_str(), rt, false, F.num) + gap;
        x += w::chip(dl, ImVec2(x, y), h, "", "ROS", s.rb_connected ? Tone::Ok : Tone::Danger, false) + gap;
        std::string cam = robot_.camera_enabled() ? fmt("%.0f fps", robot_.camera_fps()) : "끔";
        x += w::chip(dl, ImVec2(x, y), h, ic::camera, cam.c_str(), Tone::Neutral, false, F.num) + gap;
        const Json& a = s.arbiter;
        if (a.is_object() && a["action"].is_string() && !a["action"].as_string().empty())
            x += w::chip(dl, ImVec2(x, y), h, ic::robot, ("동작 " + a["action"].as_string()).c_str(), Tone::Warn, false) + gap;
        if (a.is_object() && a["vr_permitted"].is_bool() && !a["vr_permitted"].as_bool())
            x += w::chip(dl, ImVec2(x, y), h, ic::warning, "이동 차단(중재기)", Tone::Warn, false) + gap;
    }
    // 오른쪽 끝: 패드 상태
    const char* pt = pad_.state().connected ? "패드" : "패드 없음";
    float pw = w::chip_width(h, ic::gamepad, pt);
    w::chip(dl, ImVec2(o.x + W - pw - 12 * s_, y), h, ic::gamepad, pt, pad_.state().connected ? Tone::Ok : Tone::Danger, false);
}

void App::draw_left_rail(const RobotSnapshot& s) {
    (void)s;
    float wdt = ImGui::GetContentRegionAvail().x, h = 64 * s_;
    struct Item {
        const char* icon;
        const char* label;
        Screen sc;
    } items[] = {{ic::drive, "주행", Screen::Drive}, {ic::map, "지도", Screen::Map}, {ic::robot, "로봇", Screen::Robot},
                 {ic::log, "로그", Screen::Log}, {ic::settings, "연결", Screen::Connect}};
    for (const auto& it : items) {
        if (w::icon_button(it.label, it.icon, it.label, ImVec2(wdt, h), Tone::Neutral, nullptr, true, screen_ == it.sc, true))
            screen_ = it.sc;
        ImGui::Dummy(ImVec2(0, 2 * s_));
    }
    // 아래: 지금 고른 자세·동작·속도(값만, 라벨은 힌트)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - 3 * 50 * s_ - 8 * s_;
    float x = ImGui::GetWindowPos().x + 8 * s_;
    auto slot = [&](const char* hint, const std::string& value, Pad p, Tone t) {
        dl->AddText(F.body, sz::hint * s_, ImVec2(x, y), T.text3, hint);
        w::pad_glyph(dl, p, ImVec2(x + wdt - 12 * s_, y + 9 * s_), 16);
        ImVec4 clip(x, y, x + wdt, y + 60 * s_);
        dl->AddText(F.semi, sz::label * s_, ImVec2(x, y + 20 * s_), w::tone_color(t), value.c_str(), nullptr, 0.0f, &clip);
        y += 50 * s_;
    };
    const ActionGroup& a = action_groups()[action_index_];
    slot("자세", pose_presets()[pose_index_].label, Pad::DpadUD, Tone::Neutral);
    slot("동작", a.label, Pad::DpadLR, a.caution ? Tone::Warn : Tone::Neutral);
    slot("속도", fmt("%d %%", speed_full_ ? 100 : static_cast<int>(cfg_.max_speed * 100 + 0.5f)), Pad::LS, Tone::Neutral);
}

bool App::estop_button(double now, const RobotSnapshot& s, float wdt, float h) {
    bool engaged = s.v2.estop || s.robot_estop_requested;
    float k = clear_hold_start_ > 0 ? static_cast<float>((now - clear_hold_start_) / 1.0) : -1.0f;
    bool holding = false;
    bool hit = w::estop("estop", ImVec2(wdt, h), engaged, s.connected, k, &holding, now);
    if (!engaged) {
        clear_hold_start_ = 0;
        if (hit) estop_all();
        return hit;
    }
    // 해제는 1 초 길게 눌러야(실수 방지). 다 차면 한 번만 해제하고 손을 뗄 때까지 다시 하지 않는다
    if (holding) {
        if (clear_hold_start_ == 0) clear_hold_start_ = now;
        if (clear_hold_start_ > 0 && now - clear_hold_start_ >= 1.0) {
            robot_.clear_emergency_all();
            clear_hold_start_ = -1;
        }
    } else {
        clear_hold_start_ = 0;
    }
    return false;
}

void App::draw_right_rail(double now, const RobotSnapshot& s) {
    float wdt = ImGui::GetContentRegionAvail().x, bh = 56 * s_;
    auto gap = [&] { ImGui::Dummy(ImVec2(0, 2 * s_)); };
    static const Pad kY = Pad::Y, kA = Pad::A, kX = Pad::X, kRB = Pad::RB;
    if (screen_ == Screen::Drive) {
        const char* layers[] = {"카메라", "겹쳐 보기", "레이더"};
        if (w::icon_button("layer", ic::layers, layers[hud_layer_], ImVec2(wdt, bh), Tone::Neutral, &kY)) hud_layer_ = (hud_layer_ + 1) % 3;
        gap();
        if (w::icon_button("snap", ic::camera, "사진", ImVec2(wdt, bh), Tone::Neutral, cfg_.tilt_enabled ? nullptr : &kRB)) {
            CreateDirectoryA((exe_dir_ + "\\shots").c_str(), nullptr);
            std::string err, name = "cam_" + time_tag() + ".jpg";
            toast_ = robot_.save_snapshot_jpeg(exe_dir_ + "\\shots\\" + name, &err) ? "사진 저장: shots\\" + name : "사진 실패: " + err;
            toast_until_ = now + 3;
        }
        gap();
        if (w::icon_button("home", ic::home, "서기", ImVec2(wdt, bh), Tone::Neutral, &kA, !held_)) robot_.go_home();
    } else if (screen_ == Screen::Map) {
        bool can_send = !preview_m_.empty() && s.v2.state == "idle";
        if (w::icon_button("send", ic::send, "보내기", ImVec2(wdt, bh), can_send ? Tone::Accent : Tone::Neutral, nullptr, can_send)) {
            std::string err;
            if (robot_.send_path(preview_m_, &err)) {
                pad_.rumble(Rumble::Tick);
                stroke_px_.clear();
            } else {
                toast_ = "경로 실패: " + err;
                toast_until_ = now + 3;
            }
        }
        gap();
        if (w::icon_button("pcancel", ic::cancel, "취소", ImVec2(wdt, bh), Tone::Neutral, &kX)) robot_.cancel_path();
        gap();
        if (w::icon_button("erase", ic::erase, "지우기", ImVec2(wdt, bh))) {
            stroke_px_.clear();
            preview_m_.clear();
        }
        gap();
        static const char* zooms[] = {"×1", "×2", "×4"};
        int zi = map_zoom_ == 1 ? 0 : map_zoom_ == 2 ? 1 : 2;
        if (w::segmented("zoom", zooms, 3, &zi, ImVec2(wdt, 44 * s_))) map_zoom_ = zi == 0 ? 1 : zi == 1 ? 2 : 4;
        gap();
        gap();
        if (w::icon_button("savemap", ic::save, "지도 저장", ImVec2(wdt, bh))) robot_.save_map();
        gap();
        if (confirm_reset_map_ && now > confirm_reset_until_) confirm_reset_map_ = false;  // 3 초 뒤 자동 복귀
        if (!confirm_reset_map_) {
            if (w::icon_button("newmap", ic::refresh, "지도 새로", ImVec2(wdt, bh))) {
                confirm_reset_map_ = true;
                confirm_reset_until_ = now + 3.0;
            }
        } else if (w::icon_button("newmap2", ic::warning, "정말 새로?", ImVec2(wdt, bh), Tone::Warn)) {
            robot_.reset_map();
            confirm_reset_map_ = false;
        }
    } else if (screen_ == Screen::Robot) {
        if (w::icon_button("home2", ic::home, "서기", ImVec2(wdt, bh), Tone::Neutral, &kA, !held_)) robot_.go_home();
        gap();
        if (w::icon_button("allstop", ic::error, "전체 정지", ImVec2(wdt, bh))) robot_.emergency_stop_all();
        gap();
        if (w::icon_button("allclear", ic::play, "전체 해제", ImVec2(wdt, bh))) robot_.clear_emergency_all();
    } else if (screen_ == Screen::Log) {
        bool sniff = robot_.packet_capture();
        if (w::icon_button("sniff", ic::record, sniff ? "기록 중" : "패킷 기록", ImVec2(wdt, bh), Tone::Neutral, nullptr, true, sniff)) {
            robot_.set_packet_capture(!sniff);
            cfg_.sniff = !sniff;
            save_settings();
        }
        gap();
        if (w::icon_button("topics", ic::list, "토픽 목록", ImVec2(wdt, bh))) robot_.list_topics();
        gap();
        if (w::icon_button("rttreset", ic::refresh, "RTT 초기화", ImVec2(wdt, bh))) rtt_hist_.clear();
    } else if (screen_ == Screen::Connect) {
        if (w::icon_button("quit", ic::power, "앱 끝내기", ImVec2(wdt, bh))) quit_ = true;
        ImGui::PushFont(F.body, sz::hint);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T.text3));
        ImGui::TextWrapped("패드: View + Menu 1.5초");
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    // 비상정지: 모든 화면에서 오른쪽 아래, 가장 크고 유일하게 채도 높은 요소
    float big = wdt;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 12 * s_, ImGui::GetWindowHeight() - big - 14 * s_));
    estop_button(now, s, wdt, big);
}

std::string App::masked(const std::string& line) const {
    if (!privacy_) return line;
    std::string out = line, host = robot_.host();
    if (!host.empty())
        for (size_t p = out.find(host); p != std::string::npos; p = out.find(host, p + 4)) out.replace(p, host.size(), "로봇");
    return out;
}

void App::draw_events() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth(), Ht = ImGui::GetWindowHeight();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + Ht), T.bg1);
    dl->AddLine(o, ImVec2(o.x + W, o.y), T.line);
    if (held_) events_open_ = false;  // 운전 중에는 접는다
    auto last = robot_.events().tail(events_open_ ? 9 : 1);
    for (auto& l : last) l = masked(l);
    if (!last.empty()) {
        const std::string& l = last.back();
        Tone t = event_tone(l);
        dl->AddCircleFilled(ImVec2(o.x + 16 * s_, o.y + Ht / 2), 4 * s_, w::tone_color(t));
        ImVec4 clip(o.x, o.y, o.x + W - 120 * s_, o.y + Ht);
        dl->AddText(F.body, sz::hint * s_, ImVec2(o.x + 28 * s_, o.y + (Ht - sz::hint * s_) / 2 - 1), T.text2, l.c_str(), nullptr, 0.0f, &clip);
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x + W - 110 * s_, o.y));
    if (ImGui::InvisibleButton("evtoggle", ImVec2(110 * s_, Ht))) events_open_ = !events_open_;
    const char* lbl = events_open_ ? "접기 ▾" : "기록 ▴";
    ImVec2 ts = w::text_size(F.semi, sz::hint, lbl);
    dl->AddText(F.semi, sz::hint * s_, ImVec2(o.x + W - ts.x - 16 * s_, o.y + (Ht - ts.y) / 2), T.text2, lbl);
    if (events_open_) {
        // 위로 펼친 패널(최근 8줄)
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        float ph = 8 * 22 * s_ + 20 * s_, x0 = 80 * s_ + 8 * s_, x1 = W - 128 * s_ - 8 * s_, y1 = o.y - 6 * s_, y0 = y1 - ph;
        w::shadow(fg, ImVec2(x0, y0), ImVec2(x1, y1), 12 * s_);
        fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), theme::alpha(T.bg1, 0.97f), 12 * s_);
        fg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), T.line, 12 * s_, 1.0f);
        float yy = y0 + 10 * s_;
        size_t start = last.size() > 8 ? last.size() - 8 : 0;
        for (size_t i = start; i < last.size(); ++i) {
            fg->AddCircleFilled(ImVec2(x0 + 16 * s_, yy + 10 * s_), 3.5f * s_, w::tone_color(event_tone(last[i])));
            fg->AddText(F.body, sz::hint * s_, ImVec2(x0 + 28 * s_, yy), T.text1, last[i].c_str());
            yy += 22 * s_;
        }
    }
}

void App::draw_radar(const RobotSnapshot& s, float cx, float cy, float radius, float alpha) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double range = 3.0;  // 반지름 = 3 m
    float k = radius / static_cast<float>(range);
    // PIP 외곽(정사각 카드) + 원판
    ImVec2 a(cx - radius - 10 * s_, cy - radius - 10 * s_), b(cx + radius + 10 * s_, cy + radius + 10 * s_);
    w::shadow(dl, a, b, 16 * s_);
    dl->AddRectFilled(a, b, theme::alpha(T.bg0, 0.72f * alpha + 0.1f), 16 * s_);
    dl->AddRect(a, b, T.line, 16 * s_, 1.0f);
    for (int r = 1; r <= 6; ++r)
        dl->AddCircle(ImVec2(cx, cy), r * 0.5f * k, theme::alpha(T.lidar, r % 2 == 0 ? 0.32f : 0.14f), 64, 1.0f);
    dl->AddLine(ImVec2(cx, cy - radius), ImVec2(cx, cy + radius), theme::alpha(T.lidar, 0.15f));
    dl->AddLine(ImVec2(cx - radius, cy), ImVec2(cx + radius, cy), theme::alpha(T.lidar, 0.15f));
    for (int m = 1; m <= 3; ++m)
        w::text_shadowed(dl, F.num, sz::hint, ImVec2(cx + 4 * s_, cy - m * k - 18 * s_), T.text3, fmt("%d m", m).c_str());
    bool stale = !s.scan.valid || now_s() - s.scan.rx_time > 1.0;
    if (s.scan.valid) {
        for (size_t i = 0; i < s.scan.ranges.size(); ++i) {
            float r = s.scan.ranges[i];
            if (!(r > 0.05f) || r > range) continue;
            double ang = s.scan.angle_min + s.scan.angle_increment * i;
            // 로봇 앞(+x) = 위, 왼쪽(+y) = 왼쪽
            float px = cx - static_cast<float>(r * std::sin(ang)) * k;
            float py = cy - static_cast<float>(r * std::cos(ang)) * k;
            ImU32 c = stale ? T.text3 : (r < 0.35f ? T.danger : T.lidar);
            dl->AddCircleFilled(ImVec2(px, py), 2.0f * s_, c);
        }
    }
    // 로봇 몸(0.226 × 0.149 m)과 앞 방향
    float hx = 0.149f / 2 * k, hy = 0.226f / 2 * k;
    dl->AddRect(ImVec2(cx - hx, cy - hy), ImVec2(cx + hx, cy + hy), IM_COL32(255, 255, 255, 230), 3 * s_, 1.5f * s_);
    dl->AddTriangleFilled(ImVec2(cx, cy - hy - 9 * s_), ImVec2(cx - 6 * s_, cy - hy - 1 * s_), ImVec2(cx + 6 * s_, cy - hy - 1 * s_), T.accent);
    w::chip(dl, ImVec2(a.x + 8 * s_, a.y + 8 * s_), 24 * s_, "", stale ? "SCAN 없음" : "LiDAR 3 m", stale ? Tone::Warn : Tone::Neutral, false, F.num);
}

void App::draw_drive(double now, const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), T.bg0);
    double age = cam_tex_.valid() ? now - cam_tex_.rx_time : -1;
    if (hud_layer_ != 2) {
        if (cam_tex_.valid()) {
            float iw = static_cast<float>(cam_tex_.width()), ih = static_cast<float>(cam_tex_.height());
            float k = std::min(W / iw, H / ih);
            ImVec2 a(o.x + (W - iw * k) / 2, o.y + (H - ih * k) / 2), b(a.x + iw * k, a.y + ih * k);
            bool stale = age > 2.0;
            dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(cam_tex_.srv())), a, b, ImVec2(0, 0), ImVec2(1, 1),
                         stale ? IM_COL32(150, 150, 150, 255) : IM_COL32_WHITE);
            if (stale) dl->AddRect(a, b, T.warn, 0, 2 * s_);
        } else {
            w::empty_state(dl, ImVec2(o.x + W / 2, o.y + H / 2), ic::camera,
                           robot_.camera_enabled() ? "카메라 신호 없음" : "카메라 꺼짐",
                           robot_.camera_enabled() ? "연결 화면에서 카메라 영상 받기를 확인하세요" : "연결 화면에서 켤 수 있습니다");
        }
    }
    // 왼쪽 위: 텔레메트리 칩(임계를 넘으면 테두리 색만 바뀜)
    const V2Snapshot& v = s.v2;
    float cx = o.x + 16 * s_, cy = o.y + 16 * s_, ch = 28 * s_;
    if (s.connected) {
        Tone rt = !v.linked ? Tone::Danger : (v.rtt_avg_ms > 100 || v.loss_pct > 5 ? Tone::Warn : Tone::Neutral);
        cx += w::chip(dl, ImVec2(cx, cy), ch, ic::wifi, v.linked ? fmt("RTT %.0f ms", v.rtt_avg_ms).c_str() : "응답 없음", rt, false, F.num) + 8 * s_;
        if (robot_.camera_enabled()) {
            Tone ctn = age < 0 ? Tone::Muted : (age > 1.0 ? Tone::Warn : Tone::Neutral);
            std::string cam = age < 0 ? "CAM —" : fmt("CAM %.0f fps · %.1f s", robot_.camera_fps(), age);
            cx += w::chip(dl, ImVec2(cx, cy), ch, ic::camera, cam.c_str(), ctn, false, F.num) + 8 * s_;
        }
    }
    // 위 가운데: 경로 진행 배너
    if (s.path.valid && (s.path.state == "rotate" || s.path.state == "drive")) {
        std::string t = fmt("경로 %s %d/%d · 남은 %.1f m · 스틱을 쓰면 취소", path_label(s.path.state), s.path.index + 1, s.path.count, s.path.remaining_m);
        float bw = w::chip_width(32 * s_, ic::map, t.c_str());
        w::chip(dl, ImVec2(o.x + (W - bw) / 2, o.y + 56 * s_), 32 * s_, ic::map, t.c_str(), Tone::Info, true);
    }
    if (hud_layer_ == 1) draw_radar(s, o.x + W - 170 * s_, o.y + H - 170 * s_, 150 * s_, 0.9f);
    if (hud_layer_ == 2) draw_radar(s, o.x + W / 2, o.y + H / 2, std::min(W, H) / 2 - 30 * s_, 1.0f);

    // 왼쪽 아래: 명령 막대 2개 + 주행 상태 캡슐
    float bx = o.x + 20 * s_, by = o.y + H - 20 * s_ - 52 * s_;
    float bw = 420 * s_;
    dl->AddRectFilled(ImVec2(bx - 12 * s_, by - 74 * s_), ImVec2(bx + bw + 12 * s_, by - 6 * s_), theme::alpha(T.bg0, 0.55f), 12 * s_);
    w::cmd_bar(dl, ImVec2(bx, by - 66 * s_), bw, "전후", v.cmd_forward, v.cmd_active);
    w::cmd_bar(dl, ImVec2(bx, by - 36 * s_), bw, "회전", -v.cmd_turn, v.cmd_active);  // 화면 오른쪽 = 우회전
    Capsule c = capsule_for(s, held_, focus_, false, pad_.state().connected, drive_source_,
                            static_cast<int>(robot_.v2().speed_scale() * 100 + 0.5));
    w::drive_capsule(dl, ImVec2(bx - 12 * s_, by), ImVec2(bw + 24 * s_, 52 * s_), c.tone, c.filled, c.lamp, c.text.c_str());
}

void App::draw_map(double now, const RobotSnapshot& s) {
    (void)now;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), T.bg0);
    if (!map_tex_.valid() || !s.map.valid) {
        w::empty_state(dl, ImVec2(o.x + W / 2, o.y + H / 2), ic::map, "지도 기다리는 중", "로봇이 조금 움직이면 생깁니다");
        return;
    }
    const MapGeometry& g = s.map.geo;
    float pad = 16 * s_, avail_w = W - 2 * pad, avail_h = H - 2 * pad - 60 * s_;
    float iw = static_cast<float>(map_tex_.width()), ih = static_cast<float>(map_tex_.height());
    float fit = std::min(avail_w / iw, avail_h / ih) * map_zoom_;
    Pt robot_px = g.to_pixel({s.map.robot_x, s.map.robot_y});
    float rx = static_cast<float>(robot_px.first) * iw / g.width_px, ry = static_cast<float>(robot_px.second) * ih / g.height_px;
    float ox = map_zoom_ == 1 ? o.x + (W - iw * fit) / 2 : o.x + W / 2 - rx * fit;
    float oy = map_zoom_ == 1 ? o.y + pad + (avail_h - ih * fit) / 2 + 4 * s_ : o.y + H / 2 - ry * fit;
    auto to_screen = [&](const Pt& m) {
        Pt uv = g.to_pixel(m);
        return ImVec2(ox + static_cast<float>(uv.first) * iw / g.width_px * fit, oy + static_cast<float>(uv.second) * ih / g.height_px * fit);
    };
    auto to_map = [&](ImVec2 p) {
        Pt uv{(p.x - ox) / fit * g.width_px / iw, (p.y - oy) / fit * g.height_px / ih};
        return g.to_map(uv);
    };
    dl->PushClipRect(o, ImVec2(o.x + W, o.y + H), true);
    dl->AddImageRounded(ImTextureRef(reinterpret_cast<ImTextureID>(map_tex_.srv())), ImVec2(ox, oy), ImVec2(ox + iw * fit, oy + ih * fit),
                        ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8 * s_);
    dl->AddRect(ImVec2(ox, oy), ImVec2(ox + iw * fit, oy + ih * fit), T.line, 8 * s_, 1.0f);

    // 로봇이 실행 중인 경로(/api/path): 남은 구간 info, 지난 점 회색, 현재 목표 warn
    if (s.path.valid && !s.path.points.empty() && s.path.state != "idle") {
        ImVec2 prev = to_screen({s.map.robot_x, s.map.robot_y});
        for (size_t i = 0; i < s.path.points.size(); ++i) {
            ImVec2 p = to_screen(s.path.points[i]);
            int idx = static_cast<int>(i);
            if (idx >= s.path.index) dl->AddLine(prev, p, T.info, 3 * s_);
            if (idx == s.path.index) {
                dl->AddCircleFilled(p, 11 * s_, T.warn);
                dl->AddCircle(p, 11 * s_, T.ink, 0, 2 * s_);
            } else {
                dl->AddCircleFilled(p, 7 * s_, idx < s.path.index ? T.text3 : T.info);
            }
            prev = p;
        }
    }
    // 손가락 스케치(강조색)와 단순화한 경유점(번호)
    ImGui::SetCursorScreenPos(o);
    ImGui::InvisibleButton("sketch", ImVec2(W, H));
    bool path_busy = s.path.state == "rotate" || s.path.state == "drive";
    if (ImGui::IsItemActivated() && !path_busy) {
        stroke_px_.clear();
        preview_m_.clear();
    }
    if (ImGui::IsItemActive() && !path_busy) {
        ImVec2 m = ImGui::GetIO().MousePos;
        if (stroke_px_.empty() || std::hypot(m.x - stroke_px_.back().first, m.y - stroke_px_.back().second) > 3 * s_)
            stroke_px_.emplace_back(m.x, m.y);
    }
    if (ImGui::IsItemDeactivated() && !stroke_px_.empty()) {
        std::vector<Pt> m;
        for (const auto& p : stroke_px_) m.push_back(to_map(ImVec2(static_cast<float>(p.first), static_cast<float>(p.second))));
        preview_m_ = sketch_to_path(m);
    }
    if (stroke_px_.size() > 1) {
        std::vector<ImVec2> pts;
        for (const auto& p : stroke_px_) pts.emplace_back(static_cast<float>(p.first), static_cast<float>(p.second));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::alpha(T.accent, 0.85f), 4 * s_, ImDrawFlags_RoundCornersAll);
    }
    for (size_t i = 0; i < preview_m_.size(); ++i) {
        ImVec2 p = to_screen(preview_m_[i]);
        if (i) dl->AddLine(to_screen(preview_m_[i - 1]), p, T.ok, 2 * s_);
    }
    for (size_t i = 0; i < preview_m_.size(); ++i) {
        ImVec2 p = to_screen(preview_m_[i]);
        dl->AddCircleFilled(p, 10 * s_, T.bg0);
        dl->AddCircle(p, 10 * s_, T.ok, 0, 3 * s_);
        std::string n = std::to_string(i + 1);
        ImVec2 ts = w::text_size(F.num, sz::hint, n.c_str());
        dl->AddText(F.num, sz::hint * s_, ImVec2(p.x - ts.x / 2, p.y - ts.y / 2), T.text1, n.c_str());
    }
    dl->PopClipRect();

    // 위 왼쪽: 경고 칩. 아래: 경로 상태 캡슐(보내기 가능 여부와 이유)
    w::chip(dl, ImVec2(o.x + 16 * s_, o.y + 16 * s_), 30 * s_, ic::warning, "로봇은 장애물을 피하지 않습니다", Tone::Warn, false);
    std::string info = s.path.valid ? std::string("경로 ") + path_label(s.path.state) : "경로 —";
    if (s.path.valid && s.path.count)
        info += fmt(" %d/%d · 남은 %.2f m · %.0f s", std::min(s.path.index + 1, s.path.count), s.path.count, s.path.remaining_m, s.path.elapsed_s);
    std::string why;
    if (preview_m_.empty()) why = "손가락으로 선을 그리세요";
    else if (path_busy) why = "경로 실행 중";
    else if (s.v2.state != "idle") why = std::string("보내기 불가 · ") + state_label(s.v2.state);
    else why = fmt("새 경로 %zu점 %.1f m · 보내기 가능", preview_m_.size(), polyline_length(preview_m_));
    std::string line = info + "   |   " + why;
    Tone t = preview_m_.empty() || path_busy ? Tone::Info : (s.v2.state == "idle" ? Tone::Ok : Tone::Warn);
    w::drive_capsule(dl, ImVec2(o.x + 16 * s_, o.y + H - 60 * s_), ImVec2(W - 32 * s_, 44 * s_), t, false, "●", line.c_str());
}

void App::draw_robot(const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
    float gap = 8 * s_;
    auto section = [&](const char* title, const char* hint, Pad p) {
        ImVec2 c = ImGui::GetCursorScreenPos();
        dl->AddText(F.bold, sz::title * s_, c, T.text1, title);
        ImVec2 ts = w::text_size(F.bold, sz::title, title);
        dl->AddText(F.body, sz::label * s_, ImVec2(c.x + ts.x + 14 * s_, c.y + ts.y - sz::label * s_ - 2 * s_), T.text3, hint);
        w::pad_glyph(dl, p, ImVec2(c.x + avail - 16 * s_, c.y + ts.y / 2), 20);
        ImGui::Dummy(ImVec2(avail, ts.y + 8 * s_));
    };
    section("자세", "누르면 바로 바뀝니다", Pad::DpadUD);
    int n = static_cast<int>(pose_presets().size());
    float pw = (avail - gap * (n - 1)) / n, ph = 60 * s_;
    for (int i = 0; i < n; ++i) {
        const PosePreset& p = pose_presets()[i];
        if (i) ImGui::SameLine(0, gap);
        if (w::icon_button(p.name, "", p.label, ImVec2(pw, ph), Tone::Neutral, nullptr, true, pose_index_ == i)) {
            pose_index_ = i;
            robot_.set_pose(p.name);
        }
    }
    ImGui::Dummy(ImVec2(0, 10 * s_));
    section("동작", "⚠ 큰 동작은 두 번 눌러 실행 · 실행 중에는 이동이 막힙니다", Pad::LB);
    double now = now_s();
    int cols = 6, i = 0;
    float aw = (avail - gap * (cols - 1)) / cols, ah = 54 * s_;
    for (const auto& a : action_groups()) {
        if (i % cols) ImGui::SameLine(0, gap);
        bool armed = caution_action_ == a.name && now < caution_armed_until_;
        std::string label = armed ? "다시 눌러 실행" : a.label;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool hit = w::icon_button(a.name, a.caution && !armed ? ic::warning : "", label.c_str(), ImVec2(aw, ah), armed ? Tone::Warn : Tone::Neutral,
                                  nullptr, true, action_index_ == i);
        if (a.caution && !armed) dl->AddRect(p0, ImVec2(p0.x + aw, p0.y + ah), theme::alpha(T.warn, 0.8f), 10 * s_, 1.0f);
        if (armed) {  // 남은 시간 링
            float k = static_cast<float>((caution_armed_until_ - now) / 2.0);
            ImVec2 c(p0.x + aw - 16 * s_, p0.y + ah / 2);
            dl->PathArcTo(c, 9 * s_, -3.14159265f / 2, -3.14159265f / 2 + 2 * 3.14159265f * k, 24);
            dl->PathStroke(T.ink, 3 * s_);
        }
        if (hit) {
            action_index_ = i;
            if (a.caution && !armed) {
                caution_action_ = a.name;
                caution_armed_until_ = now + 2.0;
            } else {
                caution_action_.clear();
                robot_.run_action(a.name);
                pad_.rumble(Rumble::Tick);
            }
        }
        ++i;
    }
    ImGui::Dummy(ImVec2(0, 10 * s_));
    ImGui::PushFont(F.semi, sz::body);
    if (ImGui::CollapsingHeader("직접 자세 만들기")) {
        ImGui::PopFont();
        ImGui::PushItemWidth(avail * 0.5f);
        ImGui::SliderFloat("roll (°)", &pose_roll_, -20, 20, "%.0f");
        ImGui::SliderFloat("pitch (°)", &pose_pitch_, -30, 20, "%.0f");
        ImGui::SliderFloat("yaw (°)", &pose_yaw_, -20, 20, "%.0f");
        ImGui::SliderFloat("height (cm)", &pose_height_, -12, -5, "%.1f");
        ImGui::SliderFloat("x_shift (cm)", &pose_xshift_, -3, 3, "%.1f");
        ImGui::SliderFloat("stance_x (cm)", &pose_sx_, -2, 2, "%.1f");
        ImGui::SliderFloat("stance_y (cm)", &pose_sy_, -2, 2, "%.1f");
        ImGui::SliderInt("run_time (ms)", &pose_time_, 200, 2000);
        ImGui::PopItemWidth();
        if (w::icon_button("applypose", ic::play, "이 자세로", ImVec2(200 * s_, 52 * s_), Tone::Accent))
            robot_.set_pose_custom(pose_roll_ * kDeg, pose_pitch_ * kDeg, pose_yaw_ * kDeg, pose_height_, pose_xshift_, pose_sx_, pose_sy_, pose_time_);
        ImGui::SameLine(0, gap);
        if (w::icon_button("resetpose", ic::refresh, "서기 값으로", ImVec2(200 * s_, 52 * s_))) {
            pose_roll_ = pose_pitch_ = pose_yaw_ = 0;
            pose_height_ = -10;
            pose_xshift_ = -0.5f;
            pose_sx_ = pose_sy_ = 0;
        }
    } else {
        ImGui::PopFont();
    }
    // 상태 한 줄 요약(원문은 로그 화면)
    const Json& a = s.arbiter;
    std::string st = fmt("조종 %s", state_label(s.v2.state));
    if (a.is_object()) {
        std::string act = a["action"].is_string() ? a["action"].as_string() : "";
        st += " · 동작 " + (act.empty() ? std::string("없음") : act) + " · 엔진 " + a["engine"].text();
    }
    ImGui::PushFont(F.body, sz::label);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T.text2));
    ImGui::TextUnformatted(st.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void App::draw_log(double now, const RobotSnapshot& s) {
    const V2Snapshot& v = s.v2;
    float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize, gap = 10 * s_;
    float tw = (avail - 3 * gap) / 4, th = 104 * s_;
    std::vector<float> sorted = rtt_hist_;
    std::sort(sorted.begin(), sorted.end());
    float med = sorted.empty() ? -1 : sorted[sorted.size() / 2];
    float p95 = sorted.empty() ? -1 : sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)];
    std::string big = med < 0 ? "—" : fmt("%.1f ms", med), small = p95 < 0 ? "표본 없음" : fmt("p95 %.1f · n %zu", p95, sorted.size());
    w::stat_tile("t1", "ACK RTT", big.c_str(), small.c_str(), ImVec2(tw, th), med > 100 ? Tone::Warn : Tone::Neutral);
    ImGui::SameLine(0, gap);
    w::stat_tile("t2", "유실", fmt("%.1f %%", v.loss_pct).c_str(), fmt("보냄 %llu · 받음 %llu", (unsigned long long)v.tx, (unsigned long long)v.rx).c_str(),
                 ImVec2(tw, th), v.loss_pct > 5 ? Tone::Warn : Tone::Neutral);
    ImGui::SameLine(0, gap);
    double age = cam_tex_.valid() ? (now - cam_tex_.rx_time) * 1000 : -1;
    w::stat_tile("t3", "카메라", fmt("%.1f fps", robot_.camera_fps()).c_str(),
                 (age < 0 ? std::string("프레임 없음") : fmt("나이 %.0f ms · 풀기 %.1f ms", age, cam_dec_.decode_ms())).c_str(), ImVec2(tw, th));
    ImGui::SameLine(0, gap);
    ImGuiIO& io = ImGui::GetIO();
    w::stat_tile("t4", "화면", fmt("%.0f fps", io.Framerate).c_str(), fmt("%.1f ms/프레임", 1000.0f / std::max(1.0f, io.Framerate)).c_str(), ImVec2(tw, th));
    ImGui::Dummy(ImVec2(0, 4 * s_));
    float vmax = std::max(20.0f, sorted.empty() ? 20.0f : sorted.back() * 1.2f);
    ImVec2 pp = ImGui::GetCursorScreenPos();
    w::area_plot("rtt", rtt_hist_.data(), static_cast<int>(rtt_hist_.size()), vmax, 100.0f, ImVec2(avail, 150 * s_));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(F.semi, sz::label * s_, ImVec2(pp.x + 14 * s_, pp.y + 10 * s_), T.text2, "ACK RTT (ms) · 점선 100 ms");
    if (!sorted.empty()) {
        std::string sm = fmt("중앙 %.1f · p95 %.1f · 최대 %.1f", med, p95, sorted.back());
        ImVec2 ts = w::text_size(F.num, sz::hint, sm.c_str());
        dl->AddText(F.num, sz::hint * s_, ImVec2(pp.x + avail - ts.x - 14 * s_, pp.y + 11 * s_), T.text2, sm.c_str());
    }
    ImGui::Dummy(ImVec2(0, 4 * s_));
    ImGui::PushFont(F.body, sz::label);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T.text2));
    ImGui::Text("조종 %s · client %s · seq %lld", privacy_ ? "로봇:5001" : v.robot.c_str(), v.client_id.substr(0, 8).c_str(), static_cast<long long>(v.sequence));
    ImGui::Text("rosbridge %s · 지도 %s · 스캔 %zu빔", s.rb_status.c_str(), s.map.valid ? "있음" : "없음", s.scan.ranges.size());
    if (s.arbiter.is_object()) {
        const Json& a = s.arbiter;
        ImGui::Text("중재기 owner %s · estop %s · engine %s · 동작 %s", a["owner"].is_null() ? "—" : a["owner"].text().c_str(),
                    a["estop"].as_bool() ? "ON" : "off", a["engine"].text().c_str(), a["action"].is_null() ? "—" : a["action"].text().c_str());
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushFont(F.num, sz::hint);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(T.bg1));
    ImGui::BeginChild("evlog", ImVec2(avail, robot_.packet_capture() ? 200 * s_ : 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    for (const auto& raw : robot_.events().tail(200)) {
        std::string l = masked(raw);
        ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + 5 * s_, c.y + 9 * s_), 3.5f * s_, w::tone_color(event_tone(l)));
        ImGui::SetCursorScreenPos(ImVec2(c.x + 16 * s_, c.y));
        ImGui::TextUnformatted(l.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    if (robot_.packet_capture()) {
        ImGui::BeginChild("pktlog", ImVec2(avail, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        for (const auto& l : robot_.packets().tail(200)) ImGui::TextUnformatted(l.c_str());
        ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void App::draw_connect(const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float avail = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize, gap = 16 * s_;
    float cw = (avail - gap) / 2, chh = 430 * s_;
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    w::card(dl, c0, ImVec2(c0.x + cw, c0.y + chh));
    w::card(dl, ImVec2(c0.x + cw + gap, c0.y), ImVec2(c0.x + 2 * cw + gap, c0.y + chh));
    // 왼쪽 카드: 로봇 찾기
    ImGui::SetCursorScreenPos(ImVec2(c0.x + 18 * s_, c0.y + 16 * s_));
    ImGui::BeginGroup();
    dl->AddText(F.bold, sz::title * s_, ImGui::GetCursorScreenPos(), T.text1, "로봇 찾기");
    ImGui::Dummy(ImVec2(0, 34 * s_));
    bool busy = discovering_.valid() && discovering_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (discovering_.valid() && !busy) {
        found_ = discovering_.get();
        discovered_once_ = true;
    }
    float iw = cw - 36 * s_;
    if (w::icon_button("discover", ic::search, busy ? "찾는 중…" : "같은 Wi-Fi 에서 찾기", ImVec2(iw, 56 * s_), Tone::Neutral, nullptr, !busy))
        discovering_ = std::async(std::launch::async, [] { return discover_robots(1500); });
    ImGui::Dummy(ImVec2(0, 4 * s_));
    for (const auto& f : found_) {
        std::string label = f.name + "  " + f.ip;
        if (w::icon_button(("r" + f.id).c_str(), ic::wifi, label.c_str(), ImVec2(iw, 56 * s_), Tone::Ok)) connect_to(f.ip);
    }
    ImGui::PushFont(F.body, sz::label);
    if (discovered_once_ && found_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T.warn));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + iw);
        ImGui::TextWrapped("응답한 로봇이 없습니다. 로봇과 흑갈리가 같은 Wi-Fi 인지 확인하세요.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    std::string mine;
    for (const auto& ip : local_ipv4_addresses()) mine += (privacy_ ? std::string("x.x.x.x") : ip) + "  ";
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T.text3));
    ImGui::Text("이 기기: %s", mine.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndGroup();
    // 오른쪽 카드: 주소 직접 입력(숫자 패드 — 흑갈리에는 실물 키보드가 없다)
    float kx = c0.x + cw + gap + 18 * s_;
    ImGui::SetCursorScreenPos(ImVec2(kx, c0.y + 16 * s_));
    ImGui::BeginGroup();
    dl->AddText(F.bold, sz::title * s_, ImGui::GetCursorScreenPos(), T.text1, "주소 직접 입력");
    ImGui::Dummy(ImVec2(0, 34 * s_));
    ImVec2 ip0 = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ip0, ImVec2(ip0.x + iw, ip0.y + 48 * s_), T.bg0, 10 * s_);
    dl->AddRect(ip0, ImVec2(ip0.x + iw, ip0.y + 48 * s_), T.line, 10 * s_, 1.0f);
    std::string shown = (privacy_ && !ip_entry_.empty() ? std::string("192.168.x.x") : ip_entry_) + ((static_cast<int>(now_s() * 2) % 2) ? "_" : " ");
    dl->AddText(F.num, sz::num_lg * s_, ImVec2(ip0.x + 14 * s_, ip0.y + 7 * s_), T.text1, shown.c_str());
    ImGui::Dummy(ImVec2(iw, 56 * s_));
    const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "←"};
    float kw = (iw - 2 * 8 * s_) / 3, kh = 48 * s_;
    for (int i = 0; i < 12; ++i) {
        if (i % 3) ImGui::SameLine(0, 8 * s_);
        if (w::icon_button((std::string("k") + std::to_string(i)).c_str(), "", keys[i], ImVec2(kw, kh))) {
            if (i == 11) {
                if (!ip_entry_.empty()) ip_entry_.pop_back();
            } else if (ip_entry_.size() < 15) {
                ip_entry_ += keys[i];
            }
        }
    }
    if (w::icon_button("ipclear", ic::erase, "지우기", ImVec2(kw, kh))) ip_entry_.clear();
    ImGui::SameLine(0, 8 * s_);
    if (w::icon_button("ipconnect", ic::wifi, "연결", ImVec2(kw, kh), Tone::Accent, nullptr, !ip_entry_.empty())) connect_to(ip_entry_);
    ImGui::SameLine(0, 8 * s_);
    if (w::icon_button("ipdisc", ic::cancel, "끊기", ImVec2(kw, kh), Tone::Neutral, nullptr, s.connected)) robot_.disconnect();
    ImGui::EndGroup();

    // 아래 카드: 설정
    float sy = c0.y + chh + gap, shh = 330 * s_;
    w::card(dl, ImVec2(c0.x, sy), ImVec2(c0.x + avail, sy + shh));
    ImGui::SetCursorScreenPos(ImVec2(c0.x + 18 * s_, sy + 16 * s_));
    ImGui::BeginGroup();
    dl->AddText(F.bold, sz::title * s_, ImGui::GetCursorScreenPos(), T.text1, "설정");
    ImGui::Dummy(ImVec2(0, 34 * s_));
    bool changed = false;
    changed |= w::toggle("t_single", "왼쪽 스틱 하나로 운전(전후 + 회전)", &cfg_.single_stick);
    changed |= w::toggle("t_tilt", "기울여서 운전(RB 를 쥔 동안, 쥐는 순간 자세가 0)", &cfg_.tilt_enabled);
    if (cfg_.tilt_enabled) {
        ImGui::Indent(70 * s_);
        changed |= w::toggle("t_tf", "앞뒤 반대로", &cfg_.tilt_invert_forward);
        ImGui::SameLine(0, 30 * s_);
        changed |= w::toggle("t_tt", "좌우 반대로", &cfg_.tilt_invert_turn);
        ImGui::SameLine(0, 30 * s_);
        ImGui::TextDisabled("가속도계: %s", tilt_.status().c_str());
        tilt_drive_.invert_forward = cfg_.tilt_invert_forward;
        tilt_drive_.invert_turn = cfg_.tilt_invert_turn;
        ImGui::Unindent(70 * s_);
    }
    changed |= w::toggle("t_cam", "카메라 영상 받기(Wi-Fi 15~40 Mbps)", &cfg_.camera);
    if (cfg_.camera != robot_.camera_enabled()) robot_.set_camera_enabled(cfg_.camera);
    ImGui::PushItemWidth(avail * 0.45f);
    if (ImGui::SliderFloat("최고 속도 비율", &cfg_.max_speed, 0.1f, 1.0f, "%.2f")) {
        speed_full_ = false;
        robot_.v2().set_speed_scale(cfg_.max_speed);
        changed = true;
    }
    if (ImGui::SliderFloat("스틱 데드존", &cfg_.deadzone, 0.05f, 0.4f, "%.2f")) {
        pad_.deadzone = cfg_.deadzone;
        changed = true;
    }
    ImGui::PopItemWidth();
    ImGui::PushFont(F.body, sz::hint);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T.text3));
    ImGui::Text("1.00 = 로봇 상한(전진 25 cm/s, 후진 5 cm/s, 회전 0.28 rad/s) · 글꼴 %s · Dear ImGui %s", F.body_name.c_str(), IMGUI_VERSION);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndGroup();
    if (changed) save_settings();
    ImGui::SetCursorScreenPos(ImVec2(c0.x, sy + shh + gap));
    ImGui::Dummy(ImVec2(avail, 1));
}

}  // namespace hg
