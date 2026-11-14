#include "lidar.hpp"

#include "json.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace hg {

double yaw_from_quaternion(double qx, double qy, double qz, double qw) {
    return std::atan2(2.0 * (qw * qz + qx * qy), 1.0 - 2.0 * (qy * qy + qz * qz));
}

bool parse_pose_stamped(const Json& msg, Pose2D& out) {
    const Json& p = msg["pose"];
    if (!p["position"].is_object() || !p["orientation"].is_object()) return false;
    out.x = p["position"]["x"].as_number();
    out.y = p["position"]["y"].as_number();
    const Json& q = p["orientation"];
    out.yaw = yaw_from_quaternion(q["x"].as_number(), q["y"].as_number(), q["z"].as_number(), q["w"].as_number(1.0));
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.yaw);
}

bool parse_occupancy_grid(const std::string& text, OccGrid& out) {
    // 1) "data":[ ... ] 를 찾아 정수를 직접 읽는다(600×600 = 36만 개)
    size_t key = text.find("\"data\"");
    if (key == std::string::npos) return false;
    size_t lb = text.find('[', key);
    if (lb == std::string::npos) return false;
    std::vector<int8_t> cells;
    cells.reserve(360000);
    const char* p = text.c_str() + lb + 1;
    const char* end = text.c_str() + text.size();
    size_t rb = std::string::npos;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        if (p >= end) break;
        if (*p == ']') {
            rb = static_cast<size_t>(p - text.c_str());
            break;
        }
        char* q = nullptr;
        long v = std::strtol(p, &q, 10);
        if (q == p) return false;
        if (v < -1 || v > 100) v = -1;
        cells.push_back(static_cast<int8_t>(v));
        if (cells.size() > 4096u * 4096u) return false;
        p = q;
    }
    if (rb == std::string::npos) return false;
    // 2) 나머지(info)는 data 를 빈 배열로 바꾼 뒤 일반 해석
    std::string rest = text.substr(0, lb) + "[]" + text.substr(rb + 1);
    Json j;
    try {
        j = Json::parse(rest);
    } catch (const std::exception&) {
        return false;
    }
    const Json& msg = j.has("msg") ? j["msg"] : j;
    const Json& info = msg["info"];
    out.w = static_cast<int>(info["width"].as_int());
    out.h = static_cast<int>(info["height"].as_int());
    out.res = info["resolution"].as_number(0.05);
    out.ox = info["origin"]["position"]["x"].as_number();
    out.oy = info["origin"]["position"]["y"].as_number();
    if (out.w <= 0 || out.h <= 0 || out.w > 4096 || out.h > 4096 || cells.size() != static_cast<size_t>(out.w) * out.h) return false;
    out.cells = std::move(cells);
    out.x0 = out.w;
    out.y0 = out.h;
    out.x1 = out.y1 = -1;
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            if (out.cells[static_cast<size_t>(y) * out.w + x] >= 0) {
                if (x < out.x0) out.x0 = x;
                if (x > out.x1) out.x1 = x;
                if (y < out.y0) out.y0 = y;
                if (y > out.y1) out.y1 = y;
            }
    return true;
}

std::vector<std::pair<float, float>> scan_to_map(const std::vector<float>& ranges, double angle_min, double angle_inc,
                                                 double range_max, const Pose2D& pose) {
    std::vector<std::pair<float, float>> pts;
    pts.reserve(ranges.size());
    double c = std::cos(pose.yaw), s = std::sin(pose.yaw);
    for (size_t i = 0; i < ranges.size(); ++i) {
        float r = ranges[i];
        if (!(r > 0.05f) || r >= range_max) continue;
        double a = angle_min + angle_inc * static_cast<double>(i);
        double lx = r * std::cos(a), ly = r * std::sin(a);
        pts.emplace_back(static_cast<float>(pose.x + c * lx - s * ly), static_cast<float>(pose.y + s * lx + c * ly));
    }
    return pts;
}

}  // namespace hg
