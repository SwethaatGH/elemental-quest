#include "engine/Engine.hpp"

#include "engine/Input.hpp"
#include "engine/Scaling.hpp"

#include <chrono>
#include <thread>

namespace engine {
namespace {
// Cap the frame delta so a stall cannot carry a fast object through a wall
constexpr float MAX_DT = 1.0f / 20.0f;

// The background thread's sleep between ticks
constexpr int WORKER_PERIOD_MICROSECONDS = 4000;

// Slide 20's "small window"
constexpr auto GOVERNOR_WINDOW = std::chrono::microseconds(1200);

// One frame's delta, read off a timeline and clamped
float FrameDelta(Timeline &timeline, double &lastSeconds) {
    const double now = timeline.GetSeconds();
    float dt = (float)(now - lastSeconds);
    lastSeconds = now;
    // A tic change can round the reading down by up to one tic
    if (dt < 0.0f) dt = 0.0f;
    return dt > MAX_DT ? MAX_DT : dt;
}

// Copied, not captured by reference: the thread outlives this call
std::thread SpawnWorker(std::atomic<bool> &running, Timeline &gameTime,
                        std::function<void(float)> backgroundUpdate) {
    if (!backgroundUpdate) {
        return std::thread();
    }
    return std::thread([&running, &gameTime, backgroundUpdate = std::move(backgroundUpdate)]() {
        double workerLastTime = gameTime.GetSeconds();
        while (running) {
            backgroundUpdate(FrameDelta(gameTime, workerLastTime));
            std::this_thread::sleep_for(std::chrono::microseconds(WORKER_PERIOD_MICROSECONDS));
        }
    });
}
}  // namespace

void Engine::SetTargetFrameRate(int hz) {
    targetFrameRate = hz > 0 ? hz : 0;
}

void Engine::SetDeltaSmoothing(int samples) {
    deltaSamples = samples > 1 ? samples : 1;
    deltaWindow.assign((size_t)deltaSamples, 0.0f);
    deltaNext = 0;
    deltaFilled = 0;
}

float Engine::SmoothDelta(float raw) {
    // Zero means the timeline is paused
    if (deltaSamples <= 1 || raw == 0.0f) {
        deltaFilled = 0;
        deltaNext = 0;
        return raw;
    }
    deltaWindow[deltaNext] = raw;
    deltaNext = (deltaNext + 1) % deltaWindow.size();
    if (deltaFilled < deltaWindow.size()) {
        ++deltaFilled;
    }
    float sum = 0.0f;
    for (size_t i = 0; i < deltaFilled; ++i) {
        sum += deltaWindow[i];
    }
    return sum / (float)deltaFilled;
}

bool Engine::Init(const char *title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return false;
    }

    if (!SDL_CreateWindowAndRenderer(title, width, height, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }

    // Deliberately no SDL_SetRenderLogicalPresentation
    baseWidth = DEFAULT_WINDOW_WIDTH;
    baseHeight = DEFAULT_WINDOW_HEIGHT;

    return true;
}

bool Engine::InitHeadless() {
    // SDL_Init(0) starts SDL's core without the video subsystem
    if (!SDL_Init(0)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return false;
    }

    // No window to measure
    baseWidth = DEFAULT_WINDOW_WIDTH;
    baseHeight = DEFAULT_WINDOW_HEIGHT;
    return true;
}

void Engine::Shutdown() {
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    SDL_Quit();
}

void Engine::GetWindowSize(int *width, int *height) const {
    SDL_GetWindowSize(window, width, height);
}

float Engine::GetScale() const {
    int windowWidth = baseWidth;
    int windowHeight = baseHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    return Scaling::GetFactor(windowWidth, windowHeight, baseWidth, baseHeight);
}

void Engine::Clear(SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
}

void Engine::Present() {
    SDL_RenderPresent(renderer);
}

void Engine::Run(const std::function<void(float)> &update,
                 const std::function<void()> &render,
                 const std::function<void(float)> &backgroundUpdate) {
    running = true;

    // Section 3: a second thread over a disjoint slice of the world
    std::thread worker = SpawnWorker(running, gameTime, backgroundUpdate);

    // Delta comes from the game timeline, not the wall clock
    double lastTime = gameTime.GetSeconds();

    SDL_Event event;
    while (running) {
        const auto frameStart = std::chrono::steady_clock::now();
        ++iterationCount;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        const float dt = SmoothDelta(FrameDelta(gameTime, lastTime));

        Input::Update();
        update(dt);

        Clear();
        render();
        Present();

        // Frame-rate governing
        if (targetFrameRate > 0) {
            const auto target = frameStart + std::chrono::microseconds(
                Timeline::MICROSECONDS_PER_SECOND / targetFrameRate);
            const auto now = std::chrono::steady_clock::now();
            if (now + GOVERNOR_WINDOW < target) {
                std::this_thread::sleep_for(target - now - GOVERNOR_WINDOW);
            }
        }
    }

    running = false;
    if (worker.joinable()) {
        worker.join();
    }
}

void Engine::RunHeadless(const std::function<void(float)> &update, int tickHz,
                         const std::function<void(float)> &backgroundUpdate) {
    running = true;
    std::thread worker = SpawnWorker(running, gameTime, backgroundUpdate);

    // tickHz <= 0: no sleep at all
    const bool uncapped = tickHz <= 0;
    const auto period = std::chrono::microseconds(uncapped ? 0 : Timeline::MICROSECONDS_PER_SECOND / tickHz);

    double lastTime = gameTime.GetSeconds();
    auto next = std::chrono::steady_clock::now();
    while (running) {
        ++iterationCount;
        update(SmoothDelta(FrameDelta(gameTime, lastTime)));
        if (uncapped) continue;
        // Sleep until the next tick's fixed time, not a whole period after this one
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (now - next > period) next = now;
        else std::this_thread::sleep_until(next);
    }

    running = false;
    if (worker.joinable()) {
        worker.join();
    }
}
}  // namespace engine
