// Access-token verification, following docs/auth/token-verification.html:
// RS256 only, key chosen by `kid`, iss/aud from config, exp checked with a little clock skew.
#pragma once
#include "../core/clock.h"

#include <messages.h>

#include <functional>
#include <optional>
#include <string>

namespace chthonic {

struct JwtRules {
    std::string issuer;
    std::string audience;
    uint32_t clockSkewSec = 5;
};

enum class KeyStatus { ok, unknown_kid, unavailable };
struct KeyLookup {
    KeyStatus status = KeyStatus::unavailable;
    std::string pem; // RSA public key
};
using KeyForKid = std::function<KeyLookup(const std::string& kid)>;

struct TokenResult {
    proto::Code code = proto::Code::invalid_token; // ok, invalid_token, token_expired or service_unavailable
    uint64_t accountId = 0;
    proto::Role role = proto::Role::player;
};

TokenResult verify_access_token(const std::string& token, const KeyForKid& keyFor, const JwtRules& rules, SysTime now);

} // namespace chthonic
