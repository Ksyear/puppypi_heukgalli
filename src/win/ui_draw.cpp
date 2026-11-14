// 화면 그리기 v2 "검은 유리 조종석"(docs/DESIGN_PLAN_v2.md). 콘텐츠(카메라·LiDAR 지도)를 화면 전체에 깔고,
// 조작은 그 위에 뜬 유리 조각으로만 둔다. 입력·안전 로직은 ui.cpp 그대로(이 파일은 그리기와 화면 버튼만).
#include "ui.hpp"  // sock.hpp(winsock2) 가 windows.h 보다 먼저

#include "catalog.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "theme.hpp"
#include "util.hpp"
#include "widgets.hpp"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>

namespace hg {

namespace {
using theme::F;
using theme::T;
namespace sz = theme::sz;
namespace ic = theme::icon;
using w::Lamp;
using w::Pad;
using w::Tone;

struct Capsule {
    Tone tone;
    bool filled;
    Lamp lamp;
    std::string text;
};

Tone toast_tone(const std::string& t) {
    if (t.find("실패") != std::string::npos || t.find("비상") != std::string::npos || t.find("거부") != std::string::npos ||
        t.find("없음") != std::string::npos)
        return Tone::Danger;
    if (t.find("큰 동작") != std::string::npos || t.find("확인") != std::string::npos || t.find("장애물") != std::string::npos)
        return Tone::Warn;
    return Tone::Info;
}

Tone event_tone(const std::string& t) {
    if (t.find("실패") != std::string::npos || t.find("끊") != std::string::npos || t.find("비상") != std::string::npos ||
        t.find("없음") != std::string::npos)
        return Tone::Danger;
    if (t.find("거부") != std::string::npos || t.find("취소") != std::string::npos || t.find("중립") != std::string::npos ||
        t.find("장애물") != std::string::npos)
        return Tone::Warn;
    return Tone::Info;
}

// 주행 상태(색 + 램프 모양 + 짧은 글자). 비상정지·응답 없음이 가장 우선
Capsule capsule_for(const RobotSnapshot& s, bool held, bool focus, bool connect_screen, bool pad, const std::string& source,
                    int speed_pct) {
    const V2Snapshot& v = s.v2;
    if (!s.connected) return {Tone::Muted, false, Lamp::Ring, "연결 안 됨"};
    if (v.estop || s.robot_estop_requested) return {Tone::Danger, true, Lamp::Cross, "비상정지"};
    if (!v.linked) return {Tone::Danger, true, Lamp::Cross, "응답 없음"};
    if (v.state == "fault") return {Tone::Danger, true, Lamp::Cross, "로봇 오류"};
    if (connect_screen) return {Tone::Info, false, Lamp::Ring, "연결 화면 · 운전 안 함"};
    if (!focus) return {Tone::Muted, false, Lamp::Ring, "운전 안 함 · 창 포커스 없음"};
    if (held && v.release_required) return {Tone::Warn, true, Lamp::Half, "RT를 놓았다 다시 쥐세요"};
    if (held && v.cmd_active) return {Tone::Ok, true, Lamp::Full, "주행 중 · " + source + fmt(" · %d%%", speed_pct)};
    if (held) {
        if (!v.accepted && !v.reason.empty() && v.reason != "ok") return {Tone::Warn, true, Lamp::Half, std::string("운전 대기 · ") + reason_label(v.reason)};
        return {Tone::Warn, true, Lamp::Half, std::string("운전 대기 · ") + state_label(v.state)};
    }
    if (s.nav.phase == "following" || s.nav.phase == "replanning")
        return {s.nav.phase == "replanning" ? Tone::Warn : Tone::Info, true, Lamp::Full, s.nav.message};
    if (v.state == "awaiting_neutral" || v.state == "timeout") return {Tone::Warn, false, Lamp::Half, "중립 확인 중"};
    if (!pad) return {Tone::Warn, false, Lamp::Ring, "패드 없음 · 게임패드 모드 확인"};
    return {Tone::Info, false, Lamp::Ring, "대기 · RT를 쥐면 운전"};
}
}  // namespace

std::string App::masked(const std::string& line) const {
    if (!privacy_) return line;
    std::string out = line, host = robot_.host();
    if (!host.empty())
        for (size_t p = out.find(host); p != std::string::npos; p = out.find(host, p + 4)) out.replace(p, host.size(), "로봇");
    return out;
}

void App::draw(double now, const RobotSnapshot& s) {
    w::set_scale(s_);
    double dt = last_draw_ > 0 ? std::min(0.1, now - last_draw_) : 0.016;
    last_draw_ = now;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, T.bg0);
    // L0 콘텐츠(화면 전체)
    switch (screen_) {
        case Screen::Drive: draw_drive(now, s); break;
        case Screen::Map: draw_map(now, s); break;
        case Screen::Robot: draw_robot(s); break;
        case Screen::Log: draw_log(now, s); break;
        case Screen::Connect: draw_connect(s); break;
    }
    // L1·L2: 유리 조각과 안전 요소
    (void)dt;
    draw_overlay(now, s);
    ImGui::End();
    ImGui::PopStyleVar();
}

void App::draw_overlay(double now, const RobotSnapshot& s) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float W = io.DisplaySize.x, H = io.DisplaySize.y, m = 20 * s_;
    const V2Snapshot& v = s.v2;
    bool estop = v.estop || s.robot_estop_requested;
    float dim = held_ ? 0.6f : 1.0f;  // 운전 중에는 탐색 요소를 가라앉힌다(안전 요소는 그대로)
    if (estop) dl->AddRect(ImVec2(2, 2), ImVec2(W - 2, H - 2), T.danger, 0, 4 * s_);

    // 나브 필(왼쪽 위)
    static const char* icons[] = {ic::drive, ic::map, ic::robot, ic::log, ic::settings};
    static const char* names[] = {"주행", "LiDAR 지도", "로봇", "로그", "연결"};
    static const Screen order[] = {Screen::Drive, Screen::Map, Screen::Robot, Screen::Log, Screen::Connect};
    int idx = 0;
    for (int i = 0; i < 5; ++i)
        if (order[i] == screen_) idx = i;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, dim);
    ImGui::SetCursorScreenPos(ImVec2(m, m));
    if (w::nav_pill("nav", icons, names, 5, &idx, &nav_anim_x_, 1.0 / std::max(30.0f, io.Framerate))) screen_ = order[idx];
    ImGui::PopStyleVar();

    // 주행 상태 캡슐(위 가운데) — 상태를 한 곳에서만 보인다
    Capsule c = capsule_for(s, held_, focus_, screen_ == Screen::Connect, pad_.state().connected, drive_source_,
                            static_cast<int>(robot_.v2().speed_scale() * 100 + 0.5));
    float ch = 48 * s_, cw = w::capsule2_width(ch, 330 * s_, c.text.c_str());
    w::capsule2(dl, ImVec2((W - cw) / 2, m), ch, 330 * s_, c.tone, c.filled, c.lamp, c.text.c_str());
    // 경로 진행 배너(캡슐 아래)
    if (s.path.valid && (s.path.state == "rotate" || s.path.state == "drive") && screen_ != Screen::Map) {
        std::string t = fmt("경로 %s %d/%d · 남은 %.1f m · 스틱을 쓰면 취소", path_label(s.path.state), s.path.index + 1, s.path.count, s.path.remaining_m);
        float bw = w::capsule2_width(36 * s_, 0, t.c_str());
        w::capsule2(dl, ImVec2((W - bw) / 2, m + ch + 10 * s_), 36 * s_, 0, Tone::Info, false, Lamp::Full, t.c_str());
    }

    // 텔레메트리(오른쪽 위, 글자만). 탭하면 로그 화면
    {
        float x = W - m, y = m + 12 * s_;
        auto item = [&](const std::string& val, const char* unit, ImU32 col, const char* icon) {
            float uw = unit ? w::text_size(F.body, sz::caption, unit).x : 0;
            if (unit) {
                dl->AddText(F.body, sz::caption * s_, ImVec2(x - uw, y + 4 * s_), theme::alpha(T.text2, dim), unit);
                x -= uw + 4 * s_;
            }
            float vw = w::num_text(dl, F.semi, sz::label, ImVec2(x, y), theme::alpha(col, dim), val.c_str(), true);
            x -= vw;
            if (icon) {
                ImVec2 is = w::text_size(F.body, sz::icon, icon);
                dl->AddText(F.body, sz::icon * s_, ImVec2(x - is.x - 6 * s_, y - 2 * s_), theme::alpha(T.text2, dim), icon);
                x -= is.x + 6 * s_;
            }
            x -= 20 * s_;
        };
        bool pad_ok = pad_.state().connected;
        item(pad_ok ? "" : "없음", nullptr, pad_ok ? T.text1 : T.danger, ic::gamepad);
        if (s.connected) {
            if (robot_.camera_enabled()) item(fmt("%.0f", robot_.camera_fps()), "fps", T.text1, ic::camera);
            item("ROS", nullptr, s.rb_connected ? T.text1 : T.danger, nullptr);
            ImU32 rc = !v.linked ? T.danger : (v.rtt_avg_ms > 100 || v.loss_pct > 5 ? T.warn : T.text1);
            item(v.linked ? fmt("%.0f", v.rtt_avg_ms) : "—", "ms", rc, ic::wifi);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, m));
        if (ImGui::InvisibleButton("telemetry", ImVec2(W - m - x, 44 * s_))) screen_ = Screen::Log;
    }

    draw_fabs(now, s);
    if (sheet_pose_ && screen_ == Screen::Robot) draw_pose_sheet(s);

    // 비상정지(오른쪽 아래, 모든 화면·시트 위)
    float es = 128 * s_;
    ImGui::SetCursorScreenPos(ImVec2(W - m - es, H - m - es));
    estop_button(now, s, es, es);

    // 토스트(아래 가운데): 120 ms 올라오며 나타나고 끝 200 ms 동안 사라진다
    if (toast_ != toast_seen_) {
        toast_seen_ = toast_;
        toast_start_ = now;
        if (toast_until_ < now + 4.0 && !toast_.empty()) toast_until_ = now + 4.0;
    }
    if (!toast_.empty() && now < toast_until_) {
        float in = static_cast<float>(std::min(1.0, (now - toast_start_) / 0.12));
        float out = static_cast<float>(std::min(1.0, (toast_until_ - now) / 0.2));
        w::toast(ImGui::GetForegroundDrawList(), ImVec2((W - es - m) / 2, H - m - 44 * s_ - 8 * s_), masked(toast_).c_str(),
                 toast_tone(toast_), std::min(in, out), 8 * (1 - in));
    }
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

