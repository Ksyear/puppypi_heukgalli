#include "robot.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace hg {

namespace {
constexpr uint16_t kV2Port = 5001, kVideoPort = 8080, kMediaPort = 8090, kRosbridgePort = 9090;
const char* kCameraPath = "/stream?topic=/usb_cam/image_raw&type=ros_compressed";
}  // namespace

bool parse_map_info(const Json& j, MapInfo& out) {
    if (!j.is_object() || !j["width_px"].is_number() || !j["px_per_m"].is_number()) return false;
    out.geo.width_px = static_cast<int>(j["width_px"].as_int());
    out.geo.height_px = static_cast<int>(j["height_px"].as_int());
    out.geo.left_m = j["left_m"].as_number();
    out.geo.bottom_m = j["bottom_m"].as_number();
    out.geo.px_per_m = j["px_per_m"].as_number();
    out.image_seq = j["image_seq"].as_int(-1);
    out.age_s = j["age_s"].as_number(-1);
    const Json& r = j["robot"];
    out.robot_x = r["x_m"].as_number();
    out.robot_y = r["y_m"].as_number();
    out.robot_yaw = r["yaw_rad"].as_number();
    out.valid = out.geo.valid();
    return out.valid;
}

bool parse_path_status(const Json& j, PathStatus& out) {
    const Json& p = j.has("path") ? j["path"] : j;  // media_server 는 {"path": {...}} 로 준다
    if (!p.is_object() || !p["state"].is_string()) return false;
    out.state = p["state"].as_string();
    out.reason = p["reason"].as_string();
    out.request_client_id = p["request_client_id"].as_string();
    out.request_sequence = p["request_sequence"].is_number() ? p["request_sequence"].as_int() : -1;
    out.points.clear();
    for (const auto& xy : p["points"].as_array())
        if (xy.size() == 2) out.points.emplace_back(xy[0].as_number(), xy[1].as_number());
    out.index = static_cast<int>(p["index"].as_int());
    out.count = static_cast<int>(p["count"].as_int());
    out.remaining_m = p["remaining_m"].is_number() ? p["remaining_m"].as_number() : -1;
    out.elapsed_s = p["elapsed_s"].is_number() ? p["elapsed_s"].as_number() : -1;
    out.valid = true;
    return true;
}

bool parse_scan(const Json& msg, ScanData& out) {
    if (!msg["ranges"].is_array() || msg["ranges"].size() > 4096) return false;  // LD06 은 450빔
    out.angle_min = msg["angle_min"].as_number();
    out.angle_increment = msg["angle_increment"].as_number();
    out.range_max = msg["range_max"].as_number(12);
    out.ranges.clear();
    out.ranges.reserve(msg["ranges"].size());
    for (const auto& r : msg["ranges"].as_array())
        out.ranges.push_back(r.is_number() ? static_cast<float>(r.as_number()) : -1.0f);  // null = inf/nan
    out.valid = out.angle_increment != 0;
    return out.valid;
}

Robot::Robot() {
    v2_.set_log([this](const std::string& s) { events_.add(s); });
    v2_.set_packet_log([this](const std::string& s) {
        if (capture_) packets_.add(s);
    });
}

Robot::~Robot() { disconnect(); }

std::string Robot::host() const {
    std::lock_guard<std::mutex> lk(mu_);
    return host_;
}

void Robot::set_packet_capture(bool on) { capture_ = on; }

