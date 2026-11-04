#include "scenarios.h"

#include <heights.h>
#include <asio/experimental/awaitable_operators.hpp>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace chthonic::bot {
namespace {

using proto::Code;

struct Ctx {
    const Options& o;
    asio::ssl::context& tls;
    AuthClient auth;
    Checker check;

    // Real access token for the bot account `tag` (registers + verifies on first use).
    std::string token(const std::string& tag) {
        std::string err;
        auto t = auth.ensure_login(bot_account(o, tag), err);
        check.expect(t.has_value(), "access token for " + tag + (t ? "" : " (" + err + ")"));
        return t.value_or("");
    }

    asio::awaitable<std::shared_ptr<WorldClient>> login(const std::string& token, const std::string& who) {
        std::string err;
        auto c = co_await connect_and_login(o, tls, token, err);
        check.expect(c != nullptr, who + " TLS connect + AuthOk" + (c ? "" : " (" + err + ")"));
        co_return c;
    }

    // login + enter with a fresh random character name
    asio::awaitable<std::shared_ptr<WorldClient>> enter(const std::string& tag, proto::EnterWorldOk& ok) {
        auto c = co_await login(token(tag), tag);
        if (!c) co_return nullptr;
        std::string err;
        const bool in = co_await enter_world(*c, random_name(), ok, err, o.charOptions);
        check.expect(in, tag + " entered world" + (in ? "" : " (" + err + ")"));
        co_return in ? c : nullptr;
    }
};

template <typename T>
std::vector<T> only(const std::vector<proto::Message>& msgs) {
    std::vector<T> out;
    for (const auto& m : msgs)
        if (auto* t = std::get_if<T>(&m)) out.push_back(*t);
    return out;
}

// Every EntityMoves entry in msgs, in order.
std::vector<proto::EntityMoves::Entry> moves_in(const std::vector<proto::Message>& msgs) {
    std::vector<proto::EntityMoves::Entry> out;
    for (const auto& m : only<proto::EntityMoves>(msgs)) out.insert(out.end(), m.entries.begin(), m.entries.end());
    return out;
}

std::string fmt(const proto::Vec3& v) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
    return buf;
}

// The next player Spawn: NPC spawns (the sample Guard patrols the spawn point) are skipped.
asio::awaitable<std::optional<proto::Spawn>> wait_player_spawn(WorldClient& c, Millis timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto left = std::chrono::duration_cast<Millis>(deadline - std::chrono::steady_clock::now());
        auto s = co_await c.wait_for<proto::Spawn>(std::max(left, Millis(0)));
        if (!s || s->kind == proto::SpawnKind::player) co_return s;
    }
}

bool same_pos(const proto::Vec3& a, const proto::Vec3& b) {
    return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f && std::fabs(a.z - b.z) < 0.01f;
}

// Waits for a CharError or the expected reply; returns the CharError code, or ok.
template <typename Reply>
asio::awaitable<Code> request(WorldClient& c, const proto::Message& m, std::optional<Reply>* reply = nullptr) {
    c.send(m);
    for (;;) {
        auto r = co_await c.next(Millis(10000));
        if (!r) co_return Code::malformed; // no answer
        if (auto* e = std::get_if<proto::CharError>(&*r)) co_return e->code;
        if (auto* ok = std::get_if<Reply>(&*r)) {
            if (reply) *reply = *ok;
            co_return Code::ok;
        }
    }
}

// --- 1 ---
asio::awaitable<void> auth_handshake(Ctx& x) {
    auto c = co_await x.login(x.token("s1"), "s1");
    if (!c) co_return;
    x.check.expect(c->authOk->accountId > 0, "AuthOk carries account id " + std::to_string(c->authOk->accountId));
    x.check.expect(c->authOk->role == proto::Role::player, "AuthOk role is player");
    c->close();
}

// --- 2 ---
asio::awaitable<void> characters(Ctx& x) {
    auto c = co_await x.login(x.token("s2"), "s2");
    if (!c) co_return;
    std::vector<std::string> names;
    bool allCreated = true;
    for (int i = 0; i < 10; ++i) {
        names.push_back(random_name());
        std::optional<proto::CharCreated> r;
        allCreated &= co_await request(*c, proto::CharCreate{names.back()}, &r) == Code::ok && r &&
                      r->character.name == names.back();
    }
    x.check.expect(allCreated, "10 characters created");
    x.check.expect(co_await request<proto::CharCreated>(*c, proto::CharCreate{random_name()}) == Code::char_limit,
                   "11th character -> char_limit");

    std::optional<proto::CharList> list;
    co_await request(*c, proto::CharListReq{}, &list);
    x.check.expect(list && list->chars.size() == 10, "CharList has 10");

    std::optional<proto::CharDeleted> del;
    const uint64_t victim = list ? list->chars.front().id : 0;
    x.check.expect(co_await request(*c, proto::CharDelete{victim}, &del) == Code::ok, "delete one character");
    co_await request(*c, proto::CharListReq{}, &list);
    x.check.expect(list && list->chars.size() == 9, "CharList has 9 after delete");

    std::string lower = names[1];
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    x.check.expect(co_await request<proto::CharCreated>(*c, proto::CharCreate{lower}) == Code::name_taken,
                   "duplicate name in other case -> name_taken");
    x.check.expect(co_await request<proto::CharCreated>(*c, proto::CharCreate{names[0]}) == Code::name_taken,
                   "deleted character's name stays reserved -> name_taken");
    for (const char* bad : {"ab", "Abcdefghijklmnopq", "Bob1", "Bo b", "B\xC3\xA9o", ""})
        x.check.expect(co_await request<proto::CharCreated>(*c, proto::CharCreate{bad}) == Code::name_invalid,
                       std::string("invalid name \"") + bad + "\" -> name_invalid");
    x.check.expect(co_await request<proto::CharDeleted>(*c, proto::CharDelete{victim}) == Code::not_found,
                   "deleting an already deleted character -> not_found");
    x.check.expect(list && std::ranges::all_of(list->chars, [](const proto::CharInfo& ch) { return ch.options.empty(); }),
                   "characters created without options list none");
    // Creation options: the C++ limits first, then the server Lua (the default lua/ has no validateCharacter, so any
    // option is refused).
    proto::CharOptions nine;
    for (char k = 'a'; k <= 'i'; ++k) nine[std::string(1, k)] = "x";
    for (const auto& [bad, what] : {std::pair{proto::CharOptions{{"class", "mage"}}, "an option without a Lua hook"},
                                    std::pair{proto::CharOptions{{"Class", "mage"}}, "an upper-case key"},
                                    std::pair{proto::CharOptions{{"class", std::string(33, 'x')}}, "a 33-char value"},
                                    std::pair{nine, "9 options"}})
        x.check.expect(co_await request<proto::CharCreated>(*c, proto::CharCreate{random_name(), bad}) == Code::options_invalid,
                       std::string(what) + " -> options_invalid");

    // another account can't take the name either (realm-wide uniqueness)
    auto other = co_await x.login(x.token("s2b"), "s2b");
    if (other) {
        std::string upper = names[2];
        for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        x.check.expect(co_await request<proto::CharCreated>(*other, proto::CharCreate{upper}) == Code::name_taken,
                       "same name from another account -> name_taken");
        other->close();
    }
    c->close();
}

// --- 3 ---
asio::awaitable<void> ownership(Ctx& x) {
    auto a = co_await x.login(x.token("s3a"), "s3a");
    auto b = co_await x.login(x.token("s3b"), "s3b");
    if (!a || !b) co_return;
    std::optional<proto::CharCreated> created;
    co_await request(*a, proto::CharCreate{random_name()}, &created);
    x.check.expect(created.has_value(), "A created a character");
    if (!created) co_return;
    const uint64_t id = created->character.id;
    x.check.expect(co_await request<proto::EnterWorldOk>(*b, proto::EnterWorld{id}) == Code::not_found,
                   "B entering A's character -> not_found");
    x.check.expect(co_await request<proto::CharDeleted>(*b, proto::CharDelete{id}) == Code::not_found,
                   "B deleting A's character -> not_found");
    std::optional<proto::CharList> list;
    co_await request(*a, proto::CharListReq{}, &list);
    x.check.expect(list && list->chars.size() == 1 && list->chars[0].id == id, "A's character is untouched");
    std::optional<proto::EnterWorldOk> ok;
    x.check.expect(co_await request(*a, proto::EnterWorld{id}, &ok) == Code::ok, "A can enter its own character");
    a->close();
    b->close();
}

// --- 4 ---
asio::awaitable<void> aoi(Ctx& x) {
    proto::EnterWorldOk ea, eb;
    auto a = co_await x.enter("s4a", ea);
    auto b = co_await x.enter("s4b", eb);
    if (!a || !b) co_return;
    auto sa = co_await wait_player_spawn(*a, Millis(3000));
    auto sb = co_await wait_player_spawn(*b, Millis(3000));
    x.check.expect(sa && sa->entityId == eb.entityId, "A sees Spawn for B");
    x.check.expect(sb && sb->entityId == ea.entityId && !sb->name.empty(), "B sees Spawn for A (" + (sb ? sb->name : "") + ")");

    auto p = co_await walk(*a, ea.pos, 20, 0);
    auto bMsgs = co_await drain(*b, Millis(500));
    auto aMsgs = co_await drain(*a, Millis(100));
    auto moves = moves_in(bMsgs);
    std::erase_if(moves, [&](const proto::EntityMoves::Entry& m) { return m.entityId != ea.entityId; }); // not the Guard's
    const bool sawA = !moves.empty() && same_pos(moves.back().pos, p);
    x.check.expect(sawA, "B received A's moves, last at " + fmt(p) + " (" + std::to_string(moves.size()) + " updates)");
    bool echo = false;
    for (auto& m : moves_in(aMsgs)) echo |= m.entityId == ea.entityId;
    x.check.expect(!echo, "A never receives its own moves");

    co_await walk(*a, p, 130, 0); // 150 m from B: past the 128 m view radius
    auto late = co_await drain(*b, Millis(500));
    bool despawned = false;
    for (auto& d : only<proto::Despawn>(late)) despawned |= d.entityId == ea.entityId;
    x.check.expect(despawned, "B gets Despawn when A walks out of range");
    a->close();
    b->close();
}

// --- 5 ---
asio::awaitable<void> chat(Ctx& x) {
    proto::EnterWorldOk ea, eb, ec;
    auto a = co_await x.enter("s5a", ea);
    auto b = co_await x.enter("s5b", eb);
    auto c = co_await x.enter("s5c", ec);
    if (!a || !b || !c) co_return;
    co_await walk(*c, ec.pos, 0, 150); // C leaves range
    co_await drain(*a, Millis(300));
    co_await drain(*b, Millis(300));
    co_await drain(*c, Millis(300));

    a->send(proto::Say{proto::ChatKind::say, "hello in range"});
    auto own = co_await a->wait_for<proto::ChatMsg>(Millis(2000));
    auto inRange = co_await b->wait_for<proto::ChatMsg>(Millis(2000));
    auto outRange = co_await c->wait_for<proto::ChatMsg>(Millis(1500));
    x.check.expect(own && own->text == "hello in range", "sender gets its own ChatMsg");
    x.check.expect(inRange && inRange->text == "hello in range" && own && inRange->fromName == own->fromName,
                   "B in range gets ChatMsg from " + (inRange ? inRange->fromName : std::string("?")));
    x.check.expect(!outRange, "C out of range gets nothing");

    a->send(proto::Say{proto::ChatKind::say, "bad\x01\x1b[31m\ttext"});
    auto clean = co_await b->wait_for<proto::ChatMsg>(Millis(2000));
    x.check.expect(clean && clean->text == "bad[31mtext", "control characters stripped");
    a->send(proto::Say{proto::ChatKind::say, std::string(257, 'x')});
    a->send(proto::Say{proto::ChatKind::say, "bad \xC3"});

    for (int i = 0; i < 6; ++i) a->send(proto::Say{proto::ChatKind::say, "spam " + std::to_string(i)});
    auto msgs = only<proto::ChatMsg>(co_await drain(*b, Millis(1500)));
    // the oversized and invalid ones still count: 4 used, so only 1 of the 6 spam messages gets through
    x.check.expect(msgs.size() == 1, "rate limit: 5 per 10 s (1 of 6 spam delivered; got " +
                                         std::to_string(msgs.size()) + ")");
    x.check.expect(!a->closed() && !b->closed(), "oversized/invalid chat is dropped, not fatal");
    a->close();
    b->close();
    c->close();
}

// --- 6 ---
asio::awaitable<void> speedhack(Ctx& x) {
    proto::EnterWorldOk ea, eb;
    auto a = co_await x.enter("s6a", ea);
    auto b = co_await x.enter("s6b", eb);
    if (!a || !b) co_return;
    co_await drain(*b, Millis(1500)); // spawns; also lets A's move budget fill
    const auto start = std::chrono::steady_clock::now();
    const proto::Vec3 origin = ea.pos;

    a->send(proto::Move{{origin.x + 100, origin.y, origin.z}, 0, 0}); // teleport
    auto corr = co_await a->wait_for<proto::Correction>(Millis(2000));
    x.check.expect(corr && same_pos(corr->pos, origin), "teleport -> Correction back to " + fmt(origin));

    proto::Vec3 p = origin;
    asio::steady_timer t(co_await asio::this_coro::executor);
    for (int i = 0; i < 30; ++i) { // 30 m/s
        t.expires_after(Millis(100));
        co_await t.async_wait(asio::use_awaitable);
        p.x += 3;
        a->send(proto::Move{p, 0, 0});
    }
    auto aMsgs = co_await drain(*a, Millis(500));
    auto bMsgs = co_await drain(*b, Millis(500));
    x.check.expect(!only<proto::Correction>(aMsgs).empty(), "sustained 30 m/s -> Correction");
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    float farthest = 0;
    for (auto& m : moves_in(bMsgs))
        if (m.entityId == ea.entityId) farthest = std::max(farthest, distance_xz(m.pos, origin));
    const float allowed = static_cast<float>(11.0 * secs + 11.0); // (10 m/s + 1 tolerance) + 1 s burst
    x.check.expect(farthest <= allowed, "observer never sees an illegal position (farthest " + std::to_string(farthest) +
                                            " m <= " + std::to_string(allowed) + " m)");
    x.check.expect(farthest < 100, "observer never sees the teleport");
    a->close();
    b->close();
}

