#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine {
// Milestone 3, Part 1A: a game object is an id, a kind and components

class Writer;
class Reader;
class GameObject;

using ObjectId = uint32_t;
constexpr ObjectId NO_OBJECT = 0;

// A component type's identity at runtime, without RTTI
using ComponentKey = const void *;
template <class T>
ComponentKey KeyOf() {
    static const char key = 0;
    return &key;
}

class Component {
public:
    virtual ~Component() = default;

    // The name this component travels under on the network
    virtual const char *TypeName() const { return nullptr; }

    // Every field the far end needs, in a fixed order
    virtual void Write(Writer &) const {}
    virtual void Read(Reader &) {}

    GameObject &Owner() const { return *owner; }

private:
    friend class GameObject;
    GameObject *owner = nullptr;
};

// Declares a component's network name
#define ENGINE_COMPONENT(Name)                                       \
    static constexpr const char *TYPE = #Name;                       \
    const char *TypeName() const override { return TYPE; }

class GameObject {
public:
    explicit GameObject(ObjectId id, std::string kind = {}, std::string name = {});
    GameObject(const GameObject &) = delete;
    GameObject &operator=(const GameObject &) = delete;

    ObjectId Id() const { return id; }

    // What sort of thing this is - "player", "platform" - as the game sees it
    const std::string &Kind() const { return kind; }
    void SetKind(std::string k) { kind = std::move(k); }

    // A human label for logs and lookups
    const std::string &Name() const { return name; }
    void SetName(std::string n) { name = std::move(n); }

    // Adds a component, or replaces one of the same type
    template <class T, class... Args>
    T &Add(Args &&...args) {
        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T &ref = *component;
        Attach(KeyOf<T>(), std::move(component));
        return ref;
    }

    template <class T>
    T *Get() {
        return static_cast<T *>(Find(KeyOf<T>()));
    }
    template <class T>
    const T *Get() const {
        return static_cast<const T *>(Find(KeyOf<T>()));
    }
    template <class T>
    bool Has() const {
        return Find(KeyOf<T>()) != nullptr;
    }
    template <class T>
    bool Remove() {
        return Detach(KeyOf<T>());
    }

    // The untyped forms, for code that only knows a component by its key
    void Attach(ComponentKey key, std::unique_ptr<Component> component);
    Component *Find(ComponentKey key) const;
    bool Detach(ComponentKey key);

    struct Slot {
        ComponentKey key;
        std::unique_ptr<Component> component;
        // Put here by a snapshot rather than by this process's own code
        bool replicated = false;
    };
    const std::vector<Slot> &Components() const { return components; }
    void MarkReplicated(ComponentKey key, bool replicated = true);
    bool IsReplicated(ComponentKey key) const;

    // True from World::Destroy() until the World's next Flush() removes it
    bool IsDestroyed() const { return destroyed; }

private:
    friend class World;

    ObjectId id;
    std::string kind;
    std::string name;
    std::vector<Slot> components;  // a handful per object: a linear scan beats a map
    bool destroyed = false;
};
}  // namespace engine
