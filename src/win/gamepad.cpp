#include "gamepad.hpp"

#include <windows.h>
#include <xinput.h>

#include <cmath>

namespace hg {

namespace {
// 원형 데드존 뒤 0..1 로 다시 펴고, 지수 곡선으로 작은 입력을 더 세밀하게
void stick(SHORT sx, SHORT sy, float dz, float expo, float& ox, float& oy) {
    float x = sx / 32767.0f, y = sy / 32767.0f;
    float m = std::sqrt(x * x + y * y);
    if (m < dz) {
        ox = oy = 0;
        return;
    }
    float scaled = std::fmin(1.0f, (m - dz) / (1.0f - dz));
    float curved = std::pow(scaled, expo);
    ox = std::fmax(-1.0f, std::fmin(1.0f, x / m * curved));
    oy = std::fmax(-1.0f, std::fmin(1.0f, y / m * curved));
}

float trigger(BYTE v) {
    float t = (v - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) / float(255 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
    return std::fmax(0.0f, std::fmin(1.0f, t));
}
}  // namespace

void Gamepad::poll(double now) {
    XINPUT_STATE xs{};
    bool ok = false;
    if (st_.index >= 0 && XInputGetState(static_cast<DWORD>(st_.index), &xs) == ERROR_SUCCESS) {
        ok = true;
    } else if (now >= next_scan_) {
        // 연결 안 된 슬롯을 매 프레임 찾으면 느리므로 0.5 s 마다
        next_scan_ = now + 0.5;
        st_.index = -1;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
            if (XInputGetState(i, &xs) == ERROR_SUCCESS) {
                st_.index = static_cast<int>(i);
                ok = true;
                break;
            }
    }
    st_.connected = ok;
    if (!ok) {
        st_.lx = st_.ly = st_.rx = st_.ry = st_.lt = st_.rt = 0;
        st_.buttons = 0;
    } else {
        const XINPUT_GAMEPAD& g = xs.Gamepad;
        stick(g.sThumbLX, g.sThumbLY, deadzone, expo, st_.lx, st_.ly);
        stick(g.sThumbRX, g.sThumbRY, deadzone, expo, st_.rx, st_.ry);
        st_.lt = trigger(g.bLeftTrigger);
        st_.rt = trigger(g.bRightTrigger);
        st_.buttons = g.wButtons;
    }
    st_.pressed = static_cast<uint16_t>(st_.buttons & ~prev_);
    st_.released = static_cast<uint16_t>(prev_ & ~st_.buttons);
    prev_ = st_.buttons;

    // 진동 순서 재생
    if (motor_on_ && now >= seg_end_) {
        if (queue_.empty()) {
            apply(0, 0);
            motor_on_ = false;
        }
    }
    if (!queue_.empty() && now >= seg_end_) {
        Seg s = queue_.front();
        queue_.pop_front();
        apply(s.left, s.right);
        motor_on_ = true;
        seg_end_ = now + s.dur;
    }
}

void Gamepad::apply(uint16_t l, uint16_t r) {
    if (st_.index < 0) return;
    XINPUT_VIBRATION v{l, r};
    XInputSetState(static_cast<DWORD>(st_.index), &v);
}

void Gamepad::rumble(Rumble r) {
    switch (r) {
        case Rumble::Tick: queue_.push_back({0, 22000, 0.06}); break;
        case Rumble::Double:
            queue_.push_back({30000, 30000, 0.09});
            queue_.push_back({0, 0, 0.08});
            queue_.push_back({30000, 30000, 0.09});
            break;
        case Rumble::Long: queue_.push_back({65535, 65535, 0.6}); break;
        case Rumble::Done:
            queue_.push_back({0, 26000, 0.07});
            queue_.push_back({0, 0, 0.06});
            queue_.push_back({26000, 0, 0.12});
            break;
    }
    if (queue_.size() > 12) queue_.erase(queue_.begin(), queue_.end() - 12);
}

void Gamepad::stop_rumble() {
    queue_.clear();
    apply(0, 0);
    motor_on_ = false;
}

}  // namespace hg
