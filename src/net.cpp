#include "net.hpp"

#include "sock.hpp"
#include "util.hpp"

#include <cstring>

namespace hg {

bool resolve_ipv4(const std::string& host, uint16_t port, sockaddr_in& out) {
    std::memset(&out, 0, sizeof out);
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

std::string addr_text(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

sock_t tcp_connect(const std::string& host, uint16_t port, int timeout_ms, std::string* err) {
    sockaddr_in addr{};
    if (!resolve_ipv4(host, port, addr)) {
        if (err) *err = "주소 해석 실패";
        return kBadSock;
    }
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kBadSock) {
        if (err) *err = sock_err_text(sock_err());
        return kBadSock;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    sock_set_nonblock(fd, true);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    if (rc != 0) {
        int e = sock_err();
        if (!sock_would_block(e)) {
            if (err) *err = sock_err_text(e);
            sock_close(fd);
            return kBadSock;
        }
        sock_pollfd p{};
        p.fd = fd;
        p.events = POLLOUT;
        rc = sock_poll(&p, 1, timeout_ms);
        if (rc <= 0) {
            if (err) *err = rc == 0 ? "시간 초과" : sock_err_text(sock_err());
            sock_close(fd);
            return kBadSock;
        }
        int so_err = 0;
        socklen_t len = sizeof so_err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &len);
        if (so_err != 0 || (p.revents & (POLLERR | POLLHUP))) {
            if (err) *err = so_err ? sock_err_text(so_err) : "연결 거부";
            sock_close(fd);
            return kBadSock;
        }
    }
    sock_set_nonblock(fd, false);
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    return fd;
}

bool send_all(sock_t fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#ifdef MSG_NOSIGNAL
        auto n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#else
        auto n = send(fd, data.data() + off, static_cast<int>(data.size() - off), 0);
#endif
        if (n <= 0) {
            if (n < 0 && sock_interrupted(sock_err())) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

std::string HttpResponse::header(const std::string& name) const {
    std::string key = lower(name) + ":";
    for (const auto& line : split(headers, '\n')) {
        std::string l = trim(line);
        if (starts_with(lower(l), key)) return trim(l.substr(key.size()));
    }
    return "";
}

HttpResponse http_get(const std::string& host, uint16_t port, const std::string& path, int timeout_ms) {
    HttpResponse r;
    sock_t fd = tcp_connect(host, port, timeout_ms, &r.error);
    if (fd == kBadSock) return r;
    sock_set_recv_timeout(fd, timeout_ms);
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nUser-Agent: heukgalli/1\r\nConnection: close\r\n\r\n";
    if (!send_all(fd, req)) {
        r.error = "요청 전송 실패";
        sock_close(fd);
        return r;
    }
    std::string data;
    char buf[16384];
    double deadline = now_s() + timeout_ms / 1000.0 * 2;
    while (now_s() < deadline) {
        auto n = recv(fd, buf, sizeof buf, 0);
        if (n > 0) {
            data.append(buf, static_cast<size_t>(n));
            if (data.size() > 8 * 1024 * 1024) break;
        } else {
            if (n < 0 && sock_interrupted(sock_err())) continue;
            break;
        }
    }
    sock_close(fd);
    size_t he = data.find("\r\n\r\n");
    if (he == std::string::npos) {
        r.error = data.empty() ? "응답 없음" : "헤더 끝 없음";
        return r;
    }
    r.headers = data.substr(0, he);
    r.body = data.substr(he + 4);
    auto sp = r.headers.find(' ');  // "HTTP/1.x 200 OK"
    if (sp != std::string::npos) r.status = std::atoi(r.headers.c_str() + sp + 1);
    std::string cl = r.header("Content-Length");
    if (!cl.empty()) {
        size_t want = static_cast<size_t>(std::atoll(cl.c_str()));
        if (r.body.size() > want) r.body.resize(want);
    }
    return r;
}

// ---------------- MJPEG ----------------

MjpegReader::MjpegReader(std::string host, uint16_t port, std::string path)
    : host_(std::move(host)), path_(std::move(path)), port_(port) {}

MjpegReader::~MjpegReader() { stop(); }

void MjpegReader::start() {
    if (running_) return;
    running_ = true;
    th_ = std::thread([this] { loop(); });
}

void MjpegReader::stop() {
    running_ = false;
    {
        std::lock_guard<std::mutex> lk(fd_mu_);
        if (fd_ != kBadSock) shutdown(fd_, SHUT_RDWR);  // 수신 스레드가 recv 에서 깨어나 직접 닫는다
    }
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lk(mu_);
    status_ = "stopped";
}

bool MjpegReader::latest(std::string& jpeg, double& rx_time, uint64_t& seq) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (frame_.empty()) return false;
    jpeg = frame_;
    rx_time = frame_time_;
    seq = seq_;
    return true;
}

double MjpegReader::fps() const {
    std::lock_guard<std::mutex> lk(mu_);
    double t = now_s();
    int n = 0;
    for (double x : times_)
        if (t - x <= 2.0) ++n;
    return n / 2.0;
}

std::string MjpegReader::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

void MjpegReader::loop() {
    while (running_) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_ = "connecting";
        }
        std::string err;
        sock_t fd = tcp_connect(host_, port_, 2000, &err);
        if (fd != kBadSock) {
            {
                std::lock_guard<std::mutex> lk(fd_mu_);
                fd_ = fd;
            }
            sock_set_recv_timeout(fd, 2500);
            bool ok = read_stream(fd);
            {
                std::lock_guard<std::mutex> lk(fd_mu_);
                fd_ = kBadSock;
                sock_close(fd);
            }
            std::lock_guard<std::mutex> lk(mu_);
            if (status_ == "streaming" || status_ == "connecting") status_ = ok ? "closed" : "lost";
        } else {
            std::lock_guard<std::mutex> lk(mu_);
            status_ = "error: " + err;
        }
        for (int i = 0; i < 20 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// 부분 헤더의 Content-Length 로 읽고, 없으면 JPEG 끝(FF D9)까지 읽는다(로봇 문서 18 §3).
bool MjpegReader::read_stream(sock_t fd) {
    std::string req = "GET " + path_ + " HTTP/1.0\r\nHost: " + host_ + "\r\nUser-Agent: heukgalli/1\r\n\r\n";
    if (!send_all(fd, req)) return false;
    std::string buf;
    std::string chunk(65536, '\0');
    bool header_done = false;
    while (running_) {
        auto n = recv(fd, &chunk[0], static_cast<int>(chunk.size()), 0);
        if (n <= 0) {
            if (n < 0 && sock_interrupted(sock_err())) continue;
            return false;
        }
        buf.append(chunk.data(), static_cast<size_t>(n));
        if (!header_done) {
            size_t he = buf.find("\r\n\r\n");
            if (he == std::string::npos) {
                if (buf.size() > 65536) return false;
                continue;
            }
            std::string first = buf.substr(0, buf.find("\r\n"));
            if (first.find(" 200") == std::string::npos) {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = "http: " + first;
                return false;
            }
            header_done = true;
            buf.erase(0, he + 4);
            std::lock_guard<std::mutex> lk(mu_);
            status_ = "streaming";
        }
        while (true) {
            size_t ph = buf.find("\r\n\r\n");
            if (ph == std::string::npos) break;
            std::string part_head = lower(buf.substr(0, ph));
            size_t body = ph + 4;
            size_t len = std::string::npos;
            size_t cl = part_head.find("content-length:");
            if (cl != std::string::npos) len = static_cast<size_t>(std::atoll(part_head.c_str() + cl + 15));
            std::string jpeg;
            if (len != std::string::npos) {
                if (len > 4 * 1024 * 1024) return false;  // 비정상적으로 큰 부분은 연결을 끊는다
                if (buf.size() < body + len) break;
                jpeg = buf.substr(body, len);
                buf.erase(0, body + len);
            } else {
                size_t soi = buf.find("\xFF\xD8", body);
                if (soi == std::string::npos) break;
                size_t eoi = buf.find("\xFF\xD9", soi + 2);
                if (eoi == std::string::npos) break;
                jpeg = buf.substr(soi, eoi + 2 - soi);
                buf.erase(0, eoi + 2);
            }
            if (jpeg.size() > 4 && uint8_t(jpeg[0]) == 0xFF && uint8_t(jpeg[1]) == 0xD8) {
                std::lock_guard<std::mutex> lk(mu_);
                frame_ = std::move(jpeg);
                frame_time_ = now_s();
                ++seq_;
                times_.push_back(frame_time_);
                while (times_.size() > 120) times_.erase(times_.begin());
            }
            size_t b = buf.find("--");  // 다음 경계 앞의 CRLF 제거
            if (b == std::string::npos) break;
            buf.erase(0, b);
        }
        if (buf.size() > 16 * 1024 * 1024) buf.clear();
    }
    return true;
}

}  // namespace hg
