#include "draw.hpp"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <tuple>
#include <vector>

using namespace engine;

namespace ed {
namespace {
std::string AssetDir() {
    const char *base = SDL_GetBasePath();
    return std::string(base ? base : "") + "assets";
}

SDL_FRect ToScreen(const View &v, float x, float y, float w, float h) {
    return {(x - v.x) * v.scale, (y - v.y) * v.scale, w * v.scale, h * v.scale};
}

void Fill(SDL_Renderer *r, const View &v, float x, float y, float w, float h, SDL_Color c) {
    const SDL_FRect dst = ToScreen(v, x, y, w, h);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &dst);
}

void Outline(SDL_Renderer *r, const View &v, float x, float y, float w, float h, SDL_Color c) {
    const SDL_FRect dst = ToScreen(v, x, y, w, h);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderRect(r, &dst);
}

bool OnScreen(const View &v, const Transform &t) {
    return t.Right() >= v.x && t.Left() <= v.x + v.w && t.Bottom() >= v.y && t.Top() <= v.y + v.h;
}

void DrawSurface(SDL_Renderer *r, const View &v, const Transform &t, Style style, Uint8 alpha) {
    const float x = t.position.x, y = t.position.y, w = t.size.x, h = t.size.y;
    auto c = [alpha](Uint8 R, Uint8 G, Uint8 B, Uint8 A = 255) {
        return SDL_Color{R, G, B, static_cast<Uint8>(A * alpha / 255)};
    };
    switch (style) {
    case Style::Ground:
        Fill(r, v, x, y, w, h, c(88, 60, 38));
        for (float sx = x + 18; sx < x + w - 8; sx += 46) Fill(r, v, sx, y + 30 + std::fmod(sx, 37.0f), 8, 6, c(66, 44, 28));
        Fill(r, v, x, y, w, std::min(10.0f, h), c(96, 130, 50));
        Fill(r, v, x, y + 10, w, 3, c(70, 96, 38));
        break;
    case Style::Basalt:
        Fill(r, v, x, y, w, h, c(58, 38, 42));
        for (float sx = x + 24; sx < x + w - 4; sx += 52) Fill(r, v, sx, y + 6, 3, h - 6, c(240, 110, 40));
        Fill(r, v, x, y, w, 5, c(255, 140, 50));
        break;
    case Style::Ice:
        Fill(r, v, x, y, w, h, c(150, 215, 240, 225));
        for (float sx = x + 14; sx < x + w - 20; sx += 60) Fill(r, v, sx, y + h * 0.35f, 22, 3, c(245, 252, 255, 230));
        Fill(r, v, x, y, w, 4, c(235, 250, 255));
        Fill(r, v, x, y + h - 3, w, 3, c(90, 160, 205));
        break;
    case Style::Moss:
        Fill(r, v, x, y, w, h, c(110, 98, 80));
        Fill(r, v, x, y + h * 0.5f, w, 2, c(78, 68, 56));
        for (float sx = x + 40; sx < x + w; sx += 40) Fill(r, v, sx, y, 2, h, c(78, 68, 56));
        Fill(r, v, x, y, w, 7, c(90, 160, 60));
        break;
    case Style::Cloud:
        Fill(r, v, x, y + 6, w, h - 6, c(238, 243, 252, 230));
        for (float sx = x; sx < x + w - 10; sx += 34) Fill(r, v, sx + 4, y, 26, 10, c(250, 252, 255, 240));
        for (float sx = x + 6; sx < x + w - 10; sx += 24) Fill(r, v, sx, y + h - 3, 12, 3, c(170, 185, 210, 230));
        break;
    case Style::Timber:
        Fill(r, v, x, y, w, h, c(139, 94, 52));
        for (float sx = x + 30; sx < x + w; sx += 30) Fill(r, v, sx, y, 2, h, c(96, 62, 32));
        Fill(r, v, x + 14, y, 4, h, c(214, 186, 128));
        Fill(r, v, x + w - 18, y, 4, h, c(214, 186, 128));
        Fill(r, v, x, y, w, 5, c(186, 136, 82));
        break;
    case Style::Lift:
        Fill(r, v, x, y, w, h, c(194, 160, 104));
        for (float sx = x + 16; sx < x + w - 10; sx += 32) Fill(r, v, sx, y + 8, 12, h - 14, c(160, 126, 76));
        Fill(r, v, x + 12, y - 60, 3, 60, c(120, 92, 56, 200));
        Fill(r, v, x + w - 15, y - 60, 3, 60, c(120, 92, 56, 200));
        Fill(r, v, x, y, w, 4, c(230, 204, 146));
        break;
    case Style::Glider:
        Fill(r, v, x, y, w, h, c(168, 88, 56));
        for (float sx = x + 10; sx < x + w - 16; sx += 28)
            for (int k = 0; k < 4; ++k) Fill(r, v, sx + 3.0f * k, y + h - 6 - 5.0f * k, 4, 3, c(206, 132, 92));
        Fill(r, v, x, y, w, 4, c(110, 150, 62));
        break;
    case Style::Goo:
        Fill(r, v, x - 4, y + 4, w + 8, h - 4, c(18, 14, 22));
        Fill(r, v, x, y, w, h, c(26, 20, 32));
        Fill(r, v, x, y, w, 3, c(140, 230, 80));
        for (float sx = x + 10; sx < x + w - 8; sx += 22) Fill(r, v, sx, y + 7 + std::fmod(sx, 5.0f), 6, 6, c(110, 190, 70, 210));
        break;
    }
}
}

