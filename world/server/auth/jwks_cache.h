// Caches the auth service's JWKS (kid -> RSA public key PEM). Not thread-safe: owned by the auth worker thread.
#pragma once
#include "../core/clock.h"
#include "jwt_verify.h"

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace chthonic {

class JwksCache {
public:
    using Fetch = std::function<std::optional<std::string>()>; // JWKS JSON body, nullopt on any failure

    JwksCache(Fetch fetch, Millis refreshAfter, Millis minRefetchInterval)
        : fetch_(std::move(fetch)), refreshAfter_(refreshAfter), minRefetch_(minRefetchInterval) {}

    // Refreshes when stale; refetches once for an unknown kid (at most once per minRefetchInterval).
    KeyLookup key(const std::string& kid, TimePoint now);

    // kid -> PEM for every RSA key in a JWKS document; nullopt if the document is malformed.
    static std::optional<std::map<std::string, std::string>> parse(const std::string& json);

private:
    bool refresh(TimePoint now);

    Fetch fetch_;
    Millis refreshAfter_, minRefetch_;
    std::map<std::string, std::string> keys_;
    std::optional<TimePoint> fetchedAt_;   // last successful fetch
    std::optional<TimePoint> attemptedAt_; // last attempt, success or not
};

} // namespace chthonic
