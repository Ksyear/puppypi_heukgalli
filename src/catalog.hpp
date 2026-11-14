// 로봇이 제공하는 자세 프리셋·동작 그룹 목록(팀 로봇 문서 16과 같은 값). 2_legs_stand 는 로봇이 거부하므로 넣지 않는다.
#pragma once
#include <string>
#include <vector>

namespace hg {

struct PosePreset {
    const char* name;
    const char* label;  // 화면 표시
    double roll, pitch, yaw;  // rad
    double height, x_shift, stance_x, stance_y;  // cm
};

constexpr double kDeg = 3.141592653589793 / 180.0;

inline const std::vector<PosePreset>& pose_presets() {
    static const std::vector<PosePreset> p = {
        {"Stand", "서기", 0, 0, 0, -10.0, -0.5, 0, 0},
        {"LieDown", "엎드리기", 0, 0, 0, -5.0, 2.0, 0, 0},
        {"LookDown", "내려보기", 0, -15 * kDeg, 0, -10.0, -0.5, 0, 0},
        {"LookDown_10deg", "내려보기 10°", 0, -10 * kDeg, 0, -9.0, -0.1, 0, 0},
        {"LookDown_20deg", "내려보기 20°", 0, -20 * kDeg, 0, -9.0, -0.1, 0, 0},
        {"LookDown_30deg", "내려보기 30°", 0, -30 * kDeg, 0, -9.6, -1.4, 1.0, 0},
        {"StandLow", "낮게 서기", 0, 0, 0, -7.0, -0.5, 0, 0},
    };
    return p;
}

struct ActionGroup {
    const char* name;   // 로봇 파일 이름(.d6ac 제외)
    const char* label;
    bool caution;       // 몸이 크게 움직이거나 오래 걸림 → 화면에서 한 번 더 확인
};

inline const std::vector<ActionGroup>& action_groups() {
    static const std::vector<ActionGroup> a = {
        {"wave", "손 흔들기", false},       {"bow", "인사", false},
        {"nod", "끄덕", false},             {"shake_head", "도리도리", false},
        {"shake_hands", "악수", false},     {"sit", "앉기", false},
        {"stand", "일어서기", false},       {"lie_down", "눕기", false},
        {"stretch", "기지개", false},       {"pee", "영역 표시", false},
        {"push-up", "팔굽혀펴기", true},    {"moonwalk", "문워크", true},
        {"spacewalk", "스페이스워크", true}, {"boxing", "복싱", true},
        {"boxing2", "복싱 2", true},        {"kick_ball_left", "왼발 차기", true},
        {"kick_ball_left_bak", "왼발 차기(예비)", true}, {"kick_ball_right", "오른발 차기", true},
        {"jump", "점프", true},             {"up_stairs_2cm", "계단 2cm", true},
        {"up_stairs_3.5cm", "계단 3.5cm", true}, {"up_stairs_3.5cm0", "계단 3.5cm (0)", true},
        {"up_stairs_3.5cm1", "계단 3.5cm (1)", true}, {"temp", "temp", true},
    };
    return a;
}

}  // namespace hg
