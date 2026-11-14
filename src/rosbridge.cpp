#include "rosbridge.hpp"

#include "image_decode.hpp"
#include "net.hpp"
#include "util.hpp"

#include "sock.hpp"

namespace hg {

namespace {
constexpr uint64_t kMaxMessage = 8ull * 1024 * 1024;  // 가장 큰 정상 메시지는 /map(수백 KB)
}

RosBridge::RosBridge(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

RosBridge::~RosBridge() { stop(); }

void RosBridge::start() {
    if (running_) return;
    running_ = true;
    th_ = std::thread([this] { loop(); });
}

void RosBridge::stop() {
    running_ = false;
    {
        // 수신 스레드가 닫기 전인 소켓만 깨운다(닫힌 번호가 다른 소켓에 재사용되는 경쟁 방지)
        std::lock_guard<std::mutex> lk(fd_mu_);
        if (fd_ != kBadSock) shutdown(fd_, SHUT_RDWR);
    }
    if (th_.joinable()) th_.join();
}

std::string RosBridge::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

Json RosBridge::subscribe_msg(const std::string& topic, const Sub& s) const {
    Json m = Json::object();
    m.set("op", "subscribe").set("topic", topic).set("type", s.type);
    if (s.throttle_ms > 0) m.set("throttle_rate", s.throttle_ms);
    if (!s.compression.empty()) m.set("compression", s.compression);
    return m;
}

void RosBridge::subscribe(const std::string& topic, const std::string& type, TopicFn fn, int throttle_ms) {
    Sub sub{type, std::move(fn), throttle_ms, nullptr, ""};
    {
        std::lock_guard<std::mutex> lk(mu_);
        subs_[topic] = sub;
    }
    if (connected_) send_text(subscribe_msg(topic, sub).dump());
}

void RosBridge::subscribe_raw(const std::string& topic, const std::string& type, RawFn fn, int throttle_ms,
                              const std::string& compression) {
    Sub sub{type, nullptr, throttle_ms, std::move(fn), compression};
    {
        std::lock_guard<std::mutex> lk(mu_);
        subs_[topic] = sub;
    }
    if (connected_) {
        // 같은 토픽을 다른 옵션으로 다시 구독할 때는 먼저 끊는다(rosbridge 는 구독 옵션을 합친다)
        send_text(Json::object().set("op", "unsubscribe").set("topic", topic).dump());
        send_text(subscribe_msg(topic, sub).dump());
    }
}

void RosBridge::unsubscribe(const std::string& topic) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        subs_.erase(topic);
    }
    if (connected_) send_text(Json::object().set("op", "unsubscribe").set("topic", topic).dump());
}

void RosBridge::advertise(const std::string& topic, const std::string& type) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (ads_.count(topic) && ads_[topic] == type) return;
        ads_[topic] = type;
    }
    if (connected_) send_text(Json::object().set("op", "advertise").set("topic", topic).set("type", type).dump());
}

bool RosBridge::publish(const std::string& topic, const Json& msg) {
    if (!connected_) return false;
    return send_text(Json::object().set("op", "publish").set("topic", topic).set("msg", msg).dump());
}

bool RosBridge::call(const std::string& service, const std::string& type, const Json& args, ServiceFn fn) {
    if (!connected_) {
        if (fn) fn(false, Json::object().set("message", "rosbridge 연결 안 됨"));
        return false;
    }
    std::string id = "hg_" + std::to_string(next_id_++);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (fn) pending_[id] = std::move(fn);
    }
    Json m = Json::object();
    m.set("op", "call_service").set("id", id).set("service", service).set("type", type);
    m.set("args", args.is_object() ? args : Json::object());
    return send_text(m.dump());
}

