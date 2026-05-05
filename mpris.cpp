// mpris.cpp — MPRIS2 D-Bus service implementation
//
// Written for sdbus-c++ v2.x (Arch Linux ships 2.2.1+).
// The v1 API used obj_->registerProperty(iface, name).withGetter() and
// conn_->processPendingRequest() in a manual loop; v2 uses addVTable() with
// strong typedefs (sdbus::PropertyName, sdbus::InterfaceName etc.) and
// conn_->enterEventLoopAsync() for non-blocking background dispatch.
//
// MPRIS spec: https://specifications.freedesktop.org/mpris-spec/latest/
//
// Implements:
//   org.mpris.MediaPlayer2         — CanQuit, CanRaise, Identity, DesktopEntry
//   org.mpris.MediaPlayer2.Player  — PlaybackStatus, Metadata, Volume,
//                                    Position, Play, Pause, PlayPause,
//                                    Stop, Next, Previous, Seek, SetPosition,
//                                    Seeked (signal)

#include "mpris.h"
#include "types.h"
#include "plex_client.h"
#include "player_view.h"

#include <sdbus-c++/sdbus-c++.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <map>

namespace PlexTUI {

// ── MPRIS constants ────────────────────────────────────────────────────────

static constexpr char MPRIS_BUS_NAME[]     = "org.mpris.MediaPlayer2.plex-tui";
static constexpr char MPRIS_OBJECT_PATH[]  = "/org/mpris/MediaPlayer2";
static constexpr char MPRIS_ROOT_IFACE[]   = "org.mpris.MediaPlayer2";
static constexpr char MPRIS_PLAYER_IFACE[] = "org.mpris.MediaPlayer2.Player";

// ── Construction / destruction ─────────────────────────────────────────────

MprisServer::MprisServer(PlexClient& client, PlayerView& view)
    : client_(client), view_(view)
{}

MprisServer::~MprisServer() {
    stop();
}

// ── start / stop ───────────────────────────────────────────────────────────

bool MprisServer::start() {
    if (running_.load()) return true;

    try {
        // Connect to the session bus and claim our well-known name.
        // sdbus-c++ v2: ServiceName is a strong typedef, not a plain string.
        conn_ = sdbus::createSessionBusConnection(sdbus::ServiceName{MPRIS_BUS_NAME});

        // Create the /org/mpris/MediaPlayer2 object.
        obj_ = sdbus::createObject(*conn_, sdbus::ObjectPath{MPRIS_OBJECT_PATH});

        setup_interfaces();

        running_.store(true);

        // enterEventLoopAsync() starts the sdbus event loop on an internal
        // thread owned by the connection; we don't need to manage it ourselves.
        conn_->enterEventLoopAsync();

        // Separate polling thread — pushes PropertiesChanged every POLL_MS.
        thread_ = std::thread([this]() {
            while (running_.load()) {
                poll_and_notify();
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
            }
        });

        return true;

    } catch (const sdbus::Error& e) {
        std::cerr << "[mpris] Failed to start D-Bus service: "
                  << e.getMessage() << "\n";
        running_.store(false);
        return false;
    }
}

void MprisServer::stop() {
    if (!running_.load()) return;
    running_.store(false);

    if (conn_) {
        try { conn_->leaveEventLoop(); } catch (...) {}
    }

    if (thread_.joinable()) thread_.join();

    obj_.reset();
    conn_.reset();
}

// ── Interface setup ────────────────────────────────────────────────────────

void MprisServer::setup_interfaces() {
    register_root_interface();
    register_player_interface();
}

// ── org.mpris.MediaPlayer2 (root) ──────────────────────────────────────────
//
// sdbus-c++ v2: All properties and methods for an interface go into a single
// addVTable(...).forInterface(name) call.

void MprisServer::register_root_interface() {
    obj_->addVTable(
        sdbus::registerProperty(sdbus::PropertyName{"CanQuit"})
            .withGetter([]() -> bool { return true; }),

        sdbus::registerProperty(sdbus::PropertyName{"CanRaise"})
            .withGetter([]() -> bool { return false; }),

        sdbus::registerProperty(sdbus::PropertyName{"HasTrackList"})
            .withGetter([]() -> bool { return false; }),

        sdbus::registerProperty(sdbus::PropertyName{"Identity"})
            .withGetter([]() -> std::string { return "plex-tui"; }),

        sdbus::registerProperty(sdbus::PropertyName{"DesktopEntry"})
            .withGetter([]() -> std::string { return "plex-tui"; }),

        sdbus::registerProperty(sdbus::PropertyName{"SupportedUriSchemes"})
            .withGetter([]() -> std::vector<std::string> { return {}; }),

        sdbus::registerProperty(sdbus::PropertyName{"SupportedMimeTypes"})
            .withGetter([]() -> std::vector<std::string> { return {}; }),

        sdbus::registerMethod(sdbus::MethodName{"Raise"})
            .implementedAs([]() {}),   // no-op: TUI cannot be raised

        sdbus::registerMethod(sdbus::MethodName{"Quit"})
            .implementedAs([this]() { view_.request_quit(); })

    ).forInterface(sdbus::InterfaceName{MPRIS_ROOT_IFACE});
}

// ── org.mpris.MediaPlayer2.Player ─────────────────────────────────────────

void MprisServer::register_player_interface() {
    obj_->addVTable(
        // ── Capability properties ────────────────────────────────────────
        sdbus::registerProperty(sdbus::PropertyName{"CanPlay"})
            .withGetter([]() -> bool { return true; }),
        sdbus::registerProperty(sdbus::PropertyName{"CanPause"})
            .withGetter([]() -> bool { return true; }),
        sdbus::registerProperty(sdbus::PropertyName{"CanGoNext"})
            .withGetter([]() -> bool { return true; }),
        sdbus::registerProperty(sdbus::PropertyName{"CanGoPrevious"})
            .withGetter([]() -> bool { return true; }),
        sdbus::registerProperty(sdbus::PropertyName{"CanSeek"})
            .withGetter([]() -> bool { return true; }),
        sdbus::registerProperty(sdbus::PropertyName{"CanControl"})
            .withGetter([]() -> bool { return true; }),

        // ── Live state properties ────────────────────────────────────────
        sdbus::registerProperty(sdbus::PropertyName{"PlaybackStatus"})
            .withGetter([this]() -> std::string {
                return playback_status(client_.get_playback_state());
            }),

        sdbus::registerProperty(sdbus::PropertyName{"LoopStatus"})
            .withGetter([]() -> std::string { return "None"; }),

        sdbus::registerProperty(sdbus::PropertyName{"Shuffle"})
            .withGetter([]() -> bool { return false; }),

        // Volume is read-write: withGetter + withSetter makes it writable
        sdbus::registerProperty(sdbus::PropertyName{"Volume"})
            .withGetter([this]() -> double {
                return static_cast<double>(client_.get_volume());
            })
            .withSetter([this](const double& v) {
                client_.set_volume(static_cast<float>(v));
            }),

        // Position in microseconds (MPRIS uses µs; plex-tui uses ms)
        sdbus::registerProperty(sdbus::PropertyName{"Position"})
            .withGetter([this]() -> int64_t {
                return static_cast<int64_t>(client_.get_position_ms()) * int64_t{1000};
            }),

        sdbus::registerProperty(sdbus::PropertyName{"MinimumRate"})
            .withGetter([]() -> double { return 1.0; }),
        sdbus::registerProperty(sdbus::PropertyName{"MaximumRate"})
            .withGetter([]() -> double { return 1.0; }),
        sdbus::registerProperty(sdbus::PropertyName{"Rate"})
            .withGetter([]() -> double { return 1.0; }),

        sdbus::registerProperty(sdbus::PropertyName{"Metadata"})
            .withGetter([this]() -> std::map<std::string, sdbus::Variant> {
                return build_metadata(client_.get_playback_state().current_track);
            }),

        // ── Playback control methods ─────────────────────────────────────
        sdbus::registerMethod(sdbus::MethodName{"Play"})
            .implementedAs([this]() { client_.resume(); }),

        sdbus::registerMethod(sdbus::MethodName{"Pause"})
            .implementedAs([this]() { client_.pause(); }),

        sdbus::registerMethod(sdbus::MethodName{"PlayPause"})
            .implementedAs([this]() {
                auto state = client_.get_playback_state();
                if (state.paused || !state.playing) client_.resume();
                else                                client_.pause();
            }),

        sdbus::registerMethod(sdbus::MethodName{"Stop"})
            .implementedAs([this]() { client_.stop(); }),

        // Next / Previous route through PlayerView (navigation lives there)
        sdbus::registerMethod(sdbus::MethodName{"Next"})
            .implementedAs([this]() { view_.mpris_next(); }),

        sdbus::registerMethod(sdbus::MethodName{"Previous"})
            .implementedAs([this]() { view_.mpris_previous(); }),

        // Seek: MPRIS offset in µs (negative = rewind)
        sdbus::registerMethod(sdbus::MethodName{"Seek"})
            .implementedAs([this](int64_t offset_us) {
                uint32_t current_ms = client_.get_position_ms();
                int64_t  new_ms     = static_cast<int64_t>(current_ms)
                                      + offset_us / int64_t{1000};
                if (new_ms < 0) new_ms = 0;
                client_.seek(static_cast<uint32_t>(new_ms));
            }),

        // SetPosition: absolute position in µs
        sdbus::registerMethod(sdbus::MethodName{"SetPosition"})
            .implementedAs([this](const sdbus::ObjectPath& /*trackId*/,
                                  int64_t position_us) {
                if (position_us < 0) position_us = 0;
                client_.seek(static_cast<uint32_t>(position_us / int64_t{1000}));
            }),

        sdbus::registerMethod(sdbus::MethodName{"OpenUri"})
            .implementedAs([](const std::string& /*uri*/) {}),

        // Seeked signal — must be declared in the vtable for sdbus-c++ v2
        sdbus::registerSignal(sdbus::SignalName{"Seeked"})
            .withParameters<int64_t>()

    ).forInterface(sdbus::InterfaceName{MPRIS_PLAYER_IFACE});
}

// ── Poll loop — emits PropertiesChanged when state changes ─────────────────
//
// Runs on our private thread_; signal emission in sdbus-c++ v2 is thread-safe.
// emitPropertiesChangedSignal() calls each property's getter to build the
// changed-values dict, then sends org.freedesktop.DBus.Properties.PropertiesChanged.

void MprisServer::poll_and_notify() {
    if (!obj_ || !running_.load()) return;

    auto state = client_.get_playback_state();

    bool track_changed  = (state.current_track.id != last_track_id_);
    bool status_changed = (state.playing != last_state_.playing ||
                           state.paused  != last_state_.paused);
    bool volume_changed = (state.volume  != last_state_.volume);

    if (!track_changed && !status_changed && !volume_changed) return;

    // Build the list of changed property names; sdbus-c++ v2 will call each
    // getter and bundle the results into the PropertiesChanged signal.
    std::vector<sdbus::PropertyName> changed_props;

    if (status_changed || track_changed)
        changed_props.emplace_back("PlaybackStatus");
    if (track_changed)
        changed_props.emplace_back("Metadata");
    if (volume_changed)
        changed_props.emplace_back("Volume");
    if (track_changed)
        changed_props.emplace_back("Position");

    try {
        obj_->emitPropertiesChangedSignal(
            sdbus::InterfaceName{MPRIS_PLAYER_IFACE},
            changed_props
        );
    } catch (const sdbus::Error&) {
        // Non-fatal — consumer may have disconnected
    }

    // Also emit the MPRIS Seeked signal on track change (position resets to 0)
    if (track_changed) {
        try {
            obj_->emitSignal(sdbus::SignalName{"Seeked"})
                .onInterface(sdbus::InterfaceName{MPRIS_PLAYER_IFACE})
                .withArguments(int64_t{0});
        } catch (...) {}
    }

    last_state_    = state;
    last_track_id_ = state.current_track.id;
}

// ── Helpers ────────────────────────────────────────────────────────────────

std::string MprisServer::playback_status(const PlaybackState& s) const {
    if (!s.playing) return "Stopped";
    if (s.paused)   return "Paused";
    return "Playing";
}

std::map<std::string, sdbus::Variant>
MprisServer::build_metadata(const Track& t) const {
    std::map<std::string, sdbus::Variant> meta;

    // mpris:trackid must be a D-Bus object path
    std::string track_path = "/org/mpris/MediaPlayer2/Track/";
    track_path += t.id.empty() ? "0" : t.id;
    meta["mpris:trackid"] = sdbus::Variant(sdbus::ObjectPath(track_path));

    // Duration in microseconds
    meta["mpris:length"] = sdbus::Variant(
        static_cast<int64_t>(t.duration_ms) * int64_t{1000});

    // Album art URI.
    // plex_client.cpp only prepends server_url to art paths that don't start
    // with '/', but the Plex API always returns relative paths beginning with
    // '/' (e.g. /library/metadata/.../thumb/...).  We must complete them here,
    // adding the auth token so Quickshell / playerctl can actually fetch it.
    std::string art = t.get_art_url();
    if (!art.empty()) {
        if (art[0] == '/') {
            std::string server = client_.get_server_url();
            std::string tok    = client_.get_token();
            // Strip any trailing slash from the server URL to avoid double //
            if (!server.empty() && server.back() == '/')
                server.pop_back();
            art = server + art;
            if (!tok.empty())
                art += "?X-Plex-Token=" + tok;
        }
        meta["mpris:artUrl"] = sdbus::Variant(art);
    }

    meta["xesam:title"] = sdbus::Variant(
        t.title.empty()  ? std::string("Unknown") : t.title);
    meta["xesam:album"] = sdbus::Variant(
        t.album.empty()  ? std::string("Unknown") : t.album);
    meta["xesam:artist"] = sdbus::Variant(
        std::vector<std::string>{ t.artist.empty() ? "Unknown" : t.artist });

    if (t.year > 0)
        meta["xesam:contentCreated"] = sdbus::Variant(std::to_string(t.year));

    if (!t.genre.empty())
        meta["xesam:genre"] = sdbus::Variant(
            std::vector<std::string>{ t.genre });

    return meta;
}

} // namespace PlexTUI
