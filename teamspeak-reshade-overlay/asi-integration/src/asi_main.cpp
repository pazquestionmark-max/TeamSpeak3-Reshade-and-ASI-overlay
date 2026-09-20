// SPDX-License-Identifier: MIT
// Standalone .asi plugin entry point (Component A, second front end).
//
// Same overlay, no ReShade. ReShade gives an add-on a Dear ImGui frame and a device for free;
// without it the plugin has to take both, and the way to do that without patching the game's
// code is to replace three entries in the swap chain's vtable:
//
//   IDXGISwapChain::Present        (8)  -- draw here, just before the frame is handed to DXGI
//   IDXGISwapChain::ResizeBuffers (13)  -- drop our back-buffer view before the buffers go
//   IDXGISwapChain1::Present1     (22)  -- the flip-model path, used instead of Present
//
// The vtable is found by making a throwaway device and swap chain on a hidden window: every
// IDXGISwapChain in the process shares one vtable, so reading it from a swap chain of our own
// is equivalent to reading it from the game's, and needs no pattern scan and no assumption
// about the game's build.
//
// Stated plainly, because it matters to the people installing this: patching a vtable is
// exactly what a code-integrity check looks for, and an anti-cheat that sees it has no way to
// tell this overlay from something that is not an overlay. FiveM servers in "pure mode" block
// .asi plugins outright. The ReShade add-on is the lower-risk of the two front ends, and this
// one exists because the overlay should not require ReShade -- not because hooking is free.
// See docs/asi-plugin.md.
//
// WHEN the hooks go in is load-bearing, not incidental. The vendored MinHook has upstream's
// thread-freeze deleted (third_party/minhook/UPSTREAM.md says why), so it writes the patch
// without suspending anything. That is safe only while no thread can be executing the bytes
// being replaced -- which holds when an ASI loader loads this plugin at process start, before
// the game has created a device or presented a frame, and does not hold if this DLL is injected
// into a game that is already running. So: hooks first, before the profile and the IPC client,
// and injecting into a running process is not supported.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <MinHook.h>

#include "d3d11_overlay.hpp"
#include "overlay_host.hpp"
#include "tsro/log.hpp"
#include "tsro/profile_store.hpp"

namespace {

constexpr char kComponent[] = "asi";

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);

PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resize_buffers = nullptr;

tsro::overlay::OverlayHost* g_host = nullptr;
HMODULE g_module = nullptr;
std::atomic<bool> g_hooks_installed{false};
/// Set while the process is tearing down, so a Present still in flight on another thread stops
/// touching state that is being destroyed.
std::atomic<bool> g_shutting_down{false};

/// The folder the .asi was loaded from, which is where its shipped `fonts` folder sits.
std::string module_directory() {
    if (g_module == nullptr) return {};
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(g_module, path, static_cast<DWORD>(sizeof(path)));
    if (n == 0 || n >= sizeof(path)) return {};
    std::string s(path, n);
    const std::size_t cut = s.find_last_of("\\/");
    return cut == std::string::npos ? std::string{} : s.substr(0, cut);
}

// --- the hooks ------------------------------------------------------------------------------
//
// Each one draws (or tidies up) and then calls the original unconditionally. A frame is never
// dropped and never presented twice, whatever the overlay does or fails to do.

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* swap_chain, UINT interval, UINT flags) {
    if (!g_shutting_down.load(std::memory_order_acquire)) {
        // DXGI_PRESENT_TEST does not present anything -- it asks whether a present would
        // succeed -- so drawing into that frame would be wasted work on a buffer nobody shows.
        if ((flags & DXGI_PRESENT_TEST) == 0) {
            tsro::asi::overlay_instance().on_present(swap_chain);
        }
    }
    return g_present(swap_chain, interval, flags);
}

