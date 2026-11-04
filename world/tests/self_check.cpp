// Assert-style checks for the world server's pure logic: frame codec (every message), frame rejection, names,
// movement, AOI sets, chat sanitizing and rate limits, config validation, the DB save queue (no MySQL), send fan-out (one encode), buffered session reads over a 127.0.0.1 TLS loopback, the net-thread/game-thread handoff (sessions on 4 net threads), tick stats and JWT/JWKS verification with
// locally generated RSA keys; lua_checks.cpp covers the Lua layer and the maps (manifests, World on several maps). No database or outside network; time is passed in (only the lag-simulator fan-out waits a few ms).
// Exit 0 if every check passes. Run by CTest as world_self_check.
#include "auth/jwks_cache.h"
#include "auth/jwt_verify.h"
#include "core/config.h"
#include "core/log.h"
#include "core/rate_limiter.h"
#include "core/tick_stats.h"
#include "core/utc.h"
#include "db/db_worker.h"
#include "net/net_threads.h"
#include "net/session.h"
#include "net/tls_context.h"
#include "world/aoi_grid.h"
#include "world/components.h"
#include "world/rules.h"
#include "world/systems.h"
#include "world/world.h"
#include "lua/components.h"

#include <codec.h>

#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <crtdbg.h>
#include <cstdlib>
#endif

using namespace chthonic;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, std::source_location where = std::source_location::current()) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAILED line %u: %s\n", static_cast<unsigned>(where.line()), what);
}

const TimePoint T0{std::chrono::seconds(1000)};

// --- protocol ---

template <typename T>
void roundtrip(const T& msg, const char* name) {
    const auto frame = proto::encode(proto::Message{msg});
    const auto d = proto::decode_frame(frame, 4096);
    const bool ok = d.status == proto::DecodeStatus::ok && d.consumed == frame.size() &&
                    std::holds_alternative<T>(d.msg) && std::get<T>(d.msg) == msg;
    check(ok, name);
}

void checkCodecRoundTrip() {
    using namespace proto;
    roundtrip(Hello{7}, "Hello");
    roundtrip(Auth{"header.payload.sig"}, "Auth");
    roundtrip(Ping{0xDEADBEEF}, "Ping");
    roundtrip(Pong{42}, "Pong");
    roundtrip(CharListReq{}, "CharListReq");
    roundtrip(CharCreate{"Aragorn"}, "CharCreate");
    roundtrip(CharCreate{"Gandalf", {{"class", "mage"}, {"hair", "grey beard"}}}, "CharCreate with options");
    roundtrip(CharDelete{1234567890123ull}, "CharDelete");
    roundtrip(EnterWorld{99}, "EnterWorld");
    roundtrip(Move{{1.5f, -2.25f, 1000.f}, 3.14f, 123456}, "Move");
    roundtrip(Say{ChatKind::say, "hello \xC3\xA9"}, "Say");
    roundtrip(Say{ChatKind::yell, "HELLO"}, "Say yell");
    roundtrip(Say{ChatKind::emote, "waves"}, "Say emote");
    roundtrip(AuthOk{77, Role::gm}, "AuthOk");
    roundtrip(Disconnect{Code::logged_in_elsewhere}, "Disconnect");
    roundtrip(CharList{{{1, "Alice", {1, 2, 3}}, {2, "Bob", {4, 5, 6}}}}, "CharList");
    roundtrip(CharList{}, "CharList empty");
    roundtrip(CharCreated{{5, "Carol", {7, 8, 9}}}, "CharCreated");
    roundtrip(CharCreated{{6, "Dora", {1, 2, 3}, {{"class", "rogue"}}}}, "CharCreated with options");
    roundtrip(CharList{{{1, "Alice", {1, 2, 3}, {{"class", "priest"}}}, {2, "Bob", {4, 5, 6}}}}, "CharList with options");
    roundtrip(CharDeleted{5}, "CharDeleted");
    roundtrip(EnterWorldOk{11, 2, {1, 0, 1}, 1.f}, "EnterWorldOk");
    roundtrip(MapChange{65535, {-3, 4, 5000}, 2.5f, 0xFFFFFFFE}, "MapChange");
    roundtrip(CharError{Opcode::CharCreate, Code::name_taken}, "CharError");
    roundtrip(Spawn{3, "Dave", {10, 0, 10}, 0.5f, SpawnKind::player, ""}, "Spawn");
    roundtrip(Spawn{4, "Guard", {12, 1, 10}, 1.5f, SpawnKind::npc, "Guard"}, "Spawn npc");
    roundtrip(Despawn{3}, "Despawn");
    roundtrip(EntityMoves{{{3, {11, 0, 12}, 0.25f}, {4, {-1, 2, 3}, 1.5f}}}, "EntityMoves");
    roundtrip(EntityMoves{}, "EntityMoves empty");
    {
        // v9 EntityMoves: u16 count, count x (u32 entityId, Vec3 pos, f32 heading)
        EntityMoves full;
        for (uint32_t i = 0; i < EntityMoves::kMaxEntries; ++i)
            full.entries.push_back({i, {static_cast<float>(i), -1, 2}, 0.5f});
        const auto frame = encode(Message{full});
        const auto d = decode_frame(frame, kMaxPayloadBytes);
        check(frame.size() - kHeaderSize <= kMaxPayloadBytes && d.status == DecodeStatus::ok &&
                  std::get<EntityMoves>(d.msg) == full,
              "a full EntityMoves (kMaxEntries) fits one frame and round-trips");
        check(frame.size() - kHeaderSize + 20 > kMaxPayloadBytes, "kMaxEntries is the most that fits");
        const auto one = encode(Message{EntityMoves{{{3, {11, 0, 12}, 0.25f}}}});
        check(one.size() == kHeaderSize + 2 + 20 && one[4] == 1 && one[5] == 0 && one[6] == 3, "EntityMoves bytes: u16 count, then entries");
        Message out;
        const std::vector<uint8_t> payload(one.begin() + kHeaderSize, one.end());
        check(decode_payload(Opcode::EntityMoves, std::span(payload).first(payload.size() - 1), out) == DecodeStatus::malformed,
              "a cut EntityMoves entry is malformed");
        std::vector<uint8_t> more = payload;
        more[0] = 2; // count 2, one entry
        check(decode_payload(Opcode::EntityMoves, more, out) == DecodeStatus::malformed, "EntityMoves count past the payload is malformed");
        std::vector<uint8_t> trailing = payload;
        trailing[0] = 0; // count 0, an entry's bytes left over
        check(decode_payload(Opcode::EntityMoves, trailing, out) == DecodeStatus::malformed, "EntityMoves bytes past its count are malformed");
        check(decode_payload(Opcode::EntityMoves, std::vector<uint8_t>{1}, out) == DecodeStatus::malformed, "EntityMoves cut count is malformed");
        std::vector<uint8_t> nan = payload;
        nan[2 + 4] = 0; nan[2 + 5] = 0; nan[2 + 6] = 0xC0; nan[2 + 7] = 0x7F; // pos.x NaN
        check(decode_payload(Opcode::EntityMoves, nan, out) == DecodeStatus::malformed, "EntityMoves NaN position is malformed");
    }
    roundtrip(Correction{{1, 2, 3}, 0xFFFFFFFEu, 7}, "Correction");
    roundtrip(Move{{1, 2, 3}, 0.5f, 99, 0xFFFFFFFFu, 3}, "Move");
    roundtrip(ChatMsg{ChatKind::say, "Eve", "hi all"}, "ChatMsg");
    roundtrip(ChatMsg{ChatKind::yell, "Eve", "HI ALL"}, "ChatMsg yell");
    roundtrip(ChatMsg{ChatKind::emote, "Eve", "waves"}, "ChatMsg emote");
    // v8 Say: u8 kind, str text; ChatMsg: u8 kind, str fromName, str text; a kind past emote is malformed
    check(encode(Message{Say{ChatKind::emote, "a"}}) == std::vector<uint8_t>{4, 0, 0x21, 0, 2, 1, 0, 'a'}, "Say bytes: kind first");
    check(encode(Message{ChatMsg{ChatKind::yell, "B", "a"}}) == std::vector<uint8_t>{7, 0, 0x24, 0x01, 1, 1, 0, 'B', 1, 0, 'a'},
          "ChatMsg bytes: kind first");
    Message kindOut;
    check(decode_payload(Opcode::Say, std::vector<uint8_t>{3, 1, 0, 'a'}, kindOut) == DecodeStatus::malformed, "Say kind 3 is malformed");
    check(decode_payload(Opcode::Say, std::vector<uint8_t>{1, 0, 'a'}, kindOut) == DecodeStatus::malformed, "a v7 Say (no kind) is malformed");
    // the chat service's messages and the internal link
    roundtrip(ChatServer{"chat.example.com", 7778}, "ChatServer");
    roundtrip(ChatSend{ChatTarget::whisper, "Bob", "psst"}, "ChatSend whisper");
    roundtrip(ChatSend{ChatTarget::channel, "Trade", "WTS"}, "ChatSend channel");
    roundtrip(ChatSend{ChatTarget::zone, "", "anyone?"}, "ChatSend zone");
    roundtrip(ChatJoin{"Trade"}, "ChatJoin");
    roundtrip(ChatLeave{"LFG"}, "ChatLeave");
    roundtrip(ChatIgnore{"Mallory", true}, "ChatIgnore");
    roundtrip(ChatIgnore{"Mallory", false}, "ChatIgnore off");
    roundtrip(ChatMute{"Mallory", 15}, "ChatMute");
    roundtrip(ChatLine{ChatTarget::whisper_sent, "", "Bob", "psst"}, "ChatLine whisper_sent");
    roundtrip(ChatLine{ChatTarget::channel, "General", "Alice", "hi"}, "ChatLine channel");
    roundtrip(ChatChannel{2, "Trade", true}, "ChatChannel");
    roundtrip(ChatNotice{ChatNoticeCode::muted, "", 14}, "ChatNotice");
    roundtrip(LinkHello{8, "s3cret-shared-value"}, "LinkHello");
    roundtrip(LinkOk{}, "LinkOk");
    roundtrip(CharOnline{7, 1234567890123ull, "Alice", 11}, "CharOnline");
    roundtrip(MapChanged{1234567890123ull, 10}, "MapChanged");
    roundtrip(CharOffline{99}, "CharOffline");
    roundtrip(MuteState{"Mallory", 1790000000000ull}, "MuteState");
    check(decode_payload(Opcode::ChatSend, std::vector<uint8_t>{3, 0, 0, 1, 0, 'a'}, kindOut) == DecodeStatus::malformed,
          "ChatSend target whisper_sent (3) is malformed: only the server echoes");
    check(decode_payload(Opcode::ChatNotice, std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0}, kindOut) == DecodeStatus::malformed &&
              decode_payload(Opcode::ChatNotice, std::vector<uint8_t>{15, 0, 0, 0, 0, 0, 0}, kindOut) == DecodeStatus::malformed,
          "ChatNotice codes 0 and 15 are malformed");
    check(std::string(opcode_name(Opcode::MuteState)) == "MuteState" && std::string(code_name(Code::no_character)) == "no_character" &&
              std::string(chat_notice_name(ChatNoticeCode::you_unmuted)) == "you_unmuted",
          "chat opcode, code and notice names");

    // admin port (docs/world/protocol.html#admin)
    roundtrip(AdminOnlineReq{}, "AdminOnlineReq");
    roundtrip(AdminOnline{}, "AdminOnline empty");
    roundtrip(AdminOnline{true, {{7, 9, "Alice", 2, 3, 1700000000123ull}, {8, 0, "", 0, 4, 1}}}, "AdminOnline");
    roundtrip(AdminKick{7, 0, "spamming"}, "AdminKick by account");
    roundtrip(AdminKick{0, 9, ""}, "AdminKick by character");
    roundtrip(AdminKicked{2}, "AdminKicked");
    check(encode(Message{AdminKicked{0x01020304}}) == std::vector<uint8_t>{4, 0, 0x03, 0x03, 4, 3, 2, 1}, "AdminKicked bytes");
    roundtrip(AdminInstancesReq{}, "AdminInstancesReq");
    roundtrip(AdminInstances{}, "AdminInstances empty");
    roundtrip(AdminInstances{true, {{1, "Start", 0, "", 3, 40, 1700000000123ull, 0}, {6, "Crypt", 77, "party-7", 2, 1, 5, 6}}},
              "AdminInstances");
    check(kAdminProtocolVersion == 10 && kProtocolVersion == 9, "admin protocol 10, game protocol still 9");
    // map u16, name str, instance u32, key str, players u32, npcs u32, createdAtMs u64, emptySinceMs u64
    check(encode(Message{AdminInstances{false, {{6, "C", 0x01020304, "k", 2, 3, 5, 6}}}}) ==
              std::vector<uint8_t>{39, 0, 0x05, 0x03, 0, 1, 0, 6, 0, 1, 0, 'C', 4, 3, 2, 1, 1, 0, 'k', 2, 0, 0, 0, 3, 0, 0, 0,
                                   5, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0},
          "AdminInstances bytes");
    check(std::string(opcode_name(Opcode::AdminInstancesReq)) == "AdminInstancesReq" &&
              std::string(opcode_name(Opcode::AdminInstances)) == "AdminInstances",
          "admin instances opcode names");
    Message adminOut;
    std::vector<uint8_t> kickNone(18, 0), kickBoth(18, 0);
    kickBoth[0] = kickBoth[8] = 1;
    check(decode_payload(Opcode::AdminKick, kickNone, adminOut) == DecodeStatus::malformed &&
              decode_payload(Opcode::AdminKick, kickBoth, adminOut) == DecodeStatus::malformed,
          "AdminKick with neither or both ids is malformed");
    check(decode_payload(Opcode::AdminOnline, std::vector<uint8_t>{2, 0, 0}, adminOut) == DecodeStatus::malformed,
          "AdminOnline more 2 is malformed");
    AdminOnline full;
    full.entries.assign(AdminOnline::kMaxEntries, {~0ull, ~0ull, std::string(16, 'W'), 0xFFFF, ~0u, ~0ull});
    check(encode(Message{full}).size() - kHeaderSize <= kMaxPayloadBytes, "a full AdminOnline chunk fits one frame");
    check(std::string(opcode_name(Opcode::AdminKick)) == "AdminKick" && std::string(code_name(Code::kicked)) == "kicked",
          "admin opcode and kicked code names");

    // exact byte layout: u16 len, u16 opcode, payload, little-endian
    const auto f = encode(Message{Hello{0x0102}});
    check(f == std::vector<uint8_t>{2, 0, 1, 0, 0x02, 0x01}, "Hello bytes little-endian");

    roundtrip(ClientEvent{"combat.power", R"({"power":"strike"})", 0xFFFFFFFEu}, "ClientEvent with requestId");
    roundtrip(Reply{0xFFFFFFFEu, true, R"({"power":"strike"})"}, "Reply ok");
    roundtrip(Reply{3, false, R"("range")"}, "Reply rejected");
    check(kProtocolVersion == 9, "protocol version 9");
    // v7 ClientEvent: str name, str json, u32 requestId (0: chthonic.world.send); Reply: u32 requestId, u8 ok (0/1), str json
    const auto cev = encode(Message{ClientEvent{"a", "{}", 0x01020304}});
    check(cev.size() == 4 + 3 + 4 + 4 && cev[11] == 0x04 && cev[14] == 0x01, "ClientEvent requestId after json");
    const std::vector<uint8_t> badOk{1, 0, 0, 0, 2, 0, 0};
    Message replyOut;
    check(decode_payload(Opcode::Reply, badOk, replyOut) == DecodeStatus::malformed, "a Reply with ok 2 is malformed");
    // v6 Move: vec3 pos, f32 heading, u32 clientTimeMs, u32 seq, u32 epoch; Correction: vec3 pos, u32 ackSeq, u32 epoch
    const auto mv = encode(Message{Move{{0, 0, 0}, 0, 0, 0x01020304, 0x05060708}});
    check(mv.size() == 4 + 12 + 4 + 4 + 4 + 4 && mv[24] == 0x04 && mv[27] == 0x01 && mv[28] == 0x08 && mv[31] == 0x05,
          "Move seq then epoch after clientTimeMs");
    const auto co = encode(Message{Correction{{0, 0, 0}, 0x01020304, 0x05060708}});
    check(co.size() == 4 + 12 + 4 + 4 && co[16] == 0x04 && co[20] == 0x08, "Correction ackSeq then epoch after pos");
    std::vector<uint8_t> v5Move(12 + 4 + 4); // a v5 Move: no seq, no epoch
    Message v5Out;
    check(decode_payload(Opcode::Move, v5Move, v5Out) == DecodeStatus::malformed, "a v5 Move is malformed in v6");
    // v4 CharCreate: str name, u8 count, count x (str key, str value), keys in order; a key twice is malformed
    const auto cc = encode(Message{CharCreate{"Al", {{"b", "2"}, {"a", "1"}}}});
    check(cc == std::vector<uint8_t>{17, 0, 0x11, 0, 2, 0, 'A', 'l', 2, 1, 0, 'a', 1, 0, '1', 1, 0, 'b', 1, 0, '2'},
          "CharCreate options bytes");
    const std::vector<uint8_t> dupe{2, 0, 'A', 'l', 2, 1, 0, 'a', 1, 0, '1', 1, 0, 'a', 1, 0, '2'};
    Message dupeOut;
    check(decode_payload(Opcode::CharCreate, dupe, dupeOut) == DecodeStatus::malformed, "CharCreate with a key twice is malformed");
    const std::vector<uint8_t> shortOpts{2, 0, 'A', 'l', 1, 1, 0, 'a'};
    check(decode_payload(Opcode::CharCreate, shortOpts, dupeOut) == DecodeStatus::malformed, "CharCreate with a cut option is malformed");
    // C++ limits on options (valid_char_options): <= 8 entries, keys 1-16 of [a-z_], values <= 32 printable ASCII
    check(valid_char_options({}) && valid_char_options({{"class", "warrior"}}) &&
              valid_char_options({{"a_b", std::string(32, '~')}, {std::string(16, 'z'), ""}}),
          "options: empty, plain, and at the limits are valid");
    CharOptions nine;
    for (char k = 'a'; k <= 'i'; ++k) nine[std::string(1, k)] = "x";
    check(!valid_char_options(nine), "options: 9 entries refused");
    check(!valid_char_options({{"", "x"}}) && !valid_char_options({{std::string(17, 'a'), "x"}}) &&
              !valid_char_options({{"Class", "x"}}) && !valid_char_options({{"class1", "x"}}) &&
              !valid_char_options({{"a.b", "x"}}),
          "options: empty, long, upper-case, digit and dot keys refused");
    check(!valid_char_options({{"a", std::string(33, 'x')}}) && !valid_char_options({{"a", "tab\t"}}) &&
              !valid_char_options({{"a", "\xC3\xA9"}}),
          "options: long, control and non-ASCII values refused");
    // v3 EnterWorldOk: u32 entityId, u16 map, vec3 pos, f32 heading; v6 MapChange: u16 map, vec3 pos, f32 heading, u32 epoch
    const auto ewo = encode(Message{EnterWorldOk{1, 0x0201, {0, 0, 0}, 0}});
    check(ewo.size() == 4 + 4 + 2 + 12 + 4 && ewo[8] == 0x01 && ewo[9] == 0x02, "EnterWorldOk map follows entityId");
    const auto mc = encode(Message{MapChange{0x0201, {0, 0, 0}, 0}});
    check(mc.size() == 4 + 2 + 12 + 4 + 4 && mc[2] == 0x28 && mc[3] == 0x01 && mc[4] == 0x01 && mc[5] == 0x02,
          "MapChange opcode 0x0128, map first");
    check(std::string(opcode_name(Opcode::MapChange)) == "MapChange", "MapChange name");
    // ClientEvent: opcode 0x0022, str name, str json (ServerEvent's layout), v7 u32 requestId
    const auto ce = encode(Message{ClientEvent{"a.b", "{}"}});
    check(ce == std::vector<uint8_t>{13, 0, 0x22, 0, 3, 0, 'a', '.', 'b', 2, 0, '{', '}', 0, 0, 0, 0}, "ClientEvent bytes");
    check(std::string(opcode_name(Opcode::ClientEvent)) == "ClientEvent", "ClientEvent name");
    // v5 Interact: opcode 0x0023, u32 objectId
    roundtrip(Interact{0xFFFFFFFFu}, "Interact");
    check(encode(Message{Interact{0x01020304}}) == std::vector<uint8_t>{4, 0, 0x23, 0, 4, 3, 2, 1}, "Interact bytes");
    check(std::string(opcode_name(Opcode::Interact)) == "Interact", "Interact name");
    // v2 Spawn: u32 id, str name, vec3, f32 heading, u8 kind, str template
    const auto npc = encode(Message{Spawn{1, "G", {0, 0, 0}, 0, SpawnKind::npc, "T"}});
    check(npc.size() == 4 + 4 + 3 + 12 + 4 + 1 + 3 && npc[4 + 4 + 3 + 16] == 1 && npc.back() == 'T',
          "Spawn kind and template follow the heading");
    auto badKind = npc;
    badKind[4 + 4 + 3 + 16] = 2;
    check(decode_frame(badKind, 4096).status == DecodeStatus::malformed, "Spawn kind 2 is malformed");
    auto v1 = encode(Message{Spawn{1, "G", {0, 0, 0}, 0, SpawnKind::player, ""}});
    v1.resize(v1.size() - 3); // a v1 Spawn: no kind, no template
    v1[0] = static_cast<uint8_t>(v1.size() - 4);
    check(decode_frame(v1, 4096).status == DecodeStatus::malformed, "a v1 Spawn is malformed in v2");

    // two frames back to back decode one at a time
    auto two = encode(Message{Ping{1}});
    encode(two, Message{Pong{2}});
    const auto d1 = decode_frame(two, 4096);
    check(d1.status == DecodeStatus::ok && std::get<Ping>(d1.msg).nonce == 1, "first of two frames");
    const auto d2 = decode_frame(std::span(two).subspan(d1.consumed), 4096);
    check(d2.status == DecodeStatus::ok && std::get<Pong>(d2.msg).nonce == 2, "second of two frames");
}