// --- 7 ---
asio::awaitable<void> malformed(Ctx& x) {
    proto::EnterWorldOk ew;
    auto watcher = co_await x.enter("s7w", ew);
    struct Case {
        const char* what;
        std::vector<uint8_t> bytes;
        Code expect;
    };
    const std::vector<Case> cases = {
        {"malformed Move (3-byte payload)", {3, 0, 0x20, 0x00, 1, 2, 3}, Code::malformed},
        {"oversized frame (5000 bytes)", {0x88, 0x13, 0x21, 0x00}, Code::frame_too_large},
        {"unknown opcode 0x7777", {0, 0, 0x77, 0x77}, Code::unknown_opcode},
        {"server-only opcode from client", proto::encode(proto::Message{proto::AuthOk{1, proto::Role::admin}}),
         Code::unexpected_message},
    };
    int i = 0;
    for (const auto& c : cases) {
        auto bad = co_await x.login(x.token("s7m" + std::to_string(i++)), c.what);
        if (!bad) continue;
        bad->send_raw(c.bytes);
        const bool closed = co_await bad->wait_closed(Millis(5000));
        x.check.expect(closed && bad->disconnectReason == c.expect,
                       std::string(c.what) + " -> " + proto::code_name(c.expect) + " (got " +
                           (bad->disconnectReason ? proto::code_name(*bad->disconnectReason) : "none") + ")");
    }
    if (!watcher) co_return;
    std::optional<proto::CharList> unused;
    const Code still = co_await request(*watcher, proto::CharListReq{}, &unused);
    x.check.expect(!watcher->closed() && still == Code::already_in_world, "other session unaffected (still answered)");
    watcher->close();
}

// --- 8 ---
std::string mint_expired_token(const Ctx& x, std::string& err) {
    std::ifstream in(x.o.authKeyFile);
    if (!in) { err = "cannot read --auth-key " + x.o.authKeyFile; return {}; }
    std::stringstream pem;
    pem << in.rdbuf();
    AuthClient auth(x.o.auth);
    auto jwks = auth.jwks(err);
    if (!jwks) return {};
    const std::string kid = nlohmann::json::parse(*jwks)["keys"][0]["kid"];
    const auto now = std::chrono::system_clock::now();
    return jwt::create()
        .set_key_id(kid)
        .set_issuer(x.o.extra.count("issuer") ? x.o.extra.at("issuer") : "chthonic-auth")
        .set_audience(x.o.extra.count("audience") ? x.o.extra.at("audience") : "chthonic")
        .set_subject("1")
        .set_payload_claim("role", jwt::claim(std::string("player")))
        .set_issued_at(now - std::chrono::minutes(20))
        .set_expires_at(now - std::chrono::minutes(5))
        .sign(jwt::algorithm::rs256("", pem.str()));
}

asio::awaitable<void> rejected_login(Ctx& x, const std::string& what, const std::string& token, uint16_t version,
                                     Code expect) {
    auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
    if (!co_await c->connect(x.o.host, x.o.port)) { x.check.expect(false, what + ": connect"); co_return; }
    co_await c->login(token, version);
    co_await c->wait_closed(Millis(5000));
    x.check.expect(!c->authOk && c->closed() && c->disconnectReason == expect,
                   what + " -> " + proto::code_name(expect) + " (got " +
                       (c->disconnectReason ? proto::code_name(*c->disconnectReason) : "none") + ")");
}

asio::awaitable<void> handshake_rejects(Ctx& x) {
    const std::string good = x.token("s8");
    co_await rejected_login(x, "garbage token", "not-a-jwt", proto::kProtocolVersion, Code::invalid_token);
    co_await rejected_login(x, "token with tampered signature", good.substr(0, good.size() - 4) + "AAAA",
                            proto::kProtocolVersion, Code::invalid_token);
    std::string err;
    const std::string expired = mint_expired_token(x, err);
    x.check.expect(!expired.empty(), "minted an expired token with auth's key" + (err.empty() ? "" : " (" + err + ")"));
    if (!expired.empty())
        co_await rejected_login(x, "expired token", expired, proto::kProtocolVersion, Code::token_expired);
    co_await rejected_login(x, "wrong protocol version", good, 999, Code::protocol_mismatch);
    co_await rejected_login(x, "protocol version 1", good, 1, Code::protocol_mismatch);
    co_await rejected_login(x, "protocol version 4 (before Interact)", good, 4, Code::protocol_mismatch);

    {
        auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
        co_await c->connect(x.o.host, x.o.port);
        c->send(proto::Hello{});
        const auto t0 = std::chrono::steady_clock::now();
        co_await c->wait_closed(Millis(15000));
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        x.check.expect(c->disconnectReason == Code::handshake_timeout && secs > 8 && secs < 12,
                       "no Auth within 10 s -> handshake_timeout after " + std::to_string(secs) + " s");
    }
    {
        auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
        const bool connected = co_await c->connect(x.o.host, x.o.port, /*tls=*/false);
        const auto t0 = std::chrono::steady_clock::now();
        co_await c->wait_closed(Millis(15000));
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        x.check.expect(connected && c->closed() && secs > 8 && secs < 12,
                       "plain TCP without TLS is closed after " + std::to_string(secs) + " s");
    }
}

// --- 9 ---
asio::awaitable<void> duplicate_login(Ctx& x) {
    proto::EnterWorldOk e1;
    auto first = co_await x.enter("s9", e1);
    if (!first) co_return;
    const auto p = co_await walk(*first, e1.pos, 12, -7);
    co_await sleep_for(Millis(500));

    auto second = co_await x.login(x.token("s9"), "second login, same account");
    if (!second) co_return;
    co_await first->wait_closed(Millis(5000));
    x.check.expect(first->disconnectReason == Code::logged_in_elsewhere, "old session gets logged_in_elsewhere");
    proto::EnterWorldOk e2;
    std::string err;
    const bool in = co_await enter_world(*second, "unused", e2, err);
    x.check.expect(in && same_pos(e2.pos, p), "new session enters at the saved position " + fmt(p) + " (got " + fmt(e2.pos) + ")");
    second->close();
}

// --- 10 ---
asio::awaitable<void> persist_relogin(Ctx& x) {
    proto::EnterWorldOk e1;
    auto c = co_await x.enter("s10", e1);
    if (!c) co_return;
    x.check.expect(e1.map == 1, "new character enters START_MAP: EnterWorldOk.map " + std::to_string(e1.map));
    const auto p = co_await walk(*c, e1.pos, -15, 9);
    co_await sleep_for(Millis(300));
    c->close();
    co_await sleep_for(Millis(500));
    proto::EnterWorldOk e2;
    auto again = co_await x.enter("s10", e2);
    x.check.expect(again && same_pos(e2.pos, p), "position kept across logout/login: " + fmt(p) + " (got " + fmt(e2.pos) + ")");
    x.check.expect(again && e2.map == e1.map, "map kept across logout/login: " + std::to_string(e2.map));
    if (again) again->close();
}

// Graceful restart, phase 1: enter, move, then wait for the server to shut down.
asio::awaitable<void> shutdown_phase1(Ctx& x) {
    proto::EnterWorldOk e;
    auto c = co_await x.enter(x.o.extra.count("tag") ? x.o.extra.at("tag") : "s12", e);
    if (!c) co_return;
    const auto p = co_await walk(*c, e.pos, 8, 13);
    co_await sleep_for(Millis(1000)); // last move processed
    std::printf("LASTPOS %.4f %.4f %.4f\nREADY\n", p.x, p.y, p.z);
    std::fflush(stdout);
    const bool closed = co_await c->wait_closed(Millis(60000));
    x.check.expect(closed && c->disconnectReason == Code::server_shutdown, "client gets Disconnect{server_shutdown}");
}

asio::awaitable<void> restart_verify(Ctx& x) {
    proto::Vec3 want{};
    std::istringstream in(x.o.expectPos);
    char c1 = 0, c2 = 0;
    in >> want.x >> c1 >> want.y >> c2 >> want.z;
    if (!in || c1 != ',' || c2 != ',') {
        x.check.expect(false, "--expect-pos x,y,z");
        co_return;
    }
    proto::EnterWorldOk e;
    auto c = co_await x.enter(x.o.extra.count("tag") ? x.o.extra.at("tag") : "s12", e);
    x.check.expect(c && same_pos(e.pos, want), "position survived the restart: " + fmt(want) + " (got " + fmt(e.pos) + ")");
    if (c) c->close();
}

// --- 11 ---
asio::awaitable<void> db_down(Ctx& x) {
    proto::EnterWorldOk ea;
    auto a = co_await x.enter("s11a", ea);
    auto b = co_await x.login(x.token("s11b"), "s11b");
    const std::string lateToken = x.token("s11c"); // auth needs the same MySQL: get it while the DB is up
    if (!a || !b) co_return;
    std::printf("READY stop-db\n");
    std::fflush(stdout);

    // Keep A walking in a square while B polls the character list until the DB is reported down.
    asio::steady_timer t(co_await asio::this_coro::executor);
    proto::Vec3 p = ea.pos;
    int step = 0, corrections = 0;
    bool sawUnavailable = false;
    auto move_a = [&] { // a 45 m square west and south of the spawn point: clear of the sample portals (all east)
        static const float dirs[4][2] = {{-0.9f, 0}, {0, -0.9f}, {0.9f, 0}, {0, 0.9f}};
        const auto& d = dirs[(step / 50) % 4];
        p.x += d[0];
        p.z += d[1];
        a->send(proto::Move{p, 0, 0});
        ++step;
    };
    auto drain_a = [&]() -> asio::awaitable<void> {
        for (auto& m : co_await drain(*a, Millis(0))) corrections += std::holds_alternative<proto::Correction>(m);
    };
    for (int i = 0; i < 900 && !sawUnavailable; ++i) { // up to 90 s
        t.expires_after(Millis(100));
        co_await t.async_wait(asio::use_awaitable);
        move_a();
        co_await drain_a();
        if (i % 10 == 0) {
            std::optional<proto::CharList> l;
            sawUnavailable = co_await request(*b, proto::CharListReq{}, &l) == Code::service_unavailable;
        }
    }
    x.check.expect(sawUnavailable, "CharList -> service_unavailable while MySQL is down");
    x.check.expect(co_await request<proto::CharCreated>(*b, proto::CharCreate{random_name()}) == Code::service_unavailable,
                   "CharCreate -> service_unavailable while MySQL is down");
    {
        std::string err;
        auto late = co_await connect_and_login(x.o, x.tls, lateToken, err);
        x.check.expect(late != nullptr, "new auth handshake still works while MySQL is down");
        if (late) late->close();
    }
    for (int i = 0; i < 30; ++i) { // 3 more seconds in the world
        t.expires_after(Millis(100));
        co_await t.async_wait(asio::use_awaitable);
        move_a();
        co_await drain_a();
    }
    co_await sleep_for(Millis(300));
    x.check.expect(!a->closed() && corrections == 0, "in-world bot kept moving, no disconnect, no corrections");
    a->close(); // its save fails now and must be retried later
    std::printf("LASTPOS %.4f %.4f %.4f\nREADY start-db\n", p.x, p.y, p.z);
    std::fflush(stdout);

    bool back = false;
    for (int i = 0; i < 90 && !back; ++i) { // up to 3 min for MySQL to come back
        co_await sleep_for(Millis(2000));
        std::optional<proto::CharList> l;
        back = co_await request(*b, proto::CharListReq{}, &l) == Code::ok;
    }
    x.check.expect(back, "character requests work again after MySQL restarts");
    b->close();
    proto::EnterWorldOk e2;
    auto again = co_await x.enter("s11a", e2);
    x.check.expect(again && same_pos(e2.pos, p), "pending save landed: position " + fmt(p) + " (got " + fmt(e2.pos) + ")");
    if (again) again->close();
}

// --- 13: Lua components, with the sample lua/ (Health: aoi + persist, Wallet: owner + persist, systems/regen.lua) ---

std::vector<proto::ComponentUpdate> updates_for(const std::vector<proto::Message>& msgs, uint32_t entity,
                                                const std::string& name) {
    std::vector<proto::ComponentUpdate> out;
    for (const auto& u : only<proto::ComponentUpdate>(msgs))
        if (u.entityId == entity && u.name == name) out.push_back(u);
    return out;
}

nlohmann::json data_of(const proto::ComponentUpdate& u) { return nlohmann::json::parse(u.json, nullptr, false); }
double health_of(const proto::ComponentUpdate& u) { return data_of(u).value("current", -1.0); }
int gold_of(const proto::ComponentUpdate& u) { return data_of(u).value("gold", -1); }

// Index of the first message matching pred, or msgs.size().
template <typename Pred>
size_t first_index(const std::vector<proto::Message>& msgs, Pred pred) {
    for (size_t i = 0; i < msgs.size(); ++i)
        if (pred(msgs[i])) return i;
    return msgs.size();
}

// A and B in range: aoi Health reaches owner and observer, owner Wallet only the owner, Spawn before data,
// unchanged values not resent, nothing after Despawn; then A relogs and its Health and Wallet were persisted.
// Prints "COMPONENTS <gold> <health>" (A's last values) for components-verify after a restart.
asio::awaitable<void> components(Ctx& x) {
    const std::string tag = x.o.extra.count("tag") ? x.o.extra.at("tag") : "s13";
    proto::EnterWorldOk ea, eb;
    auto a = co_await x.enter(tag + "a", ea);
    auto b = co_await x.enter(tag + "b", eb);
    if (!a || !b) co_return;
    const uint32_t A = ea.entityId, B = eb.entityId;
    auto aMsgs = co_await drain(*a, Millis(1500));
    auto bMsgs = co_await drain(*b, Millis(100));

    const auto aHealth = updates_for(aMsgs, A, "Health"), aWallet = updates_for(aMsgs, A, "Wallet");
    x.check.expect(aHealth.size() == 1 && health_of(aHealth[0]) == 100.0, "A gets its own Health once (100, standing at max)");
    x.check.expect(aWallet.size() == 1 && gold_of(aWallet[0]) == 1, "A gets its own Wallet (gold 1: first login)");
    x.check.expect(updates_for(aMsgs, B, "Health").size() == 1, "A gets B's Health (aoi)");
    x.check.expect(updates_for(aMsgs, B, "Wallet").empty(), "A never gets B's Wallet (owner)");
    x.check.expect(updates_for(bMsgs, A, "Health").size() == 1, "B gets A's Health (aoi)");
    x.check.expect(updates_for(bMsgs, A, "Wallet").empty(), "B never gets A's Wallet (owner)");
    x.check.expect(updates_for(bMsgs, B, "Wallet").size() == 1, "B gets its own Wallet");
    const size_t spawnA = first_index(bMsgs, [&](const proto::Message& m) {
        auto* sp = std::get_if<proto::Spawn>(&m);
        return sp && sp->entityId == A;
    });
    const size_t dataA = first_index(bMsgs, [&](const proto::Message& m) {
        auto* u = std::get_if<proto::ComponentUpdate>(&m);
        return u && u->entityId == A;
    });
    x.check.expect(spawnA < dataA && dataA < bMsgs.size(), "B gets Spawn(A) before A's component data");

    // A walks: Health drains (sent to both); B stands at max, so nothing is resent for B.
    co_await walk(*a, ea.pos, 20, 0);
    // Over one regen.lua period (1 s, phased by entity id): the drain for the last metres walked has gone out, so A's
    // last Health seen below is settled and only regen (+) can follow it.
    co_await sleep_for(Millis(1200));
    aMsgs = co_await drain(*a, Millis(200));
    bMsgs = co_await drain(*b, Millis(100));
    const auto walked = updates_for(bMsgs, A, "Health");
    x.check.expect(!walked.empty() && health_of(walked.back()) < 95.0,
                   "B sees A's Health drain while A moves (" + std::to_string(walked.size()) + " updates, last " +
                       (walked.empty() ? std::string("-") : std::to_string(health_of(walked.back()))) + ")");
    x.check.expect(updates_for(bMsgs, B, "Health").empty() && updates_for(aMsgs, B, "Health").empty(),
                   "B's unchanged Health is not resent");
    const auto ownSeen = updates_for(aMsgs, A, "Health");
    x.check.expect(!ownSeen.empty(), "A gets its own Health changes");
    const double before = ownSeen.empty() ? -1 : health_of(ownSeen.back());

    a->close();
    auto afterLeave = co_await drain(*b, Millis(1000));
    const size_t despawnA = first_index(afterLeave, [&](const proto::Message& m) {
        auto* d = std::get_if<proto::Despawn>(&m);
        return d && d->entityId == A;
    });
    bool lateData = false;
    for (size_t i = despawnA; i < afterLeave.size(); ++i)
        if (auto* u = std::get_if<proto::ComponentUpdate>(&afterLeave[i])) lateData |= u->entityId == A;
    x.check.expect(despawnA < afterLeave.size() && !lateData, "B gets Despawn(A) and no A component data after it");

    proto::EnterWorldOk e2;
    auto again = co_await x.enter(tag + "a", e2);
    if (!again) co_return;
    auto relog = co_await drain(*again, Millis(1000));
    const auto health = updates_for(relog, e2.entityId, "Health"), wallet = updates_for(relog, e2.entityId, "Wallet");
    const double first = health.empty() ? -1 : health_of(health.front());
    // regen.lua's once-a-second +2 may land between the last update A saw and the save, and again on the first
    // tick after the relog.
    x.check.expect(first >= before - 0.01 && first <= before + 4.01,
                   "Health persisted across logout/login: " + std::to_string(before) + " -> " + std::to_string(first));
    x.check.expect(wallet.size() == 1 && gold_of(wallet[0]) == 2, "Wallet persisted across logout/login (gold 2)");
    std::printf("COMPONENTS %d %.4f\n", wallet.empty() ? -1 : gold_of(wallet.back()),
                health.empty() ? -1.0 : health_of(health.back()));
    std::fflush(stdout);
    again->close();

    // Server-authoritative: a client's ComponentUpdate is not a valid message.
    b->send(proto::ComponentUpdate{B, "Health", R"({"current":100})"});
    const bool dropped = co_await b->wait_closed(Millis(3000));
    x.check.expect(dropped && b->disconnectReason == Code::unexpected_message,
                   "a client's ComponentUpdate gets Disconnect{unexpected_message}");
}

