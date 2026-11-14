// 흑갈리 화면 앱: 입력(패드·터치·기울기) → 로봇, 로봇 상태 → 화면·진동.
#pragma once
#include <functional>
#include <future>
#include <string>
#include <vector>

#include "discovery.hpp"
#include "gamepad.hpp"
#include "path_tools.hpp"
#include "robot.hpp"
#include "textures.hpp"
#include "theme.hpp"
#include "tilt.hpp"
#include "widgets.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace hg {

// 화면 문구 도우미(ui.cpp)
const char* state_label(const std::string& st);
const char* reason_label(const std::string& r);
const char* path_label(const std::string& st);
std::string time_tag();

struct Settings {
    std::string last_host;
    bool single_stick = false;   // 왼쪽 스틱 하나로 전후·회전
    bool tilt_enabled = false;   // RB 를 쥔 동안 기기 기울기로 운전
    bool tilt_invert_forward = false, tilt_invert_turn = false;
    float max_speed = 0.6f;      // 로봇 상한(전진 25 cm/s, 회전 0.28 rad/s)에 곱하는 비율
    float deadzone = 0.12f;
    bool camera = true;
    bool sniff = false;
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

enum class Screen { Drive, Map, Robot, Log, Connect };

class App {
public:
    App(ID3D11Device* dev, ID3D11DeviceContext* ctx, void* hwnd, std::string exe_dir, float dpi_scale);
    ~App();
    void frame(double now);
    void set_focus(bool active);
    bool quit_requested() const { return quit_; }
    // "drive" | "map" | "robot" | "log" | "connect" (화면 저장 요청용)
    bool select_screen(const std::string& name);
    LogRing& events() { return robot_.events(); }
    void set_keyboard_drive(bool on) { kbdrive_ = on; }
    void set_privacy(bool on) { privacy_ = on; }  // 화면에 IP 를 보이지 않음(녹화·스크린샷용)

private:
    // 입력·안전
    void handle_input(double now, const RobotSnapshot& s);
    void feedback(double now, const RobotSnapshot& s);
    void estop_all();
    // 화면
    void draw(double now, const RobotSnapshot& s);
    void draw_overlay(double now, const RobotSnapshot& s);    // 나브 필·캡슐·텔레메트리·FAB·비상정지·토스트
    void draw_fabs(double now, const RobotSnapshot& s);
    void draw_pose_sheet(const RobotSnapshot& s);
    void draw_drive(double now, const RobotSnapshot& s);
    void draw_map(double now, const RobotSnapshot& s);
    void update_grid_texture(const RobotSnapshot& s);
    void draw_robot(const RobotSnapshot& s);
    void draw_log(double now, const RobotSnapshot& s);
    void draw_connect(const RobotSnapshot& s);
    void draw_radar(const RobotSnapshot& s, float cx, float cy, float radius);
    void draw_scan_points(ImDrawList* dl, const std::vector<std::pair<float, float>>& pts, const Pose2D& pose, bool stale,
                          const std::function<ImVec2(double, double)>& to_screen);
    void ensure_sprite();
    bool estop_button(double now, const RobotSnapshot& s, float w, float h);
    void connect_to(const std::string& host);
    void update_textures();
    void save_settings();

    ID3D11Device* dev_;
    ID3D11DeviceContext* ctx_;
    void* hwnd_;
    std::string exe_dir_;
    float s_;  // 화면 배율(DPI)
    Settings cfg_;
    Robot robot_;
    Gamepad pad_;
    TiltSensor tilt_;
    TiltDrive tilt_drive_;
    JpegDecoder cam_dec_;
    Texture cam_tex_;
    std::vector<uint8_t> scratch_;

    Screen screen_ = Screen::Connect;
    bool focus_ = true;
    bool quit_ = false;
    int hud_layer_ = 1;  // 0 카메라, 1 카메라+레이더, 2 레이더만
    int pose_index_ = 0, action_index_ = 0;
    double pose_publish_at_ = 0;
    double caution_armed_until_ = 0;
    std::string caution_action_;
    bool speed_full_ = false;
    double clear_hold_start_ = 0;
    double view_menu_hold_ = 0;
    // 운전 상태(표시용)
    bool held_ = false;
    double in_forward_ = 0, in_turn_ = 0;
    std::string drive_source_ = "-";
    // 진동용 이전 상태
    bool prev_estop_ = false, prev_linked_ = false, prev_pad_ = false;
    std::string prev_path_state_, prev_result_, prev_reject_;
    double last_reject_rumble_ = 0;
    // 지도 스케치
    std::vector<Pt> stroke_m_;    // 손가락으로 그린 선(지도 좌표)
    std::vector<Pt> preview_m_;   // 단순화한 경유점(m)
    PlanResult plan_preview_;     // 장애물을 피한 실제 보낼 경로
    bool drawing_ = false;
    int map_mode_ = 0;            // 0 자동(로봇 옆에서 시작하면 그리기), 1 이동, 2 그리기
    bool draw_gesture_ = false;
    bool map_follow_ = true;
    double map_cx_ = 0, map_cy_ = 0, map_ppm_ = 0;  // 보기 중심(m), 배율(px/m)
    Texture grid_tex_;
    uint64_t grid_tex_version_ = 0;
    Texture sprite_tex_;          // 스캔 점용 방사형 점(32×32)
    double map_ppm_target_ = 0;   // 확대 단계(보간 목표)
    // v2 화면 상태
    float nav_anim_x_ = 0;
    double last_draw_ = 0;
    bool sheet_pose_ = false, sheet_map_more_ = false;
    int connect_tab_ = 0;         // 0 연결, 1 설정
    bool map_hint_shown_ = false;
    bool confirm_reset_map_ = false;
    double confirm_reset_until_ = 0;
    bool events_open_ = false;
    std::string toast_seen_;
    double toast_start_ = 0;
    bool kbdrive_ = false;
    bool privacy_ = false;
    std::string masked(const std::string& line) const;  // 패드 없을 때 키보드 운전(--kbdrive, Shift 를 쥔 동안만)
    // 연결 화면
    std::future<std::vector<FoundRobot>> discovering_;
    std::vector<FoundRobot> found_;
    bool discovered_once_ = false;
    std::string ip_entry_;
    // 측정
    std::vector<float> rtt_hist_;
    uint64_t rtt_last_rx_ = 0;
    // 커스텀 자세
    float pose_roll_ = 0, pose_pitch_ = 0, pose_yaw_ = 0, pose_height_ = -10, pose_xshift_ = -0.5f;
    float pose_sx_ = 0, pose_sy_ = 0;
    int pose_time_ = 500;
    std::string toast_;
    double toast_until_ = 0;
};

}  // namespace hg