// 오른쪽 가장자리의 둥근 버튼들(화면마다 최대 4개). 비상정지 위쪽에 아래에서 위로 쌓는다
void App::draw_fabs(double now, const RobotSnapshot& s) {
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y, m = 20 * s_, d = 56 * s_, gap = 14 * s_;
    float x = W - m - 128 * s_ / 2 - d / 2;
    float y = H - m - 128 * s_ - 24 * s_ - d;
    auto next = [&]() {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        y -= d + gap;
    };
    static const Pad kY = Pad::Y, kA = Pad::A, kX = Pad::X, kRB = Pad::RB;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, held_ ? 0.6f : 1.0f);
    if (screen_ == Screen::Drive) {
        next();
        if (w::fab("home", ic::home, d, &kA, false, !held_, "서기")) robot_.go_home();
        next();
        if (w::fab("snap", ic::camera, d, cfg_.tilt_enabled ? nullptr : &kRB, false, true, "사진")) {
            CreateDirectoryA((exe_dir_ + "\\shots").c_str(), nullptr);
            std::string err, name = "cam_" + time_tag() + ".jpg";
            toast_ = robot_.save_snapshot_jpeg(exe_dir_ + "\\shots\\" + name, &err) ? "사진 저장: shots\\" + name : "사진 실패: " + err;
            toast_until_ = now + 3;
        }
        next();
        static const char* layers[] = {"카메라", "겹쳐 보기", "레이더"};
        if (w::fab("layer", ic::layers, d, &kY, hud_layer_ != 0, true, layers[hud_layer_])) hud_layer_ = (hud_layer_ + 1) % 3;
    } else if (screen_ == Screen::Map) {
        next();
        if (w::fab("pcancel", ic::cancel, d, &kX, false, s.nav.phase == "following" || s.nav.phase == "replanning" ||
                                                          (s.path.valid && (s.path.state == "rotate" || s.path.state == "drive")), "멈추기"))
            robot_.cancel_path();
        next();
        if (w::fab("follow", ic::pin, d, nullptr, map_follow_, true, map_follow_ ? "따라가는 중" : "로봇 보기")) map_follow_ = true;
        next();
        if (w::fab("zoomout", ic::minus, d, nullptr, false, map_ppm_target_ > 21 * s_, "축소")) map_ppm_target_ = std::max(20.0 * s_, map_ppm_target_ / 2);
        next();
        if (w::fab("zoomin", ic::plus, d, nullptr, false, map_ppm_target_ < 159 * s_, "확대")) map_ppm_target_ = std::min(160.0 * s_, map_ppm_target_ * 2);
        next();
        if (w::fab("more", ic::more, d, nullptr, sheet_map_more_, true, "지도")) sheet_map_more_ = !sheet_map_more_;
        if (sheet_map_more_) {
            // 드문 기능: 지도 저장·새로·지우기(작은 유리 메뉴)
            float pw = 220 * s_, ph = 3 * 52 * s_ + 24 * s_;
            ImVec2 p0(x - pw - 14 * s_, y + d + gap - ph + d);
            w::glass_panel(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + pw, p0.y + ph), 16 * s_);
            ImGui::SetCursorScreenPos(ImVec2(p0.x + 12 * s_, p0.y + 12 * s_));
            if (w::pill_button("savemap", "지도 저장", 48 * s_, Tone::Neutral, ic::save, nullptr, nullptr, true, pw - 24 * s_)) {
                robot_.save_map();
                sheet_map_more_ = false;
            }
            ImGui::SetCursorScreenPos(ImVec2(p0.x + 12 * s_, p0.y + 12 * s_ + 52 * s_));
            if (confirm_reset_map_ && now > confirm_reset_until_) confirm_reset_map_ = false;
            if (!confirm_reset_map_) {
                if (w::pill_button("newmap", "지도 새로", 48 * s_, Tone::Neutral, ic::refresh, nullptr, nullptr, true, pw - 24 * s_)) {
                    confirm_reset_map_ = true;
                    confirm_reset_until_ = now + 3.0;
                }
            } else if (w::pill_button("newmap2", "정말 새로?", 48 * s_, Tone::Warn, ic::warning, nullptr, nullptr, true, pw - 24 * s_)) {
                robot_.reset_map();
                confirm_reset_map_ = false;
                sheet_map_more_ = false;
            }
            ImGui::SetCursorScreenPos(ImVec2(p0.x + 12 * s_, p0.y + 12 * s_ + 104 * s_));
            if (w::pill_button("erase", "그린 선 지우기", 48 * s_, Tone::Neutral, ic::erase, nullptr, nullptr, true, pw - 24 * s_)) {
                stroke_m_.clear();
                preview_m_.clear();
                plan_preview_ = PlanResult{};
                sheet_map_more_ = false;
            }
        }
    } else if (screen_ == Screen::Robot) {
        next();
        if (w::fab("home2", ic::home, d, &kA, false, !held_, "서기")) robot_.go_home();
    } else if (screen_ == Screen::Connect) {
        next();
        if (w::fab("quit", ic::power, d, nullptr, false, true, "앱 끝내기")) quit_ = true;
    }
    ImGui::PopStyleVar();
}

// ---------------- 스캔 점(지도와 레이더가 같은 모양) ----------------