void checkFrameRejection() {
    using namespace proto;
    // oversized: header alone is enough to reject, before the payload arrives
    std::vector<uint8_t> big = {0x01, 0x10, 0x21, 0x00}; // len 4097, Say
    check(decode_frame(big, 4096).status == DecodeStatus::too_large, "oversized frame rejected from header");
    // truncated: header promises more than arrived
    auto f = encode(Message{Say{ChatKind::say, "hello"}});
    f.pop_back();
    check(decode_frame(f, 4096).status == DecodeStatus::need_more, "partial frame waits for more");
    check(decode_frame(std::span(f).first(3), 4096).status == DecodeStatus::need_more, "partial header waits");
    // truncated payload inside a complete frame: string length longer than payload
    std::vector<uint8_t> lie = {4, 0, 0x21, 0x00, 10, 0, 'a', 'b'}; // Say claims 10 chars, has 2
    check(decode_frame(lie, 4096).status == DecodeStatus::malformed, "string past payload end is malformed");
    // trailing bytes
    std::vector<uint8_t> extra = {3, 0, 0x01, 0x00, 1, 0, 9};
    check(decode_frame(extra, 4096).status == DecodeStatus::malformed, "trailing bytes are malformed");
    // short fixed field
    std::vector<uint8_t> shortHello = {1, 0, 0x01, 0x00, 1};
    check(decode_frame(shortHello, 4096).status == DecodeStatus::malformed, "short Hello is malformed");
    // unknown opcode
    std::vector<uint8_t> unk = {0, 0, 0xFF, 0x7F};
    check(decode_frame(unk, 4096).status == DecodeStatus::unknown_opcode, "unknown opcode");
    // NaN coordinate
    auto mv = encode(Message{Move{{1, 2, 3}, 0, 0}});
    mv[4] = 0x00; mv[5] = 0x00; mv[6] = 0xC0; mv[7] = 0x7F; // x = NaN
    check(decode_frame(mv, 4096).status == DecodeStatus::malformed, "NaN coordinate is malformed");
    // bad enum
    std::vector<uint8_t> role = {9, 0, 0x02, 0x01, 1, 0, 0, 0, 0, 0, 0, 0, 9};
    check(decode_frame(role, 4096).status == DecodeStatus::malformed, "unknown role is malformed");
}

// --- rules ---

void checkNames() {
    check(valid_character_name("Bob"), "3 letters ok");
    check(valid_character_name("AbcdefghijklmnoP"), "16 letters ok");
    check(!valid_character_name("Al"), "2 letters rejected");
    check(!valid_character_name("Abcdefghijklmnopq"), "17 letters rejected");
    check(!valid_character_name("Bob1"), "digit rejected");
    check(!valid_character_name("Bo b"), "space rejected");
    check(!valid_character_name("B\xC3\xA9o"), "non-ASCII rejected");
    check(!valid_character_name(""), "empty rejected");
    check(!valid_character_name(std::string("Bo\0b", 4)), "NUL rejected");
}

void checkMovement() {
    MoveRules rules; // 10 m/s + 1 tolerance, 1 s burst, y [-100, 500]; map bounds: lua_checks checkMaps
    MoveBudget b{0, T0};
    proto::Vec3 from{100, 0, 100};

    proto::Vec3 to{101, 0, 100};
    check(validate_move(rules, b, from, to, 0, T0 + 100ms) == MoveVerdict::accepted, "1 m in 100 ms accepted");
    from = to;
    to = {102.5f, 0, 100}; // budget: 0.1 left + 1.1 refill = 1.2 m
    check(validate_move(rules, b, from, to, 0, T0 + 200ms) == MoveVerdict::too_fast, "1.5 m in 100 ms rejected");
    // within tolerance: 10.5 m/s sustained for 10 moves
    MoveBudget b2{0, T0};
    proto::Vec3 p{500, 0, 500};
    bool allOk = true;
    for (int i = 1; i <= 10; ++i) {
        proto::Vec3 n{p.x + 1.05f, 0, p.z};
        allOk &= validate_move(rules, b2, p, n, 0, T0 + std::chrono::milliseconds(100 * i)) == MoveVerdict::accepted;
        p = n;
    }
    check(allOk, "10.5 m/s accepted within tolerance");
    // jitter: two moves arrive in the same instant after a pause are absorbed by the burst budget
    MoveBudget b3{0, T0};
    proto::Vec3 q{500, 0, 500}, q1{501, 0, 500}, q2{502, 0, 500};
    check(validate_move(rules, b3, q, q1, 0, T0 + 200ms) == MoveVerdict::accepted, "jitter move 1");
    check(validate_move(rules, b3, q1, q2, 0, T0 + 200ms) == MoveVerdict::accepted, "jitter move 2 same instant");
    // sustained speedhack: 30 m/s fails within the first second
    MoveBudget b4{0, T0};
    proto::Vec3 s{500, 0, 500};
    bool rejected = false;
    for (int i = 1; i <= 10 && !rejected; ++i) {
        proto::Vec3 n{s.x + 3, 0, s.z};
        if (validate_move(rules, b4, s, n, 0, T0 + std::chrono::milliseconds(100 * i)) == MoveVerdict::too_fast) rejected = true;
        else s = n;
    }
    check(rejected, "30 m/s rejected");
    // teleport
    MoveBudget b5{0, T0};
    proto::Vec3 tp{1000, 0, 1000};
    check(validate_move(rules, b5, {0, 0, 0}, tp, 0, T0 + 10s) == MoveVerdict::too_fast, "teleport rejected even after idle");
    // y clamp, not counted in speed (falling from 9000 m)
    MoveBudget b7{0, T0};
    proto::Vec3 high{10, 8000, 10};
    check(validate_move(rules, b7, {10, 9000, 10}, high, 0, T0 + 1s) == MoveVerdict::accepted && high.y == 500,
          "y clamped to MAX_Y, vertical not counted as speed");

    // ground at 10 m: groundTolerance 0.5, maxAir 6
    const float g = 10;
    MoveBudget b8{0, T0};
    proto::Vec3 at{10, g, 10}, under{11, g - 3, 10};
    check(validate_move(rules, b8, at, under, g, T0 + 1s) == MoveVerdict::accepted && under.y == g,
          "3 m under the ground: accepted, lifted onto it");
    proto::Vec3 slightly{12, g - 0.4f, 10};
    check(validate_move(rules, b8, under, slightly, g, T0 + 1s) == MoveVerdict::accepted && slightly.y == g - 0.4f,
          "0.4 m under (within groundTolerance): accepted as-is");
    proto::Vec3 edge{13, g + 6, 10};
    check(validate_move(rules, b8, slightly, edge, g, T0 + 2s) == MoveVerdict::accepted && edge.y == g + 6,
          "rising to exactly ground + maxAir: accepted");
    MoveBudget b9{0, T0};
    proto::Vec3 fly{11, g + 6.5f, 10};
    check(validate_move(rules, b9, at, fly, g, T0 + 1s) == MoveVerdict::in_air && fly.y == g + 6.5f,
          "rising past ground + maxAir: in_air");
    check(b9.meters > 10.9f, "in_air spends no budget");
    proto::Vec3 hoverFrom{10, g + 20, 10}, hover{11, g + 20, 10};
    check(validate_move(rules, b9, hoverFrom, hover, g, T0 + 1s) == MoveVerdict::in_air, "level flight 20 m up: in_air");
    proto::Vec3 sky{10, g + 50, 10}, falling{11, g + 49, 10};
    check(validate_move(rules, b9, sky, falling, g, T0 + 1s) == MoveVerdict::accepted && falling.y == g + 49,
          "falling from 50 m above the ground: accepted");
    proto::Vec3 down{12, g + 20, 10};
    check(validate_move(rules, b9, falling, down, g, T0 + 2s) == MoveVerdict::accepted, "still falling: accepted");
}

