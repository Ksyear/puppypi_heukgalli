// 직접 그린 UI 부품. 모두 "InvisibleButton → 상태 읽기 → 드로우 리스트" 순서라 터치·마우스는 ImGui 가 처리한다.
// 근거: docs/DESIGN_PLAN_v1.md §5
#pragma once
#include "imgui.h"

namespace hg::w {

enum class Tone { Neutral, Accent, Ok, Warn, Danger, Info, Muted };
enum class Pad { A, B, X, Y, LB, RB, LT, RT, LS, DpadUD, DpadLR, View, Menu };

ImU32 tone_color(Tone t);
float S();  // 현재 DPI 배율(theme::apply_style 의 값)
void set_scale(float s);

void shadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r);
void card(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r = 12);
void text_shadowed(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* txt);
ImVec2 text_size(ImFont* f, float size, const char* txt);
void pad_glyph(ImDrawList* dl, Pad p, ImVec2 center, float size);

// 상태 칩. filled = 면을 채우고 어두운 글자. 너비를 돌려준다
float chip(ImDrawList* dl, ImVec2 pos, float h, const char* icon, const char* text, Tone t, bool filled, ImFont* f = nullptr);
float chip_width(float h, const char* icon, const char* text, ImFont* f = nullptr);

// 아이콘 버튼. vertical = 아이콘 위·라벨 아래(왼쪽 레일), 아니면 아이콘 왼쪽·라벨·패드 글리프 오른쪽
bool icon_button(const char* id, const char* icon, const char* label, ImVec2 size, Tone t = Tone::Neutral,
                 const Pad* hint = nullptr, bool enabled = true, bool selected = false, bool vertical = false);
bool segmented(const char* id, const char* const* items, int n, int* index, ImVec2 size);
bool toggle(const char* id, const char* label, bool* v);

// 비상정지. engaged = 비상정지 중(해제 모드), hold_k = 해제 홀드 진행 0..1(음수면 표시 안 함)
// 반환: 정상 모드에서 눌림. 해제 모드에서는 *holding 에 누르고 있는지를 알려 준다
bool estop(const char* id, ImVec2 size, bool engaged, bool connected, float hold_k, bool* holding, double now);

// 주행 상태 캡슐: lamp = "○ ◐ ● ✕" 중 하나
void drive_capsule(ImDrawList* dl, ImVec2 pos, ImVec2 size, Tone t, bool filled, const char* lamp, const char* text);
void cmd_bar(ImDrawList* dl, ImVec2 pos, float w, const char* name, double v, bool active);
void toast(ImDrawList* dl, ImVec2 center_top, const char* text, Tone t, float alpha, float rise);
void empty_state(ImDrawList* dl, ImVec2 center, const char* icon, const char* title, const char* hint);
void stat_tile(const char* id, const char* label, const char* big, const char* small, ImVec2 size, Tone t = Tone::Neutral);
void area_plot(const char* id, const float* v, int n, float vmax, float threshold, ImVec2 size);

// ---- v2 "검은 유리 조종석"(docs/DESIGN_PLAN_v2.md §2.3, §4) ----
enum class Lamp { Ring, Half, Full, Cross };
void glass_panel(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r);   // 반투명 유리(외곽선은 안쪽 1 px 만)
void plate(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r);         // 무테 면
void scrim(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float a_top, float a_bottom, ImU32 base = 0);
// 숫자를 "0" 폭으로 고정 자간해 그린다(값이 바뀌어도 흔들리지 않게). 너비를 돌려준다
float num_text(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* txt, bool align_right = false);
float num_width(ImFont* f, float size, const char* txt);
// 필 버튼: 폭은 글자에 맞춘다(라벨을 자르지 않음). sub 는 두 번째 줄(작게)
bool pill_button(const char* id, const char* label, float h, Tone t = Tone::Neutral, const char* icon = nullptr,
                 const Pad* hint = nullptr, const char* sub = nullptr, bool enabled = true, float min_w = 0, bool selected = false,
                 ImU32 text_col = 0);
float pill_width(const char* label, float h, const char* icon = nullptr, const Pad* hint = nullptr, const char* sub = nullptr);
// 둥근 플로팅 버튼(Ø size) + 패드 글리프 배지. label 은 버튼 왼쪽에 작게
bool fab(const char* id, const char* icon, float size, const Pad* hint = nullptr, bool selected = false, bool enabled = true,
         const char* label = nullptr);
bool segmented2(const char* id, const char* const* items, int n, int* index, float h, const char* const* icons = nullptr);
bool toggle2(const char* id, const char* label, bool* v, float width);
bool slider2(const char* id, const char* label, float* v, float lo, float hi, const char* fmt, float width);
float capsule2(ImDrawList* dl, ImVec2 pos, float h, float min_w, Tone t, bool filled, Lamp lamp, const char* text);
float capsule2_width(float h, float min_w, const char* text);
// 화면 전환 필: 아이콘 n 개 + 현재 화면 이름. anim_x 는 선택 표시 위치(보간용, 호출자가 보관)
bool nav_pill(const char* id, const char* const* icons, const char* const* names, int n, int* index, float* anim_x, double dt);
void sparkline(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const float* v, int n, float vmax, float threshold);
void big_number(ImDrawList* dl, ImVec2 pos, const char* value, const char* unit, const char* label, const char* sub, Tone t);

}  // namespace hg::w
