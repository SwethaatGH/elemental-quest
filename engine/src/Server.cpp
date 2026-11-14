#include "engine/Server.hpp"

#include "engine/Engine.hpp"
#include "engine/Network.hpp"

#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace engine {
namespace {
// Signal handlers may only touch lock-free atomics
std::atomic<bool> g_signalled{false};
void OnSignal(int) { g_signalled = true; }

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A request from engine::Client is "\x01<session>\n<payload>"
constexpr char SESSION_MARK = '\x01';

bool SplitSession(const std::string &request, uint64_t &session, std::string &payload) {
    if (request.empty() || request[0] != SESSION_MARK) return false;
    const size_t newline = request.find('\n');
    if (newline == std::string::npos) return false;
    session = std::strtoull(request.c_str() + 1, nullptr, 10);
    payload.assign(request, newline + 1, std::string::npos);
    return true;
}
}  // namespace

Server::Server(Engine &engineRef, Config cfg)
    : engine(engineRef), config(cfg),
      slots(static_cast<size_t>(cfg.maxClients > 0 ? cfg.maxClients : 0)) {}

void Server::Log(const char *fmt, ...) const {
    printf("[%s] ", config.logTag);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

bool Server::IsConnected(int id) const {
    std::lock_guard<std::mutex> lock(slotsMutex);
    return id >= 0 && static_cast<size_t>(id) < slots.size() && slots[static_cast<size_t>(id)].connected;
}

int Server::ConnectedCount() const {
    std::lock_guard<std::mutex> lock(slotsMutex);
    int n = 0;
    for (const Slot &s : slots) if (s.connected) ++n;
    return n;
}

void Server::Stop() {
    running = false;
    engine.Stop();
}

// One of these per slot, on its own port
void Server::ClientLoop(const Hooks &hooks, int id) {
    const size_t slot = static_cast<size_t>(id);
    net::Socket socket(net::SocketType::Reply);
    socket.SetReceiveTimeoutMs(config.socketTimeoutMs);
    if (!socket.Bind("tcp://*:" + std::to_string(ClientPort(id)))) {
        Log("cannot serve client %d: %s", id, socket.LastError().c_str());
        std::lock_guard<std::mutex> lock(slotsMutex);
        slots[slot].connected = false;
        slots[slot].served = false;
        return;
    }
    Log("client %d served on port %d", id, ClientPort(id));

    std::string request, payload;
    while (running) {
        // A timeout is not an error
        if (!socket.Receive(request)) continue;

        uint64_t session = 0;
        const bool hasSession = SplitSession(request, session, payload);
        const std::string &body = hasSession ? payload : request;

        // Only the client admitted to this slot may speak in it
        bool admitted;
        {
            std::lock_guard<std::mutex> lock(slotsMutex);
            Slot &s = slots[slot];
            admitted = s.connected && (!hasSession || s.session == session);
            if (admitted) s.lastHeardMs = NowMs();
        }
        if (!admitted) {
            socket.Send(body == "BYE" ? "BYE" : "GONE");
            continue;
        }

        if (body == "BYE") {
            Leave(hooks, id);
            socket.Send("BYE");
            continue;
        }
        if (body == "PING") {  // a heartbeat: being heard was the point
            socket.Send("PONG");
            continue;
        }
        std::string reply = hooks.onRequest ? hooks.onRequest(id, body) : std::string("ACK");
        if (reply.empty()) reply = "ACK";  // REQ/REP must alternate; never leave the peer waiting
        socket.Send(reply);
    }

    Leave(hooks, id);
}

// Frees a slot once, on BYE, on a timeout or at shutdown, whichever comes first
void Server::Leave(const Hooks &hooks, int id) {
    const size_t slot = static_cast<size_t>(id);
    {
        std::lock_guard<std::mutex> lock(slotsMutex);
        if (slot >= slots.size() || !slots[slot].connected) return;
        slots[slot].session = 0;  // from here on, the old client is GONE
    }
    if (hooks.onLeave) hooks.onLeave(id);
    {
        std::lock_guard<std::mutex> lock(slotsMutex);
        slots[slot].connected = false;
    }
    Log("client %d left; slot %d is free", id, id);
}

// A client that has said nothing at all - not even a PING
void Server::ReapSilent(const Hooks &hooks) {
    if (config.clientTimeoutMs <= 0) return;
    const long long now = NowMs();
    std::vector<int> silent;
    {
        std::lock_guard<std::mutex> lock(slotsMutex);
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].connected && slots[i].session != 0 && now - slots[i].lastHeardMs > config.clientTimeoutMs) {
                silent.push_back(static_cast<int>(i));
            }
        }
    }
    for (int id : silent) {
        Log("client %d silent for over %d ms - treating it as disconnected", id, config.clientTimeoutMs);
        ++timedOut;
        Leave(hooks, id);
    }
}

