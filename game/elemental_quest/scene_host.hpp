#pragma once

#include "engine/Engine.hpp"
#include "engine/NetScene.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ed {
class SceneHost {
public:
    struct Callbacks {
        std::function<void(engine::World &, int clientId)> onJoin;
        std::function<void(engine::World &, int clientId)> onLeave;
        std::function<void(engine::World &, int clientId)> onUpstream;
        std::function<void(engine::World &, float dt)> step;
    };

    SceneHost(engine::Engine &engine, engine::Server::Config config, engine::NetOptions options,
              engine::ObjectFilter relay = {});

    bool Run(const Callbacks &callbacks);
    void Stop() { server.Stop(); }

    engine::World &GetWorld() { return world; }
    engine::Server &GetServer() { return server; }
    const engine::NetStats &Stats() const { return stats; }
    const engine::NetOptions &Options() const { return options; }
    const engine::SnapshotStats &LastSnapshot() const { return lastSnapshot; }
    uint64_t Keyframes() const { return keyframes; }

    engine::ObjectFilter Authority() const { return engine::OwnedBy(-1); }

private:
    void ApplyInbox(const Callbacks &callbacks);

    engine::Engine &engine;
    engine::Server server;
    engine::NetOptions options;
    engine::ObjectFilter relay;
    engine::World world;
    engine::SnapshotEncoder downstream;
    engine::SnapshotDecoder upstream;
    std::mutex inboxMutex;
    std::vector<std::pair<int, std::string>> inbox, applying;
    uint32_t tick = 0;
    uint64_t keyframes = 0;
    engine::SnapshotStats lastSnapshot;
    engine::NetStats stats;
};
}
