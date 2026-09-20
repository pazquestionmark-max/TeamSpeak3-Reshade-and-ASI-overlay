// SPDX-License-Identifier: MIT
#include "overlay_window.hpp"

#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "d3d11_font_sink.hpp"
#include "overlay_host.hpp"
#include "tsro/config.hpp"
#include "tsro/log.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace tsro::asi {
namespace {

constexpr char kComponent[] = "asi";
constexpr wchar_t kClassName[] = L"TsroOverlayWindow";
/// Posted to our own window to open or close the settings, so the switch happens on the thread
/// that owns the window rather than from wherever the key was noticed.
constexpr UINT WM_TSRO_TOGGLE = WM_APP + 1;

overlay::OverlayHost* g_host = nullptr;
D3D11FontSink g_sink;

HWND g_hwnd = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain1* g_swapchain = nullptr;
IDCompositionDevice* g_dcomp = nullptr;
IDCompositionTarget* g_dcomp_target = nullptr;
IDCompositionVisual* g_dcomp_visual = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_width = 0;
UINT g_height = 0;

HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_menu_open{false};

template <typename T> void release(T*& p) { if (p != nullptr) { p->Release(); p = nullptr; } }

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// --- the game's window -------------------------------------------------------------------
//
// Found by walking this process's own top-level windows and taking the largest visible one,
// rather than by looking up a window class. A class name would mean naming a game, and the
// overlay is not supposed to know which game it is in.

struct GameWindowSearch {
    DWORD pid = 0;
    HWND best = nullptr;
    long best_area = 0;
};

BOOL CALLBACK pick_game_window(HWND hwnd, LPARAM param) {
    GameWindowSearch* search = reinterpret_cast<GameWindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid || hwnd == g_hwnd) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return TRUE;
    const long area = (client.right - client.left) * (client.bottom - client.top);
    if (area > search->best_area) {
        search->best_area = area;
        search->best = hwnd;
    }
    return TRUE;
}

HWND game_window() {
    GameWindowSearch search;
    search.pid = GetCurrentProcessId();
    EnumWindows(&pick_game_window, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

/// The game's client area in screen coordinates, so our window covers exactly it.
bool game_client_rect(RECT& out) {
    const HWND game = game_window();
    if (game == nullptr) return false;
    RECT client{};
    if (!GetClientRect(game, &client)) return false;
    if (client.right <= client.left || client.bottom <= client.top) return false;
    POINT top_left{client.left, client.top};
    ClientToScreen(game, &top_left);
    out = {top_left.x, top_left.y, top_left.x + (client.right - client.left),
           top_left.y + (client.bottom - client.top)};
    return true;
}

// --- device ---------------------------------------------------------------------------------

bool create_rtv() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) || back == nullptr) return false;
    const HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    return SUCCEEDED(hr) && g_rtv != nullptr;
}

bool create_device(HWND hwnd, UINT width, UINT height) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                   &g_device, nullptr, &g_context);
    IDXGIDevice* dxgi = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (SUCCEEDED(hr)) hr = g_device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        // A composition swap chain rather than an hwnd one: it is what lets the surface carry
        // real per-pixel alpha, so the game shows through everywhere we have not drawn instead
        // of through a colour key.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = factory->CreateSwapChainForComposition(dxgi, &desc, nullptr, &g_swapchain);
    }
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi, IID_PPV_ARGS(&g_dcomp));
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateTargetForHwnd(hwnd, TRUE, &g_dcomp_target);
    if (SUCCEEDED(hr)) hr = g_dcomp->CreateVisual(&g_dcomp_visual);
    if (SUCCEEDED(hr)) hr = g_dcomp_visual->SetContent(g_swapchain);
    if (SUCCEEDED(hr)) hr = g_dcomp_target->SetRoot(g_dcomp_visual);
    if (SUCCEEDED(hr)) hr = g_dcomp->Commit();
    release(factory);
    release(dxgi);
    if (FAILED(hr)) {
        TSRO_ERROR(kComponent, "the overlay's own D3D11/DirectComposition surface could not be "
                               "created; the plugin will not draw");
        return false;
    }
    g_width = width;
    g_height = height;
    return create_rtv();
}