// The only well-known port
void Server::JoinLoop(const Hooks &hooks) {
    net::Socket socket(net::SocketType::Reply);
    socket.SetReceiveTimeoutMs(config.socketTimeoutMs);
    if (!socket.Bind("tcp://*:" + std::to_string(config.joinPort))) {
        Log("cannot open the join channel: %s - is another server running?", socket.LastError().c_str());
        bindFailed = true;
        Stop();
        return;
    }
    Log("join channel listening on %d", config.joinPort);

    std::string request;
    while (running) {
        if (!socket.Receive(request)) continue;
        if (request.rfind("JOIN", 0) != 0) {
            socket.Send("ERR expected JOIN");
            continue;
        }

        // The lowest free slot, or a new one if there is no limit
        int assigned = -1;
        bool spawn = false;
        uint64_t session = 0;
        {
            std::lock_guard<std::mutex> lock(slotsMutex);
            for (size_t i = 0; i < slots.size(); ++i) {
                if (!slots[i].connected) { assigned = static_cast<int>(i); break; }
            }
            if (assigned < 0 && config.maxClients <= 0) {
                slots.emplace_back();
                assigned = static_cast<int>(slots.size()) - 1;
            }
            if (assigned >= 0) {
                Slot &s = slots[static_cast<size_t>(assigned)];
                s.connected = true;
                spawn = !s.served;
                s.served = true;
                s.session = session = nextSession++;
                s.lastHeardMs = NowMs();
            }
        }
        if (assigned < 0) {
            socket.Send("ERR server full");
            continue;
        }

        if (hooks.onJoin) hooks.onJoin(assigned);
        if (spawn) clientThreads.emplace_back(&Server::ClientLoop, this, std::cref(hooks), assigned);

        socket.Send("WELCOME " + std::to_string(assigned) + ' ' +
                    std::to_string(ClientPort(assigned)) + ' ' +
                    std::to_string(config.maxClients) + ' ' +
                    std::to_string(session));
        Log("client %d joined", assigned);
    }
}

// Broadcasts the authoritative world to everyone at a fixed rate
void Server::PublishLoop(const Hooks &hooks) {
    net::Socket socket(net::SocketType::Publish);
    const bool inlinePublish = config.publishEverySteps > 0;
    if (!inlinePublish) {
        socket.SetSendQueue(config.worldQueue);
        if (!socket.Bind("tcp://*:" + std::to_string(config.worldPort))) {
            Log("cannot open the world broadcast: %s", socket.LastError().c_str());
            bindFailed = true;
            Stop();
            return;
        }
        Log("world broadcast on %d", config.worldPort);
    }

    const auto period = std::chrono::microseconds(1000000 / (config.publishHz > 0 ? config.publishHz : 60));
    auto next = std::chrono::steady_clock::now();
    while (running) {
        if (!inlinePublish && hooks.buildExtra) {
            for (const std::string &message : hooks.buildExtra()) socket.Send(message);
        }
        if (!inlinePublish && hooks.buildWorld) socket.Send(hooks.buildWorld());
        ReapSilent(hooks);
        next += period;  // fixed send times, as in Engine::RunHeadless
        const auto now = std::chrono::steady_clock::now();
        if (now - next > period) next = now;
        else std::this_thread::sleep_until(next);
    }
}

bool Server::Run(const Hooks &hooks) {
    running = true;
    bindFailed = false;
    timedOut = 0;
    g_signalled = false;
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    // With publishEverySteps the world socket belongs to the main thread
    net::Socket inlineWorld(net::SocketType::Publish);
    if (config.publishEverySteps > 0) {
        inlineWorld.SetSendQueue(config.worldQueue);
        if (!inlineWorld.Bind("tcp://*:" + std::to_string(config.worldPort))) {
            Log("cannot open the world broadcast: %s", inlineWorld.LastError().c_str());
            return false;
        }
        Log("world broadcast on %d, every %d step(s)", config.worldPort, config.publishEverySteps);
    }

    std::thread joinThread(&Server::JoinLoop, this, std::cref(hooks));
    std::thread publishThread(&Server::PublishLoop, this, std::cref(hooks));
    Log("running - Ctrl-C to stop");

    long long steps = 0;
    engine.RunHeadless(
        [this, &hooks, &inlineWorld, &steps](float dt) {
            if (hooks.step) hooks.step(dt);
            if (config.publishEverySteps > 0 && hooks.buildExtra) {
                for (const std::string &message : hooks.buildExtra()) inlineWorld.Send(message);
            }
            if (config.publishEverySteps > 0 && ++steps % config.publishEverySteps == 0 && hooks.buildWorld) {
                inlineWorld.Send(hooks.buildWorld());
            }
            if (g_signalled) Stop();
        },
        config.tickHz);

    running = false;
    joinThread.join();
    publishThread.join();
    for (std::thread &t : clientThreads) {
        if (t.joinable()) t.join();
    }
    clientThreads.clear();
    Log("stopped cleanly");
    return !bindFailed;
}
}  // namespace engine
