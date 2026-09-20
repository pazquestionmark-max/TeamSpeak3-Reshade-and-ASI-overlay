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
// WHAT IS WRITTEN, and what deliberately is not. This replaces a pointer in a vtable. It does
// not modify a single byte of anybody's code. That distinction is the whole reason it is done
// this way:
//
//   * A game process routinely has other overlays in it -- ReShade proxying dxgi.dll, ENBSeries
//     proxying d3d11.dll -- and those hook by proxy DLL and by their own inline patches.
//     Rewriting a function prologue that another overlay has already rewritten, or that it is
//     about to, is how three overlays end up disagreeing about what the original bytes were.
//   * An inline patch changes executable memory, which is exactly what a code-integrity scan is
//     built to notice. A vtable entry is data.
//   * An aligned pointer-sized store is atomic on x64, so a thread calling through the slot at
//     that moment sees either the old function or the new one, never half of each. That removes
//     the need to suspend threads while patching, which is what the vendored MinHook could not
//     do, and with it the requirement that hooks be installed before the first frame.
//
// Stated plainly, because it matters to the people installing this: patching a vtable is
// exactly what a code-integrity check looks for, and an anti-cheat that sees it has no way to
// tell this overlay from something that is not an overlay. FiveM servers in "pure mode" block
// .asi plugins outright. The ReShade add-on is the lower-risk of the two front ends, and this
// one exists because the overlay should not require ReShade -- not because hooking is free.
// See docs/asi-plugin.md.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include "d3d11_overlay.hpp"
#include "overlay_host.hpp"
#include "tsro/log.hpp"
#include "tsro/profile_store.hpp"

namespace {

constexpr char kComponent[] = "asi";

/// One replaced vtable entry, remembered so it can be put back.
struct VTableSlot {
    void** slot = nullptr;
    void* original = nullptr;

    /// Writes `detour` into the slot and hands back what was there.
    bool install(void** vtable, std::size_t index, void* detour) {
        if (vtable == nullptr) return false;
        void** target = vtable + index;
        DWORD previous = 0;
        if (!VirtualProtect(target, sizeof(void*), PAGE_READWRITE, &previous)) return false;
        original = *target;
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(target), detour);
        DWORD ignored = 0;
        VirtualProtect(target, sizeof(void*), previous, &ignored);
        slot = target;
        return true;
    }

    /// Only puts the original back if the slot still holds our detour. Another overlay may have
    /// hooked the same entry after us, and stamping over its pointer would break it.
    void remove(void* detour) {
        if (slot == nullptr || original == nullptr) return;
        DWORD previous = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
            if (*slot == detour) *slot = original;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void*), previous, &ignored);
        }
        slot = nullptr;
        original = nullptr;
    }
};

/// Which module a function belongs to, for the log. When the overlay is one of several things
/// in the process with an interest in Present, knowing whose Present we took matters -- and it
/// is the first thing anyone would want from a crash report.
std::string describe(void* address) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module != nullptr) {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path))) > 0) {
            std::string full(path);
            const std::size_t cut = full.find_last_of("\\/");
            const std::string name = cut == std::string::npos ? full : full.substr(cut + 1);
            const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(address) -
                                          reinterpret_cast<std::uintptr_t>(module);
            char buffer[64] = {};
            std::snprintf(buffer, sizeof(buffer), "+0x%llX",
                          static_cast<unsigned long long>(offset));
            return name + buffer + "  (" + full + ")";
        }
    }
    return "an address in no loaded module";
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);

PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resize_buffers = nullptr;
VTableSlot g_present_slot;
VTableSlot g_present1_slot;
VTableSlot g_resize_slot;

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
    // this throwaway are the same ones the game's swap chain will call through.
    void** vtable = *reinterpret_cast<void***>(swap_chain);

    // Present1 lives on IDXGISwapChain1. Games using the flip model call it instead of Present,
    // so it is taken too when the runtime has it; both land in the same draw path and only one
    // of them is called per frame.
    void** vtable1 = nullptr;
    IDXGISwapChain1* swap_chain1 = nullptr;
    if (SUCCEEDED(swap_chain->QueryInterface(__uuidof(IDXGISwapChain1),
                                             reinterpret_cast<void**>(&swap_chain1))) &&
        swap_chain1 != nullptr) {
        vtable1 = *reinterpret_cast<void***>(swap_chain1);
        swap_chain1->Release();
    }

    // Say whose functions these are before touching them. With ReShade proxying dxgi.dll and
    // ENBSeries proxying d3d11.dll, "Present" is not necessarily Microsoft's.
    TSRO_INFO(kComponent, "IDXGISwapChain::Present is " + describe(vtable[8]));
    TSRO_INFO(kComponent, "IDXGISwapChain::ResizeBuffers is " + describe(vtable[13]));

    swap_chain->Release();
    if (context != nullptr) context->Release();
    if (device != nullptr) device->Release();

    if (!g_present_slot.install(vtable, 8, reinterpret_cast<void*>(&hooked_present)) ||
        !g_resize_slot.install(vtable, 13, reinterpret_cast<void*>(&hooked_resize_buffers))) {
        TSRO_ERROR(kComponent, "the DXGI vtable entries could not be replaced");
        g_present_slot.remove(reinterpret_cast<void*>(&hooked_present));
        g_resize_slot.remove(reinterpret_cast<void*>(&hooked_resize_buffers));
        return false;
    }
    g_present = reinterpret_cast<PresentFn>(g_present_slot.original);
    g_resize_buffers = reinterpret_cast<ResizeBuffersFn>(g_resize_slot.original);

    if (vtable1 != nullptr && vtable1[22] != vtable[8]) {
        TSRO_INFO(kComponent, "IDXGISwapChain1::Present1 is " + describe(vtable1[22]));
        if (g_present1_slot.install(vtable1, 22, reinterpret_cast<void*>(&hooked_present1))) {
            g_present1 = reinterpret_cast<Present1Fn>(g_present1_slot.original);
        } else {
            // Not fatal: a game that never calls Present1 loses nothing, and one that does
            // shows no overlay rather than crashing.
            TSRO_WARN(kComponent, "the Present1 entry could not be replaced; the overlay will "
                                  "not draw in games that use the flip presentation model");
        }
    }

    g_hooks_installed.store(true, std::memory_order_release);
    TSRO_INFO(kComponent, "DXGI vtable entries replaced (no code was modified)");
    return true;
}

/// Opens the log before anything can fail.
///
/// The hooks go in before the profile is read -- they have to, see the note at the top of this
/// file -- which means every diagnostic from install_hooks() would otherwise be written before
/// the logger had a file to write to, and lost. A failure to attach is precisely the failure
/// with no other symptom: the game runs, the overlay does not, and nothing says why. So the
/// default path is opened first, and OverlayHost::start() later refines the level and
/// destination from the profile without discarding what is already there.
void open_log_early() {
    const std::string root = tsro::ProfileStore::default_root();
    if (root.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    tsro::Logger::instance().configure(tsro::LogLevel::Info, true, root + "/tsro-overlay.log", 1024);
}

/// Everything that must not happen on the loader lock: creating a D3D device loads more DLLs,
/// which deadlocks if attempted from DllMain.
/// Other overlays in the same process, named in the log before anything else happens.
///
/// A game process is a crowded place. ReShade installs itself as a proxy dxgi.dll and ENBSeries
/// as a proxy d3d11.dll, both of which sit in front of the functions this plugin replaces, and
/// a crash in that arrangement is otherwise a guessing game. When ReShade is one of them the
/// ReShade add-on is the better answer anyway -- same overlay, nothing hooked -- so say so.
void report_graphics_mods() {
    static const char* kNames[] = {"dxgi.dll", "d3d11.dll", "d3d12.dll", "opengl32.dll"};
    bool reshade = false;
    for (const char* name : kNames) {
        const HMODULE module = GetModuleHandleA(name);
        if (module == nullptr) continue;
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path))) == 0) continue;
        std::string full(path);
        // A copy loaded from anywhere but System32 is a proxy standing in for the real one.
        if (full.find("\\Windows\\System32\\") != std::string::npos ||
            full.find("\\windows\\system32\\") != std::string::npos) {
            continue;
        }
        TSRO_WARN(kComponent, std::string("another graphics mod is loaded as ") + name + ": " + full);
        if (GetProcAddress(module, "ReShadeRegisterAddon") != nullptr ||
            GetProcAddress(module, "ReShadeRegisterEvent") != nullptr) {
            reshade = true;
        }
    }
    if (reshade) {
        TSRO_WARN(kComponent,
                  "ReShade is already running in this process, and it supports add-ons. The "
                  "ReShade add-on build of this overlay hooks nothing at all and is the better "
                  "choice here: install TeamSpeakOverlay.addon64 beside ReShade and remove this "
                  ".asi. See docs/asi-plugin.md.");
    }
}

DWORD WINAPI bootstrap(LPVOID) {
    open_log_early();
    TSRO_INFO(kComponent, std::string("TeamSpeak Overlay .asi ") + TSRO_VERSION + " (build " +
                              TSRO_BUILD_ID + ") loaded; attaching to the renderer");
    report_graphics_mods();
    // Hooks first, so a frame is never missed while the profile is read off disk. Until
    // set_host below they pass every frame straight through -- on_present returns immediately
    // with no host -- so there is no window in which a half-built overlay draws.
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
        g_present_slot.remove(reinterpret_cast<void*>(&hooked_present));
        g_present1_slot.remove(reinterpret_cast<void*>(&hooked_present1));
        g_resize_slot.remove(reinterpret_cast<void*>(&hooked_resize_buffers));
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
