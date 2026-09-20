// SPDX-License-Identifier: MIT
#include "overlay_host.hpp"

#include <filesystem>
#include <system_error>

#include <imgui.h>
// Two hosts, one host object.
//
// Under ReShade this must follow imgui.h: reshade.hpp supplies the inline definitions for the
// ImGui:: and ImDrawList:: members that imgui.h only declares, routing them through ReShade's
// function table. The .asi build owns its own ImGui instead, links the real library, and must
// not see those definitions at all.
#if defined(TSRO_HOST_RESHADE)
#include <reshade.hpp>
#endif

#include "tsro/default_profile.hpp"
#include "tsro/log.hpp"

namespace tsro::overlay {
namespace {

/// What the plugin should bother sending us. A category nobody displays is filtered at the
/// source and never crosses the pipe at all -- which is also what keeps private messages off
/// the wire until the user asks for them.
proto::ConfigurationUpdatedPayload subscription_from(const Config& config) {
    proto::ConfigurationUpdatedPayload payload;
    const ChatConfig& chat = config.chat;
    // A category is asked for when either the feed or a toast wants it. Without this, turning
    // on the private-message toast would light up a setting that could never fire, because the
    // plugin would never have been told to send one.
    payload.chat.channel = (chat.placement.visible && chat.show_channel_messages) ||
                           config.notifications.chat.enabled;
    payload.chat.server = chat.placement.visible && chat.show_server_messages;
    payload.chat.priv = (chat.placement.visible && chat.show_private_messages) ||
                        config.notifications.private_chat.enabled;
    payload.max_chat_length = config.chat.max_message_length;
    payload.want_speaking_events = true;
    return payload;
}

}  // namespace

OverlayHost::OverlayHost(std::string client_name, std::string log_component)
    : client_name_(std::move(client_name)), log_component_(std::move(log_component)) {}

OverlayHost::~OverlayHost() { stop(); }

bool OverlayHost::started() const noexcept { return started_; }

void OverlayHost::apply_logging() {
    std::string path = config_.logging.file_name;
    const std::string root = profiles_ ? profiles_->root() : std::string{};
    if (path.empty() && !root.empty()) path = root + "/tsro-overlay.log";
    Logger::instance().configure(parse_log_level(config_.logging.level), config_.logging.to_file,
                                 path, config_.logging.max_file_kb);
    Logger::instance().set_include_message_content(config_.logging.include_message_content);
}

void OverlayHost::seed_default_profile(const std::string& shipped_profiles_dir) {
    // Only ever on a first run. An existing profile is never read here, let alone rewritten.
    if (profiles_->root().empty() || profiles_->exists("default")) return;

    // A default.json beside the binary wins, so a release or an administrator can ship a house
    // look without a rebuild. Failing that, the copy compiled into the binary is used, which is
    // the path almost everyone takes -- the add-on is copied on its own into a game folder.
    ConfigDiagnostics diag;
    Config seeded;
    std::string source;
    bool ok = false;
    if (!shipped_profiles_dir.empty()) {
        const std::string shipped = shipped_profiles_dir + "/default.json";
        std::error_code ec;
        if (std::filesystem::is_regular_file(shipped, ec)) {
            seeded = profiles_->import_from(shipped, diag, ok);
            source = shipped;
            if (!ok) {
                TSRO_WARN(log_component_,
                          "the default profile beside the binary could not be read: " + shipped);
            }
        }
    }
    if (!ok) {
        // Through the same parse-and-repair path as any other profile, so even a broken
        // embedded copy yields a working configuration rather than an unloadable file.
        json::Limits limits;
        limits.max_total_bytes = 262144;
        const json::ParseResult parsed = json::parse(kDefaultProfileJson, limits);
        if (!parsed.ok) {
            TSRO_WARN(log_component_, "the built-in default profile is unreadable: " + parsed.error);
            return;
        }
        diag = ConfigDiagnostics{};
        seeded = Config::from_json(parsed.value, diag);
        source = "the built-in default";
        ok = true;
    }

    std::string error;
    if (profiles_->save("default", seeded, error)) {
        TSRO_INFO(log_component_, "first run: seeded the default profile from " + source);
    } else {
        TSRO_WARN(log_component_, "the default profile could not be written: " + error);
    }
}

void OverlayHost::load_configuration(const std::string& shipped_profiles_dir) {
    const std::string root = ProfileStore::default_root();
    profiles_ = std::make_unique<ProfileStore>(root);
    std::string error;
    if (!profiles_->ensure_root(error)) {
        TSRO_WARN(log_component_, "configuration directory unavailable: " + error);
    }
    seed_default_profile(shipped_profiles_dir);

    // Per-game profile selection: exact executable match, then the default. One lookup at load,
    // not per frame.
    std::string profile = "default";
    ConfigDiagnostics probe;
    const Config probe_config = profiles_->load("default", probe);
    if (probe_config.general.auto_profile_by_executable) {
        const std::string executable = current_executable_name();
        if (!executable.empty()) profile = profiles_->profile_for_executable(executable);
    }

    config_diagnostics_ = ConfigDiagnostics{};
    config_ = profiles_->load(profile, config_diagnostics_);
    profile_name_ = profile;
    apply_logging();

    TSRO_INFO(log_component_, "loaded profile '" + profile + "'");
    for (const ConfigIssue& issue : config_diagnostics_.issues) {
        if (issue.severity == ConfigIssue::Severity::Info) continue;
        TSRO_WARN(log_component_, "configuration: " + issue.path + " " + issue.message);
    }
}

void OverlayHost::start_client() {
    OverlayClientConfig client_config;
    client_config.endpoint = config_.integration.pipe_name;
    client_config.reconnect_initial_ms = config_.integration.reconnect_initial_ms;
    client_config.reconnect_max_ms = config_.integration.reconnect_max_ms;
    client_config.stale_after_ms = config_.integration.stale_after_ms;
    client_config.ping_interval_ms = config_.integration.ping_interval_ms;
    client_config.chat_history = static_cast<std::size_t>(config_.chat.history_size);
    client_config.subscription = subscription_from(config_);
    client_config.hello.client = client_name_;
    client_config.hello.client_version = TSRO_VERSION;
    client_config.hello.process = current_executable_name();
    client_config.hello.pid = 0;

    client_ = std::make_unique<OverlayClient>(std::move(client_config));
    if (config_.integration.auto_connect) client_->start();
}

void OverlayHost::apply_font() {
    const AppearanceConfig& a = config_.appearance;
    // Weight is applied every time: it is cheap when unchanged and rebakes when it is not.
    fonts_.set_weight(a.font_weight);
    if (a.font_file == applied_font_file_ && a.font_face_index == applied_font_face_) return;
    applied_font_file_ = a.font_file;
    applied_font_face_ = a.font_face_index;
    if (!fonts_.select(a.font_file, a.font_face_index)) {
        TSRO_WARN(log_component_, "font: " + fonts_.error());
    }
}

void OverlayHost::start(const std::vector<std::string>& font_dirs,
                        const std::string& shipped_profiles_dir) {
    if (started_) return;
    load_configuration(shipped_profiles_dir);
    // The config folder is created even when empty so there is somewhere obvious to drop a
    // .ttf before one has been added -- the settings window prints both paths.
    for (const std::string& dir : font_dirs) {
        if (dir.empty()) continue;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
    fonts_.set_directories(font_dirs);
    renderer_.set_font_engine(&fonts_);
    settings_.set_font_engine(&fonts_);
    start_client();
    settings_.refresh_profiles(*profiles_);
    started_ = true;
}

void OverlayHost::stop() {
    if (!started_) return;
    started_ = false;
    // Stop the IPC thread before the state it references goes away.
    if (client_) client_->stop();
    renderer_.set_font_engine(nullptr);
    settings_.set_font_engine(nullptr);
    fonts_.release();
}

void OverlayHost::release_font_textures() { fonts_.release(); }

std::string OverlayHost::menu_key_name() {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_.general.menu_key;
}

void OverlayHost::draw_hud(ImDrawList* draw_list, const Viewport& viewport, FontTextureSink* sink,
                           std::int64_t now) {
    if (!started_ || draw_list == nullptr) return;
    if (viewport.width < 1.0f || viewport.height < 1.0f) return;

    const OverlayFrame& frame = client_->latest();

    std::lock_guard<std::mutex> lock(config_mutex_);

    // Font upkeep happens here, before anything is drawn: begin_frame bakes at most one atlas,
    // so switching typeface or size costs a hitch rather than a stall mid-draw-list.
    apply_font();
    fonts_.begin_frame(sink);

    const std::vector<OverlayEvent> events = client_->drain_events();
    renderer_.submit_events(events, config_, now);

    const bool preview = settings_.preview_active();
    if (preview) {
        if (preview_.users.empty()) {
            preview_ = preview_state();
            preview_chat_ = preview_chat();
        }
        // Rebuild on entry and whenever the configuration changed, so edits are reflected.
        preview_config_ = config_;
        preview_config_.chat.placement.visible = true;
        // Seed on entry, and again once the samples have aged out, so the preview keeps showing
        // every notification type instead of emptying after a few seconds.
        if (!preview_was_active_ || renderer_.notifications_empty()) {
            renderer_.seed_preview_notifications(preview_config_, now);
        }
    } else if (preview_was_active_) {
        // Leaving preview must not leave sample notifications on screen.
        renderer_.clear_notifications();
        renderer_.note_connected(now);
    }
    preview_was_active_ = preview;

    renderer_.draw(draw_list, preview ? preview_config_ : config_, frame, viewport, now,
                   preview ? &preview_ : nullptr, preview ? &preview_chat_ : nullptr);
}

void OverlayHost::draw_settings() {
    if (!started_) return;

    const OverlayFrame& frame = client_->latest();
    const LinkDiagnostics diagnostics = client_->diagnostics();

    std::lock_guard<std::mutex> lock(config_mutex_);
    const SettingsActions actions = settings_.draw(config_, diagnostics, frame, *profiles_,
                                                   renderer_.stats(), config_diagnostics_);

    if (actions.config_changed) {
        config_.clamp(config_diagnostics_);
        renderer_.invalidate();
        apply_logging();
        client_->set_stale_after_ms(config_.integration.stale_after_ms);
    }
    if (actions.subscription_changed) client_->update_subscription(subscription_from(config_));
    if (actions.reconnect_requested) client_->request_reconnect();

    if (actions.save_requested) {
        std::string error;
        if (profiles_->save(profile_name_, config_, error)) {
            settings_.set_status("Saved.", false);
            settings_.refresh_profiles(*profiles_);
        } else {
            settings_.set_status(error, true);
        }
    }
    if (actions.reload_requested) {
        config_diagnostics_ = ConfigDiagnostics{};
        config_ = profiles_->load(profile_name_, config_diagnostics_);
        renderer_.invalidate();
        client_->update_subscription(subscription_from(config_));
        settings_.set_status("Reloaded from disk.", false);
    }
    if (!actions.switch_to_profile.empty()) {
        config_diagnostics_ = ConfigDiagnostics{};
        config_ = profiles_->load(actions.switch_to_profile, config_diagnostics_);
        profile_name_ = actions.switch_to_profile;
        renderer_.invalidate();
        renderer_.clear_notifications();
        client_->update_subscription(subscription_from(config_));
        apply_logging();
        settings_.set_status("Loaded profile '" + actions.switch_to_profile + "'.", false);
    }
}

}  // namespace tsro::overlay
