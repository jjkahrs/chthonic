#include "config.h"

#include <map.h>

#include <cmath>
#include <limits>

namespace chthonic {

ConfigResult load_config(const EnvLookup& env) {
    ConfigResult r;
    Config& c = r.config;
    EnvLoader l(env);
    constexpr double u32max = std::numeric_limits<uint32_t>::max();

    l.str("WORLD_HOST", c.host);
    l.num("WORLD_PORT", c.port, 1, 65535);
    l.num("MAX_CONNECTIONS", c.maxConnections, 1, 1000000);
    l.num("MAX_PREAUTH_PER_IP", c.maxPreauthPerIp, 0, 1000000);
    l.num("MAX_FRAME_BYTES", c.maxFrameBytes, 64, 65535);
    l.num("HANDSHAKE_TIMEOUT_MS", c.handshakeTimeoutMs, 100, u32max);
    l.num("IDLE_TIMEOUT_MS", c.idleTimeoutMs, 1000, u32max);
    l.num("PING_INTERVAL_MS", c.pingIntervalMs, 100, u32max);
    l.num("MSG_RATE_PER_SEC", c.msgRatePerSec, 1, 100000);
    l.num("MSG_BURST", c.msgBurst, 1, 100000);
    l.num("MSG_RATE_GRACE_MS", c.msgRateGraceMs, 0, u32max);
    l.num("MAX_SEND_BUFFER_BYTES", c.maxSendBufferBytes, 4096, u32max);
    l.num("NET_THREADS", c.netThreads, 1, 64);
    l.num("SIM_LATENCY_MS", c.simLatencyMs, 0, 10000);
    l.num("SIM_JITTER_MS", c.simJitterMs, 0, 10000);

    l.str("TLS_CERT_FILE", c.tlsCertFile);
    l.str("TLS_KEY_FILE", c.tlsKeyFile);
    l.str("TLS_DEV_CERT_DIR", c.tlsDevCertDir);
    if (c.tlsCertFile.empty() != c.tlsKeyFile.empty())
        l.errors.push_back("TLS_CERT_FILE/TLS_KEY_FILE: set both or neither");

    l.str("AUTH_JWKS_URL", c.jwksUrl, true);
    if (!c.jwksUrl.empty() && c.jwksUrl.rfind("http://", 0) != 0 && c.jwksUrl.rfind("https://", 0) != 0)
        l.errors.push_back("AUTH_JWKS_URL: must start with http:// or https://");
    l.str("JWT_ISSUER", c.jwtIssuer);
    l.str("JWT_AUDIENCE", c.jwtAudience);
    l.num("JWT_CLOCK_SKEW_SECONDS", c.jwtClockSkewSec, 0, 60);
    l.num("JWKS_REFRESH_SECONDS", c.jwksRefreshSec, 1, 86400);
    l.num("JWKS_MIN_REFETCH_SECONDS", c.jwksMinRefetchSec, 0, 3600);
    l.num("JWKS_TIMEOUT_MS", c.jwksTimeoutMs, 100, 60000);

    l.str("DB_HOST", c.dbHost, true);
    l.num("DB_PORT", c.dbPort, 1, 65535);
    l.str("DB_USER", c.dbUser, true);
    l.str("DB_PASSWORD", c.dbPassword, true);
    l.str("DB_NAME", c.dbName, true);
    l.num("DB_CONNECT_TIMEOUT_SECONDS", c.dbConnectTimeoutSec, 1, 60);
    l.num("DB_POOL_SIZE", c.dbPoolSize, 1, 32);
    l.str("MIGRATIONS_DIR", c.migrationsDir);

    l.num("REALM_ID", c.realmId, 1, u32max, true);
    l.num("TICK_HZ", c.tickHz, 1, 1000);
    l.str("MAP_ROOT", c.mapRoot);
    l.num("START_MAP", c.startMap, 0, 65535);
    l.num("MIN_Y", c.minY, -1e6, 1e6);
    l.num("MAX_Y", c.maxY, -1e6, 1e6);
    if (c.minY > c.maxY) l.errors.push_back("MIN_Y/MAX_Y: MIN_Y must be <= MAX_Y");
    l.num("MAX_SPEED", c.maxSpeed, 0.1, 1e4);
    l.num("SPEED_TOLERANCE", c.speedTolerance, 0, 1e4);
    l.num("MOVE_BURST_SECONDS", c.moveBurstSec, 0.05, 60);
    l.num("MOVE_GROUND_TOLERANCE", c.groundTolerance, 0, 1e4);
    l.num("MOVE_MAX_AIR", c.maxAir, 0, 1e4);
    if (uint32_t navCheck = 1; l.num("MOVE_NAV_CHECK", navCheck, 0, 1)) c.moveNavCheck = navCheck == 1;
    l.num("AOI_CELL_SIZE", c.aoiCellSize, 1, 1e5);
    if (std::fmod(map::kTileSize, c.aoiCellSize) != 0) l.errors.push_back("AOI_CELL_SIZE: must divide the 256 m tile size");
    l.num("AOI_VIEW_RADIUS", c.aoiViewRadius, 1, 1e5);
    l.num("MAX_CHARS_PER_ACCOUNT", c.maxCharsPerAccount, 1, 255);
    l.num("MAX_NPCS", c.maxNpcs, 0, 100000);
    l.num("ACTIVATION_TILES", c.activationTiles, 1, 32);
    l.num("MAP_THREADS", c.mapThreads, 1, 256);
    l.num("CHAT_MAX_BYTES", c.chatMaxBytes, 1, 4000);
    l.num("CHAT_RATE_COUNT", c.chatRateCount, 1, 1000);
    l.num("CHAT_RATE_WINDOW_MS", c.chatRateWindowMs, 1, u32max);
    l.num("CHAT_YELL_RADIUS", c.chatYellRadius, 1, 1e5);
    l.str("CHAT_PUBLIC_HOST", c.chatPublicHost);
    l.num("CHAT_PUBLIC_PORT", c.chatPublicPort, 1, 65535);
    l.str("CHAT_LINK_HOST", c.chatLinkHost);
    l.num("CHAT_LINK_PORT", c.chatLinkPort, 1, 65535);
    l.str("CHAT_LINK_SECRET", c.chatLinkSecret);
    l.str("CHAT_LINK_CA_FILE", c.chatLinkCaFile);
    l.num("CHAT_LINK_RETRY_MIN_MS", c.chatLinkRetryMinMs, 10, u32max);
    l.num("CHAT_LINK_RETRY_MAX_MS", c.chatLinkRetryMaxMs, 10, u32max);
    if (c.chatLinkRetryMinMs > c.chatLinkRetryMaxMs) l.errors.push_back("CHAT_LINK_RETRY_MIN_MS: must be <= CHAT_LINK_RETRY_MAX_MS");
    if (!c.chatLinkHost.empty() && c.chatLinkSecret.size() < 16)
        l.errors.push_back("CHAT_LINK_SECRET: at least 16 characters when CHAT_LINK_HOST is set");
    l.str("WORLD_ADMIN_HOST", c.adminHost);
    l.num("WORLD_ADMIN_PORT", c.adminPort, 1, 65535);
    l.str("WORLD_ADMIN_SECRET", c.adminSecret);
    if (!c.adminSecret.empty() && c.adminSecret.size() < 16)
        l.errors.push_back("WORLD_ADMIN_SECRET: at least 16 characters (or unset: no admin port)");

    l.num("SAVE_INTERVAL_MS", c.saveIntervalMs, 100, u32max);
    l.num("SAVE_RETRY_MIN_MS", c.saveRetryMinMs, 10, u32max);
    l.num("SAVE_RETRY_MAX_MS", c.saveRetryMaxMs, 10, u32max);
    if (c.saveRetryMinMs > c.saveRetryMaxMs) l.errors.push_back("SAVE_RETRY_MIN_MS: must be <= SAVE_RETRY_MAX_MS");
    l.num("SHUTDOWN_TIMEOUT_MS", c.shutdownTimeoutMs, 100, u32max);
    l.num("TICK_STATS_INTERVAL_SECONDS", c.tickStatsIntervalSec, 1, 86400);
    l.str("LUA_ROOT", c.luaRoot);
    l.list("GAME_STATE_MACHINES", c.gameStateMachines);

    std::string level;
    l.str("LOG_LEVEL", level);
    if (!level.empty() && !log::parse_level(level, c.logLevel))
        l.errors.push_back("LOG_LEVEL: must be debug, info, warn or error");

    r.errors = std::move(l.errors);
    return r;
}

} // namespace chthonic
