// 소켓 도구: UDP, 시간 제한 TCP 연결, 간단한 HTTP GET, MJPEG(multipart JPEG) 수신기.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sock.hpp"

namespace hg {

bool resolve_ipv4(const std::string& host, uint16_t port, sockaddr_in& out);
std::string addr_text(const sockaddr_in& a);  // "1.2.3.4:5"

// 시간 제한 TCP 연결. 실패 시 kBadSock, err 에 이유
sock_t tcp_connect(const std::string& host, uint16_t port, int timeout_ms, std::string* err = nullptr);
bool send_all(sock_t fd, const std::string& data);

struct HttpResponse {
    int status = 0;  // 0 = 연결·응답 실패
    std::string headers;
    std::string body;
    std::string error;
    std::string header(const std::string& name) const;
};
HttpResponse http_get(const std::string& host, uint16_t port, const std::string& path, int timeout_ms = 1500);

// web_video_server 의 multipart/x-mixed-replace JPEG 를 받아 최신 1장만 보관. 끊기면 2 s 뒤 다시 연결.
class MjpegReader {
public:
    MjpegReader(std::string host, uint16_t port, std::string path);
    ~MjpegReader();
    void start();
    void stop();
    bool running() const { return running_; }
    // 최신 프레임(없으면 빈 문자열)과 받은 시각·번호
    bool latest(std::string& jpeg, double& rx_time, uint64_t& seq) const;
    double fps() const;
    std::string status() const;

private:
    void loop();
    bool read_stream(sock_t fd);
    std::string host_, path_;
    uint16_t port_;
    std::atomic<bool> running_{false};
    std::thread th_;
    mutable std::mutex mu_;
    std::string frame_;
    double frame_time_ = 0;
    uint64_t seq_ = 0;
    std::vector<double> times_;
    std::string status_ = "stopped";
    std::atomic<sock_t> fd_{kBadSock};
};

}  // namespace hg
