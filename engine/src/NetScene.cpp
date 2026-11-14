#include "engine/NetScene.hpp"

#include "engine/Engine.hpp"
#include "engine/Timeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace engine {
namespace {
constexpr size_t EVENT_RETENTION = 8192;
constexpr int64_t EVENT_POLL_MICROS = 33334;  // at most 30 polls/s; never catch up in bursts
constexpr size_t EVENT_BATCH = 256;
constexpr size_t EVENT_BYTES = 262144;
bool ValidRequestEvent(const Event &e) {
    if (e.Type().Name().size() > 128 || e.Params().size() > 64 ||
        e.priority < PRIORITY_HIGH || e.priority > PRIORITY_LOW ||
        !std::isfinite(e.delay) || e.delay < 0 || e.delay > 60) return false;
    for (const auto &[key, value] : e.Params()) {
        if (key.size() > 128) return false;
        if (value.GetKind() == Value::Kind::Text && value.AsText().size() > 4096) return false;
        if (value.GetKind() == Value::Kind::Float && !std::isfinite(value.AsFloat())) return false;
        if (value.GetKind() == Value::Kind::Point &&
            (!std::isfinite(value.AsPoint().x) || !std::isfinite(value.AsPoint().y))) return false;
    }
    return true;
}

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool IsSnapshot(const std::string &message) {
    return !message.empty() && (message[0] == 'T' || message[0] == 'B') && message != "BYE";
}

// Each stamp from `owner` newer than the last seen is one latency sample
void SampleLatency(const World &world, int notOwner, std::map<ObjectId, int64_t> &lastStamp, NetStats &stats) {
    const int64_t now = Timeline::RealTimeNow();
    world.Each<NetIdentity>([&](const GameObject &object, const NetIdentity &net) {
        if (net.owner < 0 || net.owner == notOwner || net.stampMicros <= 0) return;
        int64_t &last = lastStamp[object.Id()];
        if (net.stampMicros > last) {
            last = net.stampMicros;
            stats.AddLatency(now - net.stampMicros);
        }
    });
}

// An object the game has not stamped since the last send is stamped now
int64_t StampUnstamped(World &world, int owner, int64_t lastSent) {
    const int64_t now = Timeline::RealTimeNow();
    int64_t newest = lastSent;
    world.Each<NetIdentity>([&](GameObject &, NetIdentity &n) {
        if (n.owner != owner) return;
        if (n.stampMicros <= lastSent) n.stampMicros = now;
        newest = std::max(newest, n.stampMicros);
    });
    return newest;
}
}  // namespace

// --- NetOptions ------------------------------------------------------------------

bool NetOptions::Parse(int argc, char **argv, NetOptions &out, std::string *error) {
    auto fail = [error](const std::string &message) {
        if (error) *error = message;
        return false;
    };
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--topology") {
            if (value == "cs" || value == "client-server") out.topology = Topology::ClientServer;
            else if (value == "p2p" || value == "peer-to-peer") out.topology = Topology::PeerToPeer;
            else return fail("--topology takes cs or p2p, not " + value);
        } else if (flag == "--encoding") {
            if (!ParseEncoding(value, out.encoding)) return fail("--encoding takes text or binary, not " + value);
        } else if (flag == "--delta") {
            if (value == "on") out.delta = true;
            else if (value == "off") out.delta = false;
            else return fail("--delta takes on or off, not " + value);
        } else if (flag == "--upstream") {
            if (value == "state") out.upstream = Upstream::State;
            else if (value == "input") out.upstream = Upstream::Input;
            else return fail("--upstream takes state or input, not " + value);
        } else if (flag == "--keyframe") {
            out.keyframeEvery = std::atoi(value.c_str());
            if (out.keyframeEvery < 1) return fail("--keyframe takes a count of 1 or more");
        } else {
            continue;
        }
        ++i;
    }
    return true;
}

std::string NetOptions::Describe() const {
    std::string s = topology == Topology::PeerToPeer ? "p2p" : "cs";
    s += std::string(" ") + EncodingName(encoding);
    s += delta ? " delta" : " full";
    s += upstream == Upstream::Input ? " input" : " state";
    return s;
}

SnapshotOptions NetOptions::Downstream() const {
    return SnapshotOptions(encoding, delta, keyframeEvery);
}

