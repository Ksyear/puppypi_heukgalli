#include "widgets.hpp"

#include "imgui_internal.h"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hg::w {

using theme::F;
using theme::T;
namespace sz = theme::sz;

static float g_s = 1.0f;
float S() { return g_s; }
void set_scale(float s) { g_s = s; }

ImU32 tone_color(Tone t) {
    switch (t) {
        case Tone::Accent: return T.accent;
        case Tone::Ok: return T.ok;
        case Tone::Warn: return T.warn;
        case Tone::Danger: return T.danger;
        case Tone::Info: return T.info;
        case Tone::Muted: return T.text3;
        default: return T.text2;
    }
}

// 글자 크기는 논리 px, 그릴 때 DPI 배율을 곱한다
static float px(float logical_size) { return logical_size * g_s; }

ImVec2 text_size(ImFont* f, float size, const char* txt) {
    if (!f) f = F.body;
    return f->CalcTextSizeA(px(size), FLT_MAX, 0.0f, txt);
}

void shadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r) {
    float s = g_s;
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 2 * s), ImVec2(p1.x, p1.y + 2 * s), IM_COL32(0, 0, 0, 90), r);
    dl->AddRectFilled(ImVec2(p0.x - 1 * s, p0.y + 6 * s), ImVec2(p1.x + 1 * s, p1.y + 6 * s), IM_COL32(0, 0, 0, 40), r);
}

void card(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float r) {
    r *= g_s;
    shadow(dl, p0, p1, r);
    dl->AddRectFilled(p0, p1, T.bg1, r);
    dl->AddRect(p0, p1, T.line, r, 1.0f);
}

void text_shadowed(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* txt) {
    if (!f) f = F.body;
    dl->AddText(f, px(size), ImVec2(pos.x + g_s, pos.y + g_s), IM_COL32(0, 0, 0, 160), txt);
    dl->AddText(f, px(size), pos, col, txt);
}

void pad_glyph(ImDrawList* dl, Pad p, ImVec2 c, float size) {
    float s = g_s, r = size * s / 2;
    auto label = [&](const char* t, float fs) {
        ImVec2 ts = text_size(F.bold, fs, t);
        dl->AddText(F.bold, px(fs), ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), T.text1, t);
    };
    switch (p) {
        case Pad::A: case Pad::B: case Pad::X: case Pad::Y: {
            static const char* names[] = {"A", "B", "X", "Y"};
            static const ImU32 dots[] = {IM_COL32(61, 214, 140, 255), IM_COL32(229, 72, 77, 255), IM_COL32(91, 156, 255, 255),
                                         IM_COL32(245, 165, 36, 255)};
            int i = static_cast<int>(p);
            dl->AddCircleFilled(c, r, T.bg3);
            dl->AddCircle(c, r, T.text2, 0, 1.5f * s);
            label(names[i], 13);
            dl->AddCircleFilled(ImVec2(c.x, c.y + r - 2.5f * s), 2.0f * s, dots[i]);  // 관례색은 작은 점으로만
            break;
        }
        case Pad::LB: case Pad::RB: case Pad::LT: case Pad::RT: case Pad::View: case Pad::Menu: case Pad::LS: {
            static const char* names[] = {"LB", "RB", "LT", "RT", "LS", "", "", "VIEW", "MENU"};
            const char* t = names[static_cast<int>(p) - 4];
            ImVec2 ts = text_size(F.bold, 11, t);
            float hw = std::max(r * 1.1f, ts.x / 2 + 5 * s), hh = r * 0.68f;
            dl->AddRectFilled(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), T.bg3, 4 * s);
            dl->AddRect(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), T.text2, 4 * s, 1.2f * s);
            label(t, 11);
            break;
        }
        case Pad::DpadUD: case Pad::DpadLR: {
            float a = r * 0.9f, b = r * 0.32f;
            dl->AddRectFilled(ImVec2(c.x - b, c.y - a), ImVec2(c.x + b, c.y + a), T.bg3, 2 * s);
            dl->AddRectFilled(ImVec2(c.x - a, c.y - b), ImVec2(c.x + a, c.y + b), T.bg3, 2 * s);
            ImU32 hi = T.text1;
            if (p == Pad::DpadUD) {
                dl->AddTriangleFilled(ImVec2(c.x, c.y - a + 1 * s), ImVec2(c.x - b * 0.8f, c.y - a + b * 1.6f), ImVec2(c.x + b * 0.8f, c.y - a + b * 1.6f), hi);
                dl->AddTriangleFilled(ImVec2(c.x, c.y + a - 1 * s), ImVec2(c.x + b * 0.8f, c.y + a - b * 1.6f), ImVec2(c.x - b * 0.8f, c.y + a - b * 1.6f), hi);
            } else {
                dl->AddTriangleFilled(ImVec2(c.x - a + 1 * s, c.y), ImVec2(c.x - a + b * 1.6f, c.y + b * 0.8f), ImVec2(c.x - a + b * 1.6f, c.y - b * 0.8f), hi);
                dl->AddTriangleFilled(ImVec2(c.x + a - 1 * s, c.y), ImVec2(c.x + a - b * 1.6f, c.y - b * 0.8f), ImVec2(c.x + a - b * 1.6f, c.y + b * 0.8f), hi);
            }
            break;
        }
    }
}

