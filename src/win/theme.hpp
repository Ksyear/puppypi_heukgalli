// 디자인 토큰(색·글자 크기·간격)과 글꼴 로드. 근거: docs/DESIGN_PLAN_v1.md §3, §6.1
#pragma once
#include <string>

#include "imgui.h"

namespace hg::theme {

struct Tokens {
    ImU32 bg0, bg1, bg2, bg3, line;
    ImU32 text1, text2, text3;
    ImU32 accent, ok, warn, danger, danger_deep, info, lidar, ink;
};
extern const Tokens T;

// 글자 크기(논리 px). ImGui::PushFont(font, sz::title) 처럼 쓴다. 동적 글꼴 아틀라스가 크기마다
// 글리프를 새로 굽기 때문에 이 프리셋만 쓴다.
namespace sz {
constexpr float body = 18, label = 15, hint = 14, title = 22, button = 17, num_lg = 28, num_sm = 16, estop = 22, icon = 20,
                icon_lg = 28;
}

struct Fonts {
    ImFont* body = nullptr;  // 본문(아이콘·기호 병합)
    ImFont* semi = nullptr;  // 버튼 라벨
    ImFont* bold = nullptr;  // 제목·안전 문구
    ImFont* num = nullptr;   // 숫자(고정폭)
    bool icons = false;      // 아이콘 글꼴을 찾았는지(없으면 약어로 대신)
    std::string body_name;
};
extern Fonts F;

// fonts_dir: %WINDIR%\Fonts\ , exe_dir: 실행 파일 폴더(번들 글꼴 fonts\ 를 찾는다)
void load_fonts(const std::string& fonts_dir, const std::string& exe_dir);
void apply_style(float dpi_scale);
ImU32 alpha(ImU32 c, float a);   // 알파만 a 배
ImU32 shade(ImU32 c, float k);   // 밝기 k 배(1 보다 크면 밝게)

// Segoe Fluent Icons / MDL2 Assets 코드포인트(UTF-8)
namespace icon {
constexpr const char* drive = "\xEE\x9F\xBC";     // E7FC Game
constexpr const char* map = "\xEE\xA0\x9E";       // E81E MapLayers
constexpr const char* robot = "\xEE\xA6\x9A";     // E99A Robot
constexpr const char* log = "\xEE\xA0\x9C";       // E81C History
constexpr const char* settings = "\xEE\x9C\x93";  // E713 Setting
constexpr const char* layers = "\xEE\xA0\x9E";    // E81E
constexpr const char* camera = "\xEE\x9C\xA2";    // E722 Camera
constexpr const char* home = "\xEE\xA0\x8F";      // E80F Home
constexpr const char* send = "\xEE\x9C\xA4";      // E724 Send
constexpr const char* cancel = "\xEE\x9C\x91";    // E711 Cancel
constexpr const char* erase = "\xEE\x9D\x8D";     // E74D Delete
constexpr const char* zoom = "\xEE\x9C\x9E";      // E71E Zoom
constexpr const char* save = "\xEE\x9D\x8E";      // E74E Save
constexpr const char* refresh = "\xEE\x9C\xAC";   // E72C Refresh
constexpr const char* record = "\xEE\x9F\x88";    // E7C8 Record
constexpr const char* list = "\xEE\xA3\xBD";      // E8FD BulletedList
constexpr const char* power = "\xEE\x9F\xA8";     // E7E8 PowerButton
constexpr const char* search = "\xEE\x9C\xA1";    // E721 Search
constexpr const char* wifi = "\xEE\x9C\x81";      // E701 Wifi
constexpr const char* warning = "\xEE\x9E\xBA";   // E7BA Warning
constexpr const char* error = "\xEE\x9E\x83";     // E783 Error
constexpr const char* lock = "\xEE\x9C\xAE";      // E72E Lock
constexpr const char* gamepad = "\xEE\x9F\xBC";   // E7FC
constexpr const char* play = "\xEE\x9D\xA8";      // E768 Play
}  // namespace icon

}  // namespace hg::theme
