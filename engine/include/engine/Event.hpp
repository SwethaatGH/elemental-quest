#pragma once

#include "engine/Entity.hpp"  // Vec2
#include "engine/GameObject.hpp"  // ObjectId
#include "engine/Serialize.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace engine {
// Part 1.1: an event is a hashed type plus named parameters

// The event types the engine raises itself
namespace events {
constexpr const char *COLLISION = "collision";  // a, b (objects), side, trigger (bool)
constexpr const char *DEATH = "death";  // object, zone, cause (text)
constexpr const char *SPAWN = "spawn";  // object, spawn (spawn point index)
constexpr const char *INPUT = "input";  // object, key, button, down, buttons
constexpr const char *CHORD = "chord";  // object, name: keys pressed together

// Part 1.3: the replay is driven by events too
constexpr const char *REPLAY_RECORD_START = "replay_record_start";
constexpr const char *REPLAY_RECORD_STOP = "replay_record_stop";
constexpr const char *REPLAY_PLAY = "replay_play";  // seconds (how far back), speed
constexpr const char *REPLAY_PAUSE = "replay_pause";
constexpr const char *REPLAY_RESUME = "replay_resume";
constexpr const char *REPLAY_SPEED = "replay_speed";  // speed
constexpr const char *REPLAY_STOP = "replay_stop";
constexpr const char *REPLAY_FINISHED = "replay_finished";
}  // namespace events

// Lower is sooner, among events due at the same time
enum EventPriority : int { PRIORITY_HIGH = 0, PRIORITY_NORMAL = 1, PRIORITY_LOW = 2 };

// Who raised an event: a client's id, or this value for the server
constexpr int SOURCE_LOCAL = -1;

class EventType {
public:
    EventType() = default;
    EventType(const char *name);  // implicit, so "death" works wherever a type is expected
    EventType(std::string name);

    uint32_t Id() const { return id; }
    const std::string &Name() const { return name; }
    bool operator==(const EventType &other) const { return id == other.id; }
    bool operator!=(const EventType &other) const { return id != other.id; }

    // FNV-1a: the same name gives the same id in every process
    static uint32_t Hash(const std::string &name);

private:
    uint32_t id = 0;
    std::string name;
};

// A reference to a game object
struct ObjectRef {
    ObjectId id = NO_OBJECT;
};

// One parameter: a tagged union (slides 21-24)
class Value {
public:
    enum class Kind : uint8_t { Bool, Int, Float, Text, Object, Point };

    Value() = default;
    explicit Value(bool v) : data(v) {}
    explicit Value(int64_t v) : data(v) {}
    explicit Value(float v) : data(v) {}
    explicit Value(std::string v) : data(std::move(v)) {}
    explicit Value(ObjectRef v) : data(v) {}
    explicit Value(Vec2 v) : data(v) {}

    Kind GetKind() const { return static_cast<Kind>(data.index()); }

    void Write(Writer &w) const;
    static Value Read(Reader &r);

    // Each returns `fallback` when the value holds another kind
    bool AsBool(bool fallback = false) const;
    int64_t AsInt(int64_t fallback = 0) const;  // a Float or Bool converts
    float AsFloat(float fallback = 0.0f) const;  // an Int converts
    std::string AsText(const std::string &fallback = {}) const;
    ObjectId AsObject(ObjectId fallback = NO_OBJECT) const;
    Vec2 AsPoint(Vec2 fallback = {}) const;

    std::string Describe() const;  // for logs

private:
    std::variant<bool, int64_t, float, std::string, ObjectRef, Vec2> data{false};
};

class Event {
public:
    Event() = default;
    explicit Event(EventType type, int priority = PRIORITY_NORMAL) : type(std::move(type)), priority(priority) {}

    const EventType &Type() const { return type; }
    bool Is(const EventType &t) const { return type == t; }

    // Parameters: chainable setters, getters with fallbacks
    Event &Bool(const std::string &name, bool v);
    Event &Int(const std::string &name, int64_t v);
    Event &Float(const std::string &name, float v);
    Event &Text(const std::string &name, std::string v);
    Event &Object(const std::string &name, ObjectId v);
    Event &Point(const std::string &name, Vec2 v);

    bool Has(const std::string &name) const { return params.count(name) != 0; }
    bool GetBool(const std::string &name, bool fallback = false) const;
    int64_t GetInt(const std::string &name, int64_t fallback = 0) const;
    float GetFloat(const std::string &name, float fallback = 0.0f) const;
    std::string GetText(const std::string &name, const std::string &fallback = {}) const;
    ObjectId GetObject(const std::string &name, ObjectId fallback = NO_OBJECT) const;
    Vec2 GetPoint(const std::string &name, Vec2 fallback = {}) const;
    const std::map<std::string, Value> &Params() const { return params; }

    // Bookkeeping, filled in by the EventManager when raised
    int priority = PRIORITY_NORMAL;
    int64_t time = 0;  // due time, in tics of the manager's Timeline
    int age = 0;  // 0 if raised outside a handler, else parent age + 1
    uint64_t sequence = 0;  // raise order on this manager: the last tie-breaker
    int source = SOURCE_LOCAL;
    bool remote = false;  // arrived over the network
    int64_t originTime = 0;  // `time` on the endpoint that first raised it
    float delay = 0.0f;  // seconds it was raised to wait; travels with it

    // One line for logs, e.g. "death #12 t=340 p1 a0 src2 {cause=pit}"
    std::string Describe() const;

    // Serialises type, priority, age, source, time, delay and parameters
    void Write(Writer &w) const;
    bool Read(Reader &r);

private:
    EventType type;
    std::map<std::string, Value> params;
};

// Network form (Part 1.2): a batch of events in one message
std::string EncodeEvents(const std::vector<Event> &batch, Encoding encoding);
bool DecodeEvents(const std::string &message, std::vector<Event> &out);
bool IsEventMessage(const std::string &message);
}  // namespace engine
