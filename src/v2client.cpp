#include "v2client.hpp"

#include "json.hpp"
#include "net.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef _WIN32
#include <mswsock.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

namespace hg {

namespace {
constexpr double kPeriod = 0.05;        // 20 Hz
constexpr double kInputStale = 0.25;    // 화면이 이만큼 입력을 안 주면 중립
constexpr double kAckStale = 0.6;       // ACK 가 이만큼 없으면 중립
constexpr size_t kRecent = 100;
constexpr int kMaxPathPoints = 20;
constexpr double kMaxCoord = 100.0;

double clamp1(double v) { return std::max(-1.0, std::min(1.0, std::isfinite(v) ? v : 0.0)); }
}  // namespace

bool parse_v2_ack(const std::string& data, V2Ack& out) {
    Json j;
    try {
        j = Json::parse(data);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object() || j.size() != 8) return false;
    static const char* keys[] = {"protocol", "type", "client_id", "sequence", "accepted", "reason", "emergency_stop", "state"};
    for (const char* k : keys)
        if (!j.has(k)) return false;
    if (!j["protocol"].is_int() || j["protocol"].as_int() != 2 || j["type"].as_string() != "ack") return false;
    if (!j["accepted"].is_bool() || !j["emergency_stop"].is_bool() || !j["reason"].is_string() || !j["state"].is_string())
        return false;
    out.client_id = j["client_id"].as_string();
    out.sequence = j["sequence"].is_int() ? j["sequence"].as_int() : -1;
    out.accepted = j["accepted"].as_bool();
    out.reason = j["reason"].as_string();
    out.state = j["state"].as_string();
    out.emergency_stop = j["emergency_stop"].as_bool();
    return true;
}

DriveDecision decide_drive(const DriveInput& in) {
    DriveDecision d;
    bool want = in.input_fresh && in.held && (std::fabs(in.forward) > 1e-3 || std::fabs(in.turn) > 1e-3);
    d.release_required = want && in.release_required;  // 입력을 놓으면 재프레스 요구가 풀린다
    bool ready = in.state == "idle" || in.state == "active";
    d.active = want && in.ack_fresh && ready && !d.release_required && !in.estop_requested && !in.ack_estop;
    return d;
}

V2Client::V2Client() : client_id_(uuid4()) {}

V2Client::~V2Client() { stop(); }

void V2Client::set_log(LogFn fn) {
    std::lock_guard<std::mutex> lk(mu_);
    log_fn_ = std::move(fn);
}

void V2Client::set_packet_log(LogFn fn) {
    std::lock_guard<std::mutex> lk(mu_);
    packet_fn_ = std::move(fn);
}

void V2Client::log(const std::string& s) {
    LogFn fn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fn = log_fn_;
    }
    if (fn) fn(s);
}

bool V2Client::start(const std::string& host, uint16_t port, std::string* err) {
    stop();
    sockaddr_in addr{};
    if (!resolve_ipv4(host, port, addr)) {
        if (err) *err = "주소 해석 실패: " + host;
        return false;
    }
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSock) {
        if (err) *err = sock_err_text(sock_err());
        return false;
    }
#ifdef _WIN32
    // 로봇이 꺼져 ICMP port unreachable 이 오면 Windows 는 recv 를 WSAECONNRESET 으로 깨운다. 그 동작을 끈다.
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
#endif
    // connect 한 UDP 소켓은 로봇 주소에서 온 데이터그램만 받는다(Unity 앱과 같은 방식)
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (err) *err = sock_err_text(sock_err());
        sock_close(s);
        return false;
    }
    sock_set_recv_timeout(s, 200);
    {
        std::lock_guard<std::mutex> lk(mu_);
        sock_ = s;
        robot_text_ = addr_text(addr);
        sequence_ = 0;
        have_ack_ = false;
        owner_ = false;
        sent_at_.clear();
        recent_.clear();
        rtt_last_ = rtt_avg_ = -1;
        tx_ = rx_ = 0;
        release_required_ = false;
        estop_requested_ = false;
        last_state_logged_.clear();
        path_sequence_ = -1;
    }
    running_ = true;
    receiver_ = std::thread([this] { receiver_loop(); });
    sender_ = std::thread([this] { sender_loop(); });
    log("조종 연결 시작 " + robot_text_ + " client " + client_id_.substr(0, 8));
    return true;
}

