#pragma once
// mpris.h — MPRIS2 D-Bus service for plex-audio-btop-tui
//
// Registers org.mpris.MediaPlayer2 and org.mpris.MediaPlayer2.Player on the
// D-Bus session bus so that Quickshell (and any other MPRIS consumer like
// playerctl, waybar, etc.) can see and control plex-tui.
//
// ARCHITECTURE:
//   MprisServer runs on its own thread, blocking on sdbus event loop.
//   It polls PlaybackState from PlexClient every ~500ms to push
//   PropertiesChanged signals (so the bar media panel updates live).
//   Commands from D-Bus (Play, Pause, Next, Previous, Seek) are forwarded
//   to PlayerView via thread-safe callbacks set at construction time.
//
// THREAD SAFETY:
//   All callbacks are called from the sdbus event thread.
//   They must be safe to call from a thread other than the main loop.
//   PlexClient::pause()/resume()/seek() use internal mutexes — safe.
//   PlayerView callbacks use std::atomic or post to a command queue — see
//   the player_view.h additions in player_view_patch.h.
//
// DEPENDENCY:
//   sdbus-c++ >= 2.0  (pacman -S sdbus-cpp)
//   Links with: -lsdbus-c++
//
// USAGE (in main.cpp):
//   auto mpris = std::make_unique<PlexTUI::MprisServer>(client, *player_view);
//   mpris->start();
//   // ... main loop ...
//   mpris->stop();

#include "types.h"
#include <sdbus-c++/sdbus-c++.h>
#include <thread>
#include <atomic>
#include <map>
#include <memory>
#include <string>

namespace PlexTUI {

// Forward declarations
class PlexClient;
class PlayerView;

class MprisServer {
public:
    // client    — used to poll playback state and call pause/resume/seek
    // view      — used to fire next/prev (which live in PlayerView navigation)
    MprisServer(PlexClient& client, PlayerView& view);
    ~MprisServer();

    // Start the D-Bus service on a background thread.
    // Returns false if the session bus is not available.
    bool start();

    // Stop the service and join the background thread.
    void stop();

    // Returns true while the service is running
    bool is_running() const { return running_.load(); }

private:
    // ── D-Bus object setup ─────────────────────────────────────────────────
    void setup_interfaces();

    // org.mpris.MediaPlayer2 (root interface — identity / quit / raise)
    void register_root_interface();

    // org.mpris.MediaPlayer2.Player (playback control + metadata)
    void register_player_interface();

    // ── Property builders ─────────────────────────────────────────────────
    // Builds the MPRIS Metadata dict from a Track
    std::map<std::string, sdbus::Variant> build_metadata(const Track& t) const;

    // Returns "Playing", "Paused", or "Stopped"
    std::string playback_status(const PlaybackState& s) const;

    // ── Poll loop ─────────────────────────────────────────────────────────
    // Runs on the sdbus event thread after setup; periodically pushes
    // PropertiesChanged signals when state changes.
    void poll_and_notify();

    // ── Members ───────────────────────────────────────────────────────────
    PlexClient&  client_;
    PlayerView&  view_;

    std::unique_ptr<sdbus::IConnection> conn_;
    std::unique_ptr<sdbus::IObject>     obj_;

    std::thread             thread_;
    std::atomic<bool>       running_{false};

    // Last-seen state — used to detect changes and emit PropertiesChanged
    PlaybackState           last_state_;
    std::string             last_track_id_;

    // Poll interval for state updates (ms)
    static constexpr int POLL_MS = 500;
};

} // namespace PlexTUI
