#include "game_server.h"
#include "../core/log.h"
#include "../world/rules.h"

#include <cmath>

namespace chthonic {
namespace {

world::WorldRules world_rules(const Config& c, uint64_t firstInstanceId) {
    world::WorldRules r;
    r.move = {c.maxSpeed, c.speedTolerance, c.moveBurstSec, c.minY, c.maxY, c.groundTolerance, c.maxAir, c.moveNavCheck};
    r.chat.maxBytes = c.chatMaxBytes;
    r.chat.yellRadius = c.chatYellRadius;
    r.cellSize = c.aoiCellSize;
    r.viewRadius = c.aoiViewRadius;
    r.startMap = c.startMap;
    r.chatRateCount = c.chatRateCount;
    r.chatRateWindow = Millis(c.chatRateWindowMs);
    r.saveInterval = Millis(c.saveIntervalMs);
    r.tickSeconds = 1.0f / static_cast<float>(c.tickHz);
    r.maxNpcs = c.maxNpcs;
    r.activationTiles = static_cast<int>(c.activationTiles);
    r.mapThreads = static_cast<int>(c.mapThreads);
    r.firstInstanceId = firstInstanceId;
    return r;
}

net::SessionLimits session_limits(const Config& c) {
    net::SessionLimits l;
    l.maxFrameBytes = c.maxFrameBytes;
    l.handshakeTimeout = Millis(c.handshakeTimeoutMs);
    l.idleTimeout = Millis(c.idleTimeoutMs);
    l.pingInterval = Millis(c.pingIntervalMs);
    l.msgRate = c.msgRatePerSec;
    l.msgBurst = c.msgBurst;
    l.msgRateGrace = Millis(c.msgRateGraceMs);
    l.maxSendBuffer = c.maxSendBufferBytes;
    l.simLatency = Millis(c.simLatencyMs);
    l.simJitter = Millis(c.simJitterMs);
    return l;
}

const char* role_name(proto::Role r) {
    switch (r) {
    case proto::Role::gm: return "gm";
    case proto::Role::admin: return "admin";
    default: return "player";
    }
}

} // namespace

GameServer::GameServer(asio::io_context& io, net::NetThreads& net, const Config& cfg, const world::Maps& maps,
                       asio::ssl::context& tls, db::DbWorker& db, TokenService& tokens, lua::Defs defs,
                       uint64_t firstInstanceId)
    : io_(io), net_(net), cfg_(cfg), maps_(maps), tls_(tls), db_(db), tokens_(tokens), limits_(session_limits(cfg)), acceptor_(io),
      world_(world_rules(cfg, firstInstanceId), maps,
             [this](uint32_t sid, const proto::Message& m) {
                 if (auto* c = find(sid)) c->session->send(m);
             },
             [this](std::span<const uint32_t> sids, const proto::Message& m) {
                 // In place: a send only queues for the net threads, so no on_closed runs before this returns.
                 fanOut_.clear();
                 for (uint32_t sid : sids)
                     if (auto* c = find(sid)) fanOut_.push_back(c->session.get());
                 net::send_many(fanOut_, m);
             },
             [this](const db::CharSave& s) { db_.save(s); }, std::move(defs)),
      tickTimer_(io), tickPeriod_(Millis(1000 / cfg.tickHz)), windowTicks_(std::make_unique<TickStats>()),
      totalTicks_(std::make_unique<TickStats>()), windowLag_(std::make_unique<TickStats>()),
      totalLag_(std::make_unique<TickStats>()), windowContexts_(std::make_unique<TickStats>()),
      totalContexts_(std::make_unique<TickStats>()), shutdownTimer_(io) {
    if (!cfg.adminSecret.empty()) {
        net::SessionLimits adminLimits = limits_;
        adminLimits.simLatency = adminLimits.simJitter = Millis(0); // the lag simulator is for game clients
        adminPort_ = std::make_unique<AdminPort>(
            io, net, tls, adminLimits, AdminPort::Options{cfg.adminHost, cfg.adminPort, cfg.adminSecret},
            [this] { return admin_online(); }, [this](const proto::AdminKick& k) { return admin_kick(k); },
            [this] { return admin_instances(); });
    }
    if (cfg.chatLinkHost.empty()) return;
    // The dev setup's chat service presents this server's own certificate (they share TLS_DEV_CERT_DIR).
    const std::string ownCert = cfg.tlsCertFile.empty() ? cfg.tlsDevCertDir + "/server.crt" : cfg.tlsCertFile;
    chatLink_ = std::make_unique<ChatLink>(
        io, net,
        ChatLink::Options{cfg.chatLinkHost, cfg.chatLinkPort, cfg.chatLinkSecret,
                          cfg.chatLinkCaFile.empty() ? ownCert : cfg.chatLinkCaFile, Millis(cfg.chatLinkRetryMinMs),
                          Millis(cfg.chatLinkRetryMaxMs)},
        [this] { return chat_snapshot(); },
        [this](const proto::MuteState& m) {
            if (m.untilMs == 0) world_.mutes().erase(m.name);
            else world_.mutes().set(m.name, SysTime(std::chrono::milliseconds(m.untilMs)));
        },
        [this] { world_.mutes().clear(); });
}

void GameServer::start() {
    const asio::ip::tcp::endpoint ep(asio::ip::make_address(cfg_.host), cfg_.port);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    log::info("listening", {{"host", cfg_.host}, {"port", cfg_.port}, {"realm", cfg_.realmId}, {"tickHz", cfg_.tickHz},
                            {"mapThreads", cfg_.mapThreads}});
    accept();
    if (adminPort_) adminPort_->start();
    else log::info("admin port off: WORLD_ADMIN_SECRET is not set");
    if (chatLink_) chatLink_->start();
    nextTick_ = SteadyClock::now();
    windowStart_ = netWindowStart_ = netStart_ = nextTick_;
    netWindowCpu_ = netStartCpu_ = net_.cpu_ms();
    schedule_tick();
}

// The acceptor runs here (clients_ and the connection limit are game state); each socket is made on the next net
// context and its Session stays there.
void GameServer::accept() {
    acceptor_.async_accept(net_.next(), [this](asio::error_code ec, asio::ip::tcp::socket socket) {
        if (shuttingDown_ || !acceptor_.is_open()) return;
        if (!ec) {
            socket.set_option(asio::ip::tcp::no_delay(true), ec);
            const auto peer = socket.remote_endpoint(ec);
            std::string ip = ec ? std::string("unknown") : peer.address().to_string();
            const uint32_t id = nextSessionId_++;
            // Both run on the session's net thread and post here, in order: its messages, then its close.
            auto session = std::make_shared<net::Session>(
                id, std::move(socket), tls_, limits_,
                [this](net::Session& s, proto::Message&& m) {
                    asio::post(io_, [this, sid = s.id(), m = std::move(m)]() mutable { on_message(sid, std::move(m)); });
                },
                [this](net::Session& s) { asio::post(io_, [this, sid = s.id()] { on_closed(sid); }); });
            if (clients_.size() >= cfg_.maxConnections || !preauth_.try_add(ip, cfg_.maxPreauthPerIp)) {
                session->start(proto::Code::server_full);
            } else {
                clients_[id].session = session;
                clients_[id].connectedAt = std::chrono::system_clock::now();
                clients_[id].preauthIp = std::move(ip);
                session->start();
            }
        }
        accept();
    });
}

GameServer::Client* GameServer::find(uint32_t sessionId) {
    auto it = clients_.find(sessionId);
    return it == clients_.end() ? nullptr : &it->second;
}

void GameServer::release_preauth(Client& c) {
    if (c.preauthIp.empty()) return;
    preauth_.release(c.preauthIp);
    c.preauthIp.clear();
}

void GameServer::disconnect(Client& c, proto::Code reason) {
    c.leaving = true;
    c.session->disconnect(reason);
}

// The session may have closed (or been told to) since it posted m: then c is gone or leaving and m is dropped.
void GameServer::on_message(uint32_t sid, proto::Message&& m) {
    Client* c = find(sid);
    if (!c || c->leaving || shuttingDown_) return;
    switch (c->phase) {
    case Phase::hello:
        if (auto* h = std::get_if<proto::Hello>(&m)) handle_hello(*c, *h);
        else disconnect(*c, proto::Code::unexpected_message);
        break;
    case Phase::auth:
        if (auto* a = std::get_if<proto::Auth>(&m)) handle_auth(*c, *a);
        else disconnect(*c, proto::Code::unexpected_message);
        break;
    case Phase::verifying: disconnect(*c, proto::Code::unexpected_message); break;
    case Phase::lobby: handle_lobby(*c, std::move(m)); break;
    case Phase::world: handle_world(*c, std::move(m)); break;
    }
}

void GameServer::handle_hello(Client& c, const proto::Hello& m) {
    if (m.protocolVersion != proto::kProtocolVersion) {
        disconnect(c, proto::Code::protocol_mismatch);
        return;
    }
    c.phase = Phase::auth;
}

void GameServer::handle_auth(Client& c, const proto::Auth& m) {
    c.phase = Phase::verifying;
    const uint32_t sid = c.session->id();
    tokens_.verify(m.accessToken, [this, sid](TokenResult r) { auth_done(sid, r); }); // never log the token
}

void GameServer::auth_done(uint32_t sid, const TokenResult& r) {
    Client* c = find(sid);
    if (!c || c->phase != Phase::verifying || shuttingDown_) return;
    if (r.code != proto::Code::ok) {
        log::info("auth rejected", {{"session", sid}, {"reason", proto::code_name(r.code)}});
        disconnect(*c, r.code);
        return;
    }
    if (auto it = online_.find(r.accountId); it != online_.end() && it->second != sid) {
        if (Client* old = find(it->second)) {
            log::info("duplicate login: kicking old session", {{"account", r.accountId}, {"old", it->second}, {"new", sid}});
            leave_world(*old); // its save is queued before anything the new session can ask for
            disconnect(*old, proto::Code::logged_in_elsewhere);
        }
    }
    online_[r.accountId] = sid;
    c->accountId = r.accountId;
    c->role = r.role;
    c->phase = Phase::lobby;
    release_preauth(*c);
    c->session->authenticated();
    c->session->send(proto::AuthOk{r.accountId, r.role});
    log::info("authenticated", {{"session", sid}, {"account", r.accountId}, {"role", role_name(r.role)}});
}

void GameServer::char_error(Client& c, proto::Opcode request, proto::Code code) {
    c.session->send(proto::CharError{request, code});
}

void GameServer::handle_lobby(Client& c, proto::Message&& m) {
    const proto::Opcode op = proto::opcode_of(m);
    const bool charRequest = op == proto::Opcode::CharListReq || op == proto::Opcode::CharCreate ||
                             op == proto::Opcode::CharDelete || op == proto::Opcode::EnterWorld;
    if (!charRequest) {
        if (op == proto::Opcode::Move || op == proto::Opcode::Say || op == proto::Opcode::ClientEvent ||
            op == proto::Opcode::Interact)
            return; // stale in-world traffic; harmless
        disconnect(c, proto::Code::unexpected_message);
        return;
    }
    if (c.requestInFlight) { char_error(c, op, proto::Code::rate_limited); return; }

    const uint32_t sid = c.session->id();
    const uint64_t acc = c.accountId;
    // Runs on the game thread when the DB reply arrives; the client may have left meanwhile.
    auto reply = [this, sid]() -> Client* {
        Client* cl = find(sid);
        if (!cl || cl->phase != Phase::lobby) return nullptr;
        cl->requestInFlight = false;
        return cl;
    };

    if (std::holds_alternative<proto::CharListReq>(m)) {
        c.requestInFlight = true;
        db_.query<std::vector<db::CharRow>>(
            [acc](db::CharRepo& repo) { return repo.list(acc); },
            [this, reply](std::optional<std::vector<db::CharRow>> rows) {
                Client* cl = reply();
                if (!cl) return;
                if (!rows) return char_error(*cl, proto::Opcode::CharListReq, proto::Code::service_unavailable);
                proto::CharList list;
                for (const auto& r : *rows) list.chars.push_back({r.id, r.name, r.pos, r.options});
                cl->session->send(list);
            });
    } else if (auto* create = std::get_if<proto::CharCreate>(&m)) {
        if (!valid_character_name(create->name)) return char_error(c, op, proto::Code::name_invalid);
        // C++ limits first: the Lua hook only ever sees small, plain options.
        if (!proto::valid_char_options(create->options) || !world_.validate_character(create->name, create->options))
            return char_error(c, op, proto::Code::options_invalid);
        c.requestInFlight = true;
        const auto& start = maps_.info(cfg_.startMap);
        const uint16_t map = start.id;
        const proto::Vec3 spawn{start.spawn[0], start.spawn[1], start.spawn[2]};
        const float heading = start.spawnHeading;
        const uint32_t maxChars = cfg_.maxCharsPerAccount;
        db_.query<db::CreateResult>(
            [acc, name = create->name, options = create->options, map, spawn, heading, maxChars](db::CharRepo& repo) {
                return repo.create(acc, name, options, map, spawn, heading, maxChars);
            },
            [this, reply](std::optional<db::CreateResult> res) {
                Client* cl = reply();
                if (!cl) return;
                if (!res) return char_error(*cl, proto::Opcode::CharCreate, proto::Code::service_unavailable);
                if (res->code != proto::Code::ok) return char_error(*cl, proto::Opcode::CharCreate, res->code);
                cl->session->send(proto::CharCreated{{res->row.id, res->row.name, res->row.pos, res->row.options}});
                log::info("character created", {{"account", cl->accountId}, {"char", res->row.id}});
            });
    } else if (auto* del = std::get_if<proto::CharDelete>(&m)) {
        c.requestInFlight = true;
        const uint64_t charId = del->charId;
        db_.query<bool>(
            [acc, charId](db::CharRepo& repo) { return repo.soft_delete(acc, charId); },
            [this, reply, charId](std::optional<bool> ok) {
                Client* cl = reply();
                if (!cl) return;
                if (!ok) return char_error(*cl, proto::Opcode::CharDelete, proto::Code::service_unavailable);
                if (!*ok) return char_error(*cl, proto::Opcode::CharDelete, proto::Code::not_found);
                cl->session->send(proto::CharDeleted{charId});
                log::info("character deleted", {{"account", cl->accountId}, {"char", charId}});
            });
    } else if (auto* enter = std::get_if<proto::EnterWorld>(&m)) {
        c.requestInFlight = true;
        const uint64_t charId = enter->charId;
        db_.query_for_char<std::optional<db::CharRow>>(
            charId, [acc, charId](db::CharRepo& repo) { return repo.enter(acc, charId); },
            [this, reply, sid](std::optional<std::optional<db::CharRow>> row) {
                Client* cl = reply();
                if (!cl) return;
                if (!row) return char_error(*cl, proto::Opcode::EnterWorld, proto::Code::service_unavailable);
                if (!*row) return char_error(*cl, proto::Opcode::EnterWorld, proto::Code::not_found);
                std::optional<proto::ChatServer> chat; // sent right after EnterWorldOk
                if (!cfg_.chatPublicHost.empty()) chat = proto::ChatServer{cfg_.chatPublicHost, cfg_.chatPublicPort};
                cl->entity = world_.spawn(sid, **row, SteadyClock::now(), chat);
                cl->phase = Phase::world;
                cl->charId = (*row)->id;
                cl->charName = (*row)->name;
                cl->reportedMap = world_.map_of(cl->entity);
                if (chatLink_) chatLink_->char_online({cl->accountId, cl->charId, cl->charName, cl->reportedMap});
                log::info("entered world", {{"account", cl->accountId}, {"char", (*row)->id},
                                            {"population", world_.population()}});
            });
    }
}

void GameServer::handle_world(Client& c, proto::Message&& m) {
    if (auto* mv = std::get_if<proto::Move>(&m)) { world_.push_move(c.entity, *mv); return; }
    if (auto* say = std::get_if<proto::Say>(&m)) { world_.push_say(c.entity, std::move(*say)); return; }
    if (auto* in = std::get_if<proto::Interact>(&m)) { world_.push_interact(c.entity, in->objectId); return; }
    if (auto* ev = std::get_if<proto::ClientEvent>(&m)) {
        auto data = client_event_data(*ev);
        if (!data) {
            log::info("client event rejected", {{"session", c.session->id()}, {"bytes", ev->json.size()}});
            disconnect(c, proto::Code::malformed);
            return;
        }
        world_.push_client_event(c.entity, std::move(ev->name), std::move(*data), ev->requestId);
        return;
    }
    const proto::Opcode op = proto::opcode_of(m);
    if (op == proto::Opcode::CharListReq || op == proto::Opcode::CharCreate || op == proto::Opcode::CharDelete ||
        op == proto::Opcode::EnterWorld) {
        char_error(c, op, proto::Code::already_in_world);
        return;
    }
    disconnect(c, proto::Code::unexpected_message);
}

void GameServer::leave_world(Client& c) {
    if (c.phase != Phase::world) return;
    world_.despawn(c.entity); // queues the save
    if (chatLink_) chatLink_->char_offline(c.charId);
    c.entity = entt::null;
    c.phase = Phase::lobby;
}

std::vector<proto::CharOnline> GameServer::chat_snapshot() const {
    std::vector<proto::CharOnline> chars;
    for (const auto& [id, c] : clients_)
        if (c.phase == Phase::world) chars.push_back({c.accountId, c.charId, c.charName, c.reportedMap});
    return chars;
}

void GameServer::report_maps() {
    for (auto& [id, c] : clients_) {
        if (c.phase != Phase::world) continue;
        const uint16_t map = world_.map_of(c.entity);
        if (map == c.reportedMap) continue;
        c.reportedMap = map;
        chatLink_->map_changed(c.charId, map);
    }
}

std::vector<proto::AdminOnlineEntry> GameServer::admin_online() const {
    std::vector<proto::AdminOnlineEntry> out;
    for (const auto& [id, c] : clients_) {
        if ((c.phase != Phase::lobby && c.phase != Phase::world) || c.leaving) continue;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(c.connectedAt.time_since_epoch()).count();
        if (c.phase == Phase::world)
            out.push_back({c.accountId, c.charId, c.charName, world_.map_of(c.entity), id, static_cast<uint64_t>(ms)});
        else
            out.push_back({c.accountId, 0, "", 0, id, static_cast<uint64_t>(ms)});
    }
    return out;
}

std::vector<proto::AdminInstanceEntry> GameServer::admin_instances() const {
    const auto ms = [](SysTime t) {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count());
    };
    std::vector<proto::AdminInstanceEntry> out;
    for (const auto& c : world_.contexts())
        out.push_back({c.map, maps_.info(c.map).name, c.instance, c.key.value_or(""), static_cast<uint32_t>(c.players),
                       static_cast<uint32_t>(c.npcs), ms(c.createdAt), c.emptySince ? ms(*c.emptySince) : 0});
    return out;
}

