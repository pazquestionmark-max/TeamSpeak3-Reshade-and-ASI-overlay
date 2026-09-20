// SPDX-License-Identifier: MIT
// Standalone .asi plugin entry point (Component A, second front end).
//
// Same overlay as the ReShade add-on, without ReShade. ReShade gives an add-on a Dear ImGui
// frame and a device for free; this has to provide both itself.
//
// It does that with a window of its own -- transparent, click-through, always on top, sized to
// the game's client area -- and not by touching the game's renderer. See overlay_window.hpp for
// why: intercepting Present worked and FiveM killed the process for it anyway, both when the
// interception was an inline patch and when it was a replaced vtable pointer. What is objected
// to is the interception, not the technique.
//
// What is left here is small on purpose: load the profile, open the IPC client, start the UI
// thread. Nothing in this file modifies, reads or hooks anything belonging to the game.
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "overlay_host.hpp"
#include "overlay_window.hpp"
#include "tsro/log.hpp"
#include "tsro/profile_store.hpp"

namespace {

constexpr char kComponent[] = "asi";

tsro::overlay::OverlayHost* g_host = nullptr;
HMODULE g_module = nullptr;
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

/// Opens the log before anything can fail, so a failure during start-up leaves evidence.
/// OverlayHost::start() refines the level and destination from the profile afterwards without
/// discarding what is already there.
void open_log_early() {
    const std::string root = tsro::ProfileStore::default_root();
    if (root.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    tsro::Logger::instance().configure(tsro::LogLevel::Info, true, root + "/tsro-overlay.log", 1024);
}

/// Other overlays in the same process, named in the log.
///
/// Nothing here conflicts with them any more -- we share no code path with the game's renderer,
/// let alone with theirs -- but when one of them is ReShade the add-on build is still the better
/// answer, because it draws inside the game's own frame and works in exclusive fullscreen.
void report_graphics_mods() {
    static const char* kNames[] = {"dxgi.dll", "d3d11.dll", "d3d12.dll", "opengl32.dll"};
    bool reshade = false;
    for (const char* name : kNames) {
        const HMODULE module = GetModuleHandleA(name);
        if (module == nullptr) continue;
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path))) == 0) continue;
        // Case-insensitively: Windows hands this back as SYSTEM32 as often as System32, and
        // reporting the real dxgi.dll as "another graphics mod" is just noise.
        std::string lower(path);
        for (char& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (lower.find("\\windows\\system32\\") != std::string::npos) continue;
        const std::string full(path);
        TSRO_INFO(kComponent, std::string("another graphics mod is loaded as ") + name + ": " + full);
        if (GetProcAddress(module, "ReShadeRegisterAddon") != nullptr ||
            GetProcAddress(module, "ReShadeRegisterEvent") != nullptr) {
            reshade = true;
        }
    }
    if (reshade) {
        TSRO_INFO(kComponent,
                  "ReShade with add-on support is already running here. The add-on build of this "
                  "overlay draws inside the game's own frame, so it also works in exclusive "
                  "fullscreen: install TeamSpeakOverlay.addon64 beside ReShade if you prefer it.");
    }
}

/// Not on the loader lock: creating a D3D device loads more DLLs, which deadlocks in DllMain.
DWORD WINAPI bootstrap(LPVOID) {
    open_log_early();
    TSRO_INFO(kComponent, std::string("TeamSpeak Overlay .asi ") + TSRO_VERSION + " (build " +
                              TSRO_BUILD_ID + ") loaded");
    report_graphics_mods();

    g_host = new tsro::overlay::OverlayHost("asi-plugin", kComponent);
    // The user's own folder first, then the one shipped beside the .asi, so a font dropped in
    // the config directory wins over one of the same name that came with the release.
    g_host->start({tsro::ProfileStore::default_root() + "/fonts", module_directory() + "/fonts"},
                  module_directory() + "/profiles");

    if (!tsro::asi::start_overlay_window(g_host)) {
        TSRO_ERROR(kComponent, "the overlay's UI thread could not be started");
    }
    return 0;
}

void shutdown(bool process_exiting) {
    g_shutting_down.store(true, std::memory_order_release);
    if (process_exiting) {
        // Other threads are already gone and the OS is about to reclaim everything. Releasing
        // D3D objects here would run driver code on a dying process for no benefit.
        return;
    }
    tsro::asi::stop_overlay_window();
    if (g_host != nullptr) {
        g_host->stop();
        delete g_host;
        g_host = nullptr;
    }
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