bool Robot::connect(const std::string& host, std::string* err) {
    disconnect();
    if (!v2_.start(host, kV2Port, err)) return false;
    auto rb = std::make_unique<RosBridge>(host, kRosbridgePort);
    rb->advertise("/puppy_control/pose", "puppy_control/Pose");
    // throttle 을 쓰지 않는다: rosbridge 는 throttle 창 안의 메시지를 버려 비상정지 같은 변화가 늦게 보인다(둘 다 2 Hz 이하)
    rb->subscribe("/puppy_control/arbiter_state", "std_msgs/String", [this](const Json& m) {
        Json body;
        try {
            body = Json::parse(m["data"].as_string());
        } catch (const std::exception&) {
            return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        arbiter_ = body;
        arbiter_rx_ = now_s();
    });
    rb->subscribe("/puppy_control/status", "std_msgs/String", [this](const Json& m) {
        Json body;
        try {
            body = Json::parse(m["data"].as_string());
        } catch (const std::exception&) {
            return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        status_ = body;
        status_rx_ = now_s();
    });
    // LiDAR 지도(600×600 격자, 2 s 마다). PNG 압축으로 받고, 안 오면 nav_loop 가 일반 JSON 으로 바꾼다
    rb->subscribe_raw("/map", "nav_msgs/OccupancyGrid", [this](const std::string& text) {
        auto g = std::make_shared<OccGrid>();
        if (!parse_occupancy_grid(text, *g)) return;
        std::lock_guard<std::mutex> lk(mu_);
        grid_ = std::move(g);
        ++grid_version_;
        grid_rx_ = now_s();
    }, 0, "png");
    rb->subscribe("/lidar_map/pose", "geometry_msgs/PoseStamped", [this](const Json& m) {
        Pose2D p;
        if (!parse_pose_stamped(m, p)) return;
        std::lock_guard<std::mutex> lk(mu_);
        pose_ = p;
        pose_rx_ = now_s();
        if (trail_.empty() || std::hypot(trail_.back().first - p.x, trail_.back().second - p.y) > 0.03) {
            trail_.emplace_back(p.x, p.y);
            if (trail_.size() > 4000) trail_.erase(trail_.begin(), trail_.begin() + 1000);
        }
    }, 100);
    rb->subscribe("/scan", "sensor_msgs/LaserScan", [this](const Json& m) {
        ScanData s;
        if (!parse_scan(m, s)) return;
        s.rx_time = now_s();
        std::lock_guard<std::mutex> lk(mu_);
        scan_ = std::move(s);
    }, 100);
    rb->start();
    // 지도는 이제 /map 격자를 앱이 직접 그린다. 로봇이 지도 JPEG 를 만들지 않도록 그 스트림은 열지 않는다
    std::unique_ptr<MjpegReader> map_img;
    std::unique_ptr<MjpegReader> cam;
    {
        std::lock_guard<std::mutex> lk(mu_);
        host_ = host;
        connected_ = true;
        rb_ = std::move(rb);
        map_img_ = std::move(map_img);
        if (camera_enabled_) {
            camera_ = std::make_unique<MjpegReader>(host, kVideoPort, kCameraPath);
            camera_->start();
        }
        map_ = MapInfo{};
        path_ = PathStatus{};
        scan_ = ScanData{};
        arbiter_ = status_ = Json();
        robot_estop_requested_ = false;
        grid_.reset();
        grid_version_ = 0;
        grid_rx_ = pose_rx_ = 0;
        grid_png_ = true;
        rb_connected_since_ = 0;
        trail_.clear();
        nav_ = NavStatus{};
        nav_temp_.clear();
    }
    http_run_ = true;
    http_th_ = std::thread([this] { http_loop(); });
    nav_run_ = true;
    nav_th_ = std::thread([this] { nav_loop(); });
    events_.add("연결: " + host);
    return true;
}

void Robot::disconnect() {
    bool was;
    {
        std::lock_guard<std::mutex> lk(mu_);
        was = connected_;
        connected_ = false;
    }
    if (!was) return;
    nav_run_ = false;
    if (nav_th_.joinable()) nav_th_.join();
    v2_.stop();
    http_run_ = false;
    if (http_th_.joinable()) http_th_.join();
    std::unique_ptr<RosBridge> rb;
    std::unique_ptr<MjpegReader> cam, map_img;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rb = std::move(rb_);
        cam = std::move(camera_);
        map_img = std::move(map_img_);
    }
    // 소멸자가 스레드를 멈춘다(콜백이 mu_ 를 잡을 수 있으니 잠금 밖에서)
    rb.reset();
    cam.reset();
    map_img.reset();
    events_.add("연결 끊음");
}

void Robot::http_loop() {
    double next_status = 0, next_path = 0, next_map = 0;
    while (http_run_) {
        double t = now_s();
        std::string host = this->host();
        if (t >= next_path) {
            // 경로 진행 중에는 4 Hz, 아니면 1 Hz
            bool moving;
            {
                std::lock_guard<std::mutex> lk(mu_);
                moving = path_.state == "rotate" || path_.state == "drive" || path_.state == "blocked";
            }
            next_path = t + (moving ? 0.25 : 1.0);
            HttpResponse r = http_get(host, kMediaPort, "/api/path", 800);
            if (r.status == 200) {
                try {
                    PathStatus ps;
                    if (parse_path_status(Json::parse(r.body), ps)) {
                        ps.rx_time = now_s();
                        std::lock_guard<std::mutex> lk(mu_);
                        if (ps.state != path_.state && path_.valid)
                            events_.add("경로 " + path_.state + " → " + ps.state + (ps.reason.empty() ? "" : " (" + ps.reason + ")"));
                        path_ = std::move(ps);
                    }
                } catch (const std::exception&) {
                }
            }
        }
        if (t >= next_map) {
            next_map = t + 1.0;
            HttpResponse r = http_get(host, kMediaPort, "/api/map", 800);
            std::lock_guard<std::mutex> lk(mu_);
            http_ok_ = r.status == 200 || r.status == 503;  // 503 = 아직 지도 없음(서버는 살아 있음)
            if (http_ok_) http_rx_ = now_s();
            if (r.status == 200) {
                try {
                    MapInfo mi;
                    if (parse_map_info(Json::parse(r.body), mi)) {
                        mi.rx_time = now_s();
                        map_ = mi;
                        if (now_s() - pose_rx_ > 1.5) {  // 10 Hz 자세가 없으면 1 Hz 값으로 대신
                            pose_ = Pose2D{mi.robot_x, mi.robot_y, mi.robot_yaw};
                            pose_rx_ = mi.rx_time;
                        }
                    }
                } catch (const std::exception&) {
                }
            }
        }
        if (t >= next_status) {
            next_status = t + 0.5;
            bool need;
            {
                std::lock_guard<std::mutex> lk(mu_);
                need = !rb_ || !rb_->connected();  // rosbridge 가 끊겼을 때만 대체 상태원으로 쓴다
            }
            if (need) {
                HttpResponse r = http_get(host, kMediaPort, "/api/status", 800);
                if (r.status == 200) {
                    try {
                        Json j = Json::parse(r.body);
                        std::lock_guard<std::mutex> lk(mu_);
                        if (j["arbiter"].is_object()) {
                            arbiter_ = j["arbiter"];
                            arbiter_rx_ = now_s();
                        }
                    } catch (const std::exception&) {
                    }
                }
            }
        }
        for (int i = 0; i < 5 && http_run_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

RobotSnapshot Robot::snapshot() const {
    RobotSnapshot s;
    s.v2 = v2_.snapshot();
    std::lock_guard<std::mutex> lk(mu_);
    s.host = host_;
    s.connected = connected_;
    s.rb_connected = rb_ && rb_->connected();
    s.rb_status = rb_ ? rb_->status() : "off";
    s.arbiter = arbiter_;
    s.status = status_;
    s.arbiter_rx = arbiter_rx_;
    s.status_rx = status_rx_;
    s.http_ok = http_ok_ && now_s() - http_rx_ < 3.0;
    s.http_rx = http_rx_;
    s.map = map_;
    s.path = path_;
    s.scan = scan_;
    s.last_result = last_result_;
    s.robot_estop_requested = robot_estop_requested_;
    s.grid = grid_;
    s.grid_version = grid_version_;
    s.grid_rx = grid_rx_;
    s.grid_png = grid_png_;
    s.pose = pose_;
    s.pose_rx = pose_rx_;
    s.trail = trail_;
    s.nav = nav_;
    return s;
}

bool Robot::camera_frame(std::string& jpeg, double& rx, uint64_t& seq) const {
    std::lock_guard<std::mutex> lk(mu_);
    return camera_ && camera_->latest(jpeg, rx, seq);
}

bool Robot::map_frame(std::string& jpeg, double& rx, uint64_t& seq) const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_img_ && map_img_->latest(jpeg, rx, seq);
}

double Robot::camera_fps() const {
    std::lock_guard<std::mutex> lk(mu_);
    return camera_ ? camera_->fps() : 0;
}

std::string Robot::camera_status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return camera_ ? camera_->status() : "off";
}

void Robot::set_camera_enabled(bool on) {
    std::unique_ptr<MjpegReader> old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        camera_enabled_ = on;
        if (!on) {
            old = std::move(camera_);
        } else if (!camera_ && connected_) {
            camera_ = std::make_unique<MjpegReader>(host_, kVideoPort, kCameraPath);
            camera_->start();
        }
    }
    old.reset();
}

bool Robot::camera_enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return camera_enabled_;
}

