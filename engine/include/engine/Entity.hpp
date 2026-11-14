#pragma once

#include <SDL3/SDL.h>

#include "engine/Timeline.hpp"

namespace engine {
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// A generic game object
class Entity {
public:
    Vec2 position;
    Vec2 velocity;
    Vec2 size = {32.0f, 32.0f};

    bool affectedByGravity = false;
    bool active = true;

    // The fill when no texture is set, and a tint multiplied over the sprite when one is
    SDL_Color color = {255, 255, 255, 255};

    // Optional sprite sheet animation
    SDL_Texture *texture = nullptr;
    int frameCount = 1;
    int currentFrame = 0;
    int frameWidth = 0;
    int frameHeight = 0;
    Uint32 frameDelayMs = 100;

    // The instant this animation started, in seconds on whatever timeline drives it
    double animationStartSeconds = -1.0;

    // Bounds in world space
    SDL_FRect GetBounds() const;

    void SetTexture(SDL_Texture *tex, int frameCount, int frameWidth, int frameHeight, Uint32 frameDelayMs = 100);
    // Advances the frame if `frameDelayMs` of the given timeline's time has passed
    void UpdateAnimation(const Timeline &clock);

    // `scale` comes from Engine::GetScale() / the Scaling system
    void Draw(SDL_Renderer *renderer, float scale = 1.0f, Vec2 camera = {}) const;
};
}  // namespace engine
