#include "chat_rules.h"

#include <algorithm>
#include <cstdint>

namespace chthonic {

bool valid_character_name(std::string_view name) {
    if (name.size() < 3 || name.size() > 16) return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
}

std::string name_key(std::string_view name) {
    std::string key(name);
    for (char& c : key)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return key;
}

std::optional<std::string> sanitize_chat(std::string_view text, size_t maxBytes) {
    if (text.size() > maxBytes) return std::nullopt;
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (b0 < 0x80) { len = 1; cp = b0; }
        else if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1F; }
        else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; }
        else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; }
        else return std::nullopt;
        if (i + len > text.size()) return std::nullopt;
        for (size_t k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(text[i + k]);
            if ((b & 0xC0) != 0x80) return std::nullopt;
            cp = (cp << 6) | (b & 0x3F);
        }
        static constexpr uint32_t kMinForLen[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMinForLen[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return std::nullopt; // overlong/surrogate
        const bool control = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
        if (!control) out.append(text.substr(i, len));
        i += len;
    }
    if (out.empty()) return std::nullopt;
    return out;
}

std::optional<uint32_t> Mutes::minutes_left(std::string_view name, SysTime now) const {
    const auto it = until_.find(name_key(name));
    if (it == until_.end() || it->second <= now) return std::nullopt;
    const auto left = std::chrono::ceil<std::chrono::minutes>(it->second - now).count();
    return static_cast<uint32_t>(std::max<long long>(1, left));
}

size_t Mutes::expire(SysTime now) {
    return std::erase_if(until_, [now](const auto& m) { return m.second <= now; });
}

} // namespace chthonic