void V2Client::stop() {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 중립 1회 + disconnect: 로봇이 소유권을 바로 풀어 다른 조종기가 쓸 수 있다
        Json n = Json::object();
        n.set("protocol", 2).set("type", "command").set("client_id", client_id_).set("sequence", next_sequence_locked());
        n.set("control_active", false).set("forward", 0.0).set("turn", 0.0);
        send_json_locked(n.dump());
        Json d = Json::object();
        d.set("protocol", 2).set("type", "disconnect").set("client_id", client_id_).set("sequence", next_sequence_locked());
        send_json_locked(d.dump());
    }
    running_ = false;
    if (sender_.joinable()) sender_.join();
    if (receiver_.joinable()) receiver_.join();
    std::lock_guard<std::mutex> lk(mu_);
    if (sock_ != kBadSock) sock_close(sock_);
    sock_ = kBadSock;
    // disconnect 한 UUID 는 로봇이 다시 받지 않으므로 다음 연결은 새 UUID
    client_id_ = uuid4();
}

void V2Client::new_session() {
    std::string robot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        robot = robot_text_;
    }
    if (robot.empty()) return;
    auto colon = robot.rfind(':');
    std::string host = robot.substr(0, colon);
    uint16_t port = static_cast<uint16_t>(std::atoi(robot.c_str() + colon + 1));
    stop();
    std::string err;
    if (!start(host, port, &err)) log("다시 연결 실패: " + err);
}

void V2Client::set_drive(double forward, double turn, bool held) {
    std::lock_guard<std::mutex> lk(mu_);
    in_forward_ = clamp1(forward);
    in_turn_ = clamp1(turn);
    in_held_ = held;
    in_time_ = now_s();
}

void V2Client::set_speed_scale(double s) {
    std::lock_guard<std::mutex> lk(mu_);
    speed_scale_ = std::max(0.1, std::min(1.0, s));
}

double V2Client::speed_scale() const {
    std::lock_guard<std::mutex> lk(mu_);
    return speed_scale_;
}

bool V2Client::send_json_locked(const std::string& text) {
    if (sock_ == kBadSock) return false;
    auto n = send(sock_, text.data(), static_cast<int>(text.size()), 0);
    if (n < 0) return false;
    ++tx_;
    if (packet_fn_) packet_fn_("TX " + text);
    return true;
}

void V2Client::emergency_stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        estop_requested_ = true;
        Json e = Json::object();
        e.set("protocol", 2).set("type", "emergency_stop").set("client_id", client_id_).set("sequence", next_sequence_locked());
        // UDP 한 개가 사라질 수 있으니 두 번 보낸다(두 번째는 번호를 올려 out_of_order 를 피함)
        send_json_locked(e.dump());
        estop_sequence_ = next_sequence_locked();
        e.set("sequence", estop_sequence_);
        send_json_locked(e.dump());
    }
    log("비상정지 전송");
}

void V2Client::clear_emergency() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        estop_requested_ = false;
        release_required_ = true;
        Json e = Json::object();
        e.set("protocol", 2).set("type", "clear_emergency").set("client_id", client_id_).set("sequence", next_sequence_locked());
        send_json_locked(e.dump());
    }
    log("비상정지 해제 요청(입력을 한 번 놓은 뒤 다시 움직일 수 있음)");
}