void Robot::set_result(const std::string& s) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_result_ = s;
    }
    events_.add(s);
}

void Robot::call(const std::string& service, const std::string& type, const Json& args, const std::string& label) {
    RosBridge* rb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rb = rb_.get();
    }
    if (!rb) {
        set_result(label + ": 연결 안 됨");
        return;
    }
    double t0 = now_s();
    rb->call(service, type, args, [this, label, t0](bool ok, const Json& values) {
        std::string msg = values["message"].as_string();
        set_result(label + (ok ? " 완료" : " 실패") + fmt(" (%.0f ms)", (now_s() - t0) * 1000) + (msg.empty() ? "" : ": " + msg));
    });
}

// 비상정지 중에는 몸을 움직이는 요청(서기·자세·동작·경로)을 보내지 않는다. 실기(2026-11-16)에서 로봇이 비상정지 중에도
// 서기를 "완료"(1 s 넘게 걸림)로 받았다. 비상정지는 사용자가 해제할 때까지 앱이 지킨다
bool Robot::estop_blocks(const std::string& what) {
    bool blocked = v2_.snapshot().estop;
    {
        std::lock_guard<std::mutex> lk(mu_);
        blocked = blocked || robot_estop_requested_;
    }
    if (blocked) set_result(what + " 보내지 않음: 비상정지 중(먼저 해제)");
    return blocked;
}

