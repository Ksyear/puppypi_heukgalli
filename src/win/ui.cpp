#include "ui.hpp"  // sock.hpp(winsock2) 가 windows.h 보다 먼저 들어온다

#include "catalog.hpp"
#include "imgui.h"
#include "util.hpp"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

namespace hg {

namespace {
constexpr unsigned kRed = IM_COL32(220, 40, 40, 255);
constexpr unsigned kRedDark = IM_COL32(120, 20, 20, 255);
constexpr unsigned kGreen = IM_COL32(60, 200, 110, 255);
constexpr unsigned kYellow = IM_COL32(250, 200, 40, 255);
constexpr unsigned kBlue = IM_COL32(80, 160, 255, 255);
constexpr unsigned kGrey = IM_COL32(140, 140, 150, 255);
constexpr unsigned kPanel = IM_COL32(22, 24, 30, 255);
constexpr double kPi = 3.141592653589793;

ImVec4 col4(unsigned c) { return ImGui::ColorConvertU32ToFloat4(c); }

unsigned state_color(const std::string& st, bool linked) {
    if (!linked) return kGrey;
    if (st == "active") return kGreen;
    if (st == "idle") return kBlue;
    if (st == "emergency_stop" || st == "fault") return kRed;
    return kYellow;  // awaiting_neutral, timeout
}

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
    // 패드가 없을 때 개발·시연용 키보드: W/S/A/D 를 누르는 동안만(실제 키 상태)
    if (!p.connected && focus_ && !ImGui::GetIO().WantTextInput) {
        auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        double kf = (key('W') ? 1 : 0) - (key('S') ? 1 : 0);
        double kt = (key('A') ? 1 : 0) - (key('D') ? 1 : 0);
        if (kf != 0 || kt != 0) {
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

// ---------------- 화면 ----------------

bool App::big_button(const char* label, float w, float h, unsigned color) {
    if (color) {
        ImGui::PushStyleColor(ImGuiCol_Button, col4(color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(color));
    }
    bool r = ImGui::Button(label, ImVec2(w, h));
    if (color) ImGui::PopStyleColor(2);
    return r;
}

void App::draw(double now, const RobotSnapshot& s) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    float top = 64 * s_, bottom = 96 * s_, rail = 168 * s_;

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::BeginChild("status", ImVec2(W, top), 0, ImGuiWindowFlags_NoScrollbar);
    draw_status_bar(now, s);
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(0, top));
    ImGui::BeginChild("left", ImVec2(rail, H - top - bottom), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    draw_left_rail(s);
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(rail, top));
    ImGui::BeginChild("center", ImVec2(W - 2 * rail, H - top - bottom), 0,
                      screen_ == Screen::Drive || screen_ == Screen::Map ? ImGuiWindowFlags_NoScrollbar : 0);
    switch (screen_) {
        case Screen::Drive: draw_drive(now, s); break;
        case Screen::Map: draw_map(now, s); break;
        case Screen::Robot: draw_robot(s); break;
        case Screen::Log: draw_log(now, s); break;
        case Screen::Connect: draw_connect(s); break;
    }
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(W - rail, top));
    ImGui::BeginChild("right", ImVec2(rail, H - top - bottom), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    draw_right_rail(now, s);
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(0, H - bottom));
    ImGui::BeginChild("events", ImVec2(W, bottom), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    draw_events();
    ImGui::EndChild();
    ImGui::End();
}

void App::draw_status_bar(double now, const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth(), Ht = ImGui::GetWindowHeight();
    const V2Snapshot& v = s.v2;
    bool estop = v.estop || s.robot_estop_requested;
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + Ht), estop ? kRedDark : kPanel);
    float x = o.x + 14 * s_, cy = o.y + Ht / 2;
    auto chip = [&](const std::string& text, unsigned color) {
        ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        float pad = 10 * s_;
        dl->AddRectFilled(ImVec2(x, cy - ts.y / 2 - 6 * s_), ImVec2(x + ts.x + 2 * pad, cy + ts.y / 2 + 6 * s_), color, 8 * s_);
        dl->AddText(ImVec2(x + pad, cy - ts.y / 2), IM_COL32(10, 10, 12, 255), text.c_str());
        x += ts.x + 2 * pad + 10 * s_;
    };
    auto text = [&](const std::string& t, unsigned color = IM_COL32(230, 230, 235, 255)) {
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        dl->AddText(ImVec2(x, cy - ts.y / 2), color, t.c_str());
        x += ts.x + 18 * s_;
    };
    text("흑갈리 · PuppyPi", kYellow);
    text(pad_.state().connected ? "패드" : "패드 없음", pad_.state().connected ? kGreen : kRed);
    if (!s.connected) {
        text("연결 안 됨 — Menu(≡) 또는 왼쪽 '연결'", kGrey);
        return;
    }
    text(s.host);
    chip(state_label(v.state), state_color(v.state, v.linked));
    if (v.linked && !v.accepted && !v.reason.empty() && v.reason != "ok") chip(reason_label(v.reason), kYellow);
    if (estop) chip("비상정지 ON", kRed);
    text(v.linked ? fmt("RTT %.0f ms · 유실 %.0f%%", v.rtt_avg_ms, v.loss_pct) : "조종 응답 없음", v.linked ? kGreen : kRed);
    text(s.rb_connected ? "ROS 연결" : "ROS 끊김", s.rb_connected ? kGreen : kRed);
    text(fmt("카메라 %.0f fps", robot_.camera_fps()));
    // 중재기: 동작 그룹 실행 중이거나 다른 경로가 이동권을 가진 경우
    const Json& a = s.arbiter;
    if (a.is_object()) {
        std::string act = a["action"].text();
        if (a["action"].is_string() && !act.empty()) chip("동작 실행 중: " + act, kYellow);
        if (a["vr_permitted"].is_bool() && !a["vr_permitted"].as_bool()) chip("이동 차단(중재기)", kYellow);
    }
    if (now < toast_until_ && !toast_.empty()) {
        ImVec2 ts = ImGui::CalcTextSize(toast_.c_str());
        dl->AddText(ImVec2(o.x + W - ts.x - 16 * s_, cy - ts.y / 2), IM_COL32(255, 255, 255, 255), toast_.c_str());
    }
}

void App::draw_left_rail(const RobotSnapshot& s) {
    float w = ImGui::GetContentRegionAvail().x;
    float h = 56 * s_;
    struct Item {
        const char* label;
        Screen sc;
    } items[] = {{"주행", Screen::Drive}, {"지도·경로", Screen::Map}, {"로봇", Screen::Robot}, {"로그", Screen::Log}, {"연결·설정", Screen::Connect}};
    for (const auto& it : items) {
        bool cur = screen_ == it.sc;
        if (big_button(it.label, w, h, cur ? IM_COL32(60, 90, 160, 255) : 0)) screen_ = it.sc;
    }
    ImGui::Separator();
    ImGui::TextDisabled("자세 (D-pad ▲▼)");
    ImGui::TextWrapped("%s", pose_presets()[pose_index_].label);
    ImGui::TextDisabled("동작 (◀▶, LB 실행)");
    const ActionGroup& a = action_groups()[action_index_];
    ImGui::TextColored(a.caution ? col4(kYellow) : ImVec4(1, 1, 1, 1), "%s", a.label);
    ImGui::TextDisabled("속도");
    ImGui::Text("%d %%%s", speed_full_ ? 100 : int(cfg_.max_speed * 100 + 0.5f), speed_full_ ? " (LS)" : "");
    if (cfg_.tilt_enabled) ImGui::Text("기울기: %s", tilt_.status().c_str());
}

bool App::estop_button(double now, const RobotSnapshot& s, float w, float h) {
    bool estop = s.v2.estop || s.robot_estop_requested;
    if (!estop) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
        bool hit = big_button("비상정지\n(B)", w, h, kRed);
        ImGui::PopFont();
        if (hit) estop_all();
        return hit;
    }
    // 해제는 1 초 길게 눌러야(실수 방지)
    big_button("해제: 1초 누르기", w, h, IM_COL32(200, 120, 30, 255));
    if (ImGui::IsItemActive()) {
        if (clear_hold_start_ == 0) clear_hold_start_ = now;
        float k = static_cast<float>(std::min(1.0, (now - clear_hold_start_) / 1.0));
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x, b.y - 10 * s_), ImVec2(a.x + (b.x - a.x) * k, b.y), kGreen);
        if (k >= 1.0f) {
            robot_.clear_emergency_all();
            clear_hold_start_ = -1;  // 손을 뗄 때까지 다시 안 함
        }
    } else {
        clear_hold_start_ = 0;
    }
    return false;
}

