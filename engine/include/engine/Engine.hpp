#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "engine/JobSystem.hpp"
#include "engine/Timeline.hpp"

namespace engine {
constexpr int DEFAULT_WINDOW_WIDTH = 1920;
constexpr int DEFAULT_WINDOW_HEIGHT = 1080;
constexpr SDL_Color DEFAULT_CLEAR_COLOR = {25, 60, 130, 255};

// Pool workers for the JobSystem, on top of the main thread and the background update thread
constexpr int DEFAULT_JOB_WORKERS = 2;

// Task 1: Core graphics setup
class Engine {
public:
    bool Init(const char *title, int width = DEFAULT_WINDOW_WIDTH, int height = DEFAULT_WINDOW_HEIGHT);

    // No window, no renderer, no video subsystem - just the timeline and the loop
    bool InitHeadless();

    void Shutdown();

    SDL_Window *GetWindow() const { return window; }
    SDL_Renderer *GetRenderer() const { return renderer; }

    // The design resolution: always 1920x1080 as specified in the rubric
    int GetBaseWidth() const { return baseWidth; }
    int GetBaseHeight() const { return baseHeight; }

    // Current window size, in the same units the renderer draws in
    void GetWindowSize(int *width, int *height) const;

    // World-to-screen scale for this frame, per the current ScaleMode
    float GetScale() const;

    // Game time (Section 1)
    Timeline &GetGameTime() { return gameTime; }
    const Timeline &GetGameTime() const { return gameTime; }

    // A second, unrelated clock (Section 1's "loop iterations" example)
    int64_t GetIterationCount() const { return iterationCount.load(); }

    // The iteration count as a Timeline
    Timeline &GetIterationTime() { return iterationTime; }
    const Timeline &GetIterationTime() const { return iterationTime; }

    void Clear(SDL_Color color = DEFAULT_CLEAR_COLOR);
    void Present();

    // Main loop: input, update(dt), render(); optional background thread on the same clock
    void Run(const std::function<void(float deltaSeconds)> &update,
             const std::function<void()> &render,
             const std::function<void(float deltaSeconds)> &backgroundUpdate = nullptr);

    // The headless counterpart of Run()
    void RunHeadless(const std::function<void(float deltaSeconds)> &update,
                     int tickHz = 60,
                     const std::function<void(float deltaSeconds)> &backgroundUpdate = nullptr);

    // Guards world state shared between the main loop and the background update thread
    std::mutex &GetWorldMutex() { return worldMutex; }

    // The job pool (Section 3)
    JobSystem &GetJobs() { return jobs; }

    // --- frame-time quality ---

    // Frame-rate governing: a "do not exceed" rate
    void SetTargetFrameRate(int hz);
    int GetTargetFrameRate() const { return targetFrameRate; }

    // Moving average: report the delta as the mean of the last `samples` frames
    void SetDeltaSmoothing(int samples);
    int GetDeltaSmoothing() const { return deltaSamples; }

    // Ends the loop from either thread
    void Stop() { running = false; }

private:
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;

    // Read by the background thread to know when to stop
    std::atomic<bool> running{false};
    std::mutex worldMutex;
    int baseWidth = DEFAULT_WINDOW_WIDTH;
    int baseHeight = DEFAULT_WINDOW_HEIGHT;

    // Smooths `raw` per SetDeltaSmoothing
    float SmoothDelta(float raw);

    int targetFrameRate = 0;
    int deltaSamples = 1;
    std::vector<float> deltaWindow;
    size_t deltaNext = 0;
    size_t deltaFilled = 0;

    // Anchored to real time, one tic per microsecond
    Timeline gameTime{nullptr, 1};

    // Section 1: counts loop passes, not time
    std::atomic<int64_t> iterationCount{0};
    Timeline iterationTime{iterationCount, 1};  // declared after the counter it reads

    JobSystem jobs{DEFAULT_JOB_WORKERS};
};
}  // namespace engine
