#pragma once

#include "engine/Network.hpp"
#include "engine/Timeline.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace engine {
// Section 5: peer-to-peer networking, as an engine feature
class Peer {
public:
    struct Config {
        int portBase = 0;
        int maxPeers = 4;
        int gossipMs = 250;
        std::vector<std::string> topics;  // game topics to subscribe to, besides the engine's own
    };

    explicit Peer(Config config);

    // Bind this peer's publisher and so claim an id
    int Claim(int requested = -1);
    void ConnectAll();

    int Id() const { return id; }
    int MaxPeers() const { return config.maxPeers; }
    const std::string &LastError() const { return lastError; }

    // --- shared clock --------------------------------------------------------
    void EnableSharedClock();
    Timeline &SharedClock() { return sharedClock; }
    double SharedSeconds();  // convenience: SharedClock().GetSeconds()
    int64_t Epoch() const;
    int EpochProposer() const;
    int EpochAdoptions() const;
    bool HeardFrom(int peerId) const;  // any EPOCH from this peer yet
    bool AgreesOnEpoch(int peerId) const;  // its last EPOCH equals mine

    // --- event log -------------------------------------------------------------
    struct Event {
        int owner = -1;
        int64_t seq = 0;
        std::string payload;
    };
    // Any thread: stamps a seq; sent on the next gossip
    void AppendEvent(const std::string &payload);

    // This process's first seq: its start time in microseconds
    int64_t FirstSeq() const { return firstSeq; }
    // True once after new events arrived (from anyone), then false until more do
    bool TakeEventsDirty();
    std::vector<Event> Events() const;  // snapshot of everything known
    size_t EventCount() const;

    // --- talking ---
    // Worker thread: sent to every other peer at once
    void Publish(const std::string &topic, const std::string &payload);

    // Worker thread, once per iteration
    void Tick(const std::function<void(const std::string &topic, std::istringstream &body, int fromId)> &onMessage,
              const std::function<std::string()> &epochExtras = nullptr,
              const std::function<std::vector<std::string>()> &extraGossip = nullptr);

    int MessagesHeard() const { return messagesHeard; }

private:
    void OnEpoch(std::istringstream &in, const std::function<void(const std::string &, std::istringstream &, int)> &onMessage);
    void OnEvents(std::istringstream &in);
    std::vector<std::string> EncodeKnownEvents() const;

    Config config;
    int id = -1;
    std::string lastError;

    net::Socket pub{net::SocketType::Publish};
    net::Socket sub{net::SocketType::Subscribe};

    // Guards everything below that main and worker both touch
    mutable std::mutex mutex;

    bool clockEnabled = false;
    Timeline sharedClock{nullptr, 1};
    int64_t epoch = 0;
    int epochProposer = -1;
    int epochAdoptions = 0;
    std::vector<int64_t> peerEpoch;
    std::vector<bool> peerHeard;

    std::vector<Event> events;
    std::set<std::pair<int, int64_t>> eventIds;
    const int64_t firstSeq = Timeline::RealTimeNow();
    int64_t nextSeq = firstSeq;
    bool eventsDirty = false;

    long long lastGossipMs = 0;
    std::atomic<int> messagesHeard{0};
};
}  // namespace engine
