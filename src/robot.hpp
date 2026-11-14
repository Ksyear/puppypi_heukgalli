// 로봇 하나와의 모든 연결을 묶는다: 조종(v2), rosbridge, HTTP 상태 폴링, 카메라·지도 스트림.
// 화면은 매 프레임 snapshot() 을 한 번 복사해 읽기만 하고, 동작은 아래 메서드로 요청한다.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "net.hpp"
#include "path_tools.hpp"
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

    bool save_snapshot_jpeg(const std::string& path, std::string* err) const;

private:
    void http_loop();
    void call(const std::string& service, const std::string& type, const Json& args, const std::string& label);
    void set_result(const std::string& s);

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
};

// 아래 해석 함수는 단위 시험에서도 쓴다
bool parse_map_info(const Json& j, MapInfo& out);
bool parse_path_status(const Json& j, PathStatus& out);
bool parse_scan(const Json& msg, ScanData& out);

}  // namespace hg