SnapshotOptions NetOptions::Upward() const {
    if (upstream == Upstream::Input) return SnapshotOptions(encoding, delta, keyframeEvery, {InputState::TYPE});
    return SnapshotOptions(encoding, delta, keyframeEvery);
}

// --- NetStats --------------------------------------------------------------------

void NetStats::AddLatency(int64_t micros) {
    std::lock_guard<std::mutex> lock(mutex);
    if (samples.size() < MAX) samples.push_back(micros);
    else samples[next] = micros;
    next = (next + 1) % MAX;
}

LatencySummary NetStats::Latency() const {
    std::vector<int64_t> copy;
    {
        std::lock_guard<std::mutex> lock(mutex);
        copy = samples;
    }
    LatencySummary s;
    s.samples = copy.size();
    if (copy.empty()) return s;
    std::sort(copy.begin(), copy.end());
    double sum = 0;
    for (int64_t v : copy) sum += static_cast<double>(v);
    s.meanMs = sum / static_cast<double>(copy.size()) / 1000.0;
    s.p50Ms = static_cast<double>(copy[copy.size() / 2]) / 1000.0;
    s.p95Ms = static_cast<double>(copy[std::min(copy.size() - 1, copy.size() * 95 / 100)]) / 1000.0;
    s.maxMs = static_cast<double>(copy.back()) / 1000.0;
    return s;
}

void NetStats::ResetLatency() {
    std::lock_guard<std::mutex> lock(mutex);
    samples.clear();
    next = 0;
}

// --- SceneServer -----------------------------------------------------------------
#ifndef __EMSCRIPTEN__

SceneServer::SceneServer(Engine &engineRef, Server::Config config, NetOptions opts)
    : engine(engineRef), server(engineRef, config), options(opts), downstream(opts.Downstream()) {}

void SceneServer::SetRelayFilter(ObjectFilter filter) {
    std::lock_guard<std::mutex> lock(engine.GetWorldMutex());
    relay = std::move(filter);
}

void SceneServer::ShareEvents(EventManager &manager, const std::vector<EventType> &types) {
    std::lock_guard<std::mutex> lock(eventMutex);
    events = &manager;
    for (const EventType &type : types) shared.insert(type.Id());
    manager.SetRouter([this](Event &e) {
        std::lock_guard<std::mutex> guard(eventMutex);
        if (shared.count(e.Type().Id())) {
            for (auto &[id, stream] : eventStreams) {
                if (stream.gap) continue;
                if (stream.pending.size() >= EVENT_RETENTION) {
                    stream.gap = true;
                    stream.pending.clear();
                    ++stats.eventGaps;
                } else stream.pending.push_back(e);
            }
        }
        return false;  // the server handles its own copy as well
    });
}

void SceneServer::AllowClientEvents(const std::vector<EventType> &types, EventValidator validator) {
    std::lock_guard<std::mutex> lock(eventMutex);
    acceptedTypes.clear();
    for (const auto &type : types) acceptedTypes.insert(type.Name());
    eventValidator = std::move(validator);
}

