#pragma once

#include "engine/Network.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace engine {
// Client side of client-server networking (Sections 2 and 4)
class Client {
public:
    struct Config {
        int joinPort = 0;
        int worldPort = 0;
        const char *worldTopic = "WORLD";
        float sendIntervalSeconds = 1.0f / 60.0f;  // of game time
        int timeoutMs = 2000;
        // Wall time with nothing sent upstream before an idle client sends a PING
        int heartbeatMs = 1000;
        std::string host = "localhost";  // hostname or IPv4 address of the server
        int worldQueue = 32;  // world messages held before the oldest are dropped
    };

    explicit Client(Config config);
    ~Client();

    // Main thread, before the loop
    bool Join();

    // After the loop
    void Leave();

    int Id() const { return id; }
    int ChannelPort() const { return channelPort; }
    int MaxClients() const { return maxClients; }
    const std::string &LastError() const { return lastError; }

    // One request, one reply, on this client's own channel
    bool Request(const std::string &request, std::string &reply);

    // One worker iteration
    bool Tick(float dt,
              const std::function<void(const std::string &world)> &onWorld,
              const std::function<std::string()> &encodeState);

    // Readable from any thread, for the HUD
    bool Connected() const { return connected; }
    int RoundTrips() const { return roundTrips; }
    int SendsPerSecond() const { return sendsPerSecond; }
    // The server answered GONE
    bool Dropped() const { return dropped; }
    int Heartbeats() const { return heartbeats; }

private:
    // Every channel exchange goes through here: it adds the session and notices a GONE
    bool Exchange(const std::string &payload, std::string &reply);

    Config config;
    int id = -1;
    uint64_t session = 0;
    int channelPort = 0;
    int maxClients = 0;
    bool left = false;
    std::string lastError;

    net::Socket channel{net::SocketType::Request};
    net::Socket world{net::SocketType::Subscribe};

    float sendAccumulator = 0.0f;
    int sendsThisWindow = 0;
    long long windowStartMs = 0;
    long long lastExchangeMs = 0;

    std::atomic<bool> connected{false};
    std::atomic<int> roundTrips{0};
    std::atomic<int> sendsPerSecond{0};
    std::atomic<bool> dropped{false};
    std::atomic<int> heartbeats{0};
};
}  // namespace engine
