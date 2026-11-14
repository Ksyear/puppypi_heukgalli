// PuppyPi Protocol v2(UDP 5001) 조종 세션. 20 Hz 로 command 를 보내고 ACK 로 로봇 상태를 따라간다.
//
// 로봇 규칙(팀 로봇 문서 06): 단일 조종자(UUID+주소, lease 1.0 s), watchdog 0.4 s, 활성 명령 전 중립 확인,
// 비상정지 latch 는 조종자만 해제, disconnect 한 UUID 는 재사용 불가. 이 클래스가 지키는 앱 쪽 규칙:
//   - 입력이 0.25 s 넘게 갱신되지 않으면(화면 멈춤) 중립
//   - ACK 가 0.6 s 넘게 없으면 중립(로봇은 0.4 s 에 먼저 멈춘다)
//   - 로봇이 idle/active 를 알려 오기 전에는 중립만 보낸다
//   - 비상정지 해제 뒤에는 입력을 한 번 놓아야 다시 움직인다
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sock.hpp"

namespace hg {

struct V2Ack {
    std::string client_id;  // invalid_packet 이면 빈 문자열일 수 있음
    int64_t sequence = -1;
    bool accepted = false;
    std::string reason, state;
    bool emergency_stop = false;
};
// 정확히 8필드인 ACK 만 받아들인다(Unity 앱의 V2AckParser 와 같은 기준)
bool parse_v2_ack(const std::string& data, V2Ack& out);

// 이번 주기에 활성 명령을 보낼지 정하는 순수 함수(단위 시험 대상)
struct DriveInput {
    bool input_fresh = false;   // 화면이 최근 0.25 s 안에 입력을 줬다
    bool held = false;          // 데드맨을 쥐고 있다
    double forward = 0, turn = 0;
    bool ack_fresh = false;     // 최근 0.6 s 안에 ACK
    std::string state;          // 마지막 ACK 의 state
    bool ack_estop = false;     // 마지막 ACK 의 emergency_stop
    bool estop_requested = false;
    bool release_required = false;
};
struct DriveDecision {
    bool active = false;
    bool release_required = false;  // 다음 주기에 넘길 값
};
DriveDecision decide_drive(const DriveInput& in);

struct V2Snapshot {
    bool running = false;
    std::string robot;      // ip:port
    std::string client_id;
    int64_t sequence = 0;   // 마지막으로 보낸 번호
    bool linked = false;    // 최근 0.6 s 안에 ACK
    double ack_age_s = -1;  // 마지막 ACK 뒤 경과(없으면 -1)
    std::string state;      // 로봇 세션 상태(마지막 ACK)
    std::string reason;     // 마지막 ACK 이유
    bool accepted = false;
    bool estop = false;          // 로봇이 알려 온 비상정지 latch
    bool owner = false;          // 최근 ACK 가 not_owner 가 아님
    bool release_required = false;
    double rtt_last_ms = -1, rtt_avg_ms = -1;
    double loss_pct = 0;         // 최근 100개 command 중 ACK 못 받은 비율
    uint64_t tx = 0, rx = 0;
    // 지금 보내는 명령
    bool cmd_active = false;
    double cmd_forward = 0, cmd_turn = 0;
    // 마지막 경로 요청
    int64_t path_sequence = -1;
    bool path_ack_seen = false, path_accepted = false;
    std::string path_reason;
};

class V2Client {
public:
    using LogFn = std::function<void(const std::string&)>;

    V2Client();
    ~V2Client();
    bool start(const std::string& host, uint16_t port, std::string* err);
    void stop();  // 중립 + disconnect 후 정지
    bool running() const { return running_; }

    // 매 화면 프레임마다 부른다. held = 운전 버튼(데드맨)을 누르고 있음. forward/turn = -1..1(속도 비율 반영 전)
    void set_drive(double forward, double turn, bool held);
    void set_speed_scale(double s);  // 0.1 ~ 1.0
    double speed_scale() const;

    void emergency_stop();
    void clear_emergency();
    // 지도 좌표(m) 경유점 1~20개. 성공하면 보낸 sequence
    bool send_path(const std::vector<std::pair<double, double>>& points, int64_t* sequence_out, std::string* err);
    bool cancel_path();
    void new_session();  // disconnect 후 새 UUID 로 다시 시작

    V2Snapshot snapshot() const;
    void set_log(LogFn fn);
    void set_packet_log(LogFn fn);  // 모든 송수신 패킷(디버그 화면용)

private:
    void sender_loop();
    void receiver_loop();
    bool send_json_locked(const std::string& text);
    int64_t next_sequence_locked() { return ++sequence_; }
    void log(const std::string& s);

    mutable std::mutex mu_;
    sock_t sock_ = kBadSock;
    std::atomic<bool> running_{false};
    std::thread sender_, receiver_;
    std::string robot_text_;
    std::string client_id_;
    int64_t sequence_ = 0;
    double speed_scale_ = 0.5;
    // 입력
    double in_forward_ = 0, in_turn_ = 0;
    bool in_held_ = false;
    double in_time_ = 0;
    bool release_required_ = false;
    bool estop_requested_ = false;
    int64_t estop_sequence_ = -1;
    // 마지막 ACK
    V2Ack last_ack_;
    bool have_ack_ = false;
    double last_ack_time_ = 0;
    bool owner_ = false;
    // 통계
    std::map<int64_t, double> sent_at_;
    std::deque<std::pair<int64_t, bool>> recent_;  // (sequence, acked)
    double rtt_last_ = -1, rtt_avg_ = -1;
    uint64_t tx_ = 0, rx_ = 0;
    bool cmd_active_ = false;
    double cmd_forward_ = 0, cmd_turn_ = 0;
    std::string last_state_logged_;
    int64_t path_sequence_ = -1;
    bool path_ack_seen_ = false, path_accepted_ = false;
    std::string path_reason_;
    LogFn log_fn_, packet_fn_;
};

}  // namespace hg
