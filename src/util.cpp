#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <random>
#include <sstream>

namespace hg {

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string clock_text() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t sec = system_clock::to_time_t(now);
    int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &sec);
#else
    localtime_r(&sec, &t);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", t.tm_hour, t.tm_min, t.tm_sec, ms);
    return buf;
}

static std::mt19937_64& rng() {
    static std::mt19937_64 g{std::random_device{}()};
    return g;
}

std::string random_hex(int n) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    for (int i = 0; i < n; ++i) s += hex[rng()() & 15];
    return s;
}

std::string uuid4() {
    std::string h = random_hex(32);
    h[12] = '4';
    h[16] = "89ab"[h[16] % 4];
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string base64_encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool base64_decode(const std::string& in, std::string& out) {
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(t[i])] = static_cast<int8_t>(i);
        init = true;
    }
    out.clear();
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (c == '=' ) break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = table[c];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return true;
}

void LogRing::add(const std::string& line) {
    std::lock_guard<std::mutex> lk(mu_);
    lines_.push_back(clock_text() + " " + line);
    while (lines_.size() > cap_) lines_.pop_front();
    ++version_;
}

std::vector<std::string> LogRing::tail(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t start = lines_.size() > n ? lines_.size() - n : 0;
    return std::vector<std::string>(lines_.begin() + static_cast<long>(start), lines_.end());
}

std::vector<std::string> LogRing::since(uint64_t& cursor) const {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t fresh = version_ > cursor ? version_ - cursor : 0;
    size_t n = static_cast<size_t>(std::min<uint64_t>(fresh, lines_.size()));
    cursor = version_;
    return std::vector<std::string>(lines_.end() - static_cast<long>(n), lines_.end());
}

uint64_t LogRing::version() const {
    std::lock_guard<std::mutex> lk(mu_);
    return version_;
}

}  // namespace hg