bool RosBridge::handshake(sock_t fd) {
    std::string key = base64_encode(random_hex(16).substr(0, 16));
    std::string req = "GET / HTTP/1.1\r\nHost: " + host_ + ":" + std::to_string(port_) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                      "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!send_all(fd, req)) return false;
    std::string resp;
    char c;
    while (resp.size() < 8192) {
        auto n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        resp += c;
        if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    return resp.find(" 101") != std::string::npos;
}

bool RosBridge::send_frame(int op, const std::string& payload) {
    sock_t fd;
    {
        std::lock_guard<std::mutex> lk(fd_mu_);
        fd = fd_;
    }
    if (fd == kBadSock) return false;
    std::string f;
    f += static_cast<char>(0x80 | op);
    size_t n = payload.size();
    if (n < 126) {
        f += static_cast<char>(0x80 | n);
    } else if (n < 65536) {
        f += static_cast<char>(0x80 | 126);
        f += static_cast<char>((n >> 8) & 0xFF);
        f += static_cast<char>(n & 0xFF);
    } else {
        f += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i) f += static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF);
    }
    std::string mask = random_hex(8);  // 4 바이트로 사용
    unsigned char m[4];
    for (int i = 0; i < 4; ++i) m[i] = static_cast<unsigned char>(std::stoi(mask.substr(i * 2, 2), nullptr, 16));
    f.append(reinterpret_cast<char*>(m), 4);
    for (size_t i = 0; i < n; ++i) f += static_cast<char>(payload[i] ^ m[i % 4]);
    std::lock_guard<std::mutex> lk(send_mu_);
    return send_all(fd, f);
}

bool RosBridge::send_text(const std::string& text) { return send_frame(0x1, text); }

static bool recv_exact(sock_t fd, char* p, size_t n) {
    size_t got = 0;
    while (got < n) {
        auto r = recv(fd, p + got, static_cast<int>(n - got), 0);
        if (r <= 0) {
            if (r < 0 && sock_interrupted(sock_err())) continue;
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

bool RosBridge::recv_frame(sock_t fd, int& op, std::string& payload) {
    payload.clear();
    // 조각(fragment) 메시지는 이어 붙인다
    while (true) {
        unsigned char h[2];
        if (!recv_exact(fd, reinterpret_cast<char*>(h), 2)) return false;
        bool fin = h[0] & 0x80;
        int this_op = h[0] & 0x0F;
        bool masked = h[1] & 0x80;
        uint64_t len = h[1] & 0x7F;
        if (len == 126) {
            unsigned char e[2];
            if (!recv_exact(fd, reinterpret_cast<char*>(e), 2)) return false;
            len = (e[0] << 8) | e[1];
        } else if (len == 127) {
            unsigned char e[8];
            if (!recv_exact(fd, reinterpret_cast<char*>(e), 8)) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
        }
        if (len > kMaxMessage || payload.size() + len > kMaxMessage) return false;  // 조각을 이어 붙여도 8 MiB 까지
        unsigned char m[4] = {0, 0, 0, 0};
        if (masked && !recv_exact(fd, reinterpret_cast<char*>(m), 4)) return false;
        std::string data(static_cast<size_t>(len), '\0');
        if (len && !recv_exact(fd, &data[0], static_cast<size_t>(len))) return false;
        if (masked)
            for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(data[i] ^ m[i % 4]);
        if (this_op >= 0x8) {  // 제어 프레임은 조각 사이에도 올 수 있다
            if (this_op == 0x9) {
                send_frame(0xA, data);
                continue;
            }
            if (this_op == 0xA) continue;
            op = this_op;
            payload = data;
            return true;
        }
        if (this_op != 0) op = this_op;
        payload += data;
        if (fin) return true;
    }
}

// 텍스트에서 "key":"value" 의 value 를 빠르게 찾는다(큰 메시지를 다 해석하기 전에 토픽을 알기 위해)
static std::string quick_field(const std::string& text, const char* key, size_t limit = 512) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = text.find(k);
    if (p == std::string::npos || p > limit) return "";
    p = text.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = text.find('"', p);
    if (p == std::string::npos) return "";
    size_t e = text.find('"', p + 1);
    return e == std::string::npos ? "" : text.substr(p + 1, e - p - 1);
}

void RosBridge::on_message(const std::string& text, int depth) {
    std::string op = quick_field(text, "op", 64);
    if (op == "png" && depth == 0) {  // {"op":"png","data":"<base64 PNG>"} → 원래 메시지
        std::string data = quick_field(text, "data", 64), inner;
        // quick_field 는 짧은 범위만 보므로 data 는 직접 찾는다
        size_t p = text.find("\"data\"");
        if (p != std::string::npos) {
            size_t a = text.find('"', text.find(':', p) + 1), b = text.find('"', a + 1);
            if (a != std::string::npos && b != std::string::npos) data = text.substr(a + 1, b - a - 1);
        }
        if (rosbridge_png_to_text(data, inner)) on_message(inner, depth + 1);
        return;
    }
    if (op == "publish") {
        std::string topic = quick_field(text, "topic", 4096);
        RawFn raw;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = subs_.find(topic);
            if (it != subs_.end()) raw = it->second.raw;
        }
        if (raw) {
            ++rx_;
            raw(text);
            return;
        }
    }
    Json m;
    try {
        m = Json::parse(text);
    } catch (const std::exception&) {
        return;
    }
    ++rx_;
    op = m["op"].as_string();
    if (op == "publish") {
        TopicFn fn;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = subs_.find(m["topic"].as_string());
            if (it != subs_.end()) fn = it->second.fn;
        }
        if (fn) fn(m["msg"]);
    } else if (op == "service_response") {
        ServiceFn fn;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pending_.find(m["id"].as_string());
            if (it != pending_.end()) {
                fn = std::move(it->second);
                pending_.erase(it);
            }
        }
        if (fn) fn(m["result"].as_bool(false), m["values"]);
    } else if (op == "status") {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "rosbridge: " + m["msg"].as_string();
    }
}

