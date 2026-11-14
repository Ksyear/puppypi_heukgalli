// 흑갈리 가속도계(Windows Sensor API)로 기기 기울기를 읽는다. 센서가 없거나 권한이 없으면 available() == false.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace hg {

class TiltSensor {
public:
    ~TiltSensor() { stop(); }
    void start(void* hwnd);
    void stop();
    bool available() const { return available_; }
    std::string status() const;
    // 중력 방향(단위 g). 마지막 읽은 값
    bool gravity(float& x, float& y, float& z) const;

private:
    void loop(void* hwnd);
    std::thread th_;
    std::atomic<bool> run_{false}, available_{false};
    mutable std::mutex mu_;
    float gx_ = 0, gy_ = 0, gz_ = 0;
    bool have_ = false;
    std::string status_ = "꺼짐";
};

// 기울기 → 운전 입력. 기준 자세(잡는 순간의 중력)와의 앞뒤·좌우 각도 차이를 데드존 5°, 최대 25° 로 -1..1 에 대응
struct TiltDrive {
    bool calibrated = false;
    float bx = 0, by = 0, bz = 0;
    bool invert_forward = false, invert_turn = false;
    void calibrate(float x, float y, float z);
    void map(float x, float y, float z, double& forward, double& turn) const;
};

}  // namespace hg
