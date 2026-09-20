// SPDX-License-Identifier: MIT
#include "d3d11_overlay.hpp"

#include <cctype>
#include <cstdint>
#include <chrono>
#include <string>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "tsro/log.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace tsro::asi {
namespace {

constexpr char kComponent[] = "asi";

/// Names to virtual keys. The names themselves live in shared/, because the settings window
/// offers the same closed list and the two must not drift; the VK codes stay here, because
/// nothing in shared/ is allowed to know about Win32.
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

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// True for the messages the game must not see while the settings window has the mouse and
/// keyboard. Window management, focus and painting are deliberately not in this list: swallowing
/// those is how an overlay leaves a game unable to resize, minimise or regain focus.
bool is_input_message(UINT msg) {
    switch (msg) {
        case WM_MOUSEMOVE:
        case WM_NCMOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_INPUT:
            return true;
        default:
            return false;
    }
}

/// How long the startup hint stays up.
///
/// Not decoration. The shipped default profile hides the HUD entirely while TeamSpeak is closed
/// or its plugin is not enabled -- which is the correct thing for everyday use and completely
/// wrong for the first launch, because a working overlay and a broken one then look identical:
/// nothing on screen either way. Under ReShade there is at least a menu entry to find. Here
/// there is nothing but a key nobody has been told about yet. So the plugin says so itself,
/// once, and then gets out of the way.
constexpr std::int64_t kHintMs = 15000;
constexpr std::int64_t kHintFadeMs = 2500;

}  // namespace

D3D11Overlay& overlay_instance() {
    static D3D11Overlay instance;
    return instance;
}

LRESULT CALLBACK D3D11Overlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return overlay_instance().handle_message(hwnd, msg, wparam, lparam);
}

LRESULT D3D11Overlay::handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    WNDPROC original = original_wnd_proc_;

    // The menu key is deliberately NOT handled here. It is polled once per frame instead, which
    // works whether or not this hook went in and whichever window ends up with focus -- and
    // doing it in both places would toggle twice per press and net out to nothing.
    if (msg == WM_KEYDOWN && (static_cast<int>(wparam) == menu_key_ ||
                              static_cast<int>(wparam) == VK_INSERT)) {
        return 0;   // still swallowed, so the game does not also act on it
    }

    if (initialised_ && ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        if (menu_open_ && is_input_message(msg)) {
            // Held down while the menu opened, the game would never see the key-up and would
            // keep steering. Only what the settings window is actually using is swallowed.
            const ImGuiIO& io = ImGui::GetIO();
            const bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
            if ((mouse && io.WantCaptureMouse) || (!mouse && io.WantCaptureKeyboard) ||
                msg == WM_INPUT) {
                return 0;
            }
        }
    }

    if (original == nullptr) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return CallWindowProcW(original, hwnd, msg, wparam, lparam);
}

void D3D11Overlay::hook_window(HWND window) {
    if (window == nullptr || window == window_) return;
    unhook_window();
    window_ = window;
    original_wnd_proc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(&D3D11Overlay::wnd_proc)));
    if (original_wnd_proc_ == nullptr) {
        // Not fatal: the menu key is polled every frame regardless, so the settings window can
        // still be opened. Only mouse and text input into it are affected.
        TSRO_WARN(kComponent, "the window procedure could not be hooked; the menu key is polled "
                              "instead and mouse input may not reach the settings window");
    }
}

void D3D11Overlay::unhook_window() {
    if (window_ != nullptr && original_wnd_proc_ != nullptr) {
        SetWindowLongPtrW(window_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(original_wnd_proc_));
    }
    original_wnd_proc_ = nullptr;
    window_ = nullptr;
}