// Called on a client's request thread
std::string SceneServer::EventRequest(int id, const std::string &request) {
    const auto newline = request.find('\n');
    std::istringstream header(request.substr(0, newline));
    std::string verb, extra;
    uint64_t number = 0;
    if (!(header >> verb >> number) || (header >> extra) || request.size() > EVENT_BYTES)
        return "EVENT_ERROR malformed header";
    if (verb == "EVENT_POLL") {
        EventManager *manager;
        uint64_t first;
        std::vector<Event> batch;
        {
            // Snapshot just the stream under its own lock
            std::lock_guard<std::mutex> lock(eventMutex);
            const auto found = eventStreams.find(id);
            manager = events;
            if (!manager || found == eventStreams.end()) return "EVENT_ERROR no stream";
            auto &stream = found->second;
            if (stream.gap) return "EVENT_GAP retention exceeded; reconnect";
            if (number > stream.offered) return "EVENT_ERROR invalid acknowledgement";
            while (!stream.pending.empty() && stream.first <= number) {
                stream.pending.pop_front();
                ++stream.first;
            }
            const size_t count = std::min(EVENT_BATCH, stream.pending.size());
            batch.assign(stream.pending.begin(), stream.pending.begin() + count);
            first = stream.first;
            if (count) stream.offered = std::max(stream.offered, first + count - 1);
        }
        std::ostringstream out;
        out << std::setprecision(17) << "EVENTS " << first << ' '
            << manager->Now() * manager->SecondsPerTic() << ' '
            << manager->SecondsPerTic() << ' ' << manager->ClockRate() << '\n'
            << EncodeEvents(batch, options.encoding);
        stats.eventsOut += batch.size();
        return out.str();
    }
    if (verb != "EVENT_SEND" || newline == std::string::npos || number == 0)
        return "EVENT_ERROR unknown request";
    std::lock_guard<std::mutex> worldLock(engine.GetWorldMutex());
    EventManager *manager;
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        manager = events;
        if (!manager || !eventStreams.count(id)) return "EVENT_ERROR no stream";
    }
    std::vector<Event> batch;
    if (!DecodeEvents(request.substr(newline + 1), batch) || batch.empty() || batch.size() > EVENT_BATCH ||
        number > UINT64_MAX - batch.size()) return "EVENT_ERROR malformed batch";
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        if (number > eventStreams.at(id).received + 1) return "EVENT_ERROR sequence gap";
    }
    for (size_t i = 0; i < batch.size(); ++i) {
        Event &e = batch[i];
        EventValidator validator;
        bool allowed;
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            auto &stream = eventStreams.at(id);
            if (number + i <= stream.received) { ++stats.eventRetries; continue; }
            allowed = acceptedTypes.count(e.Type().Name()) && ValidRequestEvent(e);
            validator = eventValidator;
        }
        e.source = id;
        e.remote = true;
        e.age = 0;  // never trust client bookkeeping
        if (allowed && validator) {
            try { allowed = validator(world, id, e); } catch (...) { allowed = false; }
        }
        if (allowed) {
            const double delay = e.delay;
            if (!manager->RaiseAfter(e, delay)) break;  // backpressure: retry this ID
            ++stats.eventsIn;
        } else ++stats.eventsRejected;
        std::lock_guard<std::mutex> lock(eventMutex);
        auto &stream = eventStreams.at(id);
        stream.received = number + i;
        if (!allowed) ++stream.rejected;
    }
    std::lock_guard<std::mutex> lock(eventMutex);
    const auto &stream = eventStreams.at(id);
    return "EVENT_ACK " + std::to_string(stream.received) + " " + std::to_string(stream.rejected);
}

ObjectFilter SceneServer::Authority() const {
    if (options.upstream == Upstream::Input) return {};  // everything, players included
    return OwnedBy(-1);
}

bool SceneServer::Run(const Callbacks &callbacks) {
    std::mutex &mutex = engine.GetWorldMutex();
    Server::Hooks hooks;

    hooks.onJoin = [this, &callbacks, &mutex](int id) {
        std::lock_guard<std::mutex> lock(mutex);
        { std::lock_guard<std::mutex> guard(eventMutex); eventStreams[id] = EventStream{}; }
        if (callbacks.onJoin) callbacks.onJoin(world, id);
        downstream.ForceKeyframe();  // the newcomer needs everything, now
    };

    hooks.onLeave = [this, &callbacks, &mutex](int id) {
        std::lock_guard<std::mutex> lock(mutex);
        if (callbacks.onLeave) callbacks.onLeave(world, id);
        world.DestroyIf([id](const GameObject &o) { return OwnerOf(o) == id; });
        upstream.ForgetSource(id);
        { std::lock_guard<std::mutex> guard(eventMutex); eventStreams.erase(id); }
    };

    hooks.onRequest = [this, &callbacks, &mutex](int id, const std::string &request) -> std::string {
        if (request.rfind("EVENT_", 0) == 0) return EventRequest(id, request);
        // Legacy unnumbered events bypass validation and duplicate suppression
        if (IsEventMessage(request)) { ++stats.eventsRejected; return "EVENT_ERROR use EVENT_SEND"; }
        if (!IsSnapshot(request) && !IsEventMessage(request)) {
            std::lock_guard<std::mutex> lock(mutex);
            return callbacks.onRequest ? callbacks.onRequest(world, id, request) : std::string("ERR unknown request");
        }
        ++stats.messagesIn;
        stats.bytesIn += request.size();
        std::lock_guard<std::mutex> lock(inboxMutex);
        inbox.emplace_back(id, request);
        return "ACK";  // received; it is applied at the start of the next step
    };

    hooks.buildWorld = [this, &mutex]() {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto t0 = std::chrono::steady_clock::now();
            message = downstream.Encode(world, ++tick, relay);
            lastSnapshot = downstream.LastStats();
            stats.encodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
        }
        ++stats.messagesOut;
        stats.bytesOut += message.size();
        return message;
    };

    hooks.step = [this, &callbacks, &mutex](float dt) {
        std::lock_guard<std::mutex> lock(mutex);
        ApplyInbox();
        if (callbacks.step) callbacks.step(world, dt);
        world.Flush();
    };

    return server.Run(hooks);
}

