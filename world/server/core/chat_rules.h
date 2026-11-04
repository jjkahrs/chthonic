// Chat and name rules shared by the world server (say / yell / emote) and the chat service (whispers, channels, zone):
// untrusted text is checked the same way on both, and both drop a muted character's messages.
#pragma once
#include "clock.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace chthonic {

// 3-16 ASCII letters. Uniqueness (case-insensitive) is enforced by the database.
bool valid_character_name(std::string_view name);
// ASCII lower case: the key character names are compared by (they are unique case-insensitively).
std::string name_key(std::string_view name);

// Valid UTF-8, control characters stripped, 1..maxBytes bytes after stripping. nullopt = reject.
std::optional<std::string> sanitize_chat(std::string_view text, size_t maxBytes);

// Muted character names (any case) and when each mute ends (wall clock). The chat service owns and persists them; the
// world server holds the copy the chat link sends it.
class Mutes {
public:
    void set(std::string_view name, SysTime until) { until_[name_key(name)] = until; }
    void erase(std::string_view name) { until_.erase(name_key(name)); }
    void clear() { until_.clear(); }
    // Whole minutes left (at least 1) while name is muted at now; nullopt if not (an expired mute counts as none).
    std::optional<uint32_t> minutes_left(std::string_view name, SysTime now) const;
    // Forgets every mute that ended by now; returns how many.
    size_t expire(SysTime now);
    const std::unordered_map<std::string, SysTime>& all() const { return until_; }

private:
    std::unordered_map<std::string, SysTime> until_;
};

} // namespace chthonic
