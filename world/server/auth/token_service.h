// Verifies access tokens off the game thread: JWKS fetches are blocking HTTP calls.
#pragma once
#include "../core/job_thread.h"
#include "jwks_cache.h"
#include "jwt_verify.h"

#include <functional>

namespace chthonic {

struct TokenServiceOptions {
    std::string jwksUrl;
    JwtRules rules;
    Millis refreshAfter{300000}, minRefetch{10000}, httpTimeout{3000};
};

class TokenService {
public:
    using Post = std::function<void(std::function<void()>)>;
    TokenService(TokenServiceOptions opts, Post post);

    // `done` runs via `post` (on the game thread).
    void verify(std::string token, std::function<void(TokenResult)> done);
    void stop() { thread_.stop(); }

private:
    TokenServiceOptions opts_;
    Post post_;
    JwksCache cache_;  // touched only on thread_
    JobThread thread_; // last: stops before the cache goes away
};

} // namespace chthonic