void App::draw_right_rail(double now, const RobotSnapshot& s) {
    float w = ImGui::GetContentRegionAvail().x;
    float bh = 50 * s_;
    if (screen_ == Screen::Drive) {
        const char* layers[] = {"화면: 카메라", "화면: +레이더", "화면: 레이더"};
        if (big_button(layers[hud_layer_], w, bh)) hud_layer_ = (hud_layer_ + 1) % 3;
        if (big_button("사진 (RB)", w, bh)) {
            CreateDirectoryA((exe_dir_ + "\\shots").c_str(), nullptr);
            std::string err, name = "cam_" + time_tag() + ".jpg";
            toast_ = robot_.save_snapshot_jpeg(exe_dir_ + "\\shots\\" + name, &err) ? "사진 저장: shots\\" + name : "사진 실패: " + err;
            toast_until_ = now + 3;
        }
        if (big_button("서기 (A)", w, bh)) robot_.go_home();
    } else if (screen_ == Screen::Map) {
        bool can_send = !preview_m_.empty() && s.v2.state == "idle";
        ImGui::BeginDisabled(!can_send);
        if (big_button("경로 보내기", w, bh, can_send ? IM_COL32(40, 140, 80, 255) : 0)) {
            std::string err;
            if (robot_.send_path(preview_m_, &err)) {
                pad_.rumble(Rumble::Tick);
                stroke_px_.clear();
            } else {
                toast_ = "경로 실패: " + err;
                toast_until_ = now + 3;
            }
        }
        ImGui::EndDisabled();
        if (big_button("경로 취소 (X)", w, bh)) robot_.cancel_path();
        if (big_button("지우기", w, bh)) {
            stroke_px_.clear();
            preview_m_.clear();
        }
        if (big_button(map_zoom_ == 1 ? "확대 ×1" : map_zoom_ == 2 ? "확대 ×2" : "확대 ×4", w, bh)) map_zoom_ = map_zoom_ == 4 ? 1 : map_zoom_ * 2;
        if (big_button("지도 저장", w, bh)) robot_.save_map();
        if (!confirm_reset_map_) {
            if (big_button("지도 새로", w, bh)) confirm_reset_map_ = true;
        } else {
            if (big_button("정말 새로?", w, bh, kYellow)) {
                robot_.reset_map();
                confirm_reset_map_ = false;
            }
        }
    } else if (screen_ == Screen::Log) {
        bool sniff = robot_.packet_capture();
        if (big_button(sniff ? "패킷 기록 끄기" : "패킷 기록 켜기", w, bh)) {
            robot_.set_packet_capture(!sniff);
            cfg_.sniff = !sniff;
            save_settings();
        }
        if (big_button("토픽 목록", w, bh)) robot_.list_topics();
        if (big_button("RTT 초기화", w, bh)) rtt_hist_.clear();
    }
    // 비상정지는 어느 화면에서나 오른쪽 아래
    float big = std::min(w, 150 * s_);
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - big - 16 * s_));
    estop_button(now, s, w, big);
}