float chip_width(float h, const char* icon, const char* text, ImFont* f) {
    float s = g_s, pad = 10 * s;
    float w = 2 * pad + text_size(f ? f : F.semi, sz::label, text).x;
    if (icon && *icon) w += text_size(F.body, sz::label, icon).x + 6 * s;
    (void)h;
    return w;
}

float chip(ImDrawList* dl, ImVec2 pos, float h, const char* icon, const char* text, Tone t, bool filled, ImFont* f) {
    float s = g_s, pad = 10 * s;
    if (!f) f = F.semi;
    ImU32 col = tone_color(t);
    float w = chip_width(h, icon, text, f);
    ImVec2 p1(pos.x + w, pos.y + h);
    if (filled) {
        dl->AddRectFilled(pos, p1, col, h / 2);
    } else {
        dl->AddRectFilled(pos, p1, T.bg2, h / 2);
        dl->AddRect(pos, p1, col, h / 2, 1.5f * s);
    }
    ImU32 fg = filled ? T.ink : (t == Tone::Neutral ? T.text1 : col);
    float x = pos.x + pad;
    if (icon && *icon) {
        ImVec2 is = text_size(F.body, sz::label, icon);
        dl->AddText(F.body, px(sz::label), ImVec2(x, pos.y + (h - is.y) / 2), fg, icon);
        x += is.x + 6 * s;
    }
    ImVec2 ts = text_size(f, sz::label, text);
    dl->AddText(f, px(sz::label), ImVec2(x, pos.y + (h - ts.y) / 2), fg, text);
    return w;
}

