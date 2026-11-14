#pragma once

#include "engine/EventManager.hpp"
#include "engine/Snapshot.hpp"
#include "engine/Timeline.hpp"
#include "engine/World.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
// Milestone 4, Part 1.3: a replay built on the event system
class ReplayRecorder {
public:
    struct Frame {
        int64_t time;
        std::string snapshot;
    };
    struct Tape {
        std::vector<Frame> frames;
        std::vector<Event> events;
        int64_t start = 0, end = 0;
    };

    explicit ReplayRecorder(EventManager &manager, double keepSeconds = 0.0);
    ~ReplayRecorder();
    ReplayRecorder(const ReplayRecorder &) = delete;
    ReplayRecorder &operator=(const ReplayRecorder &) = delete;

    // The game's thread, with the world locked, once a frame while recording
    void Capture(const World &world);

    bool Recording() const { return recording; }
    const Tape &Data() const { return data; }
    double Seconds() const;  // length of what is held

private:
    void Trim(int64_t now);

    EventManager &manager;
    double keepSeconds;
    bool recording = false;
    Tape data;
    SnapshotEncoder encoder{SnapshotOptions(Encoding::Binary)};
    uint32_t tick = 0;
    EventManager::HandlerId handler = 0;
};

class ReplayPlayer {
public:
    // `clock` is the game's Timeline: playback time is anchored to it
    ReplayPlayer(EventManager &manager, const ReplayRecorder &recorder, Timeline &clock);
    ~ReplayPlayer();
    ReplayPlayer(const ReplayPlayer &) = delete;
    ReplayPlayer &operator=(const ReplayPlayer &) = delete;

    // The game's thread, every frame
    void Update();

    bool Playing() const { return playing; }
    bool Paused() const { return timeline && timeline->IsPaused(); }
    double Speed() const { return timeline ? timeline->GetScale() : 1.0; }
    // 0..1 through what is being played
    double Progress() const;
    // How far into the recording playback is, in seconds
    double Position() const;

    // What playback shows; valid while Playing()
    World &PlaybackWorld() { return *world; }
    // The recorded events, raised again as playback reaches them
    EventManager &Events() { return replayEvents; }

private:
    void Start(const Event &play);
    void Finish(bool stopped);

    EventManager &manager;
    const ReplayRecorder &recorder;
    Timeline &clock;

    bool playing = false;
    ReplayRecorder::Tape copy;  // frozen at replay_play, so recording can go on
    std::unique_ptr<Timeline> timeline;
    std::unique_ptr<World> world;
    std::unique_ptr<SnapshotDecoder> decoder;
    int64_t from = 0, to = 0;  // the part being played, in recording time
    size_t nextFrame = 0, nextEvent = 0;
    EventManager replayEvents;
    std::vector<EventManager::HandlerId> handlers;
};
}  // namespace engine