void App::ensure_sprite() {
    if (sprite_tex_.valid()) return;
    const int n = 32;
    std::vector<uint8_t> px(n * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float dx = (x + 0.5f) / n - 0.5f, dy = (y + 0.5f) / n - 0.5f, r = std::sqrt(dx * dx + dy * dy) * 2;  // 0 가운데, 1 가장자리
            float a = r < 0.35f ? 1.0f : std::max(0.0f, 1.0f - (r - 0.35f) / 0.65f);
            a = a * a;
            uint8_t* p = &px[(y * n + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(a * 255);
        }
    sprite_tex_.update(dev_, ctx_, px.data(), n, n);
}

void App::draw_scan_points(ImDrawList* dl, const std::vector<std::pair<float, float>>& pts, const Pose2D& pose, bool stale,
                           const std::function<ImVec2(double, double)>& to_screen) {
    ensure_sprite();
    ImTextureRef tex(reinterpret_cast<ImTextureID>(sprite_tex_.srv()));
    for (const auto& p : pts) {
        double dd = std::hypot(p.first - pose.x, p.second - pose.y);
        bool is_close = dd < 0.35;  // (windows.h 가 near 를 매크로로 정의한다)
        float r = (is_close ? 6.0f : 4.5f) * s_;
        ImU32 col = stale ? T.text3 : (is_close ? T.scan_near : T.scan);
        ImVec2 c = to_screen(p.first, p.second);
        dl->AddImage(tex, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0), ImVec2(1, 1), col);
    }
}

// ---------------- 주행 ----------------

// 레이더: 지도와 같은 색·점·로봇 마커. 로봇 앞이 위(heading-up), 반지름 3 m
void App::draw_radar(const RobotSnapshot& s, float cx, float cy, float radius) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double range = 3.0;
    float k = radius / static_cast<float>(range);
    dl->AddCircleFilled(ImVec2(cx, cy + 4 * s_), radius + 12 * s_, IM_COL32(0, 0, 0, 60), 96);
    dl->AddCircleFilled(ImVec2(cx, cy), radius + 12 * s_, T.glass, 96);
    dl->AddCircle(ImVec2(cx, cy), radius + 12 * s_, T.glass_hi, 96, 1.0f);
    for (int r = 1; r <= 3; ++r) dl->AddCircle(ImVec2(cx, cy), r * k, T.grid5, 72, 1.0f);
    for (int r = 1; r <= 3; ++r) {
        float a = -3.14159265f / 4;  // 오른쪽 위 45°
        std::string t = std::to_string(r);
        dl->AddText(F.body, sz::caption * s_, ImVec2(cx + std::cos(a) * r * k + 2 * s_, cy + std::sin(a) * r * k - 16 * s_), T.text3, t.c_str());
    }
    bool stale = !s.scan.valid || now_s() - s.scan.rx_time > 1.0;
    if (s.scan.valid) {
        // 레이저 좌표(앞 +x, 왼쪽 +y) → 화면(앞 = 위)
        Pose2D origin{0, 0, 0};
        auto pts = scan_to_map(s.scan.ranges, s.scan.angle_min, s.scan.angle_increment, range, origin);
        draw_scan_points(dl, pts, origin, stale, [&](double x, double y) {
            return ImVec2(cx - static_cast<float>(y) * k, cy - static_cast<float>(x) * k);
        });
    }
    // 로봇 마커(지도와 같은 모양, 위를 향함)
    dl->AddCircleFilled(ImVec2(cx, cy), 9 * s_, IM_COL32_WHITE, 24);
    dl->AddCircle(ImVec2(cx, cy), 9 * s_, theme::alpha(T.bg0, 0.6f), 24, 2 * s_);
    dl->AddTriangleFilled(ImVec2(cx, cy - 22 * s_), ImVec2(cx - 6 * s_, cy - 9 * s_), ImVec2(cx + 6 * s_, cy - 9 * s_), T.accent);
    const char* lbl = stale ? "SCAN 없음" : "LiDAR 3 m";
    ImVec2 ls = w::text_size(F.semi, sz::caption, lbl);
    dl->AddText(F.semi, sz::caption * s_, ImVec2(cx - ls.x / 2, cy + radius + 16 * s_), stale ? T.warn : T.text2, lbl);
}

void App::draw_drive(double now, const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y, m = 20 * s_;
    double age = cam_tex_.valid() ? now - cam_tex_.rx_time : -1;
    if (hud_layer_ != 2) {
        if (cam_tex_.valid()) {
            float iw = static_cast<float>(cam_tex_.width()), ih = static_cast<float>(cam_tex_.height());
            float k = std::min(W / iw, H / ih);  // fit: 시야를 자르지 않는다
            ImVec2 a((W - iw * k) / 2, (H - ih * k) / 2), b(a.x + iw * k, a.y + ih * k);
            dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(cam_tex_.srv())), a, b, ImVec2(0, 0), ImVec2(1, 1),
                         age > 2.0 ? IM_COL32(150, 150, 150, 255) : IM_COL32_WHITE);
        } else {
            w::empty_state(dl, ImVec2(W / 2, H / 2), ic::camera, robot_.camera_enabled() ? "카메라 신호 없음" : "카메라 꺼짐",
                           robot_.camera_enabled() ? "연결 화면 → 설정에서 카메라 영상 받기를 확인하세요" : "연결 화면 → 설정에서 켤 수 있습니다");
        }
    }
    // 위·아래 가장자리 스크림(유리와 글자가 영상 위에서도 읽히게)
    w::scrim(dl, ImVec2(0, 0), ImVec2(W, 140 * s_), 0.55f, 0.0f);
    w::scrim(dl, ImVec2(0, H - 220 * s_), ImVec2(W, H), 0.0f, 0.6f);
    if (age > 2.0) {
        std::string t = fmt("영상 %.1f초 전", age);
        float bw = w::capsule2_width(32 * s_, 0, t.c_str());
        w::capsule2(dl, ImVec2((W - bw) / 2, 128 * s_), 32 * s_, 0, Tone::Warn, false, Lamp::Half, t.c_str());
    }
    if (hud_layer_ == 1) draw_radar(s, W - m - 128 * s_ - 56 * s_ - 26 * s_ - 150 * s_ - 30 * s_, H - m - 150 * s_ - 20 * s_, 135 * s_);
    if (hud_layer_ == 2) draw_radar(s, W / 2, H / 2 + 20 * s_, std::min(W, H) / 2 - 90 * s_);

    // 왼쪽 아래: D-pad 리드아웃 1줄 + 명령 막대 2줄(패널 없이 스크림 위)
    const V2Snapshot& v = s.v2;
    float x = m + 8 * s_, y = H - m - 112 * s_;
    auto readout = [&](const char* label, const std::string& value, Pad p, ImU32 col) {
        dl->AddText(F.body, sz::caption * s_, ImVec2(x, y + 3 * s_), T.text3, label);
        x += w::text_size(F.body, sz::caption, label).x + 8 * s_;
        dl->AddText(F.semi, sz::label * s_, ImVec2(x, y), col, value.c_str());
        x += w::text_size(F.semi, sz::label, value.c_str()).x + 14 * s_;
        w::pad_glyph(dl, p, ImVec2(x, y + 10 * s_), 16);
        x += 34 * s_;
    };
    const ActionGroup& a = action_groups()[action_index_];
    readout("자세", pose_presets()[pose_index_].label, Pad::DpadUD, T.text1);
    readout("동작", a.label, Pad::DpadLR, a.caution ? T.warn : T.text1);
    readout("속도", fmt("%d%%", speed_full_ ? 100 : static_cast<int>(cfg_.max_speed * 100 + 0.5f)), Pad::LS, T.text1);
    auto bar = [&](float yy, const char* name, double val) {
        float bx = m + 8 * s_, tw = 320 * s_;
        dl->AddText(F.semi, sz::label * s_, ImVec2(bx, yy), T.text2, name);
        float x0 = bx + 48 * s_, cy = yy + 10 * s_, mid = x0 + tw / 2;
        dl->AddRectFilled(ImVec2(x0, cy - 3 * s_), ImVec2(x0 + tw, cy + 3 * s_), T.ctl_on, 3 * s_);
        float e = mid + static_cast<float>(std::clamp(val, -1.0, 1.0)) * tw / 2;
        dl->AddRectFilled(ImVec2(std::min(mid, e), cy - 3 * s_), ImVec2(std::max(mid, e), cy + 3 * s_), v.cmd_active ? T.ok : T.text3, 3 * s_);
        dl->AddLine(ImVec2(mid, cy - 8 * s_), ImVec2(mid, cy + 8 * s_), T.text2, 2 * s_);
        w::num_text(dl, F.semi, sz::label, ImVec2(x0 + tw + 14 * s_, yy), v.cmd_active ? T.text1 : T.text3, fmt("%+.2f", val).c_str());
    };
    bar(H - m - 72 * s_, "전후", v.cmd_forward);
    bar(H - m - 36 * s_, "회전", -v.cmd_turn);  // 화면 오른쪽 = 우회전
}

// ---------------- LiDAR 지도 ----------------