void App::draw_events() {
    for (const auto& l : robot_.events().tail(3)) ImGui::TextUnformatted(l.c_str());
}

void App::draw_radar(const RobotSnapshot& s, float cx, float cy, float radius, float alpha) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    unsigned a = static_cast<unsigned>(alpha * 255);
    double range = 3.0;  // 반지름 = 3 m
    float k = radius / static_cast<float>(range);
    dl->AddCircleFilled(ImVec2(cx, cy), radius, IM_COL32(0, 20, 10, a * 2 / 3));
    for (int r = 1; r <= 6; ++r) dl->AddCircle(ImVec2(cx, cy), r * 0.5f * k, IM_COL32(40, 160, 80, a / 2), 64, 1.0f);
    dl->AddLine(ImVec2(cx, cy - radius), ImVec2(cx, cy + radius), IM_COL32(40, 160, 80, a / 3));
    dl->AddLine(ImVec2(cx - radius, cy), ImVec2(cx + radius, cy), IM_COL32(40, 160, 80, a / 3));
    if (s.scan.valid) {
        bool stale = now_s() - s.scan.rx_time > 1.0;
        for (size_t i = 0; i < s.scan.ranges.size(); ++i) {
            float r = s.scan.ranges[i];
            if (!(r > 0.05f) || r > range) continue;
            double ang = s.scan.angle_min + s.scan.angle_increment * i;
            // 로봇 앞(+x) = 화면 위, 왼쪽(+y) = 화면 왼쪽
            float px = cx - static_cast<float>(r * std::sin(ang)) * k;
            float py = cy - static_cast<float>(r * std::cos(ang)) * k;
            unsigned c = stale ? IM_COL32(150, 150, 150, a) : (r < 0.35f ? IM_COL32(255, 70, 50, a) : IM_COL32(90, 255, 140, a));
            dl->AddCircleFilled(ImVec2(px, py), 2.2f * s_, c);
        }
    }
    // 로봇 몸(0.226 × 0.149 m)과 앞 방향
    float hx = 0.149f / 2 * k, hy = 0.226f / 2 * k;
    dl->AddRect(ImVec2(cx - hx, cy - hy), ImVec2(cx + hx, cy + hy), IM_COL32(255, 220, 60, a), 2.0f, ImDrawFlags_None, 2.0f);
    dl->AddTriangleFilled(ImVec2(cx, cy - hy - 8 * s_), ImVec2(cx - 6 * s_, cy - hy), ImVec2(cx + 6 * s_, cy - hy), IM_COL32(255, 220, 60, a));
    dl->AddText(ImVec2(cx + 4, cy - radius + 2), IM_COL32(120, 220, 150, a), "LiDAR 3 m");
}

