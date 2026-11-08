// The chat service's network side on the io thread: game clients on CHAT_PORT (Hello -> Auth{JWT} -> held until the
// account's character is online -> AuthOk -> chat requests) and world servers on CHAT_LINK_PORT (LinkHello{secret} ->
// LinkOk + every mute -> CharOnline / MapChanged / CharOffline). Everything chat-specific is ChatRouter's.
#pragma once
#include "chat_router.h"
#include "config.h"
#include "mute_store.h"

#include <auth/token_service.h>
#include <net/preauth_limit.h>
#include <net/session.h>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <functional>
#include <memory>
#include <unordered_map>

namespace chthonic::chat {

class ChatServer {
public:
    ChatServer(asio::io_context& io, const Config& cfg, asio::ssl::context& tls, TokenService& tokens, MuteStore& store);

    ChatRouter& router() { return router_; }
    void start();
    // Disconnects everyone with server_shutdown, then calls onDone.
    void shutdown(std::function<void()> onDone);

private:
    enum class Phase { hello, auth, verifying, holding, bound };
    struct Client {
        std::shared_ptr<net::Session> session;
        Phase phase = Phase::hello;
        uint64_t accountId = 0;
        proto::Role role = proto::Role::player;
        std::shared_ptr<asio::steady_timer> hold; // while holding: CHAT_BIND_TIMEOUT_MS
        std::string preauthIp; // counted in preauth_ until bound (AuthOk); empty once released
    };
    struct Link {
        std::shared_ptr<net::Session> session;
        bool ok = false; // LinkHello accepted
    };

    void accept(asio::ip::tcp::acceptor& acceptor, bool link);
    void on_client(net::Session& s, proto::Message&& m);
    void on_link(net::Session& s, proto::Message&& m);
    void auth_done(uint32_t sessionId, const TokenResult& r);
    void bind(Client& c);
    void release_preauth(Client& c); // bound or closed: no longer counts against MAX_PREAUTH_PER_IP
    void drop_sessions(const std::vector<uint32_t>& sessions); // their character is gone: no_character
    void on_closed(net::Session& s);
    void schedule_purge();

    asio::io_context& io_;
    const Config& cfg_;
    asio::ssl::context& tls_;
    TokenService& tokens_;
    MuteStore& store_;
    net::SessionLimits clientLimits_, linkLimits_;
    asio::ip::tcp::acceptor clientAcceptor_, linkAcceptor_;
    ChatRouter router_;
    uint32_t nextSessionId_ = 1;
    std::unordered_map<uint32_t, Client> clients_;
    std::unordered_map<uint32_t, Link> links_;
    net::PreauthLimit preauth_; // MAX_PREAUTH_PER_IP over clients_ (the client port only)
    asio::steady_timer purgeTimer_;
    bool shuttingDown_ = false;
    std::function<void()> onShutdownDone_;
};

} // namespace chthonic::chat