void Robot::go_home() {
    if (estop_blocks("서기")) return;
    call("/puppy_control/go_home", "std_srvs/Empty", Json::object(), "서기(go_home)");
}

void Robot::set_pose_custom(double roll, double pitch, double yaw, double height, double x_shift, double stance_x,
                            double stance_y, int run_time_ms) {
    if (estop_blocks("자세")) return;
    Json m = Json::object();
    m.set("roll", roll).set("pitch", pitch).set("yaw", yaw).set("height", height).set("x_shift", x_shift);
    m.set("stance_x", stance_x).set("stance_y", stance_y).set("run_time", run_time_ms);
    std::lock_guard<std::mutex> lk(mu_);
    if (rb_ && rb_->publish("/puppy_control/pose", m))
        last_result_ = "자세 전송";
    else
        last_result_ = "자세: rosbridge 연결 안 됨";
}

void Robot::set_pose(const std::string& preset_name) {
    if (estop_blocks("자세")) return;
    for (const auto& p : pose_presets())
        if (preset_name == p.name) {
            set_pose_custom(p.roll, p.pitch, p.yaw, p.height, p.x_shift, p.stance_x, p.stance_y, 500);
            events_.add(std::string("자세 ") + p.label);
            return;
        }
    set_result("모르는 자세: " + preset_name);
}

void Robot::run_action(const std::string& name) {
    if (estop_blocks("동작 " + name)) return;
    bool known = false;
    for (const auto& a : action_groups()) known = known || name == a.name;
    if (!known) {
        set_result("모르는 동작: " + name);
        return;
    }
    Json args = Json::object();
    args.set("name", name + ".d6ac").set("wait", false);
    call("/puppy_control/runActionGroup", "puppy_control/SetRunActionName", args, "동작 " + name);
}

void Robot::save_map() { call("/lidar_mapper/save_map", "std_srvs/Trigger", Json::object(), "지도 저장"); }
void Robot::reset_map() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        trail_.clear();  // 새 지도는 원점이 지금 위치로 바뀐다
    }
    {
        // 새 지도는 원점이 바뀌므로 옛 좌표의 경로를 로봇이 계속 따라가지 않게 취소한다
        std::lock_guard<std::mutex> send_lk(nav_send_mu_);
        stop_nav("지도 새로 시작");
        if (v2_.cancel_path()) events_.add("경로 취소 전송(지도 새로)");
    }
    call("/lidar_mapper/reset_map", "std_srvs/Trigger", Json::object(), "지도 새로 시작");
}