// 점유 격자 → 텍스처. 점유 칸은 코어(밝은 청록) + 흐린 할로(블러), 빈 칸은 바닥보다 아주 조금 밝게, 모름은 투명
void App::update_grid_texture(const RobotSnapshot& s) {
    if (!s.grid || s.grid_version == grid_tex_version_) return;
    const OccGrid& g = *s.grid;
    size_t n = static_cast<size_t>(g.w) * g.h;
    std::vector<float> halo(n, 0.0f), tmp(n, 0.0f);
    for (size_t i = 0; i < n; ++i) halo[i] = g.cells[i] >= 50 ? 1.0f : 0.0f;
    // 분리형 박스 블러(r=2) 두 번 ≈ 가우시안
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x) {
                float sum = 0;
                for (int k = -2; k <= 2; ++k) sum += halo[static_cast<size_t>(y) * g.w + std::clamp(x + k, 0, g.w - 1)];
                tmp[static_cast<size_t>(y) * g.w + x] = sum / 5;
            }
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x) {
                float sum = 0;
                for (int k = -2; k <= 2; ++k) sum += tmp[static_cast<size_t>(std::clamp(y + k, 0, g.h - 1)) * g.w + x];
                halo[static_cast<size_t>(y) * g.w + x] = sum / 5;
            }
    }
    std::vector<uint8_t> rgba(n * 4, 0);
    const ImVec4 occ = ImGui::ColorConvertU32ToFloat4(T.occ), fr = ImGui::ColorConvertU32ToFloat4(T.free),
                 glow = ImGui::ColorConvertU32ToFloat4(T.lidar);
    for (size_t i = 0; i < n; ++i) {
        int8_t v = g.cells[i];
        float r = 0, gg = 0, b = 0, a = 0;
        if (v >= 50) {
            r = occ.x; gg = occ.y; b = occ.z; a = 1.0f;
        } else {
            if (v >= 0) {
                r = fr.x; gg = fr.y; b = fr.z; a = 0.9f;
            }
            float h = std::min(1.0f, halo[i] * 1.6f) * 0.55f;  // 벽 둘레 은은한 빛
            if (h > 0.01f) {
                r = r * (1 - h) + glow.x * h;
                gg = gg * (1 - h) + glow.y * h;
                b = b * (1 - h) + glow.z * h;
                a = std::max(a, h);
            }
        }
        uint8_t* p = &rgba[i * 4];
        p[0] = static_cast<uint8_t>(r * 255);
        p[1] = static_cast<uint8_t>(gg * 255);
        p[2] = static_cast<uint8_t>(b * 255);
        p[3] = static_cast<uint8_t>(a * 255);
    }
    if (grid_tex_.update(dev_, ctx_, rgba.data(), g.w, g.h)) grid_tex_version_ = s.grid_version;
}