bool V2Client::send_path(const std::vector<std::pair<double, double>>& points, int64_t* sequence_out, std::string* err) {
    if (points.empty() || points.size() > static_cast<size_t>(kMaxPathPoints)) {
        if (err) *err = "경유점은 1~20개";
        return false;
    }
    Json pts = Json::array();
    for (const auto& p : points) {
        if (!std::isfinite(p.first) || !std::isfinite(p.second) || std::fabs(p.first) > kMaxCoord ||
            std::fabs(p.second) > kMaxCoord) {
            if (err) *err = "좌표가 범위(±100 m)를 벗어남";
            return false;
        }
        Json xy = Json::array();
        xy.push(std::round(p.first * 1000.0) / 1000.0);
        xy.push(std::round(p.second * 1000.0) / 1000.0);
        pts.push(xy);
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) {
        if (err) *err = "조종 연결 안 됨";
        return false;
    }
    std::string st = have_ack_ ? last_ack_.state : "";
    if (st != "idle") {
        // 로봇은 중립 확인 전 경로를 neutral_required 로 거부한다. 운전 입력 중이면 active 다.
        if (err) *err = st == "active" ? "운전 입력을 놓은 뒤 다시 시도" : "로봇이 아직 준비 안 됨(" + (st.empty() ? "응답 없음" : st) + ")";
        return false;
    }
    Json m = Json::object();
    int64_t seq = next_sequence_locked();
    m.set("protocol", 2).set("type", "path").set("client_id", client_id_).set("sequence", seq).set("points", pts);
    if (!send_json_locked(m.dump())) {
        if (err) *err = "전송 실패";
        return false;
    }
    path_sequence_ = seq;
    path_ack_seen_ = false;
    path_accepted_ = false;
    path_reason_.clear();
    if (sequence_out) *sequence_out = seq;
    return true;
}

bool V2Client::cancel_path() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) return false;
    Json m = Json::object();
    m.set("protocol", 2).set("type", "path_cancel").set("client_id", client_id_).set("sequence", next_sequence_locked());
    return send_json_locked(m.dump());
}

void V2Client::sender_loop() {
    double next = now_s();
    while (running_) {
        double t = now_s();
        if (t < next) {
            std::this_thread::sleep_for(std::chrono::duration<double>(next - t));
            continue;
        }
        next += kPeriod;
        if (next < t) next = t + kPeriod;  // 밀렸으면 따라잡지 않는다

        std::lock_guard<std::mutex> lk(mu_);
        DriveInput in;
        in.input_fresh = t - in_time_ <= kInputStale;
        in.held = in_held_;
        in.forward = in_forward_;
        in.turn = in_turn_;
        in.ack_fresh = have_ack_ && t - last_ack_time_ <= kAckStale;
        in.state = have_ack_ ? last_ack_.state : "";
        in.ack_estop = have_ack_ && last_ack_.emergency_stop;
        in.estop_requested = estop_requested_;
        in.release_required = release_required_;
        DriveDecision dd = decide_drive(in);
        release_required_ = dd.release_required;
        bool active = dd.active;

        Json m = Json::object();
        int64_t seq = next_sequence_locked();
        m.set("protocol", 2).set("type", "command").set("client_id", client_id_).set("sequence", seq);
        m.set("control_active", active);
        double f = active ? clamp1(in_forward_ * speed_scale_) : 0.0;
        double r = active ? clamp1(in_turn_ * speed_scale_) : 0.0;
        m.set("forward", std::round(f * 1000.0) / 1000.0).set("turn", std::round(r * 1000.0) / 1000.0);
        if (send_json_locked(m.dump())) {
            sent_at_[seq] = t;
            recent_.emplace_back(seq, false);
            while (recent_.size() > kRecent) recent_.pop_front();
            while (sent_at_.size() > 256) sent_at_.erase(sent_at_.begin());
        }
        cmd_active_ = active;
        cmd_forward_ = f;
        cmd_turn_ = r;
    }
}