// Main thread, world locked
void SceneServer::ApplyInbox() {
    {
        std::lock_guard<std::mutex> lock(inboxMutex);
        applying.swap(inbox);
    }
    for (const auto &message : applying) {
        const auto t0 = std::chrono::steady_clock::now();
        ApplyOptions apply;
        apply.source = message.first;
        apply.onlyOwner = message.first;  // a client speaks only for its own objects
        apply.createMissing = false;
        upstream.Apply(world, message.second, apply);
        ++stats.decoded;
        stats.decodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }
    applying.clear();
}

#endif  // __EMSCRIPTEN__

// --- SceneClient -----------------------------------------------------------------

namespace {
// The world socket carries nothing but snapshots
Client::Config ForSnapshots(Client::Config config) {
    config.worldTopic = "";
    return config;
}
}  // namespace

SceneClient::SceneClient(Client::Config config, NetOptions opts)
    : client(ForSnapshots(config)), options(opts), up(opts.Upward()) {}

bool SceneClient::Join() {
    return client.Join();
}

GameObject *SceneClient::MyPlayer() {
    GameObject *mine = nullptr;
    const int id = Id();
    world.Each<NetIdentity>([&](GameObject &o, NetIdentity &n) {
        if (!mine && n.owner == id) mine = &o;
    });
    return mine;
}

void SceneClient::ShareEvents(EventManager &manager, const std::vector<EventType> &types) {
    std::lock_guard<std::mutex> lock(eventMutex);
    events = &manager;
    for (const EventType &type : types) shared.insert(type.Id());
    manager.SetRouter([this](Event &e) {
        if (e.remote) return false;  // the server's word: handle it here
        std::lock_guard<std::mutex> guard(eventMutex);
        if (!shared.count(e.Type().Id())) return false;  // not shared: local only
        if (eventOut.size() >= EVENT_RETENTION) {
            ++stats.eventsRejected;
            throw std::overflow_error("network event queue full");
        }
        e.source = Id();
        eventOut.push_back(e);
        return true;  // the server decides when it happens, for everyone
    });
}

ObjectFilter SceneClient::Authority() const {
    if (options.upstream == Upstream::Input) return [](const GameObject &) { return false; };
    return OwnedBy(client.Id());
}

void SceneClient::StampLocal() {
    const int64_t now = Timeline::RealTimeNow();
    const int id = Id();
    world.Each<NetIdentity>([&](GameObject &, NetIdentity &n) {
        if (n.owner == id) n.stampMicros = now;
    });
}

void SceneClient::Tick(float dt, std::mutex &worldMutex) {
    const int id = Id();
    inbox.clear();
    client.Tick(
        dt,
        [&](const std::string &message) {
            ++stats.messagesIn;
            stats.bytesIn += message.size();
            inbox.push_back(message);
        },
        [&]() {
            std::string message;
            {
                std::lock_guard<std::mutex> lock(worldMutex);
                const auto t0 = std::chrono::steady_clock::now();
                lastSentStamp = StampUnstamped(world, id, lastSentStamp);
                message = up.Encode(world, ++tick, OwnedBy(id));
                stats.encodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            }
            ++stats.messagesOut;
            stats.bytesOut += message.size();
            return message;
        });

    // Shared events use acknowledged request/reply delivery, separate from snapshots
    inbox.erase(std::remove_if(inbox.begin(), inbox.end(), IsEventMessage), inbox.end());

    // Everything before the newest keyframe in the queue is superseded by it
    size_t first = 0;
    for (size_t i = inbox.size(); i-- > 0;) {
        if (IsKeyframe(inbox[i])) { first = i; break; }
    }
    stats.skipped += first;
    for (size_t i = first; i < inbox.size(); ++i) {
        ApplyOptions apply;
        apply.source = -1;
        // Sending state: my own player is mine, the server's copy is older
        apply.skipOwner = options.upstream == Upstream::State ? id : ANY_OWNER;
        std::lock_guard<std::mutex> lock(worldMutex);
        const auto t0 = std::chrono::steady_clock::now();
        const ApplyResult result = down.Apply(world, inbox[i], apply);
        if (result.ok && result.keyframe) hasWorld = true;
        world.Flush();
        ++stats.decoded;
        stats.decodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
        // The first batch is whatever queued up between Join() and the first Tick
        if (ticked) SampleLatency(world, id, lastStamp, stats);
        else SampleLatency(world, id, lastStamp, discard);
    }
    if (!inbox.empty()) ticked = true;

    TickEvents();
}

