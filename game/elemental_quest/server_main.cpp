#include "level.hpp"
#include "server_game.hpp"

#include "engine/Engine.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace engine;
using namespace ed;

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    NetOptions options;
    options.encoding = Encoding::Binary;
    std::string error;
    bool eventLog = false;
    // The web gateway runs one server per room, each on its own ports
    int portBase = kJoinPort;
    int clientTimeoutMs = 3000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--event-log") == 0) eventLog = true;
        else if (std::strcmp(argv[i], "--port-base") == 0 && i + 1 < argc) portBase = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--client-timeout-ms") == 0 && i + 1 < argc) clientTimeoutMs = std::atoi(argv[++i]);
    }
    if (portBase <= 0 || portBase > 65000 || clientTimeoutMs < 0) {
        std::fprintf(stderr, "--port-base takes a port up to 65000, --client-timeout-ms a non-negative number\n");
        return 2;
    }
    if (!NetOptions::Parse(argc, argv, options, &error)) {
        std::fprintf(stderr, "%s\nusage: %s [--event-log] [--port-base N] [--client-timeout-ms MS] [--encoding text|binary] "
                             "[--delta on|off] [--keyframe N]\n",
                     error.c_str(), argv[0]);
        return 2;
    }
    if (options.topology != Topology::ClientServer || options.upstream != Upstream::State) {
        std::fprintf(stderr, "Elemental Quest runs client-server, with clients sending state.\n");
        return 2;
    }
    RegisterGameComponents();

    Engine engine;
    if (!engine.InitHeadless()) return 1;

    Server::Config config;
    // Join, world and client ports keep their usual spacing from the base
    config.joinPort = portBase;
    config.worldPort = portBase + (kWorldPort - kJoinPort);
    config.clientPortBase = portBase + (kClientPortBase - kJoinPort);
    config.clientTimeoutMs = clientTimeoutMs;
    config.maxClients = kMaxPlayers;
    config.tickHz = 60;
    config.publishEverySteps = 1;
    config.logTag = "elemental-server";

    // SceneServer carries every snapshot and every shared event
    SceneServer host(engine, config, options);
    BuildLevel(host.GetWorld());

    ServerGame game(engine.GetJobs(), engine.GetGameTime());
    game.SetNet(&host.Stats(), &host.Options());
    game.SetEventLog(eventLog);
    game.Share(host);

    SceneServer::Callbacks callbacks;
    callbacks.onJoin = [&](World &w, int id) { game.OnJoin(w, id); };
    callbacks.onLeave = [&](World &w, int id) { game.OnLeave(w, id); };
    callbacks.step = [&](World &w, float dt) { game.Step(w, dt); };

    SDL_Log("[game] Elemental Quest server, CLIENT-SERVER, data format %s. %zu objects in the level. Up to %d players "
            "(FIRE, WATER, EARTH, AIR). Ctrl-C to stop.",
            options.Describe().c_str(), host.GetWorld().Count(), kMaxPlayers);
    const bool ok = host.Run(callbacks);
    engine.Shutdown();
    return ok ? 0 : 1;
}