bool icon_button(const char* id, const char* icon, const char* label, ImVec2 size, Tone t, const Pad* hint, bool enabled,
                 bool selected, bool vertical) {
    float s = g_s;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImGui::InvisibleButton(id, size);
    bool hovered = enabled && ImGui::IsItemHovered(), active = enabled && ImGui::IsItemActive();
    bool clicked = enabled && ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = 10 * s;
    ImU32 face_top = T.bg3, face_bot = T.bg2;
    if (t == Tone::Accent || t == Tone::Ok || t == Tone::Warn || t == Tone::Danger || t == Tone::Info) {
        face_top = theme::shade(tone_color(t), 1.0f);
        face_bot = theme::shade(tone_color(t), 0.82f);
    }
    if (!enabled) face_top = face_bot = T.bg1;
    if (hovered) face_top = theme::shade(face_top, 1.06f), face_bot = theme::shade(face_bot, 1.06f);
    if (active) face_top = face_bot = theme::shade(face_bot, 0.92f);
    if (enabled) shadow(dl, p0, p1, r);
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(p0, p1, face_top, r);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p0, ImVec2(p0.x, p1.y), face_top, face_bot);
    if (enabled) dl->AddLine(ImVec2(p0.x + r, p0.y + 1), ImVec2(p1.x - r, p0.y + 1), IM_COL32(255, 255, 255, 18));
    bool filled_tone = enabled && t != Tone::Neutral && t != Tone::Muted;
    ImU32 fg = !enabled ? T.text3 : (filled_tone ? T.ink : (selected ? T.text1 : T.text1));
    ImU32 icon_col = !enabled ? T.text3 : (filled_tone ? T.ink : (selected ? T.accent : T.text2));
    if (selected) {
        dl->AddRect(p0, p1, T.accent, r, 2.5f * s);
        if (vertical) dl->AddRectFilled(ImVec2(p0.x, p0.y + 10 * s), ImVec2(p0.x + 4 * s, p1.y - 10 * s), T.accent, 2 * s);
    }
    if (vertical) {
        ImVec2 is = text_size(F.body, sz::icon_lg, icon), ls = text_size(F.semi, sz::label, label);
        float total = is.y + 2 * s + ls.y, y = p0.y + (size.y - total) / 2;
        dl->AddText(F.body, px(sz::icon_lg), ImVec2(p0.x + (size.x - is.x) / 2, y), icon_col, icon);
        dl->AddText(F.semi, px(sz::label), ImVec2(p0.x + (size.x - ls.x) / 2, y + is.y + 2 * s), fg, label);
    } else {
        float x = p0.x + 14 * s;
        if (icon && *icon) {
            ImVec2 is = text_size(F.body, sz::icon, icon);
            dl->AddText(F.body, px(sz::icon), ImVec2(x, p0.y + (size.y - is.y) / 2), icon_col, icon);
            x += is.x + 10 * s;
        }
        float right = p1.x - (hint ? 34 * s : 12 * s);
        float fs = size.x < 150 * s ? sz::label : sz::button;  // 레일처럼 좁은 버튼은 라벨을 한 단계 작게
        ImVec2 ls = text_size(F.semi, fs, label);
        ImVec4 clip(x, p0.y, right, p1.y);
        dl->AddText(F.semi, px(fs), ImVec2(x, p0.y + (size.y - ls.y) / 2), fg, label, nullptr, 0.0f, &clip);
        if (hint) pad_glyph(dl, *hint, ImVec2(p1.x - 18 * s, p0.y + size.y / 2), 20);
    }
    return clicked;
}

bool segmented(const char* id, const char* const* items, int n, int* index, ImVec2 size) {
    float s = g_s;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = size.y, seg = size.x / n;
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + h), T.bg1, h / 2);
    dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + h), T.line, h / 2, 1.0f);
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < n; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + seg * i, p0.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("seg", ImVec2(seg, h)) && *index != i) {
            *index = i;
            changed = true;
        }
        ImGui::PopID();
        ImVec2 a(p0.x + seg * i + 3 * s, p0.y + 3 * s), b(p0.x + seg * (i + 1) - 3 * s, p0.y + h - 3 * s);
        if (*index == i) dl->AddRectFilled(a, b, T.accent, (h - 6 * s) / 2);
        ImVec2 ts = text_size(F.semi, sz::label, items[i]);
        dl->AddText(F.semi, px(sz::label), ImVec2((a.x + b.x - ts.x) / 2, p0.y + (h - ts.y) / 2), *index == i ? T.ink : T.text2,
                    items[i]);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
    ImGui::Dummy(ImVec2(size.x, 0));
    return changed;
}

