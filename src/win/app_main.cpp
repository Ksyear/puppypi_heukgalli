// 흑갈리(ROG Ally X)용 진입점: Win32 창 + D3D11 + Dear ImGui. 화면 로직은 ui.cpp 의 App.
#include "sock.hpp"  // winsock2.h 를 windows.h 보다 먼저

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "theme.hpp"
#include "ui.hpp"
#include "widgets.hpp"
#include "util.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;
bool g_occluded = false;
hg::App* g_app = nullptr;
bool g_fullscreen = true;
WINDOWPLACEMENT g_wp{};

void create_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

void release_rtv() {
    if (g_rtv) g_rtv->Release();
    g_rtv = nullptr;
}

bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                               &g_swap, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                           &g_dev, &got, &g_ctx);
    if (hr != S_OK) return false;
    create_rtv();
    return true;
}

void release_device() {
    release_rtv();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
}

// 테두리 없는 전체 화면 ↔ 창(F11)
void set_fullscreen(HWND hwnd, bool on) {
    DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE));
    if (on) {
        g_wp.length = sizeof g_wp;
        GetWindowPlacement(hwnd, &g_wp);
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_wp);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = on;
}

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                if (g_app) g_app->set_focus(false);
                return 0;
            }
            g_resize_w = LOWORD(lp);
            g_resize_h = HIWORD(lp);
            return 0;
        case WM_ACTIVATEAPP:
            // 다른 앱(Armoury Crate, 알림 등)으로 넘어가면 즉시 중립
            if (g_app) g_app->set_focus(wp != FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_F11) set_fullscreen(hwnd, !g_fullscreen);
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;  // Alt 메뉴 끄기
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::string exe_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf, n);
    w = w.substr(0, w.find_last_of(L"\\/"));
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
    return out;
}

bool file_exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

// 지금 그린 화면(백 버퍼)을 BMP 로 저장. 다른 창에 가려져도 앱 화면 그대로 남는다(포트폴리오 스크린샷용)
bool save_backbuffer_bmp(const std::string& path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &staging));
    if (ok) {
        g_ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m{};
        ok = SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            FILE* f = std::fopen(path.c_str(), "wb");
            ok = f != nullptr;
            if (f) {
                uint32_t w = d.Width, h = d.Height, row = w * 4, size = 54 + row * h;
                unsigned char hdr[54] = {'B', 'M'};
                auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) hdr[at + i] = static_cast<unsigned char>(v >> (8 * i)); };
                put32(2, size);
                put32(10, 54);
                put32(14, 40);
                put32(18, w);
                put32(22, static_cast<uint32_t>(-static_cast<int32_t>(h)));  // 위에서 아래로
                hdr[26] = 1;
                hdr[28] = 32;
                put32(34, row * h);
                std::fwrite(hdr, 1, sizeof hdr, f);
                std::string line(row, '\0');
                for (uint32_t y = 0; y < h; ++y) {
                    const unsigned char* src = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                    for (uint32_t x = 0; x < w; ++x) {  // RGBA → BGRA
                        line[x * 4 + 0] = static_cast<char>(src[x * 4 + 2]);
                        line[x * 4 + 1] = static_cast<char>(src[x * 4 + 1]);
                        line[x * 4 + 2] = static_cast<char>(src[x * 4 + 0]);
                        line[x * 4 + 3] = static_cast<char>(255);
                    }
                    std::fwrite(line.data(), 1, row, f);
                }
                std::fclose(f);
            }
            g_ctx->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
    return ok;
}

// 실행 기록(exe 옆 app_log.txt, 실행마다 새로). 화면 없이도 시작·종료 원인을 알 수 있게
FILE* g_log = nullptr;
void logf(const std::string& line) {
    if (!g_log) return;
    std::fprintf(g_log, "%s %s\n", hg::clock_text().c_str(), line.c_str());
    std::fflush(g_log);
}
}  // namespace

