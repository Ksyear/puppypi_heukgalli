// 흑갈리 내장 패드(XInput): 상태 읽기, 데드존·곡선, 버튼 눌림/뗌 감지, 진동 패턴.
#pragma once
#include <cstdint>
#include <deque>

namespace hg {

enum PadButton : uint16_t {
    kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
    kMenu = 0x0010, kView = 0x0020, kLeftStick = 0x0040, kRightStick = 0x0080,
    kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};

struct PadState {
    bool connected = false;
    int index = -1;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // 데드존·곡선 적용 후 -1..1 (위·오른쪽 +)
    float lt = 0, rt = 0;                  // 0..1
    uint16_t buttons = 0, pressed = 0, released = 0;
    bool down(uint16_t b) const { return (buttons & b) != 0; }
    bool hit(uint16_t b) const { return (pressed & b) != 0; }
};

enum class Rumble {
    Tick,     // 짧고 약하게: 명령 수락·동작 시작
    Double,   // 두 번: 거부·연결 문제
    Long,     // 길고 강하게: 비상정지
    Done,     // 경로 도착
};

class Gamepad {
public:
    void poll(double now);
    const PadState& state() const { return st_; }
    void rumble(Rumble r);
    void stop_rumble();
    float deadzone = 0.12f;
    float expo = 1.5f;

private:
    struct Seg {
        uint16_t left, right;
        double dur;
    };
    void apply(uint16_t l, uint16_t r);
    PadState st_;
    uint16_t prev_ = 0;
    std::deque<Seg> queue_;
    double seg_end_ = 0;
    bool motor_on_ = false;
    double next_scan_ = 0;
};

}  // namespace hg
