// Every wire message: opcode, struct and payload layout (see docs/world/protocol.html, and
// docs/chat/protocol.html for the chat service's). C = client->server, S = server->client; the chat service
// speaks the same frames (Hello, Auth, Ping/Pong, AuthOk, Disconnect, then the Chat* messages), and L = the internal
// link between a world server (client end) and the chat service. A = the world server's admin port (an admin tool is
// the client end).
#pragma once
#include "wire.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chthonic::proto {

constexpr uint16_t kProtocolVersion = 9;
// The admin port's own version (its LinkHello): 10 added AdminInstancesReq / AdminInstances. The game protocol, the
// chat service and its link stay on kProtocolVersion.
constexpr uint16_t kAdminProtocolVersion = 10;
constexpr size_t kHeaderSize = 4; // u16 payload length + u16 opcode
constexpr size_t kMaxPayloadBytes = 0xFFFF; // the u16 length field's limit

enum class Opcode : uint16_t {
    Hello = 0x0001,        // C
    Auth = 0x0002,         // C
    Ping = 0x0003,         // C/S
    Pong = 0x0004,         // C/S
    CharListReq = 0x0010,  // C
    CharCreate = 0x0011,   // C
    CharDelete = 0x0012,   // C
    EnterWorld = 0x0013,   // C
    Move = 0x0020,         // C
    Say = 0x0021,          // C
    ClientEvent = 0x0022,  // C
    Interact = 0x0023,     // C
    ChatSend = 0x0030,     // C (chat service)
    ChatJoin = 0x0031,     // C (chat service)
    ChatLeave = 0x0032,    // C (chat service)
    ChatIgnore = 0x0033,   // C (chat service)
    ChatMute = 0x0034,     // C (chat service)
    AuthOk = 0x0102,       // S
    Disconnect = 0x0103,   // S
    CharList = 0x0110,     // S
    CharCreated = 0x0111,  // S
    CharDeleted = 0x0112,  // S
    EnterWorldOk = 0x0113, // S
    CharError = 0x0114,    // S
    Spawn = 0x0120,        // S
    Despawn = 0x0121,      // S
    EntityMoves = 0x0122,  // S
    Correction = 0x0123,   // S
    ChatMsg = 0x0124,      // S
    ComponentUpdate = 0x0125, // S
    ComponentRemove = 0x0126, // S
    ServerEvent = 0x0127,     // S
    MapChange = 0x0128,       // S
    Reply = 0x0129,           // S
    ChatServer = 0x012A,      // S
    ChatLine = 0x0130,        // S (chat service)
    ChatChannel = 0x0131,     // S (chat service)
    ChatNotice = 0x0132,      // S (chat service; the world server for a muted say)
    LinkHello = 0x0200,       // L world -> chat
    LinkOk = 0x0201,          // L chat -> world
    CharOnline = 0x0202,      // L world -> chat
    MapChanged = 0x0203,      // L world -> chat
    CharOffline = 0x0204,     // L world -> chat
    MuteState = 0x0205,       // L chat -> world
    AdminOnlineReq = 0x0300,  // A admin -> world
    AdminOnline = 0x0301,     // A world -> admin
    AdminKick = 0x0302,       // A admin -> world
    AdminKicked = 0x0303,     // A world -> admin
    AdminInstancesReq = 0x0304, // A admin -> world
    AdminInstances = 0x0305,    // A world -> admin
};

// Reason codes carried by Disconnect and CharError.
enum class Code : uint16_t {
    ok = 0,
    protocol_mismatch = 1,
    malformed = 2,
    unknown_opcode = 3,
    frame_too_large = 4,
    rate_limited = 5,
    handshake_timeout = 6,
    idle_timeout = 7,
    invalid_token = 8,
    token_expired = 9,
    service_unavailable = 10,
    logged_in_elsewhere = 11,
    server_shutdown = 12,
    server_full = 13,
    unexpected_message = 14,
    name_taken = 20,
    name_invalid = 21,
    char_limit = 22,
    not_found = 23,
    already_in_world = 24,
    options_invalid = 25,
    no_character = 26, // chat service: the account has no character in a world (yet)
    kicked = 27,       // an admin kicked this session (world admin port)
};

const char* code_name(Code c);

enum class Role : uint8_t { player = 0, gm = 1, admin = 2 };
enum class SpawnKind : uint8_t { player = 0, npc = 1 };

struct Vec3 {
    float x = 0, y = 0, z = 0;
    bool operator==(const Vec3&) const = default;
};
inline void write(Writer& w, const Vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
inline void read(Reader& r, Vec3& v) { v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); }

