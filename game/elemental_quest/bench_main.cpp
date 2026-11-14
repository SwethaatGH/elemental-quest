#include "level.hpp"
#include "scene_host.hpp"
#include "server_game.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using namespace engine;
using namespace ed;
using Clock = std::chrono::steady_clock;

namespace {
struct Bench {
    NetOptions net;
    int statics = 100;
    int moving = 100;
    int clients = 2;
    long long iterations = 100000;
    long long warmup = 1000;
    int portBase = 9500;
};

long long IntArg(int argc, char **argv, const char *flag, long long fallback) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return std::atoll(argv[i + 1]);
    return fallback;
}

double Seconds(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }
double Avg(uint64_t total, uint64_t count) { return count ? static_cast<double>(total) / static_cast<double>(count) : 0.0; }

void PrintConditions(const Bench &b) {
    std::printf(" encoding=%s delta=%s static=%d moving=%d clients=%d", EncodingName(b.net.encoding),
                b.net.delta ? "on" : "off", b.statics, b.moving, b.clients);
}

uint32_t ScriptedButtons(int id, double t) {
    uint32_t buttons = std::fmod(t + 0.4 * id, 3.0) < 1.5 ? InputState::Right : InputState::Left;
    if (std::fmod(t + 0.2 * id, 0.7) < 0.05) buttons |= InputState::Jump;
    return buttons;
}

int RunServer(const Bench &b) {
    Engine engine;
    if (!engine.InitHeadless()) return 1;
    Server::Config cfg;
    cfg.joinPort = b.portBase;
    cfg.worldPort = b.portBase + 1;
    cfg.clientPortBase = b.portBase + 100;
    cfg.maxClients = kMaxPlayers;
    cfg.tickHz = 0;
    cfg.publishEverySteps = 1;
    cfg.logTag = "bench-server";
    SceneHost host(engine, cfg, b.net);
    BuildBenchLevel(host.GetWorld(), b.statics, b.moving);
    ServerGame game(engine.GetJobs(), engine.GetGameTime(), false);
    game.SetGateCycle(60);

    enum class Phase { Waiting, Warmup, Timed, Done } phase = Phase::Waiting;
    long long count = 0;
    Clock::time_point start, end;
    uint64_t msgs0 = 0, bytes0 = 0, enc0 = 0, up0 = 0, dec0 = 0, decoded0 = 0, keys0 = 0;
    size_t objects = 0;

    SceneHost::Callbacks cb;
    cb.onJoin = [&](World &w, int id) { game.OnJoin(w, id); };
    cb.onLeave = [&](World &w, int id) { game.OnLeave(w, id); };
    cb.step = [&](World &w, float dt) {
        game.Step(w, dt);
        switch (phase) {
        case Phase::Waiting:
            if (host.GetServer().ConnectedCount() >= b.clients) phase = Phase::Warmup;
            break;
        case Phase::Warmup:
            if (++count >= b.warmup) {
                phase = Phase::Timed;
                count = 0;
                const NetStats &s = host.Stats();
                msgs0 = s.messagesOut; bytes0 = s.bytesOut; enc0 = s.encodeMicros; up0 = s.messagesIn;
                dec0 = s.decodeMicros; decoded0 = s.decoded; keys0 = host.Keyframes();
                start = Clock::now();
            }
            break;
        case Phase::Timed:
            if (++count > b.iterations) {
                end = Clock::now();
                phase = Phase::Done;
                objects = w.Count();
                host.Stop();
            }
            break;
        case Phase::Done:
            break;
        }
    };
    if (!host.Run(cb) || phase != Phase::Done) {
        std::fprintf(stderr, "elemental_bench server: did not finish (ports in use, or stopped early)\n");
        return 1;
    }

    const NetStats &s = host.Stats();
    const uint64_t msgs = s.messagesOut - msgs0, bytes = s.bytesOut - bytes0;
    const double secs = Seconds(start, end);
    std::printf("RESULT role=server");
    PrintConditions(b);
    std::printf(" objects=%zu iterations=%lld seconds=%.6f us_per_iter=%.3f snapshots=%llu keyframes=%llu "
                "snapshot_bytes=%.1f mb_out=%.3f encode_us=%.3f upstream_msgs=%llu decode_us=%.3f\n",
                objects, b.iterations, secs, secs * 1e6 / static_cast<double>(b.iterations),
                static_cast<unsigned long long>(msgs), static_cast<unsigned long long>(host.Keyframes() - keys0),
                Avg(bytes, msgs), static_cast<double>(bytes) / 1e6, Avg(s.encodeMicros - enc0, msgs),
                static_cast<unsigned long long>(s.messagesIn - up0), Avg(s.decodeMicros - dec0, s.decoded - decoded0));
    std::fflush(stdout);
    return 0;
}

