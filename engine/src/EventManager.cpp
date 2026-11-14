#include "engine/EventManager.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <limits>

namespace engine {
namespace {
// The event this thread is handling
struct Handling {
    const EventManager *manager = nullptr;
    int age = 0;
};
thread_local Handling current;
}

void EventManager::SetClock(const Timeline *timeline) {
    std::lock_guard<std::mutex> lock(mutex);
    clock = timeline;
}

void EventManager::SetTime(int64_t tics) { manualTime = tics; }

void EventManager::SetSecondsPerTic(double seconds) {
    std::lock_guard<std::mutex> lock(mutex);
    manualSecondsPerTic = seconds;
}

int64_t EventManager::Now() const {
    const Timeline *c;
    {
        std::lock_guard<std::mutex> lock(mutex);
        c = clock;
    }
    return c ? c->GetTime() : manualTime.load();
}

double EventManager::SecondsPerTic() const {
    std::lock_guard<std::mutex> lock(mutex);
    const double own = clock ? clock->SecondsPerTic() : 0.0;
    return own > 0.0 ? own : manualSecondsPerTic;
}

// --- registration ------------------------------------------------------------

EventManager::HandlerId EventManager::Register(const EventType &type, Handler handler) {
    std::lock_guard<std::mutex> lock(mutex);
    const HandlerId id = nextHandler++;
    handlers[type.Id()].push_back(Entry{id, std::move(handler)});
    return id;
}

EventManager::HandlerId EventManager::Register(const std::vector<EventType> &types, Handler handler) {
    std::lock_guard<std::mutex> lock(mutex);
    const HandlerId id = nextHandler++;
    for (const EventType &type : types) handlers[type.Id()].push_back(Entry{id, handler});
    return id;
}

EventManager::HandlerId EventManager::Register(const EventType &type, EventHandler *handler) {
    return Register(type, [handler](const Event &e) { handler->OnEvent(e); });
}

EventManager::HandlerId EventManager::RegisterAll(Handler handler) {
    std::lock_guard<std::mutex> lock(mutex);
    const HandlerId id = nextHandler++;
    everyType.push_back(Entry{id, std::move(handler)});
    return id;
}

bool EventManager::Unregister(HandlerId id) {
    std::lock_guard<std::mutex> lock(mutex);
    bool found = false;
    auto drop = [&](std::vector<Entry> &list) {
        const size_t before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [id](const Entry &e) { return e.id == id; }), list.end());
        found = found || list.size() != before;
    };
    for (auto &entry : handlers) drop(entry.second);
    drop(everyType);
    return found;
}

size_t EventManager::HandlerCount(const EventType &type) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = handlers.find(type.Id());
    return it == handlers.end() ? 0 : it->second.size();
}

// --- raising -----------------------------------------------------------------

uint64_t EventManager::Raise(Event event, int64_t delayTics) {
    delayTics = std::max<int64_t>(0, delayTics);
    const int64_t now = Now();
    if (now > std::numeric_limits<int64_t>::max() - delayTics) { ++rejected; return 0; }
    event.time = now + delayTics;
    if (current.manager == this) event.age = current.age + 1;
    else if (!event.remote) event.age = 0;
    if (!event.remote) {
        event.originTime = event.time;
        event.delay = static_cast<float>(static_cast<double>(delayTics) * SecondsPerTic());
    }

    Router route;
    std::function<void(const std::string &)> logger;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (maxQueued && queue.size() + reserved >= maxQueued) { ++rejected; return 0; }
        ++reserved;
        event.sequence = nextSequence++;
        route = router;
        logger = log;
    }
    struct Reservation {
        std::mutex &mutex;
        size_t &count;
        bool active = true;
        ~Reservation() { if (active) { std::lock_guard<std::mutex> lock(mutex); --count; } }
    } reservation{mutex, reserved};
    // Outside the lock: the router takes its own locks, and may log
    if (route && route(event)) {
        if (logger) logger("routed " + event.Describe());
        return event.sequence;  // sent on, not rejected: do not raise it again
    }
    if (logger) logger("raise  " + event.Describe());
    const uint64_t sequence = event.sequence;
    std::lock_guard<std::mutex> lock(mutex);
    --reserved;
    reservation.active = false;
    queue.emplace(std::make_pair(event.time, sequence), std::move(event));
    ++raised;
    return sequence;
}