bool SceneClient::EventStreamHealthy() const {
    std::lock_guard<std::mutex> lock(eventMutex);
    return !eventFailed;
}

std::string SceneClient::LastEventError() const {
    std::lock_guard<std::mutex> lock(eventMutex);
    return eventError;
}

void SceneClient::TickEvents() {
    EventManager *manager;
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        manager = events;
        if (!manager || eventFailed || client.Dropped()) return;
        if (sending.empty()) {
            const size_t count = std::min(EVENT_BATCH, eventOut.size());
            sending.assign(eventOut.begin(), eventOut.begin() + count);
            eventOut.erase(eventOut.begin(), eventOut.begin() + count);
        }
    }
    auto fail = [this](const std::string &message) {
        std::lock_guard<std::mutex> lock(eventMutex);
        eventError = message;
        eventFailed = true;
        ++stats.eventGaps;
    };
    // Replies are handled in callbacks: at once natively, a few frames later in a browser.
    // One send and one poll at most in flight, so the stream keeps its order either way.
    if (!sending.empty() && !sendInFlight) {
        const std::string payload = "EVENT_SEND " + std::to_string(sendFirst) + "\n" + EncodeEvents(sending, options.encoding);
        if (payload.size() > EVENT_BYTES) { fail("event batch exceeds wire limit"); return; }
        sendInFlight = true;
        client.RequestAsync(payload, [this, fail](bool ok, const std::string &reply) {
            sendInFlight = false;
            if (!ok) { ++stats.eventRetries; return; }
            std::istringstream in(reply);
            std::string verb;
            uint64_t ack = 0, rejected = 0;
            if (!(in >> verb >> ack >> rejected) || verb != "EVENT_ACK" || ack < sendFirst - 1 || ack >= sendFirst + sending.size()) {
                fail(reply); return;
            }
            const size_t count = ack - (sendFirst - 1);
            sending.erase(sending.begin(), sending.begin() + count);
            sendFirst += count;
            stats.eventsOut += count;
            stats.eventsRejected = rejected;
        });
    }
    if (!pollInFlight && Timeline::RealTimeNow() >= nextEventPoll) {
        const int64_t started = Timeline::RealTimeNow();
        ++stats.eventPolls;
        pollInFlight = true;
        client.RequestAsync("EVENT_POLL " + std::to_string(receivedUntil), [this, fail, started](bool ok, const std::string &reply) {
            pollInFlight = false;
            // Schedule from completion, including failed requests
            nextEventPoll = Timeline::RealTimeNow() + EVENT_POLL_MICROS;
            if (!ok) { ++stats.eventRetries; return; }
            const int64_t received = Timeline::RealTimeNow();
            const auto newline = reply.find('\n');
            std::istringstream header(reply.substr(0, newline));
            std::string verb;
            uint64_t first = 0;
            double now = 0, perTic = 0, rate = 0;
            std::vector<Event> batch;
            if (!(header >> verb >> first >> now >> perTic >> rate) || verb != "EVENTS" ||
                newline == std::string::npos || first > receivedUntil + 1 ||
                !std::isfinite(now) || !std::isfinite(perTic) || perTic <= 0 ||
                !std::isfinite(rate) || rate < 0 ||
                !DecodeEvents(reply.substr(newline + 1), batch)) { fail(reply); return; }
            // Midpoint clock estimate: no assumption that hosts share a steady-clock epoch
            serverSample = now + (received - started) * 0.5e-6 * rate;
            serverRate = rate;
            sampleWall = received;
            for (size_t i = 0; i < batch.size(); ++i) {
                const uint64_t sequence = first + i;
                if (sequence <= receivedUntil) continue;  // duplicate after a lost ACK/reply
                if (scheduled.size() >= EVENT_RETENTION) break;  // stop acknowledging until drained
                Event &e = batch[i];
                e.remote = true;
                scheduled.emplace_back(e.originTime * perTic, std::move(e));
                receivedUntil = sequence;
            }
        });
    }
    if (!sampleWall) return;
    const double serverNow = serverSample + (Timeline::RealTimeNow() - sampleWall) * 1e-6 * serverRate;
    for (auto it = scheduled.begin(); it != scheduled.end();) {
        if (it->first > serverNow) { ++it; continue; }
        if (!manager->Raise(it->second)) break;  // manager capacity: keep it for retry
        ++stats.eventsIn;
        it = scheduled.erase(it);
    }
}