bool toggle(const char* id, const char* label, bool* v) {
    float s = g_s, h = 32 * s, w = 56 * s;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 ls = text_size(F.body, sz::body, label);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w + 14 * s + ls.x, std::max(h, ls.y)));
    if (clicked) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), *v ? T.accent : T.bg3, h / 2);
    float kx = *v ? p0.x + w - h / 2 : p0.x + h / 2;
    dl->AddCircleFilled(ImVec2(kx, p0.y + h / 2), h / 2 - 4 * s, *v ? T.ink : T.text2);
    dl->AddText(F.body, px(sz::body), ImVec2(p0.x + w + 14 * s, p0.y + (h - ls.y) / 2), T.text1, label);
    return clicked;
}

bool estop(const char* id, ImVec2 size, bool engaged, bool connected, float hold_k, bool* holding, double now) {
    float s = g_s;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImVec2 c((p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
    ImGui::InvisibleButton(id, size);
    bool active = ImGui::IsItemActive(), clicked = ImGui::IsItemClicked();
    if (holding) *holding = active;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = 20 * s;
    if (!engaged) {
        // 노란 바깥 링(비상정지의 "빨강 위 노랑" 기호성) → 그림자 → 빨강 그라데이션 면 → 팔각형 → 글자 → Ⓑ
        dl->AddRect(ImVec2(p0.x - 4 * s, p0.y - 4 * s), ImVec2(p1.x + 4 * s, p1.y + 4 * s), connected ? T.warn : T.line, r + 4 * s, 4 * s);
        shadow(dl, p0, p1, r);
        ImU32 top = connected ? T.danger : T.bg2, bot = connected ? T.danger_deep : T.bg1;
        if (active) top = bot;
        int v0 = dl->VtxBuffer.Size;
        dl->AddRectFilled(p0, p1, top, r);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p0, ImVec2(p0.x, p1.y), top, bot);
        dl->AddLine(ImVec2(p0.x + r, p0.y + 1), ImVec2(p1.x - r, p0.y + 1), IM_COL32(255, 255, 255, 30));
        ImVec2 oc(c.x, p0.y + size.y * 0.36f);
        float orad = 20 * s;
        dl->AddNgonFilled(oc, orad, connected ? IM_COL32(255, 255, 255, 235) : T.text3, 8);
        dl->AddNgon(oc, orad - 3.5f * s, connected ? T.danger_deep : T.bg1, 8, 2.0f * s);
        const char* t = connected ? "비상정지" : "미연결";
        ImVec2 ts = text_size(F.bold, sz::estop, t);
        dl->AddText(F.bold, px(sz::estop), ImVec2(c.x - ts.x / 2, p0.y + size.y * 0.62f), connected ? IM_COL32_WHITE : T.text3, t);
        if (connected) pad_glyph(dl, Pad::B, ImVec2(p1.x - 18 * s, p0.y + 18 * s), 20);
        return clicked && connected;
    }
    // 비상정지 중: 해제 모드. 빨간 링이 처음 1.6 s 동안 깜빡인 뒤 고정
    bool blink = std::fmod(now, 0.8) < 0.4;
    dl->AddRect(ImVec2(p0.x - 4 * s, p0.y - 4 * s), ImVec2(p1.x + 4 * s, p1.y + 4 * s), blink ? T.danger : T.danger_deep, r + 4 * s, 4 * s);
    shadow(dl, p0, p1, r);
    dl->AddRectFilled(p0, p1, active ? T.bg3 : T.bg2, r);
    ImVec2 rc(c.x, p0.y + size.y * 0.40f);
    float rr = std::min(size.x, size.y) * 0.26f;
    dl->AddCircle(rc, rr, T.bg3, 48, 6 * s);
    if (hold_k > 0) {
        const float a0 = -IM_PI / 2;
        dl->PathArcTo(rc, rr, a0, a0 + 2 * IM_PI * std::min(1.0f, hold_k), 48);
        dl->PathStroke(T.ok, 6 * s);
    }
    ImVec2 is = text_size(F.body, sz::icon_lg, theme::icon::lock);
    dl->AddText(F.body, px(sz::icon_lg), ImVec2(rc.x - is.x / 2, rc.y - is.y / 2), T.text1, theme::icon::lock);
    const char* t1 = "해제";
    const char* t2 = "1초 누르기";
    ImVec2 s1 = text_size(F.bold, sz::button, t1), s2 = text_size(F.body, sz::label, t2);
    dl->AddText(F.bold, px(sz::button), ImVec2(c.x - s1.x / 2, p0.y + size.y * 0.68f), T.text1, t1);
    dl->AddText(F.body, px(sz::label), ImVec2(c.x - s2.x / 2, p0.y + size.y * 0.68f + s1.y), T.text2, t2);
    return false;
}

void drive_capsule(ImDrawList* dl, ImVec2 pos, ImVec2 size, Tone t, bool filled, const char* lamp, const char* text) {
    float s = g_s, h = size.y;
    ImU32 col = tone_color(t);
    ImVec2 p1(pos.x + size.x, pos.y + h);
    if (filled) {
        dl->AddRectFilled(pos, p1, col, h / 2);
    } else {
        dl->AddRectFilled(pos, p1, theme::alpha(T.bg0, 0.75f), h / 2);
        dl->AddRect(pos, p1, col, h / 2, 2.0f * s);
    }
    ImU32 fg = filled ? (t == Tone::Danger ? IM_COL32_WHITE : T.ink) : T.text1;
    ImU32 lamp_col = filled ? fg : col;
    float x = pos.x + h * 0.45f;
    ImVec2 ls = text_size(F.bold, sz::button, lamp);
    dl->AddText(F.bold, px(sz::button), ImVec2(x, pos.y + (h - ls.y) / 2), lamp_col, lamp);
    x += ls.x + 10 * s;
    ImVec2 ts = text_size(F.bold, sz::button, text);
    ImVec4 clip(x, pos.y, p1.x - h * 0.4f, p1.y);
    dl->AddText(F.bold, px(sz::button), ImVec2(x, pos.y + (h - ts.y) / 2), fg, text, nullptr, 0.0f, &clip);
}

void cmd_bar(ImDrawList* dl, ImVec2 pos, float w, const char* name, double v, bool active) {
    float s = g_s;
    ImVec2 ns = text_size(F.semi, sz::label, name);
    text_shadowed(dl, F.semi, sz::label, ImVec2(pos.x, pos.y), T.text2, name);
    float x0 = pos.x + 52 * s, tw = w - 52 * s - 64 * s, cy = pos.y + ns.y / 2, mid = x0 + tw / 2;
    dl->AddRectFilled(ImVec2(x0, cy - 3 * s), ImVec2(x0 + tw, cy + 3 * s), T.bg3, 3 * s);
    float e = mid + static_cast<float>(std::clamp(v, -1.0, 1.0)) * tw / 2;
    dl->AddRectFilled(ImVec2(std::min(mid, e), cy - 3 * s), ImVec2(std::max(mid, e), cy + 3 * s), active ? T.ok : T.text3, 3 * s);
    dl->AddLine(ImVec2(mid, cy - 8 * s), ImVec2(mid, cy + 8 * s), T.text2, 2 * s);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%+.2f", v);
    text_shadowed(dl, F.num, sz::num_sm, ImVec2(x0 + tw + 10 * s, pos.y), active ? T.text1 : T.text3, buf);
}

void toast(ImDrawList* dl, ImVec2 c, const char* text, Tone t, float alpha, float rise) {
    float s = g_s, h = 44 * s;
    ImVec2 ts = text_size(F.semi, sz::body, text);
    float w = std::min(ts.x + 48 * s, 760 * s);
    ImVec2 p0(c.x - w / 2, c.y + rise * s), p1(c.x + w / 2, c.y + rise * s + h);
    ImU32 a = static_cast<ImU32>(255 * alpha);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 3 * s), ImVec2(p1.x, p1.y + 3 * s), IM_COL32(0, 0, 0, 90 * a / 255), h / 2);
    dl->AddRectFilled(p0, p1, theme::alpha(T.bg2, 0.96f * alpha), h / 2);
    dl->AddRectFilled(ImVec2(p0.x + 14 * s, p0.y + 12 * s), ImVec2(p0.x + 18 * s, p1.y - 12 * s), theme::alpha(tone_color(t), alpha), 2 * s);
    ImVec4 clip(p0.x, p0.y, p1.x - 14 * s, p1.y);
    dl->AddText(F.semi, px(sz::body), ImVec2(p0.x + 28 * s, p0.y + (h - ts.y) / 2), theme::alpha(T.text1, alpha), text, nullptr, 0.0f, &clip);
}

