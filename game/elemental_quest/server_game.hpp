#pragma once

#include "game_events.hpp"
#include "level.hpp"

#include "engine/EventManager.hpp"
#include "engine/EventSystems.hpp"
#include "engine/JobSystem.hpp"
#include "engine/NetScene.hpp"
#include "engine/Systems.hpp"
#include "engine/Timeline.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace ed {
// Authoritative game logic: collisions, client requests and timers as events, handled in Step()
class ServerGame {
public:
    ServerGame(engine::JobSystem &jobs, const engine::Timeline &clock, bool log = true);

    // For the [net] log line (optional)
    void SetNet(const engine::NetStats *stats, const engine::NetOptions *options) {
        netStats_ = stats;
        netOptions_ = options;
    }
    void SetGateCycle(int steps) { gateCycle_ = steps; }
    void SetEventLog(bool on);

    // Call before server.Run(): share results, allow and validate requests
    void Share(engine::SceneServer &server);

    engine::EventManager &Events() { return events_; }
    uint64_t CollisionsRaised() const { return collisionsRaised_; }

    void OnJoin(engine::World &world, int id);
    void OnLeave(engine::World &world, int id);
    void Step(engine::World &world, float dt);

private:
    struct TakenGem {
        float x, y;
    };

    bool IsActive(int id) const;
    Match *FindMatch(engine::World &world);
    engine::GameObject *PlayerOf(engine::World &world, int element);
    bool Validate(const engine::World &world, int source, const engine::Event &e);
    void RegisterHandlers();

    void TrackPlayers(engine::World &world);
    void MoveScene(engine::World &world, float dt);
    void ApplyTriggers(engine::World &world, bool &platePressed, int &presser);
    void UpdateGate(engine::World &world, bool platePressed, int presser);
    void SetGate(engine::World &world, bool open);
    void CheckRound(engine::World &world);
    void StartRound(engine::World &world);
    void Log(engine::World &world);

    void OnCollision(const engine::Event &e);
    void OnBurstRequest(const engine::Event &e);
    void OnBurstEnd(const engine::Event &e);
    void OnStomp(const engine::Event &e);
    void OnDeathReport(const engine::Event &e);
    void OnTempleRequest(const engine::Event &e);
    void OnSpawn(const engine::Event &e);

    engine::JobSystem &jobs_;
    bool log_;
    const engine::NetStats *netStats_ = nullptr;
    const engine::NetOptions *netOptions_ = nullptr;
    int gateCycle_ = 0;

    engine::EventManager events_;
    engine::CollisionEvents collisions_{events_};
    engine::World *world_ = nullptr;  // the world being stepped: handlers run inside Step()
    uint64_t collisionsRaised_ = 0;

    double time_ = 0;
    long long steps_ = 0;
    engine::ObjectId matchId_ = engine::NO_OBJECT;
    engine::TriggerTracker triggers_;
    double roundStartedAt_ = 0;

    bool gateOpen_ = false;
    bool closeScheduled_ = false;
    int gateGeneration_ = 0;

    std::array<bool, kMaxPlayers> joined_{};
    std::array<int64_t, kMaxPlayers> lastHeardUs_{};
    std::array<int64_t, kMaxPlayers> lastStamp_{};
    std::array<int, kMaxPlayers> upstreamThisWindow_{};
    std::array<int, kMaxPlayers> lastCheckpoint_{};
    std::array<double, kMaxPlayers> lastBurst_{};
    std::array<bool, kMaxPlayers> templeAnnounced_{};
    std::array<std::vector<TakenGem>, kMaxPlayers> sinceCheckpoint_;  // gems a death puts back
    std::array<int, kEnemies> killedBy_{};  // -1 alive, -2 coming back, else element

    int burstSeq_ = 0;
    std::map<engine::ObjectId, int> frozenMovers_;  // mover -> the WATER burst holding it
    std::map<engine::ObjectId, double> moverDelay_;  // mover -> seconds it has spent frozen
    std::map<int, engine::ObjectId> bridges_;  // EARTH burst id -> bridge

    double lastLog_ = 0;
    uint64_t snapshotsAtLog_ = 0, bytesAtLog_ = 0;
    uint64_t raisedAtLog_ = 0, handledAtLog_ = 0, inAtLog_ = 0, outAtLog_ = 0, rejectedAtLog_ = 0;
    int stepsThisWindow_ = 0, pooledThisWindow_ = 0, splitThisWindow_ = 0;
    int moverThread_ = -1, enemyThread_ = -1;
};
}