void Art::Load(SDL_Renderer *renderer) {
    sprites.SetRoot(AssetDir());
    for (const char *name : {"background.png", "background.jpg", "background.jpeg"}) {
        const std::string path = AssetDir() + "/" + name;
        if (!SDL_GetPathInfo(path.c_str(), nullptr)) continue;
        if ((background = IMG_LoadTexture(renderer, path.c_str()))) break;
    }
}

void Art::Destroy() {
    if (background) SDL_DestroyTexture(background);
    background = nullptr;
}

void RenderScene(const World &world, SDL_Renderer *r, const View &view, Art &art, const DrawRules &rules) {
    std::vector<std::tuple<int, ObjectId, const GameObject *>> visible;
    world.Each<Transform, Renderable>([&](const GameObject &o, const Transform &t, const Renderable &rd) {
        if (!rd.visible || !OnScreen(view, t) || o.Id() == rules.hidden) return;
        if (o.Id() == rules.me && rules.blinkMe) return;
        visible.emplace_back(rd.layer, o.Id(), &o);
    });
    std::sort(visible.begin(), visible.end(), [](const auto &a, const auto &b) {
        return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b) : std::get<1>(a) < std::get<1>(b);
    });

    for (const auto &item : visible) {
        const GameObject &o = *std::get<2>(item);
        const Transform &t = *o.Get<Transform>();
        const Renderable &rd = *o.Get<Renderable>();
        Uint8 alpha = rd.color.a;
        if (const Gem *gem = o.Get<Gem>(); gem && gem->element != rules.myElement) alpha = 90;
        if (const Player *p = o.Get<Player>(); p && p->dead) alpha = 70;  // between death and spawn: a ghost

        if (const Surface *s = o.Get<Surface>()) {
            DrawSurface(r, view, t, s->style, alpha);
            continue;
        }
        if (o.Has<Gate>()) {
            if (o.Has<Collider>()) {
                Fill(r, view, t.position.x, t.position.y, t.size.x, t.size.y, rd.color);
                for (float y = t.position.y + 20; y < t.Bottom(); y += 40)
                    Fill(r, view, t.position.x, y, t.size.x, 6, {90, 100, 120, 255});
            } else {
                Outline(r, view, t.position.x, t.position.y, t.size.x, t.size.y, {150, 160, 180, 255});
            }
            continue;
        }

        const SDL_FRect dst = ToScreen(view, t.position.x, t.position.y, t.size.x, t.size.y);
        if (SDL_Texture *tex = art.sprites.Get(r, rd.sprite)) {
            SDL_SetTextureColorMod(tex, 255, 255, 255);
            SDL_SetTextureAlphaMod(tex, alpha);
            SDL_RenderTextureRotated(r, tex, nullptr, &dst, 0.0, nullptr, rd.flipX ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
        } else {
            SDL_SetRenderDrawColor(r, rd.color.r, rd.color.g, rd.color.b, alpha);
            SDL_RenderFillRect(r, &dst);
            if (o.Has<Player>()) {
                const float ex = rd.flipX ? 8.0f : t.size.x - 16.0f;
                Fill(r, view, t.position.x + ex, t.position.y + 14, 8, 8, {255, 255, 255, 255});
            }
        }
    }
}

void DrawHiddenObjects(const World &world, SDL_Renderer *r, const View &view) {
    auto label = [&](const Transform &t, const std::string &text, SDL_Color c) {
        DrawWorldText(r, view, t.position.x + 2, std::max(t.position.y, view.y + 4) + 2, text, c, 1.25f);
    };
    world.Each<Transform, DeathZone>([&](const GameObject &, const Transform &t, const DeathZone &) {
        if (!OnScreen(view, t)) return;
        Fill(r, view, t.position.x, t.position.y, t.size.x, t.size.y, {255, 40, 40, 60});
        Outline(r, view, t.position.x, t.position.y, t.size.x, t.size.y, {255, 60, 60, 255});
        label(t, "DEATH ZONE", {255, 90, 90, 255});
    });
    world.Each<Transform, SpawnPoint>([&](const GameObject &, const Transform &t, const SpawnPoint &sp) {
        if (!OnScreen(view, t)) return;
        const SDL_Color c = ElementColor(sp.index % kMaxPlayers);
        Outline(r, view, t.position.x, t.position.y, t.size.x, t.size.y, c);
        DrawWorldText(r, view, t.position.x, t.position.y - 12, "S" + std::to_string(sp.index), c, 1.25f);
    });
    world.Each<Transform, Checkpoint>([&](const GameObject &, const Transform &t, const Checkpoint &cp) {
        if (!OnScreen(view, t)) return;
        Fill(r, view, t.position.x, t.position.y, t.size.x, t.size.y, {120, 230, 130, 40});
        Outline(r, view, t.position.x, t.position.y, t.size.x, t.size.y, {120, 230, 130, 255});
        DrawWorldText(r, view, t.position.x, view.y + 150, "CHECKPOINT " + std::to_string(cp.index), {120, 230, 130, 255}, 1.25f);
    });
    world.Each<Transform, ScrollBoundary>([&](const GameObject &o, const Transform &t, const ScrollBoundary &) {
        Fill(r, view, t.position.x, t.position.y, t.size.x, t.size.y, {255, 215, 90, 50});
        Outline(r, view, t.position.x, t.position.y, t.size.x, t.size.y, {255, 215, 90, 255});
        DrawWorldText(r, view, t.position.x, view.y + 180, o.Name(), {255, 215, 90, 255}, 1.25f);
    });
}