void checkAoi() {
    AoiGrid grid(64, 128);
    std::map<uint32_t, std::pair<float, float>> pos = {{1, {100.f, 100.f}}, {2, {200.f, 100.f}}, {3, {100.f, 229.f}}, {4, {500.f, 500.f}}};
    for (auto& [id, p] : pos) grid.insert(id, grid.cell_of(1, p.first, p.second));
    auto posOf = [&](uint32_t id, float& x, float& z) { x = pos[id].first; z = pos[id].second; };
    const auto query = [&](uint16_t map, float x, float z, uint32_t self) {
        std::vector<uint32_t> out{99}; // stale contents are cleared
        grid.query(map, x, z, self, posOf, out);
        return out;
    };

    auto seen = query(1, 100, 100, 1);
    check(seen == std::vector<uint32_t>{2}, "1 sees 2 (100 m) not 3 (129 m) or 4");
    check(query(1, 200, 100, 2) == std::vector<uint32_t>{1}, "visibility is symmetric");

    // touched cells: inserts touched; cleared, nothing is near; a touch is seen by every window over that cell only
    check(grid.touched_near(1, 100, 100), "inserts touch their cells");
    grid.clear_touched();
    check(!grid.touched_near(1, 100, 100), "nothing touched after clear_touched");
    grid.touch(grid.cell_of(1, 200, 100));
    check(grid.touched_near(1, 100, 100) && !grid.touched_near(1, 500, 500) && !grid.touched_near(2, 200, 100),
          "a touched cell is near windows over it only, on its own map");
    grid.clear_touched();

    // 3 walks into range, 2 walks out
    grid.remove(3, grid.cell_of(1, 100, 229));
    pos[3] = {100.f, 220.f};
    grid.insert(3, grid.cell_of(1, 100, 220));
    grid.remove(2, grid.cell_of(1, 200, 100));
    pos[2] = {300.f, 100.f};
    grid.insert(2, grid.cell_of(1, 300, 100));
    check(grid.touched_near(1, 100, 100), "removes and inserts touch");
    auto now = query(1, 100, 100, 1);
    std::vector<uint32_t> entered, left;
    diff_sorted(seen, now, entered, left);
    check(entered == std::vector<uint32_t>{3}, "3 entered");
    check(left == std::vector<uint32_t>{2}, "2 left");
    check(query(1, 1900, 1900, 0).empty(), "empty corner");
    // maps: same x/z on another map is another cell; negative cells and big map ids keep apart
    pos[5] = {100.f, 100.f};
    grid.insert(5, grid.cell_of(2, 100, 100));
    check(query(1, 100, 100, 1) == std::vector<uint32_t>{3}, "map 1 never sees map 2's entity at the same x/z");
    check(query(2, 100, 100, 0) == std::vector<uint32_t>{5}, "map 2 sees only its own");
    pos[6] = {-100.f, -100.f};
    grid.insert(6, grid.cell_of(65535, -100, -100));
    check(query(65535, -100, -100, 0) == std::vector<uint32_t>{6} && query(0, -100, -100, 0).empty(),
          "negative cells and map 65535 keyed apart");
}

// --- AOI differential check: aoi_system (skips players with no touched cell near) against the full recompute ---
namespace aoi_diff {
using namespace chthonic::world;

// The aoi_system before touched cells: every player's view recomputed on every pass (kept here as the reference).
void reference_aoi(entt::registry& reg, AoiGrid& grid, const Send& send) {
    aoi_rebucket(reg, grid);
    auto posOf = [&](uint32_t id, float& x, float& z) {
        const auto& t = reg.get<Transform>(entt::entity{id});
        x = t.pos.x;
        z = t.pos.z;
    };
    std::vector<uint32_t> entered, left;
    for (auto [e, tf, mr, vis, link] : reg.view<Transform, MapRef, Visible, SessionLink>().each()) {
        std::vector<uint32_t> now;
        grid.query(mr.map, tf.pos.x, tf.pos.z, wire_id(e), posOf, now);
        diff_sorted(vis.ids, now, entered, left);
        for (uint32_t id : entered) {
            const auto other = entt::entity{id};
            if (auto* viewers = reg.try_get<Viewers>(other)) insert_sorted(viewers->ids, wire_id(e));
            send(link.sessionId, spawn_message(reg, other));
        }
        for (uint32_t id : left) {
            if (auto* viewers = reg.try_get<Viewers>(entt::entity{id})) erase_sorted(viewers->ids, wire_id(e));
            send(link.sessionId, proto::Despawn{id});
        }
        vis.ids = std::move(now);
        vis.entered = entered;
    }
    grid.clear_touched(); // unused by the reference; keeps the list from growing
}

constexpr uint16_t kMaps = 3;
constexpr float kSide = 640; // 10 x 10 cells per map: views overlap a lot, some corners stay empty

// A registry and grid driven like World (spawn, despawn, transfer, create / destroy NPC and leave_views follow
// world.cpp), recording every message each session gets in a tick as its encoded bytes.
struct Sim {
    entt::registry reg;
    AoiGrid grid{64, 128};
    std::map<uint32_t, std::vector<std::vector<uint8_t>>> sent;
    Send send = [this](uint32_t session, const proto::Message& m) { sent[session].push_back(proto::encode(m)); };
    uint32_t nextSession = 1;

    entt::entity place(uint16_t map, proto::Vec3 pos) {
        const auto e = reg.create();
        reg.emplace<Transform>(e, pos, 0.f);
        reg.emplace<MapRef>(e, map);
        reg.emplace<AoiCell>(e, grid.cell_of(map, pos.x, pos.z));
        grid.insert(wire_id(e), grid.cell_of(map, pos.x, pos.z));
        return e;
    }
    void spawn_player(uint16_t map, proto::Vec3 pos) {
        const auto e = place(map, pos);
        reg.emplace<SessionLink>(e, nextSession++);
        reg.emplace<CharacterRef>(e, uint64_t(wire_id(e)), "p" + std::to_string(wire_id(e)));
        reg.emplace<Visible>(e);
    }
    void create_npc(uint16_t map, proto::Vec3 pos) {
        const auto e = place(map, pos);
        reg.emplace<Npc>(e, "n" + std::to_string(wire_id(e)), "goblin", "");
        reg.emplace<Viewers>(e);
    }
    void destroy(entt::entity e) { // World::despawn / destroy_npc
        leave_views(e);
        grid.remove(wire_id(e), reg.get<AoiCell>(e).cell);
        reg.destroy(e);
    }
    void move(entt::entity e, proto::Vec3 pos) { // movement_system / a Lua Transform set
        reg.get<Transform>(e).pos = pos;
        reg.emplace_or_replace<Moved>(e);
    }
    void transfer(entt::entity e, uint16_t map, proto::Vec3 pos) {
        reg.get<MapRef>(e).map = map;
        reg.get<Transform>(e) = {pos, 0.f};
        auto& cell = reg.get<AoiCell>(e);
        grid.remove(wire_id(e), cell.cell);
        cell.cell = grid.cell_of(map, pos.x, pos.z);
        grid.insert(wire_id(e), cell.cell);
        reg.remove<Moved>(e);
        if (const auto* link = reg.try_get<SessionLink>(e)) {
            send(link->sessionId, proto::MapChange{map, pos, 0.f, 0});
            for (uint32_t other : reg.get<Visible>(e).ids) send(link->sessionId, proto::Despawn{other});
        }
        leave_views(e);
    }
    void leave_views(entt::entity e) {
        const uint32_t id = wire_id(e);
        const auto unsee = [&](uint32_t viewer) {
            auto& vis = reg.get<Visible>(entt::entity{viewer});
            erase_sorted(vis.ids, id);
            erase_sorted(vis.entered, id);
            send(reg.get<SessionLink>(entt::entity{viewer}).sessionId, proto::Despawn{id});
        };
        if (auto* viewers = reg.try_get<Viewers>(e)) {
            for (uint32_t viewer : viewers->ids) unsee(viewer);
            viewers->ids.clear();
            return;
        }
        auto& vis = reg.get<Visible>(e);
        for (uint32_t other : vis.ids) {
            if (auto* viewers = reg.try_get<Viewers>(entt::entity{other})) erase_sorted(viewers->ids, id);
            else unsee(other);
        }
        vis.ids.clear();
        vis.entered.clear();
    }
    template <typename Tag>
    std::vector<entt::entity> all() const {
        auto v = reg.view<Tag>();
        std::vector<entt::entity> out(v.begin(), v.end());
        std::sort(out.begin(), out.end());
        return out;
    }
};

struct Counts {
    int idleTicks = 0, withinCell = 0, crossCell = 0, luaMoves = 0, creates = 0, destroys = 0, transfers = 0,
        luaTransfers = 0, spawns = 0, despawns = 0, spawnMsgs = 0, despawnMsgs = 0;
};

// One tick in World::tick order; every random choice comes from rng (both sims draw the same sequence).
void tick(Sim& s, std::mt19937& rng, bool incremental, Counts& n) {
    std::uniform_real_distribution<float> u(0, 1), spot(0, kSide);
    const auto chance = [&](float p) { return u(rng) < p; };
    const auto map = [&] { return uint16_t(1 + rng() % kMaps); };
    const auto pick = [&](const std::vector<entt::entity>& v) { return v[rng() % v.size()]; };
    // A short step (mostly within its cell) or a long one across cells.
    const auto step = [&](entt::entity e, bool& within) {
        const auto from = s.reg.get<Transform>(e).pos;
        std::uniform_real_distribution<float> d(chance(0.6f) ? -2.f : -120.f, 0);
        const float sx = d(rng) * (chance(0.5f) ? -1 : 1), sz = d(rng) * (chance(0.5f) ? -1 : 1);
        const proto::Vec3 to{std::clamp(from.x + sx, 0.f, kSide), 0, std::clamp(from.z + sz, 0.f, kSide)};
        within = s.grid.cell_of(1, to.x, to.z) == s.grid.cell_of(1, from.x, from.z);
        s.move(e, to);
    };
    const auto aoi = [&] { incremental ? aoi_system(s.reg, s.grid, s.send) : reference_aoi(s.reg, s.grid, s.send); };

    if (chance(0.3f)) { // an idle tick: nothing joins, leaves or moves
        ++n.idleTicks;
        aoi();
        aoi_rebucket(s.reg, s.grid);
        end_of_tick_system(s.reg);
        return;
    }
    // between ticks: logins and logouts
    if (s.all<SessionLink>().size() < 40 && chance(0.3f)) {
        s.spawn_player(map(), {spot(rng), 0, spot(rng)});
        ++n.spawns;
    }
    if (auto players = s.all<SessionLink>(); players.size() > 5 && chance(0.15f)) {
        s.destroy(pick(players));
        ++n.despawns;
    }
    // movement_system
    for (auto e : s.all<SessionLink>())
        if (chance(0.25f)) {
            bool within = false;
            step(e, within);
            ++(within ? n.withinCell : n.crossCell);
        }
    // area_system portals: to another map, or a teleport on the same one
    if (auto players = s.all<SessionLink>(); !players.empty() && chance(0.2f)) {
        s.transfer(pick(players), map(), {spot(rng), 0, spot(rng)});
        ++n.transfers;
    }
    aoi();
    // Lua update, after the aoi pass: NPC moves (and the odd player move), chthonic.create / destroy / transfer
    bool within = false;
    for (auto e : s.all<Npc>())
        if (chance(0.3f)) {
            step(e, within);
            ++n.luaMoves;
        }
    if (auto players = s.all<SessionLink>(); !players.empty() && chance(0.1f)) {
        step(pick(players), within);
        ++n.luaMoves;
    }
    if (s.all<Npc>().size() < 40 && chance(0.4f)) {
        s.create_npc(map(), {spot(rng), 0, spot(rng)});
        ++n.creates;
    }
    if (auto npcs = s.all<Npc>(); npcs.size() > 5 && chance(0.25f)) {
        s.destroy(pick(npcs));
        ++n.destroys;
    }
    if (auto all = s.all<Transform>(); !all.empty() && chance(0.15f)) {
        s.transfer(pick(all), map(), {spot(rng), 0, spot(rng)});
        ++n.luaTransfers;
    }
    aoi_rebucket(s.reg, s.grid);
    end_of_tick_system(s.reg);
}

std::string id_list(const std::vector<uint32_t>& ids) {
    std::string out;
    for (uint32_t id : ids) out += (out.empty() ? "" : " ") + std::to_string(id);
    return "[" + out + "]";
}

// The first difference between the two sims after a tick, or "" if every message and view agrees.
std::string mismatch(const Sim& ref, const Sim& inc) {
    for (const auto& [session, msgs] : ref.sent) {
        const auto it = inc.sent.find(session);
        if (it == inc.sent.end() || it->second != msgs)
            return "session " + std::to_string(session) + " messages differ (" + std::to_string(msgs.size()) +
                   " from the reference, " + std::to_string(it == inc.sent.end() ? 0 : it->second.size()) +
                   " from aoi_system)";
    }
    if (ref.sent.size() != inc.sent.size()) return "aoi_system sent to a session the reference did not";
    if (ref.reg.view<Visible>().size() != inc.reg.view<Visible>().size() ||
        ref.reg.view<Viewers>().size() != inc.reg.view<Viewers>().size())
        return "different entities";
    for (auto [e, vis] : ref.reg.view<Visible>().each()) {
        const auto* other = inc.reg.try_get<Visible>(e);
        const std::string who = "player " + std::to_string(wire_id(e));
        if (!other) return who + " missing";
        if (other->ids != vis.ids)
            return who + " Visible.ids " + id_list(vis.ids) + " (reference) vs " + id_list(other->ids);
        if (other->entered != vis.entered)
            return who + " Visible.entered " + id_list(vis.entered) + " (reference) vs " + id_list(other->entered);
    }
    for (auto [e, viewers] : ref.reg.view<Viewers>().each()) {
        const auto* other = inc.reg.try_get<Viewers>(e);
        if (!other || other->ids != viewers.ids)
            return "npc " + std::to_string(wire_id(e)) + " Viewers.ids " + id_list(viewers.ids) + " (reference) vs " +
                   (other ? id_list(other->ids) : "missing");
    }
    return "";
}
} // namespace aoi_diff

// Seeded random worlds (players and NPCs on several maps; within-cell and long moves, logins / logouts, NPC create /
// destroy, transfers before and after the aoi pass, Lua moves after it, idle ticks) run through the reference full
// recompute and through aoi_system: after every tick each session's messages (encoded, in order), every Visible and
// every Viewers must be identical.
void checkAoiDifferential() {
    using namespace aoi_diff;
    constexpr int kSeeds = 20, kTicks = 400;
    Counts n;
    int compared = 0;
    std::string why;
    for (int seed = 1; seed <= kSeeds && why.empty(); ++seed) {
        Sim ref, inc;
        std::mt19937 rngRef(seed), rngInc(seed);
        for (uint16_t i = 0; i < 20; ++i) { // the starting population
            const proto::Vec3 p{float(i * 31 % 640), 0, float(i * 97 % 640)};
            for (Sim* s : {&ref, &inc}) {
                s->spawn_player(uint16_t(1 + i % kMaps), p);
                s->create_npc(uint16_t(1 + i % kMaps), {p.z, 0, p.x});
            }
        }
        for (int t = 0; t < kTicks && why.empty(); ++t, ++compared) {
            ref.sent.clear();
            inc.sent.clear();
            Counts ignored;
            tick(ref, rngRef, false, n);
            tick(inc, rngInc, true, ignored);
            for (const auto& [session, msgs] : ref.sent)
                for (const auto& m : msgs) {
                    const auto op = proto::Opcode(m[2] | m[3] << 8);
                    n.spawnMsgs += op == proto::Opcode::Spawn;
                    n.despawnMsgs += op == proto::Opcode::Despawn;
                }
            if (why = mismatch(ref, inc); !why.empty())
                std::fprintf(stderr, "aoi differential: seed %d tick %d: %s\n", seed, t, why.c_str());
        }
    }
    check(why.empty(), "aoi differential: aoi_system matches the full recompute on every tick");
    std::printf("aoi differential: %d ticks compared; idle %d, within-cell moves %d, cross-cell %d, Lua moves %d, NPC "
                "creates %d / destroys %d, transfers %d before aoi + %d after, logins %d / logouts %d; %d Spawn, %d "
                "Despawn\n",
                compared, n.idleTicks, n.withinCell, n.crossCell, n.luaMoves, n.creates, n.destroys, n.transfers,
                n.luaTransfers, n.spawns, n.despawns, n.spawnMsgs, n.despawnMsgs);
    check(n.idleTicks > 100 && n.withinCell > 100 && n.crossCell > 100 && n.luaMoves > 100 && n.creates > 100 &&
              n.destroys > 100 && n.transfers > 100 && n.luaTransfers > 100 && n.spawns > 100 && n.despawns > 100,
          "aoi differential: every event kind ran many times");
}

