// SPDX-License-Identifier: MIT
// ReShade add-on entry point (Component A).
//
// Two registrations, each chosen for when ReShade invokes it (verified in ReShade's source, see
// docs/architecture.md §1.2):
//
//   * addon_event::reshade_overlay  -- called every frame, between ImGui::NewFrame and
//     ImGui::EndFrame. ReShade's early-out explicitly keeps building an ImGui frame when an
//     add-on has subscribed to this event, which is what makes an always-on HUD possible.
//     We draw into the background draw list, so we add no draw call batch of our own and take
//     no input.
//
//   * register_overlay("...") -- called only while ReShade's menu is open. That is exactly when
//     the user is configuring and when ReShade is already blocking game input, so it is the
//     right home for the settings window.
//
// Everything above the hook -- profiles, the IPC client, the font engine, the renderer -- lives
// in OverlayHost and is shared verbatim with the .asi build. This file is the ReShade half:
// the entry point, the two callbacks, and the texture sink that puts a font atlas on the GPU
// through ReShade's device API.
//
// Nothing here blocks: the IPC connection lives on OverlayClient's own thread and the render
// callback only reads a triple-buffered frame.
#include <chrono>
#include <cstdint>
#include <map>
#include <string>

#include <imgui.h>
// Order matters: reshade.hpp defines the ImGui:: and ImDrawList:: members that imgui.h only
// declares, routing them through ReShade's function table.
#include <reshade.hpp>

#include "font_engine.hpp"
#include "overlay_host.hpp"
#include "tsro/log.hpp"

extern "C" __declspec(dllexport) const char* NAME = "TeamSpeak Overlay";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Shows your current TeamSpeak channel, who is in it and their voice states, fed by the "
    "TeamSpeak ReShade Overlay plugin over a local named pipe. Opens no network connection.";

namespace {

constexpr char kComponent[] = "addon";

/// The ReShade half of FontEngine's texture upload.
///
/// The font engine hands out one handle per atlas and gets one back to destroy, but ReShade's
/// device API needs a resource *and* a view, and only the view's handle is an ImTextureID. So
/// the pairing is kept here rather than leaking two handles into code the .asi build shares.
class ReShadeFontSink final : public tsro::overlay::FontTextureSink {
public:
    /// The device is set per frame: ReShade hands one to the overlay callback, and a device
    /// reset gives a different pointer, at which point every texture we hold is already gone.
    void set_device(reshade::api::device* device) {
        if (device == device_) return;
        // Do not destroy through the old device -- it may no longer exist. The resources went
        // with it; the font engine is told by the same pointer change and drops its atlases.
        pairs_.clear();
        device_ = device;
    }

    std::uint64_t create(const unsigned char* rgba, int width, int height) override {
        if (device_ == nullptr || rgba == nullptr || width <= 0 || height <= 0) return 0;

        const reshade::api::resource_desc desc(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1, 1,
            reshade::api::format::r8g8b8a8_unorm, 1, reshade::api::memory_heap::gpu_only,
            reshade::api::resource_usage::shader_resource);
        reshade::api::subresource_data initial{};
        // subresource_data::data is void*, but ReShade only reads the initial contents.
        initial.data = const_cast<unsigned char*>(rgba);
        initial.row_pitch = static_cast<std::uint32_t>(width) * 4u;
        initial.slice_pitch = initial.row_pitch * static_cast<std::uint32_t>(height);

        reshade::api::resource texture{};
        if (!device_->create_resource(desc, &initial,
                                      reshade::api::resource_usage::shader_resource, &texture)) {
            return 0;
        }
        reshade::api::resource_view view{};
        if (!device_->create_resource_view(
                texture, reshade::api::resource_usage::shader_resource,
                reshade::api::resource_view_desc(reshade::api::format::r8g8b8a8_unorm, 0, 1, 0, 1),
                &view)) {
            device_->destroy_resource(texture);
            return 0;
        }
        pairs_[view.handle] = texture;
        return view.handle;
    }