void App::draw_map(double now, const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y, m = 20 * s_;
    update_grid_texture(s);
    bool have_pose = s.pose_rx > 0 && now - s.pose_rx < 3.0;
    bool have_grid = s.grid && grid_tex_.valid();
    // 보기: 지도 고정(north-up). 확대 단계는 20/40/80/160 px/m(논리), 160 ms 보간. 따라가기면 로봇이 가운데(120 ms 지연)
    if (map_ppm_target_ <= 0) map_ppm_target_ = 80 * s_;
    if (map_ppm_ <= 0) map_ppm_ = map_ppm_target_;
    double dt = 1.0 / std::max(30.0f, io.Framerate);
    map_ppm_ += (map_ppm_target_ - map_ppm_) * (1 - std::exp(-dt / 0.06));
    if (map_follow_ && have_pose && !drawing_) {
        double kf = 1 - std::exp(-dt / 0.12);
        map_cx_ += (s.pose.x - map_cx_) * kf;
        map_cy_ += (s.pose.y - map_cy_) * kf;
    }
    float cxs = W / 2, cys = H / 2 + 20 * s_;
    auto to_screen = [&](double x, double y) {
        return ImVec2(cxs + static_cast<float>((x - map_cx_) * map_ppm_), cys - static_cast<float>((y - map_cy_) * map_ppm_));
    };
    auto to_map = [&](ImVec2 p) { return Pt{map_cx_ + (p.x - cxs) / map_ppm_, map_cy_ - (p.y - cys) / map_ppm_}; };
    // 1 m / 5 m 격자
    {
        Pt a = to_map(ImVec2(0, 0)), b = to_map(ImVec2(W, H));
        bool fine = map_ppm_ >= 30 * s_;
        for (double gx = std::floor(a.first); gx <= b.first; gx += 1.0) {
            bool five = std::fmod(std::fabs(gx), 5.0) < 1e-6;
            if (!fine && !five) continue;
            float px = to_screen(gx, 0).x;
            dl->AddLine(ImVec2(px, 0), ImVec2(px, H), five ? T.grid5 : T.grid1, 1.0f);
        }
        for (double gy = std::floor(b.second); gy <= a.second; gy += 1.0) {
            bool five = std::fmod(std::fabs(gy), 5.0) < 1e-6;
            if (!fine && !five) continue;
            float py = to_screen(0, gy).y;
            dl->AddLine(ImVec2(0, py), ImVec2(W, py), five ? T.grid5 : T.grid1, 1.0f);
        }
    }
    if (have_grid) {
        const OccGrid& g = *s.grid;
        dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(grid_tex_.srv())), to_screen(g.ox, g.oy + g.h * g.res),
                     to_screen(g.ox + g.w * g.res, g.oy), ImVec2(0, 1), ImVec2(1, 0));
    }
    // 지나온 길: 오래될수록 투명(4 구간)
    if (s.trail.size() > 1) {
        size_t n = s.trail.size();
        static const float alphas[4] = {0.08f, 0.18f, 0.35f, 0.55f};
        for (int q = 0; q < 4; ++q) {
            size_t a = n * q / 4, b = std::min(n, n * (q + 1) / 4 + 1);
            if (b <= a + 1) continue;
            std::vector<ImVec2> pts;
            for (size_t i = a; i < b; ++i) pts.push_back(to_screen(s.trail[i].first, s.trail[i].second));
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::alpha(T.accent, alphas[q]), 3 * s_, ImDrawFlags_None);
        }
    }
    // 로봇이 실행 중인 경로(/api/path): 지난 구간 회색, 남은 구간 info, 현재 목표 warn
    if (s.path.valid && !s.path.points.empty() && (s.path.state == "rotate" || s.path.state == "drive")) {
        ImVec2 prev = to_screen(s.pose.x, s.pose.y);
        for (size_t i = 0; i < s.path.points.size(); ++i) {
            ImVec2 p = to_screen(s.path.points[i].first, s.path.points[i].second);
            int idx = static_cast<int>(i);
            if (idx >= s.path.index) dl->AddLine(prev, p, T.info, 3 * s_);
            if (idx == s.path.index) {
                dl->AddCircleFilled(p, 11 * s_, T.warn, 24);
                dl->AddCircle(p, 11 * s_, T.ink, 24, 2 * s_);
            } else {
                dl->AddCircleFilled(p, 5 * s_, idx < s.path.index ? T.text3 : T.info, 16);
            }
            prev = p;
        }
    }
    // 경로를 막았던 점(회피가 기억 중)
    for (const auto& h : s.nav.obstacles) {
        ImVec2 p = to_screen(h.first, h.second);
        dl->AddCircle(p, 7 * s_, T.danger, 16, 2 * s_);
    }
    // 스캔 원(3 m)과 실시간 스캔 점
    bool scan_stale = !s.scan.valid || now - s.scan.rx_time > 1.0;
    if (have_pose) {
        ImVec2 c = to_screen(s.pose.x, s.pose.y);
        float r3 = 3.0f * static_cast<float>(map_ppm_);
        dl->AddCircleFilled(c, r3, theme::alpha(T.scan, 0.03f), 96);
        dl->AddCircle(c, r3, scan_stale ? theme::alpha(T.warn, 0.5f) : theme::alpha(T.scan, 0.10f), 96, 1.0f);
        if (s.scan.valid)
            draw_scan_points(dl, scan_to_map(s.scan.ranges, s.scan.angle_min, s.scan.angle_increment, s.scan.range_max, s.pose), s.pose,
                             scan_stale, to_screen);
    }
    // 그린 선(옅게)과 앱이 계획한 실제 경로(번호)
    bool path_busy = s.nav.phase == "following" || s.nav.phase == "replanning";
    if (stroke_m_.size() > 1) {
        std::vector<ImVec2> pts;
        for (const auto& p : stroke_m_) pts.push_back(to_screen(p.first, p.second));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), theme::alpha(T.accent, drawing_ ? 0.85f : 0.3f), 4 * s_, ImDrawFlags_RoundCornersAll);
    }
    bool preview = !plan_preview_.path.empty();
    const std::vector<Pt>& show = preview ? plan_preview_.path : s.nav.planned;
    if (!show.empty() && (preview || path_busy)) {
        ImVec2 prev = to_screen(s.pose.x, s.pose.y);
        for (const auto& q : show) {
            ImVec2 p = to_screen(q.first, q.second);
            dl->AddLine(prev, p, preview ? T.ok : theme::alpha(T.ok, 0.55f), 3 * s_);
            prev = p;
        }
        for (size_t i = 0; i < show.size(); ++i) {
            ImVec2 p = to_screen(show[i].first, show[i].second);
            bool last = i + 1 == show.size();
            dl->AddCircleFilled(p, 12 * s_, T.bg0, 24);
            dl->AddCircle(p, 12 * s_, T.ok, 24, 2.5f * s_);
            if (last) dl->AddCircle(p, 16 * s_, T.ok, 24, 1.5f * s_);
            std::string nlab = std::to_string(i + 1);
            ImVec2 ts = w::text_size(F.semi, sz::caption, nlab.c_str());
            dl->AddText(F.semi, sz::caption * s_, ImVec2(p.x - ts.x / 2, p.y - ts.y / 2), T.text1, nlab.c_str());
        }
    }
    // 로봇 마커: 흰 원 + 바깥 테 + 머리 쐐기(진행 방향) + 몸체(확대 시)
    ImVec2 rc = to_screen(s.pose.x, s.pose.y);
    if (have_pose) {
        float cs = std::cos(static_cast<float>(s.pose.yaw)), sn = std::sin(static_cast<float>(s.pose.yaw));
        auto rot = [&](float fx, float fy) {  // 로봇 좌표(m) → 화면
            return ImVec2(rc.x + (fx * cs - fy * sn) * static_cast<float>(map_ppm_), rc.y - (fx * sn + fy * cs) * static_cast<float>(map_ppm_));
        };
        if (map_ppm_ >= 70 * s_)
            dl->AddQuad(rot(0.113f, 0.075f), rot(-0.113f, 0.075f), rot(-0.113f, -0.075f), rot(0.113f, -0.075f), theme::alpha(IM_COL32_WHITE, 0.6f), 1.5f * s_);
        dl->AddCircleFilled(ImVec2(rc.x, rc.y + 2 * s_), 10 * s_, IM_COL32(0, 0, 0, 90), 24);
        dl->AddCircleFilled(rc, 10 * s_, IM_COL32_WHITE, 24);
        dl->AddCircle(rc, 10 * s_, theme::alpha(T.bg0, 0.6f), 24, 2 * s_);
        ImVec2 tip(rc.x + cs * 26 * s_, rc.y - sn * 26 * s_);
        ImVec2 l(rc.x + cs * 10 * s_ - sn * 7 * s_, rc.y - sn * 10 * s_ - cs * 7 * s_), r(rc.x + cs * 10 * s_ + sn * 7 * s_, rc.y - sn * 10 * s_ + cs * 7 * s_);
        dl->AddTriangleFilled(tip, r, l, T.accent);
        // 로봇이 화면 밖이면 가장자리에 방향 화살표와 거리
        if (rc.x < 0 || rc.y < 0 || rc.x > W || rc.y > H) {
            ImVec2 cc(W / 2, H / 2);
            float dx = rc.x - cc.x, dy = rc.y - cc.y, len = std::sqrt(dx * dx + dy * dy);
            ImVec2 e(cc.x + dx / len * (std::min(W, H) / 2 - 60 * s_), cc.y + dy / len * (std::min(W, H) / 2 - 60 * s_));
            dl->AddCircleFilled(e, 16 * s_, T.accent, 24);
            std::string dist = fmt("%.1f m", std::hypot(s.pose.x - map_cx_, s.pose.y - map_cy_));
            dl->AddText(F.semi, sz::caption * s_, ImVec2(e.x + 20 * s_, e.y - 9 * s_), T.text1, dist.c_str());
        }
    }
    // 손가락: 로봇 1 m 안에서 시작하면 그리기, 밖이면 지도 옮기기. 모드 필로 강제할 수 있다
    ImGui::SetCursorScreenPos(ImVec2(0, 0));
    ImGui::SetNextItemAllowOverlap();  // 위에 뜬 버튼들이 손가락을 먼저 받게
    ImGui::InvisibleButton("mapinput", ImVec2(W, H));
    if (ImGui::IsItemActivated()) {
        ImVec2 mp = io.MousePos;
        bool near_robot = have_pose && std::hypot(mp.x - rc.x, mp.y - rc.y) <= std::max(80 * s_, static_cast<float>(map_ppm_));
        draw_gesture_ = !path_busy && (map_mode_ == 1 ? false : (map_mode_ == 2 ? true : near_robot));
        if (draw_gesture_) {
            stroke_m_.clear();
            preview_m_.clear();
            plan_preview_ = PlanResult{};
            drawing_ = true;
        } else {
            map_follow_ = false;
        }
    }
    if (ImGui::IsItemActive()) {
        if (draw_gesture_) {
            Pt mm = to_map(io.MousePos);
            if (stroke_m_.empty() || std::hypot(mm.first - stroke_m_.back().first, mm.second - stroke_m_.back().second) * map_ppm_ > 3 * s_)
                stroke_m_.push_back(mm);
        } else {
            map_cx_ -= io.MouseDelta.x / map_ppm_;
            map_cy_ += io.MouseDelta.y / map_ppm_;
        }
    }
    if (ImGui::IsItemDeactivated() && draw_gesture_) {
        drawing_ = false;
        draw_gesture_ = false;
        if (!stroke_m_.empty()) {
            preview_m_ = sketch_to_path(stroke_m_);
            plan_preview_ = robot_.preview_plan(preview_m_);
            if (!plan_preview_.ok) {
                toast_ = "경로를 만들 수 없음 · " + plan_preview_.reason;
                toast_until_ = now + 4;
            }
        }
    }
    // 로봇 주변 그리기 영역 힌트
    if (have_pose && map_mode_ == 0 && !path_busy && stroke_m_.empty())
        dl->AddCircle(rc, std::max(80 * s_, static_cast<float>(map_ppm_)), theme::alpha(T.accent, 0.35f), 64, 1.5f * s_);

    // 지도 없음 안내(격자·스캔·로봇은 그대로 보인다)
    if (!have_grid) {
        const char* t = have_pose ? "지도 생성 중 · 로봇이 조금 움직이면 벽이 나타납니다" : "LiDAR 지도와 자세를 기다리는 중";
        float bw = w::capsule2_width(36 * s_, 0, t);
        w::capsule2(dl, ImVec2((W - bw) / 2, 200 * s_), 36 * s_, 0, Tone::Warn, false, Lamp::Half, t);
    }

    // 왼쪽 아래: 축척 + 그리기/이동 모드
    {
        double meters = map_ppm_ >= 35 * s_ ? 1.0 : 5.0;
        float len = static_cast<float>(meters * map_ppm_), x0 = m + 4 * s_, y0 = H - m - 92 * s_;
        dl->AddLine(ImVec2(x0, y0), ImVec2(x0 + len, y0), T.text1, 2 * s_);
        dl->AddLine(ImVec2(x0, y0 - 8 * s_), ImVec2(x0, y0), T.text1, 2 * s_);
        dl->AddLine(ImVec2(x0 + len, y0 - 8 * s_), ImVec2(x0 + len, y0), T.text1, 2 * s_);
        std::string t = meters < 5 ? "1 m" : "5 m";
        dl->AddText(F.semi, sz::caption * s_, ImVec2(x0 + len + 8 * s_, y0 - 10 * s_), T.text1, t.c_str());
        static const char* modes[] = {"자동", "이동", "그리기"};
        static const char* micons[] = {ic::pin, ic::move, ic::edit};
        ImGui::SetCursorScreenPos(ImVec2(m, H - m - 48 * s_));
        w::segmented2("mapmode", modes, 3, &map_mode_, 48 * s_, micons);
    }
    // 아래 가운데: 보내기 필(미리보기 경로가 있을 때만) 또는 회피 상태
    {
        float ph = 60 * s_, y0 = H - m - ph;
        float right_limit = W - m - 128 * s_ - 24 * s_;
        if (!plan_preview_.path.empty() && !path_busy) {
            bool can = s.v2.state == "idle";
            std::string label = fmt("보내기 · %zu점 · %.1f m", plan_preview_.path.size(), polyline_length(plan_preview_.path));
            std::string sub = plan_preview_.detoured ? "벽·장애물을 피해 돌아갑니다 · 가는 중에 막히면 다시 계획" : "가는 중에 장애물이 나타나면 앱이 돌아가는 길을 다시 만듭니다";
            if (!can) sub = std::string("지금은 보낼 수 없음 · ") + state_label(s.v2.state);
            float pw = w::pill_width(label.c_str(), ph, ic::send, nullptr, sub.c_str());
            float x0 = std::min((W - pw) / 2, right_limit - pw);
            ImGui::SetCursorScreenPos(ImVec2(x0, y0));
            if (w::pill_button("send", label.c_str(), ph, can ? (plan_preview_.detoured ? Tone::Warn : Tone::Accent) : Tone::Neutral, ic::send,
                               nullptr, sub.c_str(), can)) {
                std::string err;
                if (robot_.go_path(preview_m_, &err)) {
                    pad_.rumble(Rumble::Tick);
                    stroke_m_.clear();
                    plan_preview_ = PlanResult{};
                    map_follow_ = true;
                } else {
                    toast_ = "경로 실패: " + err;
                    toast_until_ = now + 4;
                }
            }
        } else if (s.nav.phase == "blocked" || s.nav.phase == "done") {
            const std::string& t = s.nav.message;
            float bw = w::capsule2_width(44 * s_, 0, t.c_str());
            w::capsule2(dl, ImVec2(std::min((W - bw) / 2, right_limit - bw), H - m - 44 * s_), 44 * s_, 0,
                        s.nav.phase == "blocked" ? Tone::Danger : Tone::Ok, false, s.nav.phase == "blocked" ? Lamp::Cross : Lamp::Full, t.c_str());
        } else if (!path_busy && stroke_m_.empty() && have_pose) {
            const char* t = "로봇 옆에서 끌어 갈 길을 그리세요 · 벽을 지나면 앱이 돌아가는 길을 만듭니다";
            float bw = w::capsule2_width(40 * s_, 0, t);
            w::capsule2(dl, ImVec2(std::min((W - bw) / 2, right_limit - bw), H - m - 40 * s_ - 70 * s_), 40 * s_, 0, Tone::Info, false, Lamp::Ring, t);
        }
    }
}