// After a world restart: A's Wallet and Health (from --expect-components gold,health) were persisted.
asio::awaitable<void> components_verify(Ctx& x) {
    const std::string tag = x.o.extra.count("tag") ? x.o.extra.at("tag") : "s13";
    int gold = 0;
    double health = 0;
    char comma = 0;
    std::istringstream in(x.o.extra.count("expect-components") ? x.o.extra.at("expect-components") : "");
    in >> gold >> comma >> health;
    if (!in || comma != ',') {
        x.check.expect(false, "--expect-components gold,health");
        co_return;
    }
    proto::EnterWorldOk e;
    auto c = co_await x.enter(tag + "a", e);
    if (!c) co_return;
    auto msgs = co_await drain(*c, Millis(1000));
    const auto h = updates_for(msgs, e.entityId, "Health"), w = updates_for(msgs, e.entityId, "Wallet");
    const double got = h.empty() ? -1 : health_of(h.front());
    x.check.expect(got >= health - 0.01 && got <= health + 4.01,
                   "Health survived the restart: " + std::to_string(health) + " -> " + std::to_string(got));
    x.check.expect(w.size() == 1 && gold_of(w[0]) == gold + 1,
                   "Wallet survived the restart (gold " + std::to_string(gold) + " + 1 for this login)");
    c->close();
}

// --- 14: server events, with the sample lua/systems/events.lua ---

// The ServerEvents named name whose payload's "entity" is entity.
std::vector<nlohmann::json> events_about(const std::vector<proto::Message>& msgs, const std::string& name, uint32_t entity) {
    std::vector<nlohmann::json> out;
    for (const auto& e : only<proto::ServerEvent>(msgs)) {
        auto data = nlohmann::json::parse(e.json, nullptr, false);
        if (e.name == name && data.is_object() && data.value("entity", 0u) == entity) out.push_back(std::move(data));
    }
    return out;
}

size_t count_named(const std::vector<proto::Message>& msgs, const std::string& name) {
    size_t n = 0;
    for (const auto& e : only<proto::ServerEvent>(msgs)) n += e.name == name;
    return n;
}

// A walks out of view; B and C stand at the spawn point. When C logs in: only C gets "welcome" (sendTo); C and B,
// not A, get "player.arrived" (sendNearby, after Spawn(C)). When C logs out: A and B get "player.left" (broadcast),
// B before Despawn(C).
asio::awaitable<void> events(Ctx& x) {
    proto::EnterWorldOk ea, eb, ec;
    auto a = co_await x.enter("s14a", ea);
    if (!a) co_return;
    auto own = co_await drain(*a, Millis(500));
    x.check.expect(count_named(own, "welcome") == 1, "A gets its own welcome");
    co_await walk(*a, ea.pos, 150, 0); // past the 128 m view radius of the spawn point
    auto b = co_await x.enter("s14b", eb);
    if (!b) co_return;
    co_await drain(*b, Millis(500));
    co_await drain(*a, Millis(100));
    auto c = co_await x.enter("s14c", ec);
    if (!c) co_return;
    const uint32_t C = ec.entityId;
    auto cMsgs = co_await drain(*c, Millis(1000));
    auto bMsgs = co_await drain(*b, Millis(100));
    auto aMsgs = co_await drain(*a, Millis(100));

    x.check.expect(count_named(cMsgs, "welcome") == 1 && count_named(bMsgs, "welcome") == 0 &&
                       count_named(aMsgs, "welcome") == 0,
                   "sendTo: only C gets its welcome");
    x.check.expect(events_about(cMsgs, "player.arrived", C).size() == 1, "sendNearby: C (the owner) gets player.arrived");
    const auto arrived = events_about(bMsgs, "player.arrived", C);
    x.check.expect(arrived.size() == 1 && !arrived[0].value("name", "").empty(), "sendNearby: B (in view) gets player.arrived for C");
    x.check.expect(events_about(aMsgs, "player.arrived", C).empty(), "sendNearby: A (out of view) gets nothing");
    const size_t spawnC = first_index(bMsgs, [&](const proto::Message& m) {
        auto* sp = std::get_if<proto::Spawn>(&m);
        return sp && sp->entityId == C;
    });
    const size_t arrivedC = first_index(bMsgs, [&](const proto::Message& m) {
        auto* e = std::get_if<proto::ServerEvent>(&m);
        return e && e->name == "player.arrived";
    });
    x.check.expect(spawnC < arrivedC && arrivedC < bMsgs.size(), "B gets Spawn(C) before player.arrived");

    c->close();
    bMsgs = co_await drain(*b, Millis(1000));
    aMsgs = co_await drain(*a, Millis(100));
    x.check.expect(events_about(aMsgs, "player.left", C).size() == 1, "broadcast: A (out of view) gets player.left");
    x.check.expect(events_about(bMsgs, "player.left", C).size() == 1, "broadcast: B gets player.left");
    const size_t leftC = first_index(bMsgs, [&](const proto::Message& m) {
        auto* e = std::get_if<proto::ServerEvent>(&m);
        return e && e->name == "player.left";
    });
    const size_t despawnC = first_index(bMsgs, [&](const proto::Message& m) {
        auto* d = std::get_if<proto::Despawn>(&m);
        return d && d->entityId == C;
    });
    x.check.expect(leftC < despawnC && despawnC < bMsgs.size(), "B gets player.left before Despawn(C)");

    // Server-to-client only: a client's ServerEvent is not a valid message.
    b->send(proto::ServerEvent{"cheat", "{}"});
    const bool dropped = co_await b->wait_closed(Millis(3000));
    x.check.expect(dropped && b->disconnectReason == Code::unexpected_message,
                   "a client's ServerEvent gets Disconnect{unexpected_message}");
    a->close();
}

// --- 15: state machines, with the sample lua/fsms/locomotion.lua and lua/systems/locomotion.lua ---

// The locomotion machine's "fsm.state" events as "from->to" ("nil" on the first entry), in arrival order.
std::vector<std::string> transitions(const std::vector<proto::Message>& msgs) {
    std::vector<std::string> out;
    for (const auto& e : only<proto::ServerEvent>(msgs)) {
        auto data = nlohmann::json::parse(e.json, nullptr, false);
        if (e.name != "fsm.state" || !data.is_object() || data.value("machine", "") != "locomotion") continue;
        out.push_back((data.contains("from") ? data["from"].get<std::string>() : "nil") + "->" + data.value("to", ""));
    }
    return out;
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& i : items) out += (out.empty() ? "" : ", ") + i;
    return "[" + out + "]";
}

// A enters: only nil->idle (onEnter's idle->idle is denied and sends nothing). A walks: idle->moving, then, after
// standing still for a second, moving->idle. B, in view all along, gets only its own nil->idle (sendTo is owner-only).
asio::awaitable<void> fsm(Ctx& x) {
    proto::EnterWorldOk ea, eb;
    auto b = co_await x.enter("s15b", eb);
    if (!b) co_return;
    auto a = co_await x.enter("s15a", ea);
    if (!a) co_return;
    const auto entered = transitions(co_await drain(*a, Millis(1000)));
    x.check.expect(entered == std::vector<std::string>{"nil->idle"},
                   "entering: nil->idle only, the denied idle->idle sends nothing (got " + joined(entered) + ")");
    co_await walk(*a, ea.pos, 10, 0);
    const auto walked = transitions(co_await drain(*a, Millis(2000)));
    x.check.expect(walked == std::vector<std::string>{"idle->moving", "moving->idle"},
                   "walking then stopping: idle->moving, moving->idle (got " + joined(walked) + ")");
    const auto seen = transitions(co_await drain(*b, Millis(100)));
    x.check.expect(seen == std::vector<std::string>{"nil->idle"}, "B gets only its own transition (got " + joined(seen) + ")");
    a->close();
    b->close();
}

// --- 16 ---
// Needs the sample lua/systems/npcs.lua and lua/npcs/guard.lua: the Guard patrols around the spawn point, where a new
// character enters. A gets Spawn{kind npc, template "Guard"}, then its Health from the definition (current 50), then
// EntityMoves at changing positions; walking 200 m away gives Despawn and nothing more about it; walking back gives
// Spawn and the Health again.
asio::awaitable<void> npcs(Ctx& x) {
    proto::EnterWorldOk ea;
    auto a = co_await x.enter("s16a", ea);
    if (!a) co_return;
    std::optional<proto::Spawn> guard;
    while (!guard) {
        auto s = co_await a->wait_for<proto::Spawn>(Millis(3000));
        if (!s) break;
        if (s->kind == proto::SpawnKind::npc && s->name == "Guard") guard = s;
    }
    x.check.expect(guard && guard->templateName == "Guard", "A gets Spawn{kind npc, name Guard, template Guard}");
    if (!guard) co_return;
    const uint32_t G = guard->entityId;
    const auto aboutGuard = [&](const std::vector<proto::Message>& msgs) {
        std::vector<proto::Message> out;
        for (const auto& m : msgs)
            std::visit([&](const auto& v) {
                if constexpr (requires { v.entityId; }) {
                    if (v.entityId == G) out.push_back(m);
                } else if constexpr (std::is_same_v<std::decay_t<decltype(v)>, proto::EntityMoves>) {
                    for (const auto& e : v.entries) // its entries for the Guard, each on its own
                        if (e.entityId == G) out.push_back(proto::EntityMoves{{e}});
                }
            }, m);
        return out;
    };

    // Its Health (aoi) follows the Spawn in the same tick: the AOI-enter full set, with the definition's data.
    const auto fromDefinition = [](const std::vector<proto::ComponentUpdate>& health) {
        return !health.empty() && health_of(health.front()) == 50 && data_of(health.front()).value("max", -1.0) == 50;
    };
    // The Guard walks for ~7 s between waypoints and waits 2 s at each: 4 s always hold some walking.
    const auto first = aboutGuard(co_await drain(*a, Millis(4000)));
    const auto health = updates_for(first, G, "Health");
    x.check.expect(fromDefinition(health),
                   "A gets the Guard's Health from its definition (current 50, max 50)" +
                       (health.empty() ? std::string(", got none") : ", got " + health.front().json));
    const auto moves = moves_in(first);
    std::set<std::pair<float, float>> positions;
    for (const auto& m : moves) positions.insert({m.pos.x, m.pos.z});
    x.check.expect(moves.size() >= 3 && positions.size() >= 3,
                   "A gets the Guard's EntityMoves at changing positions (" + std::to_string(moves.size()) + " moves, " +
                       std::to_string(positions.size()) + " positions)");

    // West: east of the spawn point lie the sample portals. ~22 s at 9 m/s; the Guard stays within ~15 m of the spawn point.
    auto p = co_await walk(*a, ea.pos, -200, 0);
    auto away = aboutGuard(co_await drain(*a, Millis(1000)));
    const auto despawn = std::ranges::find_if(away, [](const auto& m) { return std::holds_alternative<proto::Despawn>(m); });
    x.check.expect(despawn != away.end(), "walking 200 m away: Despawn for the Guard");
    x.check.expect(despawn != away.end() && despawn + 1 == away.end(), "nothing about the Guard after its Despawn");
    const auto later = aboutGuard(co_await drain(*a, Millis(2000)));
    x.check.expect(later.empty(), "still nothing about the Guard while out of range (" + std::to_string(later.size()) + ")");

    co_await walk(*a, p, 200, 0);
    std::optional<proto::Spawn> again;
    const auto back = co_await drain(*a, Millis(1500));
    for (const auto& m : back)
        if (auto* s = std::get_if<proto::Spawn>(&m); s && s->kind == proto::SpawnKind::npc && s->name == "Guard") again = *s;
    x.check.expect(again && again->templateName == "Guard", "walking back: Spawn for the Guard again");
    x.check.expect(fromDefinition(updates_for(back, G, "Health")), "walking back: its Health again");
    a->close();
}

