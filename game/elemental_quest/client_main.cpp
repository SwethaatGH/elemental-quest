#include "draw.hpp"
#include "game_events.hpp"
#include "level.hpp"

#include "engine/Engine.hpp"
#include "engine/EventManager.hpp"
#include "engine/EventSystems.hpp"
#include "engine/Input.hpp"
#include "engine/NetScene.hpp"
#include "engine/Replay.hpp"
#include "engine/Scaling.hpp"
#include "engine/Timeline.hpp"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace engine;
using namespace ed;

namespace {
Engine *g_engine = nullptr;
extern "C" void OnQuitSignal(int) {
    if (g_engine) g_engine->Stop();
}

constexpr double kInvulnerableSeconds = 1.5;
constexpr int64_t kPlayerClockTic = 1000;
constexpr Uint64 kBannerMs = 2600;
constexpr Uint64 kStompWaitMs = 1000;  // an unconfirmed stomp shows the enemy again after this
constexpr Uint64 kReplayPauseDelayMs = 120;  // let the death reach the others before pausing
const SDL_Color kWhite{240, 240, 240, 255};
const SDL_Color kDim{170, 180, 200, 255};
const SDL_Color kRed{255, 90, 90, 255};
const SDL_Color kGreen{120, 230, 130, 255};
const SDL_Color kYellow{255, 215, 90, 255};
const SDL_Color kIce{150, 225, 255, 255};

std::string Format(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
std::string Format(const char *fmt, ...) {
    char buf[320];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    return buf;
}

SDL_Color WithAlpha(SDL_Color c, Uint8 a) {
    c.a = a;
    return c;
}

struct Options {
    double scale = 1.0;
    int windowW = 0, windowH = 0;
    int posX = INT_MIN, posY = INT_MIN;
    double autoshotSeconds = -1;
    int checkpoint = 0;
    bool showHidden = false;
    bool walk = false;
    bool noHelp = false;
    bool eventLog = false;
    bool noDeathCam = false;
    std::string host = "localhost";  // the machine running elemental_server
    NetOptions net;
};

bool ParseOptions(int argc, char **argv, Options &o, std::string &error) {
    o.net.encoding = Encoding::Binary;
    if (!NetOptions::Parse(argc, argv, o.net, &error)) return false;
    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        const std::string a = argv[i];
        if (a == "--host" && hasValue) {
            o.host = argv[++i];
        } else if (a == "--scale" && hasValue) {
            o.scale = std::atof(argv[++i]);
        } else if (a == "--window" && hasValue) {
            if (std::sscanf(argv[++i], "%dx%d", &o.windowW, &o.windowH) != 2) return false;
        } else if (a == "--pos" && hasValue) {
            if (std::sscanf(argv[++i], "%d,%d", &o.posX, &o.posY) != 2) return false;
        } else if (a == "--checkpoint" && hasValue) {
            o.checkpoint = std::clamp(std::atoi(argv[++i]), 0, kCheckpoints);
        } else if (a == "--autoshot" && hasValue) {
            o.autoshotSeconds = std::atof(argv[++i]);
        } else if (a == "--show-hidden") {
            o.showHidden = true;
        } else if (a == "--walk") {
            o.walk = true;
        } else if (a == "--no-help") {
            o.noHelp = true;
        } else if (a == "--event-log") {
            o.eventLog = true;
        } else if (a == "--no-deathcam") {
            o.noDeathCam = true;
        } else if ((a == "--encoding" || a == "--delta" || a == "--keyframe" || a == "--upstream" ||
                    a == "--topology") && hasValue) {
            ++i;
        } else {
            error = "unknown option " + a;
            return false;
        }
    }
    if (o.net.topology != Topology::ClientServer || o.net.upstream != Upstream::State) {
        error = "Elemental Quest runs client-server, with clients sending state";
        return false;
    }
    return true;
}

// One player's window: input, chords, collisions, deaths and spawns as events
class Game {
public:
    Game(Engine &engine, SceneClient &net, Art &art, const Options &options)
        : engine_(engine), net_(net), id_(net.Id()), options_(options), art_(art),
          playerClock_(&engine.GetGameTime(), kPlayerClockTic), events_(&engine.GetGameTime()),
          input_(events_, GameKeyBindings()), chords_(events_, kChordWindow), collisions_(events_),
          respawns_(events_, net.GetWorld(), OwnedBy(net.Id()), kRespawnDelay), recorder_(events_, kReplayKeepSeconds),
          replay_(events_, recorder_, replayClock_), checkpoint_(options.checkpoint),
          startCheckpoint_(options.checkpoint), deathCam_(!options.noDeathCam), showHelp_(!options.noHelp),
          showHidden_(options.showHidden),
          autoshotAtMs_(options.autoshotSeconds >= 0 ? static_cast<Uint64>(options.autoshotSeconds * 1000) : 0) {
        World &world = net_.GetWorld();
        std::lock_guard<std::mutex> lock(engine_.GetWorldMutex());
        MakePlayer(world, id_);

        GameObject &cam = world.Create("camera");
        Camera &c = cam.Add<Camera>();
        c.boundsMin = {0.0f, 0.0f};
        c.boundsMax = {kWorldWidth, kWorldHeight};
        c.target = PlayerObjectId(id_);
        cameraId_ = cam.Id();
        const std::pair<ScrollBoundary::Side, const char *> sides[] = {
            {ScrollBoundary::Side::Left, "scroll-left"}, {ScrollBoundary::Side::Right, "scroll-right"},
            {ScrollBoundary::Side::Top, "scroll-top"}, {ScrollBoundary::Side::Bottom, "scroll-bottom"}};
        for (const auto &side : sides) {
            GameObject &b = world.Create("scroll-boundary", side.second);
            b.Add<Transform>();
            b.Add<ScrollBoundary>(cameraId_, side.first, 0.0f);
        }
        BuildSigns(world);

        // Requests go to the server; its results are raised here by the network thread
        net_.ShareEvents(events_, ClientSharedTypes());
        if (options_.eventLog) {
            const int id = id_;
            events_.SetLog([id](const std::string &line) { SDL_Log("[event] P%d %s %s", id, ElementName(id), line.c_str()); });
        }
        AddGameChords(chords_);
        respawns_.SetAutoSpawn(false);  // the death cam decides when the spawn is raised
        RegisterHandlers();
        events_.Raise(Event(events::REPLAY_RECORD_START));  // always keep the last 5 s

        Banner(Format("You are %s. Collect %s gems and reach the %s door!", ElementName(id_), ElementName(id_),
                      ElementName(id_)),
               ElementColor(id_), 6000);
    }