void Robot::list_topics() {
    RosBridge* rb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rb = rb_.get();
    }
    if (!rb) return;
    rb->call("/rosapi/topics", "rosapi/Topics", Json::object(), [this](bool ok, const Json& v) {
        if (!ok) {
            set_result("토픽 목록 실패");
            return;
        }
        const auto& topics = v["topics"].as_array();
        const auto& types = v["types"].as_array();
        for (size_t i = 0; i < topics.size(); ++i)
            events_.add("  " + topics[i].as_string() + (i < types.size() ? "  [" + types[i].as_string() + "]" : ""));
        set_result(fmt("토픽 %zu개", topics.size()));
    });
}

void Robot::emergency_stop_all() {
    v2_.emergency_stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        robot_estop_requested_ = true;
    }
    call("/puppy_control/emergency_stop", "std_srvs/Trigger", Json::object(), "로봇 전체 비상정지");
}

void Robot::clear_emergency_all() {
    RosBridge* rb;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rb = rb_.get();
        robot_estop_requested_ = false;
    }
    // 로봇 전체 latch 가 남아 있으면 v2 세션도 비상정지 상태라서 순서가 중요하다(팀 로봇 문서 06 §3)
    if (rb && rb->connected()) {
        rb->call("/puppy_control/reset_estop", "std_srvs/Trigger", Json::object(), [this](bool ok, const Json& v) {
            set_result(std::string("로봇 전체 비상정지 해제 ") + (ok ? "완료" : "실패") + " " + v["message"].as_string());
            v2_.clear_emergency();
        });
    } else {
        v2_.clear_emergency();
    }
}

bool Robot::send_path(const std::vector<Pt>& pts, std::string* err) {
    int64_t seq = -1;
    if (!v2_.send_path(pts, &seq, err)) return false;
    events_.add(fmt("경로 전송 %zu점 (sequence %lld, 길이 %.2f m)", pts.size(), static_cast<long long>(seq),
                    polyline_length(pts)));
    return true;
}

void Robot::cancel_path() {
    std::lock_guard<std::mutex> send_lk(nav_send_mu_);  // nav 스레드가 재계획 경로를 막 보내려던 참이면 그 뒤에 취소가 간다
    stop_nav("사용자 취소");
    if (v2_.cancel_path()) events_.add("경로 취소 전송");
}

