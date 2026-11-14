#include "engine/Entity.hpp"

namespace engine {
SDL_FRect Entity::GetBounds() const {
    return SDL_FRect{position.x, position.y, size.x, size.y};
}

void Entity::SetTexture(SDL_Texture *tex, int frames, int fw, int fh, Uint32 delayMs) {
    texture = tex;
    frameCount = frames;
    frameWidth = fw;
    frameHeight = fh;
    frameDelayMs = delayMs;
    currentFrame = 0;
    animationStartSeconds = -1.0;  // unseeded: the next UpdateAnimation marks the zero
}

void Entity::UpdateAnimation(const Timeline &clock) {
    if (!texture || frameCount <= 1 || frameDelayMs == 0) {
        return;
    }
    const double now = clock.GetSeconds();

    // First use marks this animation's zero
    if (animationStartSeconds < 0.0) {
        animationStartSeconds = now;
        currentFrame = 0;
        return;
    }

    const double elapsed = now - animationStartSeconds;

    // The clock moved backwards - a re-originned shared clock, say
    if (elapsed < 0.0) {
        animationStartSeconds = now;
        currentFrame = 0;
        return;
    }

    // The frame is a pure function of elapsed time on this clock
    currentFrame = (int)(elapsed / ((double)frameDelayMs / 1000.0)) % frameCount;
}

void Entity::Draw(SDL_Renderer *renderer, float scale, Vec2 camera) const {
    if (!active) {
        return;
    }

    // The scale is applied to the position as well as the size, not just the size
    SDL_FRect dst = {(position.x - camera.x) * scale, (position.y - camera.y) * scale,
                     size.x * scale, size.y * scale};

    if (texture) {
        SDL_FRect src = {
            (float)(currentFrame * frameWidth),
            0.0f,
            (float)frameWidth,
            (float)frameHeight
        };
        // Set on every draw: entities sharing one texture each keep their own tint
        SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(texture, color.a);
        SDL_RenderTexture(renderer, texture, &src, &dst);
    } else {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(renderer, &dst);
    }
}
}  // namespace engine
