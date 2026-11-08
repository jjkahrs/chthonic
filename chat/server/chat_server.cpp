#include "chat_server.h"

#include <core/log.h>

#include <openssl/crypto.h>

namespace chthonic::chat {
namespace {

net::SessionLimits client_limits(const Config& c) {
    net::SessionLimits l;
    l.maxFrameBytes = c.maxFrameBytes;
    l.handshakeTimeout = Millis(c.handshakeTimeoutMs + c.bindTimeoutMs); // the hold counts as handshake
    l.idleTimeout = Millis(c.idleTimeoutMs);
    l.pingInterval = Millis(c.pingIntervalMs);
    l.msgRate = c.msgRatePerSec;
    l.msgBurst = c.msgBurst;
    return l;
}

// World servers: trusted once the secret matched; only the frame limit and the heartbeat apply.
net::SessionLimits link_limits(const Config& c) {
    net::SessionLimits l = client_limits(c);
    l.handshakeTimeout = Millis(c.handshakeTimeoutMs);
    l.msgRate = 100000;
    l.msgBurst = 100000;
    l.maxSendBuffer = 16u << 20;
    return l;
}

bool same_secret(const std::string& a, const std::string& b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

uint64_t unix_ms(SysTime t) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count());
}

RouterRules router_rules(const Config& c) {
    return {c.channels, c.chatMaxBytes, c.chatRateCount, Millis(c.chatRateWindowMs), c.muteMaxMinutes};
}

} // namespace

ChatServer::ChatServer(asio::io_context& io, const Config& cfg, asio::ssl::context& tls, TokenService& tokens,
                       MuteStore& store)
    : io_(io), cfg_(cfg), tls_(tls), tokens_(tokens), store_(store), clientLimits_(client_limits(cfg)),
      linkLimits_(link_limits(cfg)), clientAcceptor_(io), linkAcceptor_(io),
      router_(
          router_rules(cfg),
          [this](uint32_t sid, const proto::Message& m) {
              if (auto it = clients_.find(sid); it != clients_.end()) it->second.session->send(m);
          },
          [this](const proto::MuteState& m) {
              for (auto& [id, link] : links_)
                  if (link.ok) link.session->send(m);
          },
          [this](const MuteChange& m) { store_.save(m); }),
      purgeTimer_(io) {}

void ChatServer::start() {
    for (auto [acceptor, port] : {std::pair{&clientAcceptor_, cfg_.port}, std::pair{&linkAcceptor_, cfg_.linkPort}}) {
        const asio::ip::tcp::endpoint ep(asio::ip::make_address(cfg_.host), port);
        acceptor->open(ep.protocol());
        acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor->bind(ep);
        acceptor->listen();
    }
    log::info("listening", {{"host", cfg_.host}, {"port", cfg_.port}, {"linkPort", cfg_.linkPort}, {"channels", cfg_.channels}});
    accept(clientAcceptor_, false);
    accept(linkAcceptor_, true);
    schedule_purge();
}

void ChatServer::accept(asio::ip::tcp::acceptor& acceptor, bool link) {
    acceptor.async_accept([this, &acceptor, link](asio::error_code ec, asio::ip::tcp::socket socket) {
        if (shuttingDown_ || !acceptor.is_open()) return;
        if (!ec) {
            socket.set_option(asio::ip::tcp::no_delay(true), ec);
            const auto peer = socket.remote_endpoint(ec);
            std::string ip = ec ? std::string("unknown") : peer.address().to_string();
            const uint32_t id = nextSessionId_++;
            auto session = std::make_shared<net::Session>(
                id, std::move(socket), tls_, link ? linkLimits_ : clientLimits_,
                [this, link](net::Session& s, proto::Message&& m) {
                    if (link) on_link(s, std::move(m));
                    else on_client(s, std::move(m));
                },
                [this](net::Session& s) { on_closed(s); });
            if (link) {
                links_[id].session = session;
                session->start();
            } else if (clients_.size() >= cfg_.maxConnections || !preauth_.try_add(ip, cfg_.maxPreauthPerIp)) {
                session->start(proto::Code::server_full);
            } else {
                clients_[id].session = session;
                clients_[id].preauthIp = std::move(ip);
                session->start();
            }
        }
        accept(acceptor, link);
    });
}