// --- ScenePeer -------------------------------------------------------------------
#ifndef __EMSCRIPTEN__

namespace {
constexpr const char *TOPIC_SNAP = "SNAP";
constexpr const char *TOPIC_EVENTS = "SCENE_EVENTS";  // not "EVENTS": Peer uses that for its own event log
}

ScenePeer::ScenePeer(Config cfg, NetOptions opts)
    : config(cfg), options(opts), peer([&cfg] {
          Peer::Config p = cfg.peer;
          if (std::find(p.topics.begin(), p.topics.end(), TOPIC_SNAP) == p.topics.end()) p.topics.push_back(TOPIC_SNAP);
          if (std::find(p.topics.begin(), p.topics.end(), TOPIC_EVENTS) == p.topics.end()) p.topics.push_back(TOPIC_EVENTS);
          return p;
      }()),
      up(opts.Upward()), state(SnapshotOptions(opts.encoding)),
      sendsSinceState(opts.keyframeEvery - 1) {}  // with inputs, the very first send is the full state

ScenePeer::~ScenePeer() {
    // Say goodbye, so the others drop this peer's objects at once instead of after a timeout
    if (peer.Id() >= 0) peer.Publish(TOPIC_SNAP, std::to_string(peer.Id()) + " BYE");
}

int ScenePeer::Claim(int requested) {
    const int id = peer.Claim(requested);
    if (id < 0) return id;
    peer.ConnectAll();
    world.SetNextId((static_cast<ObjectId>(id) + 1) << 24);
    lastSendMs = NowMs();
    return id;
}

ObjectFilter ScenePeer::Authority() const {
    const int id = peer.Id();
    if (options.upstream == Upstream::Input) {
        // Every player moves here: mine from my keys, the others from theirs
        return [id](const GameObject &o) { const int owner = OwnerOf(o); return owner == id || (owner >= 0 && o.Has<InputState>()); };
    }
    return OwnedBy(id);
}

void ScenePeer::StampLocal() {
    const int64_t now = Timeline::RealTimeNow();
    const int id = Id();
    world.Each<NetIdentity>([&](GameObject &, NetIdentity &n) {
        if (n.owner == id) n.stampMicros = now;
    });
}

void ScenePeer::ShareEvents(EventManager &manager, const std::vector<EventType> &types) {
    std::lock_guard<std::mutex> lock(eventMutex);
    events = &manager;
    for (const EventType &type : types) shared.insert(type.Id());
    manager.SetRouter([this](Event &e) {
        if (e.remote) return false;
        std::lock_guard<std::mutex> guard(eventMutex);
        if (shared.count(e.Type().Id())) {
            e.source = Id();
            eventOut.push_back(e);
        }
        return false;  // distributed: the peer that raised it handles it at once
    });
}

int ScenePeer::PeersHeard() const {
    std::lock_guard<std::mutex> lock(heardMutex);
    return static_cast<int>(lastHeardMs.size());
}

void ScenePeer::Send(std::mutex &worldMutex, bool fullState) {
    const int id = Id();
    std::string message;
    {
        std::lock_guard<std::mutex> lock(worldMutex);
        const auto t0 = std::chrono::steady_clock::now();
        lastSentStamp = StampUnstamped(world, id, lastSentStamp);
        message = (fullState ? state : up).Encode(world, ++tick, OwnedBy(id));
        stats.encodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }
    peer.Publish(TOPIC_SNAP, std::to_string(id) + ' ' + message);
    ++stats.messagesOut;
    stats.bytesOut += message.size();
    lastSendMs = NowMs();
}