// --- 17: areas with the sample maps (docs/world/areas.html): start's "cave_mouth" portal sends A to map 2 (the cave) ---
asio::awaitable<void> portal(Ctx& x) {
    proto::EnterWorldOk ea, eb;
    auto a = co_await x.enter("s17a", ea);
    auto b = co_await x.enter("s17b", eb);
    if (!a || !b) co_return;
    x.check.expect(ea.map == 1 && eb.map == 1, "A and B enter map 1");
    auto sa = co_await wait_player_spawn(*a, Millis(3000));
    x.check.expect(sa && sa->entityId == eb.entityId, "A sees B on map 1");
    co_await drain(*a, Millis(300));

    // From the spawn point (1024, 1024) to (1042, 999): into cave_mouth [1040..1050] x [990..1000] at the very end.
    co_await walk(*a, ea.pos, 1042 - ea.pos.x, 999 - ea.pos.z);
    const auto msgs = co_await drain(*a, Millis(1500));
    const size_t change = first_index(msgs, [](const auto& m) { return std::holds_alternative<proto::MapChange>(m); });
    const size_t despawn = first_index(msgs, [](const auto& m) { return std::holds_alternative<proto::Despawn>(m); });
    const size_t spawn = first_index(msgs, [](const auto& m) { return std::holds_alternative<proto::Spawn>(m); });
    const auto* mc = change < msgs.size() ? &std::get<proto::MapChange>(msgs[change]) : nullptr;
    x.check.expect(mc && mc->map == 2 && same_pos(mc->pos, {256, 0, 240}),
                   "walking into the portal: MapChange{2} at (256, 0, 240)" + (mc ? " (got map " + std::to_string(mc->map) + " " + fmt(mc->pos) + ")" : ""));
    x.check.expect(mc && change < despawn && change < spawn, "MapChange arrives before any Despawn/Spawn");
    bool lostB = false, sawBat = false;
    for (const auto& d : only<proto::Despawn>(msgs)) lostB |= d.entityId == eb.entityId;
    for (const auto& s : only<proto::Spawn>(msgs)) sawBat |= s.kind == proto::SpawnKind::npc && s.name == "Bat";
    x.check.expect(lostB, "A loses sight of B (Despawn) on leaving map 1");
    x.check.expect(sawBat, "A sees the Bat on map 2");
    bool bLostA = false;
    for (const auto& d : only<proto::Despawn>(co_await drain(*b, Millis(200)))) bLostA |= d.entityId == ea.entityId;
    x.check.expect(bLostA, "B gets A's Despawn");

    a->close();
    co_await sleep_for(Millis(500));
    proto::EnterWorldOk again;
    auto a2 = co_await x.enter("s17a", again);
    x.check.expect(a2 && again.map == 2 && same_pos(again.pos, {256, 0, 240}),
                   "relog keeps A on map 2 at " + fmt(again.pos) + " (map " + std::to_string(again.map) + ")");
    if (a2) a2->close();
    b->close();
}

// --- 18: NPC spawns (docs/world/npcs.html#npc-spawns) ---
// Needs the sample lua/systems/test_hooks.lua and the Guard's spawn file (maps/start/.server/spawns/4_4.json,
// respawnSec 5). A at the spawn point sees the Guard; B enters as "Respawntest..." and the hook destroys it; A gets its
// Despawn, then about 5 s later the Spawn of a new Guard at its spawn point.
asio::awaitable<void> respawn(Ctx& x) {
    proto::EnterWorldOk ea;
    auto a = co_await x.enter("s18a", ea);
    if (!a) co_return;
    std::optional<proto::Spawn> guard;
    while (!guard) {
        auto s = co_await a->wait_for<proto::Spawn>(Millis(8000)); // 8 s: it may still be respawning from a last run
        if (!s) break;
        if (s->kind == proto::SpawnKind::npc && s->name == "Guard") guard = s;
    }
    x.check.expect(guard.has_value(), "A sees the Guard");
    if (!guard) co_return;

    auto b = co_await x.login(x.token("s18b"), "s18b");
    if (!b) co_return;
    proto::EnterWorldOk eb;
    std::string err;
    const bool in = co_await enter_world(*b, "Respawntest" + random_name(5), eb, err);
    x.check.expect(in, "B enters as Respawntest... (" + err + ")");
    if (!in) co_return;

    std::optional<proto::Despawn> gone;
    while (!gone) {
        auto d = co_await a->wait_for<proto::Despawn>(Millis(3000));
        if (!d) break;
        if (d->entityId == guard->entityId) gone = d;
    }
    const auto killed = std::chrono::steady_clock::now();
    x.check.expect(gone.has_value(), "A gets the Guard's Despawn when the test hook destroys it");
    if (!gone) co_return;

    std::optional<proto::Spawn> again;
    while (!again) {
        auto s = co_await a->wait_for<proto::Spawn>(Millis(10000));
        if (!s) break;
        if (s->kind == proto::SpawnKind::npc && s->name == "Guard") again = s;
    }
    const double after = std::chrono::duration<double>(std::chrono::steady_clock::now() - killed).count();
    x.check.expect(again.has_value(), "A gets a Spawn for the respawned Guard");
    if (!again) co_return;
    x.check.expect(after >= 4.5 && after <= 6.5, "it is back respawnSec (5 s) later (" + std::to_string(after) + " s)");
    x.check.expect(again->entityId != guard->entityId && again->templateName == "Guard",
                   "as a new entity with template Guard");
    x.check.expect(std::hypot(again->pos.x - 1024, again->pos.z - 1024) < 1,
                   "at its spawn point (1024, 1024): " + fmt(again->pos));
    a->close();
    b->close();
}

// --- 19: ground on the sample "hills" map (docs/world/maps.html#ground) ---
// A and B take start's "hills_gate" portal to map 3 and land on the ground; A walks up the slope at ground height (no
// Correction) and B sees its EntityMoves along the ground; then A flies up and is snapped back with Correction.
// The walk stays under hills' 40 degree nav.maxSlopeDeg: steeper is off the navmesh (scenario 20).
asio::awaitable<void> terrain(Ctx& x) {
    std::string err;
    const auto hills = load_terrain(std::filesystem::path(x.o.mapRoot) / "hills", err);
    x.check.expect(hills.has_value(), "maps/hills loaded from --map-root '" + x.o.mapRoot + "' " + err);
    if (!hills) co_return;
    const auto groundY = [&](float px, float pz) { return hills->ground(px, pz); };

    proto::EnterWorldOk ea, eb;
    auto a = co_await x.enter("s19a", ea);
    auto b = co_await x.enter("s19b", eb);
    if (!a || !b) co_return;
    // From the spawn point (1024, 1024) into hills_gate [1060..1070] x [1020..1030].
    std::optional<proto::MapChange> mcA, mcB;
    co_await walk(*a, ea.pos, 1065 - ea.pos.x, 1025 - ea.pos.z);
    if (auto m = only<proto::MapChange>(co_await drain(*a, Millis(1000))); !m.empty()) mcA = m[0];
    co_await walk(*b, eb.pos, 1065 - eb.pos.x, 1025 - eb.pos.z);
    if (auto m = only<proto::MapChange>(co_await drain(*b, Millis(1000))); !m.empty()) mcB = m[0];
    const float gate = hills->ground(256, 256);
    x.check.expect(mcA && mcA->map == 3 && std::fabs(mcA->pos.y - gate) < 0.01f,
                   "A: MapChange{3} on the ground at the portal (ground " + std::to_string(gate) + ", got " +
                       (mcA ? fmt(mcA->pos) : "none") + ")");
    x.check.expect(mcB && mcB->map == 3, "B: MapChange{3}");
    if (!mcA || !mcB) co_return;
    co_await drain(*b, Millis(500)); // B's Spawn of A

    // 40 m down the slope at the ground's height (9 m down; at most 36.5 degrees, so on the navmesh: hills'
    // nav.maxSlopeDeg is 40, and up toward (286, 281) the hill reaches 43).
    const proto::Vec3 top = co_await walk(*a, mcA->pos, -30, 25, 9.0f, groundY);
    const auto aMsgs = co_await drain(*a, Millis(500));
    x.check.expect(only<proto::Correction>(aMsgs).empty(), "walking the slope at ground height: no Correction");
    const auto seen = moves_in(co_await drain(*b, Millis(300)));
    size_t moves = 0, onGround = 0;
    for (const auto& m : seen)
        if (m.entityId == ea.entityId) { ++moves; onGround += std::fabs(m.pos.y - hills->ground(m.pos.x, m.pos.z)) < 0.05f; }
    x.check.expect(moves > 10 && onGround == moves, "B sees A's EntityMoves along the ground (" + std::to_string(onGround) +
                                                         "/" + std::to_string(moves) + " within 5 cm)");
    x.check.expect(std::fabs(top.y - mcA->pos.y) > 1, "the walk climbed or dropped > 1 m: " + fmt(mcA->pos) + " -> " + fmt(top));

    // Fly hack: rising to 20 m and 30 m above the ground.
    a->send(proto::Move{{top.x + 1, hills->ground(top.x + 1, top.z) + 20, top.z}, 0, 90000});
    a->send(proto::Move{{top.x + 2, hills->ground(top.x + 2, top.z) + 30, top.z}, 0, 90100});
    const auto corr = co_await a->wait_for<proto::Correction>(Millis(2000));
    x.check.expect(corr && same_pos(corr->pos, top), "fly hack -> Correction back to " + fmt(top) +
                                                         (corr ? " (got " + fmt(corr->pos) + ")" : " (none)"));
    a->close();
    b->close();
}

// --- 20: navmesh on the sample "hills" map (docs/world/maps.html#navmesh) ---
// hills has a mesa (x 170..200, z 316..346, ~81 degree walls) and the Sentry patrolling from (150, 331) to (215, 331),
// straight through it (maps/hills/.server/spawns/0_1.json). A takes hills_gate to map 3 and walks at ground height to
// the mesa's south foot (no Correction), then into its wall: Correction. Over 30 s (more than one leg) the Sentry's
// EntityMoves pass the mesa's x range but stay on the ground and off its walls: it walks the navmesh path around.
asio::awaitable<void> navmesh(Ctx& x) {
    std::string err;
    const auto hills = load_terrain(std::filesystem::path(x.o.mapRoot) / "hills", err);
    x.check.expect(hills.has_value(), "maps/hills loaded from --map-root '" + x.o.mapRoot + "' " + err);
    if (!hills) co_return;
    const auto groundY = [&](float px, float pz) { return hills->ground(px, pz); };

    proto::EnterWorldOk ea;
    auto a = co_await x.enter("s20a", ea);
    if (!a) co_return;
    co_await walk(*a, ea.pos, 1065 - ea.pos.x, 1025 - ea.pos.z);
    auto msgs = co_await drain(*a, Millis(1000));
    const auto mc = only<proto::MapChange>(msgs);
    x.check.expect(!mc.empty() && mc[0].map == 3, "A: MapChange{3}");
    if (mc.empty()) co_return;

    const proto::Vec3 foot = co_await walk(*a, mc[0].pos, 185 - mc[0].pos.x, 310 - mc[0].pos.z, 9.0f, groundY);
    auto toFoot = co_await drain(*a, Millis(500));
    x.check.expect(only<proto::Correction>(toFoot).empty(),
                   "walking to the mesa's south foot " + fmt(foot) + " at ground height: no Correction");
    msgs.insert(msgs.end(), toFoot.begin(), toFoot.end());

    co_await walk(*a, foot, 0, 12, 4.0f, groundY); // up the wall (z 316..319) at ground height
    auto wall = co_await drain(*a, Millis(1000));
    const auto corr = only<proto::Correction>(wall);
    x.check.expect(!corr.empty() && corr.back().pos.z < 317,
                   "walking into the mesa's wall -> Correction at its foot" +
                       (corr.empty() ? std::string(" (none)") : " (got " + fmt(corr.back().pos) + ")"));
    msgs.insert(msgs.end(), wall.begin(), wall.end());

    const auto more = co_await drain(*a, Millis(30000));
    msgs.insert(msgs.end(), more.begin(), more.end());
    std::optional<uint32_t> sentry;
    for (const auto& s : only<proto::Spawn>(msgs))
        if (s.kind == proto::SpawnKind::npc && s.name == "Sentry") sentry = s.entityId;
    x.check.expect(sentry.has_value(), "A gets Spawn{npc, Sentry}");
    if (!sentry) co_return;
    size_t moves = 0, onGround = 0, pastMesa = 0;
    float intoWall = 0; // deepest move into the mesa's footprint: its 3 m walls start at the edge
    for (const auto& m : moves_in(msgs)) {
        if (m.entityId != *sentry) continue;
        ++moves;
        onGround += std::fabs(m.pos.y - hills->ground(m.pos.x, m.pos.z)) < 0.05f;
        intoWall = std::max(intoWall, std::min({m.pos.x - 170, 200 - m.pos.x, m.pos.z - 316, 346 - m.pos.z}));
        pastMesa += m.pos.x > 180 && m.pos.x < 190;
    }
    x.check.expect(moves > 100 && onGround == moves, "the Sentry's EntityMoves are on the ground (" +
                                                         std::to_string(onGround) + "/" + std::to_string(moves) + ")");
    // The navmesh's edges may sit up to ~0.5 m off the eroded raster (Recast's contour simplification): a corner can
    // graze the wall's foot, never more.
    char into[48];
    std::snprintf(into, sizeof into, "%.2f m", intoWall);
    x.check.expect(pastMesa > 0 && intoWall < 0.5f, "it passes the mesa (" + std::to_string(pastMesa) +
                                                       " moves at x 180..190) without climbing it (at most " + into +
                                                       " into its wall's foot)");
    a->close();
}

// --- 21, 22: client events (chthonic.world.send's ClientEvent), with the sample lua/systems/test_hooks.lua ---

// The "test.echo" replies' n, in arrival order.
std::vector<int> echoes(const std::vector<proto::Message>& msgs, uint32_t entity) {
    std::vector<int> out;
    for (const auto& e : only<proto::ServerEvent>(msgs)) {
        auto data = nlohmann::json::parse(e.json, nullptr, false);
        if (e.name == "test.echo" && data.is_object() && data.value("entity", 0u) == entity) out.push_back(data.value("n", -2));
    }
    return out;
}

