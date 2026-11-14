#include "theme.hpp"

#include <windows.h>

#include <algorithm>

namespace hg::theme {

namespace {
constexpr ImU32 hex(unsigned rgb, unsigned a = 255) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}
bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
}  // namespace

// docs/DESIGN_PLAN_v1.md §3.1 (WCAG 대비 검증값)
const Tokens T = {
    hex(0x0B0D10), hex(0x13161B), hex(0x1B2027), hex(0x242A33), hex(0x2E3640),
    hex(0xF2F4F7), hex(0xA9B1BD), hex(0x6F7987),
    hex(0xFF6B35), hex(0x3DD68C), hex(0xF5A524), hex(0xE5484D), hex(0xB3261E), hex(0x5B9CFF), hex(0x19C6D1), hex(0x0B0D10),
};

Fonts F;

ImU32 alpha(ImU32 c, float a) {
    unsigned al = (c >> IM_COL32_A_SHIFT) & 0xFF;
    al = static_cast<unsigned>(std::clamp(al * a, 0.0f, 255.0f));
    return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

ImU32 shade(ImU32 c, float k) {
    auto ch = [&](int shift) {
        float v = static_cast<float>((c >> shift) & 0xFF) * k;
        return static_cast<ImU32>(std::clamp(v, 0.0f, 255.0f)) << shift;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | (c & IM_COL32_A_MASK);
}

// 본문 글꼴 하나에 맑은 고딕(빠진 한글)·아이콘·기호를 차례로 병합한다. 앞 글꼴에 있는 글리프가 우선.
static ImFont* add_family(const std::string& first, const std::string& fonts_dir, bool bold) {
    ImGuiIO& io = ImGui::GetIO();
    ImFont* f = nullptr;
    if (!first.empty() && exists(first)) f = io.Fonts->AddFontFromFileTTF(first.c_str(), 0.0f);
    std::string malgun = fonts_dir + (bold ? "malgunbd.ttf" : "malgun.ttf");
    ImFontConfig merge;
    merge.MergeMode = true;
    if (!f) {
        if (exists(malgun)) f = io.Fonts->AddFontFromFileTTF(malgun.c_str(), 0.0f);
        if (!f) return io.Fonts->AddFontDefault();
    } else if (exists(malgun)) {
        io.Fonts->AddFontFromFileTTF(malgun.c_str(), 0.0f, &merge);  // Pretendard Std 에 없는 한글
    }
    ImFontConfig icons;
    icons.MergeMode = true;
    icons.SizePixels = sz::body;       // GlyphOffset 은 기준 크기가 있어야 한다(크기에 비례해 적용)
    icons.GlyphOffset = ImVec2(0, 2);  // 아이콘 글꼴은 기준선이 위로 떠 있다
    std::string seg = fonts_dir + "SegoeIcons.ttf", mdl2 = fonts_dir + "segmdl2.ttf";
    if (exists(seg)) {
        io.Fonts->AddFontFromFileTTF(seg.c_str(), 0.0f, &icons);
        F.icons = true;
    } else if (exists(mdl2)) {
        io.Fonts->AddFontFromFileTTF(mdl2.c_str(), 0.0f, &icons);
        F.icons = true;
    }
    std::string sym = fonts_dir + "seguisym.ttf";
    if (exists(sym)) io.Fonts->AddFontFromFileTTF(sym.c_str(), 0.0f, &merge);  // ▲▼◀▶●○ 등
    return f;
}

void load_fonts(const std::string& fonts_dir, const std::string& exe_dir) {
    ImGuiIO& io = ImGui::GetIO();
    // 번들 글꼴: exe 옆 fonts\ 또는 저장소 루트 fonts\ (build\ 에서 실행할 때)
    std::string bundle;
    for (const char* d : {"\\fonts\\", "\\..\\fonts\\"})
        if (exists(exe_dir + d + "PretendardStd-Regular.ttf")) {
            bundle = exe_dir + d;
            break;
        }
    F.body_name = bundle.empty() ? "Malgun Gothic" : "Pretendard Std";
    F.body = add_family(bundle.empty() ? "" : bundle + "PretendardStd-Regular.ttf", fonts_dir, false);
    F.semi = add_family(bundle.empty() ? "" : bundle + "PretendardStd-SemiBold.ttf", fonts_dir, true);
    F.bold = add_family(bundle.empty() ? "" : bundle + "PretendardStd-Bold.ttf", fonts_dir, true);
    // 숫자: 고정폭(값이 바뀌어도 흔들리지 않게). 한글·아이콘은 본문과 같은 순서로 병합
    std::string mono = fonts_dir + "CascadiaMono.ttf";
    if (!exists(mono)) mono = fonts_dir + "consola.ttf";
    F.num = exists(mono) ? add_family(mono, fonts_dir, false) : F.body;
    io.FontDefault = F.body;
}

void apply_style(float dpi_scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowRounding = 0;
    st.ChildRounding = 12;
    st.FrameRounding = 10;
    st.GrabRounding = 12;
    st.PopupRounding = 12;
    st.ScrollbarRounding = 8;
    st.FramePadding = ImVec2(12, 10);
    st.ItemSpacing = ImVec2(12, 10);
    st.ScrollbarSize = 20;
    st.GrabMinSize = 32;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.FrameBorderSize = 0;
    ImVec4* c = st.Colors;
    auto v = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    for (int i = 0; i < ImGuiCol_COUNT; ++i) c[i] = v(T.bg2);
    c[ImGuiCol_Text] = v(T.text1);
    c[ImGuiCol_TextDisabled] = v(T.text3);
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = v(T.bg0);
    c[ImGuiCol_PopupBg] = v(T.bg1);
    c[ImGuiCol_Border] = c[ImGuiCol_Separator] = v(T.line);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = v(T.bg2);
    c[ImGuiCol_FrameBgHovered] = v(T.bg3);
    c[ImGuiCol_FrameBgActive] = v(T.bg3);
    c[ImGuiCol_Button] = v(T.bg2);
    c[ImGuiCol_ButtonHovered] = v(T.bg3);
    c[ImGuiCol_ButtonActive] = v(T.bg1);
    c[ImGuiCol_Header] = v(T.bg2);
    c[ImGuiCol_HeaderHovered] = c[ImGuiCol_HeaderActive] = v(T.bg3);
    c[ImGuiCol_SliderGrab] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_CheckMark] = v(T.accent);
    c[ImGuiCol_ScrollbarBg] = v(T.bg1);
    c[ImGuiCol_ScrollbarGrab] = c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = v(T.bg3);
    c[ImGuiCol_PlotLines] = c[ImGuiCol_PlotHistogram] = v(T.info);
    c[ImGuiCol_NavCursor] = v(T.accent);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = v(T.bg1);
    st.ScaleAllSizes(dpi_scale);
    st.FontScaleDpi = dpi_scale;
    st.FontSizeBase = sz::body;
}

}  // namespace hg::theme
