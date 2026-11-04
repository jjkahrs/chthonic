// Glues the pieces together on the game thread (its io_context): accepts sessions, walks each through
// Hello -> Auth -> lobby (character requests) -> world, runs the fixed-rate tick and the graceful shutdown. Tells the
// chat service (ChatLink, when CHAT_LINK_HOST is set) who is in the world and on which map, and each client where the
// chat service is (ChatServer after EnterWorldOk, when CHAT_PUBLIC_HOST is set). Answers the admin port (AdminPort,
// when WORLD_ADMIN_SECRET is set): who is online, and kicks. Sessions live on the net threads (net::NetThreads): their
// messages and closes are posted here and looked up by session id (the client may be gone by then); sends are queued back.
#pragma once
#include "../auth/token_service.h"
#include "../core/config.h"
#include "../core/tick_stats.h"
#include "../db/db_worker.h"
#include "../net/net_threads.h"
#include "../net/session.h"
#include "../world/world.h"
#include "admin_port.h"
#include "chat_link.h"

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <functional>
#include <memory>
#include <unordered_map>

namespace chthonic {

class GameServer {
public:
    // maps must outlive the server and hold cfg.startMap. firstInstanceId: WorldRules::firstInstanceId.
    GameServer(asio::io_context& io, net::NetThreads& net, const Config& cfg, const world::Maps& maps, asio::ssl::context& tls,
               db::DbWorker& db, TokenService& tokens, lua::Defs defs, uint64_t firstInstanceId);

    // Loads the Lua systems under LUA_ROOT and runs their init; every error (empty: OK to start).
    std::vector<std::string> load_scripts() { return world_.load_scripts(cfg_.luaRoot, cfg_.gameStateMachines); }
    std::vector<lua::SystemStatus> script_status() const { return world_.script_status(); }
    void start();
    // Stops accepting, saves and disconnects everyone with server_shutdown, then calls onDone.
    void shutdown(std::function<void()> onDone);

private:
    enum class Phase { hello, auth, verifying, lobby, world };
    struct Client {
        std::shared_ptr<net::Session> session;
        Phase phase = Phase::hello;
        uint64_t accountId = 0;
        proto::Role role = proto::Role::player;
        bool requestInFlight = false; // one character request at a time per client
        entt::entity entity = entt::null;
        uint64_t charId = 0;     // in the world
        std::string charName;    // in the world
        uint16_t reportedMap = 0; // the map the chat link last heard of
        bool leaving = false;     // disconnect asked for: its on_closed follows, nothing more is handled
        SysTime connectedAt{};
    };

    void accept();
    void on_message(uint32_t sessionId, proto::Message&& m);
    void on_closed(uint32_t sessionId);
    Client* find(uint32_t sessionId);
    void disconnect(Client& c, proto::Code reason);

    void handle_hello(Client& c, const proto::Hello& m);
    void handle_auth(Client& c, const proto::Auth& m);
    void auth_done(uint32_t sessionId, const TokenResult& r);
    void handle_lobby(Client& c, proto::Message&& m);
    void handle_world(Client& c, proto::Message&& m);
    void char_error(Client& c, proto::Opcode request, proto::Code code);
    void leave_world(Client& c);
    std::vector<proto::CharOnline> chat_snapshot() const; // every character in the world, for the chat link
    void report_maps();                                   // map changes since the last tick, to the chat link
    std::vector<proto::AdminOnlineEntry> admin_online() const; // every authenticated session, for the admin port
    uint32_t admin_kick(const proto::AdminKick& k);
    std::vector<proto::AdminInstanceEntry> admin_instances() const; // every live map context, for the admin port

    void schedule_tick();
    void tick();
    // contexts: one sample per map context per tick, its update ms (World::context_update_ms).
    void log_tick_stats(const char* msg, const TickStats& ticks, const TickStats& lag, const TickStats& contexts);
    // Each net thread's busy % (CPU time / wall time) since since/sinceCpu, which it then moves to now.
    void log_net_stats(const char* msg, TimePoint& since, std::vector<double>& sinceCpu, TimePoint now);

    asio::io_context& io_; // the game thread's
    net::NetThreads& net_;
    const Config& cfg_;
    const world::Maps& maps_;
    asio::ssl::context& tls_;
    db::DbWorker& db_;
    TokenService& tokens_;
    net::SessionLimits limits_;
    asio::ip::tcp::acceptor acceptor_; // on the game context; accepted sockets are made on a net context
    world::World world_;
    std::unique_ptr<ChatLink> chatLink_; // null without CHAT_LINK_HOST
    std::unique_ptr<AdminPort> adminPort_; // null without WORLD_ADMIN_SECRET

    uint32_t nextSessionId_ = 1;
    std::unordered_map<uint32_t, Client> clients_;
    std::unordered_map<uint64_t, uint32_t> online_; // accountId -> session id
    std::vector<net::Session*> fanOut_;             // the SendMany recipient buffer, kept for its capacity

    asio::steady_timer tickTimer_;
    TimePoint nextTick_{};
    Millis tickPeriod_;
    // Heap: each histogram is ~400 KB.
    std::unique_ptr<TickStats> windowTicks_, totalTicks_, windowLag_, totalLag_, windowContexts_, totalContexts_;
    TimePoint windowStart_{};
    TimePoint netWindowStart_{}, netStart_{};
    std::vector<double> netWindowCpu_, netStartCpu_; // NetThreads::cpu_ms at those times

    bool shuttingDown_ = false;
    std::function<void()> onShutdownDone_;
    asio::steady_timer shutdownTimer_;
};

} // namespace chthonic