// --- 25: zone objects on the sample "hills" map (docs/world/maps.html#objects) ---
// The fixtures (maps/hills/objects, .server/interactables): interactable 1 "Test Shrine" (a 0.6 m cylinder at
// (250, 266), range 3, event "test.shrine" {msg = "hello"}) and an obstacle crate (a 2 m box at (266, 248)). The test
// hook answers "test.shrine" with a ServerEvent carrying what it got. Interact on another map, out of range or with an
// unknown id gets nothing; in range, one answer; walking into the crate, a Correction at its side (the navmesh cut).
asio::awaitable<void> objects(Ctx& x) {
    std::string err;
    const auto hills = load_terrain(std::filesystem::path(x.o.mapRoot) / "hills", err);
    x.check.expect(hills.has_value(), "maps/hills loaded from --map-root '" + x.o.mapRoot + "' " + err);
    if (!hills) co_return;
    const auto groundY = [&](float px, float pz) { return hills->ground(px, pz); };
    const auto answers = [](const std::vector<proto::Message>& msgs) {
        std::vector<nlohmann::json> out;
        for (const auto& e : only<proto::ServerEvent>(msgs))
            if (e.name == "test.shrine") out.push_back(nlohmann::json::parse(e.json, nullptr, false));
        return out;
    };

    proto::EnterWorldOk ea;
    auto a = co_await x.enter("s25", ea);
    if (!a) co_return;
    co_await drain(*a, Millis(300));
    a->send(proto::Interact{1}); // map 1 has no interactable 1
    x.check.expect(answers(co_await drain(*a, Millis(1000))).empty(), "Interact{1} on map 1 (it is map 3's): no event");

    co_await walk(*a, ea.pos, 1065 - ea.pos.x, 1025 - ea.pos.z);
    const auto mc = only<proto::MapChange>(co_await drain(*a, Millis(1000)));
    x.check.expect(!mc.empty() && mc[0].map == 3, "A: MapChange{3}");
    if (mc.empty()) co_return;
    const float away = std::hypot(mc[0].pos.x - 250, mc[0].pos.z - 266) - 0.6f;
    a->send(proto::Interact{1});
    a->send(proto::Interact{999});
    x.check.expect(answers(co_await drain(*a, Millis(1000))).empty(),
                   "Interact{1} " + std::to_string(int(away)) + " m from the shrine's edge (range 3) and Interact{999}: no event");

    const proto::Vec3 atShrine = co_await walk(*a, mc[0].pos, 250 - mc[0].pos.x, 263.5f - mc[0].pos.z, 6.0f, groundY);
    a->send(proto::Interact{1});
    const auto msgs = co_await drain(*a, Millis(1500));
    const auto got = answers(msgs);
    x.check.expect(only<proto::Correction>(msgs).empty(), "walked to " + fmt(atShrine) + ", 1.9 m from the shrine's edge: no Correction");
    x.check.expect(got.size() == 1 && got[0].value("object", 0) == 1 && got[0].value("map", 0) == 3 &&
                       got[0].value("msg", "") == "hello" && got[0].value("entity", 0u) == ea.entityId,
                   "Interact{1} in range: test.shrine {entity, map 3, object 1, data.msg hello} reached Lua (" +
                       (got.empty() ? std::string("no answer") : got[0].dump()) + ")");

    const proto::Vec3 beside = co_await walk(*a, atShrine, 261 - atShrine.x, 248 - atShrine.z, 6.0f, groundY);
    auto toCrate = co_await drain(*a, Millis(500));
    x.check.expect(only<proto::Correction>(toCrate).empty(), "walking to " + fmt(beside) + " beside the crate: no Correction");
    co_await walk(*a, beside, 5, 0, 4.0f, groundY); // through the crate's x 265..267
    const auto corr = only<proto::Correction>(co_await drain(*a, Millis(1000)));
    x.check.expect(!corr.empty() && corr.back().pos.x < 265,
                   "walking into the obstacle crate -> Correction outside it" +
                       (corr.empty() ? std::string(" (none)") : " (got " + fmt(corr.back().pos) + ")"));
    a->close();
}

proto::ClientEvent echo(int n) { return {"test.echo", nlohmann::json{{"n", n}}.dump()}; }

// A ClientEvent reaches the server's Lua as "client.test.echo" {entity = sender, data}: the hook's reply carries both.
// Then each kind of invalid ClientEvent closes its connection with Disconnect{malformed}: a bad name, a payload over
// 1 KB, JSON that is not an object.
asio::awaitable<void> client_events(Ctx& x) {
    proto::EnterWorldOk ok;
    auto a = co_await x.enter("s21a", ok);
    if (!a) co_return;
    co_await drain(*a, Millis(300));
    a->send(echo(7));
    const auto got = echoes(co_await drain(*a, Millis(1000)), ok.entityId);
    x.check.expect(got == std::vector<int>{7}, "client.test.echo {entity = sender, data = {n = 7}} -> test.echo n 7 (" +
                                                   std::to_string(got.size()) + " replies)");
    a->close();

    const std::string over = nlohmann::json{{"s", std::string(proto::kClientEventJsonMax, 'a')}}.dump();
    const std::vector<std::pair<std::string, proto::ClientEvent>> bad = {
        {"name with capitals", {"Test.Echo", "{}"}},
        {"33-char name", {std::string(33, 'a'), "{}"}},
        {"payload over 1 KB", {"test.echo", over}},
        {"JSON array", {"test.echo", "[1]"}},
        {"not JSON", {"test.echo", "{nope"}},
    };
    int i = 0;
    for (const auto& [what, event] : bad) {
        proto::EnterWorldOk eb;
        auto b = co_await x.enter("s21b" + std::to_string(i++), eb);
        if (!b) co_return;
        b->send(event);
        const bool closed = co_await b->wait_closed(Millis(3000));
        x.check.expect(closed && b->disconnectReason == Code::malformed, what + " -> Disconnect{malformed}");
    }
}

// 30 ClientEvents at once: the first 10 reach Lua (10 a second per session), the other 20 are dropped (the server logs
// the count when the character leaves; tests/integration.sh checks it). A second later events get through again.
asio::awaitable<void> client_event_rate(Ctx& x) {
    proto::EnterWorldOk ok;
    auto a = co_await x.enter("s22", ok);
    if (!a) co_return;
    co_await drain(*a, Millis(300));
    for (int n = 1; n <= 30; ++n) a->send(echo(n));
    const auto got = echoes(co_await drain(*a, Millis(800)), ok.entityId);
    std::vector<int> first10(10);
    for (int n = 1; n <= 10; ++n) first10[n - 1] = n;
    x.check.expect(got == first10, "30 at once: only the first 10 reach Lua (" + std::to_string(got.size()) + " replies)");
    co_await drain(*a, Millis(400)); // past the 1 s window
    a->send(echo(99));
    const auto later = echoes(co_await drain(*a, Millis(1000)), ok.entityId);
    x.check.expect(later == std::vector<int>{99}, "a second later events reach Lua again");
    a->close();
}

// --- 26 / 27: client prediction (run with the server's lag simulator on, SIM_LATENCY_MS / SIM_JITTER_MS) ---

// A run at 7 m/s (under MAX_SPEED) round a 5 m circle that starts at `from`, turning all the way: where it is after
// `seconds`, and its heading (the direction of travel).
std::pair<proto::Vec3, float> circle(const proto::Vec3& from, float seconds) {
    constexpr float speed = 7, radius = 5;
    const float a = speed * seconds / radius;
    return {{from.x - radius + radius * std::cos(a), from.y, from.z + radius * std::sin(a)}, std::atan2(-std::sin(a), std::cos(a))};
}

// The round trip of a Ping through the lag simulator, printed as evidence it is on.
asio::awaitable<void> print_round_trip(WorldClient& c) {
    const auto start = std::chrono::steady_clock::now();
    c.send(proto::Ping{77});
    const auto pong = co_await c.wait_for<proto::Pong>(Millis(5000));
    const auto ms = std::chrono::duration_cast<Millis>(std::chrono::steady_clock::now() - start).count();
    std::printf("  round trip %s\n", pong ? (std::to_string(ms) + " ms").c_str() : "unknown (no Pong)");
}

// 10 s at 7 m/s round the circle, one Move every 100 ms like a game client: no Correction at all.
asio::awaitable<void> lag_run(Ctx& x) {
    proto::EnterWorldOk ea;
    auto a = co_await x.enter("s26a", ea);
    if (!a) co_return;
    co_await drain(*a, Millis(1500)); // spawns; the move budget fills
    co_await print_round_trip(*a);
    asio::steady_timer t(co_await asio::this_coro::executor);
    auto next = std::chrono::steady_clock::now();
    size_t corrections = 0;
    for (int i = 1; i <= 100; ++i) {
        next += Millis(100);
        t.expires_at(next);
        co_await t.async_wait(asio::use_awaitable);
        const auto [p, heading] = circle(ea.pos, 0.1f * static_cast<float>(i));
        a->send(proto::Move{p, heading, static_cast<uint32_t>(i * 100)});
        corrections += only<proto::Correction>(co_await drain(*a, Millis(5))).size(); // drain(0) and (1) see nothing: whole ms left
    }
    corrections += only<proto::Correction>(co_await drain(*a, Millis(1000))).size(); // the last moves' answers
    x.check.expect(corrections == 0, "10 s at 7 m/s with turns: " + std::to_string(corrections) + " Corrections (want 0)");
    x.check.expect(!a->closed(), "still connected (100 Moves in 10 s: under the rate limit)");
    a->close();
}

// The same run with one cheat 3 s in (the client jumps 50 m and keeps running from there): exactly one Correction,
// none for the moves already in flight behind it, and once the bot rebases like a game client (the Correction's
// pos + everything it moved since the Move ackSeq) the server takes its moves again: an observer's last move
// is where the bot ends.
asio::awaitable<void> lag_reject(Ctx& x) {
    proto::EnterWorldOk ea, eb;
    auto b = co_await x.enter("s27b", eb);
    auto a = co_await x.enter("s27a", ea);
    if (!a || !b) co_return;
    co_await drain(*a, Millis(1500));
    co_await drain(*b, Millis(0));
    asio::steady_timer t(co_await asio::this_coro::executor);
    auto next = std::chrono::steady_clock::now();
    std::map<uint32_t, proto::Vec3> sent; // seq -> position
    proto::Vec3 offset{0, 0, 0}, p{};
    std::vector<proto::Correction> corrections;
    uint32_t cheatSeq = 0;
    auto cheatAt = std::chrono::steady_clock::now();
    const auto handle = [&](const std::vector<proto::Message>& msgs) {
        for (const auto& c : only<proto::Correction>(msgs)) {
            if (corrections.empty())
                std::printf("  the Correction came %lld ms after the cheating Move (one round trip + up to a tick + 100 ms of polling)\n",
                            static_cast<long long>(std::chrono::duration_cast<Millis>(std::chrono::steady_clock::now() - cheatAt).count()));
            corrections.push_back(c);
            const auto acked = sent.find(c.ackSeq);
            if (acked == sent.end()) continue;
            offset.x += c.pos.x - acked->second.x; // rebased = c.pos + (where it is now - where it was at ackSeq)
            offset.z += c.pos.z - acked->second.z;
        }
    };
    for (int i = 1; i <= 100; ++i) {
        next += Millis(100);
        t.expires_at(next);
        co_await t.async_wait(asio::use_awaitable);
        handle(co_await drain(*a, Millis(5))); // what has come so far (drain(0) and (1) see nothing: whole ms left)
        if (i == 30) offset.x += 50; // the cheat
        const auto [onCircle, heading] = circle(ea.pos, 0.1f * static_cast<float>(i));
        p = {onCircle.x + offset.x, onCircle.y, onCircle.z + offset.z};
        a->send(proto::Move{p, heading, static_cast<uint32_t>(i * 100)});
        sent[a->moveSeq] = p;
        if (i == 30) {
            cheatSeq = a->moveSeq;
            cheatAt = std::chrono::steady_clock::now();
        }
    }
    handle(co_await drain(*a, Millis(1000)));
    x.check.expect(corrections.size() == 1, "a 50 m jump while running: " + std::to_string(corrections.size()) +
                                                " Corrections (want exactly 1, no cascade)");
    if (!corrections.empty())
        x.check.expect(corrections[0].ackSeq == cheatSeq && corrections[0].epoch == 1,
                       "the Correction acks the cheating Move (seq " + std::to_string(corrections[0].ackSeq) + ", want " +
                           std::to_string(cheatSeq) + ") and starts epoch 1 (got " + std::to_string(corrections[0].epoch) + ")");
    std::optional<proto::Vec3> seen;
    for (const auto& m : moves_in(co_await drain(*b, Millis(500))))
        if (m.entityId == ea.entityId) seen = m.pos;
    x.check.expect(seen && distance_xz(*seen, p) < 0.01f, "the server ends where the rebased bot ends " + fmt(p) +
                                                              " (observer's last move " + (seen ? fmt(*seen) : "none") + ")");
    a->close();
    b->close();
}

// --- 29-37: proximity chat kinds and the chat service (docs/chat) ---

// A player in the world and on the chat service: the world connection, the chat connection, its character's name.
struct Chatter {
    std::shared_ptr<WorldClient> world, chat;
    proto::EnterWorldOk ok;
    std::string name;
    std::string token;
    std::optional<proto::ChatServer> chatServer; // as the world server announced it
    void close() {
        if (chat) chat->close();
        if (world) world->close();
    }
};

// Connect + Hello + Auth to the chat service (port: o.chatPort, or the link port for the link checks); the client
// once AuthOk came, else nullptr with the reason in why.
asio::awaitable<std::shared_ptr<WorldClient>> chat_connect(Ctx& x, const std::string& token, std::string* why = nullptr) {
    auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
    if (!co_await c->connect(x.o.chatHost, x.o.chatPort)) {
        if (why) *why = "connect: " + c->lastError;
        co_return nullptr;
    }
    if (!co_await c->login(token)) {
        if (why) *why = c->disconnectReason ? proto::code_name(*c->disconnectReason) : "no AuthOk";
        c->close();
        co_return nullptr;
    }
    co_return c;
}

// Tries chat_connect every second for up to `seconds` (the chat service restarting).
asio::awaitable<std::shared_ptr<WorldClient>> chat_reconnect(Ctx& x, const std::string& token, int seconds) {
    for (int i = 0; i < seconds; ++i) {
        if (auto c = co_await chat_connect(x, token)) co_return c;
        co_await sleep_for(Millis(1000));
    }
    co_return nullptr;
}

// Logs tag in, enters the world with its character (created with a new random name if it has none), takes the world's
// ChatServer and connects to the chat service (which auto-joins General).
asio::awaitable<std::optional<Chatter>> chatter(Ctx& x, const std::string& tag) {
    Chatter p;
    p.token = x.token(tag);
    p.world = co_await x.login(p.token, tag);
    if (!p.world) co_return std::nullopt;
    std::optional<proto::CharList> list;
    co_await request(*p.world, proto::CharListReq{}, &list);
    if (list && !list->chars.empty()) {
        p.name = list->chars.front().name;
    } else {
        std::optional<proto::CharCreated> created;
        co_await request(*p.world, proto::CharCreate{random_name(), x.o.charOptions}, &created);
        p.name = created ? created->character.name : "";
    }
    std::string err;
    if (!co_await enter_world(*p.world, "unused", p.ok, err)) {
        x.check.expect(false, tag + " entered world (" + err + ")");
        co_return std::nullopt;
    }
    p.chatServer = co_await p.world->wait_for<proto::ChatServer>(Millis(3000));
    p.chat = co_await chat_connect(x, p.token, &err);
    x.check.expect(p.chat != nullptr, tag + " (" + p.name + ") on the chat service" + (p.chat ? "" : " (" + err + ")"));
    if (!p.chat) co_return std::nullopt;
    co_return p;
}

template <typename T>
asio::awaitable<std::vector<T>> collect(WorldClient& c, Millis window) {
    co_return only<T>(co_await drain(c, window));
}

bool has_line(const std::vector<proto::ChatLine>& lines, proto::ChatTarget target, const std::string& name,
              const std::string& text) {
    return std::ranges::any_of(lines, [&](const proto::ChatLine& l) { return l.target == target && l.name == name && l.text == text; });
}

// The lines c receives within window: has one {target, name, text}?
asio::awaitable<bool> hears(WorldClient& c, proto::ChatTarget target, const std::string& name, const std::string& text,
                            Millis window = Millis(1200)) {
    co_return has_line(co_await collect<proto::ChatLine>(c, window), target, name, text);
}

