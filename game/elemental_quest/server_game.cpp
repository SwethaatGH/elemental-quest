#include "server_game.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace engine;

namespace ed {
namespace {
int ThreadTag() {
    static std::atomic<int> next{0};
    thread_local const int tag = next.fetch_add(1);
    return tag;
}

int ElementOf(const GameObject &o) {
    const Player *p = o.Get<Player>();
    return p ? p->element : -1;
}

float Distance(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }
}

ServerGame::ServerGame(JobSystem &jobs, const Timeline &clock, bool log)
    : jobs_(jobs), log_(log), events_(&clock) {
    killedBy_.fill(-1);
    lastBurst_.fill(-1e9);
    RegisterHandlers();
}

void ServerGame::SetEventLog(bool on) {
    if (on) events_.SetLog([](const std::string &line) { SDL_Log("[event] server %s", line.c_str()); });
    else events_.SetLog({});
}

void ServerGame::Share(SceneServer &server) {
    server.ShareEvents(events_, ServerSharedTypes());
    server.AllowClientEvents(ClientSharedTypes(),
                             [this](const World &w, int source, const Event &e) { return Validate(w, source, e); });
}

// Validator: a client may only act for its own element, near its own player
bool ServerGame::Validate(const World &world, int source, const Event &e) {
    if (source < 0 || source >= kMaxPlayers || !joined_[source]) return false;
    if (!e.Has("element") || e.GetInt("element", -1) != source) return false;
    const GameObject *me = world.Find(PlayerObjectId(source));
    if (!me) return false;
    const Transform &t = *me->Get<Transform>();
    if (e.Is(ev::BURST_REQUEST)) {
        if (me->Get<Player>()->dead || time_ - lastBurst_[source] < kBurstCooldown - 0.25) return false;
        lastBurst_[source] = time_;
        return true;
    }
    if (e.Is(ev::STOMP_REQUEST)) {
        const GameObject *enemy = world.Find(e.GetObject("enemy"));
        return enemy && enemy->Has<Enemy>() && Distance(enemy->Get<Transform>()->Center(), t.Center()) < 220.0f;
    }
    if (e.Is(ev::DEATH_REPORT)) return true;
    if (e.Is(ev::TEMPLE_REQUEST)) {
        bool near = false;
        world.Each<Door, Transform>([&](const GameObject &, const Door &d, const Transform &dt) {
            near |= d.element == source && std::fabs(dt.Center().x - t.Center().x) < 120.0f;
        });
        return near;
    }
    return false;
}

void ServerGame::RegisterHandlers() {
    events_.Register(events::COLLISION, [this](const Event &e) { OnCollision(e); });
    events_.Register(ev::BURST_REQUEST, [this](const Event &e) { OnBurstRequest(e); });
    events_.Register(ev::BURST_END, [this](const Event &e) { OnBurstEnd(e); });
    events_.Register(ev::STOMP_REQUEST, [this](const Event &e) { OnStomp(e); });
    events_.Register(ev::DEATH_REPORT, [this](const Event &e) { OnDeathReport(e); });
    events_.Register(ev::TEMPLE_REQUEST, [this](const Event &e) { OnTempleRequest(e); });
    events_.Register(events::SPAWN, [this](const Event &e) { OnSpawn(e); });
    events_.Register(ev::GATE_TIMER, [this](const Event &e) {
        if (!world_ || e.GetInt("generation") != gateGeneration_ || !gateOpen_) return;
        closeScheduled_ = false;
        SetGate(*world_, false);
        events_.Raise(Event(ev::GATE_CLOSED, PRIORITY_LOW));
    });
}

bool ServerGame::IsActive(int id) const {
    return id >= 0 && id < kMaxPlayers && joined_[id] && lastHeardUs_[id] > 0 &&
           Timeline::RealTimeNow() - lastHeardUs_[id] <
               static_cast<int64_t>(kActiveTimeoutSeconds * Timeline::MICROSECONDS_PER_SECOND);
}

Match *ServerGame::FindMatch(World &world) {
    GameObject *o = world.Find(matchId_);
    if (!o) {
        world.Each<Match>([&](GameObject &m, Match &) { if (!o) o = &m; });
        matchId_ = o ? o->Id() : NO_OBJECT;
    }
    return o ? o->Get<Match>() : nullptr;
}

GameObject *ServerGame::PlayerOf(World &world, int element) {
    return element >= 0 && element < kMaxPlayers ? world.Find(PlayerObjectId(element)) : nullptr;
}

void ServerGame::OnJoin(World &world, int id) {
    MakePlayer(world, id);
    if (id >= 0 && id < kMaxPlayers) {
        joined_[id] = true;
        lastHeardUs_[id] = lastStamp_[id] = 0;
        upstreamThisWindow_[id] = 0;
        lastBurst_[id] = -1e9;
        sinceCheckpoint_[id].clear();
    }
    if (log_) SDL_Log("[game] player %d joined as %s", id, ElementName(id));
}

void ServerGame::OnLeave(World &, int id) {
    if (id >= 0 && id < kMaxPlayers) joined_[id] = false;
    if (log_) SDL_Log("[game] player %d (%s) left - its player is removed from every client", id, ElementName(id));
}

void ServerGame::Step(World &world, float dt) {
    world_ = &world;
    time_ += dt;
    ++steps_;

    TrackPlayers(world);
    MoveScene(world, dt);

    const PhysicsSettings physics{kGravity, kMaxFallSpeed, 1.0f / 120.0f};
    systems::StepPhysics(world, dt, physics, OwnedBy(-1));

    bool platePressed = false;
    int presser = -1;
    ApplyTriggers(world, platePressed, presser);
    UpdateGate(world, platePressed, presser);

    // Handle this tick's collisions, client requests and timers
    events_.Handle();

    CheckRound(world);

    if (Match *m = FindMatch(world)) {
        m->serverTime = static_cast<float>(time_);
        m->gateOpen = gateOpen_;
        m->present = m->active = 0;
        for (int id = 0; id < kMaxPlayers; ++id) {
            if (joined_[id]) m->present |= 1u << id;
            if (IsActive(id)) m->active |= 1u << id;
        }
    }

    if (log_ && time_ - lastLog_ >= 5.0) Log(world);
}

// A client is active while its player's stamp moves; a crossed checkpoint banks its gems
void ServerGame::TrackPlayers(World &world) {
    world.Each<Player, NetIdentity>([&](GameObject &, Player &p, NetIdentity &net) {
        const int id = net.owner;
        if (id < 0 || id >= kMaxPlayers) return;
        if (net.stampMicros != lastStamp_[id]) {
            lastStamp_[id] = net.stampMicros;
            lastHeardUs_[id] = Timeline::RealTimeNow();
            ++upstreamThisWindow_[id];
        }
        if (p.checkpoint != lastCheckpoint_[id]) {
            lastCheckpoint_[id] = p.checkpoint;
            sinceCheckpoint_[id].clear();
        }
    });
}

void ServerGame::MoveScene(World &world, float dt) {
    struct Move {
        Transform *t;
        const PathMover *p;
        double at;
    };
    std::vector<Move> movers, enemies;
    world.Each<Transform, PathMover>([&](GameObject &o, Transform &t, PathMover &p) {
        // A platform frozen by WATER keeps its clock stopped, so it resumes in place
        double &delay = moverDelay_[o.Id()];
        if (frozenMovers_.count(o.Id())) delay += dt;
        (o.Has<Enemy>() ? enemies : movers).push_back({&t, &p, time_ - delay});
    });
    jobs_.Submit([&movers, this] {
        moverThread_ = ThreadTag();
        for (auto &m : movers) m.t->position = m.p->PositionAt(m.at);
    });
    jobs_.Submit([&enemies, this] {
        enemyThread_ = ThreadTag();
        for (auto &e : enemies) e.t->position = e.p->PositionAt(e.at);
    });
    jobs_.RunAndWait();
    const int self = ThreadTag();
    ++stepsThisWindow_;
    pooledThisWindow_ += (moverThread_ != self) + (enemyThread_ != self);
    splitThisWindow_ += moverThread_ != enemyThread_;
}

// Trigger enters become collision events; a plate stays pressed while occupied
void ServerGame::ApplyTriggers(World &world, bool &platePressed, int &presser) {
    const auto livePlayer = [this](const GameObject &o) {
        const Player *p = o.Get<Player>();
        return o.Kind() == "player" && p && !p->dead && IsActive(OwnerOf(o));
    };
    const std::vector<TriggerEvent> found = triggers_.Update(world, livePlayer);
    collisionsRaised_ += collisions_.Update(found);
    std::vector<ObjectId> pressed;
    for (const TriggerEvent &e : found) {
        if (e.type == TriggerEvent::Type::Exit) continue;
        const GameObject *trigger = world.Find(e.trigger);
        const GameObject *player = world.Find(e.other);
        if (trigger && player && trigger->Has<Plate>()) {
            pressed.push_back(e.trigger);
            if (presser < 0) presser = ElementOf(*player);
        }
    }
    world.Each<Plate, Renderable>([&](GameObject &o, Plate &plate, Renderable &r) {
        plate.pressed = std::find(pressed.begin(), pressed.end(), o.Id()) != pressed.end();
        r.color = plate.pressed ? SDL_Color{120, 230, 130, 255} : SDL_Color{255, 215, 90, 255};
        platePressed |= plate.pressed;
    });
}

// Gate closes 2.5 s after the last release; a newer press invalidates the old timer
void ServerGame::UpdateGate(World &world, bool platePressed, int presser) {
    if (gateCycle_ > 0) {  // benchmark: fixed open/close cycle, no events
        SetGate(world, (steps_ / gateCycle_) % 2 == 1);
        return;
    }
    if (platePressed) {
        if (closeScheduled_) {
            ++gateGeneration_;
            closeScheduled_ = false;
        }
        if (!gateOpen_) {
            SetGate(world, true);
            events_.Raise(Event(ev::GATE_OPENED, PRIORITY_LOW).Int("by", presser));
        }
    } else if (gateOpen_ && !closeScheduled_) {
        closeScheduled_ = true;
        events_.RaiseAfter(Event(ev::GATE_TIMER).Int("generation", ++gateGeneration_), kGateHoldSeconds);
    }
}

void ServerGame::SetGate(World &world, bool open) {
    gateOpen_ = open;
    world.Each<Gate, Renderable>([open](GameObject &o, Gate &gate, Renderable &r) {
        gate.open = open;
        r.color.a = open ? 70 : 255;
        if (open && o.Has<Collider>()) o.Remove<Collider>();
        if (!open && !o.Has<Collider>()) o.Add<Collider>();
    });
}

// --- event handlers (run inside Step, world locked) ---

void ServerGame::OnCollision(const Event &e) {
    if (!world_ || !e.GetBool("trigger")) return;
    GameObject *player = world_->Find(e.GetObject("a"));
    GameObject *trigger = world_->Find(e.GetObject("b"));
    if (!player || !trigger) return;
    const Gem *gem = trigger->Get<Gem>();
    const int element = ElementOf(*player);
    if (!gem || gem->element != element || element < 0) return;
    const Transform &t = *trigger->Get<Transform>();
    sinceCheckpoint_[element].push_back({t.position.x, t.position.y});
    world_->Destroy(trigger->Id());
    if (Match *m = FindMatch(*world_)) ++m->gems[element];
}

void ServerGame::OnBurstRequest(const Event &e) {
    if (!world_) return;
    const int element = e.source;
    GameObject *me = PlayerOf(*world_, element);
    if (!me) return;
    const Vec2 center = me->Get<Transform>()->Center();
    const int id = ++burstSeq_;
    Event burst(ev::BURST);
    burst.Int("element", element).Int("id", id).Point("center", center).Float("radius", 0.0f);

    if (element == Fire) {
        int count = 0;
        world_->Each<Enemy, Transform>([&](GameObject &o, Enemy &enemy, Transform &t) {
            if (Distance(t.Center(), center) > kBurstRadius) return;
            world_->Destroy(o.Id());
            if (enemy.index >= 0 && enemy.index < kEnemies) killedBy_[enemy.index] = Fire;
            events_.Raise(Event(ev::ENEMY_DEFEATED).Int("index", enemy.index).Int("by", Fire).Text("how", "fire"));
            ++count;
        });
        burst.Float("radius", kBurstRadius).Int("count", count);
    } else if (element == Water) {
        // Nearest moving platform not behind the player
        GameObject *best = nullptr;
        float bestLeft = 0;
        world_->Each<PathMover, Transform, Surface>([&](GameObject &o, PathMover &, Transform &t, Surface &) {
            if (t.Right() < center.x - 40 || frozenMovers_.count(o.Id())) return;
            if (!best || t.Left() < bestLeft) { best = &o; bestLeft = t.Left(); }
        });
        if (best) frozenMovers_[best->Id()] = id;
        burst.Object("target", best ? best->Id() : NO_OBJECT).Int("count", best ? 1 : 0);
    } else if (element == Earth) {
        const GameObject *goo = nullptr;
        float gooLeft = 0;
        world_->Each<Surface, Transform>([&](GameObject &o, Surface &s, Transform &t) {
            if (s.style != Style::Goo || t.Right() < center.x - 20) return;
            if (!goo || t.Left() < gooLeft) { goo = &o; gooLeft = t.Left(); }
        });
        ObjectId bridge = NO_OBJECT;
        if (goo) {
            bridge = BuildBridge(*world_, *goo->Get<Transform>()).Id();
            bridges_[id] = bridge;
        }
        burst.Object("target", bridge).Int("count", goo ? 1 : 0);
    } else if (element == Air) {
        int players = 0;
        world_->Each<Player, Transform>([&](GameObject &, Player &p, Transform &t) {
            if (p.element == Air || p.dead || Distance(t.Center(), center) > kBurstRadius) return;
            players |= 1 << p.element;
        });
        burst.Float("radius", kBurstRadius).Int("players", players).Int("count", __builtin_popcount(players));
    }
    if (log_) SDL_Log("[game] %s burst #%d: %s (%lld affected)", ElementName(element), id, BurstEffect(element),
                      static_cast<long long>(burst.GetInt("count")));
    events_.Raise(burst);
    events_.RaiseAfter(Event(ev::BURST_END).Int("element", element).Int("id", id).Object("target", burst.GetObject("target")),
                       kBurstSeconds);
}

void ServerGame::OnBurstEnd(const Event &e) {
    if (!world_) return;
    const int id = static_cast<int>(e.GetInt("id"));
    for (auto it = frozenMovers_.begin(); it != frozenMovers_.end();) it = it->second == id ? frozenMovers_.erase(it) : ++it;
    if (auto it = bridges_.find(id); it != bridges_.end()) {
        world_->Destroy(it->second);
        bridges_.erase(it);
    }
}

void ServerGame::OnStomp(const Event &e) {
    if (!world_) return;
    GameObject *enemy = world_->Find(e.GetObject("enemy"));
    if (!enemy || !enemy->Has<Enemy>()) return;  // already gone: someone else got it first
    const int index = enemy->Get<Enemy>()->index;
    world_->Destroy(enemy->Id());
    if (index >= 0 && index < kEnemies) killedBy_[index] = e.source;
    events_.Raise(Event(ev::ENEMY_DEFEATED).Int("index", index).Int("by", e.source).Text("how", "stomp"));
    if (log_) SDL_Log("[game] %s stomped enemy %d", ElementName(e.source), index);
}

// Death penalty: gems since the checkpoint and enemies killed this round return after 1 s
void ServerGame::OnDeathReport(const Event &e) {
    const int element = e.source;
    const Match *m = world_ ? FindMatch(*world_) : nullptr;
    const int round = m ? m->round : 0;
    int enemies = 0;
    for (int i = 0; i < kEnemies; ++i) {
        if (killedBy_[i] != element) continue;
        killedBy_[i] = -2;
        events_.RaiseAfter(Event(events::SPAWN).Text("kind", "enemy").Int("index", i).Int("by", element).Int("round", round),
                           kRespawnDelay);
        ++enemies;
    }
    for (const TakenGem &g : sinceCheckpoint_[element])
        events_.RaiseAfter(Event(events::SPAWN, PRIORITY_LOW)
                               .Text("kind", "gem").Int("element", element).Point("at", {g.x, g.y}).Int("round", round),
                           kRespawnDelay);
    const size_t gems = sinceCheckpoint_[element].size();
    sinceCheckpoint_[element].clear();
    if (log_)
        SDL_Log("[game] %s died (%s): %d enemy(ies) and %zu gem(s) come back", ElementName(element),
                e.GetText("cause").c_str(), enemies, gems);
}

void ServerGame::OnSpawn(const Event &e) {
    if (!world_) return;
    Match *m = FindMatch(*world_);
    if (!m || e.GetInt("round") != m->round) return;  // from a round that has ended
    if (e.GetText("kind") == "enemy") {
        const int index = static_cast<int>(e.GetInt("index"));
        bool present = false;
        world_->Each<Enemy>([&](GameObject &, Enemy &en) { present |= en.index == index; });
        if (!present && index >= 0 && index < kEnemies) BuildEnemy(*world_, index);
        if (index >= 0 && index < kEnemies) killedBy_[index] = -1;
    } else if (e.GetText("kind") == "gem") {
        const int element = static_cast<int>(e.GetInt("element"));
        const Vec2 at = e.GetPoint("at");
        if (element < 0 || element >= kMaxPlayers) return;
        BuildGem(*world_, at.x, at.y, element);
        m->gems[element] = std::max(0, m->gems[element] - 1);
    }
}

void ServerGame::OnTempleRequest(const Event &e) {
    if (!world_ || templeAnnounced_[e.source]) return;
    templeAnnounced_[e.source] = true;
    int inside = 0, active = 0;
    for (int id = 0; id < kMaxPlayers; ++id) {
        inside += templeAnnounced_[id] && joined_[id];
        active += IsActive(id);
    }
    events_.Raise(Event(ev::TEMPLE_REACHED, PRIORITY_LOW).Int("element", e.source).Int("inside", inside).Int("active", std::max(active, inside)));
    if (log_) SDL_Log("[game] %s reached the %s temple (%d of %d in)", ElementName(e.source), ElementName(e.source), inside, active);
}

// --- rounds and logs ---

void ServerGame::CheckRound(World &world) {
    Match *m = FindMatch(world);
    if (!m) return;
    if (!m->complete) {
        if (time_ - roundStartedAt_ < 1.0) return;
        int active = 0, inDoor = 0;
        world.Each<Player, NetIdentity>([&](GameObject &, Player &p, NetIdentity &net) {
            if (!IsActive(net.owner)) return;
            ++active;
            inDoor += p.inDoor ? 1 : 0;
        });
        if (active > 0 && inDoor == active) {
            m->complete = true;
            m->completeAt = static_cast<float>(time_);
            int team = 0;
            for (int g : m->gems) team += g;
            if (log_)
                SDL_Log("[game] LEVEL COMPLETE at %.1fs with %d active player(s), team gems %d/%d", time_, active, team,
                        kGemsPerElement * kMaxPlayers);
        }
    } else if (time_ - m->completeAt >= kNextRoundSeconds) {
        StartRound(world);
    }
}

void ServerGame::StartRound(World &world) {
    Match *m = FindMatch(world);
    if (!m) return;
    ++m->round;
    m->complete = false;
    m->completeAt = 0;
    m->gems = {};
    roundStartedAt_ = time_;
    world.DestroyIf([](const GameObject &o) { return o.Has<Gem>() || o.Has<Enemy>() || o.Kind() == "bridge"; });
    BuildRoundObjects(world);
    world.Each<Player>([](GameObject &, Player &p) { p.inDoor = false; });
    triggers_.Clear();
    killedBy_.fill(-1);
    templeAnnounced_.fill(false);
    for (auto &list : sinceCheckpoint_) list.clear();
    frozenMovers_.clear();
    bridges_.clear();
    if (log_) SDL_Log("[game] round %d starts", m->round);
}

void ServerGame::Log(World &world) {
    const double window = time_ - lastLog_;
    lastLog_ = time_;

    std::string rates;
    int upstream = 0, clients = 0, players = 0;
    world.Each<Player, NetIdentity>([&](GameObject &, Player &p, NetIdentity &net) {
        const int id = net.owner;
        if (id < 0 || id >= kMaxPlayers) return;
        ++players;
        char buf[112];
        std::snprintf(buf, sizeof buf, "  P%d %s %5.1f snapshots/s (client %.1fx%s)", id, ElementName(id),
                      upstreamThisWindow_[id] / window, p.scale, p.paused ? " PAUSED" : IsActive(id) ? "" : " idle");
        rates += buf;
    });
    for (int id = 0; id < kMaxPlayers; ++id) {
        upstream += upstreamThisWindow_[id];
        clients += joined_[id] ? 1 : 0;
        upstreamThisWindow_[id] = 0;
    }
    if (!rates.empty()) SDL_Log("[rates]%s", rates.c_str());

    if (netStats_ && netOptions_) {
        const NetStats &s = *netStats_;
        const uint64_t snaps = s.messagesOut - snapshotsAtLog_, bytes = s.bytesOut - bytesAtLog_;
        snapshotsAtLog_ = s.messagesOut;
        bytesAtLog_ = s.bytesOut;
        const std::string format = netOptions_->Describe();
        SDL_Log("[net] CLIENT-SERVER (%s) | received %d player snapshots from %d client(s) | published %llu world "
                "snapshots (%.0f bytes avg) carrying the %d player(s): the server relays every player",
                format.c_str(), upstream, clients, static_cast<unsigned long long>(snaps),
                snaps ? static_cast<double>(bytes) / snaps : 0.0, players);
        const uint64_t raised = events_.RaisedCount(), handled = events_.HandledCount();
        const uint64_t in = s.eventsIn, out = s.eventsOut, rejected = s.eventsRejected;
        SDL_Log("[events] server | raised %llu, handled %llu (%llu collisions so far) | from clients: %llu accepted, "
                "%llu rejected by the validator | to clients: %llu deliveries | queued now: %zu",
                static_cast<unsigned long long>(raised - raisedAtLog_), static_cast<unsigned long long>(handled - handledAtLog_),
                static_cast<unsigned long long>(collisionsRaised_), static_cast<unsigned long long>(in - inAtLog_),
                static_cast<unsigned long long>(rejected - rejectedAtLog_), static_cast<unsigned long long>(out - outAtLog_),
                events_.Pending());
        raisedAtLog_ = raised;
        handledAtLog_ = handled;
        inAtLog_ = in;
        outAtLog_ = out;
        rejectedAtLog_ = rejected;
    }
    SDL_Log("[jobs] %d steps: %d of %d jobs ran on pool threads, %d steps had platforms and enemies on "
            "different threads (pool of %d + caller) | %zu objects in the world",
            stepsThisWindow_, pooledThisWindow_, 2 * stepsThisWindow_, splitThisWindow_, jobs_.WorkerCount(),
            world.Count());
    stepsThisWindow_ = pooledThisWindow_ = splitThisWindow_ = 0;
}
}