void checkChat() {
    check(sanitize_chat("hello", 256) == std::optional<std::string>("hello"), "plain text");
    check(sanitize_chat("a\nb\tc\x7f" "d", 256) == std::optional<std::string>("abcd"), "C0/DEL stripped");
    check(sanitize_chat("x\xC2\x85y", 256) == std::optional<std::string>("xy"), "C1 (U+0085) stripped");
    check(sanitize_chat("caf\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x98\x80", 256).has_value(), "valid multibyte kept");
    check(!sanitize_chat("bad \xC3", 256), "truncated sequence rejected");
    check(!sanitize_chat("bad \xC0\xAF", 256), "overlong rejected");
    check(!sanitize_chat("bad \xED\xA0\x80", 256), "surrogate rejected");
    check(!sanitize_chat("bad \xFF", 256), "invalid byte rejected");
    check(!sanitize_chat("\n\t", 256), "only control chars rejected");
    check(!sanitize_chat("", 256), "empty rejected");
    check(sanitize_chat(std::string(256, 'a'), 256).has_value(), "256 bytes ok");
    check(!sanitize_chat(std::string(257, 'a'), 256), "257 bytes rejected");

    WindowLimiter chat(5, 10s);
    int allowed = 0;
    for (int i = 0; i < 7; ++i) allowed += chat.allow(T0 + std::chrono::milliseconds(i * 100));
    check(allowed == 5, "5 per 10 s");
    check(!chat.allow(T0 + 9s), "still limited at 9 s");
    check(chat.allow(T0 + 10s), "first slot frees at 10 s");
}

// ClientEvent (chthonic.world.send): the name and payload checks GameServer applies before World sees the event.
void checkClientEvents() {
    using proto::ClientEvent;
    check(proto::valid_client_event_name("combat.attack_2"), "event name a-z 0-9 . _ ok");
    check(proto::valid_client_event_name(std::string(32, 'a')), "32-char event name ok");
    check(!proto::valid_client_event_name(std::string(33, 'a')), "33-char event name rejected");
    check(!proto::valid_client_event_name(""), "empty event name rejected");
    for (const char* bad : {"Combat", "a b", "a-b", "a/b", "\xC3\xA9", "a\n"})
        check(!proto::valid_client_event_name(bad), "event name rejected");

    const auto data = client_event_data(ClientEvent{"combat.attack", R"({"target":12})"});
    check(data && (*data)["target"] == 12, "valid ClientEvent -> its JSON object");
    check(client_event_data(ClientEvent{"x", "{}"}).has_value(), "empty object ok");
    check(!client_event_data(ClientEvent{"Bad", "{}"}), "bad name rejected");
    check(!client_event_data(ClientEvent{"x", "[1,2]"}), "array payload rejected");
    check(!client_event_data(ClientEvent{"x", "5"}), "number payload rejected");
    check(!client_event_data(ClientEvent{"x", "{not json"}), "invalid JSON rejected");
    check(!client_event_data(ClientEvent{"x", ""}), "empty payload rejected");
    const std::string atLimit = R"({"s":")" + std::string(proto::kClientEventJsonMax - 8, 'a') + R"("})";
    check(atLimit.size() == 1024 && client_event_data(ClientEvent{"x", atLimit}).has_value(), "1024-byte payload ok");
    const std::string over = R"({"s":")" + std::string(proto::kClientEventJsonMax - 7, 'a') + R"("})";
    check(!client_event_data(ClientEvent{"x", over}), "1025-byte payload rejected");
}

void checkMessageRate() {
    MessageRateLimiter rl(60, 120, 2000ms);
    int accepted = 0;
    for (int i = 0; i < 120; ++i) accepted += rl.check(T0) == MessageRateLimiter::Verdict::accept;
    check(accepted == 120, "burst of 120 accepted");
    check(rl.check(T0) == MessageRateLimiter::Verdict::drop, "121st dropped");
    check(rl.check(T0 + 1999ms) != MessageRateLimiter::Verdict::disconnect, "brief excess tolerated");
    // flood continues: bucket refills 60/s but sender exceeds it; 2 s of continuous excess disconnects
    MessageRateLimiter flood(60, 120, 2000ms);
    MessageRateLimiter::Verdict v = MessageRateLimiter::Verdict::accept;
    int ms = 0;
    for (; ms < 10000 && v != MessageRateLimiter::Verdict::disconnect; ++ms) v = flood.check(T0 + std::chrono::milliseconds(ms));
    check(v == MessageRateLimiter::Verdict::disconnect, "1000 msg/s flood disconnects");
    // steady 50 msg/s never disconnects
    MessageRateLimiter steady(60, 120, 2000ms);
    bool anyBad = false;
    for (int i = 0; i < 500; ++i) anyBad |= steady.check(T0 + std::chrono::milliseconds(i * 20)) != MessageRateLimiter::Verdict::accept;
    check(!anyBad, "50 msg/s always accepted");
}

void checkConfig() {
    std::map<std::string, std::string> env = {{"AUTH_JWKS_URL", "http://auth:3000/.well-known/jwks.json"},
                                              {"DB_HOST", "mysql"}, {"DB_USER", "u"}, {"DB_PASSWORD", "p"},
                                              {"DB_NAME", "chthonic_world"}, {"REALM_ID", "1"}};
    auto lookup = [&](const char* n) -> std::optional<std::string> {
        auto it = env.find(n);
        return it == env.end() ? std::nullopt : std::optional(it->second);
    };
    auto ok = load_config(lookup);
    check(ok.errors.empty(), "minimal config valid");
    check(ok.config.mapRoot == "/app/maps" && ok.config.startMap == 1 && ok.config.port == 7777, "defaults applied");
    for (const char* bad : {"48", "100", "0.5"}) {
        env["AOI_CELL_SIZE"] = bad;
        const auto r = load_config(lookup);
        check(r.errors.size() == 1 && r.errors[0].starts_with("AOI_CELL_SIZE"), "AOI_CELL_SIZE must divide 256");
    }
    env["AOI_CELL_SIZE"] = "128";
    check(load_config(lookup).errors.empty(), "AOI_CELL_SIZE 128 divides 256");
    env.erase("AOI_CELL_SIZE");
    check(ok.config.activationTiles == 1, "ACTIVATION_TILES defaults to 1");
    for (const char* bad : {"0", "-1", "abc", "1.5", "33"}) {
        env["ACTIVATION_TILES"] = bad;
        const auto r = load_config(lookup);
        check(r.errors.size() == 1 && r.errors[0] == std::string("ACTIVATION_TILES: must be a number in [1, 32], got \"") + bad + "\"",
              (std::string("ACTIVATION_TILES ") + bad + " refused with a clear error").c_str());
    }
    env["ACTIVATION_TILES"] = "2";
    check(load_config(lookup).errors.empty() && load_config(lookup).config.activationTiles == 2, "ACTIVATION_TILES 2 read");
    env.erase("ACTIVATION_TILES");
    check(ok.config.mapThreads == 1, "MAP_THREADS defaults to 1");
    for (const char* bad : {"0", "-1", "-4", "abc", "1.5", "", " 4", "257"}) {
        env["MAP_THREADS"] = bad;
        const auto r = load_config(lookup);
        const bool refused = r.errors.size() == 1 && r.errors[0] == std::string("MAP_THREADS: must be a number in [1, 256], got \"") + bad + "\"";
        check(*bad == 0 ? r.errors.empty() && r.config.mapThreads == 1 : refused,
              (std::string("MAP_THREADS \"") + bad + "\" refused with a clear error (empty: the default)").c_str());
    }
    env["MAP_THREADS"] = "4";
    check(load_config(lookup).errors.empty() && load_config(lookup).config.mapThreads == 4, "MAP_THREADS 4 read");
    env.erase("MAP_THREADS");
    check(ok.config.netThreads == 4, "NET_THREADS defaults to 4");
    for (const char* bad : {"0", "65", "abc"}) {
        env["NET_THREADS"] = bad;
        const auto r = load_config(lookup);
        check(r.errors.size() == 1 && r.errors[0] == std::string("NET_THREADS: must be a number in [1, 64], got \"") + bad + "\"",
              (std::string("NET_THREADS ") + bad + " refused with a clear error").c_str());
    }
    env["NET_THREADS"] = "1";
    check(load_config(lookup).errors.empty() && load_config(lookup).config.netThreads == 1, "NET_THREADS 1 read");
    env.erase("NET_THREADS");
    check(ok.config.dbPoolSize == 4, "DB_POOL_SIZE defaults to 4");
    for (const char* bad : {"0", "33", "abc"}) {
        env["DB_POOL_SIZE"] = bad;
        check(load_config(lookup).errors.size() == 1, (std::string("DB_POOL_SIZE ") + bad + " refused").c_str());
    }
    env["DB_POOL_SIZE"] = "1";
    check(load_config(lookup).errors.empty() && load_config(lookup).config.dbPoolSize == 1, "DB_POOL_SIZE 1 read");
    env.erase("DB_POOL_SIZE");
    env["START_MAP"] = "65536";
    check(load_config(lookup).errors.size() == 1, "START_MAP above 65535 refused");
    env["START_MAP"] = "2";
    env["MAP_ROOT"] = "/maps";
    check(load_config(lookup).config.startMap == 2 && load_config(lookup).config.mapRoot == "/maps", "START_MAP/MAP_ROOT read");
    env.erase("START_MAP");
    env.erase("MAP_ROOT");
    check(ok.config.gameStateMachines.empty(), "GAME_STATE_MACHINES unset: no game machines");
    env["GAME_STATE_MACHINES"] = " zone_cycle ,, quest_flow,\t";
    check(load_config(lookup).config.gameStateMachines == std::vector<std::string>{"zone_cycle", "quest_flow"},
          "GAME_STATE_MACHINES: comma list, trimmed, empty entries ignored");
    env["GAME_STATE_MACHINES"] = "";
    check(load_config(lookup).config.gameStateMachines.empty(), "GAME_STATE_MACHINES empty: none");
    check(ok.config.groundTolerance == 0.5f && ok.config.maxAir == 6, "MOVE_GROUND_TOLERANCE / MOVE_MAX_AIR default 0.5 / 6");
    env["MOVE_GROUND_TOLERANCE"] = "1";
    env["MOVE_MAX_AIR"] = "2.5";
    const auto ground = load_config(lookup);
    check(ground.errors.empty() && ground.config.groundTolerance == 1 && ground.config.maxAir == 2.5f,
          "MOVE_GROUND_TOLERANCE / MOVE_MAX_AIR read");
    env["MOVE_MAX_AIR"] = "-1";
    const auto negative = load_config(lookup);
    check(negative.errors.size() == 1 && negative.errors[0].starts_with("MOVE_MAX_AIR"), "negative MOVE_MAX_AIR refused");
    env.erase("MOVE_GROUND_TOLERANCE");
    env.erase("MOVE_MAX_AIR");
    check(ok.config.maxNpcs == 10000, "MAX_NPCS defaults to 10000");
    env["MAX_NPCS"] = "0";
    check(load_config(lookup).errors.empty() && load_config(lookup).config.maxNpcs == 0, "MAX_NPCS 0: no NPCs");
    for (const char* badNpcs : {"-1", "100001", "many", "1.5"}) {
        env["MAX_NPCS"] = badNpcs;
        const auto r = load_config(lookup);
        check(r.errors.size() == 1 && r.errors[0].starts_with("MAX_NPCS"), "MAX_NPCS out of range or not a whole number refused");
    }

    env.erase("MAX_NPCS");
    check(ok.config.simLatencyMs == 0 && ok.config.simJitterMs == 0, "SIM_LATENCY_MS / SIM_JITTER_MS default 0 (lag simulator off)");
    env["SIM_LATENCY_MS"] = "150";
    env["SIM_JITTER_MS"] = "50";
    const auto lag = load_config(lookup);
    check(lag.errors.empty() && lag.config.simLatencyMs == 150 && lag.config.simJitterMs == 50, "SIM_LATENCY_MS / SIM_JITTER_MS read");
    env["SIM_JITTER_MS"] = "-5";
    check(load_config(lookup).errors.size() == 1, "negative SIM_JITTER_MS refused");
    env.erase("SIM_LATENCY_MS");
    env.erase("SIM_JITTER_MS");

    check(ok.config.chatYellRadius == 300 && ok.config.chatPublicHost.empty() && ok.config.chatPublicPort == 7778 &&
              ok.config.chatLinkHost.empty() && ok.config.chatLinkPort == 7779,
          "chat defaults: yell 300 m, chat service not announced, no chat link");
    env["CHAT_YELL_RADIUS"] = "450";
    env["CHAT_PUBLIC_HOST"] = "127.0.0.1";
    env["CHAT_LINK_HOST"] = "chat";
    env["CHAT_LINK_SECRET"] = "too-short";
    const auto shortSecret = load_config(lookup);
    check(shortSecret.errors.size() == 1 && shortSecret.errors[0].starts_with("CHAT_LINK_SECRET"),
          "CHAT_LINK_HOST with a secret under 16 characters refused");
    env["CHAT_LINK_SECRET"] = "0123456789abcdef";
    const auto linked = load_config(lookup);
    check(linked.errors.empty() && linked.config.chatYellRadius == 450 && linked.config.chatPublicHost == "127.0.0.1" &&
              linked.config.chatLinkHost == "chat",
          "CHAT_YELL_RADIUS / CHAT_PUBLIC_HOST / CHAT_LINK_HOST + a 16-character secret read");
    for (const char* name : {"CHAT_YELL_RADIUS", "CHAT_PUBLIC_HOST", "CHAT_LINK_HOST", "CHAT_LINK_SECRET"}) env.erase(name);

    check(ok.config.adminSecret.empty() && ok.config.adminPort == 7780 && ok.config.adminHost == "127.0.0.1",
          "admin port defaults: off (no WORLD_ADMIN_SECRET), 7780, loopback only");
    env["WORLD_ADMIN_SECRET"] = "";
    check(load_config(lookup).errors.empty() && load_config(lookup).config.adminSecret.empty(),
          "WORLD_ADMIN_SECRET empty: admin port off, not an error");
    env["WORLD_ADMIN_SECRET"] = "too-short";
    const auto shortAdmin = load_config(lookup);
    check(shortAdmin.errors.size() == 1 && shortAdmin.errors[0].starts_with("WORLD_ADMIN_SECRET"),
          "WORLD_ADMIN_SECRET under 16 characters refused");
    env["WORLD_ADMIN_SECRET"] = "0123456789abcdef";
    env["WORLD_ADMIN_HOST"] = "0.0.0.0";
    env["WORLD_ADMIN_PORT"] = "9000";
    const auto admin = load_config(lookup);
    check(admin.errors.empty() && admin.config.adminSecret == "0123456789abcdef" && admin.config.adminHost == "0.0.0.0" &&
              admin.config.adminPort == 9000,
          "WORLD_ADMIN_SECRET / WORLD_ADMIN_HOST / WORLD_ADMIN_PORT read");
    for (const char* name : {"WORLD_ADMIN_SECRET", "WORLD_ADMIN_HOST", "WORLD_ADMIN_PORT"}) env.erase(name);

    env.erase("DB_HOST");
    env["REALM_ID"] = "abc";
    env["MAX_FRAME_BYTES"] = "999999";
    env["LOG_LEVEL"] = "loud";
    env["TLS_CERT_FILE"] = "/x.pem";
    env["MAX_NPCS"] = "lots";
    auto bad = load_config(lookup);
    check(bad.errors.size() == 6, "every bad variable reported, MAX_NPCS with the others");
}

