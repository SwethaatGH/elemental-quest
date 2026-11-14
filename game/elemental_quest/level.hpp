#pragma once

#include "components.hpp"

#include "engine/World.hpp"

#include <SDL3/SDL.h>

#include <vector>

namespace ed {
constexpr int kJoinPort = 7555;
constexpr int kWorldPort = 7556;
constexpr int kClientPortBase = 7600;

constexpr int kMaxPlayers = 4;

enum Element { Fire = 0, Water = 1, Earth = 2, Air = 3 };

inline const char *ElementName(int e) {
    static const char *names[] = {"FIRE", "WATER", "EARTH", "AIR"};
    return (e >= 0 && e < kMaxPlayers) ? names[e] : "?";
}
inline const char *ElementFile(int e) {
    static const char *names[] = {"fire", "water", "earth", "air"};
    return (e >= 0 && e < kMaxPlayers) ? names[e] : "fire";
}
inline SDL_Color ElementColor(int e) {
    static const SDL_Color colors[] = {
        {235, 90, 45, 255}, {60, 150, 245, 255}, {150, 110, 60, 255}, {225, 235, 245, 255}};
    return (e >= 0 && e < kMaxPlayers) ? colors[e] : SDL_Color{255, 0, 255, 255};
}
inline SDL_Color GemColor(int e) {
    static const SDL_Color colors[] = {
        {255, 70, 40, 255}, {40, 200, 255, 255}, {120, 220, 70, 255}, {240, 240, 255, 255}};
    return (e >= 0 && e < kMaxPlayers) ? colors[e] : SDL_Color{255, 0, 255, 255};
}

constexpr engine::ObjectId kPlayerIdBase = 1u << 29;
inline engine::ObjectId PlayerObjectId(int client) { return kPlayerIdBase + static_cast<engine::ObjectId>(client); }

inline int SpawnIndex(int checkpoint, int element) { return checkpoint * kMaxPlayers + element; }

constexpr float kGravity = 2200.0f;
constexpr float kRunSpeed = 380.0f;
constexpr float kJumpSpeed = 950.0f;
constexpr float kStompBounce = 650.0f;
constexpr float kMaxFallSpeed = 1400.0f;
constexpr float kPlayerW = 44.0f, kPlayerH = 60.0f;
constexpr float kEnemyW = 52.0f, kEnemyH = 44.0f;
constexpr float kGemSize = 28.0f;
constexpr float kGroundTop = 900.0f;
constexpr float kWorldWidth = 9900.0f;
constexpr float kWorldHeight = 1080.0f;
constexpr int kCheckpoints = 4;
constexpr int kGemsPerElement = 8;
constexpr double kGateHoldSeconds = 2.5;
constexpr double kNextRoundSeconds = 10.0;
constexpr double kActiveTimeoutSeconds = 3.0;

enum Layer { kLayerPlatform = 1, kLayerGoo = 2, kLayerProps = 3, kLayerGem = 4, kLayerEnemy = 5, kLayerPlayer = 6 };

void BuildLevel(engine::World &world);

void BuildRoundObjects(engine::World &world);

// Round objects, also used by the server to restore what a death undoes
constexpr int kEnemies = 3;
engine::GameObject &BuildEnemy(engine::World &world, int index);
engine::GameObject &BuildGem(engine::World &world, float x, float y, int element);
// EARTH's burst: temporary one-way stone bridge over goo
engine::GameObject &BuildBridge(engine::World &world, const engine::Transform &goo);

// Temple gap: a running jump or a standing dash clears it
constexpr float kTempleGapLeft = 8770.0f, kTempleGapRight = 9070.0f;

void BuildBenchLevel(engine::World &world, int statics, int moving);

engine::GameObject &MakePlayer(engine::World &world, int id);

void BuildSigns(engine::World &world);

float SurfaceRunScale(Style style);

const Surface *ApplySurfaceRunSpeed(engine::World &world, engine::GameObject &player);
}
