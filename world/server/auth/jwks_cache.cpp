#include "jwks_cache.h"

#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <nlohmann/json.hpp>

namespace chthonic {

KeyLookup JwksCache::key(const std::string& kid, TimePoint now) {
    if (!fetchedAt_ || now - *fetchedAt_ >= refreshAfter_) {
        if (!refresh(now)) return {KeyStatus::unavailable, {}};
    }
    auto it = keys_.find(kid);
    if (it == keys_.end() && (!attemptedAt_ || now - *attemptedAt_ >= minRefetch_)) {
        if (!refresh(now)) return {KeyStatus::unavailable, {}};
        it = keys_.find(kid);
    }
    if (it == keys_.end()) return {KeyStatus::unknown_kid, {}};
    return {KeyStatus::ok, it->second};
}

bool JwksCache::refresh(TimePoint now) {
    attemptedAt_ = now;
    const auto body = fetch_();
    if (!body) return false;
    auto parsed = parse(*body);
    if (!parsed) return false;
    keys_ = std::move(*parsed);
    fetchedAt_ = now;
    return true;
}

std::optional<std::map<std::string, std::string>> JwksCache::parse(const std::string& json) {
    try {
        const auto doc = nlohmann::json::parse(json);
        std::map<std::string, std::string> out;
        for (const auto& k : doc.at("keys")) {
            if (k.value("kty", "") != "RSA" || !k.contains("kid")) continue;
            if (k.contains("alg") && k["alg"] != "RS256") continue;
            out[k.at("kid").get<std::string>()] = jwt::helper::create_public_key_from_rsa_components(
                k.at("n").get<std::string>(), k.at("e").get<std::string>());
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace chthonic