int RunClient(const Bench &b) {
    Client::Config cfg;
    cfg.joinPort = b.portBase;
    cfg.worldPort = b.portBase + 1;
    cfg.sendIntervalSeconds = 1.0f / 60.0f;
    SceneClient net(cfg, b.net);
    if (!net.Join()) {
        std::fprintf(stderr, "elemental_bench client: %s\n", net.LastError().c_str());
        return 1;
    }
    std::mutex mutex;
    {
        std::lock_guard<std::mutex> lock(mutex);
        MakePlayer(net.GetWorld(), net.Id());
    }
    const PhysicsSettings physics{kGravity, kMaxFallSpeed, 1.0f / 120.0f};
    const auto t0 = Clock::now();
    auto last = t0, lastHeard = t0;
    uint64_t heard = 0;
    long long loops = 0;
    double t = 0.0;
    while (true) {
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        t += dt;
        {
            std::lock_guard<std::mutex> lock(mutex);
            World &w = net.GetWorld();
            if (GameObject *me = w.Find(PlayerObjectId(net.Id()))) {
                me->Get<InputState>()->buttons = ScriptedButtons(net.Id(), t);
                systems::ApplyControllers(w, net.Authority());
                systems::StepPhysics(w, dt, physics, net.Authority());
                systems::ApplyDeathZones(w, net.Authority());
                net.StampLocal();
            }
        }
        net.Tick(dt, mutex);
        ++loops;
        if (net.Stats().messagesIn != heard) {
            heard = net.Stats().messagesIn;
            lastHeard = now;
        }
        if (net.GetClient().Dropped() || Seconds(lastHeard, now) > 1.0) break;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    net.Leave();
    const NetStats &s = net.Stats();
    const LatencySummary lat = s.Latency();
    std::printf("RESULT role=client id=%d", net.Id());
    PrintConditions(b);
    std::printf(" loops=%lld snapshots_in=%llu decoded=%llu skipped=%llu snapshot_bytes=%.1f decode_us=%.3f "
                "upstream_msgs=%llu upstream_bytes=%.1f latency_ms=%.3f latency_p95_ms=%.3f\n",
                loops, static_cast<unsigned long long>(s.messagesIn.load()),
                static_cast<unsigned long long>(s.decoded.load()), static_cast<unsigned long long>(s.skipped.load()),
                Avg(s.bytesIn, s.messagesIn), Avg(s.decodeMicros, s.decoded),
                static_cast<unsigned long long>(s.messagesOut.load()), Avg(s.bytesOut, s.messagesOut), lat.meanMs,
                lat.p95Ms);
    std::fflush(stdout);
    return 0;
}
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "usage: elemental_bench server|client [options] - see the top of bench_main.cpp\n");
        return 2;
    }
    Bench b;
    std::string error;
    if (!NetOptions::Parse(argc, argv, b.net, &error)) {
        std::fprintf(stderr, "elemental_bench: %s\n", error.c_str());
        return 2;
    }
    b.statics = static_cast<int>(IntArg(argc, argv, "--static", b.statics));
    b.moving = static_cast<int>(IntArg(argc, argv, "--moving", b.moving));
    b.clients = static_cast<int>(IntArg(argc, argv, "--clients", b.clients));
    b.iterations = IntArg(argc, argv, "--iterations", b.iterations);
    b.warmup = IntArg(argc, argv, "--warmup", b.warmup);
    b.portBase = static_cast<int>(IntArg(argc, argv, "--port-base", b.portBase));
    RegisterGameComponents();

    const std::string role = argv[1];
    if (role == "server") return RunServer(b);
    if (role == "client") return RunClient(b);
    std::fprintf(stderr, "elemental_bench: the role is server or client, not %s\n", role.c_str());
    return 2;
}