    void Update(float dt) {
        HandleKeys();
        std::lock_guard<std::mutex> lock(engine_.GetWorldMutex());
        World &world = net_.GetWorld();
        GameObject *me = world.Find(PlayerObjectId(id_));
        haveWorld_ = net_.HasWorld();
        if (me && haveWorld_) {
            if (!placed_ && systems::FindSpawn(world, SpawnIndex(checkpoint_, id_))) {
                SendToSpawn(world, *me);
                placed_ = true;
            }
            FollowMatch(world, *me);

            if (!replay_.Playing()) {
                if (options_.walk) {
                    walkClock_ += dt;
                    const bool jump = std::fmod(walkClock_, 1.1) < 0.1;
                    input_.Update(me->Id(), [jump](SDL_Scancode k) {
                        return k == SDL_SCANCODE_RIGHT || (jump && k == SDL_SCANCODE_SPACE);
                    });
                } else {
                    input_.Update(me->Id());
                }
            }
            events_.Handle(PRIORITY_HIGH);  // input and chords, before physics
            if (dt > 0.0f && placed_) Simulate(world, *me, dt);
            events_.Handle();  // collisions, deaths, spawns, the server's events

            replay_.Update();
            if (!engine_.GetGameTime().IsPaused()) recorder_.Capture(world);
            WritePlayer(*me);
            net_.StampLocal();
        } else {
            events_.Handle();
        }
        UpdateReplayPause();
        UpdateCamera(world);
        if (Input::IsKeyJustPressed(SDL_SCANCODE_R) && me && placed_ && !engine_.GetGameTime().IsPaused() && !inDoor_ &&
            !dying_) {
            me->Get<Respawn>()->spawnIndex = SpawnIndex(checkpoint_, id_);
            events_.Raise(Event(events::SPAWN, PRIORITY_HIGH).Object("object", me->Id()).Int("spawn", SpawnIndex(checkpoint_, id_)));
        }
        LogTrafficEvery5s(world);
    }

    void Render() {
        SDL_Renderer *r = engine_.GetRenderer();
        int windowW = 0, windowH = 0;
        engine_.GetWindowSize(&windowW, &windowH);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        {
            std::lock_guard<std::mutex> lock(engine_.GetWorldMutex());
            World &world = net_.GetWorld();
            const Camera *cam = world.Find(cameraId_)->Get<Camera>();
            if (replay_.Playing()) {
                RenderReplay(world, r, *cam, windowW, windowH);
            } else {
                const View view = View::From(*cam, engine_.GetScale());
                DrawBackground(r, view, art_, windowW, windowH);

                DrawRules rules;
                rules.myElement = id_;
                rules.me = PlayerObjectId(id_);
                const double now = GameSeconds();
                rules.blinkMe = !dying_ && now < invulnerableUntil_ && static_cast<int>(now * 10) % 2 == 0;
                rules.hidden = pendingStomp_;
                DrawDashTrail(world, r, view);
                RenderScene(world, r, view, art_, rules);
                DrawBursts(world, r, view);

                // Signs are sized in world units, so they never run into each other at any window size
                const float signSize = std::max(1.0f, 2.0f * view.scale);
                // Hints step aside while the level-complete panel is up
                const Match *match = FindMatch(world);
                if (!(match && match->complete))
                    world.Each<Transform, Sign>([&](GameObject &, Transform &t, Sign &s) {
                        DrawWorldText(r, view, t.position.x, t.position.y, s.text, {255, 255, 210, 255}, signSize);
                    });
                DrawNameTags(world, r, view);
                if (showHidden_) DrawHiddenObjects(world, r, view);
                DrawHud(world, r, windowW, windowH);
                DrawOverlays(world, r, windowW, windowH);
            }
        }

        const bool autoshot = autoshotAtMs_ > 0 && SDL_GetTicks() >= autoshotAtMs_;
        if (Input::IsKeyJustPressed(SDL_SCANCODE_F12) || autoshot) {
            autoshotAtMs_ = 0;
            SaveScreenshot(r);
        }
    }

    // Snapshots and events to and from the server
    void Background(float dt) { net_.Tick(dt, engine_.GetWorldMutex()); }

private:
    struct ActiveBurst {
        int element = 0, id = 0, players = 0, count = 0;
        Vec2 center{};
        float radius = 0;
        ObjectId target = NO_OBJECT;
        Uint64 startedMs = 0;
    };

    // --- event handlers ---

    void RegisterHandlers() {
        events_.Register(events::INPUT, [this](const Event &e) {
            if (e.GetObject("object") == PlayerObjectId(id_)) InputEvents::Apply(e, keys_);
        });
        events_.Register(events::CHORD, [this](const Event &e) { OnChord(e); });
        events_.Register(events::COLLISION, [this](const Event &e) { OnCollision(e); });
        events_.Register(events::DEATH, [this](const Event &e) { OnDeath(e); });
        events_.Register(events::SPAWN, [this](const Event &e) { OnSpawn(e); });
        events_.Register(ev::SURFACE, [this](const Event &e) { OnSurface(e); });
        events_.Register(ev::BURST, [this](const Event &e) { OnBurst(e); });
        events_.Register(ev::BURST_END, [this](const Event &e) { OnBurstEnd(e); });
        events_.Register(ev::ENEMY_DEFEATED, [this](const Event &e) {
            const int by = static_cast<int>(e.GetInt("by"));
            if (e.GetText("how") == "stomp" && by != id_) Banner(Format("%s stomped an enemy", ElementName(by)), ElementColor(by));
        });
        events_.Register(ev::TEMPLE_REACHED, [this](const Event &e) {
            const int who = static_cast<int>(e.GetInt("element"));
            const int inside = static_cast<int>(e.GetInt("inside")), active = static_cast<int>(e.GetInt("active"));
            if (who == id_)
                Banner(Format("You reached the %s temple (%d of %d inside) - waiting for teammates", ElementName(id_), inside, active), kGreen);
            else
                Banner(Format("%s reached the %s temple!  (%d of %d inside)", ElementName(who), ElementName(who), inside, active),
                       ElementColor(who), 4000);
        });
        events_.Register(ev::GATE_OPENED, [this](const Event &e) {
            const int by = static_cast<int>(e.GetInt("by", -1));
            Banner(by >= 0 ? Format("%s opened the gate", by == id_ ? "You" : ElementName(by)) : std::string("The gate opened"), kGreen);
        });
        events_.Register(ev::GATE_CLOSED, [this](const Event &) { Banner("The gate closed", kDim); });
        events_.Register(events::REPLAY_FINISHED, [this](const Event &) {
            if (pausedForReplay_) engine_.GetGameTime().Unpause();
            pausedForReplay_ = false;
            replayPauseAtMs_ = 0;
            replayCaptions_.clear();
            if (dying_) {
                ScheduleSpawn();
                Banner(Format("Respawning at checkpoint %d...", checkpoint_), kWhite, 1200);
            }
        });

        // Replayed events go to the replay's own manager, not the live game
        replay_.Events().RegisterAll([this](const Event &e) { OnReplayEvent(e); });
    }

    void OnChord(const Event &e) {
        if (e.GetObject("object") != PlayerObjectId(id_)) return;
        const std::string name = e.GetText("name");
        if (name == "burst") TryBurst();
        else if (name.rfind("dash_", 0) == 0) TryDash(name.substr(5));
    }

    void TryDash(const std::string &dir) {
        const double now = GameSeconds();
        if (dying_ || held_ || inDoor_ || now < nextDash_ || !dashReady_) return;
        dashReady_ = false;  // one dash per jump: landing gives it back
        dashDir_ = dir;
        dashUntil_ = now + kDashSeconds;
        nextDash_ = now + kDashCooldown;
    }

