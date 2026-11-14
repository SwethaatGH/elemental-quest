#pragma once

#include "components.hpp"
#include "level.hpp"

#include "engine/Systems.hpp"
#include "engine/World.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace ed {
struct View {
    float x = 0, y = 0, w = 1920, h = 1080, scale = 1.0f;
    static View From(const engine::Camera &cam, float scale) {
        return {cam.position.x, cam.position.y, cam.view.x, cam.view.y, scale};
    }
};

struct Art {
    engine::TextureCache sprites;
    SDL_Texture *background = nullptr;
    void Load(SDL_Renderer *renderer);
    void Destroy();
};

struct DrawRules {
    int myElement = 0;
    engine::ObjectId me = engine::NO_OBJECT;
    bool blinkMe = false;
    engine::ObjectId hidden = engine::NO_OBJECT;
};

void RenderScene(const engine::World &world, SDL_Renderer *r, const View &view, Art &art, const DrawRules &rules);

void DrawHiddenObjects(const engine::World &world, SDL_Renderer *r, const View &view);

void DrawBackground(SDL_Renderer *r, const View &view, const Art &art, int windowW, int windowH);

void DrawText(SDL_Renderer *r, float x, float y, const std::string &text, SDL_Color color, float size = 2.0f);
float TextWidth(const std::string &text, float size = 2.0f);
void DrawPanel(SDL_Renderer *r, float x, float y, float w, float h, Uint8 alpha = 170);
// World-space effects: burst rings, frozen platforms, dash trails
void DrawWorldCircle(SDL_Renderer *r, const View &view, engine::Vec2 center, float radius, SDL_Color color, int thickness = 3);
void DrawWorldBox(SDL_Renderer *r, const View &view, float x, float y, float w, float h, SDL_Color color, bool fill);

void DrawWorldText(SDL_Renderer *r, const View &view, float x, float y, const std::string &text, SDL_Color color,
                   float size = 2.0f);
}
