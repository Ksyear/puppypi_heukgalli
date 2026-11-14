#include "robot.hpp"

#include "catalog.hpp"

#include <chrono>
#include <cstdio>

namespace hg {

namespace {
constexpr uint16_t kV2Port = 5001, kVideoPort = 8080, kMediaPort = 8090, kRosbridgePort = 9090;
const char* kCameraPath = "/stream?topic=/usb_cam/image_raw&type=ros_compressed";
const char* kMapPath = "/stream?topic=/lidar_map/image&type=ros_compressed";
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
    rb->subscribe("/scan", "sensor_msgs/LaserScan", [this](const Json& m) {
        ScanData s;
        if (!parse_scan(m, s)) return;
        s.rx_time = now_s();
        std::lock_guard<std::mutex> lk(mu_);
        scan_ = std::move(s);
    }, 200);
    rb->start();
    auto map_img = std::make_unique<MjpegReader>(host, kVideoPort, kMapPath);
    map_img->start();
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
    }
    http_run_ = true;
    http_th_ = std::thread([this] { http_loop(); });
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

void Robot::go_home() { call("/puppy_control/go_home", "std_srvs/Empty", Json::object(), "서기(go_home)"); }

void Robot::set_pose_custom(double roll, double pitch, double yaw, double height, double x_shift, double stance_x,
                            double stance_y, int run_time_ms) {
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
    for (const auto& p : pose_presets())
        if (preset_name == p.name) {
            set_pose_custom(p.roll, p.pitch, p.yaw, p.height, p.x_shift, p.stance_x, p.stance_y, 500);
            events_.add(std::string("자세 ") + p.label);
            return;
        }
    set_result("모르는 자세: " + preset_name);
}

void Robot::run_action(const std::string& name) {
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
void Robot::reset_map() { call("/lidar_mapper/reset_map", "std_srvs/Trigger", Json::object(), "지도 새로 시작"); }

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