// The world ChatMsgs c receives within window: has one {kind, text}?
asio::awaitable<bool> hears_say(WorldClient& c, proto::ChatKind kind, const std::string& text, Millis window = Millis(800)) {
    for (const auto& m : co_await collect<proto::ChatMsg>(c, window))
        if (m.kind == kind && m.text == text) co_return true;
    co_return false;
}

// The last ChatNotice c receives within window.
asio::awaitable<std::optional<proto::ChatNotice>> notice(WorldClient& c, Millis window = Millis(1200)) {
    const auto n = co_await collect<proto::ChatNotice>(c, window);
    co_return n.empty() ? std::nullopt : std::optional(n.back());
}

std::string notice_name(const std::optional<proto::ChatNotice>& n) {
    return n ? proto::chat_notice_name(n->code) : "none";
}

// Walks c from its spawn into the start map's cave_mouth portal, onto map 2; true once MapChange{2} came.
asio::awaitable<bool> to_cave(WorldClient& c, const proto::EnterWorldOk& ok) {
    co_await walk(c, ok.pos, 1042 - ok.pos.x, 999 - ok.pos.z);
    const auto mc = co_await c.wait_for<proto::MapChange>(Millis(3000));
    co_return mc && mc->map == 2;
}

// --- 29: say / yell / emote ranges on the world server ---
asio::awaitable<void> yell(Ctx& x) {
    proto::EnterWorldOk ea, eb, ec, ed;
    auto a = co_await x.enter("s29a", ea);
    auto b = co_await x.enter("s29b", eb);
    auto c = co_await x.enter("s29c", ec);
    auto d = co_await x.enter("s29d", ed);
    if (!a || !b || !c || !d) co_return;
    // From the spawn (1024, 1024), clear of the start map's portals (all east of x 1040): B 100 m west (in view),
    // C 200 m north (past the 128 m view radius, inside the 300 m yell radius), D 320 m south (past both).
    using namespace asio::experimental::awaitable_operators;
    co_await (walk(*b, eb.pos, -100, 0) && walk(*c, ec.pos, 0, 200) && walk(*d, ed.pos, 0, -320));
    co_await sleep_for(Millis(500));
    for (auto* p : {&a, &b, &c, &d}) co_await drain(**p, Millis(100));

    a->send(proto::Say{proto::ChatKind::say, "say test"});
    const bool sayA = co_await hears_say(*a, proto::ChatKind::say, "say test");
    const bool sayB = co_await hears_say(*b, proto::ChatKind::say, "say test", Millis(200));
    const bool sayC = co_await hears_say(*c, proto::ChatKind::say, "say test", Millis(200));
    x.check.expect(sayA && sayB && !sayC, "say: the sender and B (100 m), not C (200 m)");
    a->send(proto::Say{proto::ChatKind::yell, "YELL TEST"});
    const bool yB = co_await hears_say(*b, proto::ChatKind::yell, "YELL TEST");
    const bool yC = co_await hears_say(*c, proto::ChatKind::yell, "YELL TEST", Millis(200));
    const bool yD = co_await hears_say(*d, proto::ChatKind::yell, "YELL TEST", Millis(200));
    x.check.expect(yB && yC, "yell reaches B (100 m) and C (200 m, beyond the AOI view radius)");
    x.check.expect(!yD, "yell does not reach D (320 m, beyond CHAT_YELL_RADIUS 300)");
    a->send(proto::Say{proto::ChatKind::emote, "waves"});
    const bool eA = co_await hears_say(*a, proto::ChatKind::emote, "waves");
    const bool eB = co_await hears_say(*b, proto::ChatKind::emote, "waves", Millis(200));
    const bool eC = co_await hears_say(*c, proto::ChatKind::emote, "waves", Millis(200));
    x.check.expect(eA && eB && !eC, "emote: kind emote to the say audience (A, B), not C");
    for (auto* p : {&a, &b, &c, &d}) (*p)->close();
}

// --- 30: whispers across maps, and to an offline name ---
asio::awaitable<void> chat_whisper(Ctx& x) {
    auto a = co_await chatter(x, "s30a");
    auto b = co_await chatter(x, "s30b");
    if (!a || !b) co_return;
    x.check.expect(a->chatServer && a->chatServer->port > 0 && !a->chatServer->host.empty(),
                   "the world announces the chat service after EnterWorldOk (" +
                       (a->chatServer ? a->chatServer->host + ":" + std::to_string(a->chatServer->port) : "none") + ")");
    x.check.expect(co_await to_cave(*b->world, b->ok), "B walks into the cave portal: map 2");
    co_await drain(*a->chat, Millis(300));
    co_await drain(*b->chat, Millis(300));
    a->chat->send(proto::ChatSend{proto::ChatTarget::whisper, b->name, "psst, cave dweller"});
    x.check.expect(co_await hears(*b->chat, proto::ChatTarget::whisper, a->name, "psst, cave dweller"),
                   "whisper reaches B on another map");
    x.check.expect(co_await hears(*a->chat, proto::ChatTarget::whisper_sent, b->name, "psst, cave dweller", Millis(300)),
                   "A gets the echo: To " + b->name);
    const std::string nobody = random_name();
    a->chat->send(proto::ChatSend{proto::ChatTarget::whisper, nobody, "hello?"});
    const auto n = co_await notice(*a->chat);
    x.check.expect(n && n->code == proto::ChatNoticeCode::not_online && n->name == nobody,
                   "whisper to an offline name -> ChatNotice not_online (got " + notice_name(n) + ")");
    a->close();
    b->close();
}

// --- 31: the fixed channels ---
asio::awaitable<void> chat_channels(Ctx& x) {
    auto a = co_await chatter(x, "s31a");
    auto b = co_await chatter(x, "s31b");
    auto c = co_await chatter(x, "s31c");
    if (!a || !b || !c) co_return;
    const auto general = co_await collect<proto::ChatChannel>(*c->chat, Millis(500));
    x.check.expect(!general.empty() && general[0].number == 1 && general[0].channel == "General" && general[0].joined,
                   "a connected client is in General, channel 1");
    co_await drain(*a->chat, Millis(100));
    co_await drain(*b->chat, Millis(100));
    a->chat->send(proto::ChatSend{proto::ChatTarget::channel, "General", "hello General"});
    const bool genB = co_await hears(*b->chat, proto::ChatTarget::channel, a->name, "hello General");
    const bool genC = co_await hears(*c->chat, proto::ChatTarget::channel, a->name, "hello General", Millis(300));
    x.check.expect(genB && genC, "General reaches B and C");

    a->chat->send(proto::ChatJoin{"Trade"});
    const auto trade = co_await a->chat->wait_for<proto::ChatChannel>(Millis(1500));
    x.check.expect(trade && trade->number == 2 && trade->channel == "Trade" && trade->joined, "join Trade -> ChatChannel{2, Trade}");
    a->chat->send(proto::ChatSend{proto::ChatTarget::channel, "Trade", "WTS sword"});
    const bool tradeA = co_await hears(*a->chat, proto::ChatTarget::channel, a->name, "WTS sword");
    const bool tradeB = co_await hears(*b->chat, proto::ChatTarget::channel, a->name, "WTS sword", Millis(300));
    x.check.expect(tradeA && !tradeB, "Trade reaches its member A, not B (not a member)");
    b->chat->send(proto::ChatSend{proto::ChatTarget::channel, "Trade", "me too"});
    const auto n = co_await notice(*b->chat);
    x.check.expect(n && n->code == proto::ChatNoticeCode::not_in_channel, "a non-member sending to Trade -> not_in_channel (got " + notice_name(n) + ")");
    b->chat->send(proto::ChatJoin{"Trade"});
    co_await b->chat->wait_for<proto::ChatChannel>(Millis(1500));
    a->chat->send(proto::ChatSend{proto::ChatTarget::channel, "Trade", "WTB shield"});
    const bool joinedB = co_await hears(*b->chat, proto::ChatTarget::channel, a->name, "WTB shield");
    const bool stillC = co_await hears(*c->chat, proto::ChatTarget::channel, a->name, "WTB shield", Millis(300));
    x.check.expect(joinedB && !stillC, "after joining Trade B hears it; C (not a member) still doesn't");
    b->chat->send(proto::ChatLeave{"Trade"});
    const auto left = co_await b->chat->wait_for<proto::ChatChannel>(Millis(1500));
    x.check.expect(left && left->channel == "Trade" && !left->joined, "leave Trade -> ChatChannel{joined 0}");
    a->chat->send(proto::ChatSend{proto::ChatTarget::channel, "Trade", "last call"});
    x.check.expect(!co_await hears(*b->chat, proto::ChatTarget::channel, a->name, "last call"), "after leaving, B no longer hears Trade");
    a->close();
    b->close();
    c->close();
}

// --- 32: the zone channel follows map changes ---
asio::awaitable<void> chat_zone(Ctx& x) {
    auto a = co_await chatter(x, "s32a");
    auto b = co_await chatter(x, "s32b");
    auto c = co_await chatter(x, "s32c");
    if (!a || !b || !c) co_return;
    x.check.expect(co_await to_cave(*c->world, c->ok), "C walks into the cave: map 2");
    co_await sleep_for(Millis(300)); // the map change reaches the chat service over the link
    for (auto* p : {&*a, &*b, &*c}) co_await drain(*p->chat, Millis(100));
    a->chat->send(proto::ChatSend{proto::ChatTarget::zone, "", "zone one"});
    const bool zB = co_await hears(*b->chat, proto::ChatTarget::zone, a->name, "zone one");
    const bool zC = co_await hears(*c->chat, proto::ChatTarget::zone, a->name, "zone one", Millis(300));
    x.check.expect(zB && !zC, "A's zone line reaches B (map 1), not C (map 2)");
    x.check.expect(co_await to_cave(*b->world, b->ok), "B walks into the cave: map 2");
    co_await sleep_for(Millis(300));
    for (auto* p : {&*a, &*b, &*c}) co_await drain(*p->chat, Millis(100));
    a->chat->send(proto::ChatSend{proto::ChatTarget::zone, "", "zone two"});
    x.check.expect(!co_await hears(*b->chat, proto::ChatTarget::zone, a->name, "zone two"), "after B's map change, A's zone line no longer reaches B");
    c->chat->send(proto::ChatSend{proto::ChatTarget::zone, "", "cave zone"});
    const bool caveB = co_await hears(*b->chat, proto::ChatTarget::zone, c->name, "cave zone");
    const bool caveA = co_await hears(*a->chat, proto::ChatTarget::zone, c->name, "cave zone", Millis(300));
    x.check.expect(caveB && !caveA, "C's zone line (map 2) reaches B now, not A");
    a->close();
    b->close();
    c->close();
}

// --- 33: ignore / unignore ---
asio::awaitable<void> chat_ignore(Ctx& x) {
    auto a = co_await chatter(x, "s33a");
    auto b = co_await chatter(x, "s33b");
    auto c = co_await chatter(x, "s33c");
    if (!a || !b || !c) co_return;
    for (auto* p : {&*a, &*b, &*c}) co_await drain(*p->chat, Millis(300));
    b->chat->send(proto::ChatIgnore{a->name, true});
    const auto ig = co_await notice(*b->chat);
    x.check.expect(ig && ig->code == proto::ChatNoticeCode::ignored, "ignore -> ChatNotice ignored (got " + notice_name(ig) + ")");
    a->chat->send(proto::ChatSend{proto::ChatTarget::whisper, b->name, "ignored whisper"});
    a->chat->send(proto::ChatSend{proto::ChatTarget::channel, "General", "ignored channel"});
    a->chat->send(proto::ChatSend{proto::ChatTarget::zone, "", "ignored zone"});
    const auto toB = co_await collect<proto::ChatLine>(*b->chat, Millis(1500));
    const auto toC = co_await collect<proto::ChatLine>(*c->chat, Millis(300));
    x.check.expect(toB.empty(), "B gets none of the ignored sender's whisper, channel and zone lines (" + std::to_string(toB.size()) + ")");
    x.check.expect(has_line(toC, proto::ChatTarget::channel, a->name, "ignored channel") &&
                       has_line(toC, proto::ChatTarget::zone, a->name, "ignored zone"),
                   "C still hears A");
    b->chat->send(proto::ChatIgnore{a->name, false});
    const auto un = co_await notice(*b->chat);
    x.check.expect(un && un->code == proto::ChatNoticeCode::unignored, "unignore -> ChatNotice unignored");
    co_await sleep_for(Millis(1000));
    a->chat->send(proto::ChatSend{proto::ChatTarget::whisper, b->name, "heard again"});
    x.check.expect(co_await hears(*b->chat, proto::ChatTarget::whisper, a->name, "heard again"), "after unignore B hears A again");
    a->close();
    b->close();
    c->close();
}

// --- 34: GM mutes. Needs the gm account (tag s34g) to have role gm in auth's database (integration.sh sets it after
// "chat-mute-setup"). Prints "READY restart-chat" after the mute; the script restarts the chat service, the mute must
// survive it; then it runs out after its minute; mute + unmute last.
asio::awaitable<void> chat_mute_setup(Ctx& x) {
    x.token("s34g"); // registers the account
    std::printf("GM_EMAIL %s\n", bot_account(x.o, "s34g").email.c_str());
    co_return;
}

