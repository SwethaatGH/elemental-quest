#include "level.hpp"
#include "game_events.hpp"
#include "server_game.hpp"

#include "engine/Engine.hpp"
#include "engine/EventSystems.hpp"
#include "engine/Replay.hpp"

#include "engine/Timeline.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>

using namespace engine;
using namespace ed;
using Clock = std::chrono::steady_clock;

namespace {
int g_failures = 0;

void Check(bool ok, const std::string &what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

struct TestClient {
    SceneClient net;
    std::mutex mutex;
    uint32_t buttons = 0;
    int id = -1;
    // Client-side event setup for the checks: shared requests, and a log of every arrival
    Timeline clock{nullptr, 1};
    EventManager events{&clock};
    std::vector<Event> heard;

    explicit TestClient(int joinPort) : net(Config(joinPort), Options()) {
        if (!net.Join()) return;
        id = net.Id();
        net.ShareEvents(events, ClientSharedTypes());
        events.RegisterAll([this](const Event &e) { heard.push_back(e); });
        std::lock_guard<std::mutex> lock(mutex);
        MakePlayer(net.GetWorld(), id);
    }
    void Raise(Event e) { events.Raise(std::move(e)); }
    std::vector<Event> Heard(const char *type) {
        std::vector<Event> out;
        for (const Event &e : heard)
            if (e.Is(type)) out.push_back(e);
        return out;
    }
    static Client::Config Config(int joinPort) {
        Client::Config c;
        c.joinPort = joinPort;
        c.worldPort = joinPort + 1;
        return c;
    }
    static NetOptions Options() {
        NetOptions o;
        o.encoding = Encoding::Binary;
        return o;
    }

    void Frame(float dt) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            World &w = net.GetWorld();
            if (GameObject *me = w.Find(PlayerObjectId(id))) {
                me->Get<InputState>()->buttons = buttons;
                ApplySurfaceRunSpeed(w, *me);
                const PhysicsSettings physics{kGravity, kMaxFallSpeed, 1.0f / 120.0f};
                systems::ApplyControllers(w, net.Authority());
                systems::StepPhysics(w, dt, physics, net.Authority());
                systems::ApplyDeathZones(w, net.Authority());
                net.StampLocal();
            }
        }
        net.Tick(dt, mutex);
        std::lock_guard<std::mutex> lock(mutex);
        events.Handle();
    }

    template <class F>
    auto With(F &&f) {
        std::lock_guard<std::mutex> lock(mutex);
        return f(net.GetWorld());
    }
    void Teleport(float x, float y) {
        With([&](World &w) {
            GameObject *me = w.Find(PlayerObjectId(id));
            me->Get<Transform>()->position = {x, y};
            me->Get<RigidBody>()->velocity = {0, 0};
            return 0;
        });
    }
};