    void TryBurst() {
        if (dying_ || inDoor_) return;
        const Uint64 now = SDL_GetTicks();
        if (now < burstReadyMs_) {
            Banner(Format("Burst recharging: %.0f s", (burstReadyMs_ - now) / 1000.0), kDim);
            return;
        }
        burstReadyMs_ = now + static_cast<Uint64>(kBurstCooldown * 1000);
        // Routed to the server, which checks the cooldown
        events_.Raise(Event(ev::BURST_REQUEST).Int("element", id_));
    }

    void OnCollision(const Event &e) {
        World &world = net_.GetWorld();
        if (e.GetObject("a") != PlayerObjectId(id_)) return;
        GameObject *me = world.Find(PlayerObjectId(id_));
        GameObject *other = world.Find(e.GetObject("b"));
        if (!me || !other || dying_) return;
        if (other->Has<Enemy>()) {
            if (other->Id() == pendingStomp_) return;
            RigidBody &body = *me->Get<RigidBody>();
            const Transform &foe = *other->Get<Transform>();
            if (body.velocity.y > 0 && prevBottom_ <= foe.Top() + 14) {
                body.velocity.y = -kStompBounce;
                pendingStomp_ = other->Id();
                pendingStompUntil_ = SDL_GetTicks() + kStompWaitMs;
                events_.Raise(Event(ev::STOMP_REQUEST).Int("element", id_).Object("enemy", other->Id()));
                Banner("Stomp!", kYellow);
            } else if (GameSeconds() >= invulnerableUntil_) {
                events_.Raise(Event(events::DEATH, PRIORITY_HIGH).Object("object", me->Id()).Text("cause", "enemy"));
            }
        } else if (const Checkpoint *cp = other->Get<Checkpoint>(); cp && cp->index != checkpoint_) {
            // The last checkpoint crossed, either way, is the respawn point
            const bool forward = cp->index > checkpoint_;
            checkpoint_ = cp->index;
            me->Get<Respawn>()->spawnIndex = SpawnIndex(checkpoint_, id_);
            Banner(forward ? Format("Checkpoint %d reached - you respawn here, and your gems so far are safe", checkpoint_)
                           : Format("Back at checkpoint %d - you now respawn here", checkpoint_),
                   forward ? kGreen : kYellow);
        } else if (!e.GetBool("trigger") && e.GetText("side") == "floor") {
            if (const Surface *s = other->Get<Surface>(); s && s->style != Style::Goo)
                events_.Raise(Event(ev::SURFACE, PRIORITY_LOW).Int("style", static_cast<int>(s->style)));
        }
    }

    // Floor type sets the run speed
    void OnSurface(const Event &e) {
        const Style style = static_cast<Style>(e.GetInt("style"));
        runScale_ = SurfaceRunScale(style);
        if (style == surface_) return;
        surface_ = style;
        if (style == Style::Cloud) Banner("Cloud: soft footing - you run at 0.5x", kWhite);
        if (style == Style::Ice) Banner("Ice: slick - you run at 1.5x", {150, 215, 240, 255});
        if (style == Style::Basalt) Banner("Basalt: firm footing - normal speed (1x)", {255, 140, 50, 255});
    }

    void OnDeath(const Event &e) {
        World &world = net_.GetWorld();
        GameObject *me = world.Find(PlayerObjectId(id_));
        if (!me || e.GetObject("object") != me->Id() || dying_) return;
        std::string cause = e.GetText("cause");
        if (const GameObject *zone = world.Find(e.GetObject("zone")))
            cause = zone->Get<Transform>()->Top() > kGroundTop + 100 ? "pit" : "goo";
        dying_ = true;
        held_ = false;
        pendingStomp_ = NO_OBJECT;
        dashUntil_ = 0;
        me->Get<RigidBody>()->velocity = {0, 0};
        const char *what = cause == "pit" ? "fell into a pit" : cause == "goo" ? "fell into the goo" : "was caught by an enemy";
        Banner(Format("%s %s - your gems since checkpoint %d and your kills come back", ElementName(id_), what, checkpoint_), kRed, 3500);
        // Tell the server, so it can undo this player's progress
        events_.Raise(Event(ev::DEATH_REPORT).Int("element", id_).Text("cause", cause).Int("checkpoint", checkpoint_));
        diedAt_ = GameSeconds();
        if (deathCam_ && recorder_.Seconds() > 0.3)
            events_.Raise(Event(events::REPLAY_PLAY).Float("seconds", static_cast<float>(kReplaySeconds))
                              .Float("speed", static_cast<float>(kReplaySpeed)));
        else
            ScheduleSpawn();
    }

    // Spawn 1 s of game time after the death; game time spent in the death cam is paused
    void ScheduleSpawn() {
        const double left = std::max(0.0, kRespawnDelay - (GameSeconds() - diedAt_));
        respawns_.Spawn(PlayerObjectId(id_), left);
    }

    void OnSpawn(const Event &e) {
        if (e.Has("kind")) {  // the server's: what a death put back
            const int by = static_cast<int>(e.GetInt("by", e.GetInt("element", -1)));
            if (e.GetText("kind") == "enemy")
                Banner(by == id_ ? std::string("An enemy you defeated is back")
                                 : Format("An enemy is back - %s, who defeated it, died", ElementName(by)),
                       kRed);
            else if (by == id_)
                Banner("Your lost gems are back where you found them", GemColor(id_));
            return;
        }
        World &world = net_.GetWorld();
        GameObject *me = world.Find(PlayerObjectId(id_));
        if (!me || e.GetObject("object") != me->Id()) return;
        const bool wasDying = dying_;
        dying_ = false;
        me->Get<RigidBody>()->velocity = {0, 0};
        invulnerableUntil_ = GameSeconds() + kInvulnerableSeconds;
        Banner(wasDying ? Format("Respawned at checkpoint %d", checkpoint_) : Format("Back to checkpoint %d", checkpoint_), kWhite);
    }

    void OnBurst(const Event &e) {
        ActiveBurst b;
        b.element = static_cast<int>(e.GetInt("element"));
        b.id = static_cast<int>(e.GetInt("id"));
        b.center = e.GetPoint("center");
        b.radius = e.GetFloat("radius");
        b.target = e.GetObject("target");
        b.players = static_cast<int>(e.GetInt("players"));
        b.count = static_cast<int>(e.GetInt("count"));
        b.startedMs = SDL_GetTicks();
        bursts_.push_back(b);
        const std::string who = b.element == id_ ? std::string("Your") : ElementName(b.element);
        std::string what;
        switch (b.element) {
        case Fire: what = Format("defeated %d enemy(ies) nearby", b.count); break;
        case Water: what = b.count ? "froze the next moving platform for 4 s" : "found no moving platform ahead"; break;
        case Earth: what = b.count ? "raised a stone bridge over the next goo for 4 s" : "found no goo ahead"; break;
        case Air: what = Format("froze %d player(s) nearby for 4 s", b.count); break;
        }
        Banner(Format("%s%s burst! %s", who.c_str(), b.element == id_ ? (std::string(" ") + ElementName(id_)).c_str() : "", what.c_str()),
               ElementColor(b.element), 3500);
        if (b.element == id_) burstReadyMs_ = SDL_GetTicks() + static_cast<Uint64>(kBurstCooldown * 1000);
        if (b.element == Air && (b.players >> id_ & 1) && !dying_) {
            held_ = true;
            heldBurst_ = b.id;
            heldUntil_ = GameSeconds() + kBurstSeconds + 1.0;  // in case the end never arrives
            Banner("AIR's burst froze you for 4 s!", ElementColor(Air), 3000);
        }
    }

