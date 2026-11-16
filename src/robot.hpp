// 로봇 하나와의 모든 연결을 묶는다: 조종(v2), rosbridge, HTTP 상태 폴링, 카메라·지도 스트림.
// 화면은 매 프레임 snapshot() 을 한 번 복사해 읽기만 하고, 동작은 아래 메서드로 요청한다.
#pragma once
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "lidar.hpp"
#include "net.hpp"
#include "path_tools.hpp"
#include "planner.hpp"
#include "rosbridge.hpp"
#include "util.hpp"
#include "v2client.hpp"

namespace hg {

struct ScanData {
    bool valid = false;
    double angle_min = 0, angle_increment = 0, range_max = 12;
    std::vector<float> ranges;
    double rx_time = 0;
};

struct PathStatus {
    bool valid = false;
    std::string state;  // idle | rotate | drive | blocked | done | cancelled
    std::string reason;
    std::string request_client_id;
    int64_t request_sequence = -1;
    std::vector<Pt> points;
    int index = 0, count = 0;
    double remaining_m = -1, elapsed_s = -1;
    double rx_time = 0;
};

struct MapInfo {
    bool valid = false;
    MapGeometry geo;
    int64_t image_seq = -1;
    double robot_x = 0, robot_y = 0, robot_yaw = 0;
    double age_s = -1;
    double rx_time = 0;
};

// 앱이 맡는 장애물 회피 상태(로봇은 경유점만 따라간다)
struct NavStatus {
    std::string phase = "idle";  // idle | following | replanning | blocked | done
    std::string message;         // 화면 표시용 한 줄
    int replans = 0;
    bool detoured = false;
    std::vector<Pt> goals;       // 사용자가 그린 경유점
    std::vector<Pt> planned;     // 지금 로봇에 보낸 경로
    std::vector<Pt> obstacles;   // 최근 경로를 막은 점(지도 좌표, 잠시 기억)
};

struct RobotSnapshot {
    std::string host;
    bool connected = false;  // connect() 이후 disconnect() 전
    V2Snapshot v2;
    bool rb_connected = false;
    std::string rb_status;
    Json arbiter, status;    // /puppy_control/arbiter_state, /status (JSON 문자열을 풀어 둔 것)
    double arbiter_rx = 0, status_rx = 0;
    bool http_ok = false;
    double http_rx = 0;
    MapInfo map;
    PathStatus path;
    ScanData scan;
    std::string last_result;  // 마지막 서비스 호출 결과 한 줄
    bool robot_estop_requested = false;
    // LiDAR 지도
    std::shared_ptr<const OccGrid> grid;  // /map (없으면 nullptr)
    uint64_t grid_version = 0;
    double grid_rx = 0;
    bool grid_png = true;                 // PNG 압축으로 받는 중인지
    Pose2D pose;                          // /lidar_map/pose(10 Hz), 없으면 /api/map
    double pose_rx = 0;
    std::vector<Pt> trail;                // 지나온 길(지도 좌표)
    NavStatus nav;
};

class Robot {
public:
    Robot();
    ~Robot();
    bool connect(const std::string& host, std::string* err);
    void disconnect();
    std::string host() const;

    V2Client& v2() { return v2_; }
    RobotSnapshot snapshot() const;
    LogRing& events() { return events_; }
    LogRing& packets() { return packets_; }
    void set_packet_capture(bool on);
    bool packet_capture() const { return capture_; }

    // 카메라·지도 JPEG(가장 최근 1장)
    bool camera_frame(std::string& jpeg, double& rx, uint64_t& seq) const;
    bool map_frame(std::string& jpeg, double& rx, uint64_t& seq) const;
    double camera_fps() const;
    std::string camera_status() const;
    void set_camera_enabled(bool on);
    bool camera_enabled() const;

    // rosbridge 기능
    void go_home();
    void set_pose(const std::string& preset_name);
    void set_pose_custom(double roll, double pitch, double yaw, double height, double x_shift, double stance_x,
                         double stance_y, int run_time_ms);
    void run_action(const std::string& name);
    void save_map();
    void reset_map();
    void list_topics();

    // 비상정지: v2 latch + 로봇 전체 latch 를 함께 건다. 해제는 로봇 전체 → v2 순서
    void emergency_stop_all();
    void clear_emergency_all();

    // 경로: 지도 좌표(m). 성공 시 sequence
    bool send_path(const std::vector<Pt>& pts, std::string* err);
    void cancel_path();
    // 장애물을 피하는 경로: 지금 지도·스캔으로 계획해 보내고, 가는 중에 막히면 다시 계획한다
    PlanResult preview_plan(const std::vector<Pt>& waypoints) const;
    bool go_path(const std::vector<Pt>& waypoints, std::string* err);
    void stop_nav(const std::string& why);
    PlanConfig plan_config;

    bool save_snapshot_jpeg(const std::string& path, std::string* err) const;

private:
    void http_loop();
    void nav_loop();
    std::vector<Pt> live_obstacles_locked(double now) const;  // 지금 스캔 점 + 최근 막힘 점
    void call(const std::string& service, const std::string& type, const Json& args, const std::string& label);
    void set_result(const std::string& s);
    // 비상정지(v2 latch·요청 또는 로봇 전체 정지 요청) 중이면 true 를 돌려주고 결과 줄에 알린다
    bool estop_blocks(const std::string& what);

    mutable std::mutex mu_;
    std::string host_;
    bool connected_ = false;
    V2Client v2_;
    std::unique_ptr<RosBridge> rb_;
    std::unique_ptr<MjpegReader> camera_, map_img_;
    bool camera_enabled_ = true;
    std::thread http_th_;
    std::atomic<bool> http_run_{false};
    std::atomic<bool> capture_{false};
    LogRing events_{300}, packets_{600};
    // 수신 상태
    Json arbiter_, status_;
    double arbiter_rx_ = 0, status_rx_ = 0;
    bool http_ok_ = false;
    double http_rx_ = 0;
    MapInfo map_;
    PathStatus path_;
    ScanData scan_;
    std::string last_result_;
    bool robot_estop_requested_ = false;
    // LiDAR
    std::shared_ptr<const OccGrid> grid_;
    uint64_t grid_version_ = 0;
    double grid_rx_ = 0, rb_connected_since_ = 0;
    bool grid_png_ = true;
    Pose2D pose_;
    double pose_rx_ = 0;
    std::vector<Pt> trail_;
    // 회피
    std::thread nav_th_;
    std::atomic<bool> nav_run_{false};
    NavStatus nav_;
    std::vector<int> nav_goal_at_;
    int64_t nav_seq_ = -1;
    double nav_own_cancel_until_ = 0, nav_last_scan_ = 0;
    BlockTrigger nav_trigger_;
    std::vector<std::pair<Pt, double>> nav_temp_;  // (점, 만료 시각)
    uint64_t nav_gen_ = 0;    // 회피 주행 세대: 새 경로·멈춤마다 1 증가. nav 스레드는 세대가 같을 때만 결과를 쓰고 보낸다
    std::mutex nav_send_mu_;  // nav 스레드의 "보낼지 판단 + path 송신"과 사용자 멈춤·새 경로를 한 줄로 세운다
    std::vector<Pt> nav_pending_path_;
    double nav_pending_since_ = 0;
};

// 아래 해석 함수는 단위 시험에서도 쓴다
bool parse_map_info(const Json& j, MapInfo& out);
bool parse_path_status(const Json& j, PathStatus& out);
bool parse_scan(const Json& msg, ScanData& out);

}  // namespace hg