void empty_state(ImDrawList* dl, ImVec2 c, const char* icon, const char* title, const char* hint) {
    float s = g_s;
    ImVec2 is = text_size(F.body, 48, icon), ts = text_size(F.bold, sz::title, title), hs = text_size(F.body, sz::label, hint);
    float y = c.y - (is.y + ts.y + hs.y + 16 * s) / 2;
    dl->AddText(F.body, px(48), ImVec2(c.x - is.x / 2, y), T.text3, icon);
    y += is.y + 8 * s;
    dl->AddText(F.bold, px(sz::title), ImVec2(c.x - ts.x / 2, y), T.text2, title);
    y += ts.y + 8 * s;
    dl->AddText(F.body, px(sz::label), ImVec2(c.x - hs.x / 2, y), T.text3, hint);
}

void stat_tile(const char* id, const char* label, const char* big, const char* small, ImVec2 size, Tone t) {
    float s = g_s;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImGui::Dummy(size);
    (void)id;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    card(dl, p0, p1, 12);
    dl->AddText(F.semi, px(sz::label), ImVec2(p0.x + 16 * s, p0.y + 12 * s), T.text2, label);
    dl->AddText(F.num, px(sz::num_lg), ImVec2(p0.x + 16 * s, p0.y + 36 * s), t == Tone::Neutral ? T.text1 : tone_color(t), big);
    dl->AddText(F.num, px(sz::num_sm), ImVec2(p0.x + 16 * s, p1.y - 28 * s), T.text2, small);
}

