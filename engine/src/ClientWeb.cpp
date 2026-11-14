// engine::Client for browsers (Emscripten builds only).
//
// One WebSocket to the gateway (server/gateway), which joins the room's server on
// the browser's behalf and relays the same messages the native client sends over
// ZeroMQ. Every frame starts with one byte saying what it is:
//
//   browser -> gateway   'Q' request      (the session-framed payload, as natively)
//   gateway -> browser   'W' welcome      ("WELCOME id channelPort maxClients session")
//                        'E' error        (the join was refused; the socket then closes)
//                        'A' reply        (one per 'Q', in the same order)
//                        'X' failed       (in place of an 'A': the request timed out)
//                        'S' world        (a snapshot or event broadcast)
//
// Nothing here blocks: requests are queued and their replies handled in a later Tick.
#include "engine/Client.hpp"

#include <emscripten/websocket.h>

#include <chrono>
#include <sstream>

namespace engine {
namespace {
// Requests in flight before state sends are skipped (a slow link must not pile up)
constexpr size_t MAX_IN_FLIGHT = 90;
// Silence from the gateway that counts as a lost connection, for the HUD
constexpr long long QUIET_MS = 3000;

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

long long g_lastHeardMs = 0;

EM_BOOL OnMessage(int, const EmscriptenWebSocketMessageEvent *e, void *user) {
    static_cast<Client *>(user)->OnSocketMessage(reinterpret_cast<const char *>(e->data), e->numBytes);
    return EM_TRUE;
}

EM_BOOL OnClose(int, const EmscriptenWebSocketCloseEvent *e, void *user) {
    std::string why = e->reason[0] ? std::string(e->reason) : "connection closed (code " + std::to_string(e->code) + ")";
    static_cast<Client *>(user)->OnSocketClosed(why);
    return EM_TRUE;
}

EM_BOOL OnError(int, const EmscriptenWebSocketErrorEvent *, void *user) {
    static_cast<Client *>(user)->OnSocketClosed("could not reach the game server");
    return EM_TRUE;
}

EM_BOOL OnOpen(int, const EmscriptenWebSocketOpenEvent *, void *) {
    g_lastHeardMs = NowMs();
    return EM_TRUE;
}
}  // namespace

Client::Client(Config cfg) : config(cfg) {
    windowStartMs = NowMs();
}

Client::~Client() {
    Leave();
    if (socket > 0) emscripten_websocket_delete(socket);
}

std::string Client::Framed(const std::string &payload) const {
    std::string framed;
    framed.reserve(payload.size() + 24);
    framed += '\x01';
    framed += std::to_string(session);
    framed += '\n';
    framed += payload;
    return framed;
}

bool Client::Join() {
    if (!emscripten_websocket_is_supported()) {
        lastError = "this browser does not support WebSockets";
        joinState = JoinState::Failed;
        return false;
    }
    EmscriptenWebSocketCreateAttributes attributes;
    emscripten_websocket_init_create_attributes(&attributes);
    attributes.url = config.webSocketUrl.c_str();
    attributes.createOnMainThread = EM_TRUE;
    const EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&attributes);
    if (ws <= 0) {
        lastError = "could not open " + config.webSocketUrl;
        joinState = JoinState::Failed;
        return false;
    }
    socket = ws;
    open = true;
    joinState = JoinState::Connecting;
    emscripten_websocket_set_onopen_callback(ws, this, OnOpen);
    emscripten_websocket_set_onmessage_callback(ws, this, OnMessage);
    emscripten_websocket_set_onclose_callback(ws, this, OnClose);
    emscripten_websocket_set_onerror_callback(ws, this, OnError);
    return true;
}

bool Client::HandleWelcome(const std::string &body) {
    std::istringstream in(body);
    std::string verb;
    if (!(in >> verb >> id >> channelPort >> maxClients) || verb != "WELCOME") {
        lastError = "malformed WELCOME: " + body;
        return false;
    }
    in >> session;
    lastExchangeMs = NowMs();
    return true;
}

void Client::OnSocketMessage(const char *data, size_t size) {
    if (size == 0) return;
    g_lastHeardMs = NowMs();
    const char kind = data[0];
    std::string body(data + 1, size - 1);
    switch (kind) {
    case 'W':
        if (joinState == JoinState::Connecting) {
            joinState = HandleWelcome(body) ? JoinState::Joined : JoinState::Failed;
            connected = joinState == JoinState::Joined;
        }
        break;
    case 'E':
        lastError = body;
        if (joinState == JoinState::Connecting) joinState = JoinState::Failed;
        break;
    case 'S':
        worldInbox.push_back(std::move(body));
        while (worldInbox.size() > static_cast<size_t>(config.worldQueue)) worldInbox.pop_front();
        break;
    case 'A':
    case 'X':
        replies.push_back(std::string(1, kind) + body);
        break;
    default:
        break;
    }
}

