#pragma once

#include "engine/Components.hpp"
#include "engine/Serialize.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace ed {
struct Player : engine::Component {
    ENGINE_COMPONENT(Player)
    int element = 0;
    int checkpoint = 0;
    bool inDoor = false;
    bool paused = false;
    float scale = 1.0f;
    int sendsPerSecond = 0;
    bool dead = false;  // between its death and spawn events

    void Write(engine::Writer &w) const override {
        w.I32(element); w.I32(checkpoint); w.Bool(inDoor); w.Bool(paused); w.F32(scale);
        w.I32(sendsPerSecond); w.Bool(dead);
    }
    void Read(engine::Reader &r) override {
        element = r.I32(); checkpoint = r.I32(); inDoor = r.Bool(); paused = r.Bool(); scale = r.F32();
        sendsPerSecond = r.I32(); dead = r.Bool();
    }
};

enum class Style : uint8_t {
    Ground,
    Basalt,
    Ice,
    Moss,
    Cloud,
    Timber,
    Lift,
    Glider,
    Goo,
};
const char *StyleName(Style s);

struct Surface : engine::Component {
    ENGINE_COMPONENT(Surface)
    Style style = Style::Ground;

    Surface() = default;
    explicit Surface(Style s) : style(s) {}

    void Write(engine::Writer &w) const override { w.U8(static_cast<uint8_t>(style)); }
    void Read(engine::Reader &r) override { style = static_cast<Style>(r.U8()); }
};

struct Enemy : engine::Component {
    ENGINE_COMPONENT(Enemy)
    int index = 0;
    void Write(engine::Writer &w) const override { w.I32(index); }
    void Read(engine::Reader &r) override { index = r.I32(); }
};

struct Gem : engine::Component {
    ENGINE_COMPONENT(Gem)
    int element = 0;
    void Write(engine::Writer &w) const override { w.I32(element); }
    void Read(engine::Reader &r) override { element = r.I32(); }
};

struct Plate : engine::Component {
    ENGINE_COMPONENT(Plate)
    bool pressed = false;
    void Write(engine::Writer &w) const override { w.Bool(pressed); }
    void Read(engine::Reader &r) override { pressed = r.Bool(); }
};

struct Gate : engine::Component {
    ENGINE_COMPONENT(Gate)
    bool open = false;
    void Write(engine::Writer &w) const override { w.Bool(open); }
    void Read(engine::Reader &r) override { open = r.Bool(); }
};

struct Door : engine::Component {
    ENGINE_COMPONENT(Door)
    int element = 0;
    void Write(engine::Writer &w) const override { w.I32(element); }
    void Read(engine::Reader &r) override { element = r.I32(); }
};

struct Checkpoint : engine::Component {
    ENGINE_COMPONENT(Checkpoint)
    int index = 1;
    void Write(engine::Writer &w) const override { w.I32(index); }
    void Read(engine::Reader &r) override { index = r.I32(); }
};

struct Match : engine::Component {
    ENGINE_COMPONENT(Match)
    int round = 1;
    float serverTime = 0;
    bool complete = false;
    float completeAt = 0;
    bool gateOpen = false;
    uint32_t present = 0;
    uint32_t active = 0;
    std::array<int, 4> gems{};

    void Write(engine::Writer &w) const override {
        w.I32(round); w.F32(serverTime); w.Bool(complete); w.F32(completeAt); w.Bool(gateOpen);
        w.U32(present); w.U32(active);
        for (int g : gems) w.I32(g);
    }
    void Read(engine::Reader &r) override {
        round = r.I32(); serverTime = r.F32(); complete = r.Bool(); completeAt = r.F32();
        gateOpen = r.Bool(); present = r.U32(); active = r.U32();
        for (int &g : gems) g = r.I32();
    }
};

struct Sign : engine::Component {
    std::string text;
    explicit Sign(std::string t = {}) : text(std::move(t)) {}
};

void RegisterGameComponents();
}
