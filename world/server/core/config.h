// All settings come from environment variables, validated at startup (see docs/world/configuration.html for the table).
#pragma once
#include "env.h"
#include "log.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace chthonic {

struct Config {
    // network
    std::string host = "0.0.0.0";
    uint16_t port = 7777;
    uint32_t maxConnections = 1000;
    uint32_t maxFrameBytes = 4096;
    uint32_t handshakeTimeoutMs = 10000;
    uint32_t idleTimeoutMs = 60000;
    uint32_t pingIntervalMs = 20000;
    double msgRatePerSec = 60;
    double msgBurst = 120;
    uint32_t msgRateGraceMs = 2000;
    uint32_t maxSendBufferBytes = 1 << 20;
    uint32_t netThreads = 4; // threads doing TLS and socket I/O for every connection; the tick has its own
    // lag simulator (testing only): each message, both ways, held latency +- jitter ms, in order; 0 = off
    uint32_t simLatencyMs = 0, simJitterMs = 0;
    // TLS
    std::string tlsCertFile, tlsKeyFile;
    std::string tlsDevCertDir = "./certs";
    // auth
    std::string jwksUrl;
    std::string jwtIssuer = "chthonic-auth";
    std::string jwtAudience = "chthonic";
    uint32_t jwtClockSkewSec = 5;
    uint32_t jwksRefreshSec = 300;
    uint32_t jwksMinRefetchSec = 10;
    uint32_t jwksTimeoutMs = 3000;
    // database
    std::string dbHost;
    uint16_t dbPort = 3306;
    std::string dbUser, dbPassword, dbName;
    uint32_t dbConnectTimeoutSec = 3;
    uint32_t dbPoolSize = 4; // DbWorker threads, one connection each
    std::string migrationsDir = "./migrations";
    // world
    uint32_t realmId = 0;
    uint32_t tickHz = 20;
    std::string mapRoot = "/app/maps"; // <folder>/map.json per map
    uint16_t startMap = 1;             // map id new characters start on (its spawn)
    float minY = -100, maxY = 500;
    float maxSpeed = 10, speedTolerance = 1, moveBurstSec = 1;
    float groundTolerance = 0.5f, maxAir = 6; // m below / above the ground (rules.h MoveRules)
    bool moveNavCheck = true;                 // moves must end on the map's navmesh, if it has one
    float aoiCellSize = 64, aoiViewRadius = 128;
    uint32_t maxCharsPerAccount = 10;
    uint32_t maxNpcs = 10000;
    uint32_t activationTiles = 1; // tiles around a player's (Chebyshev) whose spawns load and whose NPCs stay awake
    uint32_t mapThreads = 1;      // workers updating the map contexts at once; 1: all on the tick thread
    uint32_t chatMaxBytes = 256;
    uint32_t chatRateCount = 5;
    uint32_t chatRateWindowMs = 10000;
    float chatYellRadius = 300; // m, same map; may exceed aoiViewRadius
    // chat service (docs/chat): where clients find it (empty host: not announced) and the internal link
    // (empty host: no link; mutes then never reach this server)
    std::string chatPublicHost;
    uint16_t chatPublicPort = 7778;
    std::string chatLinkHost;
    uint16_t chatLinkPort = 7779;
    std::string chatLinkSecret;
    std::string chatLinkCaFile; // empty: trust this server's own TLS certificate (the dev setup shares it)
    uint32_t chatLinkRetryMinMs = 500, chatLinkRetryMaxMs = 30000;
    // admin port (online list, kick) for the admin service; empty secret: off
    std::string adminHost = "127.0.0.1";
    uint16_t adminPort = 7780;
    std::string adminSecret;
    // persistence / ops
    uint32_t saveIntervalMs = 60000;
    uint32_t saveRetryMinMs = 500;
    uint32_t saveRetryMaxMs = 30000;
    uint32_t shutdownTimeoutMs = 10000;
    uint32_t tickStatsIntervalSec = 10;
    std::string luaRoot = "/app/lua"; // components/, fsms/ and systems/
    std::vector<std::string> gameStateMachines; // game state machine names (fsms/ definitions) run for the lifetime
    log::Level logLevel = log::Level::info;
};

struct ConfigResult {
    Config config;
    std::vector<std::string> errors; // one line per bad or missing variable
};

ConfigResult load_config(const EnvLookup& env);

} // namespace chthonic
