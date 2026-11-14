// rosbridge v2(WebSocket, TCP 9090) 클라이언트. 서비스 호출·토픽 구독·발행. 끊기면 2 s 뒤 다시 연결하고 구독·광고를 되살린다.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "json.hpp"
#include "sock.hpp"

namespace hg {

class RosBridge {
public:
    using TopicFn = std::function<void(const Json& msg)>;
    using ServiceFn = std::function<void(bool ok, const Json& values)>;

    RosBridge(std::string host, uint16_t port = 9090);
    ~RosBridge();
    void start();
    void stop();
    bool connected() const { return connected_; }
    std::string status() const;

    // throttle_ms: rosbridge 가 이 간격 안에 온 메시지를 버린다(0 = 모두)
    void subscribe(const std::string& topic, const std::string& type, TopicFn fn, int throttle_ms = 0);
    void unsubscribe(const std::string& topic);
    void advertise(const std::string& topic, const std::string& type);
    bool publish(const std::string& topic, const Json& msg);
    bool call(const std::string& service, const std::string& type, const Json& args, ServiceFn fn = nullptr);

    uint64_t rx_count() const { return rx_; }

private:
    struct Sub {
        std::string type;
        TopicFn fn;
        int throttle_ms;
    };
    void loop();
    bool handshake(sock_t fd);
    bool send_text(const std::string& text);
    bool send_frame(int op, const std::string& payload);
    bool recv_frame(sock_t fd, int& op, std::string& payload);
    void on_message(const std::string& text);

    std::string host_;
    uint16_t port_;
    std::atomic<bool> running_{false}, connected_{false};
    std::atomic<sock_t> fd_{kBadSock};
    std::thread th_;
    mutable std::mutex mu_;      // subs_, ads_, pending_, status_
    std::mutex send_mu_;
    std::map<std::string, Sub> subs_;
    std::map<std::string, std::string> ads_;
    std::map<std::string, ServiceFn> pending_;
    std::string status_ = "stopped";
    std::atomic<uint64_t> next_id_{1}, rx_{0};
};

}  // namespace hg
