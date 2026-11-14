#pragma once

#include "engine/Entity.hpp"

namespace engine {
// Generic bounding-box collision between any two entities
class Collision {
public:
    static bool CheckCollision(const Entity &a, const Entity &b);

    // The smallest move that takes `mover` out of `solid`, or (0,0) if they do not overlap
    static Vec2 Resolve(const Entity &mover, const Entity &solid);
};
}  // namespace engine
