// Assert-style checks for the chat service's logic: the chat messages' codec, ChatRouter (whispers across maps and to
// offline names, channel membership, the zone channel following map changes, ignore, GM mutes with their expiry, rate
// limit and sanitizing) and the config. No network, database or sleeping: time is passed in.
// Exit 0 if every check passes. Run by CTest as chat_self_check.
#include "chat_router.h"
#include "config.h"

#include <auth/jwt_verify.h>
#include <codec.h>

#include <cstdio>
#include <map>
#include <source_location>

using namespace chthonic;
using namespace chthonic::chat;
using namespace std::chrono_literals;
using proto::ChatNoticeCode;
using proto::ChatTarget;

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
const SysTime W0{std::chrono::seconds(1'800'000'000)};

template <typename T>
void roundtrip(const T& msg, const char* name) {
    const auto frame = proto::encode(proto::Message{msg});
    const auto d = proto::decode_frame(frame, 4096);
    check(d.status == proto::DecodeStatus::ok && std::holds_alternative<T>(d.msg) && std::get<T>(d.msg) == msg, name);
}

void checkCodec() {
    using namespace proto;
    check(kProtocolVersion == 9, "protocol version 9");
    roundtrip(ChatSend{ChatTarget::zone, "", "anyone?"}, "ChatSend");
    roundtrip(ChatJoin{"Trade"}, "ChatJoin");
    roundtrip(ChatLeave{"Trade"}, "ChatLeave");
    roundtrip(ChatIgnore{"Mallory", false}, "ChatIgnore");
    roundtrip(ChatMute{"Mallory", 60}, "ChatMute");
    roundtrip(ChatLine{ChatTarget::whisper, "", "Alice", "psst"}, "ChatLine");
    roundtrip(ChatChannel{1, "General", false}, "ChatChannel");
    roundtrip(ChatNotice{ChatNoticeCode::not_online, "Bob", 0}, "ChatNotice");
    roundtrip(LinkHello{8, "0123456789abcdef"}, "LinkHello");
    roundtrip(CharOnline{1, 2, "Alice", 10}, "CharOnline");
    roundtrip(MuteState{"Bob", 123}, "MuteState");
    // ChatSend: u8 target, str to, str text
    check(encode(Message{ChatSend{ChatTarget::channel, "T", "x"}}) ==
              std::vector<uint8_t>{7, 0, 0x30, 0, 1, 1, 0, 'T', 1, 0, 'x'},
          "ChatSend bytes");
}

// Everything the router sends, by session; mutes it persisted and sent to the links.
struct Out {
    std::map<uint32_t, std::vector<proto::Message>> bySession;
    std::vector<proto::MuteState> links;
    std::vector<MuteChange> persisted;
    template <typename T>
    std::vector<T> of(uint32_t sid) const {
        std::vector<T> out;
        if (auto it = bySession.find(sid); it != bySession.end())
            for (const auto& m : it->second)
                if (auto* t = std::get_if<T>(&m)) out.push_back(*t);
        return out;
    }
    void clear() { bySession.clear(); links.clear(); persisted.clear(); }
    std::vector<proto::ChatLine> lines(uint32_t sid) const { return of<proto::ChatLine>(sid); }
    bool got(uint32_t sid, ChatTarget target, const std::string& name, const std::string& text) const {
        for (const auto& l : lines(sid))
            if (l.target == target && l.name == name && l.text == text) return true;
        return false;
    }
    std::optional<proto::ChatNotice> notice(uint32_t sid) const {
        const auto n = of<proto::ChatNotice>(sid);
        return n.empty() ? std::nullopt : std::optional(n.back());
    }
};

struct Rig {
    Out out;
    ChatRouter router;
    explicit Rig(RouterRules rules = {})
        : router(
              std::move(rules), [this](uint32_t sid, const proto::Message& m) { out.bySession[sid].push_back(m); },
              [this](const proto::MuteState& m) { out.links.push_back(m); },
              [this](const MuteChange& m) { out.persisted.push_back(m); }) {}
    // A character on link 1 with chat session sid (0: no chat client), account = char id.
    void online(uint64_t id, const std::string& name, uint16_t map, uint32_t sid, proto::Role role = proto::Role::player) {
        router.char_online(1, {id, id, name, map});
        if (sid) router.bind(sid, id, role);
    }
    void send(uint32_t sid, ChatTarget target, const std::string& to, const std::string& text, TimePoint now = T0,
              SysTime wall = W0) {
        router.handle(sid, proto::ChatSend{target, to, text}, now, wall);
    }
};

// Alice (session 1, map 10), Bob (2, map 11), Carol (3, map 10), Dan (map 10, no chat client), Gina (5, map 11, GM).
void populate(Rig& r) {
    r.online(11, "Alice", 10, 1);
    r.online(12, "Bob", 11, 2);
    r.online(13, "Carol", 10, 3);
    r.online(14, "Dan", 10, 0);
    r.online(15, "Gina", 11, 5, proto::Role::gm);
}

void checkBinding() {
    Rig r;
    check(!r.router.can_bind(11), "no character online: can't bind (held by the server)");
    r.router.char_online(1, {11, 11, "Alice", 10});
    check(r.router.can_bind(11), "after CharOnline the account can bind");
    check(!r.router.bind(1, 11, proto::Role::player), "first bind replaces nothing");
    const auto joined = r.out.of<proto::ChatChannel>(1);
    check(joined.size() == 1 && joined[0].number == 1 && joined[0].channel == "General" && joined[0].joined,
          "bind auto-joins General as channel 1");
    check(r.router.bind(7, 11, proto::Role::player) == std::optional<uint32_t>(1), "a second client replaces the first");
    check(r.router.name_of_session(7) == std::optional<std::string>("Alice") && !r.router.name_of_session(1),
          "the new session speaks for Alice, the old one for nobody");
    check(r.router.char_offline(11) == std::vector<uint32_t>{7} && !r.router.can_bind(11),
          "CharOffline: its client is returned (to disconnect), the account can't bind");
    r.online(12, "Bob", 11, 2);
    r.online(13, "Carol", 10, 0);
    r.router.char_online(2, {14, 14, "Dan", 10});
    check(r.router.link_lost(1) == std::vector<uint32_t>{2} && r.router.online() == 1,
          "a lost link takes its characters offline (Bob's client returned), the other link's stay");
    r.online(16, "Eve", 10, 8);
    check(r.router.char_online(2, {16, 17, "Evelyn", 10}) == std::vector<uint32_t>{8},
          "the same account online with another character: the old one's client is returned");
}

void checkWhispers() {
    Rig r;
    populate(r);
    r.out.clear();
    r.send(1, ChatTarget::whisper, "bob", "psst");
    check(r.out.got(2, ChatTarget::whisper, "Alice", "psst"), "whisper reaches Bob on another map (name in any case)");
    check(r.out.got(1, ChatTarget::whisper_sent, "Bob", "psst"), "the sender gets the echo with the canonical name");
    check(r.out.lines(3).empty(), "nobody else hears it");
    r.out.clear();
    r.send(1, ChatTarget::whisper, "Nobody", "hello?");
    check(r.out.notice(1) && r.out.notice(1)->code == ChatNoticeCode::not_online && r.out.notice(1)->name == "Nobody",
          "whisper to an offline name -> not_online");
    r.send(1, ChatTarget::whisper, "Dan", "hello?");
    check(r.out.notice(1)->code == ChatNoticeCode::not_online, "whisper to a character without a chat client -> not_online");
    r.send(1, ChatTarget::whisper, "B0b!", "hello?");
    check(r.out.notice(1)->code == ChatNoticeCode::bad_name, "whisper to an invalid name -> bad_name");
}

void checkChannels() {
    Rig r;
    populate(r);
    r.out.clear();
    r.send(1, ChatTarget::channel, "General", "hi all");
    for (uint32_t sid : {1u, 2u, 3u, 5u}) check(r.out.got(sid, ChatTarget::channel, "Alice", "hi all"), "General reaches every member");
    check(r.out.lines(1).at(0).channel == "General", "the line names its channel");
    r.out.clear();
    r.router.handle(1, proto::ChatJoin{"trade"}, T0, W0);
    const auto join = r.out.of<proto::ChatChannel>(1);
    check(join.size() == 1 && join[0].number == 2 && join[0].channel == "Trade" && join[0].joined,
          "join trade -> ChatChannel{2, Trade, joined}: numbered by config order");
    r.send(1, ChatTarget::channel, "Trade", "WTS sword");
    check(r.out.got(1, ChatTarget::channel, "Alice", "WTS sword") && r.out.lines(2).empty() && r.out.lines(3).empty(),
          "Trade reaches its only member, not the non-members");
    r.send(2, ChatTarget::channel, "Trade", "me too");
    check(r.out.notice(2)->code == ChatNoticeCode::not_in_channel && r.out.notice(2)->name == "Trade",
          "a non-member sending to Trade -> not_in_channel");
    r.router.handle(2, proto::ChatJoin{"Trade"}, T0, W0);
    r.out.clear();
    r.send(1, ChatTarget::channel, "Trade", "WTB shield");
    check(r.out.got(2, ChatTarget::channel, "Alice", "WTB shield"), "after joining, Bob hears Trade");
    r.router.handle(2, proto::ChatLeave{"Trade"}, T0, W0);
    check(!r.out.of<proto::ChatChannel>(2).back().joined, "leave -> ChatChannel{joined 0}");
    r.out.clear();
    r.send(1, ChatTarget::channel, "Trade", "anyone?");
    check(r.out.lines(2).empty(), "after leaving, Bob no longer hears Trade");
    r.router.handle(2, proto::ChatLeave{"Trade"}, T0, W0);
    check(r.out.notice(2)->code == ChatNoticeCode::not_in_channel, "leaving a channel you are not in -> not_in_channel");
    r.router.handle(1, proto::ChatJoin{"Guild"}, T0, W0);
    check(r.out.notice(1)->code == ChatNoticeCode::no_channel && r.out.notice(1)->name == "Guild",
          "joining an unknown channel -> no_channel (no player-made channels)");
}

void checkZone() {
    Rig r;
    populate(r);
    r.out.clear();
    r.send(1, ChatTarget::zone, "", "anyone near?");
    check(r.out.got(1, ChatTarget::zone, "Alice", "anyone near?") && r.out.got(3, ChatTarget::zone, "Alice", "anyone near?"),
          "zone reaches the characters on the sender's map");
    check(r.out.lines(2).empty() && r.out.lines(5).empty(), "zone: nobody on map 11");
    r.router.map_changed({12, 10}); // Bob walks into map 10
    r.router.map_changed({13, 11}); // Carol leaves it
    r.out.clear();
    r.send(1, ChatTarget::zone, "", "again");
    check(r.out.got(2, ChatTarget::zone, "Alice", "again") && r.out.lines(3).empty(), "map changes move zone membership");
}

void checkIgnore() {
    Rig r;
    populate(r);
    r.router.handle(2, proto::ChatIgnore{"alice", true}, T0, W0);
    check(r.out.notice(2)->code == ChatNoticeCode::ignored, "ignore -> ignored notice");
    r.out.clear();
    r.send(1, ChatTarget::whisper, "Bob", "w");
    r.send(1, ChatTarget::channel, "General", "c");
    r.send(1, ChatTarget::zone, "", "z");
    r.router.map_changed({12, 10});
    r.send(1, ChatTarget::zone, "", "z2");
    check(r.out.lines(2).empty(), "an ignored sender's whisper, channel and zone lines never reach Bob");
    check(r.out.got(1, ChatTarget::whisper_sent, "Bob", "w") && r.out.got(3, ChatTarget::channel, "Alice", "c"),
          "the sender still gets its echo; others still hear it");
    r.router.handle(2, proto::ChatIgnore{"Alice", false}, T0, W0);
    check(r.out.notice(2)->code == ChatNoticeCode::unignored, "unignore -> unignored notice");
    r.out.clear();
    r.send(1, ChatTarget::whisper, "Bob", "back", T0 + 20s);
    check(r.out.got(2, ChatTarget::whisper, "Alice", "back"), "after unignore Bob hears Alice again");
}

void checkMutes() {
    Rig r;
    populate(r);
    r.out.clear();
    r.router.handle(1, proto::ChatMute{"Bob", 10}, T0, W0);
    check(r.out.notice(1)->code == ChatNoticeCode::not_allowed && r.out.persisted.empty() && r.out.links.empty() &&
              !r.router.mutes().minutes_left("Bob", W0),
          "a player can't mute: not_allowed, nothing persisted or sent");
    r.router.handle(5, proto::ChatMute{"bob", 10}, T0, W0);
    check(r.out.notice(5)->code == ChatNoticeCode::mute_set && r.out.notice(5)->name == "Bob" && r.out.notice(5)->minutes == 10,
          "a GM mutes Bob for 10 minutes: mute_set");
    check(r.out.notice(2)->code == ChatNoticeCode::you_muted && r.out.notice(2)->minutes == 10, "Bob is told: you_muted 10");
    check(r.out.persisted.size() == 1 && r.out.persisted[0].name == "Bob" && r.out.persisted[0].until == W0 + 10min &&
              r.out.persisted[0].byAccount == 15,
          "the mute is persisted (name, end, the GM's account)");
    check(r.out.links.size() == 1 && r.out.links[0].untilMs == 1'800'000'600'000ull, "every world link gets MuteState{Bob, end}");
    r.out.clear();
    r.send(2, ChatTarget::whisper, "Alice", "let me talk", T0, W0 + 1min);
    r.send(2, ChatTarget::channel, "General", "let me talk", T0, W0 + 1min);
    r.send(2, ChatTarget::zone, "", "let me talk", T0, W0 + 1min);
    check(r.out.lines(1).empty() && r.out.lines(3).empty() && r.out.lines(5).empty(), "muted: whisper, channel and zone dropped");
    const auto muted = r.out.of<proto::ChatNotice>(2);
    check(muted.size() == 3 && muted[0].code == ChatNoticeCode::muted && muted[0].minutes == 9, "muted: a notice with 9 minutes left");
    r.out.clear();
    r.send(2, ChatTarget::channel, "General", "free", T0 + 20s, W0 + 10min);
    check(r.out.got(1, ChatTarget::channel, "Bob", "free"), "the mute expires after its 10 minutes");
    r.router.handle(5, proto::ChatMute{"Bob", 10}, T0, W0 + 11min);
    r.out.clear();
    r.router.handle(5, proto::ChatMute{"Bob", 0}, T0, W0 + 12min);
    check(r.out.notice(5)->code == ChatNoticeCode::mute_cleared && r.out.notice(2)->code == ChatNoticeCode::you_unmuted,
          "unmute (0 minutes): mute_cleared to the GM, you_unmuted to Bob");
    check(r.out.persisted.size() == 1 && r.out.persisted[0].until == SysTime{} && r.out.links.at(0).untilMs == 0,
          "unmute: deleted in the store, MuteState{Bob, 0} to the links");
    r.send(2, ChatTarget::zone, "", "free again", T0 + 30s, W0 + 12min);
    check(r.out.got(2, ChatTarget::zone, "Bob", "free again"), "unmuted Bob talks again");
    r.router.handle(5, proto::ChatMute{"Offline", 5}, T0, W0);
    check(r.router.mutes().minutes_left("offline", W0) == 5u, "a character that isn't online can be muted too (by name)");
    r.router.load_mute("Loaded", W0 + 1min);
    check(r.router.expire_mutes(W0 + 2min) == 1 && !r.router.mutes().minutes_left("Loaded", W0) &&
              r.router.mutes().minutes_left("Offline", W0 + 2min) == 3u,
          "expired mutes are purged, running ones kept");
    r.router.handle(5, proto::ChatMute{"Bob", 99999999}, T0, W0);
    check(r.out.notice(5)->minutes == 525600, "a mute is capped at CHAT_MUTE_MAX_MINUTES");
}

void checkLimits() {
    Rig r;
    populate(r);
    r.out.clear();
    for (int i = 0; i < 6; ++i) r.send(1, ChatTarget::channel, "General", "spam " + std::to_string(i), T0 + std::chrono::milliseconds(i));
    check(r.out.lines(3).size() == 5 && r.out.notice(1)->code == ChatNoticeCode::rate_limited,
          "5 per 10 s per sender: the 6th is dropped with rate_limited");
    r.out.clear();
    r.send(1, ChatTarget::channel, "General", "bad\x01\x1b[31m\ttext", T0 + 10s);
    check(r.out.got(3, ChatTarget::channel, "Alice", "bad[31mtext"), "control characters stripped");
    r.send(1, ChatTarget::channel, "General", std::string(257, 'x'), T0 + 30s);
    check(r.out.notice(1)->code == ChatNoticeCode::rejected, "over CHAT_MAX_BYTES -> rejected");
    r.send(1, ChatTarget::channel, "General", "bad \xC3", T0 + 50s);
    check(r.out.notice(1)->code == ChatNoticeCode::rejected, "invalid UTF-8 -> rejected");
    r.send(1, ChatTarget::channel, "General", "\n\t", T0 + 70s);
    check(r.out.notice(1)->code == ChatNoticeCode::rejected, "only control characters -> rejected");
    r.router.handle(99, proto::ChatSend{ChatTarget::zone, "", "who am I"}, T0 + 90s, W0);
    check(r.out.bySession.count(99) == 0, "an unbound session is ignored");
}

void checkConfig() {
    std::map<std::string, std::string> env = {{"AUTH_JWKS_URL", "http://auth:3000/.well-known/jwks.json"},
                                              {"DB_HOST", "mysql"}, {"DB_USER", "u"}, {"DB_PASSWORD", "p"},
                                              {"DB_NAME", "chthonic_chat"}, {"CHAT_LINK_SECRET", "0123456789abcdef"}};
    auto lookup = [&](const char* n) -> std::optional<std::string> {
        auto it = env.find(n);
        return it == env.end() ? std::nullopt : std::optional(it->second);
    };
    const auto ok = load_config(lookup);
    check(ok.errors.empty() && ok.config.port == 7778 && ok.config.linkPort == 7779 && ok.config.bindTimeoutMs == 5000,
          "minimal config valid; ports 7778 / 7779");
    check(ok.config.channels == std::vector<std::string>{"General", "Trade", "LFG"}, "default channels General, Trade, LFG");
    env["CHAT_CHANNELS"] = " World , Trade,,";
    check(load_config(lookup).config.channels == std::vector<std::string>{"World", "Trade"}, "CHAT_CHANNELS read in order");
    env["CHAT_CHANNELS"] = "Good,Bad Name";
    check(load_config(lookup).errors.size() == 1, "a channel name with a space refused");
    env["CHAT_CHANNELS"] = "";
    check(load_config(lookup).errors.size() == 1, "no channels refused");
    env.erase("CHAT_CHANNELS");
    env["CHAT_LINK_SECRET"] = "short";
    check(load_config(lookup).errors.size() == 1, "a link secret under 16 characters refused");
    env.erase("CHAT_LINK_SECRET");
    env["CHAT_LINK_PORT"] = "7778";
    check(load_config(lookup).errors.size() == 2, "missing secret and the link port equal to CHAT_PORT both reported");
}

void checkTokens() {
    const auto keyFor = [](const std::string&) { return KeyLookup{KeyStatus::unknown_kid, ""}; };
    const JwtRules rules{"chthonic-auth", "chthonic", 5};
    check(verify_access_token("not-a-jwt", keyFor, rules, std::chrono::system_clock::now()).code == proto::Code::invalid_token,
          "a garbage token is invalid_token (the world server's verifier, shared)");
}

} // namespace

int main() {
    checkCodec();
    checkBinding();
    checkWhispers();
    checkChannels();
    checkZone();
    checkIgnore();
    checkMutes();
    checkLimits();
    checkConfig();
    checkTokens();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
