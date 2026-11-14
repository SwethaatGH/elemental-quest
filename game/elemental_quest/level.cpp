#include "level.hpp"

#include "engine/Systems.hpp"

#include <algorithm>
#include <random>
#include <string>

using namespace engine;

namespace ed {
namespace {
struct Box {
    float x, y, w, h;
};

GameObject &Object(World &w, const char *kind, const Box &b) {
    GameObject &o = w.Create(kind);
    o.Add<Transform>(b.x, b.y, b.w, b.h);
    o.Add<NetIdentity>();
    return o;
}

SDL_Color StyleColor(Style s) {
    switch (s) {
    case Style::Ground: return {88, 60, 38, 255};
    case Style::Basalt: return {58, 38, 42, 255};
    case Style::Ice: return {150, 215, 240, 235};
    case Style::Moss: return {110, 98, 80, 255};
    case Style::Cloud: return {238, 243, 252, 235};
    case Style::Timber: return {139, 94, 52, 255};
    case Style::Lift: return {194, 160, 104, 255};
    case Style::Glider: return {168, 88, 56, 255};
    case Style::Goo: return {18, 14, 22, 255};
    }
    return {255, 0, 255, 255};
}

GameObject &Platform(World &w, const char *kind, const Box &b, Style style) {
    GameObject &o = Object(w, kind, b);
    o.Add<Collider>(Collider::Type::Solid, style == Style::Cloud);
    o.Add<Renderable>(StyleColor(style), kLayerPlatform);
    o.Add<Surface>(style);
    return o;
}

GameObject &Mover(World &w, const Box &b, float dx, float dy, float period, float phase = 0.0f) {
    const Style style = dy == 0.0f ? Style::Timber : dx == 0.0f ? Style::Lift : Style::Glider;
    GameObject &o = Platform(w, "mover", b, style);
    PathMover &p = o.Add<PathMover>();
    p.pattern = PathMover::Pattern::Linear;
    p.origin = {b.x, b.y};
    p.offset = {dx, dy};
    p.period = period;
    p.phase = phase;
    return o;
}

void SpawnPoints(World &w, int checkpoint, float x) {
    for (int e = 0; e < kMaxPlayers; ++e) {
        GameObject &s = Object(w, "spawn", {x + 60.0f * e, kGroundTop - 100.0f, kPlayerW, 100.0f});
        s.Add<SpawnPoint>(SpawnIndex(checkpoint, e));
    }
}

void DeathZoneAt(World &w, const Box &b) {
    Object(w, "deathzone", b).Add<DeathZone>();
}

const float kFlags[kCheckpoints] = {1450, 3480, 6050, 7670};
}

void BuildLevel(World &w) {
    Platform(w, "ground", {0, 900, 1100, 180}, Style::Ground);
    Platform(w, "ledge", {820, 760, 200, 30}, Style::Moss);
    Platform(w, "ground", {1280, 900, 920, 180}, Style::Ground);
    Platform(w, "ledge", {2350, 780, 260, 30}, Style::Cloud);
    Platform(w, "ledge", {2700, 660, 260, 30}, Style::Ice);
    Platform(w, "ledge", {3050, 780, 260, 30}, Style::Basalt);
    Platform(w, "ground", {3450, 900, 1150, 180}, Style::Ground);
    Platform(w, "ground", {6000, 900, 1900, 180}, Style::Ground);
    Platform(w, "ledge", {6420, 660, 220, 30}, Style::Basalt);
    Platform(w, "ledge", {8050, 780, kTempleGapLeft - 8050, 30}, Style::Basalt);  // firm footing before the temple gap
    Platform(w, "ground", {kTempleGapRight, 900, kWorldWidth - kTempleGapRight, 180}, Style::Ground);

    Mover(w, {4720, 820, 320, 26}, 240, 0, 4.0f);
    Mover(w, {5380, 860, 200, 26}, 0, -240, 5.0f);
    Mover(w, {5620, 760, 220, 26}, 160, 90, 4.5f);

    const Box goo[] = {{700, 884, 90, 22},  {1800, 884, 110, 22}, {3760, 884, 100, 22},
                       {7250, 884, 90, 22}, {8600, 764, 80, 22}};
    for (const Box &b : goo) {
        GameObject &o = Object(w, "goo", b);
        o.Add<Renderable>(StyleColor(Style::Goo), kLayerGoo);
        o.Add<Surface>(Style::Goo);
        DeathZoneAt(w, b);
    }
    DeathZoneAt(w, {-500, 1150, kWorldWidth + 1000, 400});

    SpawnPoints(w, 0, 120);
    for (int c = 1; c <= kCheckpoints; ++c) {
        SpawnPoints(w, c, kFlags[c - 1]);
        GameObject &cp = Object(w, "checkpoint", {kFlags[c - 1] - 20, 0, 60, kGroundTop});
        cp.Add<Collider>(Collider::Type::Trigger);
        cp.Add<Checkpoint>().index = c;
    }

    for (const Box &b : {Box{7050, 888, 100, 12}, Box{7600, 888, 100, 12}}) {
        GameObject &p = Object(w, "plate", b);
        p.Add<Collider>(Collider::Type::Trigger);
        p.Add<Renderable>(SDL_Color{255, 215, 90, 255}, kLayerProps);
        p.Add<Plate>();
    }
    GameObject &gate = Object(w, "gate", {7450, 560, 40, 340});
    gate.Add<Collider>();
    gate.Add<Renderable>(SDL_Color{150, 160, 180, 255}, kLayerProps);
    gate.Add<Gate>();

    for (int e = 0; e < kMaxPlayers; ++e) {
        GameObject &d = Object(w, "door", {9300.0f + 120.0f * e, kGroundTop - 130.0f, 90, 130});
        d.Add<Collider>(Collider::Type::Trigger);
        Renderable &r = d.Add<Renderable>(ElementColor(e), kLayerProps);
        r.sprite = std::string("door_") + ElementFile(e) + ".png";
        d.Add<Door>().element = e;
    }

    GameObject &match = w.Create("match");
    match.Add<Match>();
    match.Add<NetIdentity>();

    BuildRoundObjects(w);
}

namespace {
struct Patrol {
    float minX, maxX, top, period;
};
const Patrol kPatrols[kEnemies] = {{3900, 4400, 900, 9.0f}, {6680, 6980, 900, 8.0f}, {8100, 8520, 780, 7.0f}};
const SDL_FPoint kClusters[kGemsPerElement] = {
    {845, 700},  {1115, 760}, {2720, 610}, {4020, 845},
    {5395, 585}, {6435, 615}, {8300, 725}, {8840, 740},  // the last hang in the temple gap, at dash height
};
}

GameObject &BuildEnemy(World &w, int index) {
    const Patrol &p = kPatrols[std::clamp(index, 0, kEnemies - 1)];
    GameObject &o = Object(w, "enemy", {p.maxX, p.top - kEnemyH, kEnemyW, kEnemyH});
    o.Add<Collider>(Collider::Type::Trigger);
    Renderable &r = o.Add<Renderable>(SDL_Color{125, 25, 45, 255}, kLayerEnemy);
    r.sprite = "enemy.png";
    PathMover &m = o.Add<PathMover>();
    m.origin = {p.minX, p.top - kEnemyH};
    m.offset = {p.maxX - p.minX, 0};
    m.period = p.period;
    m.phase = 0.5f;
    o.Add<Enemy>().index = index;
    return o;
}

GameObject &BuildGem(World &w, float x, float y, int element) {
    GameObject &g = Object(w, "gem", {x, y, kGemSize, kGemSize});
    g.Add<Collider>(Collider::Type::Trigger);
    Renderable &r = g.Add<Renderable>(GemColor(element), kLayerGem);
    r.sprite = std::string("gem_") + ElementFile(element) + ".png";
    g.Add<Gem>().element = element;
    return g;
}

GameObject &BuildBridge(World &w, const Transform &goo) {
    // Bridge sits 80 px above the goo, so walking under it still kills
    GameObject &o = Platform(w, "bridge", {goo.position.x - 70, goo.position.y - 80, goo.size.x + 140, 20}, Style::Moss);
    o.Get<Collider>()->oneWay = true;
    return o;
}

void BuildRoundObjects(World &w) {
    for (int i = 0; i < kEnemies; ++i) BuildEnemy(w, i);
    for (const SDL_FPoint &c : kClusters)
        for (int e = 0; e < kMaxPlayers; ++e) BuildGem(w, c.x + 45.0f * e, c.y, e);
}

void BuildBenchLevel(World &w, int statics, int moving) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> x(0.0f, 19800.0f), y(300.0f, 1800.0f), width(80.0f, 400.0f),
        height(16.0f, 40.0f), period(2.0f, 6.0f), phase(0.0f, 1.0f);