// ---------------- 로봇 ----------------

void App::draw_robot(const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, m = 20 * s_, x0 = m + 4 * s_, right = W - m - 128 * s_ - 30 * s_;
    float y = 96 * s_;
    auto title = [&](const char* t, const char* hint, Pad p) {
        dl->AddText(F.bold, sz::title * s_, ImVec2(x0, y), T.text1, t);
        float tw = w::text_size(F.bold, sz::title, t).x;
        dl->AddText(F.body, sz::caption * s_, ImVec2(x0 + tw + 14 * s_, y + 8 * s_), T.text3, hint);
        w::pad_glyph(dl, p, ImVec2(x0 + tw + 14 * s_ + w::text_size(F.body, sz::caption, hint).x + 22 * s_, y + 16 * s_), 16);
        y += 44 * s_;
    };
    title("자세", "누르면 바로 바뀝니다", Pad::DpadUD);
    {  // 자세 7개 = 한 줄 필
        float x = x0;
        for (size_t i = 0; i < pose_presets().size(); ++i) {
            const PosePreset& p = pose_presets()[i];
            float pw = w::pill_width(p.label, 52 * s_);
            if (x + pw > right) {
                x = x0;
                y += 52 * s_ + 12 * s_;
            }
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (w::pill_button(p.name, p.label, 52 * s_, Tone::Neutral, nullptr, nullptr, nullptr, true, 0, pose_index_ == static_cast<int>(i))) {
                pose_index_ = static_cast<int>(i);
                robot_.set_pose(p.name);
            }
            x += pw + 12 * s_;
        }
        y += 52 * s_ + 26 * s_;
    }
    title("동작", "◀▶ 고르기 · LB 실행 · 실행 중에는 이동이 막힙니다", Pad::LB);
    double now = now_s();
    auto flow = [&](bool caution) {
        float x = x0;
        for (size_t i = 0; i < action_groups().size(); ++i) {
            const ActionGroup& a = action_groups()[i];
            if (a.caution != caution) continue;
            bool armed = caution_action_ == a.name && now < caution_armed_until_;
            std::string label = armed ? std::string("다시 눌러 실행 · ") + a.label : std::string(a.label);
            float pw = w::pill_width(label.c_str(), 48 * s_, caution && !armed ? ic::warning : nullptr);
            if (x + pw > right) {
                x = x0;
                y += 48 * s_ + 10 * s_;
            }
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImVec2 p0(x, y);
            // 큰 동작은 글자만 amber(옅은 바탕), 무장되면 채움
            if (caution && !armed) dl->AddRectFilled(p0, ImVec2(p0.x + pw, p0.y + 48 * s_), theme::alpha(T.warn, 0.10f), 24 * s_);
            bool sel = action_index_ == static_cast<int>(i);
            bool hit = w::pill_button(a.name, label.c_str(), 48 * s_, armed ? Tone::Warn : Tone::Neutral, caution && !armed ? ic::warning : nullptr,
                                      nullptr, nullptr, true, 0, sel, caution && !armed ? T.warn : 0);
            if (armed) {  // 남은 시간 링
                float k = static_cast<float>((caution_armed_until_ - now) / 2.0);
                ImVec2 c(p0.x + pw - 20 * s_, p0.y + 24 * s_);
                dl->PathArcTo(c, 9 * s_, -IM_PI / 2, -IM_PI / 2 + 2 * IM_PI * k, 24);
                dl->PathStroke(T.ink, 3 * s_);
            }
            if (hit) {
                action_index_ = static_cast<int>(i);
                if (a.caution && !armed) {
                    caution_action_ = a.name;
                    caution_armed_until_ = now + 2.0;
                } else {
                    caution_action_.clear();
                    robot_.run_action(a.name);
                    pad_.rumble(Rumble::Tick);
                }
            }
            x += pw + 10 * s_;
        }
        y += 48 * s_ + 22 * s_;
    };
    dl->AddText(F.body, sz::caption * s_, ImVec2(x0, y), T.text3, "기본");
    y += 24 * s_;
    flow(false);
    dl->AddText(F.body, sz::caption * s_, ImVec2(x0, y), T.warn, "큰 동작 · 두 번 눌러 실행");
    y += 24 * s_;
    flow(true);
    ImGui::SetCursorScreenPos(ImVec2(x0, y));
    if (w::pill_button("customsheet", "직접 자세 만들기", 48 * s_, Tone::Neutral, ic::sliders)) sheet_pose_ = true;
    const Json& a = s.arbiter;
    std::string st = fmt("조종 %s", state_label(s.v2.state));
    if (a.is_object()) {
        std::string act = a["action"].is_string() ? a["action"].as_string() : "";
        st += " · 동작 " + (act.empty() ? std::string("없음") : act) + " · 엔진 " + a["engine"].text();
    }
    dl->AddText(F.body, sz::caption * s_, ImVec2(x0 + w::pill_width("직접 자세 만들기", 48 * s_, ic::sliders) + 20 * s_, y + 16 * s_), T.text3, st.c_str());
}