/// A game does not necessarily present from one swap chain for its whole life. FiveM shows a
/// loading screen on one and the game on another; a resolution or display-mode change can
/// produce a third. Binding to the first one seen and never checking again means drawing into a
/// back buffer nobody shows any more -- invisible, with every log line still reporting success,
/// and the window hook left on a window that no longer has focus.
bool D3D11Overlay::rebind(IDXGISwapChain* swap_chain) {
    ID3D11Device* device = nullptr;
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) ||
        device == nullptr) {
        return false;   // not a D3D11 swap chain; leave the old binding alone
    }
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swap_chain->GetDesc(&desc)) || desc.OutputWindow == nullptr) {
        device->Release();
        return false;
    }

    release_render_target();
    swap_chain_ = swap_chain;
    ++rebind_count_;

    if (device != device_) {
        // A different device means the ImGui D3D11 backend and every font texture belong to
        // something that is going away. Rebuild both.
        sink_.release();
        sink_.set_device(nullptr);
        if (initialised_) ImGui_ImplDX11_Shutdown();
        if (context_ != nullptr) { context_->Release(); context_ = nullptr; }
        if (device_ != nullptr) { device_->Release(); device_ = nullptr; }
        device_ = device;                     // the reference from GetDevice is kept
        device_->GetImmediateContext(&context_);
        if (initialised_ && !ImGui_ImplDX11_Init(device_, context_)) {
            TSRO_ERROR(kComponent, "the D3D11 ImGui backend could not be rebuilt");
            return false;
        }
        sink_.set_device(device_);
    } else {
        device->Release();                    // already held
    }

    hook_window(desc.OutputWindow);
    // Start the hint again on the swap chain the player is actually looking at. On FiveM the
    // first one is the loading screen, and a hint that spent its fifteen seconds there was
    // never seen by anyone.
    first_frame_ms_ = 0;
    hint_done_ = false;
    TSRO_INFO(kComponent, "bound to swap chain " + std::to_string(rebind_count_) + ": " +
                              std::to_string(desc.BufferDesc.Width) + "x" +
                              std::to_string(desc.BufferDesc.Height) + ", " +
                              std::to_string(desc.BufferCount) + " buffer(s), hwnd " +
                              std::to_string(reinterpret_cast<std::uintptr_t>(desc.OutputWindow)));
    return true;
}

bool D3D11Overlay::ensure_initialised(IDXGISwapChain* swap_chain) {
    if (swap_chain == nullptr) return false;
    // The swap chain being presented is not necessarily the one we bound to last frame.
    if (initialised_) return swap_chain == swap_chain_ ? true : rebind(swap_chain);

    ID3D11Device* device = nullptr;
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) ||
        device == nullptr) {
        // A D3D12 or D3D10 swap chain. The overlay does not draw; it also does not crash, and
        // the Present hook keeps passing frames through untouched.
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swap_chain->GetDesc(&desc)) || desc.OutputWindow == nullptr) {
        device->Release();
        return false;
    }

    device_ = device;  // the reference from GetDevice is kept until shutdown
    device_->GetImmediateContext(&context_);
    swap_chain_ = swap_chain;
    rebind_count_ = 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No imgui.ini: the overlay is a guest in someone else's folder and has no business
    // writing layout files next to the game executable.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // The game hides the OS cursor; ImGui draws its own so the settings window is usable.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(desc.OutputWindow)) {
        TSRO_ERROR(kComponent, "the Win32 ImGui backend could not be initialised");
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(device_, context_)) {
        TSRO_ERROR(kComponent, "the D3D11 ImGui backend could not be initialised");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    hook_window(desc.OutputWindow);

    sink_.set_device(device_);
    initialised_ = true;
    TSRO_INFO(kComponent, "Dear ImGui attached: " + std::to_string(desc.BufferDesc.Width) + "x" +
                              std::to_string(desc.BufferDesc.Height) + ", " +
                              std::to_string(desc.BufferCount) + " buffer(s), hwnd " +
                              std::to_string(reinterpret_cast<std::uintptr_t>(desc.OutputWindow)));
    return true;
}

bool D3D11Overlay::ensure_render_target(IDXGISwapChain* swap_chain) {
    if (render_target_ != nullptr) return true;
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void**>(&back_buffer))) ||
        back_buffer == nullptr) {
        return false;
    }
    const HRESULT hr = device_->CreateRenderTargetView(back_buffer, nullptr, &render_target_);
    back_buffer->Release();
    return SUCCEEDED(hr) && render_target_ != nullptr;
}

void D3D11Overlay::release_render_target() {
    if (render_target_ != nullptr) {
        render_target_->Release();
        render_target_ = nullptr;
    }
}

void D3D11Overlay::on_resize_buffers() {
    // ResizeBuffers fails outright while anything holds a reference to a back buffer, so the
    // view has to go before the original call, not after it.
    release_render_target();
}

