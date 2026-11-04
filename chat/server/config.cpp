#include "config.h"

#include <limits>

namespace chthonic::chat {

ConfigResult load_config(const EnvLookup& env) {
    ConfigResult r;
    Config& c = r.config;
    EnvLoader l(env);
    constexpr double u32max = std::numeric_limits<uint32_t>::max();

    l.str("CHAT_HOST", c.host);
    l.num("CHAT_PORT", c.port, 1, 65535);
    l.num("CHAT_LINK_PORT", c.linkPort, 1, 65535);
    if (c.port == c.linkPort) l.errors.push_back("CHAT_LINK_PORT: must differ from CHAT_PORT");
    l.str("CHAT_LINK_SECRET", c.linkSecret, true);
    if (!c.linkSecret.empty() && c.linkSecret.size() < 16) l.errors.push_back("CHAT_LINK_SECRET: at least 16 characters");
    l.num("MAX_CONNECTIONS", c.maxConnections, 1, 1000000);
    l.num("MAX_FRAME_BYTES", c.maxFrameBytes, 64, 65535);
    l.num("HANDSHAKE_TIMEOUT_MS", c.handshakeTimeoutMs, 100, u32max);
    l.num("IDLE_TIMEOUT_MS", c.idleTimeoutMs, 1000, u32max);
    l.num("PING_INTERVAL_MS", c.pingIntervalMs, 100, u32max);
    l.num("MSG_RATE_PER_SEC", c.msgRatePerSec, 1, 100000);
    l.num("MSG_BURST", c.msgBurst, 1, 100000);
    l.num("CHAT_BIND_TIMEOUT_MS", c.bindTimeoutMs, 0, 600000);

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
    l.str("MIGRATIONS_DIR", c.migrationsDir);

    if (auto v = env("CHAT_CHANNELS")) {
        std::vector<std::string> channels;
        l.list("CHAT_CHANNELS", channels);
        c.channels = std::move(channels);
    }
    if (c.channels.empty() || c.channels.size() > 9) l.errors.push_back("CHAT_CHANNELS: 1 to 9 channel names");
    for (const auto& ch : c.channels) {
        bool ok = !ch.empty() && ch.size() <= 16;
        for (char x : ch) ok = ok && ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9'));
        if (!ok) l.errors.push_back("CHAT_CHANNELS: \"" + ch + "\" is not 1-16 letters or digits");
    }
    l.num("CHAT_MAX_BYTES", c.chatMaxBytes, 1, 4000);
    l.num("CHAT_RATE_COUNT", c.chatRateCount, 1, 1000);
    l.num("CHAT_RATE_WINDOW_MS", c.chatRateWindowMs, 1, u32max);
    l.num("CHAT_MUTE_MAX_MINUTES", c.muteMaxMinutes, 1, 5256000);

    std::string level;
    l.str("LOG_LEVEL", level);
    if (!level.empty() && !log::parse_level(level, c.logLevel))
        l.errors.push_back("LOG_LEVEL: must be debug, info, warn or error");

    r.errors = std::move(l.errors);
    return r;
}

} // namespace chthonic::chat
