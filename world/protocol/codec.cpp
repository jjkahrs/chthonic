#include "codec.h"

namespace chthonic::proto {
namespace {

template <size_t I = 0>
DecodeStatus decode_as(Opcode op, std::span<const uint8_t> payload, Message& out) {
    if constexpr (I == std::variant_size_v<Message>) {
        return DecodeStatus::unknown_opcode;
    } else {
        using T = std::variant_alternative_t<I, Message>;
        if (T::op != op) return decode_as<I + 1>(op, payload, out);
        T m;
        Reader r(payload);
        m.read(r);
        if (!r.done()) return DecodeStatus::malformed; // short, bad value, or trailing bytes
        out = std::move(m);
        return DecodeStatus::ok;
    }
}

} // namespace

Opcode opcode_of(const Message& m) {
    return std::visit([](const auto& v) { return std::decay_t<decltype(v)>::op; }, m);
}

const char* opcode_name(Opcode op) {
    switch (op) {
    case Opcode::Hello: return "Hello";
    case Opcode::Auth: return "Auth";
    case Opcode::Ping: return "Ping";
    case Opcode::Pong: return "Pong";
    case Opcode::CharListReq: return "CharListReq";
    case Opcode::CharCreate: return "CharCreate";
    case Opcode::CharDelete: return "CharDelete";
    case Opcode::EnterWorld: return "EnterWorld";
    case Opcode::Move: return "Move";
    case Opcode::Say: return "Say";
    case Opcode::ClientEvent: return "ClientEvent";
    case Opcode::Interact: return "Interact";
    case Opcode::AuthOk: return "AuthOk";
    case Opcode::Disconnect: return "Disconnect";
    case Opcode::CharList: return "CharList";
    case Opcode::CharCreated: return "CharCreated";
    case Opcode::CharDeleted: return "CharDeleted";
    case Opcode::EnterWorldOk: return "EnterWorldOk";
    case Opcode::CharError: return "CharError";
    case Opcode::Spawn: return "Spawn";
    case Opcode::Despawn: return "Despawn";
    case Opcode::EntityMoves: return "EntityMoves";
    case Opcode::Correction: return "Correction";
    case Opcode::ChatMsg: return "ChatMsg";
    case Opcode::ComponentUpdate: return "ComponentUpdate";
    case Opcode::ComponentRemove: return "ComponentRemove";
    case Opcode::ServerEvent: return "ServerEvent";
    case Opcode::MapChange: return "MapChange";
    case Opcode::Reply: return "Reply";
    case Opcode::ChatServer: return "ChatServer";
    case Opcode::ChatSend: return "ChatSend";
    case Opcode::ChatJoin: return "ChatJoin";
    case Opcode::ChatLeave: return "ChatLeave";
    case Opcode::ChatIgnore: return "ChatIgnore";
    case Opcode::ChatMute: return "ChatMute";
    case Opcode::ChatLine: return "ChatLine";
    case Opcode::ChatChannel: return "ChatChannel";
    case Opcode::ChatNotice: return "ChatNotice";
    case Opcode::LinkHello: return "LinkHello";
    case Opcode::LinkOk: return "LinkOk";
    case Opcode::CharOnline: return "CharOnline";
    case Opcode::MapChanged: return "MapChanged";
    case Opcode::CharOffline: return "CharOffline";
    case Opcode::MuteState: return "MuteState";
    case Opcode::AdminOnlineReq: return "AdminOnlineReq";
    case Opcode::AdminOnline: return "AdminOnline";
    case Opcode::AdminKick: return "AdminKick";
    case Opcode::AdminKicked: return "AdminKicked";
    case Opcode::AdminInstancesReq: return "AdminInstancesReq";
    case Opcode::AdminInstances: return "AdminInstances";
    }
    return "unknown";
}

const char* code_name(Code c) {
    switch (c) {
    case Code::ok: return "ok";
    case Code::protocol_mismatch: return "protocol_mismatch";
    case Code::malformed: return "malformed";
    case Code::unknown_opcode: return "unknown_opcode";
    case Code::frame_too_large: return "frame_too_large";
    case Code::rate_limited: return "rate_limited";
    case Code::handshake_timeout: return "handshake_timeout";
    case Code::idle_timeout: return "idle_timeout";
    case Code::invalid_token: return "invalid_token";
    case Code::token_expired: return "token_expired";
    case Code::service_unavailable: return "service_unavailable";
    case Code::logged_in_elsewhere: return "logged_in_elsewhere";
    case Code::server_shutdown: return "server_shutdown";
    case Code::server_full: return "server_full";
    case Code::unexpected_message: return "unexpected_message";
    case Code::name_taken: return "name_taken";
    case Code::name_invalid: return "name_invalid";
    case Code::char_limit: return "char_limit";
    case Code::not_found: return "not_found";
    case Code::already_in_world: return "already_in_world";
    case Code::options_invalid: return "options_invalid";
    case Code::no_character: return "no_character";
    case Code::kicked: return "kicked";
    }
    return "unknown";
}

const char* chat_notice_name(ChatNoticeCode c) {
    switch (c) {
    case ChatNoticeCode::not_online: return "not_online";
    case ChatNoticeCode::muted: return "muted";
    case ChatNoticeCode::not_allowed: return "not_allowed";
    case ChatNoticeCode::no_channel: return "no_channel";
    case ChatNoticeCode::not_in_channel: return "not_in_channel";
    case ChatNoticeCode::rate_limited: return "rate_limited";
    case ChatNoticeCode::rejected: return "rejected";
    case ChatNoticeCode::bad_name: return "bad_name";
    case ChatNoticeCode::ignored: return "ignored";
    case ChatNoticeCode::unignored: return "unignored";
    case ChatNoticeCode::mute_set: return "mute_set";
    case ChatNoticeCode::mute_cleared: return "mute_cleared";
    case ChatNoticeCode::you_muted: return "you_muted";
    case ChatNoticeCode::you_unmuted: return "you_unmuted";
    }
    return "unknown";
}

void encode(std::vector<uint8_t>& out, const Message& m) {
    const size_t start = out.size();
    Writer w(out);
    w.u16(0); // length, patched below
    w.u16(static_cast<uint16_t>(opcode_of(m)));
    std::visit([&](const auto& v) { v.write(w); }, m);
    const size_t len = out.size() - start - kHeaderSize;
    out[start] = static_cast<uint8_t>(len);
    out[start + 1] = static_cast<uint8_t>(len >> 8);
}

std::vector<uint8_t> encode(const Message& m) {
    std::vector<uint8_t> out;
    encode(out, m);
    return out;
}

Header parse_header(std::span<const uint8_t> bytes) {
    Reader r(bytes.first(kHeaderSize));
    Header h;
    h.length = r.u16();
    h.opcode = static_cast<Opcode>(r.u16());
    return h;
}

DecodeStatus decode_payload(Opcode op, std::span<const uint8_t> payload, Message& out) {
    return decode_as(op, payload, out);
}

Decoded decode_frame(std::span<const uint8_t> buf, size_t maxPayload) {
    Decoded d;
    if (buf.size() < kHeaderSize) return d;
    const Header h = parse_header(buf);
    if (h.length > maxPayload) { d.status = DecodeStatus::too_large; return d; }
    if (buf.size() < kHeaderSize + h.length) return d;
    d.status = decode_payload(h.opcode, buf.subspan(kHeaderSize, h.length), d.msg);
    if (d.status == DecodeStatus::ok) d.consumed = kHeaderSize + h.length;
    return d;
}

} // namespace chthonic::proto