asio::awaitable<void> chat_mute(Ctx& x) {
    auto a = co_await chatter(x, "s34a");
    auto b = co_await chatter(x, "s34b");
    auto g = co_await chatter(x, "s34g");
    if (!a || !b || !g) co_return;
    x.check.expect(g->world->authOk && g->world->authOk->role == proto::Role::gm, "s34g's token says role gm");
    for (auto* p : {&*a, &*b, &*g}) co_await drain(*p->chat, Millis(300));
    a->chat->send(proto::ChatMute{b->name, 1});
    const auto refused = co_await notice(*a->chat);
    x.check.expect(refused && refused->code == proto::ChatNoticeCode::not_allowed, "a player can't mute: not_allowed (got " + notice_name(refused) + ")");
    g->chat->send(proto::ChatMute{b->name, 1});
    const auto set = co_await notice(*g->chat);
    const auto told = co_await notice(*b->chat, Millis(300));
    const auto mutedAt = std::chrono::steady_clock::now();
    x.check.expect(set && set->code == proto::ChatNoticeCode::mute_set && set->minutes == 1, "the GM mutes B for 1 minute: mute_set");
    x.check.expect(told && told->code == proto::ChatNoticeCode::you_muted, "B is told: you_muted (got " + notice_name(told) + ")");

    const auto muted_everywhere = [&](const char* when) -> asio::awaitable<void> {
        b->chat->send(proto::ChatSend{proto::ChatTarget::channel, "General", "can I talk?"});
        const auto n = co_await notice(*b->chat);
        x.check.expect(n && n->code == proto::ChatNoticeCode::muted && !co_await hears(*a->chat, proto::ChatTarget::channel, b->name, "can I talk?", Millis(300)),
                       std::string(when) + ": B's channel line dropped, ChatNotice muted (got " + notice_name(n) + ")");
        for (auto kind : {proto::ChatKind::say, proto::ChatKind::yell, proto::ChatKind::emote}) {
            b->world->send(proto::Say{kind, "muted proximity"});
            const auto wn = co_await notice(*b->world, Millis(800));
            const bool heardA = co_await hears_say(*a->world, kind, "muted proximity", Millis(200));
            x.check.expect(wn && wn->code == proto::ChatNoticeCode::muted && !heardA,
                           std::string(when) + ": B's world " + (kind == proto::ChatKind::say ? "say" : kind == proto::ChatKind::yell ? "yell" : "emote") +
                               " dropped with ChatNotice muted from the world server");
        }
    };
    co_await muted_everywhere("muted");
    const auto firstBatch = std::chrono::steady_clock::now();

    std::printf("READY restart-chat\n");
    std::fflush(stdout);
    co_await b->chat->wait_closed(Millis(60000)); // the chat service goes down
    co_await sleep_for(Millis(2000));
    for (auto* p : {&*a, &*b, &*g}) {
        p->chat->close();
        p->chat = co_await chat_reconnect(x, p->token, 60);
        x.check.expect(p->chat != nullptr, p->name + " reconnects after the chat service restart");
        if (!p->chat) co_return;
    }
    co_await sleep_for(Millis(1500)); // the world link is back too: its mutes re-synced
    while (std::chrono::steady_clock::now() < firstBatch + std::chrono::seconds(11)) // the world's 5 per 10 s say limit
        co_await sleep_for(Millis(500));
    for (auto* p : {&*a, &*b, &*g}) co_await drain(*p->chat, Millis(200));
    co_await muted_everywhere("after the chat restart (persisted)");

    // The mute ends a minute after it was set.
    while (std::chrono::steady_clock::now() < mutedAt + std::chrono::seconds(62)) co_await sleep_for(Millis(1000));
    b->chat->send(proto::ChatSend{proto::ChatTarget::channel, "General", "free at last"});
    x.check.expect(co_await hears(*a->chat, proto::ChatTarget::channel, b->name, "free at last"), "the mute expired after 1 minute: B talks on General");
    b->world->send(proto::Say{proto::ChatKind::say, "free say"});
    x.check.expect(co_await hears_say(*a->world, proto::ChatKind::say, "free say"), "... and in say");

    g->chat->send(proto::ChatMute{b->name, 5});
    co_await notice(*g->chat);
    g->chat->send(proto::ChatMute{b->name, 0});
    const auto cleared = co_await notice(*g->chat);
    const auto unmuted = co_await notice(*b->chat, Millis(300));
    x.check.expect(cleared && cleared->code == proto::ChatNoticeCode::mute_cleared && unmuted &&
                       unmuted->code == proto::ChatNoticeCode::you_unmuted,
                   "mute 5 then unmute: mute_cleared to the GM, you_unmuted to B");
    co_await sleep_for(Millis(300));
    b->world->send(proto::Say{proto::ChatKind::say, "unmuted say"});
    x.check.expect(co_await hears_say(*a->world, proto::ChatKind::say, "unmuted say"), "unmuted: B's say reaches A");
    a->close();
    b->close();
    g->close();
}

// --- 35: the chat service's text rules, as the world's ---
asio::awaitable<void> chat_limits(Ctx& x) {
    auto a = co_await chatter(x, "s35a");
    auto b = co_await chatter(x, "s35b");
    if (!a || !b) co_return;
    for (auto* p : {&*a, &*b}) co_await drain(*p->chat, Millis(300));
    a->chat->send(proto::ChatSend{proto::ChatTarget::channel, "General", "bad\x01\x1b[31m\ttext"});
    x.check.expect(co_await hears(*b->chat, proto::ChatTarget::channel, a->name, "bad[31mtext"), "control characters stripped");
    a->chat->send(proto::ChatSend{proto::ChatTarget::channel, "General", std::string(257, 'x')});
    const auto big = co_await notice(*a->chat);
    a->chat->send(proto::ChatSend{proto::ChatTarget::channel, "General", "bad \xC3"});
    const auto utf = co_await notice(*a->chat);
    x.check.expect(big && big->code == proto::ChatNoticeCode::rejected && utf && utf->code == proto::ChatNoticeCode::rejected,
                   "over 256 bytes and invalid UTF-8 -> ChatNotice rejected");
    for (int i = 0; i < 6; ++i) a->chat->send(proto::ChatSend{proto::ChatTarget::channel, "General", "spam " + std::to_string(i)});
    const auto lines = co_await collect<proto::ChatLine>(*b->chat, Millis(1500));
    const auto limited = co_await notice(*a->chat, Millis(300));
    // the oversized and invalid ones count: 3 used, so 2 of the 6 spam lines get through
    x.check.expect(lines.size() == 2 && limited && limited->code == proto::ChatNoticeCode::rate_limited,
                   "rate limit 5 per 10 s: 2 of 6 spam lines delivered (" + std::to_string(lines.size()) + "), then rate_limited");
    x.check.expect(!a->chat->closed() && !b->chat->closed(), "rejected and rate-limited chat is dropped, not fatal");
    a->close();
    b->close();
}

// --- 36: who the chat service refuses ---
asio::awaitable<void> chat_reject_login(Ctx& x, const std::string& what, const std::string& token, Code expect) {
    auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
    if (!co_await c->connect(x.o.chatHost, x.o.chatPort)) { x.check.expect(false, what + ": connect"); co_return; }
    co_await c->login(token);
    co_await c->wait_closed(Millis(10000));
    x.check.expect(!c->authOk && c->disconnectReason == expect,
                   "chat: " + what + " -> " + proto::code_name(expect) + " (got " +
                       (c->disconnectReason ? proto::code_name(*c->disconnectReason) : "none") + ")");
}

asio::awaitable<void> chat_link_reject(Ctx& x, const std::string& what, const proto::LinkHello& hello, Code expect) {
    auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
    if (!co_await c->connect(x.o.chatHost, x.o.chatLinkPort)) { x.check.expect(false, what + ": connect"); co_return; }
    c->send(hello);
    co_await c->wait_closed(Millis(5000));
    x.check.expect(c->disconnectReason == expect, "chat link: " + what + " -> " + proto::code_name(expect) + " (got " +
                                                      (c->disconnectReason ? proto::code_name(*c->disconnectReason) : "none") + ")");
}

asio::awaitable<void> chat_auth(Ctx& x) {
    const std::string good = x.token("s36");
    co_await chat_reject_login(x, "garbage token", "not-a-jwt", Code::invalid_token);
    co_await chat_reject_login(x, "token with tampered signature", good.substr(0, good.size() - 4) + "AAAA", Code::invalid_token);
    std::string err;
    const std::string expired = mint_expired_token(x, err);
    x.check.expect(!expired.empty(), "minted an expired token with auth's key" + (err.empty() ? "" : " (" + err + ")"));
    if (!expired.empty()) co_await chat_reject_login(x, "expired token", expired, Code::token_expired);
    const auto t0 = std::chrono::steady_clock::now();
    co_await chat_reject_login(x, "valid token, no character in a world", good, Code::no_character);
    const double held = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    x.check.expect(held > 4 && held < 8, "... after being held CHAT_BIND_TIMEOUT_MS (" + std::to_string(held) + " s)");
    co_await chat_link_reject(x, "wrong shared secret", {proto::kProtocolVersion, "wrong-secret-0123456789"}, Code::invalid_token);
    co_await chat_link_reject(x, "wrong protocol version", {7, "wrong-secret-0123456789"}, Code::protocol_mismatch);
    {
        auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
        co_await c->connect(x.o.chatHost, x.o.chatLinkPort);
        c->send(proto::CharOnline{1, 1, "Sneaky", 1});
        co_await c->wait_closed(Millis(5000));
        x.check.expect(c->disconnectReason == Code::unexpected_message, "chat link: CharOnline before LinkHello -> unexpected_message");
    }
}

// --- 37: the chat service down and back. Prints "READY stop-chat" (the script stops it), then "READY start-chat" once
// proximity chat was checked without it; the clients reconnect, rejoin General, whispers work again.
asio::awaitable<void> chat_down(Ctx& x) {
    auto a = co_await chatter(x, "s37a");
    auto b = co_await chatter(x, "s37b");
    if (!a || !b) co_return;
    std::printf("READY stop-chat\n");
    std::fflush(stdout);
    const bool aLost = co_await a->chat->wait_closed(Millis(60000));
    const bool bLost = co_await b->chat->wait_closed(Millis(5000));
    x.check.expect(aLost && bLost, "both chat connections close when the chat service stops");
    co_await sleep_for(Millis(1000));
    std::string why;
    x.check.expect(!co_await chat_connect(x, a->token, &why), "the chat service is unreachable (" + why + ")");
    co_await drain(*b->world, Millis(300));
    a->world->send(proto::Say{proto::ChatKind::say, "say without chat"});
    x.check.expect(co_await hears_say(*b->world, proto::ChatKind::say, "say without chat", Millis(1500)), "say still works without the chat service");
    x.check.expect(!a->world->closed() && !b->world->closed(), "the world connections stay up");
    std::printf("READY start-chat\n");
    std::fflush(stdout);
    for (auto* p : {&*a, &*b}) {
        p->chat = co_await chat_reconnect(x, p->token, 90);
        x.check.expect(p->chat != nullptr, p->name + " reconnects to the chat service");
        if (!p->chat) co_return;
        const auto general = co_await p->chat->wait_for<proto::ChatChannel>(Millis(2000));
        x.check.expect(general && general->channel == "General" && general->joined, p->name + " is in General again");
    }
    a->chat->send(proto::ChatSend{proto::ChatTarget::whisper, b->name, "you're back"});
    x.check.expect(co_await hears(*b->chat, proto::ChatTarget::whisper, a->name, "you're back"), "whispers work again");
    a->close();
    b->close();
}

// --- 39-40: the admin port (docs/world/protocol.html#admin) ---

std::string admin_secret() {
#ifdef _MSC_VER
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "WORLD_ADMIN_SECRET") != 0 || !buf) return "";
    std::string s(buf);
    std::free(buf);
    return s;
#else
    const char* s = std::getenv("WORLD_ADMIN_SECRET");
    return s ? s : "";
#endif
}

// Connects to the admin port and says hello with secret; the client, or nullptr if the connection failed.
asio::awaitable<std::shared_ptr<WorldClient>> admin_connect(Ctx& x, const std::string& secret) {
    auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
    if (!co_await c->connect(x.o.host, x.o.adminPort)) co_return nullptr;
    c->send(proto::LinkHello{proto::kAdminProtocolVersion, secret});
    co_return c;
}

asio::awaitable<std::vector<proto::AdminOnlineEntry>> admin_online(WorldClient& admin) {
    std::vector<proto::AdminOnlineEntry> all;
    admin.send(proto::AdminOnlineReq{});
    for (;;) {
        auto chunk = co_await admin.wait_for<proto::AdminOnline>(Millis(5000));
        if (!chunk) co_return all;
        all.insert(all.end(), chunk->entries.begin(), chunk->entries.end());
        if (!chunk->more) co_return all;
    }
}

asio::awaitable<std::optional<uint32_t>> admin_kick(WorldClient& admin, const proto::AdminKick& k) {
    admin.send(k);
    auto r = co_await admin.wait_for<proto::AdminKicked>(Millis(5000));
    co_return r ? std::optional(r->kicked) : std::nullopt;
}

// 39: wrong secret refused; the online list shows a bot in the world and one in the lobby; kick by character
// disconnects it with kicked and its position is saved; kick by account reaches a lobby session.
asio::awaitable<void> admin_port(Ctx& x) {
    const std::string secret = admin_secret();
    x.check.expect(secret.size() >= 16, "WORLD_ADMIN_SECRET is set for the bot");

    {
        auto bad = co_await admin_connect(x, "wrong-secret-0123456789");
        x.check.expect(bad != nullptr, "admin port: TLS connect");
        if (!bad) co_return;
        co_await bad->wait_closed(Millis(5000));
        x.check.expect(bad->disconnectReason == Code::invalid_token, "admin port: wrong secret -> invalid_token");
    }
    {
        auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
        co_await c->connect(x.o.host, x.o.adminPort);
        c->send(proto::AdminOnlineReq{});
        co_await c->wait_closed(Millis(5000));
        x.check.expect(c->disconnectReason == Code::unexpected_message, "admin port: request before hello -> unexpected_message");
    }

    proto::EnterWorldOk e1;
    auto player = co_await x.enter("s39", e1);
    if (!player) co_return;
    const auto p = co_await walk(*player, e1.pos, 6, -11);
    co_await sleep_for(Millis(500));
    auto lobby = co_await x.login(x.token("s39b"), "s39b (lobby)");
    if (!lobby) co_return;

    auto admin = co_await admin_connect(x, secret);
    const bool up = admin && co_await admin->wait_for<proto::LinkOk>(Millis(5000));
    x.check.expect(up, "admin port: right secret -> LinkOk");
    if (!up) co_return;

    const uint64_t acc = player->authOk->accountId;
    auto online = co_await admin_online(*admin);
    auto mine = std::find_if(online.begin(), online.end(), [&](const auto& e) { return e.accountId == acc; });
    const bool listed = mine != online.end() && mine->charId != 0 && !mine->name.empty() &&
                        mine->map == e1.map && mine->sessionId != 0 && mine->connectedAtMs > 1700000000000ull;
    x.check.expect(listed, "online list has the bot in the world (account " + std::to_string(acc) + ", " +
                               std::to_string(online.size()) + " entries)");
    if (!listed) co_return;
    std::printf("  online: account %llu char %llu %s map %u session %u\n", static_cast<unsigned long long>(mine->accountId),
                static_cast<unsigned long long>(mine->charId), mine->name.c_str(), mine->map, mine->sessionId);
    const uint64_t lobbyAcc = lobby->authOk->accountId;
    x.check.expect(std::any_of(online.begin(), online.end(),
                               [&](const auto& e) { return e.accountId == lobbyAcc && e.charId == 0 && e.name.empty(); }),
                   "online list has the lobby session with no character");

    const uint64_t charId = mine->charId;
    const auto kicked = co_await admin_kick(*admin, {0, charId, "integration test"});
    x.check.expect(kicked == 1u, "kick by character -> kicked 1");
    co_await player->wait_closed(Millis(5000));
    x.check.expect(player->disconnectReason == Code::kicked, "the kicked client gets Disconnect{kicked}");
    online = co_await admin_online(*admin);
    x.check.expect(std::none_of(online.begin(), online.end(), [&](const auto& e) { return e.accountId == acc; }),
                   "the kicked account is no longer online");
    x.check.expect(co_await admin_kick(*admin, {0, charId, "again"}) == 0u, "kicking an offline character -> kicked 0");

    x.check.expect(co_await admin_kick(*admin, {lobbyAcc, 0, "lobby"}) == 1u, "kick by account reaches a lobby session");
    co_await lobby->wait_closed(Millis(5000));
    x.check.expect(lobby->disconnectReason == Code::kicked, "the lobby session gets Disconnect{kicked}");
    admin->close();

    co_await sleep_for(Millis(500));
    proto::EnterWorldOk e2;
    auto again = co_await x.enter("s39", e2);
    x.check.expect(again && same_pos(e2.pos, p), "the kicked character was saved: " + fmt(p) + " (got " + fmt(e2.pos) + ")");
    if (again) again->close();
}

