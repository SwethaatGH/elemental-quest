#include "engine/Client.hpp"

#include <chrono>
#include <sstream>

namespace engine {
namespace {
// Closing a window must not hang on a server that has already gone
constexpr int LEAVE_TIMEOUT_MS = 250;

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

Client::Client(Config cfg) : config(cfg) {
    channel.SetReceiveTimeoutMs(config.timeoutMs);
    world.SetReceiveTimeoutMs(0);  // a poll, so Tick never blocks on the broadcast
    world.SetReceiveQueue(config.worldQueue);
    world.Subscribe(config.worldTopic);
    windowStartMs = NowMs();
}

Client::~Client() {
    Leave();
}

void Client::Leave() {
    if (id < 0 || left) return;
    left = true;
    channel.SetReceiveTimeoutMs(LEAVE_TIMEOUT_MS);
    std::string reply;
    Exchange("BYE", reply);
}

// The session goes first
std::string Client::Framed(const std::string &payload) const {
    std::string framed;
    framed.reserve(payload.size() + 24);
    framed += '\x01';
    framed += std::to_string(session);
    framed += '\n';
    framed += payload;
    return framed;
}

bool Client::Exchange(const std::string &payload, std::string &reply) {
    if (!channel.Send(Framed(payload)) || !channel.Receive(reply)) {
        // A timed-out REQ socket cannot send again
        channel = net::Socket(net::SocketType::Request);
        channel.SetReceiveTimeoutMs(config.timeoutMs);
        if (channelPort > 0) channel.Connect("tcp://" + config.host + ":" + std::to_string(channelPort));
        return false;
    }
    lastExchangeMs = NowMs();
    if (reply == "GONE") {
        dropped = true;
        connected = false;
        return false;
    }
    return true;
}

bool Client::Join() {
    joinState = JoinState::Failed;  // until the end of this function
    net::Socket join(net::SocketType::Request);
    join.SetReceiveTimeoutMs(config.timeoutMs);
    if (!join.Connect("tcp://" + config.host + ":" + std::to_string(config.joinPort))) {
        lastError = join.LastError();
        return false;
    }
    join.Send("JOIN");

    std::string welcome;
    if (!join.Receive(welcome)) {
        lastError = "no answer from the server on port " + std::to_string(config.joinPort) + " - is it running?";
        return false;
    }
    if (welcome.rfind("WELCOME", 0) != 0) {
        lastError = "server refused: " + welcome;
        return false;
    }

    std::istringstream in(welcome);
    std::string verb;
    if (!(in >> verb >> id >> channelPort >> maxClients)) {
        lastError = "malformed WELCOME: " + welcome;
        return false;
    }
    in >> session;  // absent from an older server: 0, and it checks nothing
    lastExchangeMs = NowMs();

    if (!channel.Connect("tcp://" + config.host + ":" + std::to_string(channelPort))) {
        lastError = channel.LastError();
        return false;
    }
    if (!world.Connect("tcp://" + config.host + ":" + std::to_string(config.worldPort))) {
        lastError = world.LastError();
        return false;
    }
    joinState = JoinState::Joined;
    return true;
}

bool Client::Request(const std::string &request, std::string &reply) {
    return Exchange(request, reply);
}

void Client::RequestAsync(const std::string &request, ReplyHandler onReply) {
    std::string reply;
    const bool ok = Exchange(request, reply);
    if (onReply) onReply(ok, reply);
}

bool Client::Tick(float dt,
                  const std::function<void(const std::string &)> &onWorld,
                  const std::function<std::string()> &encodeState) {
    // 1. Drain the world broadcast, even while paused
    std::string message;
    while (world.Receive(message)) {
        // Hearing the world is proof of connection too
        connected = true;
        if (onWorld) onWorld(message);
    }

    // 2. Send upstream, paced by game time (remainder carried, stalls capped)
    bool sent = false;
    sendAccumulator += dt;
    if (sendAccumulator >= config.sendIntervalSeconds) {
        sendAccumulator -= config.sendIntervalSeconds;
        if (sendAccumulator > config.sendIntervalSeconds) sendAccumulator = config.sendIntervalSeconds;

        if (encodeState && !dropped) {
            const std::string payload = encodeState();
            std::string ack;
            if (Exchange(payload, ack)) {
                ++roundTrips;
                connected = true;
            } else {
                connected = false;
            }
            sent = true;
            ++sendsThisWindow;
        }
    }

    // 3. Heartbeat on wall time, so a paused client is not taken for a crashed one
    if (config.heartbeatMs > 0 && !dropped && id >= 0 && NowMs() - lastExchangeMs >= config.heartbeatMs) {
        std::string pong;
        if (Exchange("PING", pong)) ++heartbeats;
    }

    // 4. Publish the measured send rate once a wall second
    const long long now = NowMs();
    if (now - windowStartMs >= 1000) {
        sendsPerSecond = sendsThisWindow;
        sendsThisWindow = 0;
        windowStartMs = now;
    }
    return sent;
}
}  // namespace engine