/// True when the window in front belongs to this process.
///
/// Checked against the process rather than against our own HWND on purpose: a game can own
/// several windows, and focus does not always sit on the one whose swap chain we draw into.
/// Comparing handles meant a missed key with no way for anyone to tell why.
namespace {
bool foreground_is_ours() {
    const HWND fg = GetForegroundWindow();
    if (fg == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
}  // namespace

void D3D11Overlay::poll_menu_key() {
    // Insert is always live, on top of whatever key the profile names. It is the one documented
    // everywhere and the one people reach for, and a settings window that cannot be opened is a
    // plugin with no way to configure it -- so it does not depend on the profile parsing, on the
    // window hook going in, or on us having picked the right window.
    const bool down = ((GetAsyncKeyState(menu_key_) & 0x8000) != 0) ||
                      ((GetAsyncKeyState(VK_INSERT) & 0x8000) != 0);

    // Edge off the raw key state first and test the foreground second, not the other way round.
    // Folding focus into `down` means a key held while another window was in front fires the
    // moment focus comes back, which reads as the overlay opening itself.
    if (down && !menu_key_was_down_) {
        if (foreground_is_ours()) {
            menu_open_ = !menu_open_;
            hint_done_ = true;
            TSRO_INFO(kComponent, menu_open_ ? "settings window opened" : "settings window closed");
        } else {
            // Worth a line: "the key does nothing" and "the key is being ignored because the
            // game is not in front" look identical from the outside.
            TSRO_DEBUG(kComponent, "menu key ignored: the foreground window is not ours");
        }
    }
    menu_key_was_down_ = down;
}

void D3D11Overlay::draw_startup_hint(std::int64_t now) {
    // Opening the settings answers the question the hint exists to answer.
    if (menu_open_) hint_done_ = true;
    if (hint_done_) return;
    const std::int64_t age = now - first_frame_ms_;
    if (age > kHintMs) { hint_done_ = true; return; }

    float alpha = 1.0f;
    if (age > kHintMs - kHintFadeMs) {
        alpha = static_cast<float>(kHintMs - age) / static_cast<float>(kHintFadeMs);
    }
    if (alpha <= 0.0f) return;

    const std::string line =
        "Paz' TeamSpeak Overlay " TSRO_VERSION "  -  press " + menu_key_name_ + " for settings";

    // ImGui's own font, not the overlay's: the font engine may still be baking its first atlas,
    // and a hint that does not draw for two seconds is a hint that misses its moment.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (dl == nullptr) return;
    const ImVec2 size = ImGui::CalcTextSize(line.c_str());
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float x = (screen.x - size.x) * 0.5f;
    const float y = screen.y * 0.08f;
    const float pad = 8.0f;

    const auto fade = [alpha](float a) { return static_cast<unsigned>(a * alpha * 255.0f) << 24; };
    dl->AddRectFilled(ImVec2(x - pad, y - pad * 0.5f),
                      ImVec2(x + size.x + pad, y + size.y + pad * 0.5f),
                      fade(0.60f) | 0x00100C0Au, 4.0f);
    dl->AddText(ImVec2(x + 1.0f, y + 1.0f), fade(0.85f) | 0x00000000u, line.c_str());
    dl->AddText(ImVec2(x, y), fade(1.0f) | 0x00FFFFFFu, line.c_str());
}

void D3D11Overlay::on_present(IDXGISwapChain* swap_chain) {
    if (host_ == nullptr || !host_->started()) return;
    if (!ensure_initialised(swap_chain)) return;
    if (!ensure_render_target(swap_chain)) return;
    // Re-read every frame so changing the key in the settings window takes effect at once.
    menu_key_name_ = host_->menu_key_name();
    menu_key_ = virtual_key_from_name(menu_key_name_);
    poll_menu_key();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = menu_open_;

    if (first_frame_ms_ == 0) first_frame_ms_ = now_ms();
    draw_startup_hint(now_ms());

    Viewport viewport;
    viewport.width = io.DisplaySize.x;
    viewport.height = io.DisplaySize.y;
    host_->draw_hud(ImGui::GetBackgroundDrawList(), viewport, &sink_, now_ms());

    if (menu_open_) {
        ImGui::SetNextWindowSize(ImVec2(620.0f, 720.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.94f);
        if (ImGui::Begin("TeamSpeak Overlay", &menu_open_)) host_->draw_settings();
        ImGui::End();
    }

    ImGui::Render();
    context_->OMSetRenderTargets(1, &render_target_, nullptr);
    // imgui_impl_dx11 saves and restores the whole pipeline state around this call, so the
    // game's next draw sees exactly what it left behind.
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void D3D11Overlay::shutdown() {
    unhook_window();
    if (initialised_) {
        // Font textures first: they were made on the device that is about to be released.
        sink_.release();
        sink_.set_device(nullptr);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        initialised_ = false;
    }
    release_render_target();
    if (context_ != nullptr) { context_->Release(); context_ = nullptr; }
    if (device_ != nullptr) { device_->Release(); device_ = nullptr; }
    swap_chain_ = nullptr;
    menu_open_ = false;
}

}  // namespace tsro::asi