    Platform(w, "ground", {0, 1900, 20000, 100}, Style::Ground);
    const Style looks[] = {Style::Basalt, Style::Ice, Style::Moss, Style::Cloud};
    for (int i = 0; i < statics; ++i) {
        const Box b{x(rng), y(rng), width(rng), height(rng)};
        Platform(w, "ledge", b, looks[i % 4]);
    }
    for (int i = 0; i < moving; ++i) {
        const Box b{x(rng), y(rng), 120, 20};
        const float p = period(rng), ph = phase(rng);
        switch (i % 3) {
        case 0: Mover(w, b, 300, 0, p, ph); break;
        case 1: Mover(w, b, 0, 200, p, ph); break;
        default: Mover(w, b, 200, 150, p, ph); break;
        }
    }
    for (int e = 0; e < kMaxPlayers; ++e) {
        GameObject &s = Object(w, "spawn", {200.0f + 400.0f * e, 1800, kPlayerW, 100});
        s.Add<SpawnPoint>(SpawnIndex(0, e));
    }
    DeathZoneAt(w, {-2000, 2100, 24000, 400});
    for (int c = 1; c <= kCheckpoints; ++c) {
        GameObject &cp = Object(w, "checkpoint", {4000.0f * c, 0, 60, 1900});
        cp.Add<Collider>(Collider::Type::Trigger);
        cp.Add<Checkpoint>().index = c;
    }
    GameObject &gate = Object(w, "gate", {10000, 1560, 40, 340});
    gate.Add<Collider>();
    gate.Add<Renderable>(SDL_Color{150, 160, 180, 255}, kLayerProps);
    gate.Add<Gate>();
    GameObject &match = w.Create("match");
    match.Add<Match>();
    match.Add<NetIdentity>();
}

