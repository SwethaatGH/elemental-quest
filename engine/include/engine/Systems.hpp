#pragma once

#include "engine/Components.hpp"
#include "engine/World.hpp"

#include <SDL3/SDL.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace engine {
// The behaviour half of the object model
using ObjectFilter = std::function<bool(const GameObject &)>;

// -1 for objects with no NetIdentity, which are the authority's
int OwnerOf(const GameObject &object);
ObjectFilter OwnedBy(int owner);

struct PhysicsSettings {
    float gravity = 1800.0f;  // world units per second squared, for bodies with gravity
    float maxFallSpeed = 1400.0f;
    float maxStep = 1.0f / 120.0f;  // dt is split into steps no longer than this
};

// Something a body ran into during a physics step, from the body's side
struct Contact {
    enum class Side : uint8_t { Left, Right, Floor, Ceiling };
    ObjectId body;
    ObjectId other;
    Side side;
};
const char *SideName(Contact::Side side);  // "left", "right", "floor", "ceiling"

struct RespawnEvent {
    ObjectId object;
    ObjectId zone;
    ObjectId spawn;
};

// A mover inside a Trigger collider
struct TriggerContact {
    ObjectId trigger;
    ObjectId other;
};

// What changed since the last Update
struct TriggerEvent {
    enum class Type : uint8_t { Enter, Stay, Exit };
    Type type;
    ObjectId trigger;
    ObjectId other;
};

class TriggerTracker {
public:
    // Movers (Transform + RigidBody) that `filter` accepts, against every Trigger collider
    std::vector<TriggerEvent> Update(World &world, const ObjectFilter &filter = {});
    void Clear() { inside.clear(); }

private:
    std::vector<std::pair<ObjectId, ObjectId>> inside;  // sorted (trigger, other)
};

// Which key presses which button
struct KeyBinding {
    SDL_Scancode key;
    uint32_t button;  // an InputState::Button
};
// A/D or the arrows run, W/Up/Space jump, S/Down is Down, E/Enter is Action
std::vector<KeyBinding> DefaultKeyBindings();

namespace systems {
// Places every PathMover on its pattern for the given clock time
void MovePaths(World &world, double seconds);

// InputState + PlatformerController + RigidBody -> velocity
void ApplyControllers(World &world, const ObjectFilter &filter = {});

// Moves every Transform + RigidBody
void StepPhysics(World &world, float dt, const PhysicsSettings &settings = {}, const ObjectFilter &filter = {},
                 std::vector<Contact> *contacts = nullptr);

// Sends every Respawn object that overlaps a DeathZone to its spawn point
std::vector<RespawnEvent> ApplyDeathZones(World &world, const ObjectFilter &filter = {});

// Every mover (Transform + RigidBody, passing `filter`) inside every Trigger collider
std::vector<TriggerContact> FindTriggerContacts(World &world, const ObjectFilter &filter = {});

// Sets `input` from the keyboard: each binding whose key is down presses its button
void ReadKeyboard(InputState &input, const std::vector<KeyBinding> &bindings = DefaultKeyBindings(),
                  const std::function<bool(SDL_Scancode)> &isDown = {});

// The SpawnPoint with this index, or the lowest-indexed one; nullptr if none
GameObject *FindSpawn(World &world, int index);
// Stands `object` on the spawn point (bottom-centre to bottom-centre) and stops it
bool PlaceAtSpawn(World &world, GameObject &object, int index);

// Keeps each camera's scroll boundaries in place around its view
void UpdateCameras(World &world);

// Makes the two hidden side boundaries (and, if insetY > 0, top and bottom) for a camera
int AddScrollBoundaries(World &world, ObjectId camera, float insetX, float insetY = 0.0f);
}  // namespace systems

// Loads sprites named by Renderable::sprite from the media/ folder beside the build folder
class TextureCache {
public:
    SDL_Texture *Get(SDL_Renderer *renderer, const std::string &name);
    void SetRoot(const std::string &directory) { root = directory; }
    // A texture the game made itself, under a name Renderable::sprite can use
    void Put(const std::string &name, SDL_Texture *texture) { cache[name] = texture; }

private:
    std::map<std::string, SDL_Texture *> cache;  // a failed load is remembered as nullptr
    std::string root;
};

namespace systems {
// Draws every visible Renderable, lowest layer first, through `camera`
void Render(const World &world, SDL_Renderer *renderer, const Camera &camera, float scale,
            double seconds, TextureCache *textures = nullptr);
}  // namespace systems
}  // namespace engine
