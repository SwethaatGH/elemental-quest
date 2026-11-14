#pragma once

namespace engine {
enum class ScaleMode {
    // Constant, pixel-based: one world unit is always one screen pixel
    Pixel,
    // Percentage-based: the design resolution is mapped onto the window
    Proportional
};

// Task 6: pixel or proportional world-to-screen scaling
class Scaling {
public:
    static void Toggle();
    static ScaleMode GetMode();
    static const char *GetModeName();

    // Multiplier to apply to an entity's position and size given the current window dimensions
    static float GetFactor(int windowWidth, int windowHeight, int baseWidth, int baseHeight);

private:
    static ScaleMode mode;
};
}  // namespace engine