void DrawBackground(SDL_Renderer *r, const View &view, const Art &art, int windowW, int windowH) {
    if (SDL_Texture *tex = art.background) {
        float tw = 0, th = 0;
        SDL_GetTextureSize(tex, &tw, &th);
        if (tw > 0 && th > 0) {
            const float h = static_cast<float>(windowH), w = tw * h / th;
            const float offset = std::fmod(view.x * 0.2f * view.scale, w);
            for (float x = -offset; x < windowW; x += w) {
                const SDL_FRect dst{x, 0, w, h};
                SDL_RenderTexture(r, tex, nullptr, &dst);
            }
            return;
        }
    }
    SDL_SetRenderDrawColor(r, 25, 60, 130, 255);
    SDL_RenderClear(r);
    struct HillLayer {
        float factor;
        SDL_Color color;
        float spacing, baseHeight;
    };
    const HillLayer layers[] = {{0.15f, {32, 72, 145, 255}, 520, 260}, {0.35f, {40, 88, 160, 255}, 380, 170}};
    for (const HillLayer &layer : layers) {
        SDL_SetRenderDrawColor(r, layer.color.r, layer.color.g, layer.color.b, 255);
        const float offset = view.x * layer.factor;
        const int first = static_cast<int>(offset / layer.spacing) - 1;
        for (int k = first; k < first + 14; ++k) {
            const float h = layer.baseHeight + static_cast<float>((k * 73 % 5 + 5) % 5) * 45.0f;
            const float x = k * layer.spacing - offset;
            const SDL_FRect hill{x * view.scale, (kGroundTop - h - view.y) * view.scale, layer.spacing * 0.8f * view.scale,
                                 (h + 200) * view.scale};
            SDL_RenderFillRect(r, &hill);
        }
    }
}

void DrawText(SDL_Renderer *r, float x, float y, const std::string &text, SDL_Color color, float size) {
    float oldX = 1, oldY = 1;
    SDL_GetRenderScale(r, &oldX, &oldY);
    SDL_SetRenderScale(r, size, size);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 200);
    SDL_RenderDebugText(r, x / size + 1, y / size + 1, text.c_str());
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    SDL_RenderDebugText(r, x / size, y / size, text.c_str());
    SDL_SetRenderScale(r, oldX, oldY);
}

float TextWidth(const std::string &text, float size) {
    return static_cast<float>(text.size()) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * size;
}

void DrawPanel(SDL_Renderer *r, float x, float y, float w, float h, Uint8 alpha) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 5, 10, 25, alpha);
    const SDL_FRect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

void DrawWorldText(SDL_Renderer *r, const View &view, float x, float y, const std::string &text, SDL_Color color,
                   float size) {
    const float sx = (x - view.x) * view.scale;
    const float sy = (y - view.y) * view.scale;
    const float w = TextWidth(text, size);
    if (sx + w < 0 || sx > view.w * view.scale || sy < -20 || sy > view.h * view.scale) return;
    DrawText(r, sx, sy, text, color, size);
}

void DrawWorldCircle(SDL_Renderer *r, const View &view, Vec2 center, float radius, SDL_Color color, int thickness) {
    constexpr int kSegments = 64;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    for (int k = 0; k < thickness; ++k) {
        const float rad = (radius - k) * view.scale;
        SDL_FPoint points[kSegments + 1];
        for (int i = 0; i <= kSegments; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / kSegments;
            points[i] = {(center.x - view.x) * view.scale + rad * std::cos(a), (center.y - view.y) * view.scale + rad * std::sin(a)};
        }
        SDL_RenderLines(r, points, kSegments + 1);
    }
}

void DrawWorldBox(SDL_Renderer *r, const View &view, float x, float y, float w, float h, SDL_Color color, bool fill) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (fill) Fill(r, view, x, y, w, h, color);
    else {
        Outline(r, view, x, y, w, h, color);
        Outline(r, view, x + 1, y + 1, w - 2, h - 2, color);
    }
}
}