    void OnBurstEnd(const Event &e) {
        const int id = static_cast<int>(e.GetInt("id"));
        bursts_.erase(std::remove_if(bursts_.begin(), bursts_.end(), [id](const ActiveBurst &b) { return b.id == id; }), bursts_.end());
        if (held_ && heldBurst_ == id) held_ = false;
        const int element = static_cast<int>(e.GetInt("element"));
        if (element == Earth) Banner("The stone bridge crumbles", ElementColor(Earth), 1500);
    }

    void OnReplayEvent(const Event &e) {
        std::string text;
        if (e.Is(events::DEATH)) {
            const std::string cause = e.GetText("cause", "zone");
            text = cause == "enemy" ? "Caught by an enemy" : "Fell to your death";
        } else if (e.Is(events::SPAWN)) {
            if (e.Has("kind")) text = e.GetText("kind") == "enemy" ? "An enemy came back" : "Gems came back";
        } else if (e.Is(events::CHORD)) {
            const std::string name = e.GetText("name");
            text = name == "burst" ? "Burst!" : name == "dash_left" ? "Dash left" : name == "dash_right" ? "Dash right" : name;
        } else if (e.Is(ev::BURST)) {
            text = std::string(ElementName(static_cast<int>(e.GetInt("element")))) + " burst";
        } else if (e.Is(ev::ENEMY_DEFEATED)) {
            text = "Enemy defeated";
        } else if (e.Is(events::COLLISION) && e.GetBool("trigger")) {
            const GameObject *b = net_.GetWorld().Find(e.GetObject("b"));
            if (b && b->Has<Enemy>()) text = "Hit an enemy";
        }
        if (text.empty()) return;
        replayCaptions_.push_back(text);
        if (replayCaptions_.size() > 5) replayCaptions_.pop_front();
    }

    // Death cam: pause only my game time, shortly after the replay starts
    void UpdateReplayPause() {
        Timeline &time = engine_.GetGameTime();
        if (replay_.Playing() && !pausedForReplay_ && replayPauseAtMs_ == 0) replayPauseAtMs_ = SDL_GetTicks() + kReplayPauseDelayMs;
        if (replay_.Playing() && replayPauseAtMs_ && SDL_GetTicks() >= replayPauseAtMs_ && !pausedForReplay_) {
            if (!time.IsPaused()) {
                time.Pause();
                pausedForReplay_ = true;
            }
            replayPauseAtMs_ = 0;
        }
    }

    // --- keys, match, simulation ---

    static bool AnyPressed(std::initializer_list<SDL_Scancode> keys) {
        for (SDL_Scancode k : keys)
            if (Input::IsKeyJustPressed(k)) return true;
        return false;
    }

    void HandleKeys() {
        Timeline &time = engine_.GetGameTime();
        if (Input::IsKeyJustPressed(SDL_SCANCODE_ESCAPE)) engine_.Stop();
        if (Input::IsKeyJustPressed(SDL_SCANCODE_H)) showHelp_ = !showHelp_;
        if (showHelp_ && AnyPressed({SDL_SCANCODE_RETURN, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_LEFT,
                                     SDL_SCANCODE_RIGHT, SDL_SCANCODE_SPACE}))
            showHelp_ = false;
        if (showHelp_ && !helpDismissedOnce_ && SDL_GetTicks() > 15000) showHelp_ = false;
        if (!showHelp_) helpDismissedOnce_ = true;
        if (Input::IsKeyJustPressed(SDL_SCANCODE_O)) {
            showHidden_ = !showHidden_;
            Banner(showHidden_ ? "Showing hidden checkpoints and danger zones"
                               : "Hidden objects hidden again",
                   kYellow);
        }
        if (Input::IsKeyJustPressed(SDL_SCANCODE_V)) {
            deathCam_ = !deathCam_;
            if (!deathCam_ && replay_.Playing()) events_.Raise(Event(events::REPLAY_STOP));
            Banner(deathCam_ ? "Death cam ON: dying replays your last 3 s at 0.5x" : "Death cam OFF", kYellow);
        }

        if (Input::IsKeyJustPressed(SDL_SCANCODE_P) && !replay_.Playing()) {
            time.IsPaused() ? time.Unpause() : time.Pause();
            Banner(time.IsPaused() ? "PAUSED - your teammates keep playing"
                                   : "Resumed from exactly where you stopped",
                   time.IsPaused() ? kRed : kGreen);
        }
        double scale = time.GetScale();
        if (Input::IsKeyJustPressed(SDL_SCANCODE_LEFTBRACKET)) scale -= 0.5;
        if (Input::IsKeyJustPressed(SDL_SCANCODE_RIGHTBRACKET)) scale += 0.5;
        if (Input::IsKeyJustPressed(SDL_SCANCODE_1)) scale = 0.5;
        if (Input::IsKeyJustPressed(SDL_SCANCODE_2)) scale = 1.0;
        if (Input::IsKeyJustPressed(SDL_SCANCODE_3)) scale = 1.5;
        if (Input::IsKeyJustPressed(SDL_SCANCODE_4)) scale = 2.0;
        if (scale != time.GetScale()) {
            time.SetScale(scale);
            Banner(Format("Game speed %.1fx (just for you)", time.GetScale()), kYellow);
        }
        if (Input::IsKeyJustPressed(SDL_SCANCODE_TAB)) {
            Scaling::Toggle();
            Banner(Format("Scaling mode: %s", Scaling::GetModeName()), kWhite);
        }
    }

    Match *FindMatch(World &world) {
        Match *found = nullptr;
        world.Each<Match>([&](GameObject &, Match &m) { found = &m; });
        return found;
    }

    void FollowMatch(World &world, GameObject &me) {
        const Match *m = FindMatch(world);
        if (!m) return;
        if (m->round != round_) StartRound(world, me, m->round);

        const int gems = m->gems[id_];
        if (gems > myGems_)
            Banner(Format("+1 %s gem  (%d/%d)", ElementName(id_), gems, kGemsPerElement), GemColor(id_));
        myGems_ = gems;

        if (pendingStomp_ != NO_OBJECT && (!world.Find(pendingStomp_) || SDL_GetTicks() > pendingStompUntil_))
            pendingStomp_ = NO_OBJECT;

        if (rosterSeeded_) {
            for (int pid = 0; pid < kMaxPlayers; ++pid) {
                const uint32_t bit = 1u << pid;
                if (pid == id_ || (m->present & bit) == (knownPlayers_ & bit)) continue;
                if (m->present & bit)
                    Banner(Format("%s joined - %s", ElementName(pid),
                                  m->complete ? "they start the next round with everyone" : "they must reach their temple too"),
                           ElementColor(pid));
                else
                    Banner(Format("%s left the game - their player is gone from every screen", ElementName(pid)), kDim);
            }
        }
        knownPlayers_ = m->present;
        rosterSeeded_ = true;

        if (!m->complete) sawRoundInProgress_ = true;
        if (m->complete && !announcedComplete_) {
            announcedComplete_ = true;
            Banner(sawRoundInProgress_ ? "LEVEL COMPLETE!" : "You joined between rounds - next round soon",
                   sawRoundInProgress_ ? kGreen : kYellow, 5000);
        }
    }