// Like the duplicate-login kick: the character leaves the world (its save is queued), then the session is told why.
uint32_t GameServer::admin_kick(const proto::AdminKick& k) {
    uint32_t kicked = 0;
    for (auto& [id, c] : clients_) {
        if (c.phase != Phase::lobby && c.phase != Phase::world) continue;
        if (c.leaving) continue;
        if (k.accountId ? c.accountId != k.accountId : (c.phase != Phase::world || c.charId != k.charId)) continue;
        log::info("kicked by an admin", {{"session", id}, {"account", c.accountId}, {"char", c.charId}});
        leave_world(c);
        disconnect(c, proto::Code::kicked); // only queued: clients_ does not change here
        ++kicked;
    }
    return kicked;
}

void GameServer::on_closed(uint32_t sid) {
    auto it = clients_.find(sid);
    if (it == clients_.end()) return;
    Client& c = it->second;
    leave_world(c);
    release_preauth(c);
    if (auto o = online_.find(c.accountId); o != online_.end() && o->second == sid) online_.erase(o);
    clients_.erase(it);
    if (shuttingDown_ && clients_.empty() && onShutdownDone_) {
        shutdownTimer_.cancel();
        auto done = std::move(onShutdownDone_);
        onShutdownDone_ = nullptr;
        done();
    }
}