// --- game clients ---

void ChatServer::on_client(net::Session& s, proto::Message&& m) {
    auto it = clients_.find(s.id());
    if (it == clients_.end() || shuttingDown_) return;
    Client& c = it->second;
    switch (c.phase) {
    case Phase::hello:
        if (auto* h = std::get_if<proto::Hello>(&m); h && h->protocolVersion == proto::kProtocolVersion) c.phase = Phase::auth;
        else s.disconnect(h ? proto::Code::protocol_mismatch : proto::Code::unexpected_message);
        return;
    case Phase::auth:
        if (auto* a = std::get_if<proto::Auth>(&m)) {
            c.phase = Phase::verifying;
            tokens_.verify(a->accessToken, [this, sid = s.id()](TokenResult r) { auth_done(sid, r); }); // never log the token
        } else {
            s.disconnect(proto::Code::unexpected_message);
        }
        return;
    case Phase::verifying:
    case Phase::holding: s.disconnect(proto::Code::unexpected_message); return;
    case Phase::bound: break;
    }
    const proto::Opcode op = proto::opcode_of(m);
    if (op == proto::Opcode::ChatSend || op == proto::Opcode::ChatJoin || op == proto::Opcode::ChatLeave ||
        op == proto::Opcode::ChatIgnore || op == proto::Opcode::ChatMute)
        router_.handle(s.id(), m, SteadyClock::now(), std::chrono::system_clock::now());
    else
        s.disconnect(proto::Code::unexpected_message);
}

void ChatServer::auth_done(uint32_t sid, const TokenResult& r) {
    auto it = clients_.find(sid);
    if (it == clients_.end() || it->second.phase != Phase::verifying || shuttingDown_) return;
    Client& c = it->second;
    if (r.code != proto::Code::ok) {
        log::info("auth rejected", {{"session", sid}, {"reason", proto::code_name(r.code)}});
        c.session->disconnect(r.code);
        return;
    }
    c.accountId = r.accountId;
    c.role = r.role;
    if (router_.can_bind(c.accountId)) return bind(c);
    // Its world server may not have reported the character yet (the client connects right after EnterWorldOk): hold.
    c.phase = Phase::holding;
    c.hold = std::make_shared<asio::steady_timer>(io_, Millis(cfg_.bindTimeoutMs));
    c.hold->async_wait([this, sid](asio::error_code ec) {
        auto held = clients_.find(sid);
        if (ec || held == clients_.end() || held->second.phase != Phase::holding) return;
        log::info("no character online for the account", {{"session", sid}, {"account", held->second.accountId}});
        held->second.session->disconnect(proto::Code::no_character);
    });
}

void ChatServer::bind(Client& c) {
    if (c.hold) c.hold->cancel();
    c.phase = Phase::bound;
    release_preauth(c);
    c.session->authenticated();
    c.session->send(proto::AuthOk{c.accountId, c.role});
    if (auto replaced = router_.bind(c.session->id(), c.accountId, c.role))
        if (auto old = clients_.find(*replaced); old != clients_.end()) old->second.session->disconnect(proto::Code::logged_in_elsewhere);
    log::info("chat client bound", {{"session", c.session->id()}, {"account", c.accountId}});
}

void ChatServer::release_preauth(Client& c) {
    if (c.preauthIp.empty()) return;
    preauth_.release(c.preauthIp);
    c.preauthIp.clear();
}

void ChatServer::drop_sessions(const std::vector<uint32_t>& sessions) {
    for (uint32_t sid : sessions)
        if (auto it = clients_.find(sid); it != clients_.end()) it->second.session->disconnect(proto::Code::no_character);
}