HRESULT STDMETHODCALLTYPE hooked_present1(IDXGISwapChain1* swap_chain, UINT interval, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS* parameters) {
    if (!g_shutting_down.load(std::memory_order_acquire)) {
        if ((flags & DXGI_PRESENT_TEST) == 0) {
            tsro::asi::overlay_instance().on_present(swap_chain);
        }
    }
    return g_present1(swap_chain, interval, flags, parameters);
}

HRESULT STDMETHODCALLTYPE hooked_resize_buffers(IDXGISwapChain* swap_chain, UINT buffer_count,
                                                UINT width, UINT height, DXGI_FORMAT format,
                                                UINT flags) {
    // Before, not after: ResizeBuffers fails outright while anything still references a back
    // buffer, and the view onto it is ours.
    tsro::asi::overlay_instance().on_resize_buffers();
    return g_resize_buffers(swap_chain, buffer_count, width, height, format, flags);
}

// --- finding the vtable ---------------------------------------------------------------------

/// A hidden window to hang the throwaway swap chain off. Never shown, never given messages of
/// consequence, destroyed as soon as the vtable has been read.
class ScratchWindow {
public:
    ScratchWindow() {
        klass_.cbSize = sizeof(WNDCLASSEXW);
        klass_.style = CS_HREDRAW | CS_VREDRAW;
        klass_.lpfnWndProc = DefWindowProcW;
        klass_.hInstance = GetModuleHandleW(nullptr);
        klass_.lpszClassName = L"TsroOverlayScratch";
        if (RegisterClassExW(&klass_) == 0) return;
        registered_ = true;
        hwnd_ = CreateWindowExW(0, klass_.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16,
                                nullptr, nullptr, klass_.hInstance, nullptr);
    }
    ~ScratchWindow() {
        if (hwnd_ != nullptr) DestroyWindow(hwnd_);
        if (registered_) UnregisterClassW(klass_.lpszClassName, klass_.hInstance);
    }
    ScratchWindow(const ScratchWindow&) = delete;
    ScratchWindow& operator=(const ScratchWindow&) = delete;

    HWND handle() const noexcept { return hwnd_; }

private:
    WNDCLASSEXW klass_ = {};
    HWND hwnd_ = nullptr;
    bool registered_ = false;
};