bool Robot::save_snapshot_jpeg(const std::string& path, std::string* err) const {
    std::string jpeg;
    double rx;
    uint64_t seq;
    if (!camera_frame(jpeg, rx, seq)) {
        if (err) *err = "카메라 프레임 없음";
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        if (err) *err = "파일을 열 수 없음: " + path;
        return false;
    }
    std::fwrite(jpeg.data(), 1, jpeg.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace hg

namespace hg {

// ---------------- 장애물 회피(앱 쪽) ----------------

std::vector<Pt> Robot::live_obstacles_locked(double now) const {
    std::vector<Pt> obs;
    if (scan_.valid && now - scan_.rx_time < 1.0 && now - pose_rx_ < 1.0)
        for (const auto& p : scan_to_map(scan_.ranges, scan_.angle_min, scan_.angle_increment, std::min(scan_.range_max, 6.0), pose_))
            obs.emplace_back(p.first, p.second);
    for (const auto& t : nav_temp_)
        if (t.second > now) obs.push_back(t.first);
    return obs;
}

PlanResult Robot::preview_plan(const std::vector<Pt>& waypoints) const {
    std::shared_ptr<const OccGrid> g;
    std::vector<Pt> obs;
    Pt start;
    {
        std::lock_guard<std::mutex> lk(mu_);
        g = grid_;
        obs = live_obstacles_locked(now_s());
        start = {pose_.x, pose_.y};
    }
    return plan_path(g.get(), obs, start, waypoints, plan_config);
}

bool Robot::go_path(const std::vector<Pt>& waypoints, std::string* err) {
    std::lock_guard<std::mutex> send_lk(nav_send_mu_);  // nav 스레드가 옛 우회 경로를 이 경로 뒤에 보내지 않게
    if (estop_blocks("경로")) {
        if (err) *err = "비상정지 중";
        return false;
    }
    PlanResult r = preview_plan(waypoints);
    if (!r.ok) {
        if (err) *err = r.reason;
        std::lock_guard<std::mutex> lk(mu_);
        ++nav_gen_;
        nav_ = NavStatus{};
        nav_.phase = "blocked";
        nav_.message = r.reason;
        nav_.goals = waypoints;
        return false;
    }
    int64_t seq = -1;
    if (!v2_.send_path(r.path, &seq, err)) return false;
    events_.add(fmt("경로 전송 %zu점%s (sequence %lld, 길이 %.2f m)", r.path.size(), r.detoured ? ", 장애물 우회" : "",
                    static_cast<long long>(seq), polyline_length(r.path)));
    std::lock_guard<std::mutex> lk(mu_);
    ++nav_gen_;
    nav_ = NavStatus{};
    nav_.phase = "following";
    nav_.message = r.detoured ? "장애물을 피해 가는 중" : "경로 따라가는 중";
    nav_.detoured = r.detoured;
    nav_.goals = waypoints;
    nav_.planned = r.path;
    nav_goal_at_ = r.goal_at;
    nav_seq_ = seq;
    nav_trigger_.replanned(now_s());
    return true;
}

void Robot::stop_nav(const std::string& why) {
    std::lock_guard<std::mutex> lk(mu_);
    if (nav_.phase == "following" || nav_.phase == "replanning") {
        nav_.phase = "idle";
        nav_.message = why;
        events_.add("회피 주행 끝: " + why);
    }
    ++nav_gen_;  // 계획 중이던 nav 스레드의 결과는 버려진다
}

// 10 Hz: 로봇 경로 상태를 따라가며, 남은 경로 앞 1.6 m 안에 장애물이 스캔 4번 중 3번 잡히면 취소 → 다시 계획 → 다시 보냄
void Robot::nav_loop() {
    const double lookahead = 1.6;
    while (nav_run_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        double now = now_s();
        V2Snapshot v = v2_.snapshot();
        RobotSnapshot s;
        std::vector<Pt> obs, planned, goals;
        std::vector<int> goal_at;
        int64_t seq;
        uint64_t gen;
        std::string phase;
        bool new_scan;
        {
            std::lock_guard<std::mutex> lk(mu_);
            // 지도 PNG 압축이 8 초 넘게 안 오면 로봇의 rosbridge 가 PNG 를 못 만드는 것으로 보고 일반 JSON 으로 바꾼다
            bool rbc = rb_ && rb_->connected();
            if (rbc && rb_connected_since_ == 0) rb_connected_since_ = now;
            if (!rbc) rb_connected_since_ = 0;
            if (rbc && grid_png_ && grid_rx_ == 0 && now - rb_connected_since_ > 8.0) {
                grid_png_ = false;
                rb_->subscribe_raw("/map", "nav_msgs/OccupancyGrid", [this](const std::string& text) {
                    auto g = std::make_shared<OccGrid>();
                    if (!parse_occupancy_grid(text, *g)) return;
                    std::lock_guard<std::mutex> lk2(mu_);
                    grid_ = std::move(g);
                    ++grid_version_;
                    grid_rx_ = now_s();
                }, 2000, "");
                events_.add("지도: PNG 압축 응답 없음 → 일반 JSON 으로 받음");
            }
            for (size_t i = 0; i < nav_temp_.size();)
                if (nav_temp_[i].second <= now) nav_temp_.erase(nav_temp_.begin() + static_cast<long>(i));
                else ++i;
            phase = nav_.phase;
            gen = nav_gen_;
            if (phase != "following" && phase != "replanning") continue;
            s.path = path_;
            s.pose = pose_;
            s.pose_rx = pose_rx_;
            new_scan = scan_.valid && scan_.rx_time > nav_last_scan_;
            if (new_scan) nav_last_scan_ = scan_.rx_time;
            obs = live_obstacles_locked(now);
            planned = nav_.planned;
            goals = nav_.goals;
            goal_at = nav_goal_at_;
            seq = nav_seq_;
        }
        // 사용자가 스틱으로 직접 운전하면(로봇이 경로를 취소한다) 회피 주행을 끝낸다
        if (v.cmd_active) {
            stop_nav("직접 운전으로 전환");
            continue;
        }
        if (v.estop) {
            stop_nav("비상정지");
            continue;
        }
        bool ours = s.path.valid && s.path.request_sequence == seq && s.path.request_client_id == v.client_id;
        if (phase == "following" && ours) {
            if (s.path.state == "done") {
                std::lock_guard<std::mutex> lk(mu_);
                nav_.phase = "done";
                nav_.message = "도착";
                continue;
            }
            if (s.path.state == "cancelled" && now > nav_own_cancel_until_) {
                stop_nav("로봇이 경로를 취소함" + (s.path.reason.empty() ? std::string() : " (" + s.path.reason + ")"));
                continue;
            }
        }
        Pt here{s.pose.x, s.pose.y};
        // 다시 계획한 경로를 보낼 차례: 로봇이 앞 경로 취소를 처리하고 중립(idle)이면 보낸다
        if (phase == "replanning") {
            bool robot_free = !(s.path.valid && (s.path.state == "rotate" || s.path.state == "drive") && s.path.request_sequence == seq);
            // 사용자가 그사이 멈췄거나 새 경로를 보냈으면 보내지 않는다. 판단과 송신 사이에 멈춤이 끼어들지 못하게 묶는다
            std::lock_guard<std::mutex> send_lk(nav_send_mu_);
            std::vector<Pt> next;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (nav_gen_ != gen || nav_.phase != "replanning") continue;
                next = nav_pending_path_;
            }
            if (robot_free && v.state == "idle") {
                int64_t nseq = -1;
                std::string err;
                if (v2_.send_path(next, &nseq, &err)) {
                    std::lock_guard<std::mutex> lk(mu_);
                    nav_.phase = "following";
                    nav_.message = fmt("장애물을 피해 다시 가는 중(%d번째 우회)", nav_.replans);
                    nav_.planned = next;
                    nav_seq_ = nseq;
                    nav_trigger_.reset();
                    events_.add(fmt("우회 경로 전송 %zu점 (sequence %lld)", next.size(), static_cast<long long>(nseq)));
                }
            } else if (now - nav_pending_since_ > 3.0) {
                stop_nav("우회 경로를 보내지 못함(로봇 " + v.state + ")");
            }
            continue;
        }
        if (!new_scan || now - s.pose_rx > 1.0) continue;
        int index = ours ? std::max(0, s.path.index) : 0;
        std::vector<Pt> hits;
        bool hit = path_blocked_by(obs, here, planned, static_cast<size_t>(index), plan_config.inflate - 0.03, lookahead, &hits);
        bool fire;
        {
            std::lock_guard<std::mutex> lk(mu_);
            fire = nav_trigger_.push(hit, now);  // 스캔 4번 중 3번, 직전 재계획 뒤 1.5 s
        }
        if (!fire) continue;
        // 막힘 확정 → 멈추고 다시 계획
        nav_own_cancel_until_ = now + 2.0;
        v2_.cancel_path();
        std::vector<Pt> remaining = remaining_goals(goals, goal_at, index);
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& h : hits) nav_temp_.emplace_back(h, now + 20.0);  // 막힌 점은 20 초 기억
            obs = live_obstacles_locked(now);
        }
        std::shared_ptr<const OccGrid> g;
        {
            std::lock_guard<std::mutex> lk(mu_);
            g = grid_;
        }
        PlanResult r = plan_path(g.get(), obs, here, remaining, plan_config);
        std::lock_guard<std::mutex> lk(mu_);
        if (nav_gen_ != gen || nav_.phase != "following") continue;  // 계획하는 동안 사용자가 멈췄거나 새 경로를 보냈다
        nav_trigger_.replanned(now);
        nav_.obstacles.clear();
        for (const auto& h : hits) nav_.obstacles.push_back(h);
        if (!r.ok) {
            nav_.phase = "blocked";
            nav_.message = "우회 경로 없음 — 다시 그려 주세요 (" + r.reason + ")";
            events_.add("장애물 감지, " + nav_.message);
            continue;
        }
        ++nav_.replans;
        nav_.phase = "replanning";
        nav_.message = "장애물 감지 → 우회 경로로 바꾸는 중";
        nav_pending_path_ = r.path;
        nav_pending_since_ = now;
        nav_goal_at_ = r.goal_at;
        nav_.goals = remaining;
        events_.add(fmt("장애물 감지(%zu점) → 우회 경로 %zu점 계획", hits.size(), r.path.size()));
    }
}

}  // namespace hg
