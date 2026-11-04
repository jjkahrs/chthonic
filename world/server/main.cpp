// Chthonic World Server: TLS game connections, auth-service JWTs, every map under MAP_ROOT on a fixed tick, MySQL
// persistence.
// Threads: game (this one: the tick, GameServer, World, Lua, chat link and admin port logic, signals), NET_THREADS net
// threads (TLS and socket I/O; sessions post decoded messages to the game thread), DB_POOL_SIZE DB workers, token
// verifier, and MAP_THREADS - 1 map context workers during a tick.
#include "app/game_server.h"
#include "crash_dialogs.h"
#include "auth/token_service.h"
#include "core/config.h"
#include "core/log.h"
#include "db/char_repo.h"
#include "db/db_worker.h"
#include "db/migrate.h"
#include "lua/components.h"
#include "world/components.h"
#include "world/maps.h"
#include "net/net_threads.h"
#include "net/tls_context.h"

#include <asio.hpp>

#include <cstdio>
#include <filesystem>

using namespace chthonic;

int main() {
    disableCrashDialogsIfRequested();
    const ConfigResult loaded = load_config(process_env);
    if (!loaded.errors.empty()) {
        std::fprintf(stderr, "Invalid configuration:\n");
        for (const auto& e : loaded.errors) std::fprintf(stderr, "  %s\n", e.c_str());
        return 1;
    }
    const Config& cfg = loaded.config;
    log::set_level(cfg.logLevel);
    if (cfg.simLatencyMs || cfg.simJitterMs)
        log::warn("LAG SIMULATOR ON: every message is delayed both ways; never run this in production",
                  {{"SIM_LATENCY_MS", cfg.simLatencyMs}, {"SIM_JITTER_MS", cfg.simJitterMs}});

    // Maps, then the Lua component definitions: a broken one stops the start before anything else happens.
    std::vector<std::string> mapErrors;
    const world::Maps maps = world::Maps::load(cfg.mapRoot, mapErrors);
    if (mapErrors.empty() && !maps.by_id(cfg.startMap))
        mapErrors.push_back("START_MAP " + std::to_string(cfg.startMap) + ": no such map under MAP_ROOT");
    else if (mapErrors.empty() && maps.info(cfg.startMap).instanced())
        mapErrors.push_back("START_MAP " + std::to_string(cfg.startMap) + ": an instance map; START_MAP must be a world map");
    for (const auto& [id, m] : maps.all())
        if (m.spawn[1] < cfg.minY || m.spawn[1] > cfg.maxY)
            mapErrors.push_back("map " + std::to_string(id) + ": spawn y outside [MIN_Y, MAX_Y]");
    if (!mapErrors.empty()) {
        for (const auto& e : mapErrors) log::error("map error", {{"error", e}});
        log::error("startup failed: maps", {{"errors", mapErrors.size()}, {"mapRoot", cfg.mapRoot}});
        return 1;
    }
    nlohmann::json loadedMaps = nlohmann::json::array();
    for (const auto& [id, m] : maps.all()) loadedMaps.push_back({{"id", id}, {"name", m.name}, {"tileRects", m.tiles.size()}, {"areas", m.areas.list.size()}, {"spawns", m.spawns.size()}, {"type", m.instanced() ? "instance" : "world"}});
    log::info("maps loaded", {{"mapRoot", cfg.mapRoot}, {"startMap", cfg.startMap}, {"activationTiles", cfg.activationTiles}, {"maps", loadedMaps}});

    // Lua component definitions: a broken one stops the start too.
    std::error_code ec;
    const bool haveLua = std::filesystem::is_directory(cfg.luaRoot, ec);
    lua::cache_lua_files(); // every map context and instance copy loads the same LUA_ROOT
    lua::DefsResult defs;
    if (haveLua) defs = lua::load_component_defs(cfg.luaRoot, world::kReservedComponentNames);
    else log::info("no Lua components or systems (LUA_ROOT is not a folder)", {{"luaRoot", cfg.luaRoot}});

    db::library_init();
    const db::ConnectOptions dbOpts{cfg.dbHost, cfg.dbPort, cfg.dbUser, cfg.dbPassword, cfg.dbName,
                                    cfg.dbConnectTimeoutSec, 5};
    uint64_t firstInstanceId = 1;
    try {
        db::Connection conn(dbOpts);
        const auto applied = db::migrate(conn, cfg.migrationsDir);
        log::info("migrations done", {{"applied", applied.size()}});
        // Above every saved id, so a relog never joins a new copy that took an old copy's id.
        firstInstanceId = uint64_t{db::CharRepo(conn, cfg.realmId).max_instance_id()} + 1;
        log::info("instance ids start", {{"firstInstanceId", firstInstanceId}});
    } catch (const std::exception& e) {
        log::error("startup failed: database/migrations", {{"error", e.what()}});
        return 1;
    }

    try {
        asio::ssl::context tls = net::make_server_tls({cfg.tlsCertFile, cfg.tlsKeyFile, cfg.tlsDevCertDir});
        // Outlive everything below: sessions (and handlers still queued for them) hold sockets on these contexts.
        net::NetThreads net(cfg.netThreads);
        asio::io_context io(1); // the game thread's
        auto post = [&io](std::function<void()> f) { asio::post(io, std::move(f)); };

        db::DbWorker dbWorker({dbOpts, cfg.realmId, Millis(cfg.saveRetryMinMs), Millis(cfg.saveRetryMaxMs), cfg.dbPoolSize},
                              post);
        TokenService tokens({cfg.jwksUrl, {cfg.jwtIssuer, cfg.jwtAudience, cfg.jwtClockSkewSec},
                             Millis(cfg.jwksRefreshSec * 1000ull), Millis(cfg.jwksMinRefetchSec * 1000ull),
                             Millis(cfg.jwksTimeoutMs)},
                            post);
        const size_t defCount = defs.defs.size();
        GameServer server(io, net, cfg, maps, tls, dbWorker, tokens, std::move(defs.defs), firstInstanceId);
        // Destroyed first, on every way out: no net thread may still run (and post into io) once server is gone.
        struct JoinNet {
            net::NetThreads& n;
            ~JoinNet() { n.stop(); }
        } joinNet{net};
        std::vector<std::string> luaErrors = std::move(defs.errors);
        // Also without LUA_ROOT: a GAME_STATE_MACHINES name then has no definition, which refuses the start.
        for (std::string& e : server.load_scripts()) luaErrors.push_back(std::move(e));
        if (!luaErrors.empty()) {
            for (const auto& e : luaErrors) log::error("Lua error", {{"error", e}});
            log::error("startup failed: Lua", {{"errors", luaErrors.size()}, {"luaRoot", cfg.luaRoot}});
            tokens.stop();
            dbWorker.stop(SteadyClock::now());
            return 1;
        }
        if (haveLua) {
            const auto systems = server.script_status();
            log::info("Lua loaded", {{"luaRoot", cfg.luaRoot}, {"components", defCount}, {"systems", systems.size()},
                                     {"gameStateMachines", cfg.gameStateMachines}});
        }
        server.start();
        log::info("threads", {{"game", 1}, {"net", cfg.netThreads}, {"db", cfg.dbPoolSize}, {"map", cfg.mapThreads}});

        TimePoint shutdownStart{};
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](asio::error_code ec, int sig) {
            if (ec) return;
            log::info("signal received", {{"signal", sig}});
            shutdownStart = SteadyClock::now();
            server.shutdown([&io] { io.stop(); });
        });
        io.run();

        // Everything after the last client is gone: flush saves within what is left of the shutdown budget.
        net.stop();
        tokens.stop();
        const auto deadline = shutdownStart + Millis(cfg.shutdownTimeoutMs) - Millis(500);
        const size_t lost = dbWorker.stop(deadline);
        const auto stats = dbWorker.stats();
        if (lost > 0) {
            log::error("shutdown: saves not flushed", {{"pending", lost}});
            return 1;
        }
        log::info("shutdown complete", {{"saves_ok", stats.ok}, {"saves_failed_attempts", stats.failedAttempts}});
        return 0;
    } catch (const std::exception& e) {
        log::error("fatal", {{"error", e.what()}});
        return 1;
    }
}