void destroy_device() {
    g_sink.release();
    g_sink.set_device(nullptr);
    release(g_rtv);
    release(g_dcomp_visual);
    release(g_dcomp_target);
    release(g_dcomp);
    release(g_swapchain);
    release(g_context);
    release(g_device);
}

// --- window ---------------------------------------------------------------------------------

void set_click_through(bool on) {
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
    switch (msg) {
        case WM_TSRO_TOGGLE: {
            const bool open = !g_menu_open.load(std::memory_order_acquire);
            g_menu_open.store(open, std::memory_order_release);
            // Click-through is the only thing standing between the settings window and a
            // mouse: while it is on, every click goes to the game underneath.
            set_click_through(!open);
            if (open) {
                SetForegroundWindow(hwnd);
                SetFocus(hwnd);
            } else if (const HWND game = game_window()) {
                SetForegroundWindow(game);
            }
            TSRO_INFO(kComponent, open ? "settings window opened" : "settings window closed");
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                SetCursor(nullptr);   // ImGui draws its own
                return TRUE;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/// Keeps our window exactly over the game's client area, and the swap chain the same size.
void track_game_window() {
    RECT r{};
    if (!game_client_rect(r)) return;
    const UINT w = static_cast<UINT>(r.right - r.left);
    const UINT h = static_cast<UINT>(r.bottom - r.top);
    SetWindowPos(g_hwnd, HWND_TOPMOST, r.left, r.top, static_cast<int>(w), static_cast<int>(h),
                 SWP_NOACTIVATE);
    if ((w != g_width || h != g_height) && w != 0 && h != 0) {
        release(g_rtv);
        if (SUCCEEDED(g_swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
            g_width = w;
            g_height = h;
            create_rtv();
        }
    }
}

// --- the menu key ----------------------------------------------------------------------------

int virtual_key_from_name(const std::string& name) {
    struct Entry { const char* name; int vk; };
    static const Entry kKeys[] = {
        {"INSERT", VK_INSERT}, {"HOME", VK_HOME}, {"END", VK_END},
        {"DELETE", VK_DELETE}, {"PAUSE", VK_PAUSE}, {"SCROLL", VK_SCROLL},
        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
        {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
        {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
    };
    std::string upper = name;
    for (char& c : upper) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    for (const Entry& e : kKeys) {
        if (upper == e.name) return e.vk;
    }
    return VK_INSERT;
}

bool foreground_is_ours() {
    const HWND fg = GetForegroundWindow();
    if (fg == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void poll_menu_key() {
    static bool was_down = false;
    const int configured = g_host != nullptr ? virtual_key_from_name(g_host->menu_key_name())
                                             : VK_INSERT;
    // Insert is always live alongside whatever the profile names, so the settings can be
    // reached even if the profile never loaded.
    const bool down = ((GetAsyncKeyState(configured) & 0x8000) != 0) ||
                      ((GetAsyncKeyState(VK_INSERT) & 0x8000) != 0);
    if (down && !was_down && foreground_is_ours()) {
        PostMessageW(g_hwnd, WM_TSRO_TOGGLE, 0, 0);
    }
    was_down = down;
}

// --- the frame --------------------------------------------------------------------------------

void render_frame() {
    if (g_rtv == nullptr || g_host == nullptr) return;

    ImGuiIO& io = ImGui::GetIO();
    const bool menu = g_menu_open.load(std::memory_order_acquire);
    io.MouseDrawCursor = menu;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Our window is exactly the game's client area, so the backend's own size is right here --
    // but take it from the swap chain anyway, because that is what is being drawn into.
    io.DisplaySize = ImVec2(static_cast<float>(g_width), static_cast<float>(g_height));
    ImGui::NewFrame();

    Viewport viewport;
    viewport.width = static_cast<float>(g_width);
    viewport.height = static_cast<float>(g_height);
    g_host->draw_hud(ImGui::GetBackgroundDrawList(), viewport, &g_sink, now_ms());

    if (menu) {
        ImGui::SetNextWindowSize(ImVec2(620.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.94f);
        bool open = true;
        if (ImGui::Begin("TeamSpeak Overlay", &open)) g_host->draw_settings();
        ImGui::End();
        if (!open) PostMessageW(g_hwnd, WM_TSRO_TOGGLE, 0, 0);   // the window's own [x]
    }

    ImGui::Render();
    // Alpha zero everywhere we have not drawn: that is what the game shows through.
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, transparent);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // No vsync: this is our own swap chain and must never pace, or contend with, the game's.
    g_swapchain->Present(0, 0);
}

DWORD WINAPI ui_thread(LPVOID) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = &wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) {
        TSRO_ERROR(kComponent, "the overlay window class could not be registered");
        return 0;
    }

    // The game may not have a window yet. Wait for one rather than covering the whole screen.
    RECT r{};
    for (int i = 0; i < 600 && !g_stop.load(std::memory_order_acquire); ++i) {
        if (game_client_rect(r)) break;
        Sleep(100);
    }
    if (r.right <= r.left) {
        r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    UINT w = static_cast<UINT>(r.right - r.left);
    UINT h = static_cast<UINT>(r.bottom - r.top);

    // Layered, click-through, topmost, no-activate tool window: draws over the game, never
    // appears in the task bar or alt-tab, and never steals focus until the settings open.
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName, L"TeamSpeak Overlay", WS_POPUP, r.left, r.top, static_cast<int>(w),
        static_cast<int>(h), nullptr, nullptr, wc.hInstance, nullptr);
    if (g_hwnd == nullptr) {
        TSRO_ERROR(kComponent, "the overlay window could not be created");
        UnregisterClassW(kClassName, wc.hInstance);
        return 0;
    }
    // Opaque as far as user32 is concerned, so hit testing covers the whole window and
    // click-through is decided purely by WS_EX_TRANSPARENT. The actual transparency comes from
    // the composition surface's alpha, with the DWM frame extended so the window's own
    // never-painted GDI surface does not show as black.
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    const MARGINS glass{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_hwnd, &glass);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    if (!create_device(g_hwnd, w, h)) {
        destroy_device();
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        UnregisterClassW(kClassName, wc.hInstance);
        return 0;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        TSRO_ERROR(kComponent, "the Dear ImGui backends could not be initialised");
        ImGui::DestroyContext();
        destroy_device();
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
        UnregisterClassW(kClassName, wc.hInstance);
        return 0;
    }
    g_sink.set_device(g_device);

    TSRO_INFO(kComponent, "overlay ready: own " + std::to_string(w) + "x" + std::to_string(h) +
                              " layered window, nothing in the game is hooked");

    MSG msg{};
    while (!g_stop.load(std::memory_order_acquire)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_stop.store(true, std::memory_order_release); break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_stop.load(std::memory_order_acquire)) break;
        poll_menu_key();
        track_game_window();
        render_frame();
        // ~60 Hz. Present is unsynchronised so that it never paces the game; the sleep is what
        // stops this thread spinning a core.
        Sleep(8);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(kClassName, wc.hInstance);
    TSRO_INFO(kComponent, "overlay window closed");
    return 0;
}

}  // namespace

bool start_overlay_window(overlay::OverlayHost* host) {
    g_host = host;
    g_stop.store(false, std::memory_order_release);
    g_thread = CreateThread(nullptr, 0, &ui_thread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

void stop_overlay_window() {
    g_stop.store(true, std::memory_order_release);
    if (g_hwnd != nullptr) PostMessageW(g_hwnd, WM_QUIT, 0, 0);
    if (g_thread != nullptr) {
        // Bounded: a UI thread that will not come back must not hold up the process exit.
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    g_host = nullptr;
}

}  // namespace tsro::asi