uint64_t EventManager::RaiseAfter(Event event, double delaySeconds) {
    if (!std::isfinite(delaySeconds) || delaySeconds < 0 || delaySeconds > 86400.0) { ++rejected; return 0; }
    const double perTic = SecondsPerTic();  // the clock's, or SetSecondsPerTic's
    const int64_t tics = perTic > 0.0 ? static_cast<int64_t>(std::llround(delaySeconds / perTic)) : 0;
    return Raise(std::move(event), tics);
}

// --- handling ----------------------------------------------------------------

size_t EventManager::Handle(int lowestPriority) {
    const int64_t now = Now();

    // Keep selected events in the queue until dispatch
    std::vector<Event> batch;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = queue.begin(); it != queue.end() && it->first.first <= now; ++it)
            if (it->second.priority <= lowestPriority) batch.push_back(it->second);
        const auto earlier = [](const Event &a, const Event &b) {
            return std::tie(a.priority, a.age, a.time, a.sequence) < std::tie(b.priority, b.age, b.time, b.sequence);
        };
        const size_t count = maxPerHandle ? std::min(maxPerHandle, batch.size()) : batch.size();
        std::partial_sort(batch.begin(), batch.begin() + count, batch.end(), earlier);
        batch.resize(count);
    }
    size_t dispatched = 0;
    for (const Event &event : batch) {
        std::vector<Handler> targets;
        std::function<void(const std::string &)> logger;
        std::function<void(const Event &, std::exception_ptr)> report;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!queue.erase({event.time, event.sequence})) continue;  // cancelled
            auto it = handlers.find(event.Type().Id());
            if (it != handlers.end()) for (const Entry &e : it->second) targets.push_back(e.handler);
            for (const Entry &e : everyType) targets.push_back(e.handler);
            logger = log;
            report = errorHandler;
        }
        struct Restore {
            Handling saved;
            ~Restore() { current = saved; }
        } restore{current};
        current = Handling{this, event.age};
        auto failure = [&]() {
            ++handlerFailures;
            // A broken error reporter must not prevent other handlers running
            if (report) { try { report(event, std::current_exception()); } catch (...) {} }
        };
        if (logger) { try { logger("handle " + event.Describe()); } catch (...) { failure(); } }
        for (const Handler &handler : targets) {
            try { handler(event); } catch (...) { failure(); }
        }
        ++handled;
        ++dispatched;
    }
    return dispatched;
}

bool EventManager::Cancel(uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        if (it->second.sequence == sequence) { queue.erase(it); return true; }
    }
    return false;
}

void EventManager::SetLimits(size_t capacity, size_t budget) {
    std::lock_guard<std::mutex> lock(mutex);
    maxQueued = capacity;
    maxPerHandle = budget;
}

void EventManager::SetErrorHandler(std::function<void(const Event &, std::exception_ptr)> handler) {
    std::lock_guard<std::mutex> lock(mutex);
    errorHandler = std::move(handler);
}

double EventManager::ClockRate() const {
    std::lock_guard<std::mutex> lock(mutex);
    return clock ? clock->EffectiveScale() : 0.0;
}

size_t EventManager::Pending() const {
    std::lock_guard<std::mutex> lock(mutex);
    return queue.size();
}

void EventManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex);
    queue.clear();
}

void EventManager::SetRouter(Router r) {
    std::lock_guard<std::mutex> lock(mutex);
    router = std::move(r);
}

void EventManager::SetLog(std::function<void(const std::string &)> l) {
    std::lock_guard<std::mutex> lock(mutex);
    log = std::move(l);
}
}  // namespace engine