void App::draw_pose_sheet(const RobotSnapshot& s) {
    (void)s;
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y, sh = 440 * s_, m = 20 * s_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // 바깥 스크림(누르면 닫힘)
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), theme::alpha(T.bg0, 0.4f));
    ImGui::SetCursorScreenPos(ImVec2(0, 0));
    if (ImGui::InvisibleButton("sheetscrim", ImVec2(W, H - sh))) sheet_pose_ = false;
    float x0 = m, x1 = W - m - 128 * s_ - 24 * s_, y0 = H - sh;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, H), T.bg1, 16 * s_, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2((x0 + x1) / 2 - 20 * s_, y0 + 10 * s_), ImVec2((x0 + x1) / 2 + 20 * s_, y0 + 14 * s_), T.line, 2 * s_);
    dl->AddText(F.bold, sz::title * s_, ImVec2(x0 + 24 * s_, y0 + 24 * s_), T.text1, "직접 자세 만들기");
    float colw = (x1 - x0 - 72 * s_) / 2;
    struct S {
        const char* label;
        float* v;
        float lo, hi;
        const char* f;
    } sl[] = {{"roll (°)", &pose_roll_, -20, 20, "%.0f"},       {"pitch (°)", &pose_pitch_, -30, 20, "%.0f"},
              {"yaw (°)", &pose_yaw_, -20, 20, "%.0f"},         {"height (cm)", &pose_height_, -12, -5, "%.1f"},
              {"x_shift (cm)", &pose_xshift_, -3, 3, "%.1f"},   {"stance_x (cm)", &pose_sx_, -2, 2, "%.1f"},
              {"stance_y (cm)", &pose_sy_, -2, 2, "%.1f"}};
    for (int i = 0; i < 7; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x0 + 24 * s_ + (i % 2) * (colw + 24 * s_), y0 + 70 * s_ + (i / 2) * 70 * s_));
        w::slider2(sl[i].label, sl[i].label, sl[i].v, sl[i].lo, sl[i].hi, sl[i].f, colw);
    }
    float rt = static_cast<float>(pose_time_);
    ImGui::SetCursorScreenPos(ImVec2(x0 + 24 * s_ + colw + 24 * s_, y0 + 70 * s_ + 3 * 70 * s_));
    if (w::slider2("runtime", "run_time (ms)", &rt, 200, 2000, "%.0f", colw)) pose_time_ = static_cast<int>(rt);
    ImGui::SetCursorScreenPos(ImVec2(x0 + 24 * s_, H - 72 * s_));
    if (w::pill_button("applypose", "이 자세로", 52 * s_, Tone::Accent, ic::play))
        robot_.set_pose_custom(pose_roll_ * kDeg, pose_pitch_ * kDeg, pose_yaw_ * kDeg, pose_height_, pose_xshift_, pose_sx_, pose_sy_, pose_time_);
    ImGui::SameLine(0, 12 * s_);
    if (w::pill_button("resetpose", "서기 값으로", 52 * s_, Tone::Neutral, ic::refresh)) {
        pose_roll_ = pose_pitch_ = pose_yaw_ = 0;
        pose_height_ = -10;
        pose_xshift_ = -0.5f;
        pose_sx_ = pose_sy_ = 0;
    }
    ImGui::SameLine(0, 12 * s_);
    if (w::pill_button("closesheet", "닫기", 52 * s_, Tone::Neutral, ic::cancel)) sheet_pose_ = false;
}

// ---------------- 로그 ----------------

void App::draw_log(double now, const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y, m = 20 * s_, x0 = m + 8 * s_;
    float right = W - m - 128 * s_ - 30 * s_;
    const V2Snapshot& v = s.v2;
    std::vector<float> sorted = rtt_hist_;
    std::sort(sorted.begin(), sorted.end());
    float med = sorted.empty() ? -1 : sorted[sorted.size() / 2];
    float p95 = sorted.empty() ? -1 : sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)];
    double age = cam_tex_.valid() ? (now - cam_tex_.rx_time) * 1000 : -1;
    float colw = (right - x0) / 4, y = 100 * s_;
    w::big_number(dl, ImVec2(x0, y), med < 0 ? "—" : fmt("%.1f", med).c_str(), "ms", "ACK RTT",
                  p95 < 0 ? "" : fmt("p95 %.1f · n %zu", p95, sorted.size()).c_str(), med > 100 ? Tone::Warn : Tone::Neutral);
    w::big_number(dl, ImVec2(x0 + colw, y), fmt("%.1f", v.loss_pct).c_str(), "%", "유실",
                  fmt("%llu/%llu", (unsigned long long)v.tx, (unsigned long long)v.rx).c_str(), v.loss_pct > 5 ? Tone::Warn : Tone::Neutral);
    w::big_number(dl, ImVec2(x0 + colw * 2, y), fmt("%.1f", robot_.camera_fps()).c_str(), "fps", "카메라",
                  age < 0 ? "프레임 없음" : fmt("나이 %.0f ms", age).c_str(), Tone::Neutral);
    w::big_number(dl, ImVec2(x0 + colw * 3, y), fmt("%.0f", io.Framerate).c_str(), "fps", "화면",
                  fmt("%.1f ms", 1000.0f / std::max(1.0f, io.Framerate)).c_str(), Tone::Neutral);
    y += 92 * s_;
    dl->AddLine(ImVec2(x0, y), ImVec2(right, y), T.line, 1.0f);
    y += 14 * s_;
    float vmax = std::max(20.0f, sorted.empty() ? 20.0f : sorted.back() * 1.2f);
    w::sparkline(dl, ImVec2(x0, y + 10 * s_), ImVec2(right, y + 110 * s_), rtt_hist_.data(), static_cast<int>(rtt_hist_.size()), vmax, 100.0f);
    if (!sorted.empty()) {
        std::string sm = fmt("ACK RTT · 중앙 %.1f · p95 %.1f · 최대 %.1f ms · 점선 100 ms", med, p95, sorted.back());
        dl->AddText(F.body, sz::caption * s_, ImVec2(x0, y + 116 * s_), T.text3, sm.c_str());
    }
    y += 150 * s_;
    // 왼쪽: 연결 키-값, 오른쪽: 이벤트 콘솔
    float kx = x0, ky = y + 6 * s_, kvw = 360 * s_;
    auto kv = [&](const char* k, const std::string& val, ImU32 col = 0) {
        dl->AddText(F.body, sz::caption * s_, ImVec2(kx, ky + 2 * s_), T.text3, k);
        dl->AddText(F.semi, sz::label * s_, ImVec2(kx + 96 * s_, ky), col ? col : T.text1, val.c_str());
        ky += 30 * s_;
    };
    kv("조종", privacy_ ? "로봇:5001" : v.robot);
    kv("client", v.client_id.substr(0, 8) + fmt(" · seq %lld", static_cast<long long>(v.sequence)));
    kv("rosbridge", s.rb_status, s.rb_connected ? T.ok : T.danger);
    kv("지도", s.grid ? fmt("%d×%d · %.2f m · %s", s.grid->w, s.grid->h, s.grid->res, s.grid_png ? "PNG" : "JSON") : "없음");
    kv("스캔", fmt("%zu빔", s.scan.ranges.size()));
    if (s.arbiter.is_object()) {
        const Json& a = s.arbiter;
        kv("중재기", std::string("owner ") + (a["owner"].is_null() ? "—" : a["owner"].text()) + " · estop " + (a["estop"].as_bool() ? "ON" : "off"));
        kv("엔진", a["engine"].text() + " · 동작 " + (a["action"].is_null() ? "—" : a["action"].text()));
    }
    kv("회피", s.nav.phase + (s.nav.replans ? fmt(" · 우회 %d회", s.nav.replans) : ""));
    // 콘솔 + 액션 필
    float cx0 = x0 + kvw + 20 * s_, cy0 = y, cy1 = H - m - 8 * s_;
    bool sniff = robot_.packet_capture();
    {
        float ax = right;
        auto act = [&](const char* id, const char* label, const char* icon, bool sel) {
            float pw = w::pill_width(label, 40 * s_, icon);
            ax -= pw;
            ImGui::SetCursorScreenPos(ImVec2(ax, cy0));
            bool r = w::pill_button(id, label, 40 * s_, Tone::Neutral, icon, nullptr, nullptr, true, 0, sel);
            ax -= 10 * s_;
            return r;
        };
        if (act("rttreset", "RTT 초기화", ic::refresh, false)) rtt_hist_.clear();
        if (act("topics", "토픽 목록", ic::list, false)) robot_.list_topics();
        if (act("sniff", sniff ? "패킷 기록 중" : "패킷 기록", ic::record, sniff)) {
            robot_.set_packet_capture(!sniff);
            cfg_.sniff = !sniff;
            save_settings();
        }
        dl->AddText(F.bold, sz::title * s_, ImVec2(cx0, cy0 + 4 * s_), T.text1, "이벤트");
    }
    cy0 += 52 * s_;
    dl->AddRectFilled(ImVec2(cx0, cy0), ImVec2(right, cy1), T.bg2, 16 * s_);
    ImGui::SetCursorScreenPos(ImVec2(cx0 + 12 * s_, cy0 + 10 * s_));
    ImGui::PushFont(F.num, sz::label);
    ImGui::BeginChild("console", ImVec2(right - cx0 - 24 * s_, cy1 - cy0 - 20 * s_), 0, 0);
    auto lines = sniff ? robot_.packets().tail(200) : robot_.events().tail(200);
    for (const auto& raw : lines) {
        std::string l = masked(raw);
        ImVec2 c = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + 4 * s_, c.y + 10 * s_), 3 * s_, w::tone_color(event_tone(l)), 12);
        ImGui::SetCursorScreenPos(ImVec2(c.x + 16 * s_, c.y));
        if (l.size() > 13) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T.text3));
            ImGui::TextUnformatted(l.substr(0, 12).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 10 * s_);
            ImGui::TextUnformatted(l.substr(13).c_str());
        } else {
            ImGui::TextUnformatted(l.c_str());
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopFont();
}

