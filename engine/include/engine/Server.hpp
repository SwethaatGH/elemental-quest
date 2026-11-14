#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace engine {
class Engine;

// Sections 2 and 4: the headless authoritative server, as an engine feature
class Server {
public:
    struct Config {
        int joinPort = 0;
        int worldPort = 0;
        int clientPortBase = 0;
        int maxClients = 4;  // 0: no limit
        int publishHz = 60;
        int tickHz = 60;  // 0 or less: uncapped (benchmarks)
        int socketTimeoutMs = 500;
        int clientTimeoutMs = 3000;  // silence that counts as a disconnect; 0: never
        int worldQueue = 32;  // world messages held per client before the oldest are dropped
        // 0: the publisher thread sends the world at publishHz
        int publishEverySteps = 0;
        const char *logTag = "server";
    };

    struct Hooks {
        // A client was admitted with this id
        std::function<void(int id)> onJoin;

        // A client left - it sent BYE, or the server is stopping
        std::function<void(int id)> onLeave;

        // A request arrived on this client's channel; return the reply
        std::function<std::string(int id, const std::string &request)> onRequest;

        // The world to broadcast
        std::function<std::string()> buildWorld;

        // Other messages to broadcast - events (Milestone 4)
        std::function<std::vector<std::string>()> buildExtra;

        // Advance the authoritative world by dt seconds of the server's game time
        std::function<void(float dt)> step;
    };

    Server(Engine &engine, Config config);

    // Blocks until Stop(), SIGINT or SIGTERM
    bool Run(const Hooks &hooks);
    void Stop();

    int ClientPort(int id) const { return config.clientPortBase + id; }
    int MaxClients() const { return config.maxClients; }
    bool IsConnected(int id) const;
    int ConnectedCount() const;
    int TimedOutCount() const { return timedOut.load(); }

private:
    void JoinLoop(const Hooks &hooks);
    void ClientLoop(const Hooks &hooks, int id);
    void PublishLoop(const Hooks &hooks);
    void Leave(const Hooks &hooks, int id);
    void ReapSilent(const Hooks &hooks);
    void Log(const char *fmt, ...) const;

    Engine &engine;
    Config config;

    std::atomic<bool> running{false};
    std::atomic<bool> bindFailed{false};
    std::vector<std::thread> clientThreads;

    // The slot table is the only state the server itself owns
    struct Slot {
        bool connected = false;
        bool served = false;  // has a thread and a bound port
        uint64_t session = 0;
        long long lastHeardMs = 0;
    };
    mutable std::mutex slotsMutex;
    std::vector<Slot> slots;
    uint64_t nextSession = 1;
    std::atomic<int> timedOut{0};
};
}  // namespace engine
