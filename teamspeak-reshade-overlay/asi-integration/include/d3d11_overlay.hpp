// SPDX-License-Identifier: MIT
// The Dear ImGui frame the .asi build has to provide for itself.
//
// Under ReShade the overlay is handed a live ImGui frame every time ReShade draws one. There is
// no ReShade here, so this owns the whole of it: an ImGui context, the Win32 and D3D11
// backends, a render target view onto the game's back buffer, and the window procedure that
// feeds keyboard and mouse to the settings window.
//
// Everything is created lazily on the first Present, because that is the first moment a swap
// chain, a device and a window are all known to exist.
#ifndef TSRO_D3D11_OVERLAY_HPP
#define TSRO_D3D11_OVERLAY_HPP

#include <cstdint>
#include <string>

#include <d3d11.h>
#include <dxgi.h>

#include "d3d11_font_sink.hpp"
#include "overlay_host.hpp"

namespace tsro::asi {

class D3D11Overlay {
public:
    /// The host whose HUD and settings window are drawn. Borrowed, never owned.
    void set_host(overlay::OverlayHost* host) noexcept { host_ = host; }

    /// The virtual-key code that opens and closes the settings window.
    void set_menu_key(int vk) noexcept { menu_key_ = vk; }
    int menu_key() const noexcept { return menu_key_; }
    bool menu_open() const noexcept { return menu_open_; }

    /// One frame. Called from the Present hook, on the render thread.
    void on_present(IDXGISwapChain* swap_chain);
    /// Called from the ResizeBuffers hook *before* the original runs: the back buffer cannot be
    /// resized while we hold a view onto it.
    void on_resize_buffers();

    /// Drops the ImGui context, the backends, the font textures and the window hook. Safe to
    /// call when nothing was ever initialised.
    void shutdown();

    /// The hooked window procedure. Installed on the swap chain's window at first Present.
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    bool ensure_initialised(IDXGISwapChain* swap_chain);
    /// Moves everything that belongs to one swap chain -- back-buffer view, window hook, and
    /// the D3D11 backend if the device changed too -- onto `swap_chain`. See the note at the
    /// call site for why a game presents from more than one.
    bool rebind(IDXGISwapChain* swap_chain);
    void hook_window(HWND window);
    void unhook_window();
    bool ensure_render_target(IDXGISwapChain* swap_chain);
    void release_render_target();
    /// Edge-detected fallback for the menu key, used only when the window hook could not be
    /// installed -- without it there would be no way to open the settings at all.
    void poll_menu_key();
    /// A few seconds of "I am here, and this is the key" on first load. See the note on
    /// kHintMs in the .cpp for why this is not optional polish.
    void draw_startup_hint(std::int64_t now_ms);
    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    overlay::OverlayHost* host_ = nullptr;
    D3D11FontSink sink_;

    bool initialised_ = false;
    /// The swap chain the state below belongs to. Latching onto the first one seen and never
    /// checking again is what made the overlay invisible on FiveM: it presents a loading-screen
    /// swap chain first and the game's real one afterwards.
    IDXGISwapChain* swap_chain_ = nullptr;
    int rebind_count_ = 0;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11RenderTargetView* render_target_ = nullptr;
    /// The back buffer's own size. The frame is sized from this and never from the window:
    /// which window ImGui's Win32 backend is holding is a separate question, and getting it
    /// wrong should cost correct mouse coordinates, not a frame rendered at the wrong size
    /// into the corner of the screen.
    UINT back_buffer_width_ = 0;
    UINT back_buffer_height_ = 0;
    HWND window_ = nullptr;
    WNDPROC original_wnd_proc_ = nullptr;

    int menu_key_ = VK_INSERT;
    std::string menu_key_name_ = "INSERT";
    bool menu_open_ = false;
    bool menu_key_was_down_ = false;

    DWORD render_thread_ = 0;
    bool logged_threads_ = false;

    std::int64_t first_frame_ms_ = 0;
    bool hint_done_ = false;
};

/// The one instance the hooks call into. A single global rather than a parameter because a
/// vtable hook has nowhere to put state.
D3D11Overlay& overlay_instance();

}  // namespace tsro::asi

#endif  // TSRO_D3D11_OVERLAY_HPP
