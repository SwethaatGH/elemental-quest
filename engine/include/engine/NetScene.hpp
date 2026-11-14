#pragma once

#include "engine/Client.hpp"
#include "engine/EventManager.hpp"
#include "engine/Peer.hpp"
#include "engine/Server.hpp"
#include "engine/Snapshot.hpp"
#include "engine/Systems.hpp"
#include "engine/World.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace engine {
class Engine;

// A World kept in step over the network (M3), with shared events (M4)
enum class Topology : uint8_t { ClientServer, PeerToPeer };
enum class Upstream : uint8_t { State, Input };

struct NetOptions {
    Topology topology = Topology::ClientServer;
    Encoding encoding = Encoding::Text;
    bool delta = false;
    Upstream upstream = Upstream::State;
    int keyframeEvery = 60;

    // Reads the flags above and leaves every other argument alone
    static bool Parse(int argc, char **argv, NetOptions &out, std::string *error = nullptr);
    std::string Describe() const;  // e.g. "cs binary delta input"

    SnapshotOptions Downstream() const;  // the authority's world
    SnapshotOptions Upward() const;  // an endpoint's own objects
};

struct LatencySummary {
    size_t samples = 0;
    double meanMs = 0, p50Ms = 0, p95Ms = 0, maxMs = 0;
};

// Counters every endpoint keeps, for HUDs and experiments
class NetStats {
public:
    std::atomic<uint64_t> messagesOut{0}, messagesIn{0};
    std::atomic<uint64_t> bytesOut{0}, bytesIn{0};
    std::atomic<uint64_t> encodeMicros{0}, decodeMicros{0};
    std::atomic<uint64_t> decoded{0};  // messages actually decoded (decodeMicros is their total)
    std::atomic<uint64_t> skipped{0};  // messages superseded by a newer keyframe before decoding
    std::atomic<uint64_t> eventPolls{0};  // actual receipt polls, independent of network tick rate
    std::atomic<uint64_t> eventsRejected{0}, eventRetries{0}, eventGaps{0};
    std::atomic<uint64_t> eventsOut{0}, eventsIn{0};  // shared events sent and received

    // One-way latency: owner's stamp to another endpoint's receipt
    void AddLatency(int64_t micros);
    LatencySummary Latency() const;
    void ResetLatency();

private:
    mutable std::mutex mutex;
    std::vector<int64_t> samples;  // the most recent, at most MAX
    size_t next = 0;
    static constexpr size_t MAX = 4096;
};

// The authoritative server
class SceneServer {
public:
    struct Callbacks {
        // A client joined: make its player (NetIdentity owner = clientId)
        std::function<void(World &, int clientId)> onJoin;
        // A client left, by BYE or by going silent
        std::function<void(World &, int clientId)> onLeave;
        // The game's tick, on the server's clock
        std::function<void(World &, float dt)> step;
        // Requests that are not snapshots, for the game to answer
        std::function<std::string(World &, int clientId, const std::string &request)> onRequest;
    };

    SceneServer(Engine &engine, Server::Config config, NetOptions options);

    // Blocks until Stop(), Ctrl-C or SIGTERM
    bool Run(const Callbacks &callbacks);
    void Stop() { server.Stop(); }

    // Guard with engine.GetWorldMutex() outside the callbacks
    World &GetWorld() { return world; }
    Server &GetServer() { return server; }
    const NetStats &Stats() const { return stats; }
    const NetOptions &Options() const { return options; }
    const SnapshotStats &LastSnapshot() const { return lastSnapshot; }

    // What this server simulates
    ObjectFilter Authority() const;

    // Objects this filter rejects are left out of the world the server broadcasts
    void SetRelayFilter(ObjectFilter filter);

    // Before Run(): retain these outgoing types for every connected client
    void ShareEvents(EventManager &manager, const std::vector<EventType> &types);
    // Before Run(): deny all client events unless explicitly allowed
    using EventValidator = std::function<bool(const World &, int, const Event &)>;
    void AllowClientEvents(const std::vector<EventType> &types, EventValidator validator = {});

private:
    void ApplyInbox();
    std::string EventRequest(int clientId, const std::string &request);

    Engine &engine;
    Server server;
    NetOptions options;
    World world;
    SnapshotEncoder downstream;
    SnapshotDecoder upstream;
    // What clients sent since the last step
    std::mutex inboxMutex;
    std::vector<std::pair<int, std::string>> inbox, applying;
    uint32_t tick = 0;
    SnapshotStats lastSnapshot;
    NetStats stats;