    void StartRound(World &world, GameObject &me, int round) {
        checkpoint_ = round_ == 0 ? startCheckpoint_ : 0;
        round_ = round;
        pendingStomp_ = NO_OBJECT;
        announcedComplete_ = false;
        inDoor_ = false;
        held_ = false;
        bursts_.clear();
        triggers_.Clear();
        collisions_.Clear();
        if (dying_) {
            if (!respawns_.CancelRespawn(me.Id())) respawns_.Spawn(me.Id());  // still in the death cam
            dying_ = false;
        }
        if (placed_) SendToSpawn(world, me);
        if (round > 1) Banner(Format("Round %d - back to the start!", round), kYellow);
    }

    void SendToSpawn(World &world, GameObject &me) {
        me.Get<Respawn>()->spawnIndex = SpawnIndex(checkpoint_, id_);
        systems::PlaceAtSpawn(world, me, SpawnIndex(checkpoint_, id_));
        invulnerableUntil_ = GameSeconds() + kInvulnerableSeconds;
    }

    void Simulate(World &world, GameObject &me, float dt) {
        const ObjectFilter mine = net_.Authority();
        InputState &in = *me.Get<InputState>();
        Transform &t = *me.Get<Transform>();
        RigidBody &body = *me.Get<RigidBody>();
        const double now = GameSeconds();

        // Dead or frozen by AIR: stay in place
        if (held_ && now > heldUntil_) held_ = false;
        if (dying_ || held_) {
            in.buttons = 0;
            body.velocity = {0, 0};
            return;
        }
        in.buttons = inDoor_ ? 0u : keys_.buttons;
        me.Get<PlatformerController>()->moveSpeed = kRunSpeed * runScale_;

        prevBottom_ = t.Bottom();
        const PhysicsSettings physics{kGravity, kMaxFallSpeed, 1.0f / 120.0f};
        systems::ApplyControllers(world, mine);
        ApplyDash(body, now);
        contacts_.clear();
        systems::StepPhysics(world, dt, physics, mine, &contacts_);
        if (body.grounded && now >= dashUntil_) dashReady_ = true;
        t.position.x = std::clamp(t.position.x, 0.0f, kWorldWidth - kPlayerW);

        collisions_.Update(contacts_);  // one "collision" per meeting: floors become "surface" events
        respawns_.Update(mine);  // one "death" per fall into a death zone

        bool inDoor = false;
        const std::vector<TriggerEvent> found = triggers_.Update(world, mine);
        collisions_.Update(found);  // enemies, checkpoints (handled in OnCollision)
        for (const TriggerEvent &e : found) {
            if (e.type == TriggerEvent::Type::Exit) continue;
            const GameObject *trigger = world.Find(e.trigger);
            const Door *door = trigger ? trigger->Get<Door>() : nullptr;
            if (!door || door->element != id_) continue;
            const Transform &d = *trigger->Get<Transform>();
            const float cx = t.Center().x;
            inDoor = body.grounded && cx > d.Left() && cx < d.Right();
        }
        if (inDoor && !inDoor_) events_.Raise(Event(ev::TEMPLE_REQUEST).Int("element", id_));
        inDoor_ = inDoor;
    }

    void ApplyDash(RigidBody &body, double now) {
        if (now >= dashUntil_) return;
        if (dashDir_ == "left" || dashDir_ == "right") {
            body.velocity.x = dashDir_ == "left" ? -kDashSpeed : kDashSpeed;
            body.velocity.y = 0;  // a straight line through the air
        }
    }

    void WritePlayer(GameObject &me) {
        const Timeline &time = engine_.GetGameTime();
        Player &p = *me.Get<Player>();
        p.element = id_;
        p.checkpoint = checkpoint_;
        p.inDoor = inDoor_;
        p.paused = time.IsPaused() && !pausedForReplay_;
        p.scale = static_cast<float>(time.GetScale());
        p.sendsPerSecond = net_.GetClient().SendsPerSecond();
        p.dead = dying_;
    }

    void UpdateCamera(World &world) {
        int windowW = 0, windowH = 0;
        engine_.GetWindowSize(&windowW, &windowH);
        const float scale = engine_.GetScale();
        Camera &cam = *world.Find(cameraId_)->Get<Camera>();
        cam.view = {windowW / scale, windowH / scale};
        world.Each<ScrollBoundary>([&](GameObject &, ScrollBoundary &b) {
            switch (b.side) {
            case ScrollBoundary::Side::Left: b.inset = cam.view.x * 0.30f; break;
            case ScrollBoundary::Side::Right: b.inset = cam.view.x * 0.45f; break;
            case ScrollBoundary::Side::Top: b.inset = cam.view.y * 0.25f; break;
            case ScrollBoundary::Side::Bottom: b.inset = cam.view.y * 0.25f; break;
            }
        });
        systems::UpdateCameras(world);
    }

    // --- drawing ---

    void DrawNameTags(World &world, SDL_Renderer *r, const View &view) {
        world.Each<Player, Transform>([&](GameObject &, Player &p, Transform &t) {
            const bool me = p.element == id_;
            std::string tag = std::string(ElementName(p.element)) + (me ? " (you)" : "");
            if (!me && p.paused) tag += " (paused)";
            if (p.dead) tag += " (dead)";
            const float y = t.position.y - 18 - 16.0f * (p.element % 2) - 32.0f * (p.element / 2 % 2);
            DrawWorldText(r, view, t.Center().x - TextWidth(tag, 1.5f) / 2, y, tag, me ? kWhite : ElementColor(p.element), 1.5f);
        });
    }

    void DrawDashTrail(World &world, SDL_Renderer *r, const View &view) {
        if (GameSeconds() >= dashUntil_ + 0.08) return;
        const GameObject *me = world.Find(PlayerObjectId(id_));
        if (!me) return;
        const Transform &t = *me->Get<Transform>();
        const float dx = dashDir_ == "left" ? 1.0f : dashDir_ == "right" ? -1.0f : 0.0f;
        const float dy = 0.0f;
        for (int k = 3; k >= 1; --k)
            DrawWorldBox(r, view, t.position.x + dx * 26.0f * k, t.position.y + dy * 26.0f * k, t.size.x, t.size.y,
                         WithAlpha(ElementColor(id_), static_cast<Uint8>(150 - 40 * k)), true);
    }

