#include "engine/Event.hpp"

#include <cstdio>

namespace engine {
// --- EventType ---------------------------------------------------------------

uint32_t EventType::Hash(const std::string &name) {
    uint32_t h = 2166136261u;
    for (unsigned char c : name) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

EventType::EventType(const char *n) : EventType(std::string(n ? n : "")) {}
EventType::EventType(std::string n) : id(Hash(n)), name(std::move(n)) {}

// --- Value -------------------------------------------------------------------

void Value::Write(Writer &w) const {
    w.U8(static_cast<uint8_t>(GetKind()));
    switch (GetKind()) {
    case Kind::Bool: w.Bool(std::get<bool>(data)); break;
    case Kind::Int: w.I64(std::get<int64_t>(data)); break;
    case Kind::Float: w.F32(std::get<float>(data)); break;
    case Kind::Text: w.Str(std::get<std::string>(data)); break;
    case Kind::Object: w.U32(std::get<ObjectRef>(data).id); break;
    case Kind::Point: w.F32(std::get<Vec2>(data).x); w.F32(std::get<Vec2>(data).y); break;
    }
}

Value Value::Read(Reader &r) {
    switch (static_cast<Kind>(r.U8())) {
    case Kind::Bool: return Value(r.Bool());
    case Kind::Int: return Value(r.I64());
    case Kind::Float: return Value(r.F32());
    case Kind::Text: return Value(r.Str());
    case Kind::Object: return Value(ObjectRef{r.U32()});
    case Kind::Point: {
        Vec2 p;
        p.x = r.F32();
        p.y = r.F32();
        return Value(p);
    }
    }
    r.Fail();  // a kind this build does not know
    return Value();
}

bool Value::AsBool(bool fallback) const {
    if (const bool *v = std::get_if<bool>(&data)) return *v;
    return fallback;
}

int64_t Value::AsInt(int64_t fallback) const {
    if (const int64_t *v = std::get_if<int64_t>(&data)) return *v;
    if (const float *v = std::get_if<float>(&data)) return static_cast<int64_t>(*v);
    if (const bool *v = std::get_if<bool>(&data)) return *v ? 1 : 0;
    return fallback;
}

float Value::AsFloat(float fallback) const {
    if (const float *v = std::get_if<float>(&data)) return *v;
    if (const int64_t *v = std::get_if<int64_t>(&data)) return static_cast<float>(*v);
    return fallback;
}

std::string Value::AsText(const std::string &fallback) const {
    if (const std::string *v = std::get_if<std::string>(&data)) return *v;
    return fallback;
}

ObjectId Value::AsObject(ObjectId fallback) const {
    if (const ObjectRef *v = std::get_if<ObjectRef>(&data)) return v->id;
    return fallback;
}

Vec2 Value::AsPoint(Vec2 fallback) const {
    if (const Vec2 *v = std::get_if<Vec2>(&data)) return *v;
    return fallback;
}

std::string Value::Describe() const {
    char buf[64];
    switch (GetKind()) {
    case Kind::Bool: return std::get<bool>(data) ? "true" : "false";
    case Kind::Int: return std::to_string(std::get<int64_t>(data));
    case Kind::Float: std::snprintf(buf, sizeof buf, "%g", std::get<float>(data)); return buf;
    case Kind::Text: return std::get<std::string>(data);
    case Kind::Object: return "@" + std::to_string(std::get<ObjectRef>(data).id);
    case Kind::Point: std::snprintf(buf, sizeof buf, "(%g,%g)", std::get<Vec2>(data).x, std::get<Vec2>(data).y); return buf;
    }
    return "?";
}

// --- Event -------------------------------------------------------------------

Event &Event::Bool(const std::string &name, bool v) { params[name] = Value(v); return *this; }
Event &Event::Int(const std::string &name, int64_t v) { params[name] = Value(v); return *this; }
Event &Event::Float(const std::string &name, float v) { params[name] = Value(v); return *this; }
Event &Event::Text(const std::string &name, std::string v) { params[name] = Value(std::move(v)); return *this; }
Event &Event::Object(const std::string &name, ObjectId v) { params[name] = Value(ObjectRef{v}); return *this; }
Event &Event::Point(const std::string &name, Vec2 v) { params[name] = Value(v); return *this; }

bool Event::GetBool(const std::string &name, bool fallback) const {
    auto it = params.find(name);
    return it == params.end() ? fallback : it->second.AsBool(fallback);
}
int64_t Event::GetInt(const std::string &name, int64_t fallback) const {
    auto it = params.find(name);
    return it == params.end() ? fallback : it->second.AsInt(fallback);
}
float Event::GetFloat(const std::string &name, float fallback) const {
    auto it = params.find(name);
    return it == params.end() ? fallback : it->second.AsFloat(fallback);
}
std::string Event::GetText(const std::string &name, const std::string &fallback) const {
    auto it = params.find(name);
    return it == params.end() ? fallback : it->second.AsText(fallback);
}
ObjectId Event::GetObject(const std::string &name, ObjectId fallback) const {
    auto it = params.find(name);
    return it == params.end() ? fallback : it->second.AsObject(fallback);
}
Vec2 Event::GetPoint(const std::string &name, Vec2 fallback) const {
    auto it = params.find(name);
    return it == params.end() ? fallback : it->second.AsPoint(fallback);
}

std::string Event::Describe() const {
    std::string s = type.Name() + " #" + std::to_string(sequence) + " t=" + std::to_string(time) +
                    " p" + std::to_string(priority) + " a" + std::to_string(age);
    if (source != SOURCE_LOCAL) s += " src=" + std::to_string(source);
    else s += remote ? " src=server" : " src=local";
    if (remote) s += " remote";
    s += " {";
    bool first = true;
    for (const auto &p : params) {
        if (!first) s += ' ';
        first = false;
        s += p.first + "=" + p.second.Describe();
    }
    return s + "}";
}

void Event::Write(Writer &w) const {
    w.Str(type.Name());
    w.I32(priority);
    w.I32(age);
    w.I32(source);
    w.I64(time);
    w.F32(delay);
    w.U16(static_cast<uint16_t>(params.size()));
    for (const auto &p : params) {
        w.Str(p.first);
        p.second.Write(w);
    }
}

bool Event::Read(Reader &r) {
    type = EventType(r.Str());
    priority = r.I32();
    age = r.I32();
    source = r.I32();
    originTime = r.I64();
    delay = r.F32();
    params.clear();
    const uint16_t count = r.U16();
    if (count > 64 || type.Name().size() > 128) return false;
    for (uint16_t i = 0; i < count && r.Ok(); ++i) {
        std::string name = r.Str();
        params[name] = Value::Read(r);
    }
    return r.Ok() && !type.Name().empty();
}

// --- the network form --------------------------------------------------------

namespace {
constexpr char EVENT_MARK = 'E';
constexpr uint32_t MAX_BATCH = 4096;  // a corrupt count must not allocate the world
}

bool IsEventMessage(const std::string &message) {
    return message.size() >= 2 && message[0] == EVENT_MARK && (message[1] == 'T' || message[1] == 'B');
}

std::string EncodeEvents(const std::vector<Event> &batch, Encoding encoding) {
    std::unique_ptr<Writer> w = MakeWriter(encoding);
    w->U32(static_cast<uint32_t>(batch.size()));
    for (const Event &e : batch) e.Write(*w);
    std::string message;
    message += EVENT_MARK;
    message += encoding == Encoding::Text ? 'T' : 'B';
    if (encoding == Encoding::Text) message += ' ';
    message += w->Take();
    return message;
}

bool DecodeEvents(const std::string &message, std::vector<Event> &out) {
    if (message.size() > 4 * 1024 * 1024 || !IsEventMessage(message)) return false;
    const Encoding encoding = message[1] == 'T' ? Encoding::Text : Encoding::Binary;
    std::unique_ptr<Reader> r = MakeReader(encoding, message, encoding == Encoding::Text ? 3 : 2);
    const uint32_t count = r->U32();
    if (!r->Ok() || count > MAX_BATCH) return false;
    std::vector<Event> batch;
    batch.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Event e;
        if (!e.Read(*r)) return false;
        batch.push_back(std::move(e));
    }
    if (!r->AtEnd()) return false;
    for (Event &e : batch) out.push_back(std::move(e));
    return true;
}
}  // namespace engine