void App::draw_drive(double now, const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), IM_COL32(5, 6, 8, 255));
    if (hud_layer_ != 2) {
        if (cam_tex_.valid()) {
            float iw = static_cast<float>(cam_tex_.width()), ih = static_cast<float>(cam_tex_.height());
            float k = std::min(W / iw, H / ih);
            ImVec2 a(o.x + (W - iw * k) / 2, o.y + (H - ih * k) / 2), b(a.x + iw * k, a.y + ih * k);
            bool stale = now - cam_tex_.rx_time > 2.0;
            dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(cam_tex_.srv())), a, b, ImVec2(0, 0), ImVec2(1, 1),
                         stale ? IM_COL32(110, 110, 110, 255) : IM_COL32_WHITE);
            if (stale) dl->AddText(ImVec2(a.x + 20 * s_, a.y + 20 * s_), kYellow, fmt("영상 %.0f초 전", now - cam_tex_.rx_time).c_str());
        } else {
            const char* t = robot_.camera_enabled() ? "카메라 신호 없음" : "카메라 꺼짐(설정)";
            ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(o.x + (W - ts.x) / 2, o.y + H / 2), kGrey, t);
        }
    }
    if (hud_layer_ == 1) draw_radar(s, o.x + W - 190 * s_, o.y + H - 190 * s_, 170 * s_, 0.85f);
    if (hud_layer_ == 2) draw_radar(s, o.x + W / 2, o.y + H / 2, std::min(W, H) / 2 - 20 * s_, 1.0f);

    // 아래: 명령 막대와 데드맨 표시
    const V2Snapshot& v = s.v2;
    float bx = o.x + 24 * s_, by = o.y + H - 120 * s_;
    dl->AddRectFilled(ImVec2(bx - 10 * s_, by - 10 * s_), ImVec2(bx + 420 * s_, by + 100 * s_), IM_COL32(0, 0, 0, 150), 10 * s_);
    auto bar = [&](float y, const char* name, double val) {
        dl->AddText(ImVec2(bx, y), IM_COL32_WHITE, name);
        float x0 = bx + 70 * s_, w = 300 * s_, mid = x0 + w / 2;
        dl->AddRectFilled(ImVec2(x0, y + 4 * s_), ImVec2(x0 + w, y + 22 * s_), IM_COL32(60, 60, 70, 255), 4);
        float e = mid + static_cast<float>(val) * w / 2;
        dl->AddRectFilled(ImVec2(std::min(mid, e), y + 4 * s_), ImVec2(std::max(mid, e), y + 22 * s_), v.cmd_active ? kGreen : kGrey, 4);
        dl->AddLine(ImVec2(mid, y), ImVec2(mid, y + 26 * s_), IM_COL32_WHITE);
    };
    bar(by, "전후", v.cmd_forward);
    bar(by + 32 * s_, "회전", -v.cmd_turn);  // 화면에서는 오른쪽 = 우회전
    std::string dm = held_ ? "● 데드맨 쥠 — " + drive_source_ : "○ RT 를 쥐면 운전";
    if (held_ && v.release_required) dm = "해제 뒤에는 한 번 놓았다 다시 쥐세요";
    else if (held_ && !v.cmd_active && v.linked) dm = std::string("운전 대기: ") + state_label(v.state);
    dl->AddText(ImVec2(bx, by + 64 * s_), held_ ? (v.cmd_active ? kGreen : kYellow) : kGrey, dm.c_str());
    if (s.path.valid && (s.path.state == "rotate" || s.path.state == "drive"))
        dl->AddText(ImVec2(bx, by - 40 * s_), kYellow,
                    fmt("경로 %s %d/%d · 남은 %.2f m (스틱을 쓰면 취소)", path_label(s.path.state), s.path.index + 1, s.path.count, s.path.remaining_m).c_str());
}