void RosBridge::loop() {
    while (running_) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_ = "connecting";
        }
        std::string err;
        sock_t fd = tcp_connect(host_, port_, 2000, &err);
        if (fd != kBadSock) sock_set_recv_timeout(fd, 2000);
        bool shook = fd != kBadSock && handshake(fd);
        if (fd != kBadSock) sock_set_recv_timeout(fd, 0);  // 0 = 제한 없음. 끊김은 TCP 가 알려 준다
        if (shook) {
            {
                std::lock_guard<std::mutex> lk(fd_mu_);
                fd_ = fd;
            }
            connected_ = true;
            {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = "connected";
            }
            // 구독·광고 되살리기
            std::map<std::string, Sub> subs;
            std::map<std::string, std::string> ads;
            {
                std::lock_guard<std::mutex> lk(mu_);
                subs = subs_;
                ads = ads_;
            }
            for (const auto& a : ads)
                send_text(Json::object().set("op", "advertise").set("topic", a.first).set("type", a.second).dump());
            for (const auto& s : subs) send_text(subscribe_msg(s.first, s.second).dump());
            int op = 0;
            std::string payload;
            while (running_ && recv_frame(fd, op, payload)) {
                if (op == 0x8) break;
                if (op == 0x1) on_message(payload);
            }
            connected_ = false;
            {
                std::lock_guard<std::mutex> lk(fd_mu_);
                std::lock_guard<std::mutex> sk(send_mu_);  // 보내는 중인 프레임이 끝난 뒤 닫는다
                fd_ = kBadSock;
                sock_close(fd);
            }
            std::map<std::string, ServiceFn> dropped;
            {
                std::lock_guard<std::mutex> lk(mu_);
                dropped.swap(pending_);
                status_ = "lost";
            }
            for (auto& p : dropped)
                if (p.second) p.second(false, Json::object().set("message", "연결 끊김"));
        } else {
            if (fd != kBadSock) sock_close(fd);
            std::lock_guard<std::mutex> lk(mu_);
            status_ = "error: " + (err.empty() ? std::string("handshake 실패") : err);
        }
        for (int i = 0; i < 20 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace hg
