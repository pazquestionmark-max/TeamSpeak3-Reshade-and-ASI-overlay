// SPDX-License-Identifier: MIT
// Everything the overlay does that is not drawing or hooking.
//
// The overlay ships in two forms -- a ReShade add-on and a standalone .asi plugin -- and the
// only real difference between them is how they get a Dear ImGui frame and a graphics device.
// Profiles, the IPC client, the font engine, the preview mode and the settings actions are
// identical, so they live here and each host owns nothing but its entry point, its hook and its
// texture sink.
//
// Nothing in here blocks the render thread: the IPC connection lives on OverlayClient's own
// thread and the per-frame calls only read a triple-buffered frame.
#ifndef TSRO_OVERLAY_HOST_HPP
#define TSRO_OVERLAY_HOST_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "font_engine.hpp"
#include "renderer.hpp"
#include "settings_ui.hpp"
#include "tsro/config.hpp"
#include "tsro/overlay_client.hpp"
#include "tsro/profile_store.hpp"

struct ImDrawList;

namespace tsro::overlay {

class OverlayHost {
public:
    /// `client_name` is what the plugin sees in the handshake ("reshade-addon", "asi-plugin");
    /// `log_component` is the tag on this host's log lines.
    OverlayHost(std::string client_name, std::string log_component);
    ~OverlayHost();
    OverlayHost(const OverlayHost&) = delete;
    OverlayHost& operator=(const OverlayHost&) = delete;

    /// Loads the profile, opens the IPC client and points the font engine at `font_dirs`
    /// (user folder first, then the one shipped beside the binary). Safe to call once.
    ///
    /// `shipped_profiles_dir`, when it exists and the user has no `default` profile yet, seeds
    /// one from the `default.json` in it -- so a first run looks like the release's own
    /// screenshots instead of like the compiled-in fallback. An existing profile is never
    /// touched, so this cannot overwrite anything the user has set.
    void start(const std::vector<std::string>& font_dirs,
               const std::string& shipped_profiles_dir = {});
    /// Stops the IPC thread and releases every font texture. Idempotent.
    void stop();
    bool started() const noexcept;

    /// One frame of HUD, drawn into `draw_list`. `sink` is the host's texture uploader, which
    /// may change when the graphics device is recreated -- the font engine notices by pointer.
    void draw_hud(ImDrawList* draw_list, const Viewport& viewport, FontTextureSink* sink,
                  std::int64_t now_ms);
    /// The settings window's contents. The caller owns the window (ReShade's overlay, or an
    /// ImGui::Begin of its own) and any actions are applied here before this returns.
    void draw_settings();

    /// Releases font textures without stopping the IPC client, for a device reset.
    void release_font_textures();

    /// The configured name of the key that opens the settings window, e.g. "INSERT". Only the
    /// .asi host has a use for it; under ReShade the settings live inside ReShade's own menu.
    std::string menu_key_name();

    FontEngine& fonts() noexcept { return fonts_; }
    SettingsUi& settings() noexcept { return settings_; }
    const std::string& profile_name() const noexcept { return profile_name_; }

private:
    void load_configuration(const std::string& shipped_profiles_dir);
    void seed_default_profile(const std::string& shipped_profiles_dir);
    void start_client();
    void apply_font();
    void apply_logging();

    std::string client_name_;
    std::string log_component_;

    Config config_ = Config::defaults();
    ConfigDiagnostics config_diagnostics_;
    std::unique_ptr<ProfileStore> profiles_;
    std::unique_ptr<OverlayClient> client_;
    Renderer renderer_;
    SettingsUi settings_;
    /// The overlay's own typeface, independent of whatever the host draws its own UI with.
    FontEngine fonts_;
    std::string applied_font_file_;
    int applied_font_face_ = -1;

    /// Guards `config_` only. The HUD reads it and the settings window writes it; under both
    /// hosts they run on the same thread, so this is uncontended and exists to make the
    /// ordering explicit rather than assumed.
    std::mutex config_mutex_;

    OverlayState preview_;
    std::vector<ChatMessage> preview_chat_;
    /// A copy of `config_` with the chat feed forced visible, used only while previewing: the
    /// point is to show where every piece sits, and a hidden chat panel shows nothing.
    Config preview_config_;
    bool preview_was_active_ = false;
    bool started_ = false;
    std::string profile_name_ = "default";
};

}  // namespace tsro::overlay

#endif  // TSRO_OVERLAY_HOST_HPP
