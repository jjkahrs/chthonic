#include "jwt_verify.h"

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

#include <charconv>

namespace chthonic {
namespace {

struct FixedClock {
    jwt::date t;
    jwt::date now() const { return t; }
};

std::optional<uint64_t> parse_account_id(const std::string& sub) {
    uint64_t v = 0;
    auto [p, ec] = std::from_chars(sub.data(), sub.data() + sub.size(), v);
    if (ec != std::errc() || p != sub.data() + sub.size() || sub.empty() || v == 0) return std::nullopt;
    return v;
}

std::optional<proto::Role> parse_role(const std::string& s) {
    if (s == "player") return proto::Role::player;
    if (s == "gm") return proto::Role::gm;
    if (s == "admin") return proto::Role::admin;
    return std::nullopt;
}

} // namespace

TokenResult verify_access_token(const std::string& token, const KeyForKid& keyFor, const JwtRules& rules, SysTime now) {
    TokenResult r;
    try {
        const auto decoded = jwt::decode<jwt::traits::nlohmann_json>(token);
        if (decoded.get_algorithm() != "RS256" || !decoded.has_key_id()) return r; // rejects none, HS*, etc.
        if (!decoded.has_expires_at() || !decoded.has_subject()) return r;

        const KeyLookup key = keyFor(decoded.get_key_id());
        if (key.status == KeyStatus::unavailable) { r.code = proto::Code::service_unavailable; return r; }
        if (key.status == KeyStatus::unknown_kid) return r;

        std::error_code ec;
        jwt::verify<FixedClock, jwt::traits::nlohmann_json>(FixedClock{now})
            .allow_algorithm(jwt::algorithm::rs256(key.pem))
            .with_issuer(rules.issuer)
            .with_audience(rules.audience)
            .leeway(rules.clockSkewSec)
            .verify(decoded, ec);
        if (ec == jwt::error::token_verification_error::token_expired) { r.code = proto::Code::token_expired; return r; }
        if (ec) return r;

        const auto account = parse_account_id(decoded.get_subject());
        if (!account || !decoded.has_payload_claim("role")) return r;
        const auto role = parse_role(decoded.get_payload_claim("role").as_string());
        if (!role) return r;
        r.code = proto::Code::ok;
        r.accountId = *account;
        r.role = *role;
    } catch (const std::exception&) {
        // malformed token, bad base64/JSON, wrong claim type: all invalid
    }
    return r;
}

} // namespace chthonic