void Client::OnSocketClosed(const std::string &why) {
    if (!open) return;
    open = false;
    connected = false;
    if (joinState == JoinState::Connecting) {
        if (lastError.empty()) lastError = why;
        joinState = JoinState::Failed;
    } else if (!left) {
        dropped = true;
        if (lastError.empty()) lastError = why;
    }
    // Nothing more will answer the requests still in flight
    while (!waiting.empty()) {
        ReplyHandler handler = std::move(waiting.front());
        waiting.pop_front();
        if (handler) handler(false, "");
    }
}

bool Client::SendFrame(char kind, const std::string &body) {
    if (!open || socket <= 0) return false;
    std::string frame;
    frame.reserve(body.size() + 1);
    frame += kind;
    frame += body;
    return emscripten_websocket_send_binary(socket, frame.data(), static_cast<uint32_t>(frame.size())) ==
           EMSCRIPTEN_RESULT_SUCCESS;
}

void Client::Leave() {
    if (left) return;
    left = true;
    // Closing the socket is the goodbye: the gateway sends BYE to the server for us
    if (open && socket > 0) emscripten_websocket_close(socket, 1000, "bye");
    open = false;
}

bool Client::Request(const std::string &, std::string &) {
    lastError = "blocking requests are not available in a browser";
    return false;
}

void Client::RequestAsync(const std::string &request, ReplyHandler onReply) {
    if (dropped || joinState != JoinState::Joined || !SendFrame('Q', Framed(request))) {
        if (onReply) onReply(false, "");
        return;
    }
    waiting.push_back(std::move(onReply));
    lastExchangeMs = NowMs();
}

void Client::DeliverReply(bool ok, const std::string &reply) {
    if (waiting.empty()) return;  // a reply nobody asked for: ignore it
    ReplyHandler handler = std::move(waiting.front());
    waiting.pop_front();
    if (ok && reply == "GONE") {
        dropped = true;
        connected = false;
        ok = false;
    }
    if (handler) handler(ok, reply);
}

bool Client::Tick(float dt,
                  const std::function<void(const std::string &)> &onWorld,
                  const std::function<std::string()> &encodeState) {
    // 1. Replies, in the order the requests went out
    while (!replies.empty()) {
        const std::string frame = std::move(replies.front());
        replies.pop_front();
        DeliverReply(frame[0] == 'A', frame.substr(1));
    }

    // 2. The world broadcast
    while (!worldInbox.empty()) {
        const std::string message = std::move(worldInbox.front());
        worldInbox.pop_front();
        if (onWorld) onWorld(message);
    }
    const long long now = NowMs();
    connected = open && joinState == JoinState::Joined && !dropped && now - g_lastHeardMs < QUIET_MS;

    // 3. Send upstream, paced by game time; a send never waits for its ACK
    bool sent = false;
    sendAccumulator += dt;
    if (sendAccumulator >= config.sendIntervalSeconds) {
        sendAccumulator -= config.sendIntervalSeconds;
        if (sendAccumulator > config.sendIntervalSeconds) sendAccumulator = config.sendIntervalSeconds;
        if (encodeState && open && !dropped && joinState == JoinState::Joined && waiting.size() < MAX_IN_FLIGHT) {
            RequestAsync(encodeState(), [this](bool ok, const std::string &) {
                if (ok) ++roundTrips;
            });
            sent = true;
            ++sendsThisWindow;
        }
    }

    // 4. Heartbeat on wall time, so a paused client is not taken for a crashed one
    if (config.heartbeatMs > 0 && open && !dropped && joinState == JoinState::Joined &&
        now - lastExchangeMs >= config.heartbeatMs) {
        RequestAsync("PING", [this](bool ok, const std::string &) {
            if (ok) ++heartbeats;
        });
    }

    // 5. Publish the measured send rate once a wall second
    if (now - windowStartMs >= 1000) {
        sendsPerSecond = sendsThisWindow;
        sendsThisWindow = 0;
        windowStartMs = now;
    }
    return sent;
}
}  // namespace engine
