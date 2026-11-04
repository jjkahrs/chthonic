// The chat service's settings, all from environment variables, validated at startup (docs/chat/configuration.html).
#pragma once
#include <core/env.h>
#include <core/log.h>

#include <cstdint>
#include <string>
#include <vector>

namespace chthonic::chat {

struct Config {
    // network
    std::string host = "0.0.0.0";
    uint16_t port = 7778;     // game clients
    uint16_t linkPort = 7779; // world servers (internal)
    std::string linkSecret;   // shared with every world server's CHAT_LINK_SECRET
    uint32_t maxConnections = 5000;
    uint32_t maxFrameBytes = 4096;
    uint32_t handshakeTimeoutMs = 10000;
    uint32_t idleTimeoutMs = 60000;
    uint32_t pingIntervalMs = 20000;
    double msgRatePerSec = 20;
    double msgBurst = 40;
    uint32_t bindTimeoutMs = 5000; // how long an authenticated client waits for its character's CharOnline
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
    // database (mutes)
    std::string dbHost;
    uint16_t dbPort = 3306;
    std::string dbUser, dbPassword, dbName;
    uint32_t dbConnectTimeoutSec = 3;
    std::string migrationsDir = "./migrations";
    // chat
    std::vector<std::string> channels{"General", "Trade", "LFG"};
    uint32_t chatMaxBytes = 256;
    uint32_t chatRateCount = 5;
    uint32_t chatRateWindowMs = 10000;
    uint32_t muteMaxMinutes = 525600;
    log::Level logLevel = log::Level::info;
};

struct ConfigResult {
    Config config;
    std::vector<std::string> errors; // one line per bad or missing variable
};

ConfigResult load_config(const EnvLookup& env);

} // namespace chthonic::chat
