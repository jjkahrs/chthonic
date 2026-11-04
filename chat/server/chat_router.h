// Who is online on which map (from the world links), which chat client belongs to which character, and where each
// chat request goes: whispers, the fixed channels, the zone (map) channel, ignore lists and GM mutes. No network, no
// database: it talks through the three callbacks, and time is passed in, so the self-check drives it directly.
// Never logs chat text. All calls on the io thread.
#pragma once
#include <core/chat_rules.h>
#include <core/clock.h>
#include <core/rate_limiter.h>

#include <messages.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace chthonic::chat {

struct RouterRules {
    std::vector<std::string> channels{"General", "Trade", "LFG"}; // CHAT_CHANNELS; numbered from 1 in this order
    size_t maxBytes = 256;                                         // CHAT_MAX_BYTES
    size_t rateCount = 5;                                          // CHAT_RATE_COUNT per sender ...
    Millis rateWindow{10000};                                      // ... per CHAT_RATE_WINDOW_MS
    uint32_t maxMuteMinutes = 525600;                              // a year; longer mutes are cut to it
};

// A mute to persist: until == SysTime{} means lifted.
struct MuteChange {
    std::string name;
    SysTime until{};
    uint64_t byAccount = 0;
};

class ChatRouter {
public:
    using Send = std::function<void(uint32_t sessionId, const proto::Message&)>; // to one chat client
    using ToLinks = std::function<void(const proto::MuteState&)>;               // to every world link
    using Persist = std::function<void(const MuteChange&)>;

    ChatRouter(RouterRules rules, Send send, ToLinks toLinks, Persist persist);

    // --- world links ---
    // A character entered a world (through link). Another online character of the same name or account is dropped:
    // returns the chat sessions that lost their character, for the caller to disconnect (no_character).
    std::vector<uint32_t> char_online(uint32_t link, const proto::CharOnline& c);
    void map_changed(const proto::MapChanged& m);
    // The chat sessions bound to characters that are gone now: the caller disconnects them (no_character).
    std::vector<uint32_t> char_offline(uint64_t charId);
    std::vector<uint32_t> link_lost(uint32_t link); // every character of that link goes offline

    // --- chat clients ---
    bool can_bind(uint64_t accountId) const; // the account has a character online
    // Binds an authenticated session to its account's character (can_bind first) and joins it to the first channel
    // (ChatChannel). Returns a session of the same character it replaced, for the caller to disconnect.
    std::optional<uint32_t> bind(uint32_t sessionId, uint64_t accountId, proto::Role role);
    void client_closed(uint32_t sessionId);
    // A ChatSend / ChatJoin / ChatLeave / ChatIgnore / ChatMute from a bound session; answers through Send.
    void handle(uint32_t sessionId, const proto::Message& m, TimePoint now, SysTime wallNow);

    // --- mutes ---
    void load_mute(const std::string& name, SysTime until) { mutes_.set(name, until); }
    size_t expire_mutes(SysTime now) { return mutes_.expire(now); }
    const Mutes& mutes() const { return mutes_; }

    size_t online() const { return chars_.size(); }
    std::optional<std::string> name_of_session(uint32_t sessionId) const;

private:
    struct Character {
        uint64_t charId = 0, accountId = 0;
        std::string name;
        uint16_t map = 0;
        uint32_t link = 0;
        uint32_t session = 0; // its chat client, 0 if none
        proto::Role role = proto::Role::player;
        std::set<size_t> channels;   // indices into rules_.channels
        std::set<std::string> ignored; // name_key()s
        WindowLimiter limiter;
    };

    Character* by_session(uint32_t sessionId);
    Character* by_name(const std::string& name);
    std::optional<size_t> channel_index(const std::string& name) const;
    void notice(const Character& c, proto::ChatNoticeCode code, std::string name = "", uint32_t minutes = 0);
    // Sends line to every listener in the list that has a chat client and does not ignore from.
    void deliver(const Character& from, const std::vector<const Character*>& to, const proto::ChatLine& line);
    void send_chat(Character& c, const proto::ChatSend& m, TimePoint now, SysTime wallNow);
    void mute(Character& gm, const proto::ChatMute& m, SysTime wallNow);
    std::optional<uint32_t> remove(uint64_t charId); // returns its session

    RouterRules rules_;
    Send send_;
    ToLinks toLinks_;
    Persist persist_;
    std::map<uint64_t, Character> chars_;                 // by charId
    std::unordered_map<std::string, uint64_t> byName_;    // name_key -> charId
    std::unordered_map<uint64_t, uint64_t> byAccount_;    // accountId -> charId
    std::unordered_map<uint32_t, uint64_t> bySession_;    // chat session -> charId
    Mutes mutes_;
};

} // namespace chthonic::chat