bool RunUntil(const std::vector<TestClient *> &clients, double seconds, const std::function<bool()> &done) {
    const auto start = Clock::now();
    auto last = start;
    while (std::chrono::duration<double>(Clock::now() - start).count() < seconds) {
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        for (TestClient *c : clients) c->Frame(dt);
        if (done && done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    return !done;
}

// --- events ---

double Since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

Vec2 EnemyCenter(TestClient &c, int index) {
    return c.With([index](World &w) {
        Vec2 at{-1, -1};
        w.Each<Enemy, Transform>([&](GameObject &, Enemy &e, Transform &t) { if (e.index == index) at = t.Center(); });
        return at;
    });
}

// Networked requests: validated by the server, same order on every client
void NetworkedEventChecks(TestClient &a, TestClient &b, const std::vector<TestClient *> &both) {
    // FIRE's burst next to enemy 0
    const Vec2 e0 = EnemyCenter(a, 0);
    a.Teleport(e0.x - 120, kGroundTop - kPlayerH - 1);
    RunUntil(both, 0.3, nullptr);
    a.Raise(Event(ev::BURST_REQUEST).Int("element", Fire));
    const auto t0 = Clock::now();
    const bool fireHeard = RunUntil(both, 2.0, [&] { return !b.Heard(ev::BURST).empty(); });
    const double fireMs = Since(t0) * 1000;
    const std::vector<Event> fire = b.Heard(ev::BURST);
    const bool enemyGone = RunUntil(both, 1.0, [&] { return EnemyCenter(b, 0).x < 0; });
    char line[220];
    std::snprintf(line, sizeof line, "events: FIRE's J+K burst_request -> the server's \"burst\" reaches WATER in %.0f ms; "
                  "enemy 0 (within %.0f px) is defeated on WATER's screen", fireMs, kBurstRadius);
    Check(fireHeard && fire.front().GetInt("element") == Fire && fire.front().GetInt("count") >= 1 && enemyGone, line);
    const Clock::time_point fireAt = Clock::now();

    // A second burst inside the cooldown is rejected
    a.Raise(Event(ev::BURST_REQUEST).Int("element", Fire));
    // WATER's burst at the pit edge freezes the next moving platform
    b.Teleport(4480, kGroundTop - kPlayerH - 1);
    RunUntil(both, 0.3, nullptr);
    b.Raise(Event(ev::BURST_REQUEST).Int("element", Water));
    RunUntil(both, 2.0, [&] { return a.Heard(ev::BURST).size() >= 2 && b.Heard(ev::BURST).size() >= 2; });
    RunUntil(both, 0.3, nullptr);
    auto order = [](TestClient &c) {
        std::vector<int64_t> o;
        for (const Event &e : c.Heard(ev::BURST)) o.push_back(e.GetInt("id") * 10 + e.GetInt("element"));
        return o;
    };
    Check(order(a) == order(b) && order(a).size() == 2,
          "events: FIRE's and WATER's bursts arrive on both clients in the same order (server-centric); "
          "FIRE's second burst inside its 8 s cooldown was rejected by the server's validator");

    const std::vector<Event> water = a.Heard(ev::BURST);
    const ObjectId frozen = water.back().GetObject("target");
    auto moverAt = [&](TestClient &c, ObjectId id) {
        return c.With([id](World &w) { const GameObject *o = w.Find(id); return o ? o->Get<Transform>()->position : Vec2{-1, -1}; });
    };
    const Vec2 p0 = moverAt(a, frozen);
    RunUntil(both, 0.6, nullptr);
    const Vec2 p1 = moverAt(a, frozen);
    Check(frozen != NO_OBJECT && p0.x >= 0 && std::fabs(p1.x - p0.x) + std::fabs(p1.y - p0.y) < 0.5f,
          "events: WATER's burst froze the next moving platform; it stands still on FIRE's screen");

    const bool ended = RunUntil(both, 5.0, [&] {
        for (const Event &e : b.Heard(ev::BURST_END)) if (e.GetInt("element") == Fire) return true;
        return false;
    });
    const double endAfter = Since(fireAt) + fireMs / 1000.0;
    std::snprintf(line, sizeof line, "events: the delayed burst_end (RaiseAfter %.0f s on the server) reaches WATER %.1f s after the burst",
                  kBurstSeconds, endAfter);
    Check(ended && std::fabs(endAfter - kBurstSeconds) < 0.6, line);
    RunUntil(both, 0.8, nullptr);
    const Vec2 p2 = moverAt(a, frozen);
    RunUntil(both, 0.3, nullptr);
    const Vec2 p3 = moverAt(a, frozen);
    Check(std::fabs(p3.x - p2.x) + std::fabs(p3.y - p2.y) > 0.5f, "events: after WATER's burst_end the platform moves again");

    // FIRE's death brings back the enemy it defeated
    a.Raise(Event(ev::DEATH_REPORT).Int("element", Fire).Text("cause", "goo").Int("checkpoint", 0));
    const auto d0 = Clock::now();
    const bool back = RunUntil(both, 3.0, [&] { return EnemyCenter(b, 0).x >= 0; });
    bool spawnHeard = false;
    for (const Event &e : b.Heard(events::SPAWN)) spawnHeard |= e.GetText("kind") == "enemy" && e.GetInt("index") == 0 && e.GetInt("by") == Fire;
    std::snprintf(line, sizeof line, "events: FIRE's death_report -> the enemy FIRE defeated respawns %.1f s later on WATER's "
                  "screen, with a shared \"spawn\" event (kind enemy, by FIRE)", Since(d0));
    Check(back && spawnHeard, line);

    // Temple claims: a client may only speak for itself
    b.Raise(Event(ev::TEMPLE_REQUEST).Int("element", Fire));  // WATER pretending to be FIRE
    a.Teleport(9320, kGroundTop - kPlayerH - 1);
    RunUntil(both, 0.3, nullptr);
    a.Raise(Event(ev::TEMPLE_REQUEST).Int("element", Fire));
    RunUntil(both, 2.0, [&] { return !b.Heard(ev::TEMPLE_REACHED).empty(); });
    RunUntil(both, 0.3, nullptr);
    const std::vector<Event> temple = b.Heard(ev::TEMPLE_REACHED);
    Check(temple.size() == 1 && temple[0].GetInt("element") == Fire,
          "events: FIRE in its door -> temple_reached broadcast to WATER; WATER's forged request for FIRE was rejected");
    a.With([&](World &w) { return systems::PlaceAtSpawn(w, *w.Find(PlayerObjectId(0)), SpawnIndex(0, 0)); });
    b.With([&](World &w) { return systems::PlaceAtSpawn(w, *w.Find(PlayerObjectId(1)), SpawnIndex(0, 1)); });
}

// Client-side event chains, headless, with the client's settings
void LocalEventChecks() {
    std::printf("\n== EVENTS ON ONE CLIENT ==\n");
    {
        Timeline clock(nullptr, 1);
        EventManager m(&clock);
        InputEvents input(m, GameKeyBindings());
        ChordEvents chords(m, kChordWindow);
        AddGameChords(chords);
        std::vector<std::string> fired;
        int inputs = 0;
        InputState state;
        m.Register(events::INPUT, [&](const Event &e) { ++inputs; InputEvents::Apply(e, state); });
        m.Register(events::CHORD, [&](const Event &e) { fired.push_back(e.GetText("name")); });
        auto press = [&](std::set<SDL_Scancode> keys) {
            input.Update(1, [&](SDL_Scancode k) { return keys.count(k) != 0; });
            m.Handle();
            m.Handle();
        };
        press({SDL_SCANCODE_SPACE, SDL_SCANCODE_RIGHT});
        const bool jumpOnly = fired.empty() && inputs == 2 && (state.buttons & InputState::Right) && (state.buttons & InputState::Jump);
        press({});
        press({SDL_SCANCODE_M, SDL_SCANCODE_RIGHT});
        const bool dash = fired.size() == 1 && fired[0] == "dash_right";
        press({});
        press({SDL_SCANCODE_J});
        press({SDL_SCANCODE_J, SDL_SCANCODE_K});
        const bool burst = fired.size() == 2 && fired[1] == "burst";
        press({});
        press({SDL_SCANCODE_RIGHT});
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        press({SDL_SCANCODE_RIGHT, SDL_SCANCODE_M});
        const bool slow = fired.size() == 2;
        Check(jumpOnly && dash && burst && slow,
              "input events drive the InputState; Space+Right is just a jump while running, M+Right together is the "
              "dash_right chord, J+K the burst chord; Right held 0.3 s before M is NOT a chord (0.2 s window)");
    }
    {
        Timeline clock(nullptr, 1);
        EventManager m(&clock);
        World w;
        BuildLevel(w);
        GameObject &p = MakePlayer(w, 0);
        RespawnEvents respawns(m, w, OwnedBy(0), kRespawnDelay);
        int deaths = 0;
        double spawnedAfter = -1;
        const auto t0 = Clock::now();
        m.Register(events::DEATH, [&](const Event &) { ++deaths; });
        m.Register(events::SPAWN, [&](const Event &) { spawnedAfter = Since(t0); });
        p.Get<Transform>()->position = {4700, 1200};  // in the pit's death zone
        while (Since(t0) < 1.6) {
            respawns.Update(OwnedBy(0));
            m.Handle();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const Transform &t = *p.Get<Transform>();
        char line[200];
        std::snprintf(line, sizeof line, "a fall into the pit raises ONE death (not one per frame), and the spawn event %.2f s later "
                      "puts the player on its hidden spawn point", spawnedAfter);
        Check(deaths == 1 && std::fabs(spawnedAfter - kRespawnDelay) < 0.1 && t.position.x < 400 && t.position.y < kGroundTop, line);
    }
    {
        // Death cam path: no automatic spawn; the game raises it with Spawn()
        Timeline clock(nullptr, 1);
        EventManager m(&clock);
        World w;
        BuildLevel(w);
        GameObject &p = MakePlayer(w, 0);
        RespawnEvents respawns(m, w, OwnedBy(0), kRespawnDelay);
        respawns.SetAutoSpawn(false);
        double spawnedAfter = -1, asked = -1;
        const auto t0 = Clock::now();
        m.Register(events::SPAWN, [&](const Event &) { spawnedAfter = Since(t0); });
        p.Get<Transform>()->position = {4700, 1200};
        while (Since(t0) < 2.3) {
            respawns.Update(OwnedBy(0));
            if (asked < 0 && Since(t0) > 1.5) {
                asked = Since(t0);
                respawns.Spawn(p.Id(), 0.3);
            }
            m.Handle();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        char line[200];
        std::snprintf(line, sizeof line, "with SetAutoSpawn(false) a death raises no spawn by itself; Spawn(id, 0.3) at %.1f s "
                      "raises it %.2f s later", asked, spawnedAfter - asked);
        Check(asked > 0 && spawnedAfter > asked && std::fabs(spawnedAfter - asked - 0.3) < 0.1 &&
              p.Get<Transform>()->position.x < 400, line);
    }
    {
        Timeline game(nullptr, 1), replayClock(nullptr, 1);
        EventManager m(&game);
        World w;
        BuildLevel(w);
        GameObject &p = MakePlayer(w, 0);
        ReplayRecorder recorder(m, kReplayKeepSeconds);
        ReplayPlayer player(m, recorder, replayClock);
        bool finished = false, sawMove = false;
        m.Register(events::DEATH, [&](const Event &) {
            m.Raise(Event(events::REPLAY_PLAY).Float("seconds", static_cast<float>(kReplaySeconds)).Float("speed", static_cast<float>(kReplaySpeed)));
        });
        m.Register(events::REPLAY_FINISHED, [&](const Event &) { finished = true; });
        m.Raise(Event(events::REPLAY_RECORD_START));
        auto t0 = Clock::now();
        while (Since(t0) < 1.0) {
            p.Get<Transform>()->position.x = 120 + 400 * static_cast<float>(Since(t0));
            m.Handle();
            recorder.Capture(w);
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        m.Raise(Event(events::DEATH).Object("object", p.Id()).Text("cause", "goo"));
        m.Handle();
        m.Handle();
        const bool playing = player.Playing();
        game.Pause();  // the death cam pauses game time; playback runs on real time
        float firstX = -1;
        t0 = Clock::now();
        while (!finished && Since(t0) < 4.0) {
            player.Update();
            m.Handle();
            if (player.Playing())
                if (const GameObject *o = player.PlaybackWorld().Find(p.Id())) {
                    const float x = o->Get<Transform>()->position.x;
                    if (firstX < 0) firstX = x;
                    sawMove |= x > firstX + 100;
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        char line[220];
        std::snprintf(line, sizeof line, "replay: a death raises replay_play; the recorded ~1 s plays back at %.1fx while game time is "
                      "paused (%.1f s), the player moves in the playback world, then replay_finished", kReplaySpeed, Since(t0));
        Check(playing && sawMove && finished && Since(t0) > 1.5, line);
    }
    {
        // Temple gap: a running jump and a standing dash both clear it
        auto attempt = [](bool dash) {
            World w;
            BuildLevel(w);
            GameObject &p = MakePlayer(w, 0);
            p.Get<Transform>()->position = {kTempleGapLeft - kPlayerW - 2, 780 - kPlayerH - 1};
            const PhysicsSettings physics{kGravity, kMaxFallSpeed, 1.0f / 120.0f};
            InputState &in = *p.Get<InputState>();
            RigidBody &body = *p.Get<RigidBody>();
            double t = 0;
            for (int i = 0; i < 360; ++i, t += 1.0 / 120) {
                // settle on the ledge, then jump (or dash without jumping)
                in.buttons = InputState::Right | (!dash && i >= 6 && i < 9 ? InputState::Jump : 0u);
                systems::ApplyControllers(w, OwnedBy(0));
                if (dash && t >= 0.05 && t < 0.05 + kDashSeconds) { body.velocity.x = kDashSpeed; body.velocity.y = 0; }
                systems::StepPhysics(w, 1.0f / 120, physics, OwnedBy(0));
                const Transform &tr = *p.Get<Transform>();
                if (tr.Top() > kGroundTop + 50) return false;  // in the pit
                if (body.grounded && tr.Left() > kTempleGapRight - kPlayerW && i > 10) return true;  // across
            }
            return false;
        };
        const bool jump = attempt(false), dashed = attempt(true);
        char line[200];
        std::snprintf(line, sizeof line, "the %.0f px temple gap: a running jump clears it, and an M+Right dash clears it "
                      "without jumping", kTempleGapRight - kTempleGapLeft);
        Check(jump && dashed, line);
    }
}

void CheckClientServer(int joinPort) {
    std::printf("\n== CLIENT-SERVER ==\n");

    Engine engine;
    engine.InitHeadless();
    Server::Config cfg;
    cfg.joinPort = joinPort;
    cfg.worldPort = joinPort + 1;
    cfg.clientPortBase = joinPort + 10;
    cfg.maxClients = kMaxPlayers;
    cfg.publishEverySteps = 1;
    cfg.logTag = "check-server";
    NetOptions options;
    options.encoding = Encoding::Binary;
    SceneServer host(engine, cfg, options);
    BuildLevel(host.GetWorld());
    ServerGame game(engine.GetJobs(), engine.GetGameTime(), false);
    game.Share(host);
    std::atomic<int> relayedPlayers{0};
    SceneServer::Callbacks cb;
    cb.onJoin = [&](World &w, int id) { game.OnJoin(w, id); };
    cb.onLeave = [&](World &w, int id) { game.OnLeave(w, id); };
    cb.step = [&](World &w, float dt) {
        game.Step(w, dt);
        int n = 0;
        w.Each<Player>([&](GameObject &, Player &) { ++n; });
        relayedPlayers = n;
    };
    std::thread serverThread([&] { host.Run(cb); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto a = std::make_unique<TestClient>(joinPort);
    auto b = std::make_unique<TestClient>(joinPort);
    Check(a->id == 0 && b->id == 1, "two clients join and get elements FIRE (0) and WATER (1)");
    std::vector<TestClient *> both{a.get(), b.get()};
    RunUntil(both, 3.0, [&] { return a->net.HasWorld() && b->net.HasWorld(); });
    RunUntil(both, 0.5, nullptr);

    b->With([&](World &w) {
        std::map<std::string, int> kinds;
        std::set<int> styles;
        std::set<std::tuple<float, float, float, float, int>> looks;
        int hiddenSpawns = 0, spawns = 0, hiddenZones = 0, zones = 0, hiddenCps = 0, cps = 0;
        bool horizontal = false, vertical = false, diagonal = false;
        w.Each<NetIdentity>([&](GameObject &o, NetIdentity &) { ++kinds[o.Kind()]; });
        w.Each<Transform, Surface, Collider>([&](GameObject &o, Transform &t, Surface &s, Collider &) {
            if (o.Has<PathMover>()) return;
            styles.insert(static_cast<int>(s.style));
            const SDL_Color c = o.Get<Renderable>()->color;
            looks.insert({t.position.x, t.position.y, t.size.x, t.size.y, c.r << 16 | c.g << 8 | c.b});
        });
        w.Each<PathMover, Surface>([&](GameObject &, PathMover &p, Surface &) {
            horizontal |= p.offset.y == 0 && p.offset.x != 0;
            vertical |= p.offset.x == 0 && p.offset.y != 0;
            diagonal |= p.offset.x != 0 && p.offset.y != 0;
        });
        w.Each<SpawnPoint>([&](GameObject &o, SpawnPoint &) { ++spawns; hiddenSpawns += !o.Has<Renderable>(); });
        w.Each<DeathZone>([&](GameObject &o, DeathZone &) { ++zones; hiddenZones += !o.Has<Renderable>(); });
        w.Each<Checkpoint>([&](GameObject &o, Checkpoint &) { ++cps; hiddenCps += !o.Has<Renderable>(); });
        Check(w.Count() > 90, "the server's level arrived as objects in the client's World (" + std::to_string(w.Count()) + " objects)");
        Check(styles.size() >= 5 && looks.size() >= 3,
              std::to_string(styles.size()) + " static platform looks (ground, basalt, ice, moss stone, cloud), " +
                  std::to_string(looks.size()) + " distinct position/size/colour combinations");
        Check(horizontal && vertical && diagonal, "moving platforms: horizontal, vertical and diagonal patterns");
        Check(spawns == 20 && hiddenSpawns == spawns, std::to_string(spawns) + " spawn points, all hidden (no Renderable)");
        Check(zones == 6 && hiddenZones == zones, std::to_string(zones) + " death zones (5 goo + the pit), all hidden");
        Check(cps == kCheckpoints && hiddenCps == cps, std::to_string(cps) + " checkpoints, all hidden");
        return 0;
    });

    auto moverX = [](TestClient &c) {
        return c.With([](World &w) {
            std::vector<float> xs;
            w.Each<PathMover, Surface, Transform>([&](GameObject &, PathMover &, Surface &, Transform &t) {
                xs.push_back(t.position.x + t.position.y);
            });
            return xs;
        });
    };
    const std::vector<float> before = moverX(*a);
    RunUntil(both, 0.5, nullptr);
    const std::vector<float> afterA = moverX(*a), afterB = moverX(*b);
    bool moved = false, agree = afterA.size() == afterB.size() && !afterA.empty();
    for (size_t i = 0; i < afterA.size() && i < before.size(); ++i) moved |= std::fabs(afterA[i] - before[i]) > 1.0f;
    for (size_t i = 0; agree && i < afterA.size(); ++i) agree = std::fabs(afterA[i] - afterB[i]) < 25.0f;
    Check(moved, "moving platforms move on the client (driven by the server's snapshots)");
    Check(agree, "the moving platforms are in the same place on both clients (within one tick)");

    a->Teleport(2000, 800);
    const auto t0 = Clock::now();
    const bool seen = RunUntil(both, 2.0, [&] {
        return b->With([&](World &w) {
            const GameObject *p = w.Find(PlayerObjectId(0));
            return p && std::fabs(p->Get<Transform>()->position.x - 2000) < 5;
        });
    });
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    char line[160];
    std::snprintf(line, sizeof line, "FIRE's move shows on WATER's client through the server, in %.0f ms (<100 ms wanted)", ms);
    Check(seen && ms < 100, line);
    Check(relayedPlayers == 2, "the server relays both players in its snapshots");

    {
        auto c = std::make_unique<TestClient>(joinPort);
        auto d = std::make_unique<TestClient>(joinPort);
        std::vector<TestClient *> four{a.get(), b.get(), c.get(), d.get()};
        auto playersIn = [](TestClient &t) {
            return t.With([](World &w) { int n = 0; w.Each<Player>([&](GameObject &, Player &) { ++n; }); return n; });
        };
        Check(RunUntil(four, 3.0, [&] {
                  for (TestClient *t : four)
                      if (playersIn(*t) != 4) return false;
                  return true;
              }),
              "four clients at once (EARTH and AIR join): every client's World holds all four players");
        c->net.Leave();
        d->net.Leave();
        c.reset();
        d.reset();
        Check(RunUntil(both, 2.0, [&] { return playersIn(*a) == 2 && playersIn(*b) == 2; }),
              "EARTH and AIR leave: the other two clients are back to two players");
    }

    auto gateHasCollider = [](TestClient &c) {
        return c.With([](World &w) {
            bool has = false;
            w.Each<Gate>([&](GameObject &o, Gate &) { has = o.Has<Collider>(); });
            return has;
        });
    };
    Check(gateHasCollider(*b), "the gate starts closed: it has a Collider on WATER's client");
    a->With([&](World &w) { return systems::PlaceAtSpawn(w, *w.Find(PlayerObjectId(0)), SpawnIndex(4, 0)); });
    Check(RunUntil(both, 2.0, [&] { return !gateHasCollider(*b) && !gateHasCollider(*a); }),
          "FIRE stands on a plate: the server takes the gate's Collider off, and both clients' copies lose it");
    a->With([&](World &w) { return systems::PlaceAtSpawn(w, *w.Find(PlayerObjectId(0)), SpawnIndex(0, 0)); });
    Check(RunUntil(both, 4.0, [&] { return gateHasCollider(*b); }), "FIRE steps off: 2.5 s later the gate's Collider is back");
    RunUntil(both, 0.3, nullptr);
    {
        const auto opened = b->Heard(ev::GATE_OPENED), closed = b->Heard(ev::GATE_CLOSED);
        Check(!opened.empty() && opened.back().GetInt("by") == 0 && !closed.empty() && opened.back().remote,
              "events: WATER's client heard the server's gate_opened (by FIRE) and gate_closed (its 2.5 s gate_timer)");
    }

    {
        auto runOn = [&](float x, float top) {
            a->Teleport(x, top - kPlayerH - 2);
            RunUntil(both, 0.3, nullptr);
            const float x0 = a->With([](World &w) { return w.Find(PlayerObjectId(0))->Get<Transform>()->position.x; });
            a->buttons = InputState::Right;
            RunUntil(both, 0.25, nullptr);
            a->buttons = 0;
            return a->With([](World &w) { return w.Find(PlayerObjectId(0))->Get<Transform>()->position.x; }) - x0;
        };
        const float cloud = runOn(2370, 780), ice = runOn(2720, 660), basalt = runOn(3070, 780);
        char line[200];
        std::snprintf(line, sizeof line, "surfaces: in 0.25 s FIRE runs %.0f px on cloud, %.0f on basalt, %.0f on ice "
                      "(cloud/basalt %.2f, ice/basalt %.2f; 0.5 and 1.5 wanted)", cloud, basalt, ice, cloud / basalt, ice / basalt);
        Check(std::fabs(cloud / basalt - 0.5f) < 0.12f && std::fabs(ice / basalt - 1.5f) < 0.2f, line);
        a->With([&](World &w) { return systems::PlaceAtSpawn(w, *w.Find(PlayerObjectId(0)), SpawnIndex(0, 0)); });
    }

    a->Teleport(4700, 1200);
    RunUntil(both, 0.2, nullptr);
    a->With([&](World &w) {
        const GameObject *me = w.Find(PlayerObjectId(0));
        const Transform &t = *me->Get<Transform>();
        Check(me->Get<Respawn>()->deaths == 1 && t.position.y < kGroundTop && t.position.x < 400,
              "FIRE falls into the hidden pit death zone and is sent to its hidden spawn point");
        return 0;
    });

    ObjectId gemId = NO_OBJECT;
    float gx = 0, gy = 0;
    b->With([&](World &w) {
        w.Each<Gem, Transform>([&](GameObject &o, Gem &g, Transform &t) {
            if (g.element == 0 && gemId == NO_OBJECT && t.position.x > 4000 && t.position.x < 4100) {
                gemId = o.Id(); gx = t.position.x; gy = t.position.y;
            }
        });
        return 0;
    });
    auto fireGems = [&] { return b->With([](World &w) { int g = 0; w.Each<Match>([&](GameObject &, Match &m) { g = m.gems[0]; }); return g; }); };
    const int gemsBefore = fireGems();
    const uint64_t collisionsBefore = game.CollisionsRaised();
    a->Teleport(gx - 8, gy - 20);
    Check(RunUntil(both, 2.0, [&] { return b->With([&](World &w) { return w.Find(gemId) == nullptr; }); }) &&
              fireGems() == gemsBefore + 1,
          "FIRE touches a FIRE gem: the server destroys it, it is gone on WATER's client, FIRE's count goes up by 1");
    Check(game.CollisionsRaised() > collisionsBefore,
          "events: the gem pickup was a \"collision\" event on the server (CollisionEvents from the trigger enter)");
    a->With([&](World &w) { return systems::PlaceAtSpawn(w, *w.Find(PlayerObjectId(0)), SpawnIndex(0, 0)); });

    NetworkedEventChecks(*a, *b, both);

    b->net.Leave();
    b.reset();
    std::vector<TestClient *> justA{a.get()};
    Check(RunUntil(justA, 2.0, [&] { return a->With([](World &w) { return w.Find(PlayerObjectId(1)) == nullptr; }); }),
          "WATER leaves (BYE): its player is removed from FIRE's World");
    const uint64_t heard = a->net.Stats().messagesIn;
    RunUntil(justA, 0.3, nullptr);
    Check(a->net.Stats().messagesIn > heard, "FIRE and the server carry on normally after the leave");

    auto silent = std::make_unique<TestClient>(joinPort);
    const int silentId = silent->id;
    RunUntil(justA, 0.5, nullptr);
    std::printf("  ...  a third client (id %d) joins and then goes silent; waiting for the server's 3 s timeout\n", silentId);
    Check(RunUntil(justA, 6.0, [&] {
              return a->With([&](World &w) {
                  uint32_t present = 0;
                  w.Each<Match>([&](GameObject &, Match &m) { present = m.present; });
                  return !(present >> silentId & 1u) && !w.Find(PlayerObjectId(silentId));
              });
          }),
          "a silent client is timed out and removed; the others never notice a pause");
    silent.reset();

    a->net.Leave();
    a.reset();
    host.Stop();
    serverThread.join();
    engine.Shutdown();
}
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    RegisterGameComponents();
    LocalEventChecks();
    CheckClientServer(8855);
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : ("FAILED: " + std::to_string(g_failures)).c_str());
    return g_failures == 0 ? 0 : 1;
}