// The lag simulator's schedule (net/session.h lag_due): latency +- jitter, never reordering, never in the past.
void checkLagSim() {
    std::minstd_rand rng(1);
    TimePoint previous = T0;
    bool ordered = true, inRange = true, spread = false;
    Millis first{-1};
    for (int i = 0; i < 1000; ++i) {
        const TimePoint now = T0 + Millis(i); // one message a millisecond: jitter would reorder them
        const TimePoint due = net::lag_due(now, previous, Millis(150), Millis(50), rng);
        ordered &= due >= previous;
        const auto delay = std::chrono::duration_cast<Millis>(due - now);
        inRange &= delay >= Millis(100) && delay <= Millis(200); // waiting for the previous one never exceeds it either
        if (first.count() < 0) first = delay;
        spread |= delay != first;
        previous = due;
    }
    check(ordered && inRange && spread, "lag sim: delays of 150 +- 50 ms, in order");
    check(net::lag_due(T0, T0, Millis(0), Millis(0), rng) == T0, "lag sim: 0 / 0 delivers at once");
    std::minstd_rand fixed(7);
    bool exact = true;
    for (int i = 0; i < 100; ++i) exact &= net::lag_due(T0 + Millis(i), T0, Millis(150), Millis(0), fixed) == T0 + Millis(i + 150);
    check(exact, "lag sim: no jitter -> exactly the latency");
}

// The world's fan-out (GameServer's SendMany -> net::send_many): one encode for every recipient, each session getting
// exactly proto::encode(m), with the lag simulator off and on. Sessions on unconnected sockets: nothing is written
// before TLS, so pending() holds what the next write would send. The lag run waits for its timers (a few ms).
void checkSendMany() {
    asio::io_context io;
    asio::ssl::context tls(asio::ssl::context::tls_server);
    const proto::Message m = proto::ChatMsg{proto::ChatKind::say, "Bob", "hello everyone"};
    const auto expected = proto::encode(m);
    for (const Millis latency : {Millis(0), Millis(5)}) {
        net::SessionLimits limits;
        limits.simLatency = latency;
        std::vector<std::shared_ptr<net::Session>> sessions;
        std::vector<net::Session*> to;
        for (uint32_t id = 1; id <= 5; ++id) {
            sessions.push_back(std::make_shared<net::Session>(id, asio::ip::tcp::socket(io), tls, limits, nullptr, nullptr));
            to.push_back(sessions.back().get());
        }
        const size_t before = net::framesEncoded;
        net::send_many(to, m);
        net::send_many({}, m); // nobody to send to: no encode
        const size_t encodes = net::framesEncoded - before;
        io.restart();
        io.run_for(latency + Millis(50));
        bool same = true;
        for (const auto* s : to) same &= std::ranges::equal(s->pending(), expected);
        const bool lagged = latency.count() > 0;
        check(encodes == 1, lagged ? "send_many (lag sim): 5 recipients, 1 encode" : "send_many: 5 recipients, 1 encode");
        check(same, lagged ? "send_many (lag sim): every session gets proto::encode(m)"
                           : "send_many: every session gets proto::encode(m)");
        if (encodes != 1) std::printf("  send_many encoded %zu times for 5 recipients\n", encodes);
    }
}

// A server Session on 127.0.0.1 and a raw TLS client writing bytes at it: what the buffered reader delivers.
struct Loopback {
    asio::io_context io;
    asio::ssl::context& serverTls;
    asio::ssl::context clientTls{asio::ssl::context::tls_client}; // verify_none: the cert is a throwaway
    asio::ip::tcp::acceptor acceptor{io, {asio::ip::make_address("127.0.0.1"), 0}};
    asio::ssl::stream<asio::ip::tcp::socket> client{io, clientTls};
    std::shared_ptr<net::Session> server;
    std::vector<proto::Message> delivered;
    std::vector<uint8_t> received; // what the client got from the server
    std::array<uint8_t, 4096> chunk{};
    bool ready = false;
    bool kickOnMessage = false; // on_message disconnects the session (as an admin kick would)

    Loopback(asio::ssl::context& tls, uint32_t maxFrameBytes) : serverTls(tls) {
        net::SessionLimits limits;
        limits.maxFrameBytes = maxFrameBytes;
        acceptor.async_accept([this, limits](asio::error_code ec, asio::ip::tcp::socket s) {
            if (ec) return;
            server = std::make_shared<net::Session>(
                1, std::move(s), serverTls, limits,
                [this](net::Session& s, proto::Message&& m) {
                    delivered.push_back(std::move(m));
                    if (kickOnMessage) s.disconnect(proto::Code::kicked);
                },
                nullptr);
            server->start();
        });
        client.lowest_layer().connect(acceptor.local_endpoint());
        client.async_handshake(asio::ssl::stream_base::client, [this](asio::error_code ec) {
            ready = !ec;
            if (ready) read_client();
        });
        run_until([&] { return ready && server; });
    }
    void read_client() {
        client.async_read_some(asio::buffer(chunk), [this](asio::error_code ec, size_t n) {
            if (ec) return;
            received.insert(received.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
            read_client();
        });
    }
    template <class Pred> bool run_until(Pred done, Millis timeout = Millis(3000)) {
        const auto end = SteadyClock::now() + timeout;
        while (!done() && SteadyClock::now() < end) {
            io.restart();
            io.run_for(Millis(5));
        }
        return done();
    }
    void write(std::vector<uint8_t> bytes) {
        bool sent = false;
        asio::async_write(client, asio::buffer(bytes), [&](asio::error_code, size_t) { sent = true; });
        run_until([&] { return sent; });
    }
    // The server's Disconnect reason, once it arrived.
    std::optional<proto::Code> disconnect_reason() const {
        for (size_t at = 0;;) {
            const auto d = proto::decode_frame(std::span(received).subspan(at), proto::kMaxPayloadBytes);
            if (d.status != proto::DecodeStatus::ok) return std::nullopt;
            if (const auto* dc = std::get_if<proto::Disconnect>(&d.msg)) return dc->reason;
            at += d.consumed;
        }
    }
    bool disconnected_with(proto::Code c) {
        run_until([&] { return disconnect_reason().has_value(); });
        return disconnect_reason() == c;
    }
};

std::vector<uint8_t> say_frame(std::string text) {
    return proto::encode(proto::Message{proto::Say{proto::ChatKind::say, std::move(text)}});
}

bool said(const proto::Message& m, const std::string& text) {
    const auto* s = std::get_if<proto::Say>(&m);
    return s && s->text == text;
}

// net::Session's reader over real TLS: one async_read_some can carry many frames, a frame can span many reads, and
// a frame that disconnects stops everything after it.
void checkSessionReads() {
    const auto dir = std::filesystem::temp_directory_path() / "chthonic_self_check_tls"; // generated dev cert, not in the repo
    asio::ssl::context tls = net::make_server_tls({"", "", dir.string()});

    {
        Loopback lb(tls, 4096);
        constexpr size_t N = 100; // under the rate limiter's burst of 120
        std::vector<uint8_t> batch;
        for (size_t i = 0; i < N; ++i) {
            const auto f = say_frame("m" + std::to_string(i));
            batch.insert(batch.end(), f.begin(), f.end());
        }
        const size_t before = net::readsIssued;
        lb.write(batch);
        lb.run_until([&] { return lb.delivered.size() >= N; });
        const size_t reads = net::readsIssued - before;
        bool inOrder = lb.delivered.size() == N;
        for (size_t i = 0; inOrder && i < N; ++i) inOrder = said(lb.delivered[i], "m" + std::to_string(i));
        check(inOrder, "loopback: 100 frames in one write all delivered, in order");
        check(reads < N, "loopback: the batched frames share reads (readsIssued grows by fewer than the frame count)");
        std::printf("  loopback batch: %zu frames (%zu bytes) in one write, %zu reads issued\n", N, batch.size(), reads);
    }
    {
        Loopback lb(tls, 4096);
        const auto frame = say_frame("one byte at a time");
        const size_t before = net::readsIssued;
        for (uint8_t b : frame) lb.write({b});
        lb.run_until([&] { return !lb.delivered.empty(); });
        lb.run_until([] { return false; }, Millis(50)); // a duplicate would show up here
        check(lb.delivered.size() == 1 && said(lb.delivered[0], "one byte at a time"),
              "loopback: a frame written a byte at a time is delivered once, intact");
        std::printf("  loopback byte-at-a-time: %zu bytes in %zu writes, %zu reads issued\n", frame.size(), frame.size(),
                    net::readsIssued - before);
    }
    {
        Loopback lb(tls, 4096);
        lb.write({0x01, 0x10, 0x21, 0x00}); // header only: a Say of 4097 bytes, no payload sent
        check(lb.disconnected_with(proto::Code::frame_too_large), "loopback: oversized header -> Disconnect{frame_too_large}");
    }
    {
        Loopback lb(tls, 4096);
        std::vector<uint8_t> bytes = {4, 0, 0x21, 0, 3, 1, 0, 'a'}; // Say kind 3: malformed
        for (const char* t : {"after1", "after2", "after3"}) {
            const auto f = say_frame(t);
            bytes.insert(bytes.end(), f.begin(), f.end());
        }
        lb.write(bytes);
        check(lb.disconnected_with(proto::Code::malformed), "loopback: malformed payload -> Disconnect{malformed}");
        check(lb.delivered.empty(), "loopback: frames after the malformed one in the same write are not delivered");
    }
    {
        Loopback lb(tls, 4096);
        std::vector<uint8_t> bytes = {0, 0, 0xFF, 0x7F};
        const auto f = say_frame("after");
        bytes.insert(bytes.end(), f.begin(), f.end());
        lb.write(bytes);
        check(lb.disconnected_with(proto::Code::unknown_opcode), "loopback: unknown opcode -> Disconnect{unknown_opcode}");
        check(lb.delivered.empty(), "loopback: a frame after the unknown opcode in the same write is not delivered");
    }
    {
        Loopback lb(tls, 4096);
        lb.kickOnMessage = true;
        std::vector<uint8_t> bytes;
        for (const char* t : {"first", "second", "third"}) {
            const auto f = say_frame(t);
            bytes.insert(bytes.end(), f.begin(), f.end());
        }
        lb.write(bytes);
        check(lb.disconnected_with(proto::Code::kicked), "loopback: on_message disconnecting -> Disconnect{kicked}");
        lb.run_until([] { return false; }, Millis(50)); // anything still delivered would show up here
        check(lb.delivered.size() == 1 && said(lb.delivered[0], "first"),
              "loopback: frames after one whose on_message disconnected, in the same write, are not delivered");
    }
    {
        constexpr uint32_t kMax = 32 * 1024; // bigger than one 16 KB read: the buffer grows to hold the frame
        Loopback lb(tls, kMax);
        const std::string big(kMax - 3, 'x'); // Say payload: kind u8 + u16 length + text
        auto bytes = say_frame(big);
        const bool exact = bytes.size() == proto::kHeaderSize + kMax;
        const auto f = say_frame("next");
        bytes.insert(bytes.end(), f.begin(), f.end());
        lb.write(bytes);
        lb.run_until([&] { return lb.delivered.size() >= 2; });
        check(exact && lb.delivered.size() == 2 && said(lb.delivered[0], big) && said(lb.delivered[1], "next"),
              "loopback: a maxFrameBytes (32 KB) frame and the frame after it are both delivered");
    }
}

// The world's thread split (main.cpp, GameServer): Sessions on net::NetThreads, this thread as the game thread. Real
// 127.0.0.1 TLS sessions, accepted round-robin over the net threads, post their messages and closes to game as
// GameServer's callbacks do.
struct NetRig {
    struct Peer {
        std::shared_ptr<net::Session> server; // held on the game thread, like GameServer::Client::session
        std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket>> client;
        std::vector<std::string> said; // Say texts as the game thread got them
        std::vector<uint8_t> received; // what the client read
        std::array<uint8_t, 4096> chunk{};
        bool ready = false, closed = false;
    };
    asio::io_context game;
    net::NetThreads net{4};
    asio::ssl::context& serverTls;
    asio::ssl::context clientTls{asio::ssl::context::tls_client};
    asio::ip::tcp::acceptor acceptor{game, {asio::ip::make_address("127.0.0.1"), 0}};
    std::vector<std::unique_ptr<Peer>> peers;
    std::atomic<int> onGameThread{0}; // session callbacks that ran on the game thread: must stay 0

