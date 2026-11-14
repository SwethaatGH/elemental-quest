#pragma once

#include "engine/EventManager.hpp"
#include "engine/Systems.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace engine {
// Part 1.1: the engine raises collision, death, spawn and input itself

// Collision events: one per meeting, not one per frame
class CollisionEvents {
public:
    explicit CollisionEvents(EventManager &manager) : manager(manager) {}

    // One frame's contacts from StepPhysics
    size_t Update(const std::vector<Contact> &contacts);
    // A TriggerTracker update: each Enter is a collision with trigger = true
    size_t Update(const std::vector<TriggerEvent> &triggers);
    void Clear() { touching.clear(); }

private:
    EventManager &manager;
    std::set<std::pair<ObjectId, ObjectId>> touching;  // (body, other) as of the last update
};

// Death on entering a death zone, then a spawn after the delay (or when the game calls Spawn())
class RespawnEvents {
public:
    RespawnEvents(EventManager &manager, World &world, ObjectFilter owns = {}, double delaySeconds = 0.0);
    ~RespawnEvents();
    RespawnEvents(const RespawnEvents &) = delete;
    RespawnEvents &operator=(const RespawnEvents &) = delete;

    size_t Update(const ObjectFilter &filter = {});
    // Cancel a pending spawn before a manual reset or object removal
    bool CancelRespawn(ObjectId id);
    bool Dying(ObjectId id) const { return dying.count(id) != 0; }
    World &GetWorld() { return world; }

    void SetAutoSpawn(bool on) { autoSpawn = on; }
    // Raises the spawn for a dead object, to its Respawn's spawn point
    void Spawn(ObjectId id, double delaySeconds = 0.0);

private:
    EventManager &manager;
    World &world;
    ObjectFilter owns;
    double delaySeconds;
    bool autoSpawn = true;
    std::map<ObjectId, uint64_t> pendingSpawns;
    std::map<ObjectId, int> retrySpawns;  // rejected by queue capacity; retry from Update()
    std::set<ObjectId> dying;  // death raised, spawn not handled yet
    EventManager::HandlerId onDeath = 0, onSpawn = 0;
};

// Input events from the bound keys, instead of SDL events
class InputEvents {
public:
    explicit InputEvents(EventManager &manager, std::vector<KeyBinding> bindings = DefaultKeyBindings());

    // Main thread (it reads the keyboard)
    size_t Update(ObjectId target, const std::function<bool(SDL_Scancode)> &isDown = {});

    static void Apply(const Event &input, InputState &state);
    uint32_t Buttons() const { return held; }

private:
    EventManager &manager;
    std::vector<KeyBinding> bindings;
    std::set<SDL_Scancode> down;
    uint32_t held = 0;
};

// Input chords: several keys pressed together count as one input
class ChordEvents {
public:
    explicit ChordEvents(EventManager &manager, double windowSeconds = 0.2);
    ~ChordEvents();
    ChordEvents(const ChordEvents &) = delete;
    ChordEvents &operator=(const ChordEvents &) = delete;

    void Add(const std::string &name, std::vector<SDL_Scancode> keys);

private:
    struct Chord {
        std::string name;
        std::vector<SDL_Scancode> keys;
        bool fired = false;
    };
    void OnInput(const Event &e);

    EventManager &manager;
    double windowSeconds;
    std::vector<Chord> chords;
    std::map<SDL_Scancode, int64_t> downAt;  // keys held, and when each went down
    EventManager::HandlerId handler = 0;
};
}  // namespace engine