// --- tick ---

void GameServer::schedule_tick() {
    nextTick_ += tickPeriod_;
    tickTimer_.expires_at(nextTick_);
    tickTimer_.async_wait([this](asio::error_code ec) {
        if (!ec && !shuttingDown_) tick();
    });
}

void GameServer::tick() {
    const TimePoint start = SteadyClock::now();
    const double lagMs = std::chrono::duration<double, std::milli>(start - nextTick_).count();
    world_.tick(start);
    if (chatLink_) report_maps();
    const double ms = std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
    windowTicks_->add(ms);
    totalTicks_->add(ms);
    windowLag_->add(lagMs);
    totalLag_->add(lagMs);
    for (const double ctxMs : world_.context_update_ms()) {
        windowContexts_->add(ctxMs);
        totalContexts_->add(ctxMs);
    }
    if (start - windowStart_ >= std::chrono::seconds(cfg_.tickStatsIntervalSec)) {
        log_tick_stats("tick stats", *windowTicks_, *windowLag_, *windowContexts_);
        log_net_stats("net stats", netWindowStart_, netWindowCpu_, start);
        windowTicks_->reset();
        windowLag_->reset();
        windowContexts_->reset();
        windowStart_ = start;
    }
    if (SteadyClock::now() > nextTick_ + tickPeriod_) nextTick_ = SteadyClock::now(); // fell behind: don't burst
    schedule_tick();
}