    NetRig(asio::ssl::context& tls, size_t n) : serverTls(tls) {
        for (size_t i = 0; i < n; ++i) {
            auto* p = peers.emplace_back(std::make_unique<Peer>()).get();
            acceptor.async_accept(net.next(), [this, p](asio::error_code ec, asio::ip::tcp::socket s) {
                if (ec) return;
                p->server = std::make_shared<net::Session>(
                    uint32_t(peers.size()), std::move(s), serverTls, net::SessionLimits{},
                    [this, p](net::Session&, proto::Message&& m) {
                        onGameThread += game.get_executor().running_in_this_thread();
                        asio::post(game, [p, m = std::move(m)] {
                            if (const auto* say = std::get_if<proto::Say>(&m)) p->said.push_back(say->text);
                        });
                    },
                    [this, p](net::Session&) {
                        onGameThread += game.get_executor().running_in_this_thread();
                        asio::post(game, [p] { p->closed = true; });
                    });
                p->server->start(); // queued for its net thread
            });
            p->client = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket>>(game, clientTls);
            p->client->lowest_layer().connect(acceptor.local_endpoint());
            p->client->async_handshake(asio::ssl::stream_base::client, [this, p](asio::error_code ec) {
                p->ready = !ec;
                if (p->ready) read_client(p);
            });
            run_until([&] { return p->ready && p->server; });
        }
    }
    ~NetRig() { net.stop(); } // before the members: no net thread runs a callback into them
    void read_client(Peer* p) {
        p->client->async_read_some(asio::buffer(p->chunk), [this, p](asio::error_code ec, size_t n) {
            if (ec) return;
            p->received.insert(p->received.end(), p->chunk.begin(), p->chunk.begin() + static_cast<std::ptrdiff_t>(n));
            read_client(p);
        });
    }
    template <class Pred> bool run_until(Pred done, Millis timeout = Millis(5000)) {
        const auto end = SteadyClock::now() + timeout;
        while (!done() && SteadyClock::now() < end) {
            game.restart();
            game.run_for(Millis(5));
        }
        return done();
    }
    // Waits for f(session) to run on the session's own net thread (after everything posted to it before).
    template <class F> void on_session_thread(const std::shared_ptr<net::Session>& s, F f) {
        std::atomic<bool> done{false};
        asio::post(s->executor(), [&, s] {
            f(*s);
            done = true;
        });
        run_until([&] { return done.load(); });
    }
};

void checkNetHandoff() {
    const auto dir = std::filesystem::temp_directory_path() / "chthonic_self_check_tls";
    asio::ssl::context tls = net::make_server_tls({"", "", dir.string()});
    NetRig rig(tls, 8); // two sessions per net thread
    check(std::ranges::all_of(rig.peers, [](auto& p) { return p->ready && p->server; }),
          "net threads: 8 loopback sessions up, accepted round-robin over 4 net threads");

    // 1. Every client writes 100 frames at once: the net threads decode concurrently, the game thread gets each
    //    session's messages in the order they were sent.
    constexpr size_t N = 100; // under the rate limiter's burst of 120
    std::vector<std::vector<uint8_t>> batches(rig.peers.size());
    for (size_t i = 0; i < rig.peers.size(); ++i) {
        for (size_t k = 0; k < N; ++k) {
            const auto f = say_frame(std::to_string(i) + ":" + std::to_string(k));
            batches[i].insert(batches[i].end(), f.begin(), f.end());
        }
        asio::async_write(*rig.peers[i]->client, asio::buffer(batches[i]), [](asio::error_code, size_t) {});
    }
    rig.run_until([&] { return std::ranges::all_of(rig.peers, [](auto& p) { return p->said.size() >= N; }); });
    bool inOrder = true;
    for (size_t i = 0; i < rig.peers.size(); ++i) {
        inOrder &= rig.peers[i]->said.size() == N;
        for (size_t k = 0; inOrder && k < N; ++k) inOrder = rig.peers[i]->said[k] == std::to_string(i) + ":" + std::to_string(k);
    }
    check(inOrder, "net threads: 8 sessions x 100 frames reach the game thread, each session's in send order");
    check(rig.onGameThread == 0, "net threads: session callbacks run on the net threads, never on the game thread");

    // 3. One send_many from the game thread to sessions on every net thread: one encode, one shared buffer, the same
    //    bytes at every client.
    const proto::Message m = proto::ChatMsg{proto::ChatKind::say, "Bob", "hello every net thread"};
    const auto expected = proto::encode(m);
    std::vector<net::Session*> to;
    for (auto& p : rig.peers) to.push_back(p->server.get());
    const size_t before = net::framesEncoded;
    net::send_many(to, m);
    const size_t encodes = net::framesEncoded - before;
    rig.run_until([&] { return std::ranges::all_of(rig.peers, [&](auto& p) { return p->received.size() >= expected.size(); }); });
    check(encodes == 1, "net threads: send_many to 8 sessions on 4 net threads encodes once (framesEncoded +1)");
    check(std::ranges::all_of(rig.peers, [&](auto& p) { return p->received == expected; }),
          "net threads: every client receives identical bytes, exactly proto::encode(m)");

    // 2. A client goes away: its session closes on its net thread while the game thread keeps sending to it (racing
    //    the close), then sends again once on_closed arrived and drops its reference. Nothing crashes, nothing queues.
    auto& gone = *rig.peers[0];
    gone.client->lowest_layer().close();
    for (int k = 0; !gone.closed && k < 2000; ++k) {
        gone.server->send(proto::Say{proto::ChatKind::say, "racing the close"});
        rig.run_until([] { return false; }, Millis(1));
    }
    check(rig.run_until([&] { return gone.closed; }), "net threads: a client's close reaches the game thread (on_closed)");
    std::shared_ptr<net::Session> s = std::move(gone.server); // the game side's last reference
    size_t queuedBefore = SIZE_MAX, queuedAfter = 0;
    bool closing = false;
    rig.on_session_thread(s, [&](net::Session& x) { queuedBefore = x.pending().size(); });
    s->send(proto::Say{proto::ChatKind::say, "after close"});
    net::send_many(std::vector<net::Session*>{s.get()}, m);
    s->disconnect(proto::Code::kicked);
    rig.on_session_thread(s, [&](net::Session& x) {
        queuedAfter = x.pending().size();
        closing = x.closing();
    });
    std::weak_ptr<net::Session> weak = s;
    s->send(proto::Say{proto::ChatKind::say, "last reference in the post"});
    s.reset(); // only that queued send holds it now
    rig.run_until([&] { return weak.expired(); });
    check(closing && queuedAfter == queuedBefore,
          "net threads: send / send_many / disconnect to a session that closed on its net thread are dropped");
    check(weak.expired(), "net threads: a queued send holding the last reference runs, then the session is freed");
    std::printf("  net threads: %zu sessions on %zu net threads, %zu frames each in order: %s; send_many encodes: %zu; "
                "after close: %zu bytes queued before, %zu after (dropped: %s); freed: %s\n",
                rig.peers.size(), rig.net.size(), N, inOrder ? "yes" : "no", encodes, queuedBefore, queuedAfter,
                closing && queuedAfter == queuedBefore ? "yes" : "no", weak.expired() ? "yes" : "no");
}

void checkTickStats() {
    TickStats s;
    for (int i = 1; i <= 100; ++i) s.add(i * 0.1); // 0.1 .. 10 ms
    check(s.count() == 100, "count");
    check(s.percentile(0.5) >= 5.0 && s.percentile(0.5) <= 5.02, "p50 ~5 ms");
    check(s.percentile(0.99) >= 9.9 && s.percentile(0.99) <= 9.92, "p99 ~9.9 ms");
    check(s.max() > 9.99 && s.max() < 10.01, "max");
}

// --- JWT ---

struct RsaKey {
    std::string privPem, pubPem, n, e;
};

std::string bio_string(BIO* bio) {
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<size_t>(len));
}

std::string b64url_bn(const BIGNUM* bn) {
    std::string bin(static_cast<size_t>(BN_num_bytes(bn)), '\0');
    BN_bn2bin(bn, reinterpret_cast<unsigned char*>(bin.data()));
    return jwt::base::trim<jwt::alphabet::base64url>(jwt::base::encode<jwt::alphabet::base64url>(bin));
}

RsaKey make_rsa_key() {
    RsaKey k;
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    BIO* priv = BIO_new(BIO_s_mem());
    BIO* pub = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(priv, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    PEM_write_bio_PUBKEY(pub, pkey);
    k.privPem = bio_string(priv);
    k.pubPem = bio_string(pub);
    BIGNUM *n = nullptr, *e = nullptr;
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n);
    EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e);
    k.n = b64url_bn(n);
    k.e = b64url_bn(e);
    BN_free(n);
    BN_free(e);
    BIO_free(priv);
    BIO_free(pub);
    EVP_PKEY_free(pkey);
    return k;
}

std::string b64url(const std::string& s) {
    return jwt::base::trim<jwt::alphabet::base64url>(jwt::base::encode<jwt::alphabet::base64url>(s));
}

void checkJwt() {
    const RsaKey key = make_rsa_key();
    const RsaKey other = make_rsa_key();
    const SysTime now = std::chrono::system_clock::now();
    const JwtRules rules{"chthonic-auth", "chthonic", 5};

    const std::string jwks = nlohmann::json{{"keys", {{{"kty", "RSA"}, {"kid", "k1"}, {"alg", "RS256"}, {"use", "sig"},
                                                       {"n", key.n}, {"e", key.e}}}}}.dump();
    JwksCache cache([&] { return std::optional(jwks); }, 300s, 10s);
    const TimePoint t = T0;
    auto keyFor = [&](const std::string& kid) { return cache.key(kid, t); };

    auto token = [&](auto mutate) {
        auto b = jwt::create()
                     .set_key_id("k1")
                     .set_issuer("chthonic-auth")
                     .set_audience("chthonic")
                     .set_subject("42")
                     .set_payload_claim("email", jwt::claim(std::string("a@b.c")))
                     .set_payload_claim("role", jwt::claim(std::string("gm")))
                     .set_issued_at(now)
                     .set_expires_at(now + 15min);
        mutate(b);
        return b.sign(jwt::algorithm::rs256(key.pubPem, key.privPem));
    };
    auto none = [](auto&) {};

    auto valid = verify_access_token(token(none), keyFor, rules, now);
    check(valid.code == proto::Code::ok && valid.accountId == 42 && valid.role == proto::Role::gm, "valid token");

    auto expired = verify_access_token(token([&](auto& b) { b.set_expires_at(now - 60s); }), keyFor, rules, now);
    check(expired.code == proto::Code::token_expired, "expired token");
    auto skew = verify_access_token(token([&](auto& b) { b.set_expires_at(now - 3s); }), keyFor, rules, now);
    check(skew.code == proto::Code::ok, "expired 3 s ago is within skew");

    check(verify_access_token(token([](auto& b) { b.set_issuer("evil"); }), keyFor, rules, now).code ==
              proto::Code::invalid_token, "wrong iss");
    check(verify_access_token(token([](auto& b) { b.set_audience("other"); }), keyFor, rules, now).code ==
              proto::Code::invalid_token, "wrong aud");
    check(verify_access_token(token([](auto& b) { b.set_subject("abc"); }), keyFor, rules, now).code ==
              proto::Code::invalid_token, "non-numeric sub");
    check(verify_access_token(token([](auto& b) { b.set_payload_claim("role", jwt::claim(std::string("god"))); }),
                              keyFor, rules, now).code == proto::Code::invalid_token, "unknown role");

    // alg none
    const std::string header = R"({"alg":"none","typ":"JWT","kid":"k1"})";
    const std::string payload = nlohmann::json{{"sub", "42"}, {"role", "admin"}, {"iss", "chthonic-auth"}, {"aud", "chthonic"},
                                               {"exp", std::chrono::system_clock::to_time_t(now + 15min)}}.dump();
    check(verify_access_token(b64url(header) + "." + b64url(payload) + ".", keyFor, rules, now).code ==
              proto::Code::invalid_token, "alg none");

    // HS256 signed with the public key as the HMAC secret (classic key-confusion attack)
    const auto hs = jwt::create().set_key_id("k1").set_issuer("chthonic-auth").set_audience("chthonic").set_subject("42")
                        .set_payload_claim("role", jwt::claim(std::string("admin")))
                        .set_expires_at(now + 15min).sign(jwt::algorithm::hs256(key.pubPem));
    check(verify_access_token(hs, keyFor, rules, now).code == proto::Code::invalid_token, "HS256");

    check(verify_access_token(token([](auto& b) { b.set_key_id("k9"); }), keyFor, rules, now).code ==
              proto::Code::invalid_token, "unknown kid");
    const auto forged = jwt::create().set_key_id("k1").set_issuer("chthonic-auth").set_audience("chthonic").set_subject("42")
                            .set_payload_claim("role", jwt::claim(std::string("admin")))
                            .set_expires_at(now + 15min).sign(jwt::algorithm::rs256(other.pubPem, other.privPem));
    check(verify_access_token(forged, keyFor, rules, now).code == proto::Code::invalid_token, "signed by another key");
    check(verify_access_token("not.a.token", keyFor, rules, now).code == proto::Code::invalid_token, "garbage");
    check(verify_access_token("", keyFor, rules, now).code == proto::Code::invalid_token, "empty");
}

void checkJwksCache() {
    const RsaKey key = make_rsa_key();
    auto doc = [&](const char* kid) {
        return nlohmann::json{{"keys", {{{"kty", "RSA"}, {"kid", kid}, {"n", key.n}, {"e", key.e}}}}}.dump();
    };
    int fetches = 0;
    std::optional<std::string> body = doc("k1");
    JwksCache cache([&] { ++fetches; return body; }, 300s, 10s);

    check(cache.key("k1", T0).status == KeyStatus::ok && fetches == 1, "first lookup fetches");
    check(cache.key("k1", T0 + 299s).status == KeyStatus::ok && fetches == 1, "cached for 300 s");
    check(cache.key("k1", T0 + 300s).status == KeyStatus::ok && fetches == 2, "refreshed after 300 s");

    body = doc("k2"); // rotation
    check(cache.key("k2", T0 + 320s).status == KeyStatus::ok && fetches == 3, "unknown kid refetches once");
    check(cache.key("k9", T0 + 321s).status == KeyStatus::unknown_kid && fetches == 3,
          "unknown kid refetch is rate limited");
    check(cache.key("k9", T0 + 331s).status == KeyStatus::unknown_kid && fetches == 4, "refetch again after interval");

    body.reset(); // auth down
    check(cache.key("k2", T0 + 700s).status == KeyStatus::unavailable, "fetch failure -> unavailable");
    body = "{not json";
    check(cache.key("k2", T0 + 701s).status == KeyStatus::unavailable, "bad JWKS -> unavailable");
    check(!JwksCache::parse(R"({"nokeys":1})"), "JWKS without keys rejected");
}

// --- logging cost (ctest log_bench): stdout goes to the null device so the console is not what is measured ---

// Caller-side ns per line of the pre-Quill path: JSON line built and written with a locked fwrite + fflush.
double sync_stdout_ns(int n) {
    std::mutex m;
    const auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        nlohmann::json line = {{"ts", utc_iso8601(std::chrono::system_clock::now())}, {"level", "info"},
                               {"msg", "bench line"}};
        line.update(nlohmann::json{{"i", i}, {"zone", "x"}});
        const std::string out = line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
        std::lock_guard lock(m);
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
    }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t).count() / n;
}

// Caller-side ns per line of log::info (before the backend has written them).
double log_info_ns(int n) {
    const auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) log::info("bench line", {{"i", i}, {"zone", "x"}});
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t).count() / n;
    log::flush();
    return ns;
}

bool stdout_to_null() {
#ifdef _WIN32
    FILE* null = nullptr;
    return freopen_s(&null, "NUL", "w", stdout) == 0;
#else
    return std::freopen("/dev/null", "w", stdout) != nullptr;
#endif
}

constexpr int kLogLines = 100000;

int log_bench() {
    if (!stdout_to_null()) return 1;
    log_info_ns(kLogLines / 10); // warm up (backend thread, queue allocation)
    const double before = sync_stdout_ns(kLogLines);
    const double after = log_info_ns(kLogLines);
    std::fprintf(stderr, "log bench: %d lines, sync stdout %.0f ns/line, log::info %.0f ns/line (caller side)\n",
                 kLogLines, before, after);
    // ponytail: printed, not checked; one timing sample on a busy CI or docker builder is too noisy to fail a build on
    if (after >= before) std::fprintf(stderr, "log bench: NOTE log::info was not faster this run\n");
    return 0;
}

// ctest log_pressure: 4 threads log as fast as they can (several MiB each, faster than stdout); none is dropped.
int log_pressure() {
    if (!stdout_to_null()) return 1;
    constexpr int kLines = kLogLines;
    std::atomic<int> seen{0};
    log::set_test_sink([&seen](const std::string&) { ++seen; });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([t] { for (int i = 0; i < kLines; ++i) log::info("pressure", {{"t", t}, {"i", i}}); });
    for (auto& th : threads) th.join();
    log::flush();
    log::set_test_sink(nullptr);
    std::fprintf(stderr, "log pressure: %d of %d lines reached the sink\n", seen.load(), 4 * kLines);
    check(seen == 4 * kLines, "no log line is dropped under queue pressure");
    std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// --- AOI cost (ctest aoi_bench): ns per aoi_system call, players + NPCs spread over one 2 km map ---

// One aoi_system scenario: `players` players and players/5 NPCs at random spots; `movingPct` of the players take a
// short random step (often within their cell) every tick. Returns ns per call over the timed ticks.
double aoi_bench_ns(int players, int movingPct) {
    using namespace chthonic::world;
    constexpr float kSide = 2000;
    entt::registry reg;
    AoiGrid grid(64, 128); // the WorldRules defaults
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> spot(0, kSide), step(-3, 3);
    std::vector<entt::entity> movers;
    for (int i = 0; i < players + players / 5; ++i) {
        const auto e = reg.create();
        const proto::Vec3 p{spot(rng), 0, spot(rng)};
        reg.emplace<Transform>(e, p, 0.f);
        reg.emplace<MapRef>(e, uint16_t(1));
        reg.emplace<AoiCell>(e, grid.cell_of(1, p.x, p.z));
        grid.insert(wire_id(e), grid.cell_of(1, p.x, p.z));
        if (i < players) {
            reg.emplace<SessionLink>(e, uint32_t(i + 1));
            reg.emplace<CharacterRef>(e, uint64_t(i + 1), "p");
            reg.emplace<Visible>(e);
            if (i < players * movingPct / 100) movers.push_back(e);
        } else {
            reg.emplace<Npc>(e, "n", "t", "");
            reg.emplace<Viewers>(e);
        }
    }
    const Send send = [](uint32_t, const proto::Message&) {};
    const auto tick = [&] {
        for (auto e : movers) {
            auto& p = reg.get<Transform>(e).pos;
            p.x = std::clamp(p.x + step(rng), 0.f, kSide);
            p.z = std::clamp(p.z + step(rng), 0.f, kSide);
            reg.emplace_or_replace<Moved>(e);
        }
        const auto t = std::chrono::steady_clock::now();
        aoi_system(reg, grid, send);
        const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t).count();
        end_of_tick_system(reg);
        return ns;
    };
    for (int i = 0; i < 20; ++i) tick(); // warm up: first views, buffers grown
    constexpr int kTicks = 200;
    double total = 0;
    for (int i = 0; i < kTicks; ++i) total += tick();
    return total / kTicks;
}

