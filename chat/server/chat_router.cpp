#include "chat_router.h"

#include <core/log.h>

#include <algorithm>

namespace chthonic::chat {

using proto::ChatNoticeCode;
using proto::ChatTarget;

ChatRouter::ChatRouter(RouterRules rules, Send send, ToLinks toLinks, Persist persist)
    : rules_(std::move(rules)), send_(std::move(send)), toLinks_(std::move(toLinks)), persist_(std::move(persist)) {}

// --- world links ---

std::vector<uint32_t> ChatRouter::char_online(uint32_t link, const proto::CharOnline& c) {
    if (auto it = chars_.find(c.charId); it != chars_.end()) { // reported again: same character, same client
        it->second.map = c.map;
        it->second.link = link;
        return {};
    }
    // The same name or account online elsewhere (a relog through another world): that one can't chat any more.
    std::vector<uint32_t> orphans;
    if (auto it = byName_.find(name_key(c.name)); it != byName_.end())
        if (auto s = remove(it->second); s && *s) orphans.push_back(*s);
    if (auto it = byAccount_.find(c.accountId); it != byAccount_.end())
        if (auto s = remove(it->second); s && *s) orphans.push_back(*s);
    chars_.emplace(c.charId, Character{c.charId, c.accountId, c.name, c.map, link, 0, proto::Role::player, {}, {},
                                       WindowLimiter(rules_.rateCount, rules_.rateWindow)});
    byName_[name_key(c.name)] = c.charId;
    byAccount_[c.accountId] = c.charId;
    log::debug("character online", {{"char", c.charId}, {"map", c.map}, {"link", link}});
    return orphans;
}

void ChatRouter::map_changed(const proto::MapChanged& m) {
    if (auto it = chars_.find(m.charId); it != chars_.end()) it->second.map = m.map;
}

std::vector<uint32_t> ChatRouter::char_offline(uint64_t charId) {
    std::vector<uint32_t> sessions;
    if (auto s = remove(charId); s && *s) sessions.push_back(*s);
    return sessions;
}

std::vector<uint32_t> ChatRouter::link_lost(uint32_t link) {
    std::vector<uint64_t> ids;
    for (const auto& [id, c] : chars_)
        if (c.link == link) ids.push_back(id);
    std::vector<uint32_t> sessions;
    for (uint64_t id : ids)
        if (auto s = remove(id); s && *s) sessions.push_back(*s);
    return sessions;
}

std::optional<uint32_t> ChatRouter::remove(uint64_t charId) {
    auto it = chars_.find(charId);
    if (it == chars_.end()) return std::nullopt;
    const Character& c = it->second;
    if (auto n = byName_.find(name_key(c.name)); n != byName_.end() && n->second == charId) byName_.erase(n);
    if (auto a = byAccount_.find(c.accountId); a != byAccount_.end() && a->second == charId) byAccount_.erase(a);
    const uint32_t session = c.session;
    if (session) bySession_.erase(session);
    chars_.erase(it);
    return session;
}

// --- chat clients ---

bool ChatRouter::can_bind(uint64_t accountId) const { return byAccount_.contains(accountId); }

std::optional<uint32_t> ChatRouter::bind(uint32_t sessionId, uint64_t accountId, proto::Role role) {
    Character& c = chars_.at(byAccount_.at(accountId));
    std::optional<uint32_t> replaced;
    if (c.session) {
        replaced = c.session;
        bySession_.erase(c.session);
    }
    c.session = sessionId;
    c.role = role;
    bySession_[sessionId] = c.charId;
    if (!rules_.channels.empty()) c.channels.insert(0); // the first channel (General) on every connect
    for (size_t i : c.channels) send_(sessionId, proto::ChatChannel{static_cast<uint8_t>(i + 1), rules_.channels[i], true});
    return replaced;
}

void ChatRouter::client_closed(uint32_t sessionId) {
    auto it = bySession_.find(sessionId);
    if (it == bySession_.end()) return;
    if (auto c = chars_.find(it->second); c != chars_.end() && c->second.session == sessionId) c->second.session = 0;
    bySession_.erase(it);
}

std::optional<std::string> ChatRouter::name_of_session(uint32_t sessionId) const {
    auto it = bySession_.find(sessionId);
    if (it == bySession_.end()) return std::nullopt;
    return chars_.at(it->second).name;
}

ChatRouter::Character* ChatRouter::by_session(uint32_t sessionId) {
    auto it = bySession_.find(sessionId);
    return it == bySession_.end() ? nullptr : &chars_.at(it->second);
}

ChatRouter::Character* ChatRouter::by_name(const std::string& name) {
    auto it = byName_.find(name_key(name));
    return it == byName_.end() ? nullptr : &chars_.at(it->second);
}

std::optional<size_t> ChatRouter::channel_index(const std::string& name) const {
    const std::string key = name_key(name);
    for (size_t i = 0; i < rules_.channels.size(); ++i)
        if (name_key(rules_.channels[i]) == key) return i;
    return std::nullopt;
}

void ChatRouter::notice(const Character& c, ChatNoticeCode code, std::string name, uint32_t minutes) {
    if (c.session) send_(c.session, proto::ChatNotice{code, std::move(name), minutes});
}

void ChatRouter::deliver(const Character& from, const std::vector<const Character*>& to, const proto::ChatLine& line) {
    const std::string fromKey = name_key(from.name);
    for (const Character* c : to)
        if (c->session && !c->ignored.contains(fromKey)) send_(c->session, line);
}

void ChatRouter::handle(uint32_t sessionId, const proto::Message& m, TimePoint now, SysTime wallNow) {
    Character* c = by_session(sessionId);
    if (!c) return;
    if (const auto* send = std::get_if<proto::ChatSend>(&m)) return send_chat(*c, *send, now, wallNow);
    if (const auto* mute = std::get_if<proto::ChatMute>(&m)) return this->mute(*c, *mute, wallNow);
    if (const auto* ignore = std::get_if<proto::ChatIgnore>(&m)) {
        if (!valid_character_name(ignore->name)) return notice(*c, ChatNoticeCode::bad_name, ignore->name);
        if (ignore->on) c->ignored.insert(name_key(ignore->name));
        else c->ignored.erase(name_key(ignore->name));
        return notice(*c, ignore->on ? ChatNoticeCode::ignored : ChatNoticeCode::unignored, ignore->name);
    }
    const bool join = std::holds_alternative<proto::ChatJoin>(m);
    if (join || std::holds_alternative<proto::ChatLeave>(m)) {
        const std::string& name = join ? std::get<proto::ChatJoin>(m).channel : std::get<proto::ChatLeave>(m).channel;
        const auto i = channel_index(name);
        if (!i) return notice(*c, ChatNoticeCode::no_channel, name);
        if (!join && !c->channels.contains(*i)) return notice(*c, ChatNoticeCode::not_in_channel, rules_.channels[*i]);
        if (join) c->channels.insert(*i);
        else c->channels.erase(*i);
        send_(sessionId, proto::ChatChannel{static_cast<uint8_t>(*i + 1), rules_.channels[*i], join});
    }
}

void ChatRouter::send_chat(Character& c, const proto::ChatSend& m, TimePoint now, SysTime wallNow) {
    if (!c.limiter.allow(now)) {
        log::debug("chat rate limited", {{"char", c.charId}});
        return notice(c, ChatNoticeCode::rate_limited);
    }
    auto text = sanitize_chat(m.text, rules_.maxBytes);
    if (!text) {
        log::debug("chat rejected", {{"char", c.charId}, {"bytes", m.text.size()}});
        return notice(c, ChatNoticeCode::rejected);
    }
    if (const auto left = mutes_.minutes_left(c.name, wallNow)) return notice(c, ChatNoticeCode::muted, "", *left);

    switch (m.target) {
    case ChatTarget::whisper: {
        if (!valid_character_name(m.to)) return notice(c, ChatNoticeCode::bad_name, m.to);
        const Character* to = by_name(m.to);
        if (!to || !to->session) return notice(c, ChatNoticeCode::not_online, m.to);
        send_(c.session, proto::ChatLine{ChatTarget::whisper_sent, "", to->name, *text}); // the echo first
        deliver(c, {to}, {ChatTarget::whisper, "", c.name, std::move(*text)});
        return;
    }
    case ChatTarget::channel: {
        const auto i = channel_index(m.to);
        if (!i) return notice(c, ChatNoticeCode::no_channel, m.to);
        if (!c.channels.contains(*i)) return notice(c, ChatNoticeCode::not_in_channel, rules_.channels[*i]);
        std::vector<const Character*> members;
        for (const auto& [id, other] : chars_)
            if (other.channels.contains(*i)) members.push_back(&other);
        return deliver(c, members, {ChatTarget::channel, rules_.channels[*i], c.name, std::move(*text)});
    }
    case ChatTarget::zone: {
        // ponytail: scans every online character per zone line; index by map if realms get big.
        std::vector<const Character*> here;
        for (const auto& [id, other] : chars_)
            if (other.map == c.map) here.push_back(&other);
        return deliver(c, here, {ChatTarget::zone, "", c.name, std::move(*text)});
    }
    case ChatTarget::whisper_sent: return; // never decoded from a client
    }
}

void ChatRouter::mute(Character& gm, const proto::ChatMute& m, SysTime wallNow) {
    if (gm.role != proto::Role::gm && gm.role != proto::Role::admin) {
        log::info("mute refused: not a GM", {{"char", gm.charId}, {"account", gm.accountId}});
        return notice(gm, ChatNoticeCode::not_allowed, m.name);
    }
    if (!valid_character_name(m.name)) return notice(gm, ChatNoticeCode::bad_name, m.name);
    const Character* target = by_name(m.name);
    const std::string name = target ? target->name : m.name;
    if (m.minutes == 0) {
        mutes_.erase(name);
        persist_({name, SysTime{}, gm.accountId});
        toLinks_({name, 0});
        log::info("unmuted", {{"name", name}, {"by", gm.accountId}});
        notice(gm, ChatNoticeCode::mute_cleared, name);
        if (target) notice(*target, ChatNoticeCode::you_unmuted);
        return;
    }
    const uint32_t minutes = std::min(m.minutes, rules_.maxMuteMinutes);
    const SysTime until = std::chrono::time_point_cast<SysTime::duration>(wallNow + std::chrono::minutes(minutes));
    mutes_.set(name, until);
    persist_({name, until, gm.accountId});
    toLinks_({name, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(until.time_since_epoch()).count())});
    log::info("muted", {{"name", name}, {"minutes", minutes}, {"by", gm.accountId}});
    notice(gm, ChatNoticeCode::mute_set, name, minutes);
    if (target) notice(*target, ChatNoticeCode::you_muted, "", minutes);
}

} // namespace chthonic::chat
