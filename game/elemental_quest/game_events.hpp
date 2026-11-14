#pragma once

// Event names, shared types, tuning and key bindings for client, server and checks

#include "level.hpp"

#include "engine/Event.hpp"
#include "engine/EventSystems.hpp"

#include <vector>

namespace ed {
namespace ev {
// client -> server (validated by the server)
constexpr const char *BURST_REQUEST = "burst_request";  // element
constexpr const char *STOMP_REQUEST = "stomp_request";  // element, enemy (object)
constexpr const char *DEATH_REPORT = "death_report";  // element, cause, checkpoint
constexpr const char *TEMPLE_REQUEST = "temple_request";  // element

// server -> every client
constexpr const char *BURST = "burst";  // element, id, center, radius, target, players, count
constexpr const char *BURST_END = "burst_end";  // element, id, target  (raised 4 s after BURST)
constexpr const char *ENEMY_DEFEATED = "enemy_defeated";  // index, by, how ("stomp" | "fire")
constexpr const char *TEMPLE_REACHED = "temple_reached";  // element, inside, active
constexpr const char *GATE_OPENED = "gate_opened";  // by
constexpr const char *GATE_CLOSED = "gate_closed";
// engine::events::SPAWN is shared too: kind "enemy" (index, by) or "gem" (element, at)

// local only
constexpr const char *SURFACE = "surface";  // style: the player landed on a new kind of floor
constexpr const char *GATE_TIMER = "gate_timer";  // server: 2.5 s after the last plate was released
}  // namespace ev

inline std::vector<engine::EventType> ClientSharedTypes() {
    return {ev::BURST_REQUEST, ev::STOMP_REQUEST, ev::DEATH_REPORT, ev::TEMPLE_REQUEST};
}
inline std::vector<engine::EventType> ServerSharedTypes() {
    return {ev::BURST, ev::BURST_END, ev::ENEMY_DEFEATED, ev::TEMPLE_REACHED,
            ev::GATE_OPENED, ev::GATE_CLOSED, engine::events::SPAWN};
}

// Tuning (seconds and world pixels)
constexpr double kRespawnDelay = 1.0;  // death -> spawn
constexpr double kBurstSeconds = 4.0;  // burst -> burst_end
constexpr double kBurstCooldown = 8.0;  // per player, enforced by the server
constexpr float kBurstRadius = 350.0f;  // FIRE (enemies) and AIR (players)
constexpr double kChordWindow = 0.2;  // keys of a chord must go down within this
constexpr double kDashSeconds = 0.18;
constexpr float kDashSpeed = 1300.0f;  // left / right: a straight air dash
constexpr double kDashCooldown = 0.6;
constexpr double kReplaySeconds = 3.0;  // played back on death
constexpr double kReplaySpeed = 0.5;
constexpr double kReplayKeepSeconds = 5.0;

// Extra game buttons (engine buttons stop at 32)
constexpr uint32_t kButtonBurstJ = 64u, kButtonBurstK = 128u, kButtonDashM = 256u;

inline std::vector<engine::KeyBinding> GameKeyBindings() {
    std::vector<engine::KeyBinding> b = engine::DefaultKeyBindings();
    b.push_back({SDL_SCANCODE_J, kButtonBurstJ});
    b.push_back({SDL_SCANCODE_K, kButtonBurstK});
    b.push_back({SDL_SCANCODE_M, kButtonDashM});
    return b;
}

// M + Left/Right dashes, J + K bursts; Space stays a plain jump
inline void AddGameChords(engine::ChordEvents &chords) {
    chords.Add("dash_left", {SDL_SCANCODE_M, SDL_SCANCODE_LEFT});
    chords.Add("dash_right", {SDL_SCANCODE_M, SDL_SCANCODE_RIGHT});
    chords.Add("burst", {SDL_SCANCODE_J, SDL_SCANCODE_K});
}

inline const char *BurstEffect(int element) {
    switch (element) {
    case Fire: return "defeats every enemy near FIRE";
    case Water: return "freezes the next moving platform";
    case Earth: return "raises a stone bridge over the next goo";
    case Air: return "freezes every player near AIR";
    }
    return "?";
}
}  // namespace ed