// ---------------- 연결·설정 ----------------

void App::draw_connect(const RobotSnapshot& s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y, m = 20 * s_;
    float right = W - m - 128 * s_ - 30 * s_, x0 = m + 8 * s_;
    static const char* tabs[] = {"연결", "설정"};
    static const char* ticons[] = {ic::wifi, ic::settings};
    ImGui::SetCursorScreenPos(ImVec2(x0, 92 * s_));
    w::segmented2("ctab", tabs, 2, &connect_tab_, 44 * s_, ticons);
    float y = 160 * s_;
    if (connect_tab_ == 0) {
        float colw = (right - x0 - 40 * s_) / 2;
        // 왼쪽: 로봇 찾기
        dl->AddText(F.bold, sz::title * s_, ImVec2(x0, y), T.text1, "로봇 찾기");
        bool busy = discovering_.valid() && discovering_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        if (discovering_.valid() && !busy) {
            found_ = discovering_.get();
            discovered_once_ = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, y + 44 * s_));
        if (w::pill_button("discover", busy ? "찾는 중…" : "같은 Wi-Fi 에서 찾기", 56 * s_, Tone::Neutral, ic::search, nullptr, nullptr, !busy))
            discovering_ = std::async(std::launch::async, [] { return discover_robots(1500); });
        float ry = y + 116 * s_;
        for (const auto& f : found_) {
            dl->AddRectFilled(ImVec2(x0, ry), ImVec2(x0 + colw, ry + 64 * s_), T.bg1, 16 * s_);
            dl->AddCircleFilled(ImVec2(x0 + 24 * s_, ry + 32 * s_), 6 * s_, T.ok, 16);
            dl->AddText(F.semi, sz::headline * s_, ImVec2(x0 + 42 * s_, ry + 8 * s_), T.text1, f.name.c_str());
            dl->AddText(F.body, sz::caption * s_, ImVec2(x0 + 42 * s_, ry + 36 * s_), T.text2, privacy_ ? "192.168.x.x" : f.ip.c_str());
            float pw = w::pill_width("연결", 44 * s_);
            ImGui::SetCursorScreenPos(ImVec2(x0 + colw - pw - 10 * s_, ry + 10 * s_));
            if (w::pill_button(("c" + f.id).c_str(), "연결", 44 * s_, Tone::Accent)) connect_to(f.ip);
            ry += 76 * s_;
        }
        if (discovered_once_ && found_.empty())
            dl->AddText(F.body, sz::body * s_, ImVec2(x0, ry), T.warn, "응답한 로봇이 없습니다. 같은 Wi-Fi 인지 확인하세요.");
        std::string mine;
        for (const auto& ip : local_ipv4_addresses()) mine += (privacy_ ? std::string("x.x.x.x") : ip) + "  ";
        dl->AddText(F.body, sz::caption * s_, ImVec2(x0, H - m - 30 * s_), T.text3, ("이 기기  " + mine).c_str());
        // 오른쪽: 주소 직접 입력(숫자 패드 — 흑갈리에는 실물 키보드가 없다)
        float kx = x0 + colw + 40 * s_;
        dl->AddText(F.bold, sz::title * s_, ImVec2(kx, y), T.text1, "주소 직접 입력");
        std::string shown = (privacy_ && !ip_entry_.empty() ? std::string("192.168.x.x") : ip_entry_) + ((static_cast<int>(now_s() * 2) % 2) ? "_" : " ");
        w::num_text(dl, F.semi, sz::display, ImVec2(kx, y + 40 * s_), T.text1, shown.c_str());
        const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "←"};
        float kw = (colw - 2 * 12 * s_) / 3, kh = 60 * s_, ky = y + 110 * s_;
        for (int i = 0; i < 12; ++i) {
            ImVec2 p0(kx + (i % 3) * (kw + 12 * s_), ky + (i / 3) * (kh + 10 * s_));
            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton((std::string("k") + std::to_string(i)).c_str(), ImVec2(kw, kh));
            bool act = ImGui::IsItemActive();
            if (ImGui::IsItemClicked()) {
                if (i == 11) {
                    if (!ip_entry_.empty()) ip_entry_.pop_back();
                } else if (ip_entry_.size() < 15) {
                    ip_entry_ += keys[i];
                }
            }
            dl->AddRectFilled(p0, ImVec2(p0.x + kw, p0.y + kh), act ? T.ctl_down : T.ctl, 16 * s_);
            ImVec2 ts = w::text_size(F.semi, sz::title, keys[i]);
            dl->AddText(F.semi, sz::title * s_, ImVec2(p0.x + (kw - ts.x) / 2, p0.y + (kh - ts.y) / 2), T.text1, keys[i]);
        }
        float by = ky + 4 * (kh + 10 * s_) + 6 * s_;
        ImGui::SetCursorScreenPos(ImVec2(kx, by));
        if (w::pill_button("ipclear", "지우기", 52 * s_, Tone::Neutral, ic::erase)) ip_entry_.clear();
        ImGui::SameLine(0, 10 * s_);
        if (w::pill_button("ipconnect", "연결", 52 * s_, Tone::Accent, ic::wifi, nullptr, nullptr, !ip_entry_.empty())) connect_to(ip_entry_);
        ImGui::SameLine(0, 10 * s_);
        if (w::pill_button("ipdisc", "끊기", 52 * s_, Tone::Neutral, ic::cancel, nullptr, nullptr, s.connected)) robot_.disconnect();
    } else {
        float cw = std::min(right - x0, 760 * s_);
        bool changed = false;
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        changed |= w::toggle2("t_single", "왼쪽 스틱 하나로 운전(전후 + 회전)", &cfg_.single_stick, cw);
        ImGui::SetCursorScreenPos(ImVec2(x0, y + 56 * s_));
        changed |= w::toggle2("t_tilt", "기울여서 운전(RB 를 쥔 동안, 쥐는 순간 자세가 0)", &cfg_.tilt_enabled, cw);
        float yy = y + 112 * s_;
        if (cfg_.tilt_enabled) {
            ImGui::SetCursorScreenPos(ImVec2(x0 + 24 * s_, yy));
            changed |= w::toggle2("t_tf", "앞뒤 반대로", &cfg_.tilt_invert_forward, cw - 24 * s_);
            ImGui::SetCursorScreenPos(ImVec2(x0 + 24 * s_, yy + 56 * s_));
            changed |= w::toggle2("t_tt", "좌우 반대로", &cfg_.tilt_invert_turn, cw - 24 * s_);
            dl->AddText(F.body, sz::caption * s_, ImVec2(x0 + 24 * s_, yy + 112 * s_), T.text3, ("가속도계: " + tilt_.status()).c_str());
            tilt_drive_.invert_forward = cfg_.tilt_invert_forward;
            tilt_drive_.invert_turn = cfg_.tilt_invert_turn;
            yy += 140 * s_;
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, yy));
        changed |= w::toggle2("t_cam", "카메라 영상 받기(Wi-Fi 15~40 Mbps)", &cfg_.camera, cw);
        if (cfg_.camera != robot_.camera_enabled()) robot_.set_camera_enabled(cfg_.camera);
        yy += 70 * s_;
        ImGui::SetCursorScreenPos(ImVec2(x0, yy));
        if (w::slider2("s_speed", "최고 속도 비율", &cfg_.max_speed, 0.1f, 1.0f, "%.2f", cw)) {
            speed_full_ = false;
            robot_.v2().set_speed_scale(cfg_.max_speed);
            changed = true;
        }
        yy += 70 * s_;
        ImGui::SetCursorScreenPos(ImVec2(x0, yy));
        if (w::slider2("s_dz", "스틱 데드존", &cfg_.deadzone, 0.05f, 0.4f, "%.2f", cw)) {
            pad_.deadzone = cfg_.deadzone;
            changed = true;
        }
        yy += 76 * s_;
        dl->AddText(F.body, sz::caption * s_, ImVec2(x0, yy), T.text3,
                    fmt("1.00 = 로봇 상한(전진 25 cm/s, 후진 5 cm/s, 회전 0.28 rad/s) · 글꼴 %s · Dear ImGui %s", F.body_name.c_str(), IMGUI_VERSION).c_str());
        if (changed) save_settings();
    }
}

}  // namespace hg