void ScenePeer::OnSnapshot(int sender, const std::string &snapshot, std::mutex &worldMutex) {
    {
        std::lock_guard<std::mutex> lock(heardMutex);
        lastHeardMs[sender] = NowMs();
    }
    if (snapshot == "BYE") {
        std::lock_guard<std::mutex> lock(heardMutex);
        lastHeardMs.erase(sender);
    }
    std::lock_guard<std::mutex> lock(worldMutex);
    if (snapshot == "BYE") {
        world.DestroyIf([sender](const GameObject &o) { return OwnerOf(o) == sender; });
        world.Flush();
        down.ForgetSource(sender);
        return;
    }
    ++stats.messagesIn;
    stats.bytesIn += snapshot.size();
    const auto t0 = std::chrono::steady_clock::now();
    ApplyOptions apply;
    apply.source = sender;
    apply.onlyOwner = sender;
    down.Apply(world, snapshot, apply);
    world.Flush();
    ++stats.decoded;
    stats.decodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count());
    SampleLatency(world, Id(), lastStamp, stats);
}

void ScenePeer::Tick(float dt, std::mutex &worldMutex) {
    // 1. Hear everyone
    peer.Tick([&](const std::string &topic, std::istringstream &body, int) {
        if (topic != TOPIC_SNAP && topic != TOPIC_EVENTS) return;
        int sender = -1;
        body >> sender;
        body.get();  // the space after the id
        const std::string rest((std::istreambuf_iterator<char>(body)), std::istreambuf_iterator<char>());
        if (sender < 0 || sender == Id()) return;
        if (topic == TOPIC_SNAP) {
            OnSnapshot(sender, rest, worldMutex);
            return;
        }
        // Another peer's shared events: raised here as they arrive
        EventManager *manager;
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            manager = events;
        }
        std::vector<Event> batch;
        if (!manager || !DecodeEvents(rest, batch)) return;
        for (Event &e : batch) {
            e.remote = true;
            e.source = sender;
            const double delay = e.delay;
            manager->RaiseAfter(std::move(e), delay);
            ++stats.eventsIn;
        }
    });

    // My shared events, already handled here, to every other peer
    {
        std::vector<Event> out;
        {
            std::lock_guard<std::mutex> lock(eventMutex);
            out.swap(eventOut);
        }
        if (!out.empty()) {
            const std::string message = EncodeEvents(out, options.encoding);
            peer.Publish(TOPIC_EVENTS, std::to_string(Id()) + ' ' + message);
            ++stats.messagesOut;
            stats.bytesOut += message.size();
            stats.eventsOut += out.size();
        }
    }

    // 2. Send my objects, paced by game time, with periodic full-state corrections
    sendAccumulator += dt;
    if (sendAccumulator >= config.sendIntervalSeconds) {
        sendAccumulator -= config.sendIntervalSeconds;
        if (sendAccumulator > config.sendIntervalSeconds) sendAccumulator = config.sendIntervalSeconds;
        bool full = false;
        if (options.upstream == Upstream::Input && ++sendsSinceState >= options.keyframeEvery) {
            sendsSinceState = 0;
            full = true;
        }
        Send(worldMutex, full);
    }

    // 3. Heartbeat on wall time when paused: the full state
    if (NowMs() - lastSendMs >= config.heartbeatMs) Send(worldMutex, true);

    // 4. Drop peers that have gone silent, and their objects
    std::vector<int> silent;
    {
        std::lock_guard<std::mutex> lock(heardMutex);
        const long long now = NowMs();
        for (auto it = lastHeardMs.begin(); it != lastHeardMs.end();) {
            if (now - it->second > config.peerTimeoutMs) {
                silent.push_back(it->first);
                it = lastHeardMs.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!silent.empty()) {
        std::lock_guard<std::mutex> lock(worldMutex);
        for (int sender : silent) {
            world.DestroyIf([sender](const GameObject &o) { return OwnerOf(o) == sender; });
            down.ForgetSource(sender);
        }
        world.Flush();
    }
}
#endif  // __EMSCRIPTEN__
}  // namespace engine
