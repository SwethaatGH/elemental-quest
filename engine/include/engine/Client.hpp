#pragma once

#ifndef __EMSCRIPTEN__
#include "engine/Network.hpp"
#endif

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace engine {
// Client side of client-server networking (Sections 2 and 4)
//
// Natively it speaks ZeroMQ straight to the server. In a browser (Emscripten) it
// speaks one WebSocket to a gateway that relays the same messages to the server,
// and requests never block: their replies arrive in a later Tick.
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
        // Browser builds only: the gateway's WebSocket URL, room included
        std::string webSocketUrl;
    };

    // Called with the reply, or ok = false if the request failed or timed out
    using ReplyHandler = std::function<void(bool ok, const std::string &reply)>;

    enum class JoinState { Idle, Connecting, Joined, Failed };

    explicit Client(Config config);
    ~Client();

    // Main thread, before the loop. Natively this blocks until the server answers.
    // In a browser it only starts connecting: watch GetJoinState() from the main loop
    bool Join();
    JoinState GetJoinState() const { return joinState; }

    // After the loop
    void Leave();

    int Id() const { return id; }
    int ChannelPort() const { return channelPort; }
    int MaxClients() const { return maxClients; }
    const std::string &LastError() const { return lastError; }

    // One request, one reply, on this client's own channel (native builds only)
    bool Request(const std::string &request, std::string &reply);

    // The same, without waiting: natively the handler runs before this returns,
    // in a browser it runs in a later Tick. Replies keep the order of the requests
    void RequestAsync(const std::string &request, ReplyHandler onReply);

    // One worker iteration
    bool Tick(float dt,
              const std::function<void(const std::string &world)> &onWorld,
              const std::function<std::string()> &encodeState);

    // Readable from any thread, for the HUD
    bool Connected() const { return connected; }
    int RoundTrips() const { return roundTrips; }
    int SendsPerSecond() const { return sendsPerSecond; }
    // The server answered GONE (or, in a browser, the connection closed)
    bool Dropped() const { return dropped; }
    int Heartbeats() const { return heartbeats; }

private:
    // Every channel exchange goes through here: it adds the session and notices a GONE
    bool Exchange(const std::string &payload, std::string &reply);
    std::string Framed(const std::string &payload) const;

    Config config;
    int id = -1;
    uint64_t session = 0;
    int channelPort = 0;
    int maxClients = 0;
    bool left = false;
    std::string lastError;
    JoinState joinState = JoinState::Idle;

#ifdef __EMSCRIPTEN__
public:
    // WebSocket callbacks land here; they run on the main thread between frames
    void OnSocketMessage(const char *data, size_t size);
    void OnSocketClosed(const std::string &why);

private:
    bool HandleWelcome(const std::string &body);
    void DeliverReply(bool ok, const std::string &reply);
    bool SendFrame(char kind, const std::string &body);

    int socket = 0;  // EMSCRIPTEN_WEBSOCKET_T
    bool open = false;
    std::deque<std::string> worldInbox;  // newest kept, oldest dropped past worldQueue
    std::deque<std::string> replies;  // 'A' or 'X' frames, in request order
    std::deque<ReplyHandler> waiting;  // one per request in flight
#else
    net::Socket channel{net::SocketType::Request};
    net::Socket world{net::SocketType::Subscribe};
#endif

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
