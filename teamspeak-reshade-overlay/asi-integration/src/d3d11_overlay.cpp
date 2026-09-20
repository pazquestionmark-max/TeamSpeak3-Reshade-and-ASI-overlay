// SPDX-License-Identifier: MIT
#include "d3d11_overlay.hpp"

#include <cctype>
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

    // The menu key is read before ImGui sees it, so the window can always be closed again even
    // if ImGui has swallowed keyboard focus.
    if (msg == WM_KEYDOWN && static_cast<int>(wparam) == menu_key_) {
        menu_open_ = !menu_open_;
        return 0;
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

bool D3D11Overlay::ensure_initialised(IDXGISwapChain* swap_chain) {
    if (initialised_) return true;
    if (swap_chain == nullptr) return false;

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
    window_ = desc.OutputWindow;

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

    if (!ImGui_ImplWin32_Init(window_)) {
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

    original_wnd_proc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&D3D11Overlay::wnd_proc)));
    if (original_wnd_proc_ == nullptr) {
        // Not fatal: the menu key is polled instead, and the settings window then works with
        // whatever input ImGui's backend can still see.
        TSRO_WARN(kComponent, "the window procedure could not be hooked; the menu key is polled "
                              "instead and mouse input may not reach the settings window");
    }

    sink_.set_device(device_);
    initialised_ = true;
    TSRO_INFO(kComponent, "Dear ImGui attached to the game's D3D11 swap chain");
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

void D3D11Overlay::poll_menu_key() {
    const bool down = (GetAsyncKeyState(menu_key_) & 0x8000) != 0;
    // Only when this window has the foreground: a background game must not eat the key.
    if (down && !menu_key_was_down_ && GetForegroundWindow() == window_) menu_open_ = !menu_open_;
    menu_key_was_down_ = down;
}

void D3D11Overlay::on_present(IDXGISwapChain* swap_chain) {
    if (host_ == nullptr || !host_->started()) return;
    if (!ensure_initialised(swap_chain)) return;
    if (!ensure_render_target(swap_chain)) return;
    // Re-read every frame so changing the key in the settings window takes effect at once.
    menu_key_ = virtual_key_from_name(host_->menu_key_name());
    if (original_wnd_proc_ == nullptr) poll_menu_key();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = menu_open_;

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
    if (window_ != nullptr && original_wnd_proc_ != nullptr) {
        SetWindowLongPtrW(window_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(original_wnd_proc_));
        original_wnd_proc_ = nullptr;
    }
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
    window_ = nullptr;
    menu_open_ = false;
}

}  // namespace tsro::asi