    void destroy(std::uint64_t handle) override {
        const auto it = pairs_.find(handle);
        if (it == pairs_.end()) return;
        if (device_ != nullptr) {
            device_->destroy_resource_view(reshade::api::resource_view{handle});
            device_->destroy_resource(it->second);
        }
        pairs_.erase(it);
    }

private:
    reshade::api::device* device_ = nullptr;
    std::map<std::uint64_t, reshade::api::resource> pairs_;
};

tsro::overlay::OverlayHost* g_host = nullptr;
ReShadeFontSink* g_sink = nullptr;
HMODULE g_module = nullptr;

/// The folder the add-on DLL was loaded from, which is where its shipped `fonts` folder sits.
std::string module_directory() {
    if (g_module == nullptr) return {};
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(g_module, path, static_cast<DWORD>(sizeof(path)));
    if (n == 0 || n >= sizeof(path)) return {};
    std::string s(path, n);
    const std::size_t cut = s.find_last_of("\\/");
    return cut == std::string::npos ? std::string{} : s.substr(0, cut);
}

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// Called every frame by ReShade, inside its ImGui frame.
void on_reshade_overlay(reshade::api::effect_runtime* runtime) {
    if (g_host == nullptr || !g_host->started() || runtime == nullptr) return;

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    if (draw_list == nullptr) return;

    // The viewport comes from ReShade's own API rather than ImGuiIO::DisplaySize.
    //
    // Reading a field off ImGuiIO means trusting that this add-on's imgui.h lays the struct out
    // exactly as ReShade's ImGui build does. That assumption is what crashed the game when the
    // font list walked ImFontAtlas, and it is the same assumption here.
    // get_screenshot_width_and_height is a virtual call across a versioned interface, so there
    // is no layout to guess at.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    tsro::Viewport viewport;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);

    g_sink->set_device(runtime->get_device());
    g_host->draw_hud(draw_list, viewport, g_sink, now_ms());
}

/// Called by ReShade only while its menu is open.
void on_settings_overlay(reshade::api::effect_runtime* runtime) {
    (void)runtime;
    if (g_host == nullptr || !g_host->started()) return;
    g_host->draw_settings();
}

bool initialise(HMODULE module) {
    g_module = module;
    // register_addon also resolves ReShade's ImGui function table, and fails if this add-on was
    // built against an ImGui version ReShade does not export. Failing here rather than drawing
    // through a null table is what turns a version mismatch into "the add-on did not load"
    // instead of a crash inside the game.
    if (!reshade::register_addon(module)) return false;

    g_sink = new ReShadeFontSink();
    g_host = new tsro::overlay::OverlayHost("reshade-addon", kComponent);
    // The user's own folder first, then the one shipped beside the add-on, so a font dropped in
    // the config directory wins over one of the same name that came with the release.
    g_host->start({tsro::ProfileStore::default_root() + "/fonts", module_directory() + "/fonts"},
                  module_directory() + "/profiles");

    reshade::register_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay);
    reshade::register_overlay("TeamSpeak Overlay", &on_settings_overlay);
    TSRO_INFO(kComponent, "add-on initialised");
    return true;
}

void shutdown(HMODULE module) {
    if (g_host != nullptr) {
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(&on_reshade_overlay);
        reshade::unregister_overlay("TeamSpeak Overlay", &on_settings_overlay);
        delete g_host;
        g_host = nullptr;
        delete g_sink;
        g_sink = nullptr;
    }
    reshade::unregister_addon(module);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // No thread is created here: OverlayClient starts its own, which DllMain is allowed
            // to request because CreateThread does not take the loader lock's critical path for
            // work we do not wait on.
            DisableThreadLibraryCalls(module);
            if (!initialise(module)) return FALSE;
            break;
        case DLL_PROCESS_DETACH:
            shutdown(module);
            break;
        default:
            break;
    }
    return TRUE;
}