GameObject &MakePlayer(World &w, int id) {
    GameObject &p = w.CreateWithId(PlayerObjectId(id), "player");
    p.Add<Transform>(0.0f, 0.0f, kPlayerW, kPlayerH);
    p.Add<RigidBody>().gravity = true;
    p.Add<Collider>(Collider::Type::Solid, true);
    Renderable &r = p.Add<Renderable>(ElementColor(id), kLayerPlayer);
    r.sprite = std::string("player_") + ElementFile(id) + ".png";
    p.Add<InputState>();
    PlatformerController &pc = p.Add<PlatformerController>();
    pc.moveSpeed = kRunSpeed;
    pc.jumpSpeed = kJumpSpeed;
    p.Add<Respawn>().spawnIndex = SpawnIndex(0, id);
    p.Add<Player>().element = id;
    p.Add<NetIdentity>(id);
    systems::PlaceAtSpawn(w, p, SpawnIndex(0, id));
    return p;
}

float SurfaceRunScale(Style style) {
    switch (style) {
    case Style::Cloud: return 0.5f;
    case Style::Ice: return 1.5f;
    default: return 1.0f;
    }
}

const Surface *ApplySurfaceRunSpeed(World &w, GameObject &player) {
    const RigidBody *body = player.Get<RigidBody>();
    PlatformerController *pc = player.Get<PlatformerController>();
    if (!body || !pc || !body->grounded) return nullptr;
    const GameObject *ground = w.Find(body->ground);
    const Surface *surface = ground ? ground->Get<Surface>() : nullptr;
    if (!surface) return nullptr;
    pc->moveSpeed = kRunSpeed * SurfaceRunScale(surface->style);
    return surface;
}

void BuildSigns(World &w) {
    struct Text {
        float x, y;
        const char *text;
    };
    const Text signs[] = {
        {60, 600, "ELEMENTAL QUEST - reach the door of YOUR element far to the right  -->"},
        {520, 640, "Black goo is deadly: JUMP over it (EARTH: J+K bridges it)"},
        {1080, 670, "Mind the gap!"},
        {1330, 530, "Hidden checkpoints: the last one you cross (either way) is your respawn"},
        {1330, 560, "Dying drops the gems you took since your checkpoint"},
        {2240, 560, "Cloud: soft, run 0.5x  Ice: slick, 1.5x  Basalt: firm, 1x"},
        {3860, 580, "Enemies: stomp them by landing on them from above"},
        {3860, 610, "FIRE: J+K burns every enemy nearby"},
        {3860, 640, "Your kills come back to life when YOU die"},
        {4660, 520, "Raft (side to side), lift (up/down), glider (diagonal)"},
        {4660, 550, "WATER: J+K freezes the next moving platform"},
        {6300, 560, "High ledge: stand on a teammate's head"},
        {6980, 620, "Pressure plate opens the gate"},
        {8080, 600, "Jump the gap to the temples - or dash: M + RIGHT together"},
        {8080, 630, "The gems in the gap sit at dash height"},
        {9300, 600, "Enter the door of YOUR element"},
    };
    for (const Text &t : signs) {
        GameObject &o = w.Create("sign");
        o.Add<Transform>(t.x, t.y, 0.0f, 0.0f);
        o.Add<Sign>(t.text);
    }
}
}