struct Hello {
    static constexpr Opcode op = Opcode::Hello;
    uint16_t protocolVersion = kProtocolVersion;
    void write(Writer& w) const { w.u16(protocolVersion); }
    void read(Reader& r) { protocolVersion = r.u16(); }
    bool operator==(const Hello&) const = default;
};

struct Auth {
    static constexpr Opcode op = Opcode::Auth;
    std::string accessToken;
    void write(Writer& w) const { w.str(accessToken); }
    void read(Reader& r) { accessToken = r.str(); }
    bool operator==(const Auth&) const = default;
};

struct Ping {
    static constexpr Opcode op = Opcode::Ping;
    uint32_t nonce = 0;
    void write(Writer& w) const { w.u32(nonce); }
    void read(Reader& r) { nonce = r.u32(); }
    bool operator==(const Ping&) const = default;
};

struct Pong {
    static constexpr Opcode op = Opcode::Pong;
    uint32_t nonce = 0;
    void write(Writer& w) const { w.u32(nonce); }
    void read(Reader& r) { nonce = r.u32(); }
    bool operator==(const Pong&) const = default;
};

struct CharListReq {
    static constexpr Opcode op = Opcode::CharListReq;
    void write(Writer&) const {}
    void read(Reader&) {}
    bool operator==(const CharListReq&) const = default;
};

// Character creation options: string pairs the game defines (a class, a look, ...); the protocol and the client only
// carry them. u8 count, then count x (str key, str value); a key twice is malformed. The server takes at most
// kCharOptionsMax entries, keys of 1-kCharOptionKeyMax chars of [a-z_], values of up to kCharOptionValueMax printable
// ASCII chars (valid_char_options), and its Lua decides the rest (docs/world/lua.html#character-creation).
using CharOptions = std::map<std::string, std::string>;
constexpr size_t kCharOptionsMax = 8;
constexpr size_t kCharOptionKeyMax = 16;
constexpr size_t kCharOptionValueMax = 32;
inline void write(Writer& w, const CharOptions& o) {
    size_t n = 0;
    w.u8(static_cast<uint8_t>(o.size() > 255 ? 255 : o.size()));
    for (const auto& [key, value] : o) {
        if (n++ == 255) break;
        w.str(key);
        w.str(value);
    }
}
inline void read(Reader& r, CharOptions& o) {
    o.clear();
    for (uint8_t n = r.u8(); n > 0 && r.ok(); --n) {
        std::string key = r.str();
        if (!o.emplace(std::move(key), r.str()).second) r.fail();
    }
}

inline bool valid_char_options(const CharOptions& o) {
    if (o.size() > kCharOptionsMax) return false;
    for (const auto& [key, value] : o) {
        if (key.empty() || key.size() > kCharOptionKeyMax || value.size() > kCharOptionValueMax) return false;
        for (char ch : key)
            if (!((ch >= 'a' && ch <= 'z') || ch == '_')) return false;
        for (char ch : value)
            if (ch < 0x20 || ch > 0x7E) return false;
    }
    return true;
}

struct CharCreate {
    static constexpr Opcode op = Opcode::CharCreate;
    std::string name;
    CharOptions options = {};
    void write(Writer& w) const { w.str(name); proto::write(w, options); }
    void read(Reader& r) { name = r.str(); proto::read(r, options); }
    bool operator==(const CharCreate&) const = default;
};

struct CharDelete {
    static constexpr Opcode op = Opcode::CharDelete;
    uint64_t charId = 0;
    void write(Writer& w) const { w.u64(charId); }
    void read(Reader& r) { charId = r.u64(); }
    bool operator==(const CharDelete&) const = default;
};

struct EnterWorld {
    static constexpr Opcode op = Opcode::EnterWorld;
    uint64_t charId = 0;
    void write(Writer& w) const { w.u64(charId); }
    void read(Reader& r) { charId = r.u64(); }
    bool operator==(const EnterWorld&) const = default;
};

// v6: seq counts the client's Moves (from 1); epoch is the one of the last Correction it got (0 before any). The
// server drops a Move of an older epoch without a word: it was sent before the client heard of the Correction.
struct Move {
    static constexpr Opcode op = Opcode::Move;
    Vec3 pos;
    float heading = 0;
    uint32_t clientTimeMs = 0;
    uint32_t seq = 0;
    uint32_t epoch = 0;
    void write(Writer& w) const { proto::write(w, pos); w.f32(heading); w.u32(clientTimeMs); w.u32(seq); w.u32(epoch); }
    void read(Reader& r) { proto::read(r, pos); heading = r.f32(); clientTimeMs = r.u32(); seq = r.u32(); epoch = r.u32(); }
    bool operator==(const Move&) const = default;
};