// --- world links ---

void ChatServer::on_link(net::Session& s, proto::Message&& m) {
    auto it = links_.find(s.id());
    if (it == links_.end() || shuttingDown_) return;
    Link& link = it->second;
    if (!link.ok) {
        const auto* hello = std::get_if<proto::LinkHello>(&m);
        if (!hello) return s.disconnect(proto::Code::unexpected_message);
        if (hello->protocolVersion != proto::kProtocolVersion) return s.disconnect(proto::Code::protocol_mismatch);
        if (!same_secret(hello->secret, cfg_.linkSecret)) {
            log::warn("world link refused: wrong secret", {{"session", s.id()}, {"remote", s.remote()}});
            return s.disconnect(proto::Code::invalid_token);
        }
        link.ok = true;
        s.authenticated();
        s.send(proto::LinkOk{});
        const SysTime now = std::chrono::system_clock::now();
        size_t mutes = 0;
        for (const auto& [name, until] : router_.mutes().all())
            if (until > now) {
                s.send(proto::MuteState{name, unix_ms(until)});
                ++mutes;
            }
        log::info("world link up", {{"session", s.id()}, {"remote", s.remote()}, {"mutes", mutes}});
        return;
    }
    if (auto* on = std::get_if<proto::CharOnline>(&m)) {
        drop_sessions(router_.char_online(s.id(), *on));
        for (auto& [sid, c] : clients_) // clients held for this account
            if (c.phase == Phase::holding && c.accountId == on->accountId && router_.can_bind(c.accountId)) {
                bind(c);
                break;
            }
    } else if (auto* mc = std::get_if<proto::MapChanged>(&m)) {
        router_.map_changed(*mc);
    } else if (auto* off = std::get_if<proto::CharOffline>(&m)) {
        drop_sessions(router_.char_offline(off->charId));
    } else {
        s.disconnect(proto::Code::unexpected_message);
    }
}

void ChatServer::on_closed(net::Session& s) {
    if (auto it = clients_.find(s.id()); it != clients_.end()) {
        if (it->second.hold) it->second.hold->cancel();
        release_preauth(it->second);
        router_.client_closed(s.id());
        clients_.erase(it);
    } else if (auto l = links_.find(s.id()); l != links_.end()) {
        if (l->second.ok) {
            log::warn("world link down", {{"session", s.id()}, {"remote", s.remote()}});
            drop_sessions(router_.link_lost(s.id()));
        }
        links_.erase(l);
    }
    if (shuttingDown_ && clients_.empty() && links_.empty() && onShutdownDone_) {
        auto done = std::move(onShutdownDone_);
        onShutdownDone_ = nullptr;
        done();
    }
}

// Expired mutes leave memory and the database once a minute; the worlds check the end time themselves.
void ChatServer::schedule_purge() {
    purgeTimer_.expires_after(std::chrono::minutes(1));
    purgeTimer_.async_wait([this](asio::error_code ec) {
        if (ec || shuttingDown_) return;
        const SysTime now = std::chrono::system_clock::now();
        if (const size_t n = router_.expire_mutes(now)) log::info("mutes expired", {{"count", n}});
        store_.purge(now);
        schedule_purge();
    });
}

void ChatServer::shutdown(std::function<void()> onDone) {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    log::info("shutting down", {{"clients", clients_.size()}, {"links", links_.size()}});
    asio::error_code ec;
    clientAcceptor_.close(ec);
    linkAcceptor_.close(ec);
    purgeTimer_.cancel();
    std::vector<std::shared_ptr<net::Session>> sessions;
    for (auto& [id, c] : clients_) sessions.push_back(c.session);
    for (auto& [id, l] : links_) sessions.push_back(l.session);
    if (sessions.empty()) return onDone();
    onShutdownDone_ = std::move(onDone);
    for (auto& s : sessions) s->disconnect(proto::Code::server_shutdown);
}

} // namespace chthonic::chat
