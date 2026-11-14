#pragma once

#include <SDL3/SDL.h>

#include <vector>

namespace engine {
// Task 4: keyboard state with just-pressed edge detection
class Input {
public:
    // Refreshes the cached keyboard state
    static void Update();

    // True on every frame the key is held down
    static bool IsKeyPressed(SDL_Scancode scancode);

    // True only on the frame the key goes down
    static bool IsKeyJustPressed(SDL_Scancode scancode);

private:
    // Snapshots, not SDL's live pointer
    static std::vector<bool> current;
    static std::vector<bool> previous;
    static int numKeys;
};
}  // namespace engine