void GameServer::log_tick_stats(const char* msg, const TickStats& ticks, const TickStats& lag,
                                const TickStats& contexts) {
    const auto saves = db_.stats();
    log::info(msg, {{"ticks", ticks.count()},
                    {"p50_ms", ticks.percentile(0.50)},
                    {"p99_ms", ticks.percentile(0.99)},
                    {"max_ms", ticks.max()},
                    {"lag_p99_ms", lag.percentile(0.99)},
                    {"lag_max_ms", lag.max()},
                    {"map_threads", cfg_.mapThreads},
                    {"contexts", world_.context_update_ms().size()},
                    {"ctx_p50_ms", contexts.percentile(0.50)},
                    {"ctx_p99_ms", contexts.percentile(0.99)},
                    {"ctx_max_ms", contexts.max()},
                    {"population", world_.population()},
                    {"connections", clients_.size()},
                    {"saves_ok", saves.ok},
                    {"saves_failed_attempts", saves.failedAttempts},
                    {"saves_pending", saves.pending}});
}

void GameServer::log_net_stats(const char* msg, TimePoint& since, std::vector<double>& sinceCpu, TimePoint now) {
    const auto cpu = net_.cpu_ms();
    const double wallMs = std::max(1.0, std::chrono::duration<double, std::milli>(now - since).count());
    auto busy = nlohmann::json::array();
    for (size_t i = 0; i < cpu.size() && i < sinceCpu.size(); ++i)
        busy.push_back(std::round((cpu[i] - sinceCpu[i]) / wallMs * 1000) / 10); // %, one decimal
    log::info(msg, {{"net_threads", cpu.size()}, {"busy_pct", busy}});
    since = now;
    sinceCpu = cpu;
}