// v8: proximity chat on the world server. say: the AOI audience; yell: everyone on the map within CHAT_YELL_RADIUS;
// emote: free text shown as "<name> <text>", to the say audience.
enum class ChatKind : uint8_t { say = 0, yell = 1, emote = 2 };
inline ChatKind read_chat_kind(Reader& r) {
    const uint8_t v = r.u8();
    if (v > static_cast<uint8_t>(ChatKind::emote)) r.fail();
    return static_cast<ChatKind>(v);
}

struct Say {
    static constexpr Opcode op = Opcode::Say;
    ChatKind kind = ChatKind::say;
    std::string text;
    void write(Writer& w) const { w.u8(static_cast<uint8_t>(kind)); w.str(text); }
    void read(Reader& r) { kind = read_chat_kind(r); text = r.str(); }
    bool operator==(const Say&) const = default;
};

// A named event for the server's Lua (docs/world/protocol.html#client-events). Never S->C.
// The server re-checks both limits: the name is untrusted, the JSON is parsed and must be an object.
// requestId: 0 for a plain event (no answer); otherwise a request, which the server's Lua answers with one Reply (or the
// server with Reply{requestId, false, "rate_limited"} if it drops it for the rate limit).
constexpr size_t kClientEventNameMax = 32;
constexpr size_t kClientEventJsonMax = 1024;
struct ClientEvent {
    static constexpr Opcode op = Opcode::ClientEvent;
    std::string name;
    std::string json;
    uint32_t requestId = 0;
    void write(Writer& w) const { w.str(name); w.str(json); w.u32(requestId); }
    void read(Reader& r) { name = r.str(); json = r.str(); requestId = r.u32(); }
    bool operator==(const ClientEvent&) const = default;
};

// Activates a zone object (docs/world/protocol.html#interact): the id of an interactable in the map's
// objects files. The server checks the map and the range and replies nothing; its Lua answers if it wants to.
struct Interact {
    static constexpr Opcode op = Opcode::Interact;
    uint32_t objectId = 0;
    void write(Writer& w) const { w.u32(objectId); }
    void read(Reader& r) { objectId = r.u32(); }
    bool operator==(const Interact&) const = default;
};

