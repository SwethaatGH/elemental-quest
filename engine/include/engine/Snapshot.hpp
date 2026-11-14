#pragma once

#include "engine/Serialize.hpp"
#include "engine/Systems.hpp"
#include "engine/World.hpp"

#include <climits>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {
// Milestone 3, Parts 1B and 1C: the networked scene's messages
struct SnapshotOptions {
    Encoding encoding = Encoding::Text;
    bool delta = false;
    int keyframeEvery = 60;
    std::vector<std::string> components;

    SnapshotOptions() = default;
    SnapshotOptions(Encoding e, bool deltaStream = false, int keyframe = 60, std::vector<std::string> only = {})
        : encoding(e), delta(deltaStream), keyframeEvery(keyframe), components(std::move(only)) {}
};

struct SnapshotStats {
    size_t objects = 0;  // objects written
    size_t removed = 0;  // removals written
    size_t bytes = 0;
    bool keyframe = false;
};

class SnapshotEncoder {
public:
    explicit SnapshotEncoder(SnapshotOptions options = {});

    // Every object with a NetIdentity that `include` accepts (all, if empty)
    std::string Encode(const World &world, uint32_t tick, const ObjectFilter &include = {});

    // The next Encode is a keyframe - after a new client joins, say
    void ForceKeyframe() { forceKeyframe = true; }

    const SnapshotStats &LastStats() const { return stats; }
    const SnapshotOptions &Options() const { return options; }

private:
    const ComponentInfo *Travels(ComponentKey key) const;
    void ResolveWhitelist();

    SnapshotOptions options;
    std::vector<const ComponentInfo *> allowed;  // the whitelist, once resolved
    bool resolved = false;
    std::unordered_map<ObjectId, std::string> lastSent;  // delta: each object's components as last sent
    int sinceKeyframe = 0;
    bool forceKeyframe = true;
    SnapshotStats stats;
    std::unique_ptr<Writer> body, objects, scratch;
    std::vector<std::pair<const ComponentInfo *, const Component *>> travelling;  // reused per object
};

// True if `message` is a keyframe
bool IsKeyframe(const std::string &message);

constexpr int ANY_OWNER = INT_MIN;

struct ApplyOptions {
    // Who sent it: -1 the server, otherwise a client or peer id
    int source = -1;
    // Objects this endpoint owns: incoming copies are read and dropped
    int skipOwner = ANY_OWNER;
    // Accept only objects with this owner
    int onlyOwner = ANY_OWNER;
    // Create objects this World does not have
    bool createMissing = true;
};

struct ApplyResult {
    bool ok = false;  // parsed completely
    bool skipped = false;  // a delta that arrived before any keyframe from its source
    bool keyframe = false;
    uint32_t tick = 0;
    int64_t sentMicros = 0;
    size_t created = 0, updated = 0, removed = 0;
    std::vector<ObjectId> createdIds;
};

class SnapshotDecoder {
public:
    SnapshotDecoder();

    // Applies one snapshot to `world`
    ApplyResult Apply(World &world, const std::string &message, const ApplyOptions &options = {});

    // Forget everything heard from this source - it disconnected
    void ForgetSource(int source);

private:
    std::map<int, std::vector<ObjectId>> known;  // per source, sorted: the objects it has told us about
    std::set<int> keyed;  // sources we have had a keyframe from
};
}  // namespace engine
