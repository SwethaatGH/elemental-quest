#include "scene_host.hpp"

#include <chrono>

using namespace engine;

namespace ed {
namespace {
bool IsSnapshot(const std::string &message) {
    return !message.empty() && (message[0] == 'T' || message[0] == 'B');
}

uint64_t MicrosSince(std::chrono::steady_clock::time_point t0) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
}
}

SceneHost::SceneHost(Engine &engineRef, Server::Config config, NetOptions opts, ObjectFilter relayFilter)
    : engine(engineRef), server(engineRef, config), options(opts), relay(std::move(relayFilter)),
      downstream(opts.Downstream()) {}

bool SceneHost::Run(const Callbacks &callbacks) {
    std::mutex &mutex = engine.GetWorldMutex();
    Server::Hooks hooks;

    hooks.onJoin = [this, &callbacks, &mutex](int id) {
        std::lock_guard<std::mutex> lock(mutex);
        if (callbacks.onJoin) callbacks.onJoin(world, id);
        downstream.ForceKeyframe();
    };

    hooks.onLeave = [this, &callbacks, &mutex](int id) {
        std::lock_guard<std::mutex> lock(mutex);
        if (callbacks.onLeave) callbacks.onLeave(world, id);
        world.DestroyIf([id](const GameObject &o) { return OwnerOf(o) == id; });
        upstream.ForgetSource(id);
    };

    hooks.onRequest = [this](int id, const std::string &request) -> std::string {
        if (!IsSnapshot(request)) return "ERR unknown request";
        ++stats.messagesIn;
        stats.bytesIn += request.size();
        std::lock_guard<std::mutex> lock(inboxMutex);
        inbox.emplace_back(id, request);
        return "ACK";
    };

    hooks.buildWorld = [this, &mutex]() {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto t0 = std::chrono::steady_clock::now();
            message = downstream.Encode(world, ++tick, relay);
            lastSnapshot = downstream.LastStats();
            keyframes += lastSnapshot.keyframe ? 1 : 0;
            stats.encodeMicros += MicrosSince(t0);
        }
        ++stats.messagesOut;
        stats.bytesOut += message.size();
        return message;
    };

    hooks.step = [this, &callbacks, &mutex](float dt) {
        std::lock_guard<std::mutex> lock(mutex);
        ApplyInbox(callbacks);
        if (callbacks.step) callbacks.step(world, dt);
        world.Flush();
    };

    return server.Run(hooks);
}

void SceneHost::ApplyInbox(const Callbacks &callbacks) {
    {
        std::lock_guard<std::mutex> lock(inboxMutex);
        applying.swap(inbox);
    }
    for (const auto &message : applying) {
        const auto t0 = std::chrono::steady_clock::now();
        ApplyOptions apply;
        apply.source = message.first;
        apply.onlyOwner = message.first;
        apply.createMissing = false;
        const ApplyResult result = upstream.Apply(world, message.second, apply);
        ++stats.decoded;
        stats.decodeMicros += MicrosSince(t0);
        if (result.ok && callbacks.onUpstream) callbacks.onUpstream(world, message.first);
    }
    applying.clear();
}
}