int aoi_bench() {
    for (int players : {500, 2000})
        for (int moving : {0, 10, 100})
            std::fprintf(stderr, "aoi bench: %4d players %4d npcs, %3d%% moving: %10.0f ns/aoi_system\n", players,
                         players / 5, moving, aoi_bench_ns(players, moving));
    // ponytail: printed, not checked (like log_bench); before/after numbers are compared by hand in the report
    return 0;
}

// --- parallel differential check: World with MAP_THREADS=1 against MAP_THREADS=4, message for message ---
namespace parallel_diff {
using namespace chthonic::world;
namespace fs = std::filesystem;
constexpr uint16_t kMaps = 4;     // world maps 1..4
constexpr uint16_t kInstance = 5; // an instance map: maxPlayers 3, emptyTimeoutSec 1, exit on map 1
constexpr float kSide = 512;      // 2 x 2 tiles per map

// The Lua root: an aoi + persisted component every character gets, a spawn NPC definition, a game machine, and one
// system that, on its own seeded generator (Lua's math.random is not used), moves NPCs, creates them (here and on
// other maps; past MAX_NPCS the error is caught), destroys and transfers NPCs and players, sets components, posts to
// one map or "all", answers posts with a broadcast, sends client events and changes the game machine. Its instanceKey
// picks one of 3 keys or none (a new copy), chthonic.transfer's instance = one of 3; instance.full goes back to the player.
constexpr const char* kSystem = R"lua(
local seed, tick = 0, 0
local function rnd(n) seed = (seed * 16807) % 2147483647 ; return seed % n end
local function spot() return {x = rnd(490) + 10, y = 0, z = rnd(490) + 10} end
local function here()
    for e in chthonic.query("Character") do return chthonic.get(e, "Transform").map end
    for e in chthonic.query("Npc") do return chthonic.get(e, "Transform").map end
end
return {
    init = function()
        chthonic.subscribe("ping", function(p) chthonic.broadcast("pinged", {from = p.from, n = p.data.n}) end)
        chthonic.subscribe("instance.full", function(p) chthonic.sendTo(p.entity, "full", {key = p.key, map = p.map}) end)
        local i = chthonic.map.instance()
        if i then chthonic.broadcast("copy", {id = i.id, key = i.key}) end
    end,
    instanceKey = function(e, map)
        local k = rnd(4)
        if k > 0 then return "k" .. k end
    end,
    onEnter = function(e) chthonic.add(e, "Hp") end,
    update = function()
        tick = tick + 1
        local m = here()
        if not m then return end
        if seed == 0 then seed = m * 7919 end
        local npcs, players = {}, {}
        for e in chthonic.query("Npc") do npcs[#npcs + 1] = e end
        for e in chthonic.query("Character") do players[#players + 1] = e end
        for i = 1, #npcs do
            if rnd(3) == 0 then
                local p = chthonic.get(npcs[i], "Transform").position
                chthonic.set(npcs[i], "Transform", {position = {x = p.x + rnd(21) - 10, y = 0, z = p.z + rnd(21) - 10}})
            end
        end
        if #npcs < 40 and rnd(3) == 0 then pcall(chthonic.create, "N", {template = "T", position = spot()}) end
        if rnd(8) == 0 then pcall(chthonic.create, "X", {template = "T", map = rnd(4) + 1, position = spot()}) end
        if #npcs > 8 and rnd(4) == 0 then pcall(chthonic.destroy, npcs[rnd(#npcs) + 1]) end -- maybe gone already
        if #npcs > 0 and rnd(6) == 0 then pcall(chthonic.transfer, npcs[rnd(#npcs) + 1], rnd(4) + 1, spot()) end
        if #players > 0 and rnd(25) == 0 then pcall(chthonic.transfer, players[rnd(#players) + 1], rnd(4) + 1, spot()) end
        if #players > 0 and rnd(12) == 0 then
            pcall(chthonic.transfer, players[rnd(#players) + 1], {map = 5, position = spot(), instance = "k" .. (rnd(3) + 1)})
        end
        if #npcs > 0 and rnd(40) == 0 then pcall(chthonic.transfer, npcs[rnd(#npcs) + 1], 5, spot()) end
        for i = 1, #players do
            if rnd(4) == 0 then pcall(chthonic.set, players[i], "Hp", {cur = rnd(100)}) end
        end
        if rnd(5) == 0 then chthonic.world.post(rnd(4) + 1, "ping", {n = tick}) end
        if rnd(30) == 0 then chthonic.world.post("all", "ping", {n = -tick}) end
        if #players > 0 and rnd(3) == 0 then chthonic.sendNearby(players[rnd(#players) + 1], "near", {t = tick, m = m}) end
        if #players > 0 and rnd(4) == 0 then chthonic.sendTo(players[rnd(#players) + 1], "own", {t = tick}) end
        if rnd(40) == 0 then pcall(chthonic.fsm.change, "cycle", chthonic.fsm.state("cycle") == "a" and "b" or "a") end
    end,
}
)lua";

// Map i: 2 x 2 tiles, 10 Mob spawns (respawn after 2 s) and a portal area at (400..410, 400..410) to map i % 4 + 1;
// maps 1..4 also a "crypt" portal at (200..210, 200..210) into instance map 5, whose door leads to map 1.
Maps diff_maps() {
    Maps maps;
    for (uint16_t id = 1; id <= kInstance; ++id) {
        map::MapInfo m;
        m.id = id;
        m.name = "diff" + std::to_string(id);
        m.tiles = {{0, 0, 1, 1}};
        m.spawn = {kSide / 2, 0, kSide / 2};
        if (id == kInstance) {
            m.type = map::MapType::Instance;
            m.maxPlayers = 3;
            m.emptyTimeoutSec = 1;
            m.exit = map::Portal{1, {kSide / 2, 0, kSide / 2}, 0};
        }
        for (int i = 0; i < 10; ++i)
            m.spawns.push_back({"m" + std::to_string(i), "Mob", {float(30 + 45 * i), 0, float(480 - 40 * i)}, 0, 2, {}, ""});
        std::string err;
        const auto areas = map::parse_areas(nlohmann::json::parse(std::format(
            R"({{"version":1,"areas":[{{"id":"door","rect":[400,400,410,410],"portal":{{"map":{},"pos":[100,0,100]}}}}{}]}})",
            id == kInstance ? 1 : id % kMaps + 1,
            id == kInstance ? "" : R"(,{"id":"crypt","rect":[200,200,210,210],"portal":{"map":5,"pos":[100,0,100]}})")), err);
        if (areas) m.areas = *areas;
        maps.add(m);
    }
    return maps;
}

struct LuaFiles {
    fs::path root = fs::temp_directory_path() / ("chthonic_parallel_diff_" + std::to_string(std::random_device{}()));
    LuaFiles() {
        const std::pair<const char*, const char*> files[] = {
            {"components/hp.lua", R"(return { name = "Hp", replicate = "aoi", persist = true, fields = {
                { name = "cur", type = "int", default = 50, min = 0 } } })"},
            {"npcs/mob.lua", "return { name = 'Mob', template = 'MobTpl', components = { Hp = { cur = 5 } } }"},
            {"fsms/cycle.lua", "return { name = 'cycle', initial = 'a', states = { a = {}, b = {} } }"},
            {"systems/chaos.lua", kSystem}};
        for (const auto& [rel, text] : files) {
            fs::create_directories((root / rel).parent_path());
            std::ofstream(root / rel, std::ios::binary) << text;
        }
    }
    ~LuaFiles() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

// One World and what it sent: each session's messages (encoded) and the saves, since the last clear.
struct Run {
    std::map<uint32_t, std::vector<std::vector<uint8_t>>> sent;
    std::vector<std::string> saves;
    World world;
    std::vector<std::pair<uint32_t, entt::entity>> players; // (session, entity), login order
    std::mt19937 rng;
    uint32_t nextSession = 1;

    Run(const Maps& maps, const LuaFiles& lua, int threads, unsigned seed)
        : world(rules(threads), maps,
                [this](uint32_t sid, const proto::Message& m) { sent[sid].push_back(proto::encode(m)); },
                [this](std::span<const uint32_t> sids, const proto::Message& m) {
                    for (uint32_t sid : sids) sent[sid].push_back(proto::encode(m));
                },
                [this](const db::CharSave& s) {
                    saves.push_back(std::format("{} {}/{} {} {} {} {} {}", s.charId, s.mapId, s.instanceId, s.pos.x,
                                                s.pos.y, s.pos.z, s.heading, s.components.value_or("-")));
                },
                lua::load_component_defs(lua.root, world::kReservedComponentNames).defs),
          rng(seed) {}
    static WorldRules rules(int threads) {
        WorldRules r;
        r.mapThreads = threads;
        r.maxNpcs = 150; // reached now and then: the shares of MAX_NPCS take part
        r.saveInterval = Millis(500);
        return r;
    }
};

struct Counts {
    int logins = 0, logouts = 0, moves = 0, corrections = 0, says = 0, transfers = 0, creates = 0, destroys = 0,
        mapChanges = 0, spawns = 0, serverEvents = 0, componentUpdates = 0, saves = 0, copiesMade = 0,
        copiesDestroyed = 0, fullRefusals = 0, copyLogins = 0;
};

// Between two ticks, as GameServer would: logins (some next to a portal), logouts, moves (now and then a speedhack),
// says and yells, client events, C++ transfers and NPC creates / destroys. Every choice comes from r.rng.
void drive(Run& r, TimePoint now, Counts& n) {
    std::uniform_real_distribution<float> u(0, 1), spot(10, kSide - 10), step(-0.4f, 0.4f);
    const auto chance = [&](float p) { return u(r.rng) < p; };
    const auto map = [&] { return uint16_t(1 + r.rng() % kMaps); };
    if (r.players.size() < 60 && chance(0.4f)) {
        const uint32_t sid = r.nextSession++;
        const uint16_t m = chance(0.15f) ? kInstance : map();
        const float edge = chance(0.5f) ? 198.f : 398.f; // next to the crypt portal or the door
        const proto::Vec3 pos = chance(0.3f) ? proto::Vec3{edge + u(r.rng), 0, edge + u(r.rng)} : proto::Vec3{spot(r.rng), 0, spot(r.rng)};
        db::CharRow row{sid, sid, "P" + std::to_string(sid), m, pos, 0, "", {}};
        if (m == kInstance) { // a saved copy: a live one (rejoined if it has room), or one long gone (the exit)
            const auto live = r.world.contexts();
            const auto& pick = live[r.rng() % live.size()];
            row.instanceId = pick.map == kInstance && chance(0.8f) ? pick.instance : 999999;
            n.copyLogins += row.instanceId != 999999;
        }
        r.players.emplace_back(sid, r.world.spawn(sid, row, now));
        ++n.logins;
    }
    if (r.players.size() > 10 && chance(0.15f)) {
        const size_t i = r.rng() % r.players.size();
        r.world.despawn(r.players[i].second);
        r.players.erase(r.players.begin() + std::ptrdiff_t(i));
        ++n.logouts;
    }
    for (const auto& [sid, e] : r.players) {
        entt::registry& reg = *r.world.registry_of(e);
        if (chance(0.5f)) {
            const auto& ms = reg.get<MoveState>(e);
            proto::Vec3 to = reg.get<Transform>(e).pos;
            const bool hack = chance(0.03f);
            to.x = std::clamp(to.x + (hack ? 40 : step(r.rng)), 1.f, kSide - 1);
            to.z = std::clamp(to.z + step(r.rng), 1.f, kSide - 1);
            to.y = 0;
            r.world.push_move(e, proto::Move{to, 0, 0, 0, ms.epoch});
            ++n.moves;
        }
        if (chance(0.02f)) {
            r.world.push_say(e, proto::Say{chance(0.5f) ? proto::ChatKind::say : proto::ChatKind::yell, "hi " + std::to_string(sid)});
            ++n.says;
        }
        if (chance(0.02f)) r.world.push_client_event(e, "poke", nlohmann::json{{"n", sid}});
    }
    if (!r.players.empty() && chance(0.08f)) {
        const bool copy = chance(0.3f);
        const std::optional<std::string> key = copy && chance(0.7f) ? std::optional("k" + std::to_string(1 + r.rng() % 3)) : std::nullopt;
        r.world.transfer(r.players[r.rng() % r.players.size()].second,
                         {copy ? kInstance : map(), {spot(r.rng), 0, spot(r.rng)}, 0, key});
        ++n.transfers;
    }
    if (chance(0.1f)) {
        r.world.create_npc("C", "T", map(), proto::Vec3{spot(r.rng), 0, spot(r.rng)}, 0);
        ++n.creates;
    }
    if (chance(0.1f)) {
        std::vector<entt::entity> npcs;
        for (uint16_t m = 1; m <= kMaps; ++m)
            for (auto e : r.world.registry(m).view<Npc>()) npcs.push_back(e);
        if (!npcs.empty()) {
            r.world.destroy_npc(npcs[r.rng() % npcs.size()]);
            ++n.destroys;
        }
    }
}

// "" if a and b sent the same, else the first difference.
std::string mismatch(const Run& a, const Run& b) {
    for (const auto& [sid, msgs] : a.sent) {
        const auto it = b.sent.find(sid);
        const size_t got = it == b.sent.end() ? 0 : it->second.size();
        if (got != msgs.size()) return std::format("session {}: {} messages vs {}", sid, msgs.size(), got);
        for (size_t i = 0; i < msgs.size(); ++i)
            if (it->second[i] != msgs[i])
                return std::format("session {} message {} differs (opcode {} vs {})", sid, i, msgs[i][2] | msgs[i][3] << 8,
                                   it->second[i][2] | it->second[i][3] << 8);
    }
    if (a.sent.size() != b.sent.size()) return "MAP_THREADS=4 sent to a session MAP_THREADS=1 did not";
    if (a.saves != b.saves) return std::format("saves differ ({} vs {})", a.saves.size(), b.saves.size());
    if (a.world.population() != b.world.population()) return "population differs";
    const auto live = [](const Run& r) {
        std::string out;
        for (const auto& c : r.world.contexts())
            out += std::format("{}/{}/{}:{}:{} ", c.map, c.instance, c.key.value_or("-"), c.players, c.npcs);
        return out;
    };
    if (live(a) != live(b)) return "live contexts differ: " + live(a) + "vs " + live(b);
    return "";
}

// seeds x ticks; prints what ran. Returns the first difference, "" if none.
std::string run(int seeds, int ticks, Counts& n, int& compared) {
    const Maps maps = diff_maps();
    const LuaFiles lua;
    for (int seed = 1; seed <= seeds; ++seed) {
        Run one(maps, lua, 1, unsigned(seed)), four(maps, lua, 4, unsigned(seed));
        for (Run* r : {&one, &four})
            if (const auto errors = r->world.load_scripts(lua.root, {"cycle"}); !errors.empty()) return errors.front();
        TimePoint now = T0;
        std::set<uint32_t> copies; // live in `one` after the last tick
        for (int t = 0; t < ticks; ++t, ++compared) {
            now += Millis(50);
            Counts ignored;
            drive(one, now, n);
            drive(four, now, ignored);
            one.world.tick(now);
            four.world.tick(now);
            if (std::string why = mismatch(one, four); !why.empty())
                return std::format("seed {} tick {}: {}", seed, t, why);
            for (const auto& [sid, msgs] : one.sent)
                for (const auto& m : msgs) {
                    const auto op = proto::Opcode(m[2] | m[3] << 8);
                    n.corrections += op == proto::Opcode::Correction;
                    n.mapChanges += op == proto::Opcode::MapChange;
                    n.spawns += op == proto::Opcode::Spawn;
                    n.serverEvents += op == proto::Opcode::ServerEvent;
                    n.componentUpdates += op == proto::Opcode::ComponentUpdate;
                    if (op == proto::Opcode::ServerEvent)
                        if (const auto d = proto::decode_frame(m, proto::kMaxPayloadBytes); d.status == proto::DecodeStatus::ok)
                            n.fullRefusals += std::get<proto::ServerEvent>(d.msg).name == "full";
                }
            std::set<uint32_t> alive;
            for (const auto& c : one.world.contexts())
                if (c.instance) alive.insert(c.instance);
            for (uint32_t id : alive) n.copiesMade += !copies.contains(id);
            for (uint32_t id : copies) n.copiesDestroyed += !alive.contains(id);
            copies = std::move(alive);
            n.saves += int(one.saves.size());
            for (Run* r : {&one, &four}) {
                r->sent.clear();
                r->saves.clear();
            }
        }
        for (const auto& s : one.world.script_status()) // a disabled system would stop making the traffic checked here
            if (!s.ok) return std::format("seed {}: {} disabled: {}", seed, s.path, s.error);
    }
    return "";
}

void print(const Counts& n, int compared) {
    std::printf("parallel differential: %d ticks compared; logins %d / logouts %d, moves %d (%d Corrections), says %d, "
                "C++ transfers %d, C++ NPC creates %d / destroys %d; %d MapChange, %d Spawn, %d ServerEvent, %d "
                "ComponentUpdate, %d saves; instance copies made %d / destroyed %d, full refusals %d, logins into a "
                "saved copy %d\n",
                compared, n.logins, n.logouts, n.moves, n.corrections, n.says, n.transfers, n.creates, n.destroys,
                n.mapChanges, n.spawns, n.serverEvents, n.componentUpdates, n.saves, n.copiesMade, n.copiesDestroyed,
                n.fullRefusals, n.copyLogins);
}
} // namespace parallel_diff

// Seeded random worlds on 4 maps and an instance map (players, spawn and Lua NPCs, moves, logins / logouts, NPC create
// / destroy, portals, C++ and Lua transfers across maps, instance copies made by key / hook / portal, refused when
// full, rejoined on login and destroyed when empty, chthonic.world.post, broadcasts, chat, MAX_NPCS reached) run with
// MAP_THREADS=1 and MAP_THREADS=4: after every tick each session's messages (encoded, in order), the saves and the live
// contexts must be identical.
void checkParallelDifferential() {
    log::set_level(log::Level::error); // Lua's NPC moves past a map edge and MAX_NPCS warn; not what is checked
    parallel_diff::Counts n;
    int compared = 0;
    const std::string why = parallel_diff::run(20, 400, n, compared);
    log::set_level(log::Level::info);
    if (!why.empty()) std::fprintf(stderr, "parallel differential: %s\n", why.c_str());
    check(why.empty(), "parallel differential: MAP_THREADS=4 sends and saves exactly what MAP_THREADS=1 does");
    parallel_diff::print(n, compared);
    check(n.logins > 100 && n.logouts > 100 && n.corrections > 100 && n.says > 100 && n.transfers > 100 &&
              n.creates > 100 && n.destroys > 100 && n.mapChanges > 1000 && n.spawns > 1000 && n.serverEvents > 1000 &&
              n.componentUpdates > 1000 && n.saves > 100 && n.copiesMade > 100 && n.copiesDestroyed > 100 &&
              n.fullRefusals > 50 && n.copyLogins > 50,
          "parallel differential: every event kind ran many times");
}

// world_self_check --parallel-stress N: the differential check over N seeds (the long race run).
int parallel_stress(int seeds) {
    log::set_level(log::Level::error);
    const auto start = std::chrono::steady_clock::now();
    parallel_diff::Counts n;
    int compared = 0;
    const std::string why = parallel_diff::run(seeds, 400, n, compared);
    parallel_diff::print(n, compared);
    std::printf("parallel stress: %d seeds x 400 ticks in %.1f s: %s\n", seeds,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                why.empty() ? "identical" : why.c_str());
    return why.empty() ? 0 : 1;
}

// --- parallel map update cost (ctest map_threads_bench): p50/p95 World::tick ms on 4 maps, each with 100 moving
// players and 100 patrolling spawn-file Guards (the sample lua/) around them, on the injected clock ---
namespace threads_bench {
using namespace chthonic::world;
constexpr uint16_t kMaps = 4;
constexpr int kPlayers = 100, kGuards = 100;
constexpr float kSide = 1024; // 4 x 4 tiles per map

Maps bench_maps() {
    Maps maps;
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> in(8, kSide - 8), leg(-30, 30);
    for (uint16_t id = 1; id <= kMaps; ++id) {
        map::MapInfo m;
        m.id = id;
        m.name = "bench" + std::to_string(id);
        m.tiles = {{0, 0, 3, 3}};
        m.spawn = {kSide / 2, 0, kSide / 2};
        for (int i = 0; i < kGuards; ++i) {
            const float x = in(rng), z = in(rng);
            const float x2 = std::clamp(x + leg(rng), 8.f, kSide - 8), z2 = std::clamp(z + leg(rng), 8.f, kSide - 8);
            m.spawns.push_back({"g" + std::to_string(i), "Guard", {x, 0, z}, 0, 30,
                                map::PatrolPath{{{x, 0, z}, {x2, 0, z2}}, 1, true}, ""});
        }
        maps.add(m);
    }
    return maps;
}

struct Result {
    double p50 = -1, p95 = -1;
};

double percentile(std::vector<double> v, double p) {
    const auto at = v.begin() + static_cast<std::ptrdiff_t>(p * double(v.size() - 1));
    std::ranges::nth_element(v, at);
    return *at;
}

Result run(int threads) {
    const Maps maps = bench_maps();
    WorldRules rules;
    rules.mapThreads = threads;
    World world(rules, maps, [](uint32_t, const proto::Message&) {}, [](std::span<const uint32_t>, const proto::Message&) {},
                [](const db::CharSave&) {},
                lua::load_component_defs(CHTHONIC_WORLD_LUA_SAMPLE, world::kReservedComponentNames).defs);
    if (const auto errors = world.load_scripts(CHTHONIC_WORLD_LUA_SAMPLE); !errors.empty()) {
        std::fprintf(stderr, "map threads bench: %s\n", errors.front().c_str());
        return {};
    }
    struct Player {
        entt::entity e;
        proto::Vec3 pos;
        float heading;
        uint32_t seq = 0;
    };
    std::vector<Player> players;
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> in(8, kSide - 8), turn(0, 6.2831853f);
    TimePoint now = T0;
    for (uint16_t map = 1; map <= kMaps; ++map)
        for (int i = 0; i < kPlayers; ++i) {
            const uint32_t id = uint32_t(players.size() + 1);
            const proto::Vec3 p{in(rng), 0, in(rng)};
            const db::CharRow row{id, id, "P" + std::to_string(id), map, p, 0, "", {}};
            players.push_back({world.spawn(id, row, now), p, turn(rng)});
        }
    const auto tick = [&] {
        now += std::chrono::milliseconds(50);
        for (Player& p : players) { // 8 m/s, turning back at the edges
            proto::Vec3 to{p.pos.x + 0.4f * std::sin(p.heading), 0, p.pos.z + 0.4f * std::cos(p.heading)};
            if (to.x < 8 || to.x > kSide - 8 || to.z < 8 || to.z > kSide - 8) {
                p.heading += 3.14159265f;
                to = {p.pos.x + 0.4f * std::sin(p.heading), 0, p.pos.z + 0.4f * std::cos(p.heading)};
            }
            p.pos = to;
            const uint32_t epoch = world.registry_of(p.e)->get<MoveState>(p.e).epoch;
            world.push_move(p.e, proto::Move{to, p.heading, 0, ++p.seq, epoch});
        }
        const auto start = std::chrono::steady_clock::now();
        world.tick(now);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    for (int i = 0; i < 50; ++i) tick(); // warm up: spawns, first views, buffers grown
    std::vector<double> ms;
    for (int i = 0; i < 300; ++i) ms.push_back(tick());
    return {percentile(ms, 0.50), percentile(ms, 0.95)};
}
} // namespace threads_bench

int map_threads_bench() {
    log::set_level(log::Level::error); // the sample's per-entity warnings are not what is measured
    const unsigned cores = std::thread::hardware_concurrency();
    const auto one = threads_bench::run(1), four = threads_bench::run(4);
    std::fprintf(stderr, "map threads bench: %u hardware threads; 4 maps x (100 players + 100 Guards)\n", cores);
    std::fprintf(stderr, "map threads bench: MAP_THREADS=1 tick p50 %.3f ms, p95 %.3f ms\n", one.p50, one.p95);
    std::fprintf(stderr, "map threads bench: MAP_THREADS=4 tick p50 %.3f ms, p95 %.3f ms; p95 = %.1f%% of 1 thread's\n",
                 four.p50, four.p95, 100 * four.p95 / one.p95);
    if (one.p95 < 0 || four.p95 < 0) return 1;
#ifndef NDEBUG
    // Measured: MSVC Debug runs 4 threads at 140-230% of 1 thread's time (its checked STL and debug heap take
    // process-wide locks), so a Debug build says nothing about the speedup; Release checks it (ctest --preset perf).
    std::fprintf(stderr, "map threads bench: SKIPPED the 40%% threshold: a Debug build (build Release to check it)\n");
    return 0;
#else
    if (cores < 4) {
        std::fprintf(stderr, "map threads bench: SKIPPED the 40%% threshold: %u hardware threads, need 4\n", cores);
        return 0;
    }
    if (four.p95 > 0.40 * one.p95) {
        std::fprintf(stderr, "map threads bench: FAIL, the 4-thread p95 must be at most 40%% of the 1-thread p95\n");
        return 1;
    }
    return 0;
#endif
}

void segv() {
    volatile int* null = nullptr;
    *null = 1; // SIGSEGV / access violation
}

// ctest log_flush: a line logged right before the process ends must still reach stdout.
//   segv, abort    crash on main (Quill's crash handler flushes)
//   segv-thread    crash on a thread other than the one that started the log backend
//   kill           SIGKILL / TerminateProcess while the backend is busy: a warn line already written is on stdout
//   sigterm        the backend must not take over SIGTERM from the app (main's graceful shutdown)
int log_crash(std::string_view how) {
    if (how == "segv-thread") {
        log::info("main thread logs first");
        std::thread([how] { log::info("before crash", {{"how", how}}); segv(); }).join();
    } else if (how == "kill") {
        std::atomic<bool> written{false};
        log::set_test_sink([&written](const std::string& l) { if (l.find("before crash") != l.npos) written = true; });
        std::thread([] { for (int i = 0;; ++i) log::info("busy", {{"i", i}}); }).detach();
        log::warn("before crash", {{"how", how}});
        while (!written) std::this_thread::yield();
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 9);
#else
        std::raise(SIGKILL);
#endif
    } else if (how == "sigterm") {
        static volatile std::sig_atomic_t got = 0;
        std::signal(SIGTERM, [](int) { got = 1; }); // as main's asio::signal_set, set before the first log line
        log::info("before crash", {{"how", how}});   // starts the backend
        std::raise(SIGTERM);
        if (got) log::info("app SIGTERM handler ran");
        log::flush();
        return got ? 0 : 1;
    }
    log::info("before crash", {{"how", how}});
    if (how == "abort") std::abort();
    segv();
    return 0;
}

} // namespace

int lua_checks(int& checks); // lua_checks.cpp
extern bool g_timing_gates;    // lua_checks.cpp
int activation_bench();        // lua_checks.cpp
int instance_bench();          // lua_checks.cpp

// A test never waits for a click: abort(), failed asserts, debug-CRT reports (STL debug checks) and crashes write to
// stderr and end the process instead of opening a modal dialog. Like map/crash_dialogs.h's, without its
// CHTHONIC_NO_CRASH_DIALOGS switch.
static void disable_crash_dialogs() {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // abort(): no "Debug Error!" box, no WER report
    _set_error_mode(_OUT_TO_STDERR);                               // assert() in a release build
#ifdef _DEBUG
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); // access violations: no "has stopped working" box
#endif
}

// DbWorker's save queue without MySQL: shard routing, latest wins (keeping the retry schedule), fair due order.
void checkSaveQueue() {
    using namespace chthonic::db;
    check(shard_of(9, 4) == 1 && shard_of(12, 4) == 0 && shard_of(9, 1) == 0, "a character's saves go to worker charId % N");

    const TimePoint t0{};
    PendingSaves p;
    CharSave a;
    a.charId = 7;
    a.pos = {1, 0, 1};
    queue_save(p, a, t0);
    a.pos = {2, 0, 2};
    queue_save(p, a, t0 + Millis(5));
    check(p.size() == 1 && p[7].save.pos.x == 2 && p[7].due == t0 + Millis(5), "latest save wins");
    p[7].attempts = 3; // failing: retry scheduled at +1 s
    p[7].due = t0 + Millis(1000);
    a.pos = {3, 0, 3};
    queue_save(p, a, t0 + Millis(10));
    check(p[7].save.pos.x == 3 && p[7].attempts == 3 && p[7].due == t0 + Millis(1000),
          "a newer save keeps the retry schedule of a failing one");

    PendingSaves q;
    for (uint64_t id = 1; id <= 20; ++id) {
        CharSave s;
        s.charId = id;
        queue_save(q, s, t0 + Millis(100 - id)); // higher ids queued earlier
    }
    q[3].due = t0 + Millis(500); // backing off
    const auto due = due_saves(q, t0 + Millis(200), 8);
    check(due == std::vector<uint64_t>{20, 19, 18, 17, 16, 15, 14, 13}, "due saves: at most N, earliest due first");
    check(due_saves(q, t0 + Millis(200), 100).size() == 19, "a backing-off save is not due");
    check(due_saves(q, TimePoint::max(), SIZE_MAX).size() == 20, "shutdown flush takes every save");
}

int main(int argc, char** argv) {
    disable_crash_dialogs();
    if (argc > 1 && std::string_view(argv[1]) == "--log-bench") return log_bench();
    if (argc > 1 && std::string_view(argv[1]) == "--log-pressure") return log_pressure();
    g_timing_gates = argc > 1 && std::string_view(argv[1]) == "--timing";
    if (argc > 1 && std::string_view(argv[1]) == "--aoi-bench") return aoi_bench();
    if (argc > 1 && std::string_view(argv[1]) == "--activation-bench") return activation_bench();
    if (argc > 1 && std::string_view(argv[1]) == "--instance-bench") return instance_bench();
    if (argc > 1 && std::string_view(argv[1]) == "--map-threads-bench") return map_threads_bench();
    if (argc > 2 && std::string_view(argv[1]) == "--parallel-stress") return parallel_stress(std::atoi(argv[2]));
    if (argc > 2 && std::string_view(argv[1]) == "--log-crash") return log_crash(argv[2]);
    checkCodecRoundTrip();
    checkFrameRejection();
    checkNames();
    checkMovement();
    checkAoi();
    checkAoiDifferential();
    checkParallelDifferential();
    checkChat();
    checkClientEvents();
    checkMessageRate();
    checkConfig();
    checkSaveQueue();
    checkLagSim();
    checkSendMany();
    checkSessionReads();
    checkNetHandoff();
    checkTickStats();
    checkJwt();
    checkJwksCache();
    g_failures += lua_checks(g_checks);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