void hg_imgui_assert_failed(const char* expr, const char* file, int line) {
    logf(hg::fmt("ImGui 검사 실패: %s (%s:%d)", expr, file, line));
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
    std::abort();
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    g_log = std::fopen((exe_dir() + "\\app_log.txt").c_str(), "w");
    logf("시작");
    hg::sock_init();
    timeBeginPeriod(1);  // 20 Hz 송신 스레드의 sleep 해상도(기본 15.6 ms → 1 ms)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool kbdrive = false, privacy = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--windowed") g_fullscreen = false;
        if (std::wstring(argv[i]) == L"--kbdrive") kbdrive = true;  // 패드 없는 PC 시연용(Shift 데드맨)
        if (std::wstring(argv[i]) == L"--privacy") privacy = true;  // 화면에 IP 를 보이지 않음
    }
    LocalFree(argv);

    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, inst, LoadIconW(inst, L"APPICON"), LoadCursorW(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"PuppyPiHeukgalli",
                      static_cast<HICON>(LoadImageW(inst, L"APPICON", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                    GetSystemMetrics(SM_CYSMICON), 0))};  // 제목 표시줄용 작은 아이콘
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"흑갈리 PuppyPi 조종기", WS_OVERLAPPEDWINDOW, 80, 80,
                              static_cast<int>(1280 * scale), static_cast<int>(720 * scale), nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    logf(hwnd ? hg::fmt("창 생성, DPI 배율 %.2f", scale) : "창 생성 실패");
    if (!create_device(hwnd)) {
        logf("D3D11 장치 생성 실패");
        release_device();
        MessageBoxW(nullptr, L"Direct3D 11 장치를 만들 수 없습니다.", L"흑갈리 PuppyPi", MB_ICONERROR);
        return 1;
    }
    logf("D3D11 장치 준비");
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    if (g_fullscreen) set_fullscreen(hwnd, true);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // 창 배치는 코드가 정한다
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // 패드는 로봇 조종에만 쓴다(ImGui 패드 탐색 끔)
    // 색·간격·글꼴은 theme.cpp(디자인 토큰). 시스템 글꼴은 실행 시 읽기만 하고, Pretendard Std 는 fonts\ 에 번들(OFL)
    logf("ImGui 문맥 생성");
    hg::theme::apply_style(scale);
    hg::w::set_scale(scale);
    char windir[MAX_PATH] = "C:\\Windows";
    GetWindowsDirectoryA(windir, MAX_PATH);
    logf("스타일 적용, 글꼴 읽는 중");
    hg::theme::load_fonts(std::string(windir) + "\\Fonts\\", exe_dir());
    logf("글꼴: " + hg::theme::F.body_name + (hg::theme::F.icons ? " + 아이콘" : " (아이콘 글꼴 없음)"));
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    logf("ImGui 준비");

    {
        hg::App app(g_dev, g_ctx, hwnd, exe_dir(), scale);
        app.set_keyboard_drive(kbdrive);
        app.set_privacy(privacy);
        g_app = &app;
        bool done = false, was_occluded = false;
        // 화면 저장: F12, 또는 exe 옆에 screenshot.request 파일이 생기면(원격 점검용, 1 초마다 확인)
        const std::string dir = exe_dir(), request = dir + "\\screenshot.request";
        double next_request_check = 0;
        bool shot_pending = false, shot_after_switch = false, shot_unmask = false;
        uint64_t event_cursor = 0;
        double next_perf_log = 0;
        while (!done && !app.quit_requested()) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;
            if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                // 화면이 가려져(잠금·다른 전체 화면) 있는 동안은 중립. 프레임 처리는 계속해 진동·상태를 유지한다
                if (!was_occluded) app.set_focus(false);
                was_occluded = true;
                Sleep(10);
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
                app.frame(hg::now_s());
                ImGui::EndFrame();
                continue;
            }
            g_occluded = false;
            if (was_occluded) {
                was_occluded = false;
                app.set_focus(GetForegroundWindow() == hwnd);
            }
            if (g_resize_w && g_resize_h) {
                release_rtv();
                g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
                g_resize_w = g_resize_h = 0;
                create_rtv();
            }
            double t0 = hg::now_s();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            app.frame(t0);
            ImGui::Render();
            const float clear[4] = {0.03f, 0.03f, 0.04f, 1.0f};
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_ctx->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            app.set_cpu_ms((hg::now_s() - t0) * 1000.0, t0);  // vsync 대기(Present) 전까지의 CPU 시간
            if (t0 >= next_perf_log) {  // 5 초마다 성능 한 줄(docs/MEASUREMENTS.md 근거)
                next_perf_log = t0 + 5.0;
                logf(app.perf_line());
            }
            for (const auto& line : app.events().since(event_cursor)) logf("이벤트 " + line.substr(13));  // 앞의 시각 13자 제외
            if (hg::now_s() >= next_request_check) {
                next_request_check = hg::now_s() + 1.0;
                if (file_exists(request.c_str())) {
                    // 파일 내용이 화면 이름이면 그 화면으로 바꾼 뒤 다음 프레임에 저장
                    char name[32] = {0};
                    if (FILE* rf = std::fopen(request.c_str(), "rb")) {
                        size_t n = std::fread(name, 1, sizeof name - 1, rf);
                        name[n] = 0;
                        std::fclose(rf);
                    }
                    DeleteFileA(request.c_str());
                    std::string want = hg::trim(name);
                    if (!want.empty() && app.select_screen(want)) {
                        next_request_check = hg::now_s() + 0.5;  // 새 화면이 한 번 그려진 뒤 저장
                        shot_after_switch = true;
                    } else {
                        shot_pending = true;
                    }
                } else if (shot_after_switch) {
                    shot_after_switch = false;
                    shot_pending = true;
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) shot_pending = true;
            if (shot_pending && !app.privacy()) {
                // 저장하는 이미지에는 IP 를 남기지 않는다: 가린 화면을 한 프레임 그린 뒤 저장하고 되돌린다
                app.set_privacy(true);
                shot_unmask = true;
            } else if (shot_pending) {
                shot_pending = false;
                CreateDirectoryA((dir + "\\shots").c_str(), nullptr);
                SYSTEMTIME t;
                GetLocalTime(&t);
                std::string path = dir + hg::fmt("\\shots\\ui_%04d%02d%02d_%02d%02d%02d.bmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                logf(save_backbuffer_bmp(path) ? "화면 저장 " + path : "화면 저장 실패");
                if (shot_unmask) app.set_privacy(false);
                shot_unmask = false;
            }
            HRESULT hr = g_swap->Present(1, 0);  // vsync(흑갈리 120 Hz)
            g_occluded = hr == DXGI_STATUS_OCCLUDED;
        }
        logf(done ? "창 닫힘(WM_QUIT)" : "사용자 종료 요청");
        g_app = nullptr;
    }  // App 소멸자: 중립 + disconnect, 설정 저장
    logf("종료");

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    timeEndPeriod(1);
    if (g_log) std::fclose(g_log);
    return 0;
}