// --- shutdown ---

void GameServer::shutdown(std::function<void()> onDone) {
    if (shuttingDown_) return;
    shuttingDown_ = true;
    log::info("shutting down", {{"connections", clients_.size()}, {"population", world_.population()}});
    asio::error_code ec;
    acceptor_.close(ec);
    tickTimer_.cancel();
    if (chatLink_) chatLink_->stop();
    if (adminPort_) adminPort_->stop();
    log_tick_stats("tick stats total", *totalTicks_, *totalLag_, *totalContexts_);
    log_net_stats("net stats total", netStart_, netStartCpu_, SteadyClock::now());

    world_.shutdown_scripts(); // onLeave changes are part of the saves below
    onShutdownDone_ = std::move(onDone);
    for (auto& [id, c] : clients_) leave_world(c);
    world_.remove_npcs(); // no players left to tell; NPCs are never saved
    for (auto& [id, c] : clients_) disconnect(c, proto::Code::server_shutdown); // each on_closed comes back posted
    if (clients_.empty()) {
        auto done = std::move(onShutdownDone_);
        onShutdownDone_ = nullptr;
        done();
        return;
    }
    // Clients that never read get closed by their session's 2 s grace timer; this is a backstop.
    shutdownTimer_.expires_after(Millis(3000));
    shutdownTimer_.async_wait([this](asio::error_code e) {
        if (e || !onShutdownDone_) return;
        auto done = std::move(onShutdownDone_);
        onShutdownDone_ = nullptr;
        done();
    });
}

} // namespace chthonic