    void DrawBursts(World &world, SDL_Renderer *r, const View &view) {
        const Uint64 now = SDL_GetTicks();
        bursts_.erase(std::remove_if(bursts_.begin(), bursts_.end(),
                                     [now](const ActiveBurst &b) { return now - b.startedMs > (kBurstSeconds + 2.0) * 1000; }),
                      bursts_.end());
        for (const ActiveBurst &b : bursts_) {
            const double age = (now - b.startedMs) / 1000.0;
            const double left = std::max(0.0, kBurstSeconds - age);
            const SDL_Color c = ElementColor(b.element);
            // Burst ring: radius for FIRE and AIR, a pulse for the others
            if (age < 1.0) {
                const float radius = b.radius > 0 ? b.radius : static_cast<float>(60 + 140 * age);
                DrawWorldCircle(r, view, b.center, radius, WithAlpha(c, static_cast<Uint8>(255 * (1.0 - age * 0.6))), 4);
                if (b.radius > 0) DrawWorldCircle(r, view, b.center, static_cast<float>(radius * age), WithAlpha(c, 120), 2);
            }
            if (const GameObject *target = world.Find(b.target)) {
                const Transform &t = *target->Get<Transform>();
                if (b.element == Water) {
                    DrawWorldBox(r, view, t.position.x - 4, t.position.y - 4, t.size.x + 8, t.size.y + 8, WithAlpha(kIce, 110), true);
                    DrawWorldText(r, view, t.position.x, t.position.y - 22, Format("FROZEN %.1fs", left), kIce, 1.5f);
                } else if (b.element == Earth) {
                    DrawWorldText(r, view, t.position.x, t.position.y - 22, Format("STONE BRIDGE %.1fs", left), ElementColor(Earth), 1.5f);
                }
            }
            if (b.element == Air) {
                world.Each<Player, Transform>([&](GameObject &, Player &p, Transform &t) {
                    if (!(b.players >> p.element & 1)) return;
                    DrawWorldBox(r, view, t.position.x - 6, t.position.y - 6, t.size.x + 12, t.size.y + 12, WithAlpha(kIce, 220), false);
                    // Just the countdown, centred: neighbours stand too close for a longer label
                    const std::string label = Format("%.1fs", left);
                    DrawWorldText(r, view, t.Center().x - TextWidth(label, 1.0f) / 2 / view.scale, t.Bottom() + 6, label, kIce, 1.0f);
                });
            }
        }
    }

    void RenderReplay(World &live, SDL_Renderer *r, const Camera &liveCam, int windowW, int windowH) {
        World &world = replay_.PlaybackWorld();
        const float scale = engine_.GetScale();
        View view = View::From(liveCam, scale);
        if (const GameObject *me = world.Find(PlayerObjectId(id_))) {
            const Vec2 c = me->Get<Transform>()->Center();
            view.x = std::clamp(c.x - view.w * 0.45f, 0.0f, std::max(0.0f, kWorldWidth - view.w));
            view.y = std::clamp(c.y - view.h * 0.55f, 0.0f, std::max(0.0f, kWorldHeight - view.h));
        }
        DrawBackground(r, view, art_, windowW, windowH);
        DrawRules rules;
        rules.myElement = id_;
        rules.me = PlayerObjectId(id_);
        RenderScene(world, r, view, art_, rules);
        live.Each<Transform, Sign>([&](GameObject &, Transform &t, Sign &s) {
            DrawWorldText(r, view, t.position.x, t.position.y, s.text, {255, 255, 210, 160}, std::max(1.0f, 2.0f * view.scale));
        });
        DrawNameTags(world, r, view);

        // Replay overlay: frame, title, progress and event captions
        SDL_SetRenderDrawColor(r, 220, 40, 40, 255);
        for (int k = 0; k < 6; ++k) {
            const SDL_FRect f{static_cast<float>(k), static_cast<float>(k), windowW - 2.0f * k, windowH - 2.0f * k};
            SDL_RenderRect(r, &f);
        }
        const float cx = windowW / 2.0f;
        const std::string title = Format("REPLAY %.1fx - the last %.0f s before your death", replay_.Speed(), kReplaySeconds);
        const float ts = windowW >= 1400 ? 3.0f : 2.0f;
        DrawPanel(r, cx - TextWidth(title, ts) / 2 - 16, 20, TextWidth(title, ts) + 32, 12 * ts + 8, 200);
        DrawText(r, cx - TextWidth(title, ts) / 2, 26, title, kRed, ts);
        const float barW = std::min(600.0f, windowW - 80.0f);
        DrawPanel(r, cx - barW / 2, 74, barW, 14, 200);
        SDL_SetRenderDrawColor(r, 220, 60, 60, 255);
        const SDL_FRect bar{cx - barW / 2, 74, barW * static_cast<float>(replay_.Progress()), 14};
        SDL_RenderFillRect(r, &bar);
        float y = 100;
        for (const std::string &c : replayCaptions_) {
            DrawPanel(r, 14, y - 4, TextWidth(c, 1.5f) + 16, 20, 180);
            DrawText(r, 22, y, c, kYellow, 1.5f);
            y += 24;
        }
        const std::string hint = "V turns the death cam off";
        DrawText(r, cx - TextWidth(hint, 1.5f) / 2, windowH - 40.0f, hint, kDim, 1.5f);
    }

    struct HudLine {
        std::string text;
        SDL_Color color;
    };

    // Top-left: this player. Top-right: the round and the team.
    void DrawHud(World &world, SDL_Renderer *r, int windowW, int windowH) {
        const Timeline &time = engine_.GetGameTime();
        const Match *m = FindMatch(world);
        const float scale = engine_.GetScale();
        const float size = std::max(1.0f, 1.25f * scale);
        const float unit = size / 1.25f;
        const float line = 8 * size + 3 * unit;
        const float margin = 6 * scale;
        int teamGems = 0;
        if (m)
            for (int g : m->gems) teamGems += g;
        const GameObject *me = world.Find(PlayerObjectId(id_));
        const int deaths = me ? me->Get<Respawn>()->deaths : 0;

        std::vector<HudLine> mine;
        mine.push_back({Format("%s (you)   Gems %d/%d   Deaths %d   Checkpoint %d/%d", ElementName(id_), myGems_,
                               kGemsPerElement, deaths, checkpoint_, kCheckpoints),
                        ElementColor(id_)});
        const Uint64 now = SDL_GetTicks();
        if (now >= burstReadyMs_)
            mine.push_back({Format("Burst (J+K) ready: %s", BurstEffect(id_)), ElementColor(id_)});
        else
            mine.push_back({Format("Burst (J+K) recharging: %.0fs", (burstReadyMs_ - now) / 1000.0), kDim});
        if (!net_.GetClient().Connected())
            mine.push_back({"Connection lost - waiting for the server", kRed});
        else if (time.GetScale() != 1.0)
            mine.push_back({Format("Game speed %.1fx", time.GetScale()), kYellow});
        mine.push_back({"H  help", kDim});

        std::vector<HudLine> team;
        team.push_back({Format("Round %d   Team gems %d/%d", std::max(round_, 1), teamGems, kGemsPerElement * kMaxPlayers), kWhite});
        for (int pid = 0; pid < kMaxPlayers; ++pid)
            if (pid != id_ && m && (m->present >> pid & 1u)) team.push_back(RosterLine(world, m, pid));

        auto panelWidth = [&](const std::vector<HudLine> &lines) {
            float width = 0;
            for (const HudLine &l : lines) width = std::max(width, TextWidth(l.text, size));
            return width + 16 * unit;
        };
        auto drawPanel = [&](const std::vector<HudLine> &lines, float x, float y, float w) {
            DrawPanel(r, x, y, w, line * lines.size() + 10 * unit);
            float ty = y + 6 * unit;
            for (const HudLine &l : lines) {
                DrawText(r, x + 8 * unit, ty, l.text, l.color, size);
                ty += line;
            }
        };
        const float mineW = panelWidth(mine), teamW = panelWidth(team);
        drawPanel(mine, margin, margin, mineW);
        // Side by side when they fit, otherwise the team panel goes under this player's
        if (margin * 3 + mineW + teamW <= windowW)
            drawPanel(team, windowW - margin - teamW, margin, teamW);
        else
            drawPanel(team, margin, margin * 2 + line * mine.size() + 10 * unit, teamW);

        if (!haveWorld_) {
            const std::string msg = "Waiting for the server...";
            DrawText(r, windowW / 2.0f - TextWidth(msg, 2) / 2, windowH / 2.0f, msg, kYellow, 2);
        }
    }

