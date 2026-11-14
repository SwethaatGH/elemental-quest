#include "engine/Replay.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace engine {
namespace {
// The replay's own controls are not part of what it records
bool IsReplayControl(const Event &e) {
    return e.Is(events::REPLAY_RECORD_START) || e.Is(events::REPLAY_RECORD_STOP) || e.Is(events::REPLAY_PLAY) ||
           e.Is(events::REPLAY_PAUSE) || e.Is(events::REPLAY_RESUME) || e.Is(events::REPLAY_SPEED) ||
           e.Is(events::REPLAY_STOP) || e.Is(events::REPLAY_FINISHED);
}

int64_t SecondsToTics(double seconds, double secondsPerTic) {
    return secondsPerTic > 0.0 ? static_cast<int64_t>(std::llround(seconds / secondsPerTic)) : 0;
}
}  // namespace

// --- recording ---------------------------------------------------------------

ReplayRecorder::ReplayRecorder(EventManager &manager, double keepSeconds) : manager(manager), keepSeconds(keepSeconds) {
    handler = manager.RegisterAll([this](const Event &e) {
        if (e.Is(events::REPLAY_RECORD_START)) {
            data = Tape{};
            data.start = data.end = e.time;
            recording = true;
            return;
        }
        if (e.Is(events::REPLAY_RECORD_STOP)) {
            recording = false;
            return;
        }
        if (!recording || IsReplayControl(e)) return;
        data.events.push_back(e);
        data.end = std::max(data.end, e.time);
        Trim(e.time);
    });
}

ReplayRecorder::~ReplayRecorder() { manager.Unregister(handler); }

void ReplayRecorder::Capture(const World &world) {
    if (!recording) return;
    const int64_t now = manager.Now();
    data.frames.push_back(Frame{now, encoder.Encode(world, ++tick)});
    data.end = std::max(data.end, now);
    Trim(now);
}

void ReplayRecorder::Trim(int64_t now) {
    if (keepSeconds <= 0.0) return;
    const int64_t keep = SecondsToTics(keepSeconds, manager.SecondsPerTic());
    if (keep <= 0) return;
    const int64_t cutoff = now - keep;
    if (data.start >= cutoff) return;
    data.start = cutoff;
    auto frameEnd = std::find_if(data.frames.begin(), data.frames.end(), [cutoff](const Frame &f) { return f.time >= cutoff; });
    data.frames.erase(data.frames.begin(), frameEnd);
    data.events.erase(std::remove_if(data.events.begin(), data.events.end(), [cutoff](const Event &e) { return e.time < cutoff; }),
                      data.events.end());
}

double ReplayRecorder::Seconds() const {
    return static_cast<double>(data.end - data.start) * manager.SecondsPerTic();
}

// --- playback ----------------------------------------------------------------

ReplayPlayer::ReplayPlayer(EventManager &manager, const ReplayRecorder &recorder, Timeline &clock)
    : manager(manager), recorder(recorder), clock(clock) {
    handlers.push_back(manager.Register(events::REPLAY_PLAY, [this](const Event &e) { Start(e); }));
    handlers.push_back(manager.Register(events::REPLAY_PAUSE, [this](const Event &) {
        if (playing) timeline->Pause();
    }));
    handlers.push_back(manager.Register(events::REPLAY_RESUME, [this](const Event &) {
        if (playing) timeline->Unpause();
    }));
    handlers.push_back(manager.Register(events::REPLAY_SPEED, [this](const Event &e) {
        if (playing) timeline->SetScale(e.GetFloat("speed", 1.0f));
    }));
    handlers.push_back(manager.Register(events::REPLAY_STOP, [this](const Event &) {
        if (playing) Finish(true);
    }));
}

ReplayPlayer::~ReplayPlayer() {
    for (EventManager::HandlerId id : handlers) manager.Unregister(id);
}

void ReplayPlayer::Start(const Event &play) {
    copy = recorder.Data();
    if (copy.frames.empty()) {
        manager.Raise(Event(events::REPLAY_FINISHED).Bool("empty", true));
        return;
    }
    // Recorded in handling order; play back in time order
    std::stable_sort(copy.events.begin(), copy.events.end(), [](const Event &a, const Event &b) { return a.time < b.time; });

    to = copy.frames.back().time;
    from = copy.frames.front().time;
    const double seconds = play.GetFloat("seconds", 0.0f);
    if (seconds > 0.0) from = std::max(from, to - SecondsToTics(seconds, manager.SecondsPerTic()));

    timeline = std::make_unique<Timeline>(&clock);  // counts from now, on the game's clock
    timeline->SetScale(play.GetFloat("speed", 1.0f));
    world = std::make_unique<World>();
    decoder = std::make_unique<SnapshotDecoder>();
    replayEvents.Clear();

    // Start on the newest frame at or before `from`, so there is a world to show at once
    nextFrame = 0;
    while (nextFrame + 1 < copy.frames.size() && copy.frames[nextFrame + 1].time <= from) ++nextFrame;
    nextEvent = 0;
    while (nextEvent < copy.events.size() && copy.events[nextEvent].time < from) ++nextEvent;

    playing = true;
    Update();
}

void ReplayPlayer::Update() {
    if (!playing) return;
    const int64_t cursor = from + timeline->GetTime();

    // Only the newest frame that is due needs decoding
    size_t due = SIZE_MAX;
    while (nextFrame < copy.frames.size() && copy.frames[nextFrame].time <= cursor) due = nextFrame++;
    if (due != SIZE_MAX) {
        decoder->Apply(*world, copy.frames[due].snapshot);
        world->Flush();
    }
    while (nextEvent < copy.events.size() && copy.events[nextEvent].time <= cursor) {
        replayEvents.Raise(copy.events[nextEvent++]);
    }
    replayEvents.Handle();

    if (cursor >= to) Finish(false);
}

void ReplayPlayer::Finish(bool stopped) {
    playing = false;
    manager.Raise(Event(events::REPLAY_FINISHED).Bool("stopped", stopped));
}

double ReplayPlayer::Progress() const {
    if (!timeline || to <= from) return 0.0;
    return std::clamp(static_cast<double>(timeline->GetTime()) / static_cast<double>(to - from), 0.0, 1.0);
}

double ReplayPlayer::Position() const {
    return timeline ? static_cast<double>(timeline->GetTime()) * manager.SecondsPerTic() : 0.0;
}
}  // namespace engine