void area_plot(const char* id, const float* v, int n, float vmax, float threshold, ImVec2 size) {
    float s = g_s;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImGui::Dummy(size);
    (void)id;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    card(dl, p0, p1, 12);
    if (n <= 0) return;
    float l = p0.x + 12 * s, r = p1.x - 12 * s, top = p0.y + 34 * s, bot = p1.y - 12 * s;
    int bars = std::max(1, std::min(n, static_cast<int>((r - l) / (3 * s))));
    float bw = (r - l) / bars;
    float prevx = -1, prevy = 0;
    for (int b = 0; b < bars; ++b) {
        int i0 = b * n / bars, i1 = std::max(i0 + 1, (b + 1) * n / bars);
        float sum = 0;
        for (int i = i0; i < i1; ++i) sum += v[i];
        float avg = sum / (i1 - i0), k = std::min(1.0f, avg / vmax);
        float x = l + b * bw, y = bot - k * (bot - top);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw - 1 * s, bot), theme::alpha(T.info, 0.24f));
        if (prevx >= 0) dl->AddLine(ImVec2(prevx, prevy), ImVec2(x + bw / 2, y), avg > threshold ? T.warn : T.info, 2 * s);
        prevx = x + bw / 2;
        prevy = y;
    }
    if (threshold < vmax) {  // 임계선(점선)
        float ty = bot - threshold / vmax * (bot - top);
        for (float x = l; x < r; x += 10 * s) dl->AddLine(ImVec2(x, ty), ImVec2(std::min(r, x + 6 * s), ty), theme::alpha(T.warn, 0.7f), 1.0f);
    }
}

}  // namespace hg::w
