// Chthonic Chat Service: one per realm. Whispers, the fixed channels and the zone channel over TLS for game clients
// (auth-service JWTs), GM mutes in MySQL, and the internal link world servers report their characters on.
// Threads: io (network + routing), token verifier, mute writer.
#include "chat_server.h"
#include "config.h"

#include <auth/token_service.h>
#include <core/log.h>
#include <db/migrate.h>
#include <net/tls_context.h>

#include <asio.hpp>

#include <cstdio>

using namespace chthonic;

int main() {
    const chat::ConfigResult loaded = chat::load_config(process_env);
    if (!loaded.errors.empty()) {
        std::fprintf(stderr, "Invalid configuration:\n");
        for (const auto& e : loaded.errors) std::fprintf(stderr, "  %s\n", e.c_str());
        return 1;
    }
    const chat::Config& cfg = loaded.config;
    log::set_level(cfg.logLevel);

    db::library_init();
    const db::ConnectOptions dbOpts{cfg.dbHost, cfg.dbPort, cfg.dbUser, cfg.dbPassword, cfg.dbName, cfg.dbConnectTimeoutSec, 5};
    chat::MuteStore store(dbOpts);
    std::vector<std::pair<std::string, SysTime>> mutes;
    try {
        db::Connection conn(dbOpts);
        const auto applied = db::migrate(conn, cfg.migrationsDir, "chthonic_chat_migrate");
        log::info("migrations done", {{"applied", applied.size()}});
        mutes = store.load(std::chrono::system_clock::now());
    } catch (const std::exception& e) {
        log::error("startup failed: database/migrations", {{"error", e.what()}});
        return 1;
    }

    try {
        asio::io_context io(1);
        asio::ssl::context tls = net::make_server_tls({cfg.tlsCertFile, cfg.tlsKeyFile, cfg.tlsDevCertDir});
        auto post = [&io](std::function<void()> f) { asio::post(io, std::move(f)); };
        TokenService tokens({cfg.jwksUrl, {cfg.jwtIssuer, cfg.jwtAudience, cfg.jwtClockSkewSec},
                             Millis(cfg.jwksRefreshSec * 1000ull), Millis(cfg.jwksMinRefetchSec * 1000ull),
                             Millis(cfg.jwksTimeoutMs)},
                            post);
        chat::ChatServer server(io, cfg, tls, tokens, store);
        for (const auto& [name, until] : mutes) server.router().load_mute(name, until);
        log::info("mutes loaded", {{"count", mutes.size()}});
        server.start();

        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](asio::error_code ec, int sig) {
            if (ec) return;
            log::info("signal received", {{"signal", sig}});
            server.shutdown([&io] { io.stop(); });
        });
        io.run();
        tokens.stop();
        log::info("shutdown complete");
        return 0;
    } catch (const std::exception& e) {
        log::error("fatal", {{"error", e.what()}});
        return 1;
    }
}
