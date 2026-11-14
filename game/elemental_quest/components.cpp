#include "components.hpp"

namespace ed {
const char *StyleName(Style s) {
    switch (s) {
    case Style::Ground: return "ground";
    case Style::Basalt: return "basalt";
    case Style::Ice: return "ice";
    case Style::Moss: return "moss stone";
    case Style::Cloud: return "cloud";
    case Style::Timber: return "timber raft";
    case Style::Lift: return "sandstone lift";
    case Style::Glider: return "clay glider";
    case Style::Goo: return "goo";
    }
    return "?";
}

void RegisterGameComponents() {
    engine::ComponentRegistry::RegisterBuiltins();
    engine::ComponentRegistry::Register<Player>();
    engine::ComponentRegistry::Register<Surface>();
    engine::ComponentRegistry::Register<Enemy>();
    engine::ComponentRegistry::Register<Gem>();
    engine::ComponentRegistry::Register<Plate>();
    engine::ComponentRegistry::Register<Gate>();
    engine::ComponentRegistry::Register<Door>();
    engine::ComponentRegistry::Register<Checkpoint>();
    engine::ComponentRegistry::Register<Match>();
}
}