void V2Client::receiver_loop() {
    std::string buf(4096, '\0');
    while (running_) {
        sock_t s;
        {
            std::lock_guard<std::mutex> lk(mu_);
            s = sock_;
        }
        auto n = recv(s, &buf[0], static_cast<int>(buf.size()), 0);
        if (n <= 0) continue;  // 시간 초과(200 ms) 또는 일시 오류
        std::string data(buf.data(), static_cast<size_t>(n));
        V2Ack ack;
        bool ok = parse_v2_ack(data, ack);
        std::string state_event;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++rx_;
            if (packet_fn_) packet_fn_("RX " + data);
            if (!ok) continue;
            if (!ack.client_id.empty() && ack.client_id != client_id_) continue;  // 이전 세션의 늦은 ACK
            double t = now_s();
            last_ack_ = ack;
            have_ack_ = true;
            last_ack_time_ = t;
            owner_ = ack.reason != "not_owner";
            auto it = sent_at_.find(ack.sequence);
            if (it != sent_at_.end()) {
                rtt_last_ = (t - it->second) * 1000.0;
                rtt_avg_ = rtt_avg_ < 0 ? rtt_last_ : rtt_avg_ * 0.9 + rtt_last_ * 0.1;
                sent_at_.erase(it);
            }
            for (auto& r : recent_)
                if (r.first == ack.sequence) r.second = true;
            if (ack.sequence == path_sequence_) {
                path_ack_seen_ = true;
                path_accepted_ = ack.accepted;
                path_reason_ = ack.reason;
            }
            std::string key = ack.state + "/" + (ack.accepted ? "ok" : ack.reason) + (ack.emergency_stop ? "/estop" : "");
            if (key != last_state_logged_) {
                last_state_logged_ = key;
                state_event = "로봇 상태 " + ack.state + (ack.accepted ? "" : " · 거부 " + ack.reason) +
                              (ack.emergency_stop ? " · 비상정지 latch" : "");
            }
            // 비상정지 뒤에 보낸 패킷의 ACK 인데 latch 가 없으면(해제됐거나 비상정지 패킷이 모두 유실) 요청 표시를 지운다
            if (estop_requested_ && ack.sequence > estop_sequence_ && !ack.emergency_stop) estop_requested_ = false;
        }
        if (!state_event.empty()) log(state_event);
    }
}

V2Snapshot V2Client::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    V2Snapshot s;
    double t = now_s();
    s.running = running_;
    s.robot = robot_text_;
    s.client_id = client_id_;
    s.sequence = sequence_;
    s.ack_age_s = have_ack_ ? t - last_ack_time_ : -1;
    s.linked = have_ack_ && t - last_ack_time_ <= kAckStale;
    s.state = have_ack_ ? last_ack_.state : "";
    s.reason = have_ack_ ? last_ack_.reason : "";
    s.accepted = have_ack_ && last_ack_.accepted;
    s.estop = (have_ack_ && last_ack_.emergency_stop) || estop_requested_;
    s.owner = have_ack_ && owner_;
    s.release_required = release_required_;
    s.rtt_last_ms = rtt_last_;
    s.rtt_avg_ms = rtt_avg_;
    // 1 s 넘게 ACK 가 없는 command 만 유실로 센다
    int counted = 0, lost = 0;
    for (const auto& r : recent_) {
        auto it = sent_at_.find(r.first);
        double age = it != sent_at_.end() ? t - it->second : 99;
        if (!r.second && age < 1.0) continue;
        ++counted;
        if (!r.second) ++lost;
    }
    s.loss_pct = counted ? 100.0 * lost / counted : 0;
    s.tx = tx_;
    s.rx = rx_;
    s.cmd_active = cmd_active_;
    s.cmd_forward = cmd_forward_;
    s.cmd_turn = cmd_turn_;
    s.path_sequence = path_sequence_;
    s.path_ack_seen = path_ack_seen_;
    s.path_accepted = path_accepted_;
    s.path_reason = path_reason_;
    return s;
}

}  // namespace hg
