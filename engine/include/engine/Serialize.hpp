#pragma once

#include "engine/GameObject.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace engine {
// Milestone 3, Parts 1B and 1C: how objects become bytes
enum class Encoding : uint8_t { Text, Binary };

const char *EncodingName(Encoding encoding);
// "text" or "binary"; false for anything else
bool ParseEncoding(const std::string &name, Encoding &out);

struct ComponentInfo;

class Writer {
public:
    virtual ~Writer() = default;
    virtual void U8(uint8_t v) = 0;
    virtual void U16(uint16_t v) = 0;
    virtual void U32(uint32_t v) = 0;
    virtual void I32(int32_t v) = 0;
    virtual void I64(int64_t v) = 0;
    virtual void F32(float v) = 0;
    virtual void Str(const std::string &v) = 0;
    void Bool(bool v) { U8(v ? 1 : 0); }

    // Names a component type: its name in text, its 16-bit wire id in binary
    virtual void Tag(const ComponentInfo &info) = 0;

    // Appends the output of another writer of the same encoding
    virtual void Raw(const std::string &encoded) = 0;

    virtual const std::string &Data() const = 0;
    virtual std::string Take() = 0;
    virtual void Clear() = 0;
};

class Reader {
public:
    virtual ~Reader() = default;
    virtual uint8_t U8() = 0;
    virtual uint16_t U16() = 0;
    virtual uint32_t U32() = 0;
    virtual int32_t I32() = 0;
    virtual int64_t I64() = 0;
    virtual float F32() = 0;
    virtual std::string Str() = 0;
    bool Bool() { return U8() != 0; }

    // The component type a Tag() named, or nullptr
    virtual const ComponentInfo *Tag() = 0;

    bool Ok() const { return ok; }
    void Fail() { ok = false; }
    virtual bool AtEnd() const = 0;

protected:
    bool ok = true;
};

std::unique_ptr<Writer> MakeWriter(Encoding encoding);
// Reads `data` from byte `start`
std::unique_ptr<Reader> MakeReader(Encoding encoding, const std::string &data, size_t start = 0);

// Every component type that can travel
struct ComponentInfo {
    const char *name;
    uint16_t wireId;  // a hash of the name, so it is the same in every process
    ComponentKey key;
    std::unique_ptr<Component> (*make)();
};

class ComponentRegistry {
public:
    template <class T>
    static void Register() {
        Add(T::TYPE, KeyOf<T>(), []() -> std::unique_ptr<Component> { return std::make_unique<T>(); });
    }
    // The engine's own components (Components.hpp)
    static void RegisterBuiltins();

    static const ComponentInfo *ByName(const std::string &name);
    static const ComponentInfo *ByName(const char *name, size_t length);
    static const ComponentInfo *ByWireId(uint16_t wireId);
    static const ComponentInfo *ByKey(ComponentKey key);

private:
    static void Add(const char *name, ComponentKey key, std::unique_ptr<Component> (*make)());
};
}  // namespace engine