    std::mutex eventMutex;  // guards the three below
    EventManager *events = nullptr;
    std::set<uint32_t> shared;  // EventType ids
    struct EventStream {
        std::deque<Event> pending;
        uint64_t first = 1, offered = 0, received = 0, rejected = 0;
        bool gap = false;
    };
    std::map<int, EventStream> eventStreams;
    std::set<std::string> acceptedTypes;
    EventValidator eventValidator;

    ObjectFilter relay;  // under the world mutex
};

// One client's view of the server's World, plus its own player
class SceneClient {
public:
    SceneClient(Client::Config config, NetOptions options);

    // Main thread, before the loop
    bool Join();
    const std::string &LastError() const { return client.LastError(); }
    void Leave() { client.Leave(); }

    int Id() const { return client.Id(); }
    World &GetWorld() { return world; }  // guard with the engine's world mutex
    // This client's player, once the server has sent it
    GameObject *MyPlayer();
    // What this client simulates
    ObjectFilter Authority() const;

    // Marks this client's objects as sampled now
    void StampLocal();

    // The engine's worker thread, every iteration
    void Tick(float dt, std::mutex &worldMutex);

    // These outgoing types are queued for acknowledged delivery to the server
    void ShareEvents(EventManager &manager, const std::vector<EventType> &types);

    // A retention overflow requires reconnecting; never silently skip lost events
    bool EventStreamHealthy() const;
    std::string LastEventError() const;

    bool HasWorld() const { return hasWorld; }
    Client &GetClient() { return client; }
    const NetStats &Stats() const { return stats; }

private:
    void MeasureLatency();
    void TickEvents();

    Client client;
    NetOptions options;
    World world{1u << 30};  // local objects count from here; the server's keep their ids
    SnapshotEncoder up;
    SnapshotDecoder down;
    uint32_t tick = 0;
    std::atomic<bool> hasWorld{false};
    std::map<ObjectId, int64_t> lastStamp;
    int64_t lastSentStamp = 0;
    std::vector<std::string> inbox;  // one Tick's worth of world messages
    bool ticked = false;  // a first batch has been applied
    NetStats stats, discard;

    mutable std::mutex eventMutex;
    EventManager *events = nullptr;
    std::set<uint32_t> shared;
    std::vector<Event> eventOut;  // new arrivals, bounded separately from in-flight work
    std::vector<Event> sending;
    uint64_t sendFirst = 1, receivedUntil = 0;
    std::deque<std::pair<double, Event>> scheduled;
    double serverSample = 0, serverRate = 0;
    int64_t sampleWall = 0, nextEventPoll = 0;  // wall time; polls capped at 30 Hz
    bool eventFailed = false;
    std::string eventError;
};

// A peer: no server
class ScenePeer {
public:
    struct Config {
        Peer::Config peer;
        float sendIntervalSeconds = 1.0f / 60.0f;  // of game time
        int heartbeatMs = 500;  // wall time
        int peerTimeoutMs = 3000;  // silence that counts as gone
    };

    ScenePeer(Config config, NetOptions options);
    ~ScenePeer();

    // Build the shared level BEFORE this
    int Claim(int requested = -1);
    const std::string &LastError() const { return peer.LastError(); }

    int Id() const { return peer.Id(); }
    World &GetWorld() { return world; }
    Peer &GetPeer() { return peer; }
    // Its own objects; with inputs, every player (the others from their buttons)
    ObjectFilter Authority() const;
    void StampLocal();

    // The engine's worker thread, every iteration
    void Tick(float dt, std::mutex &worldMutex);

    // Part 1.2, distributed: handled here at once and published to every peer
    void ShareEvents(EventManager &manager, const std::vector<EventType> &types);

    int PeersHeard() const;
    const NetStats &Stats() const { return stats; }

private:
    void Send(std::mutex &worldMutex, bool fullState);
    void OnSnapshot(int sender, const std::string &snapshot, std::mutex &worldMutex);

    Config config;
    NetOptions options;
    Peer peer;
    World world;
    SnapshotEncoder up;  // per the options
    SnapshotEncoder state;  // always full state: heartbeats, and corrections in input mode
    SnapshotDecoder down;
    uint32_t tick = 0;
    float sendAccumulator = 0.0f;
    int sendsSinceState = 0;
    long long lastSendMs = 0;
    std::map<int, long long> lastHeardMs;
    std::map<ObjectId, int64_t> lastStamp;
    int64_t lastSentStamp = 0;
    mutable std::mutex heardMutex;
    NetStats stats;

    std::mutex eventMutex;  // guards the three below
    EventManager *events = nullptr;
    std::set<uint32_t> shared;
    std::vector<Event> eventOut;  // handled here, not yet published
};
}  // namespace engine