void App::draw_map(double now, const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + H), IM_COL32(40, 40, 46, 255));
    if (!map_tex_.valid() || !s.map.valid) {
        const char* t = "지도 기다리는 중(로봇이 조금 움직이면 생깁니다)";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(o.x + (W - ts.x) / 2, o.y + H / 2), kGrey, t);
        return;
    }
    const MapGeometry& g = s.map.geo;
    // 지도 이미지 픽셀 → 화면. 이미지 크기가 /api/map 과 다르면 비율로 맞춘다
    float iw = static_cast<float>(map_tex_.width()), ih = static_cast<float>(map_tex_.height());
    float fit = std::min(W / iw, H / ih) * map_zoom_;
    Pt robot_px = g.to_pixel({s.map.robot_x, s.map.robot_y});
    float rx = static_cast<float>(robot_px.first) * iw / g.width_px, ry = static_cast<float>(robot_px.second) * ih / g.height_px;
    // 확대하면 로봇을 화면 가운데에
    float ox = map_zoom_ == 1 ? o.x + (W - iw * fit) / 2 : o.x + W / 2 - rx * fit;
    float oy = map_zoom_ == 1 ? o.y + (H - ih * fit) / 2 : o.y + H / 2 - ry * fit;
    auto to_screen = [&](const Pt& m) {
        Pt uv = g.to_pixel(m);
        return ImVec2(ox + static_cast<float>(uv.first) * iw / g.width_px * fit, oy + static_cast<float>(uv.second) * ih / g.height_px * fit);
    };
    auto to_map = [&](ImVec2 p) {
        Pt uv{(p.x - ox) / fit * g.width_px / iw, (p.y - oy) / fit * g.height_px / ih};
        return g.to_map(uv);
    };
    dl->PushClipRect(o, ImVec2(o.x + W, o.y + H), true);
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(map_tex_.srv())), ImVec2(ox, oy), ImVec2(ox + iw * fit, oy + ih * fit));

    // 로봇이 실행 중인 경로(/api/path)
    if (s.path.valid && !s.path.points.empty() && s.path.state != "idle") {
        bool mine = s.path.request_client_id == s.v2.client_id;
        ImVec2 prev = to_screen({s.map.robot_x, s.map.robot_y});
        for (size_t i = 0; i < s.path.points.size(); ++i) {
            ImVec2 p = to_screen(s.path.points[i]);
            bool done = static_cast<int>(i) < s.path.index;
            if (static_cast<int>(i) >= s.path.index) dl->AddLine(prev, p, mine ? kGreen : kBlue, 3 * s_);
            dl->AddCircleFilled(p, 7 * s_, done ? kGrey : (static_cast<int>(i) == s.path.index ? kYellow : kGreen));
            prev = p;
        }
    }
    // 손가락 스케치(노랑)와 단순화한 경유점(초록 테두리)
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
    for (size_t i = 1; i < stroke_px_.size(); ++i)
        dl->AddLine(ImVec2(float(stroke_px_[i - 1].first), float(stroke_px_[i - 1].second)),
                    ImVec2(float(stroke_px_[i].first), float(stroke_px_[i].second)), IM_COL32(250, 210, 40, 200), 4 * s_);
    for (size_t i = 0; i < preview_m_.size(); ++i) {
        ImVec2 p = to_screen(preview_m_[i]);
        if (i) dl->AddLine(to_screen(preview_m_[i - 1]), p, IM_COL32(60, 220, 120, 255), 2 * s_);
        dl->AddCircle(p, 9 * s_, IM_COL32(60, 220, 120, 255), 20, 3 * s_);
    }
    dl->PopClipRect();

    std::string info = s.path.valid ? fmt("경로: %s", path_label(s.path.state)) : "경로: -";
    if (s.path.valid && s.path.count) info += fmt("  %d/%d  남은 %.2f m  %.0f s", std::min(s.path.index + 1, s.path.count), s.path.count, s.path.remaining_m, s.path.elapsed_s);
    if (!preview_m_.empty()) info += fmt("   새 경로 %zu점 %.2f m", preview_m_.size(), polyline_length(preview_m_));
    dl->AddRectFilled(ImVec2(o.x, o.y + H - 40 * s_), ImVec2(o.x + W, o.y + H), IM_COL32(0, 0, 0, 170));
    dl->AddText(ImVec2(o.x + 14 * s_, o.y + H - 32 * s_), IM_COL32_WHITE, info.c_str());
    dl->AddText(ImVec2(o.x + 14 * s_, o.y + 10 * s_), kYellow, "손가락으로 선을 그리고 '경로 보내기' · 로봇은 장애물을 피하지 않습니다");
    (void)now;
}