bool install_hooks() {
    ScratchWindow window;
    if (window.handle() == nullptr) {
        TSRO_ERROR(kComponent, "could not create the scratch window used to read the DXGI vtable");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 16;
    desc.BufferDesc.Height = 16;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window.handle();
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
        static_cast<UINT>(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION, &desc,
        &swap_chain, &device, nullptr, &context);
    if (FAILED(hr) || swap_chain == nullptr) {
        TSRO_ERROR(kComponent, "D3D11CreateDeviceAndSwapChain failed; the overlay cannot attach");
        if (context != nullptr) context->Release();
        if (device != nullptr) device->Release();
        return false;
    }

    // One vtable is shared by every IDXGISwapChain in the process, so the entries read from
    // this throwaway are the same functions the game's swap chain will call.
    void** vtable = *reinterpret_cast<void***>(swap_chain);
    void* present = vtable[8];
    void* resize_buffers = vtable[13];

    // Present1 lives on IDXGISwapChain1. Games using the flip model call it instead of Present,
    // so it is hooked too when the runtime has it; both hooks land in the same draw path and
    // only one of them is called per frame.
    void* present1 = nullptr;
    IDXGISwapChain1* swap_chain1 = nullptr;
    if (SUCCEEDED(swap_chain->QueryInterface(__uuidof(IDXGISwapChain1),
                                             reinterpret_cast<void**>(&swap_chain1))) &&
        swap_chain1 != nullptr) {
        void** vtable1 = *reinterpret_cast<void***>(swap_chain1);
        present1 = vtable1[22];
        swap_chain1->Release();
    }

    swap_chain->Release();
    if (context != nullptr) context->Release();
    if (device != nullptr) device->Release();

    if (MH_Initialize() != MH_OK) {
        TSRO_ERROR(kComponent, "MinHook could not be initialised");
        return false;
    }

    bool ok = MH_CreateHook(present, reinterpret_cast<void*>(&hooked_present),
                            reinterpret_cast<void**>(&g_present)) == MH_OK;
    ok = ok && MH_CreateHook(resize_buffers, reinterpret_cast<void*>(&hooked_resize_buffers),
                             reinterpret_cast<void**>(&g_resize_buffers)) == MH_OK;
    if (!ok) {
        TSRO_ERROR(kComponent, "the DXGI Present/ResizeBuffers hooks could not be created");
        MH_Uninitialize();
        return false;
    }
    if (present1 != nullptr && present1 != present) {
        // Not fatal: a game that never calls Present1 loses nothing, and one that does simply
        // shows no overlay rather than crashing.
        if (MH_CreateHook(present1, reinterpret_cast<void*>(&hooked_present1),
                          reinterpret_cast<void**>(&g_present1)) != MH_OK) {
            TSRO_WARN(kComponent, "the Present1 hook could not be created; the overlay will not "
                                  "draw in games that use the flip presentation model");
            g_present1 = nullptr;
        }
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        TSRO_ERROR(kComponent, "the DXGI hooks could not be enabled");
        MH_Uninitialize();
        return false;
    }

    g_hooks_installed.store(true, std::memory_order_release);
    TSRO_INFO(kComponent, "DXGI hooks installed");
    return true;
}

/// Everything that must not happen on the loader lock: MinHook suspends threads, and creating a
/// D3D device loads more DLLs. Both deadlock if attempted from DllMain.
DWORD WINAPI bootstrap(LPVOID) {
    // Hooks first and nothing before them: see the note at the top of this file. Reading a
    // profile off disk takes milliseconds, and spending those milliseconds before the patch
    // goes in is spending them on the wrong side of the one window where the patch is safe.
    //
    // Until set_host below, the hooks pass every frame straight through -- on_present returns
    // immediately with no host -- so there is no window where a half-built overlay draws.
    if (!install_hooks()) {
        TSRO_ERROR(kComponent, "the overlay could not attach to the game's renderer");
        return 0;
    }

    g_host = new tsro::overlay::OverlayHost("asi-plugin", kComponent);
    // The user's own folder first, then the one shipped beside the .asi, so a font dropped in
    // the config directory wins over one of the same name that came with the release.
    g_host->start({tsro::ProfileStore::default_root() + "/fonts", module_directory() + "/fonts"},
                  module_directory() + "/profiles");

    // Only once the host is running, so the first frame that draws has something to draw.
    tsro::asi::overlay_instance().set_host(g_host);
    TSRO_INFO(kComponent, "plugin initialised");
    return 0;
}

void shutdown(bool process_exiting) {
    g_shutting_down.store(true, std::memory_order_release);

    if (process_exiting) {
        // The process is going away and other threads have already been killed, possibly inside
        // a hooked function. Unhooking now would be a write into code another thread might be
        // executing, and releasing D3D objects would run driver code on a dying process. Let
        // the operating system reclaim it.
        return;
    }

    tsro::asi::overlay_instance().set_host(nullptr);
    if (g_hooks_installed.exchange(false, std::memory_order_acq_rel)) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    // The host releases its font textures through the sink, so it has to stop before the sink's
    // device does.
    if (g_host != nullptr) {
        g_host->stop();
        delete g_host;
        g_host = nullptr;
    }
    tsro::asi::overlay_instance().shutdown();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_module = module;
            DisableThreadLibraryCalls(module);
            const HANDLE thread = CreateThread(nullptr, 0, &bootstrap, nullptr, 0, nullptr);
            if (thread != nullptr) {
                CloseHandle(thread);
            } else {
                // Nothing else can be done from here, and the game must still start.
                OutputDebugStringA("[TeamSpeakOverlay] bootstrap thread could not be created\n");
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            // lpReserved is non-null when the process is exiting rather than the library being
            // unloaded, and the two need very different treatment.
            shutdown(reserved != nullptr);
            break;
        default:
            break;
    }
    return TRUE;
}
