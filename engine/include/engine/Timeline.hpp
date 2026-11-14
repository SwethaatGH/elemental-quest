#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace engine {
// Section 1: an explicit representation of time
class Timeline {
public:
    static constexpr double MIN_SCALE = 0.5;
    static constexpr double MAX_SCALE = 2.0;

    // Real time is measured in microseconds
    static constexpr int64_t MICROSECONDS_PER_SECOND = 1000000;

    // `anchor` == nullptr anchors this timeline to real time
    explicit Timeline(Timeline *anchor = nullptr, int64_t tic = 1);

    // Anchored to a counter instead of a clock: one anchor tic per increment
    explicit Timeline(const std::atomic<int64_t> &counter, int64_t tic = 1);

    // Elapsed tics, excluding time spent paused
    int64_t GetTime() const;

    // The same elapsed time in seconds
    double GetSeconds() const;

    // How many seconds one tic of this timeline represents
    double SecondsPerTic() const;

    void Pause();
    void Unpause();
    bool IsPaused() const;

    // 1.0 runs at the anchor's rate
    void SetScale(double newScale);
    double GetScale() const;
    // Wall-time rate, including every ancestor; zero when paused or counter-based
    double EffectiveScale() const;

    void SetTic(int64_t newTic);
    int64_t GetTic() const;

    // The clock a null-anchored timeline reads, in microseconds
    static int64_t RealTimeNow();

    // Re-zeroes this timeline so GetTime() measures from `anchorTime`
    void SetOrigin(int64_t anchorTime);

private:
    // Where the anchor is now, in root units
    int64_t ReadAnchor() const;

    // Settles and returns `accumulated`
    int64_t Accumulated() const;

    // How many root units one tic of this timeline is
    int64_t RootUnitsPerTic() const;

    // Advances `accumulated` to the present
    void Accumulate() const;

    Timeline *anchor = nullptr;
    const std::atomic<int64_t> *counter = nullptr;
    int64_t tic;

    // Both in root units
    mutable int64_t accumulated;
    mutable int64_t lastSample;

    // The part of a root unit the last scaled step earned but could not credit
    mutable double carry = 0.0;

    bool paused;
    double scale;

    mutable std::mutex mutex;
};
}  // namespace engine
