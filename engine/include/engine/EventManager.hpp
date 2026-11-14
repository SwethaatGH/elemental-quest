#pragma once

#include "engine/Event.hpp"
#include "engine/Timeline.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <exception>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace engine {
// Part 1.1: registration, raising (queued, timed) and handling (by priority)
class EventHandler {
public:
    virtual ~EventHandler() = default;
    virtual void OnEvent(const Event &event) = 0;
};

class EventManager {
public:
    using Handler = std::function<void(const Event &)>;
    using HandlerId = uint64_t;
    // Sees every raised event after it is stamped and before it is queued
    using Router = std::function<bool(Event &)>;

    // `clock` stamps events; it must outlive the manager
    explicit EventManager(const Timeline *clock = nullptr) : clock(clock) {}

    void SetClock(const Timeline *timeline);
    void SetTime(int64_t tics);  // only used when there is no clock
    // Seconds per tic when the clock has none (no clock, or a counter), so RaiseAfter still works
    void SetSecondsPerTic(double seconds);
    int64_t Now() const;  // tics on the clock
    double SecondsPerTic() const;  // the clock's, or SetSecondsPerTic's

    // --- registration ---
    HandlerId Register(const EventType &type, Handler handler);
    HandlerId Register(const std::vector<EventType> &types, Handler handler);
    HandlerId Register(const EventType &type, EventHandler *handler);  // calls handler->OnEvent
    HandlerId RegisterAll(Handler handler);  // every type, after its own handlers
    bool Unregister(HandlerId id);
    size_t HandlerCount(const EventType &type) const;

    // --- raising ---
    // Queues the event, due delayTics from now; returns its sequence (also when routed), 0 if rejected
    uint64_t Raise(Event event, int64_t delayTics = 0);
    // The same, with the delay in seconds of the clock's time
    uint64_t RaiseAfter(Event event, double delaySeconds);

    // --- handling ---
    // Handles due events raised before this call, most urgent first; one thread at a time
    size_t Handle(int lowestPriority = PRIORITY_LOW);

    // Cancel a queued event
    bool Cancel(uint64_t sequence);
    // Reject new arrivals when full; never evict existing events
    void SetLimits(size_t maxQueued, size_t maxPerHandle);
    // Contains throwing handlers: counts them and calls this optional reporter
    void SetErrorHandler(std::function<void(const Event &, std::exception_ptr)> handler);
    uint64_t RejectedCount() const { return rejected.load(); }
    uint64_t HandlerFailureCount() const { return handlerFailures.load(); }
    double ClockRate() const;
    size_t Pending() const;
    void Clear();
    void SetRouter(Router router);

    // Called with Event::Describe() for every raise and every handle
    void SetLog(std::function<void(const std::string &)> log);

    uint64_t RaisedCount() const { return raised.load(); }
    uint64_t HandledCount() const { return handled.load(); }

private:
    struct Entry {
        HandlerId id;
        Handler handler;
    };

    const Timeline *clock = nullptr;
    std::atomic<int64_t> manualTime{0};
    double manualSecondsPerTic = 0.0;

    mutable std::mutex mutex;
    std::map<std::pair<int64_t, uint64_t>, Event> queue;  // by (due time, sequence): due ones come first
    std::unordered_map<uint32_t, std::vector<Entry>> handlers;  // by EventType::Id()
    std::vector<Entry> everyType;
    HandlerId nextHandler = 1;
    uint64_t nextSequence = 1;
    Router router;
    std::function<void(const std::string &)> log;

    size_t maxQueued = 65536, maxPerHandle = 1024, reserved = 0;
    std::function<void(const Event &, std::exception_ptr)> errorHandler;
    std::atomic<uint64_t> raised{0}, handled{0}, rejected{0}, handlerFailures{0};
};
}  // namespace engine
