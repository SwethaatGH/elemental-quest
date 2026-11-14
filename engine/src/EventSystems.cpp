#include "engine/EventSystems.hpp"

#include "engine/Input.hpp"

#include <algorithm>

namespace engine {
// --- collisions --------------------------------------------------------------

size_t CollisionEvents::Update(const std::vector<Contact> &contacts) {
    std::set<std::pair<ObjectId, ObjectId>> now;
    size_t raisedCount = 0;
    for (const Contact &c : contacts) {
        const auto pair = std::make_pair(c.body, c.other);
        if (!now.insert(pair).second) continue;  // the same pair again, a later sub-step
        if (touching.count(pair)) continue;  // still touching since last update
        manager.Raise(Event(events::COLLISION)
                         .Object("a", c.body)
                         .Object("b", c.other)
                         .Text("side", SideName(c.side))
                         .Bool("trigger", false));
        ++raisedCount;
    }
    touching.swap(now);
    return raisedCount;
}

size_t CollisionEvents::Update(const std::vector<TriggerEvent> &triggers) {
    size_t raisedCount = 0;
    for (const TriggerEvent &t : triggers) {
        if (t.type != TriggerEvent::Type::Enter) continue;
        manager.Raise(Event(events::COLLISION)
                         .Object("a", t.other)
                         .Object("b", t.trigger)
                         .Text("side", "trigger")
                         .Bool("trigger", true));
        ++raisedCount;
    }
    return raisedCount;
}

// --- death and spawn ---------------------------------------------------------

RespawnEvents::RespawnEvents(EventManager &manager, World &world, ObjectFilter owns, double delaySeconds)
    : manager(manager), world(world), owns(std::move(owns)), delaySeconds(delaySeconds) {
    onDeath = manager.Register(events::DEATH, [this](const Event &e) {
        const ObjectId id = e.GetObject("object");
        GameObject *object = this->world.Find(id);
        if (!object || (this->owns && !this->owns(*object))) return;
        Respawn *r = object->Get<Respawn>();
        if (!r || pendingSpawns.count(id) || retrySpawns.count(id)) return;
        ++r->deaths;
        dying.insert(id);  // a death that came back over the network counts too
        if (autoSpawn) Spawn(id, this->delaySeconds);
    });
    onSpawn = manager.Register(events::SPAWN, [this](const Event &e) {
        const ObjectId id = e.GetObject("object");
        dying.erase(id);
        pendingSpawns.erase(id);
        retrySpawns.erase(id);
        GameObject *object = this->world.Find(id);
        if (!object || (this->owns && !this->owns(*object))) return;
        systems::PlaceAtSpawn(this->world, *object, static_cast<int>(e.GetInt("spawn")));
    });
}

void RespawnEvents::Spawn(ObjectId id, double delay) {
    const GameObject *object = world.Find(id);
    const Respawn *r = object ? object->Get<Respawn>() : nullptr;
    const int spawn = r ? r->spawnIndex : 0;
    const uint64_t sequence = manager.RaiseAfter(Event(events::SPAWN, PRIORITY_HIGH).Object("object", id).Int("spawn", spawn), delay);
    if (sequence) pendingSpawns[id] = sequence;
    else retrySpawns[id] = spawn;
}

bool RespawnEvents::CancelRespawn(ObjectId id) {
    if (retrySpawns.erase(id)) { dying.erase(id); return true; }
    const auto it = pendingSpawns.find(id);
    if (it == pendingSpawns.end()) return false;
    const bool cancelled = manager.Cancel(it->second);
    pendingSpawns.erase(it);
    dying.erase(id);
    return cancelled;
}

RespawnEvents::~RespawnEvents() {
    for (const auto &[id, sequence] : pendingSpawns) manager.Cancel(sequence);
    manager.Unregister(onDeath);
    manager.Unregister(onSpawn);
}

size_t RespawnEvents::Update(const ObjectFilter &filter) {
    // A failed spawn raise must not strand the object or count a second death
    size_t raisedCount = 0;
    for (auto it = retrySpawns.begin(); it != retrySpawns.end();) {
        auto *object = world.Find(it->first);
        if (!object || !object->Has<Respawn>() || (owns && !owns(*object))) {
            dying.erase(it->first);
            it = retrySpawns.erase(it);
            continue;
        }
        const auto sequence = manager.RaiseAfter(Event(events::SPAWN, PRIORITY_HIGH)
            .Object("object", it->first).Int("spawn", it->second), delaySeconds);
        if (!sequence) { ++it; continue; }
        pendingSpawns[it->first] = sequence;
        it = retrySpawns.erase(it);
        ++raisedCount;
    }
    std::vector<std::pair<ObjectId, const GameObject *>> zones;
    world.Each<Transform, DeathZone>([&zones](GameObject &object, Transform &, DeathZone &) {
        zones.emplace_back(object.Id(), &object);
    });
    if (zones.empty()) return raisedCount;

    world.Each<Transform, Respawn>([&](GameObject &object, Transform &t, Respawn &) {
        if (filter && !filter(object)) return;
        if (dying.count(object.Id())) return;  // already dying: once per death, not once per frame
        for (const auto &zone : zones) {
            if (!t.Overlaps(*zone.second->Get<Transform>())) continue;
            const auto sequence = manager.Raise(Event(events::DEATH, PRIORITY_HIGH)
                             .Object("object", object.Id())
                             .Object("zone", zone.first)
                             .Text("cause", zone.second->Kind().empty() ? "zone" : zone.second->Kind()));
            if (sequence) { dying.insert(object.Id()); ++raisedCount; }
            return;
        }
    });
    return raisedCount;
}

// --- input -------------------------------------------------------------------

InputEvents::InputEvents(EventManager &manager, std::vector<KeyBinding> bindings)
    : manager(manager), bindings(std::move(bindings)) {}

size_t InputEvents::Update(ObjectId target, const std::function<bool(SDL_Scancode)> &isDown) {
    // Each distinct key once, with every button it presses
    std::vector<std::pair<SDL_Scancode, uint32_t>> keys;
    for (const KeyBinding &b : bindings) {
        bool merged = false;
        for (auto &k : keys) {
            if (k.first == b.key) { k.second |= b.button; merged = true; }
        }
        if (!merged) keys.emplace_back(b.key, b.button);
    }

    size_t raisedCount = 0;
    for (const auto &k : keys) {
        const bool pressed = isDown ? isDown(k.first) : Input::IsKeyPressed(k.first);
        const bool was = down.count(k.first) != 0;
        if (pressed == was) continue;
        if (pressed) down.insert(k.first);
        else down.erase(k.first);

        uint32_t buttons = 0;
        for (const auto &other : keys) {
            if (down.count(other.first)) buttons |= other.second;
        }
        held = buttons;
        manager.Raise(Event(events::INPUT, PRIORITY_HIGH)
                         .Object("object", target)
                         .Int("key", static_cast<int64_t>(k.first))
                         .Int("button", static_cast<int64_t>(k.second))
                         .Bool("down", pressed)
                         .Int("buttons", static_cast<int64_t>(buttons)));
        ++raisedCount;
    }
    return raisedCount;
}

void InputEvents::Apply(const Event &input, InputState &state) {
    state.buttons = static_cast<uint32_t>(input.GetInt("buttons"));
}

// --- chords ------------------------------------------------------------------

ChordEvents::ChordEvents(EventManager &manager, double windowSeconds) : manager(manager), windowSeconds(windowSeconds) {
    handler = manager.Register(events::INPUT, [this](const Event &e) { OnInput(e); });
}

ChordEvents::~ChordEvents() { manager.Unregister(handler); }

void ChordEvents::Add(const std::string &name, std::vector<SDL_Scancode> keys) {
    chords.push_back(Chord{name, std::move(keys), false});
}

void ChordEvents::OnInput(const Event &e) {
    const SDL_Scancode key = static_cast<SDL_Scancode>(e.GetInt("key"));
    if (!e.GetBool("down")) {
        downAt.erase(key);
        for (Chord &c : chords) {
            if (std::find(c.keys.begin(), c.keys.end(), key) != c.keys.end()) c.fired = false;  // re-armed
        }
        return;
    }
    downAt[key] = e.time;

    const double perTic = manager.SecondsPerTic();
    const int64_t window = perTic > 0.0 ? static_cast<int64_t>(windowSeconds / perTic) : static_cast<int64_t>(windowSeconds);
    for (Chord &c : chords) {
        if (c.fired || c.keys.empty() || std::find(c.keys.begin(), c.keys.end(), key) == c.keys.end()) continue;
        int64_t first = e.time, last = e.time;
        bool allHeld = true;
        for (SDL_Scancode k : c.keys) {
            auto it = downAt.find(k);
            if (it == downAt.end()) { allHeld = false; break; }
            first = std::min(first, it->second);
            last = std::max(last, it->second);
        }
        if (!allHeld || last - first > window) continue;
        c.fired = true;
        manager.Raise(Event(events::CHORD, PRIORITY_HIGH).Object("object", e.GetObject("object")).Text("name", c.name));
    }
}
}  // namespace engine