    HudLine RosterLine(World &world, const Match *m, int pid) {
        const GameObject *o = world.Find(PlayerObjectId(pid));
        if (!o) return {Format("%-5s  joining...", ElementName(pid)), kDim};
        const Player &p = *o->Get<Player>();
        const bool active = m->active >> pid & 1u;
        std::string status;
        if (p.dead) status = "  DEAD";
        else if (p.inDoor) status = "  IN TEMPLE";
        else if (p.paused) status = "  PAUSED";
        else if (!active) status = "  away";
        return {Format("%-5s  Gems %d   Checkpoint %d%s", ElementName(pid), m->gems[pid], p.checkpoint, status.c_str()),
                active ? ElementColor(pid) : kDim};
    }

    void DrawOverlays(World &world, SDL_Renderer *r, int windowW, int windowH) {
        const float cx = windowW / 2.0f;
        const Match *m = FindMatch(world);
        if (m && m->complete) {
            int team = 0;
            for (int g : m->gems) team += g;
            const double next = std::max(0.0, kNextRoundSeconds - (m->serverTime - m->completeAt));
            const std::string a = "LEVEL COMPLETE!";
            const std::string b = sawRoundInProgress_
                                      ? Format("Team gems %d/%d   -   next round in %.0fs", team, kGemsPerElement * kMaxPlayers, next)
                                      : Format("You joined between rounds - you start with everyone in %.0fs", next);
            DrawPanel(r, cx - 360, windowH * 0.35f - 20, 720, 120, 245);
            DrawText(r, cx - TextWidth(a, 5) / 2, windowH * 0.35f, a, kGreen, 5);
            DrawText(r, cx - TextWidth(b, 2) / 2, windowH * 0.35f + 60, b, kWhite, 2);
        } else if (inDoor_ && m) {
            int active = 0, ready = 0;
            world.Each<Player>([&](GameObject &, Player &p) {
                if (!(m->active >> p.element & 1u)) return;
                ++active;
                ready += p.inDoor ? 1 : 0;
            });
            const std::string msg = Format("In your door: %d/%d active players ready", ready, std::max(active, 1));
            DrawText(r, cx - TextWidth(msg, 2) / 2, windowH - 60.0f, msg, kGreen, 2);
        }
        if (dying_ && !replay_.Playing()) {
            const std::string msg = "RESPAWNING...";
            DrawText(r, cx - TextWidth(msg, 4) / 2, windowH * 0.22f, msg, kRed, 4);
        } else if (held_) {
            const std::string msg = "FROZEN BY AIR";
            DrawText(r, cx - TextWidth(msg, 4) / 2, windowH * 0.22f, msg, kIce, 4);
        } else if (engine_.GetGameTime().IsPaused()) {
            const std::string msg = "PAUSED  (P to resume)";
            DrawText(r, cx - TextWidth(msg, 4) / 2, windowH * 0.22f, msg, kRed, 4);
        }
        if (SDL_GetTicks() < bannerUntil_) {
            DrawPanel(r, cx - TextWidth(banner_, 2) / 2 - 14, windowH - 118.0f, TextWidth(banner_, 2) + 28, 36);
            DrawText(r, cx - TextWidth(banner_, 2) / 2, windowH - 108.0f, banner_, bannerColor_, 2);
        }
        if (showHelp_) DrawHelp(r, windowW, windowH);
    }

    void DrawHelp(SDL_Renderer *r, int windowW, int windowH) {
        const std::vector<std::string> lines{
            "ELEMENTAL QUEST  -  a co-op platformer for FIRE, WATER, EARTH and AIR",
            "",
            Format("You are %s. Collect %s gems and stand in the %s door (far right).", ElementName(id_),
                   ElementName(id_), ElementName(id_)),
            "The level is complete when every ACTIVE player stands in their own door.",
            "Goo, pits and enemies kill you: you respawn at your checkpoint 1 s later.",
            "A death puts back the gems you took since that checkpoint, and every",
            "enemy YOU defeated this round comes back to life for everyone.",
            "Clouds slow your run to 0.5x, ice speeds it to 1.5x, basalt keeps it at 1x.",
            "Pressure plates open the gate; stand on a teammate to reach the high ledge.",
            "",
            "DASH   M + Left/Right, pressed TOGETHER: a straight air-dash, one per jump",
            Format("BURST  J + K together: %s (4 s, 8 s recharge)", BurstEffect(id_)),
            "MOVE   A/D or Left/Right        JUMP   Space / W / Up",
            "PAUSE  P    SPEED  1-4    DEATH CAM  V    BACK TO CHECKPOINT  R",
            "SCREENSHOT  F12    HELP  H    QUIT  Esc",
            "",
            "Press H, Enter or start moving to play (closes by itself after 15 s).",
        };
        const float size = windowW >= 1600 ? 2.0f : 1.5f;
        const float line = 8 * size + 8;
        float width = 0;
        for (const std::string &l : lines) width = std::max(width, TextWidth(l, size));
        const float x = (windowW - width) / 2, y = std::max(20.0f, (windowH - line * lines.size()) / 2);
        DrawPanel(r, x - 24, y - 20, width + 48, line * lines.size() + 40, 225);
        for (size_t i = 0; i < lines.size(); ++i)
            DrawText(r, x, y + line * i, lines[i], i == 0 ? ElementColor(id_) : kWhite, size);
    }

    void SaveScreenshot(SDL_Renderer *r) {
        SDL_Surface *shot = SDL_RenderReadPixels(r, nullptr);
        if (!shot) return;
        const std::string name = Format("screenshot_P%d_%s_%llu.png", id_, ElementName(id_),
                                        static_cast<unsigned long long>(SDL_GetTicks()));
        if (IMG_SavePNG(shot, name.c_str())) SDL_Log("[screenshot] saved %s", name.c_str());
        SDL_DestroySurface(shot);
    }

    void Banner(const std::string &text, SDL_Color color, Uint64 ms = kBannerMs) {
        banner_ = text;
        bannerColor_ = color;
        bannerUntil_ = SDL_GetTicks() + ms;
    }