// 40: with WORLD_ADMIN_SECRET unset the admin port is closed and the world still serves players.
asio::awaitable<void> admin_off(Ctx& x) {
    auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, x.tls);
    x.check.expect(!co_await c->connect(x.o.host, x.o.adminPort), "admin port: nothing listening");
    proto::EnterWorldOk e;
    auto player = co_await x.enter("s40", e);
    if (player) player->close();
}

// --- 41: instance maps (docs/world/instances.html) with the sample maps/crypt (map 6: maxPlayers 2,
// emptyTimeoutSec 5, exit on map 1 at (1030, 0, 1060)) and lua/systems/test_hooks.lua's instanceKey ---

// The crypt gate on the start map (crypt_gate [900..910] x [1100..1110]): its middle.
constexpr float kCryptGateX = 905, kCryptGateZ = 1105;

// Sets c's instance key (test_hooks.lua), then walks from pos into the start map's crypt gate.
asio::awaitable<void> to_crypt(WorldClient& c, proto::Vec3 pos, const std::string& key) {
    c.send(proto::ClientEvent{"test.instance_key", nlohmann::json{{"key", key}}.dump()});
    co_await sleep_for(Millis(300)); // the key is set before the walk ends: the next tick's Lua update has it
    co_await walk(c, pos, kCryptGateX - pos.x, kCryptGateZ - pos.z);
}

// The player and NPC Spawns c got after its MapChange onto map 6 (nullopt: no such MapChange).
struct CryptView {
    std::optional<proto::MapChange> change;
    std::set<uint32_t> players, npcs;
};
CryptView crypt_view(const std::vector<proto::Message>& msgs) {
    CryptView v;
    for (const auto& m : msgs) {
        if (auto* mc = std::get_if<proto::MapChange>(&m); mc && mc->map == 6) v.change = *mc;
        if (auto* s = std::get_if<proto::Spawn>(&m); s && v.change) {
            if (s->kind == proto::SpawnKind::player) v.players.insert(s->entityId);
            else if (s->name == "Bat") v.npcs.insert(s->entityId);
        }
    }
    return v;
}

// Every live context the admin port lists, or nullopt without WORLD_ADMIN_SECRET (the instance checks are skipped).
asio::awaitable<std::optional<std::vector<proto::AdminInstanceEntry>>> admin_instances(Ctx& x) {
    const std::string secret = admin_secret();
    if (secret.empty()) co_return std::nullopt;
    auto admin = co_await admin_connect(x, secret);
    if (!admin || !co_await admin->wait_for<proto::LinkOk>(Millis(5000))) {
        x.check.expect(false, "admin port: LinkOk for the instance list");
        co_return std::nullopt;
    }
    std::vector<proto::AdminInstanceEntry> all;
    admin->send(proto::AdminInstancesReq{});
    for (;;) {
        auto chunk = co_await admin->wait_for<proto::AdminInstances>(Millis(5000));
        if (!chunk) break;
        all.insert(all.end(), chunk->entries.begin(), chunk->entries.end());
        if (!chunk->more) break;
    }
    admin->close();
    co_return all;
}

const proto::AdminInstanceEntry* copy_keyed(const std::vector<proto::AdminInstanceEntry>& all, const std::string& key) {
    for (const auto& e : all)
        if (e.map == 6 && e.key == key) return &e;
    return nullptr;
}

// A and B with key x, C with key y walk into the crypt gate: A and B share a copy (they see each other and the same
// Bat), C has its own (neither, and another Bat). D with key x is refused: no MapChange, test.instance_full. The admin
// port lists both copies. A relogs: back in its copy. Everyone leaves; 5 s later the copy is gone (admin list) and A
// relogs at the crypt's exit on map 1.
asio::awaitable<void> instances(Ctx& x) {
    const std::string kx = "x-" + x.o.runId, ky = "y-" + x.o.runId;
    proto::EnterWorldOk ea, eb, ec, ed;
    auto a = co_await x.enter("s41a", ea);
    auto b = co_await x.enter("s41b", eb);
    auto c = co_await x.enter("s41c", ec);
    auto d = co_await x.enter("s41d", ed);
    if (!a || !b || !c || !d) co_return;
    x.check.expect(ea.map == 1 && eb.map == 1 && ec.map == 1 && ed.map == 1, "A, B, C and D enter map 1");
    using namespace asio::experimental::awaitable_operators;
    co_await (to_crypt(*a, ea.pos, kx) && to_crypt(*b, eb.pos, kx) && to_crypt(*c, ec.pos, ky));
    co_await sleep_for(Millis(1500)); // the last one's Spawns
    const CryptView va = crypt_view(co_await drain(*a, Millis(300))), vb = crypt_view(co_await drain(*b, Millis(300))),
                    vc = crypt_view(co_await drain(*c, Millis(300)));
    x.check.expect(va.change && vb.change && vc.change && same_pos(va.change->pos, {128, 0, 128}),
                   "A, B and C: MapChange{6} at the portal's (128, 0, 128)");
    x.check.expect(va.players.contains(eb.entityId) && vb.players.contains(ea.entityId),
                   "same key: A and B are in one copy and see each other");
    x.check.expect(!vc.players.contains(ea.entityId) && !vc.players.contains(eb.entityId) &&
                       !va.players.contains(ec.entityId) && !vb.players.contains(ec.entityId),
                   "other key: C is in another copy, seeing neither A nor B, seen by neither");
    x.check.expect(va.npcs.size() == 1 && va.npcs == vb.npcs && vc.npcs.size() == 1 && vc.npcs != va.npcs,
                   "each copy has its own Bat: A and B see the same one, C another (" + std::to_string(va.npcs.size()) +
                       ", " + std::to_string(vc.npcs.size()) + ")");

    // D, key x: the copy holds 2 already.
    co_await to_crypt(*d, ed.pos, kx);
    const auto dm = co_await drain(*d, Millis(1500));
    bool full = false;
    for (const auto& ev : only<proto::ServerEvent>(dm))
        full |= ev.name == "test.instance_full" && ev.json.find(kx) != std::string::npos && ev.json.find("\"map\":6") != std::string::npos;
    x.check.expect(!crypt_view(dm).change && only<proto::MapChange>(dm).empty(), "D, key x: refused, no MapChange");
    x.check.expect(full, "D gets test.instance_full {map = 6, key = x} (instance.full on its map's bus)");

    if (const auto listed = co_await admin_instances(x)) {
        const auto* cx = copy_keyed(*listed, kx);
        const auto* cy = copy_keyed(*listed, ky);
        const bool start = std::any_of(listed->begin(), listed->end(), [](const auto& e) { return e.map == 1 && e.instance == 0; });
        x.check.expect(cx && cx->players == 2 && cx->npcs == 1 && cx->mapName == "Crypt" && cx->instance > 0 &&
                           cx->createdAtMs > 1700000000000ull && cx->emptySinceMs == 0 && cy && cy->players == 1 &&
                           cy->instance != cx->instance && start,
                       "admin port lists map 1's context and both copies (x: 2 players, 1 NPC; y: 1 player)");
        if (cx) std::printf("  admin: copy %u key %s players %u npcs %u\n", cx->instance, cx->key.c_str(), cx->players, cx->npcs);
    } else {
        std::printf("  WORLD_ADMIN_SECRET not set: the admin instance list is not checked\n");
    }

    // A relogs: its save names the copy, which is live with room (B is still in it).
    a->close();
    co_await sleep_for(Millis(500));
    proto::EnterWorldOk again;
    auto a2 = co_await x.enter("s41a", again);
    x.check.expect(a2 && again.map == 6 && same_pos(again.pos, {128, 0, 128}),
                   "relog inside a live copy: back on map 6 at " + fmt(again.pos) + " (map " + std::to_string(again.map) + ")");
    const auto rejoin = a2 ? co_await drain(*a2, Millis(1000)) : std::vector<proto::Message>{};
    bool seesB = false;
    for (const auto& s : only<proto::Spawn>(rejoin)) seesB |= s.entityId == eb.entityId;
    x.check.expect(seesB, "the relogged A sees B again: the same copy");

    // Everyone out; the empty copy x is destroyed 5 s later; A then relogs at the crypt's exit.
    for (auto* p : {&a2, &b, &c, &d})
        if (*p) (*p)->close();
    co_await sleep_for(Millis(7000));
    if (const auto listed = co_await admin_instances(x))
        x.check.expect(!copy_keyed(*listed, kx) && !copy_keyed(*listed, ky), "5 s empty: both copies are gone from the admin list");
    proto::EnterWorldOk out;
    auto a3 = co_await x.enter("s41a", out);
    x.check.expect(a3 && out.map == 1 && same_pos(out.pos, {1030, 0, 1060}) && std::fabs(out.heading - 3.14f) < 0.01f,
                   "relog after the copy was destroyed: the crypt's exit, map 1 at (1030, 0, 1060) (got map " +
                       std::to_string(out.map) + " " + fmt(out.pos) + ")");
    if (a3) a3->close();
}

// 42: a server restart loses every copy. Phase 1: a bot walks into the crypt (key r) and logs out there (saved with its
// instance id); integration.sh restarts the world; phase 2: the same character enters at the crypt's exit.
asio::awaitable<void> instance_restart(Ctx& x) {
    proto::EnterWorldOk e;
    auto c = co_await x.enter("s42", e);
    if (!c) co_return;
    co_await to_crypt(*c, e.pos, "r-" + x.o.runId);
    const auto mc = co_await c->wait_for<proto::MapChange>(Millis(3000));
    x.check.expect(mc && mc->map == 6, "into the crypt: MapChange{6}");
    c->close();
}

asio::awaitable<void> instance_restart_verify(Ctx& x) {
    proto::EnterWorldOk e;
    auto c = co_await x.enter("s42", e);
    x.check.expect(c && e.map == 1 && same_pos(e.pos, {1030, 0, 1060}),
                   "after a restart the saved copy is gone: the crypt's exit, map 1 at (1030, 0, 1060) (got map " +
                       std::to_string(e.map) + " " + fmt(e.pos) + ")");
    if (c) c->close();
}

// --- 43: layers (docs/world/maps.html#layers), START_MAP with "layerCap": 2 (integration.sh mounts a copy of
// the maps with it) ---

// A and B enter START_MAP's base layer (2, its cap) and see each other. C enters: every layer is full, so a new one;
// C sees neither, neither sees C. The admin port lists map 1's base with 2 players and the new layer with 1.
asio::awaitable<void> layers(Ctx& x) {
    proto::EnterWorldOk ea, eb, ec;
    auto a = co_await x.enter("s43a", ea);
    auto b = co_await x.enter("s43b", eb);
    if (!a || !b) co_return;
    co_await sleep_for(Millis(500));
    auto c = co_await x.enter("s43c", ec);
    if (!c) co_return;
    co_await sleep_for(Millis(1500)); // the Spawns, if any
    const auto ma = co_await drain(*a, Millis(300)), mb = co_await drain(*b, Millis(300)), mc = co_await drain(*c, Millis(300));
    const auto sees = [](const std::vector<proto::Message>& msgs, uint32_t id) {
        for (const auto& s : only<proto::Spawn>(msgs))
            if (s.entityId == id) return true;
        return false;
    };
    x.check.expect(ea.map == 1 && eb.map == 1 && ec.map == 1 && same_pos(ea.pos, ec.pos), "A, B and C enter map 1 at its spawn");
    x.check.expect(sees(ma, eb.entityId) && sees(mb, ea.entityId), "A and B share the base layer and see each other");
    x.check.expect(!sees(mc, ea.entityId) && !sees(mc, eb.entityId) && !sees(ma, ec.entityId) && !sees(mb, ec.entityId),
                   "C is in a new layer: it sees neither A nor B, and neither sees C");
    if (const auto listed = co_await admin_instances(x)) {
        const proto::AdminInstanceEntry *base = nullptr, *extra = nullptr;
        for (const auto& e : *listed)
            if (e.map == 1) (e.instance ? extra : base) = &e;
        x.check.expect(base && base->players == 2 && extra && extra->players == 1 && extra->key.empty(),
                       "admin port lists map 1's base layer (2 players) and the new layer (1 player)");
        if (extra) std::printf("  admin: map 1 layer %u players %u npcs %u\n", extra->instance, extra->players, extra->npcs);
    } else {
        std::printf("  WORLD_ADMIN_SECRET not set: the admin instance list is not checked\n");
    }
    for (auto* p : {&a, &b, &c}) (*p)->close();
}

} // namespace

int run_scenario(const Options& o) {
    asio::io_context io;
    asio::ssl::context tls = make_client_tls(o);
    Ctx x{o, tls, AuthClient(o.auth), {}};
    using Fn = asio::awaitable<void> (*)(Ctx&);
    const std::map<std::string, Fn> all = {
        {"auth", auth_handshake},       {"characters", characters},   {"ownership", ownership},
        {"aoi", aoi},                   {"chat", chat},               {"speedhack", speedhack},
        {"malformed", malformed},       {"handshake", handshake_rejects}, {"duplicate", duplicate_login},
        {"persist", persist_relogin},   {"shutdown", shutdown_phase1}, {"restart-verify", restart_verify},
        {"dbdown", db_down},            {"components", components},   {"components-verify", components_verify},
        {"events", events},             {"fsm", fsm},                 {"npcs", npcs},
        {"portal", portal},             {"respawn", respawn},         {"terrain", terrain},
        {"navmesh", navmesh},           {"client-events", client_events}, {"client-event-rate", client_event_rate},
        {"objects", objects},
        {"lag-run", lag_run},           {"lag-reject", lag_reject},
        {"yell", yell},                 {"chat-whisper", chat_whisper}, {"chat-channels", chat_channels},
        {"chat-zone", chat_zone},       {"chat-ignore", chat_ignore},   {"chat-mute-setup", chat_mute_setup},
        {"chat-mute", chat_mute},       {"chat-limits", chat_limits},   {"chat-auth", chat_auth},
        {"chat-down", chat_down},       {"admin", admin_port},          {"admin-off", admin_off},
        {"instances", instances},
        {"instance-restart", instance_restart}, {"instance-restart-verify", instance_restart_verify},
        {"layers", layers},
    };
    auto it = all.find(o.scenario);
    if (it == all.end()) {
        std::fprintf(stderr, "unknown scenario '%s'\n", o.scenario.c_str());
        return 2;
    }
    std::printf("scenario %s\n", o.scenario.c_str());
    asio::co_spawn(io, it->second(x), [&](std::exception_ptr e) {
        io.stop(); // clients left open by a failed step must not keep the run alive
        if (!e) return;
        try { std::rethrow_exception(e); } catch (const std::exception& ex) { x.check.expect(false, std::string("exception: ") + ex.what()); }
    });
    io.run();
    std::printf("%s %s\n", x.check.ok ? "PASS" : "FAIL", o.scenario.c_str());
    return x.check.ok ? 0 : 1;
}

} // namespace chthonic::bot