void App::draw_robot(const RobotSnapshot& s) {
    float avail = ImGui::GetContentRegionAvail().x;
    float bw = (avail - 3 * 12 * s_) / 4, bh = 64 * s_;
    ImGui::Dummy(ImVec2(0, 8 * s_));
    ImGui::Indent(12 * s_);
    ImGui::TextDisabled("자세 프리셋 (rosbridge /puppy_control/pose)");
    int i = 0;
    for (const auto& p : pose_presets()) {
        if (i++ % 4) ImGui::SameLine();
        if (big_button(p.label, bw, bh, pose_presets()[pose_index_].name == std::string(p.name) ? IM_COL32(60, 90, 160, 255) : 0)) {
            pose_index_ = i - 1;
            robot_.set_pose(p.name);
        }
    }
    if (big_button("서기 (go_home)", bw, bh)) robot_.go_home();
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("직접 자세 만들기")) {
        ImGui::PushItemWidth(avail * 0.45f);
        ImGui::SliderFloat("roll (°)", &pose_roll_, -20, 20, "%.0f");
        ImGui::SliderFloat("pitch (°)", &pose_pitch_, -30, 20, "%.0f");
        ImGui::SliderFloat("yaw (°)", &pose_yaw_, -20, 20, "%.0f");
        ImGui::SliderFloat("height (cm)", &pose_height_, -12, -5, "%.1f");
        ImGui::SliderFloat("x_shift (cm)", &pose_xshift_, -3, 3, "%.1f");
        ImGui::SliderFloat("stance_x (cm)", &pose_sx_, -2, 2, "%.1f");
        ImGui::SliderFloat("stance_y (cm)", &pose_sy_, -2, 2, "%.1f");
        ImGui::SliderInt("run_time (ms)", &pose_time_, 200, 2000);
        ImGui::PopItemWidth();
        if (big_button("이 자세로", bw, bh))
            robot_.set_pose_custom(pose_roll_ * kDeg, pose_pitch_ * kDeg, pose_yaw_ * kDeg, pose_height_, pose_xshift_, pose_sx_, pose_sy_, pose_time_);
        ImGui::SameLine();
        if (big_button("서기 값으로", bw, bh)) {
            pose_roll_ = pose_pitch_ = pose_yaw_ = 0;
            pose_height_ = -10;
            pose_xshift_ = -0.5f;
            pose_sx_ = pose_sy_ = 0;
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("동작 그룹 (노란색 = 큰 동작, 두 번 눌러 실행 · 실행 중에는 이동이 막힙니다)");
    double now = now_s();
    i = 0;
    for (const auto& a : action_groups()) {
        if (i++ % 4) ImGui::SameLine();
        bool armed = caution_action_ == a.name && now < caution_armed_until_;
        unsigned c = armed ? IM_COL32(200, 120, 30, 255) : (a.caution ? IM_COL32(90, 80, 30, 255) : 0);
        std::string label = std::string(armed ? "다시 눌러 실행: " : "") + a.label + "##" + a.name;
        if (big_button(label.c_str(), bw, bh, c)) {
            action_index_ = i - 1;
            if (a.caution && !armed) {
                caution_action_ = a.name;
                caution_armed_until_ = now + 2.0;
            } else {
                caution_action_.clear();
                robot_.run_action(a.name);
                pad_.rumble(Rumble::Tick);
            }
        }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("로봇 전체 비상정지(rosbridge, 다른 조종기·동작 그룹 포함)");
    if (big_button("전체 정지", bw, bh, kRed)) robot_.emergency_stop_all();
    ImGui::SameLine();
    if (big_button("전체 해제", bw, bh)) robot_.clear_emergency_all();
    if (s.status.is_object()) {
        ImGui::Spacing();
        ImGui::TextDisabled("/puppy_control/status");
        for (const auto& kv : s.status.as_object()) ImGui::Text("%s: %s", kv.first.c_str(), kv.second.text().c_str());
    }
    ImGui::Unindent(12 * s_);
}

void App::draw_log(double now, const RobotSnapshot& s) {
    const V2Snapshot& v = s.v2;
    ImGui::Dummy(ImVec2(0, 8 * s_));
    ImGui::Indent(12 * s_);
    ImGui::Text("조종(v2) %s  client %s  sequence %lld", v.robot.c_str(), v.client_id.substr(0, 8).c_str(), static_cast<long long>(v.sequence));
    ImGui::Text("ACK RTT 지금 %.2f ms · 평균 %.2f ms · 유실 %.1f %% · 보냄 %llu · 받음 %llu", v.rtt_last_ms, v.rtt_avg_ms, v.loss_pct,
                static_cast<unsigned long long>(v.tx), static_cast<unsigned long long>(v.rx));
    if (!rtt_hist_.empty()) {
        std::vector<float> sorted = rtt_hist_;
        std::sort(sorted.begin(), sorted.end());
        ImGui::Text("RTT 표본 %zu개 · 중앙값 %.2f ms · p95 %.2f ms · 최대 %.2f ms", sorted.size(), sorted[sorted.size() / 2],
                    sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)], sorted.back());
        ImGui::PlotLines("##rtt", rtt_hist_.data(), static_cast<int>(rtt_hist_.size()), 0, "ACK RTT (ms)", 0.0f, std::max(5.0f, sorted.back() * 1.2f),
                         ImVec2(ImGui::GetContentRegionAvail().x - 12 * s_, 110 * s_));
    }
    ImGui::Text("카메라 %s %.1f fps · 풀기 %.1f ms · 프레임 나이 %.0f ms · 화면 %.0f fps", robot_.camera_status().c_str(), robot_.camera_fps(),
                cam_dec_.decode_ms(), cam_tex_.valid() ? (now - cam_tex_.rx_time) * 1000 : -1.0, ImGui::GetIO().Framerate);
    ImGui::Text("rosbridge %s · 지도 정보 %s · 스캔 %zu빔", s.rb_status.c_str(), s.map.valid ? "있음" : "없음", s.scan.ranges.size());
    if (s.arbiter.is_object()) ImGui::TextWrapped("중재기: %s", s.arbiter.dump().c_str());
    ImGui::Separator();
    ImGui::TextDisabled("이벤트");
    ImGui::BeginChild("evlog", ImVec2(0, robot_.packet_capture() ? 220 * s_ : 0), ImGuiChildFlags_Borders);
    for (const auto& l : robot_.events().tail(200)) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    if (robot_.packet_capture()) {
        ImGui::TextDisabled("패킷(송신 TX / 수신 RX)");
        ImGui::BeginChild("pktlog", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (const auto& l : robot_.packets().tail(200)) ImGui::TextUnformatted(l.c_str());
        ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::Unindent(12 * s_);
}

void App::draw_connect(const RobotSnapshot& s) {
    float avail = ImGui::GetContentRegionAvail().x;
    float bh = 64 * s_;
    ImGui::Dummy(ImVec2(0, 8 * s_));
    ImGui::Indent(12 * s_);
    ImGui::TextDisabled("로봇 찾기 (UDP 5002)");
    bool busy = discovering_.valid() && discovering_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (discovering_.valid() && !busy) {
        found_ = discovering_.get();
        discovered_once_ = true;
    }
    ImGui::BeginDisabled(busy);
    if (big_button(busy ? "찾는 중…" : "같은 Wi-Fi 에서 찾기", avail * 0.4f, bh))
        discovering_ = std::async(std::launch::async, [] { return discover_robots(1500); });
    ImGui::EndDisabled();
    for (const auto& f : found_) {
        std::string label = f.name + "   " + f.ip + "##" + f.id;
        if (big_button(label.c_str(), avail * 0.6f, bh, IM_COL32(40, 110, 70, 255))) connect_to(f.ip);
    }
    if (discovered_once_ && found_.empty()) ImGui::TextColored(col4(kYellow), "응답한 로봇이 없습니다. 로봇과 흑갈리가 같은 Wi-Fi 인지 확인하세요.");
    std::string mine;
    for (const auto& ip : local_ipv4_addresses()) mine += ip + "  ";
    ImGui::TextDisabled("이 기기 주소: %s", mine.c_str());

    ImGui::Spacing();
    ImGui::TextDisabled("주소 직접 입력");
    // 화면 숫자 패드(흑갈리에는 실물 키보드가 없다)
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::Text("%s_", ip_entry_.c_str());
    ImGui::PopFont();
    const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "←"};
    float kw = 96 * s_, kh = 64 * s_;
    for (int i = 0; i < 12; ++i) {
        if (i % 3) ImGui::SameLine();
        if (ImGui::Button(keys[i], ImVec2(kw, kh))) {
            if (i == 11) {
                if (!ip_entry_.empty()) ip_entry_.pop_back();
            } else if (ip_entry_.size() < 15) {
                ip_entry_ += keys[i];
            }
        }
    }
    if (big_button("지우기##ip", kw * 1.5f, kh)) ip_entry_.clear();
    ImGui::SameLine();
    if (big_button("연결", kw * 1.5f, kh, IM_COL32(40, 110, 70, 255)) && !ip_entry_.empty()) connect_to(ip_entry_);
    if (s.connected) {
        ImGui::SameLine();
        if (big_button("연결 끊기", kw * 1.5f, kh)) robot_.disconnect();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("설정");
    bool changed = false;
    changed |= ImGui::Checkbox("왼쪽 스틱 하나로 운전(전후 + 회전)", &cfg_.single_stick);
    changed |= ImGui::Checkbox("기울여서 운전(RB 를 쥔 동안, 쥐는 순간 자세가 0)", &cfg_.tilt_enabled);
    if (cfg_.tilt_enabled) {
        ImGui::Text("   가속도계: %s", tilt_.status().c_str());
        changed |= ImGui::Checkbox("   앞뒤 반대로", &cfg_.tilt_invert_forward);
        changed |= ImGui::Checkbox("   좌우 반대로", &cfg_.tilt_invert_turn);
        tilt_drive_.invert_forward = cfg_.tilt_invert_forward;
        tilt_drive_.invert_turn = cfg_.tilt_invert_turn;
    }
    ImGui::PushItemWidth(avail * 0.4f);
    if (ImGui::SliderFloat("최고 속도 비율", &cfg_.max_speed, 0.1f, 1.0f, "%.2f")) {
        speed_full_ = false;
        robot_.v2().set_speed_scale(cfg_.max_speed);
        changed = true;
    }
    ImGui::TextDisabled("   1.00 = 로봇 상한(전진 25 cm/s, 후진 5 cm/s, 회전 0.28 rad/s)");
    if (ImGui::SliderFloat("스틱 데드존", &cfg_.deadzone, 0.05f, 0.4f, "%.2f")) {
        pad_.deadzone = cfg_.deadzone;
        changed = true;
    }
    ImGui::PopItemWidth();
    if (ImGui::Checkbox("카메라 영상 받기(Wi-Fi 15~40 Mbps)", &cfg_.camera)) {
        robot_.set_camera_enabled(cfg_.camera);
        changed = true;
    }
    if (changed) save_settings();
    ImGui::Spacing();
    if (big_button("앱 끝내기 (View+Menu 1.5초)", avail * 0.4f, bh)) quit_ = true;
    ImGui::TextDisabled("puppypi_heukgalli · Dear ImGui %s · stb_image 2.32 · 로봇 소프트웨어는 바꾸지 않는 클라이언트", IMGUI_VERSION);
    ImGui::Unindent(12 * s_);
}

}  // namespace hg
