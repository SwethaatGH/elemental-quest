#pragma once

#include "engine/GameObject.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine {
// Owns every GameObject in a scene
class World {
public:
    explicit World(ObjectId firstId = 1);

    GameObject &Create(const std::string &kind = {}, const std::string &name = {});

    // Creates an object with an id chosen elsewhere - the id the authority gave it
    GameObject &CreateWithId(ObjectId id, const std::string &kind = {});

    // Nullptr if there is no such object or it has been destroyed
    GameObject *Find(ObjectId id);
    const GameObject *Find(ObjectId id) const;
    GameObject *FindByName(const std::string &name);

    void Destroy(ObjectId id);
    // Destroys every object the predicate accepts
    size_t DestroyIf(const std::function<bool(const GameObject &)> &predicate);

    // Deletes everything destroyed since the last Flush
    size_t Flush();

    // Live objects only
    size_t Count() const;
    std::vector<ObjectId> Ids() const;

    // Calls fn(object, components...) for every live object that has all of Ts, in id order
    template <class... Ts, class F>
    void Each(F &&fn) {
        for (auto &entry : objects) {
            GameObject &object = *entry.second;
            if (object.destroyed) continue;
            if ((object.template Has<Ts>() && ...)) fn(object, *object.template Get<Ts>()...);
        }
    }
    template <class... Ts, class F>
    void Each(F &&fn) const {
        for (const auto &entry : objects) {
            const GameObject &object = *entry.second;
            if (object.destroyed) continue;
            if ((object.template Has<Ts>() && ...)) fn(object, *object.template Get<Ts>()...);
        }
    }

    // Removes everything at once, without waiting for a Flush
    void Clear();

    ObjectId NextId() const { return nextId; }
    // Moves where Create() counts from
    void SetNextId(ObjectId id) { nextId = id == NO_OBJECT ? 1 : id; }

private:
    std::map<ObjectId, std::unique_ptr<GameObject>> objects;  // ordered: queries and snapshots are in id order
    std::vector<ObjectId> pending;
    ObjectId nextId;
};
}  // namespace engine