// 1-32 chars of [a-z0-9._].
inline bool valid_client_event_name(std::string_view name) {
    if (name.empty() || name.size() > kClientEventNameMax) return false;
    for (char ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_')) return false;
    return true;
}

struct AuthOk {
    static constexpr Opcode op = Opcode::AuthOk;
    uint64_t accountId = 0;
    Role role = Role::player;
    void write(Writer& w) const { w.u64(accountId); w.u8(static_cast<uint8_t>(role)); }
    void read(Reader& r) {
        accountId = r.u64();
        const uint8_t v = r.u8();
        if (v > static_cast<uint8_t>(Role::admin)) r.fail();
        role = static_cast<Role>(v);
    }
    bool operator==(const AuthOk&) const = default;
};

struct Disconnect {
    static constexpr Opcode op = Opcode::Disconnect;
    Code reason = Code::ok;
    void write(Writer& w) const { w.u16(static_cast<uint16_t>(reason)); }
    void read(Reader& r) { reason = static_cast<Code>(r.u16()); }
    bool operator==(const Disconnect&) const = default;
};

struct CharInfo {
    uint64_t id = 0;
    std::string name;
    Vec3 pos;
    CharOptions options = {}; // as created
    bool operator==(const CharInfo&) const = default;
};
inline void write(Writer& w, const CharInfo& c) { w.u64(c.id); w.str(c.name); write(w, c.pos); write(w, c.options); }
inline void read(Reader& r, CharInfo& c) { c.id = r.u64(); c.name = r.str(); read(r, c.pos); read(r, c.options); }

struct CharList {
    static constexpr Opcode op = Opcode::CharList;
    std::vector<CharInfo> chars; // at most 255 on the wire
    void write(Writer& w) const {
        const size_t n = chars.size() > 255 ? 255 : chars.size();
        w.u8(static_cast<uint8_t>(n));
        for (size_t i = 0; i < n; ++i) proto::write(w, chars[i]);
    }
    void read(Reader& r) {
        chars.resize(r.u8());
        for (auto& c : chars) proto::read(r, c);
    }
    bool operator==(const CharList&) const = default;
};

struct CharCreated {
    static constexpr Opcode op = Opcode::CharCreated;
    CharInfo character;
    void write(Writer& w) const { proto::write(w, character); }
    void read(Reader& r) { proto::read(r, character); }
    bool operator==(const CharCreated&) const = default;
};

struct CharDeleted {
    static constexpr Opcode op = Opcode::CharDeleted;
    uint64_t charId = 0;
    void write(Writer& w) const { w.u64(charId); }
    void read(Reader& r) { charId = r.u64(); }
    bool operator==(const CharDeleted&) const = default;
};

struct EnterWorldOk {
    static constexpr Opcode op = Opcode::EnterWorldOk;
    uint32_t entityId = 0;
    uint16_t map = 0; // map.json "id"
    Vec3 pos;
    float heading = 0;
    void write(Writer& w) const { w.u32(entityId); w.u16(map); proto::write(w, pos); w.f32(heading); }
    void read(Reader& r) { entityId = r.u32(); map = r.u16(); proto::read(r, pos); heading = r.f32(); }
    bool operator==(const EnterWorldOk&) const = default;
};

// Non-fatal error for a character request; the connection stays open.
struct CharError {
    static constexpr Opcode op = Opcode::CharError;
    Opcode request = Opcode::CharListReq;
    Code code = Code::ok;
    void write(Writer& w) const { w.u16(static_cast<uint16_t>(request)); w.u16(static_cast<uint16_t>(code)); }
    void read(Reader& r) { request = static_cast<Opcode>(r.u16()); code = static_cast<Code>(r.u16()); }
    bool operator==(const CharError&) const = default;
};

// kind npc: a server-created NPC; templateName names the client's template for it (empty for players).
struct Spawn {
    static constexpr Opcode op = Opcode::Spawn;
    uint32_t entityId = 0;
    std::string name;
    Vec3 pos;
    float heading = 0;
    SpawnKind kind = SpawnKind::player;
    std::string templateName;
    void write(Writer& w) const {
        w.u32(entityId); w.str(name); proto::write(w, pos); w.f32(heading);
        w.u8(static_cast<uint8_t>(kind)); w.str(templateName);
    }
    void read(Reader& r) {
        entityId = r.u32(); name = r.str(); proto::read(r, pos); heading = r.f32();
        const uint8_t v = r.u8();
        if (v > static_cast<uint8_t>(SpawnKind::npc)) r.fail();
        kind = static_cast<SpawnKind>(v);
        templateName = r.str();
    }
    bool operator==(const Spawn&) const = default;
};

struct Despawn {
    static constexpr Opcode op = Opcode::Despawn;
    uint32_t entityId = 0;
    void write(Writer& w) const { w.u32(entityId); }
    void read(Reader& r) { entityId = r.u32(); }
    bool operator==(const Despawn&) const = default;
};

// v9: every move a viewer sees in one tick, in one message (u16 count, then count x {u32 entityId, Vec3 pos, f32
// heading}); more than kMaxEntries go as several, in order.
struct EntityMoves {
    static constexpr Opcode op = Opcode::EntityMoves;
    static constexpr size_t kMaxEntries = (kMaxPayloadBytes - 2) / 20; // 3276 x 20 bytes stays under the 64 KB frame
    struct Entry {
        uint32_t entityId = 0;
        Vec3 pos;
        float heading = 0;
        bool operator==(const Entry&) const = default;
    };
    std::vector<Entry> entries;
    void write(Writer& w) const {
        w.u16(static_cast<uint16_t>(entries.size()));
        uint8_t* p = w.grow(entries.size() * 20); // as u32 entityId, Vec3 pos, f32 heading, in one go
        for (const auto& e : entries) {
            for (uint32_t v : {e.entityId, std::bit_cast<uint32_t>(e.pos.x), std::bit_cast<uint32_t>(e.pos.y),
                               std::bit_cast<uint32_t>(e.pos.z), std::bit_cast<uint32_t>(e.heading)}) {
                Writer::le32(p, v);
                p += 4;
            }
        }
    }
    void read(Reader& r) {
        entries.clear();
        const uint16_t n = r.u16();
        if (n > kMaxEntries) r.fail();
        for (uint16_t i = 0; i < n && r.ok(); ++i) {
            Entry e;
            e.entityId = r.u32(); proto::read(r, e.pos); e.heading = r.f32();
            entries.push_back(e);
        }
    }
    bool operator==(const EntityMoves&) const = default;
};

// v6: pos is where the server has the player after the Move ackSeq, the last one it processed (the rejected one);
// epoch is the new epoch, for the client's later Moves (docs/world/protocol.html#movement).
struct Correction {
    static constexpr Opcode op = Opcode::Correction;
    Vec3 pos;
    uint32_t ackSeq = 0;
    uint32_t epoch = 0;
    void write(Writer& w) const { proto::write(w, pos); w.u32(ackSeq); w.u32(epoch); }
    void read(Reader& r) { proto::read(r, pos); ackSeq = r.u32(); epoch = r.u32(); }
    bool operator==(const Correction&) const = default;
};

struct ChatMsg {
    static constexpr Opcode op = Opcode::ChatMsg;
    ChatKind kind = ChatKind::say;
    std::string fromName;
    std::string text;
    void write(Writer& w) const { w.u8(static_cast<uint8_t>(kind)); w.str(fromName); w.str(text); }
    void read(Reader& r) { kind = read_chat_kind(r); fromName = r.str(); text = r.str(); }
    bool operator==(const ChatMsg&) const = default;
};

// A Lua component's full value as JSON (docs/world/lua.html). Server-authoritative: never C->S.
struct ComponentUpdate {
    static constexpr Opcode op = Opcode::ComponentUpdate;
    uint32_t entityId = 0;
    std::string name;
    std::string json;
    void write(Writer& w) const { w.u32(entityId); w.str(name); w.str(json); }
    void read(Reader& r) { entityId = r.u32(); name = r.str(); json = r.str(); }
    bool operator==(const ComponentUpdate&) const = default;
    // Whether a component of these sizes fits one frame (str would truncate it otherwise).
    static bool fits(size_t nameBytes, size_t jsonBytes) { return 4 + 2 + nameBytes + 2 + jsonBytes <= kMaxPayloadBytes; }
};

struct ComponentRemove {
    static constexpr Opcode op = Opcode::ComponentRemove;
    uint32_t entityId = 0;
    std::string name;
    void write(Writer& w) const { w.u32(entityId); w.str(name); }
    void read(Reader& r) { entityId = r.u32(); name = r.str(); }
    bool operator==(const ComponentRemove&) const = default;
};

// A named event a Lua system sent with chthonic.sendTo/sendNearby/broadcast (docs/world/protocol.html#server-events). Never C->S.
struct ServerEvent {
    static constexpr Opcode op = Opcode::ServerEvent;
    std::string name;
    std::string json;
    void write(Writer& w) const { w.str(name); w.str(json); }
    void read(Reader& r) { name = r.str(); json = r.str(); }
    bool operator==(const ServerEvent&) const = default;
    // Whether an event of these sizes fits one frame (str would truncate it otherwise).
    static bool fits(size_t nameBytes, size_t jsonBytes) { return 2 + nameBytes + 2 + jsonBytes <= kMaxPayloadBytes; }
};

// The player was moved to another map (or another place on its own): drop every remote entity, place yourself here.
// Comes before the Despawns and Spawns of the move (docs/world/protocol.html#maps). epoch: the Moves' epoch
// from now on, as after a Correction (Moves sent before this arrived are from the old place: dropped, not corrected).
struct MapChange {
    static constexpr Opcode op = Opcode::MapChange;
    uint16_t map = 0;
    Vec3 pos;
    float heading = 0;
    uint32_t epoch = 0;
    void write(Writer& w) const { w.u16(map); proto::write(w, pos); w.f32(heading); w.u32(epoch); }
    void read(Reader& r) { map = r.u16(); proto::read(r, pos); heading = r.f32(); epoch = r.u32(); }
    bool operator==(const MapChange&) const = default;
};

// The answer to a ClientEvent's requestId (chthonic.reply), to its sender only. json: the reply's data when ok, else the
// reason as a JSON string.
struct Reply {
    static constexpr Opcode op = Opcode::Reply;
    uint32_t requestId = 0;
    bool ok = false;
    std::string json;
    void write(Writer& w) const { w.u32(requestId); w.u8(ok ? 1 : 0); w.str(json); }
    void read(Reader& r) {
        requestId = r.u32();
        const uint8_t v = r.u8();
        if (v > 1) r.fail();
        ok = v == 1;
        json = r.str();
    }
    bool operator==(const Reply&) const = default;
};

// --- chat service (docs/chat/protocol.html) ---

// Where the realm's chat service listens: sent by the world server as the next message after EnterWorldOk, before any
// other (CHAT_PUBLIC_HOST / CHAT_PUBLIC_PORT); not sent when it has none. The client opens a second TLS connection
// there with the same token.
struct ChatServer {
    static constexpr Opcode op = Opcode::ChatServer;
    std::string host;
    uint16_t port = 0;
    void write(Writer& w) const { w.str(host); w.u16(port); }
    void read(Reader& r) { host = r.str(); port = r.u16(); }
    bool operator==(const ChatServer&) const = default;
};

// Who a chat-service line goes to. whisper_sent: only in ChatLine, the echo of the sender's own whisper.
enum class ChatTarget : uint8_t { whisper = 0, channel = 1, zone = 2, whisper_sent = 3 };
inline ChatTarget read_chat_target(Reader& r, ChatTarget max) {
    const uint8_t v = r.u8();
    if (v > static_cast<uint8_t>(max)) r.fail();
    return static_cast<ChatTarget>(v);
}

// to: the character name for whisper, the channel name for channel, empty for zone (the sender's map).
struct ChatSend {
    static constexpr Opcode op = Opcode::ChatSend;
    ChatTarget target = ChatTarget::whisper;
    std::string to;
    std::string text;
    void write(Writer& w) const { w.u8(static_cast<uint8_t>(target)); w.str(to); w.str(text); }
    void read(Reader& r) { target = read_chat_target(r, ChatTarget::zone); to = r.str(); text = r.str(); }
    bool operator==(const ChatSend&) const = default;
};

struct ChatJoin {
    static constexpr Opcode op = Opcode::ChatJoin;
    std::string channel;
    void write(Writer& w) const { w.str(channel); }
    void read(Reader& r) { channel = r.str(); }
    bool operator==(const ChatJoin&) const = default;
};

struct ChatLeave {
    static constexpr Opcode op = Opcode::ChatLeave;
    std::string channel;
    void write(Writer& w) const { w.str(channel); }
    void read(Reader& r) { channel = r.str(); }
    bool operator==(const ChatLeave&) const = default;
};

// on 1: ignore name's whispers, channel and zone lines until logout; on 0: stop.
struct ChatIgnore {
    static constexpr Opcode op = Opcode::ChatIgnore;
    std::string name;
    bool on = true;
    void write(Writer& w) const { w.str(name); w.u8(on ? 1 : 0); }
    void read(Reader& r) {
        name = r.str();
        const uint8_t v = r.u8();
        if (v > 1) r.fail();
        on = v == 1;
    }
    bool operator==(const ChatIgnore&) const = default;
};

// GM / admin only: mutes name for minutes (0: unmutes).
struct ChatMute {
    static constexpr Opcode op = Opcode::ChatMute;
    std::string name;
    uint32_t minutes = 0;
    void write(Writer& w) const { w.str(name); w.u32(minutes); }
    void read(Reader& r) { name = r.str(); minutes = r.u32(); }
    bool operator==(const ChatMute&) const = default;
};

// A line for this client. channel: the channel's name (target channel), empty otherwise. name: the sender (whisper,
// channel, zone) or the recipient (whisper_sent).
struct ChatLine {
    static constexpr Opcode op = Opcode::ChatLine;
    ChatTarget target = ChatTarget::whisper;
    std::string channel;
    std::string name;
    std::string text;
    void write(Writer& w) const { w.u8(static_cast<uint8_t>(target)); w.str(channel); w.str(name); w.str(text); }
    void read(Reader& r) {
        target = read_chat_target(r, ChatTarget::whisper_sent);
        channel = r.str();
        name = r.str();
        text = r.str();
    }
    bool operator==(const ChatLine&) const = default;
};

// Channel membership changed: joined 1 with the channel's number (its place in CHAT_CHANNELS, from 1), joined 0 left.
struct ChatChannel {
    static constexpr Opcode op = Opcode::ChatChannel;
    uint8_t number = 0;
    std::string channel;
    bool joined = true;
    void write(Writer& w) const { w.u8(number); w.str(channel); w.u8(joined ? 1 : 0); }
    void read(Reader& r) {
        number = r.u8();
        channel = r.str();
        const uint8_t v = r.u8();
        if (v > 1) r.fail();
        joined = v == 1;
    }
    bool operator==(const ChatChannel&) const = default;
};

// Errors and confirmations for chat requests; the connection stays. name / minutes as the code says.
enum class ChatNoticeCode : uint8_t {
    not_online = 1,     // whisper: name has no chat connection
    muted = 2,          // you are muted for minutes more: your message was dropped (chat service and world server)
    not_allowed = 3,    // mute / unmute: GM or admin only
    no_channel = 4,     // join / leave / send: no such channel (name)
    not_in_channel = 5, // send / leave: you are not in channel name
    rate_limited = 6,   // over CHAT_RATE_COUNT per CHAT_RATE_WINDOW_MS: dropped
    rejected = 7,       // empty, over CHAT_MAX_BYTES or invalid UTF-8: dropped
    bad_name = 8,       // whisper / ignore / mute: not a valid character name
    ignored = 9,        // ignore: done (name)
    unignored = 10,     // unignore: done (name)
    mute_set = 11,      // mute: name muted for minutes
    mute_cleared = 12,  // unmute: name unmuted
    you_muted = 13,     // a GM muted you for minutes
    you_unmuted = 14,   // your mute was lifted
};

struct ChatNotice {
    static constexpr Opcode op = Opcode::ChatNotice;
    ChatNoticeCode code = ChatNoticeCode::not_online;
    std::string name;
    uint32_t minutes = 0;
    void write(Writer& w) const { w.u8(static_cast<uint8_t>(code)); w.str(name); w.u32(minutes); }
    void read(Reader& r) {
        const uint8_t v = r.u8();
        if (v < 1 || v > static_cast<uint8_t>(ChatNoticeCode::you_unmuted)) r.fail();
        code = static_cast<ChatNoticeCode>(v);
        name = r.str();
        minutes = r.u32();
    }
    bool operator==(const ChatNotice&) const = default;
};

const char* chat_notice_name(ChatNoticeCode c);

// --- internal link: world server -> chat service (CHAT_LINK_*), never from a game client ---

// First message on the link; a wrong secret is answered Disconnect{invalid_token}, a wrong version
// Disconnect{protocol_mismatch}.
struct LinkHello {
    static constexpr Opcode op = Opcode::LinkHello;
    uint16_t protocolVersion = kProtocolVersion;
    std::string secret;
    void write(Writer& w) const { w.u16(protocolVersion); w.str(secret); }
    void read(Reader& r) { protocolVersion = r.u16(); secret = r.str(); }
    bool operator==(const LinkHello&) const = default;
};

// The link is up: every current mute follows as MuteState; the world server then sends CharOnline for every character
// it has in the world.
struct LinkOk {
    static constexpr Opcode op = Opcode::LinkOk;
    void write(Writer&) const {}
    void read(Reader&) {}
    bool operator==(const LinkOk&) const = default;
};

struct CharOnline {
    static constexpr Opcode op = Opcode::CharOnline;
    uint64_t accountId = 0;
    uint64_t charId = 0;
    std::string name;
    uint16_t map = 0;
    void write(Writer& w) const { w.u64(accountId); w.u64(charId); w.str(name); w.u16(map); }
    void read(Reader& r) { accountId = r.u64(); charId = r.u64(); name = r.str(); map = r.u16(); }
    bool operator==(const CharOnline&) const = default;
};

struct MapChanged {
    static constexpr Opcode op = Opcode::MapChanged;
    uint64_t charId = 0;
    uint16_t map = 0;
    void write(Writer& w) const { w.u64(charId); w.u16(map); }
    void read(Reader& r) { charId = r.u64(); map = r.u16(); }
    bool operator==(const MapChanged&) const = default;
};

struct CharOffline {
    static constexpr Opcode op = Opcode::CharOffline;
    uint64_t charId = 0;
    void write(Writer& w) const { w.u64(charId); }
    void read(Reader& r) { charId = r.u64(); }
    bool operator==(const CharOffline&) const = default;
};

// name is muted until untilMs (Unix ms, wall clock); 0: not muted (any more).
struct MuteState {
    static constexpr Opcode op = Opcode::MuteState;
    std::string name;
    uint64_t untilMs = 0;
    void write(Writer& w) const { w.str(name); w.u64(untilMs); }
    void read(Reader& r) { name = r.str(); untilMs = r.u64(); }
    bool operator==(const MuteState&) const = default;
};

// --- admin port: an admin tool -> world server (WORLD_ADMIN_*), never from a game client ---
// Opens with LinkHello{kAdminProtocolVersion, secret = WORLD_ADMIN_SECRET}, answered LinkOk (or
// Disconnect{invalid_token / protocol_mismatch}); then any number of requests, each answered in order.

struct AdminOnlineReq {
    static constexpr Opcode op = Opcode::AdminOnlineReq;
    void write(Writer&) const {}
    void read(Reader&) {}
    bool operator==(const AdminOnlineReq&) const = default;
};

// An authenticated session: in the lobby (charId 0, name empty, map 0) or in the world.
struct AdminOnlineEntry {
    uint64_t accountId = 0;
    uint64_t charId = 0;
    std::string name;
    uint16_t map = 0;
    uint32_t sessionId = 0;
    uint64_t connectedAtMs = 0; // Unix ms, wall clock
    bool operator==(const AdminOnlineEntry&) const = default;
};

// The answer to AdminOnlineReq, in chunks of at most kMaxEntries: more 1 = another AdminOnline follows.
struct AdminOnline {
    static constexpr Opcode op = Opcode::AdminOnline;
    static constexpr size_t kMaxEntries = 1000; // 1000 x <= 48 bytes stays under the 64 KB frame
    bool more = false;
    std::vector<AdminOnlineEntry> entries;
    void write(Writer& w) const {
        w.u8(more ? 1 : 0);
        w.u16(static_cast<uint16_t>(entries.size()));
        for (const auto& e : entries) {
            w.u64(e.accountId); w.u64(e.charId); w.str(e.name); w.u16(e.map); w.u32(e.sessionId); w.u64(e.connectedAtMs);
        }
    }
    void read(Reader& r) {
        const uint8_t v = r.u8();
        if (v > 1) r.fail();
        more = v == 1;
        entries.clear();
        for (uint16_t n = r.u16(); n > 0 && r.ok(); --n) {
            AdminOnlineEntry e;
            e.accountId = r.u64(); e.charId = r.u64(); e.name = r.str(); e.map = r.u16(); e.sessionId = r.u32();
            e.connectedAtMs = r.u64();
            entries.push_back(std::move(e));
        }
    }
    bool operator==(const AdminOnline&) const = default;
};

// Kicks every session of accountId, or the session playing charId: exactly one of them non-zero, else malformed.
// Each gets Disconnect{kicked} after its character left the world (saved, as on a duplicate login).
struct AdminKick {
    static constexpr Opcode op = Opcode::AdminKick;
    uint64_t accountId = 0;
    uint64_t charId = 0;
    std::string reason; // logged by the world server; not sent to the client
    void write(Writer& w) const { w.u64(accountId); w.u64(charId); w.str(reason); }
    void read(Reader& r) {
        accountId = r.u64();
        charId = r.u64();
        reason = r.str();
        if ((accountId == 0) == (charId == 0)) r.fail();
    }
    bool operator==(const AdminKick&) const = default;
};

struct AdminKicked {
    static constexpr Opcode op = Opcode::AdminKicked;
    uint32_t kicked = 0; // sessions disconnected (0: nobody matched)
    void write(Writer& w) const { w.u32(kicked); }
    void read(Reader& r) { kicked = r.u32(); }
    bool operator==(const AdminKicked&) const = default;
};

struct AdminInstancesReq {
    static constexpr Opcode op = Opcode::AdminInstancesReq;
    void write(Writer&) const {}
    void read(Reader&) {}
    bool operator==(const AdminInstancesReq&) const = default;
};

// A live map context: a world map's (instance 0, key empty) or an instance copy's.
struct AdminInstanceEntry {
    uint16_t map = 0;
    std::string mapName;
    uint32_t instance = 0;
    std::string key; // empty: keyless (or a world map)
    uint32_t players = 0;
    uint32_t npcs = 0;
    uint64_t createdAtMs = 0;  // Unix ms, wall clock
    uint64_t emptySinceMs = 0; // a copy's empty timer started then (Unix ms); 0: it has players, or a world map
    bool operator==(const AdminInstanceEntry&) const = default;
};

// The answer to AdminInstancesReq, in chunks of at most kMaxEntries (more 1 = another follows), in map then instance
// order.
struct AdminInstances {
    static constexpr Opcode op = Opcode::AdminInstances;
    static constexpr size_t kMaxEntries = 100; // 100 x <= 548 bytes (two 255-byte strings) stays under the 64 KB frame
    bool more = false;
    std::vector<AdminInstanceEntry> entries;
    void write(Writer& w) const {
        w.u8(more ? 1 : 0);
        w.u16(static_cast<uint16_t>(entries.size()));
        for (const auto& e : entries) {
            w.u16(e.map); w.str(e.mapName); w.u32(e.instance); w.str(e.key); w.u32(e.players); w.u32(e.npcs);
            w.u64(e.createdAtMs); w.u64(e.emptySinceMs);
        }
    }
    void read(Reader& r) {
        const uint8_t v = r.u8();
        if (v > 1) r.fail();
        more = v == 1;
        entries.clear();
        for (uint16_t n = r.u16(); n > 0 && r.ok(); --n) {
            AdminInstanceEntry e;
            e.map = r.u16(); e.mapName = r.str(); e.instance = r.u32(); e.key = r.str(); e.players = r.u32();
            e.npcs = r.u32(); e.createdAtMs = r.u64(); e.emptySinceMs = r.u64();
            entries.push_back(std::move(e));
        }
    }
    bool operator==(const AdminInstances&) const = default;
};

using Message = std::variant<Hello, Auth, Ping, Pong, CharListReq, CharCreate, CharDelete, EnterWorld, Move, Say, ClientEvent, Interact,
                             AuthOk, Disconnect, CharList, CharCreated, CharDeleted, EnterWorldOk, CharError, Spawn,
                             Despawn, EntityMoves, Correction, ChatMsg, ComponentUpdate,
                             ComponentRemove, ServerEvent, MapChange, Reply, ChatServer, ChatSend, ChatJoin, ChatLeave,
                             ChatIgnore, ChatMute, ChatLine, ChatChannel, ChatNotice, LinkHello, LinkOk, CharOnline,
                             MapChanged, CharOffline, MuteState, AdminOnlineReq, AdminOnline, AdminKick, AdminKicked,
                             AdminInstancesReq, AdminInstances>;

} // namespace chthonic::proto
