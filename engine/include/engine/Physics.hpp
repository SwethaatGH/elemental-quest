#pragma once

#include "engine/Entity.hpp"

namespace engine {
// Generic physics system
class Physics {
public:
    static void SetGravity(float gravityPixelsPerSecondSquared);
    static float GetGravity();

    // Integrates gravity into velocity
    static void Update(Entity &entity, float deltaSeconds);

private:
    static float gravity;
};
}  // namespace engine