    void LogTrafficEvery5s(World &world) {
        const Uint64 ticks = SDL_GetTicks();
        const NetStats &s = net_.Stats();
        if (ticks - trafficLogStart_ < 5000) return;
        const double seconds = (ticks - trafficLogStart_) / 1000.0;
        trafficLogStart_ = ticks;
        const uint64_t out = s.messagesOut - log_.out, in = s.messagesIn - log_.in, bytesIn = s.bytesIn - log_.bytesIn;
        log_ = {s.messagesOut.load(), s.messagesIn.load(), s.bytesIn.load()};
        int others = 0;
        world.Each<Player>([&](GameObject &, Player &p) { others += p.element != id_; });

        const char *me = ElementName(id_);
        const double avg = in ? static_cast<double>(bytesIn) / in : 0.0;
        const LatencySummary lat = s.Latency();
        SDL_Log("[net] P%d %s | CLIENT-SERVER | to server: %llu player snapshots | from server: %llu world "
                "snapshots (%.0f bytes avg) carrying the other %d player(s) | latency %.1f ms "
                "(%.0f s window)",
                id_, me, static_cast<unsigned long long>(out), static_cast<unsigned long long>(in), avg, others,
                lat.meanMs, seconds);
        const uint64_t raised = events_.RaisedCount(), handled = events_.HandledCount();
        SDL_Log("[events] P%d %s | raised %llu, handled %llu here | requests to the server: %llu | events from the "
                "server: %llu | replay buffer %.1f s",
                id_, me, static_cast<unsigned long long>(raised - log_.raised), static_cast<unsigned long long>(handled - log_.handled),
                static_cast<unsigned long long>(s.eventsOut.load()), static_cast<unsigned long long>(s.eventsIn.load()),
                recorder_.Seconds());
        log_.raised = raised;
        log_.handled = handled;
    }

    double GameSeconds() const { return playerClock_.GetSeconds(); }

    Engine &engine_;
    SceneClient &net_;
    const int id_;
    const Options options_;
    Art &art_;

    ObjectId cameraId_ = NO_OBJECT;
    TriggerTracker triggers_;
    Timeline playerClock_;

    // Event system members, in construction order
    EventManager events_;
    InputEvents input_;
    ChordEvents chords_;
    CollisionEvents collisions_;
    RespawnEvents respawns_;
    ReplayRecorder recorder_;
    Timeline replayClock_{nullptr, 1};  // real time, so playback runs while MY game time is paused
    ReplayPlayer replay_;
    InputState keys_;  // the buttons the input events say are held
    std::vector<Contact> contacts_;
    std::vector<ActiveBurst> bursts_;
    std::deque<std::string> replayCaptions_;

    double invulnerableUntil_ = 0;
    double walkClock_ = 0;
    bool haveWorld_ = false;
    bool placed_ = false;
    int checkpoint_ = 0;
    int startCheckpoint_ = 0;
    bool inDoor_ = false;
    bool dying_ = false;
    double diedAt_ = 0;
    bool held_ = false;
    int heldBurst_ = 0;
    double heldUntil_ = 0;
    std::string dashDir_;
    double dashUntil_ = 0, nextDash_ = 0;
    bool dashReady_ = true;
    float prevBottom_ = 0;
    float runScale_ = 1.0f;
    Uint64 burstReadyMs_ = 0;
    bool deathCam_ = true;
    bool pausedForReplay_ = false;
    Uint64 replayPauseAtMs_ = 0;
    int myGems_ = 0;
    ObjectId pendingStomp_ = NO_OBJECT;
    Uint64 pendingStompUntil_ = 0;
    Style surface_ = Style::Ground;
    int round_ = 0;
    bool announcedComplete_ = false;
    bool sawRoundInProgress_ = false;
    uint32_t knownPlayers_ = 0;
    bool rosterSeeded_ = false;
    bool showHelp_ = true;
    bool helpDismissedOnce_ = false;
    bool showHidden_ = false;
    std::string banner_;
    SDL_Color bannerColor_{255, 255, 255, 255};
    Uint64 bannerUntil_ = 0;
    Uint64 autoshotAtMs_ = 0;
    Uint64 trafficLogStart_ = SDL_GetTicks();
    struct {
        uint64_t out = 0, in = 0, bytesIn = 0, raised = 0, handled = 0;
    } log_;
};
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Options options;
    std::string error;
    if (!ParseOptions(argc, argv, options, error)) {
        std::fprintf(stderr,
                     "%s\nusage: %s [--host HOST] [--scale 0.5|1|1.5|2] [--window WxH] [--pos X,Y] [--checkpoint N] [--show-hidden] "
                     "[--no-help] [--walk] [--autoshot SECONDS] [--event-log] [--no-deathcam] [--encoding text|binary] "
                     "[--delta on|off] [--keyframe N]\n",
                     error.c_str(), argv[0]);
        return 2;
    }
    RegisterGameComponents();

    Client::Config netConfig;
    netConfig.host = options.host;
    netConfig.joinPort = kJoinPort;
    netConfig.worldPort = kWorldPort;
    netConfig.sendIntervalSeconds = 1.0f / 60.0f;
    SceneClient net(netConfig, options.net);

    if (!net.Join()) {
        const bool full = net.LastError().find("full") != std::string::npos;
        std::fprintf(stderr, "Could not join: %s\n%s\n", net.LastError().c_str(),
                     full ? "All 4 elements are taken. Close a player's window to free its slot."
                          : "Start ./build/elemental_server first (or check the --host address and that ports 7555, 7556 and 7600-7603 are reachable).");
        return 1;
    }
    const int id = net.Id();
    if (id < 0 || id >= kMaxPlayers) {
        std::fprintf(stderr, "Server gave id %d; only %d elements exist.\n", id, kMaxPlayers);
        return 1;
    }

    Engine engine;
    const std::string title = Format("Elemental Quest - %s", ElementName(id));
    if (!engine.Init(title.c_str())) return 1;
    SDL_SetRenderVSync(engine.GetRenderer(), 1);
    if (options.windowW > 0 && options.windowH > 0) SDL_SetWindowSize(engine.GetWindow(), options.windowW, options.windowH);
    if (options.posX != INT_MIN) SDL_SetWindowPosition(engine.GetWindow(), options.posX, options.posY);
    engine.GetGameTime().SetScale(options.scale);

    Art art;
    art.Load(engine.GetRenderer());
    g_engine = &engine;
    std::signal(SIGINT, OnQuitSignal);
    std::signal(SIGTERM, OnQuitSignal);
    sigset_t quitSignals;
    sigemptyset(&quitSignals);
    sigaddset(&quitSignals, SIGINT);
    sigaddset(&quitSignals, SIGTERM);
    pthread_sigmask(SIG_UNBLOCK, &quitSignals, nullptr);
    {
        Game game(engine, net, art, options);
        engine.Run([&](float dt) { game.Update(dt); }, [&]() { game.Render(); },
                   [&](float dt) { game.Background(dt); });
    }
    g_engine = nullptr;
    net.Leave();
    art.Destroy();
    engine.Shutdown();
    return 0;
}
