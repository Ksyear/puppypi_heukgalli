// 공용 도구: 시각, UUID, 문자열, base64, 이벤트 기록.
#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace hg {

// 로봇 쪽과 같은 단조 시계(초)
double now_s();
// 사람이 읽는 벽시계 HH:MM:SS.mmm
std::string clock_text();

std::string uuid4();           // 하이픈 포함 36자, 소문자
std::string random_hex(int n);  // n 글자 16진수

std::vector<std::string> split_ws(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
std::string trim(const std::string& s);
std::string lower(std::string s);
bool starts_with(const std::string& s, const std::string& p);
#ifdef __MINGW32__
std::string fmt(const char* f, ...) __attribute__((format(gnu_printf, 1, 2)));  // UCRT 는 %zu 등 C99 형식을 지원
#else
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
#endif

std::string base64_encode(const std::string& in);

// 화면 아래 이벤트 창과 sniff 창에 쓰는 고정 길이 기록(스레드 안전)
class LogRing {
public:
    explicit LogRing(size_t cap = 400) : cap_(cap) {}
    void add(const std::string& line);
    std::vector<std::string> tail(size_t n) const;
    uint64_t version() const;
    // cursor 이후에 추가된 줄(버퍼에 남은 것만)을 돌려주고 cursor 를 갱신
    std::vector<std::string> since(uint64_t& cursor) const;

private:
    size_t cap_;
    mutable std::mutex mu_;
    std::deque<std::string> lines_;
    uint64_t version_ = 0;
};

}  // namespace hg
