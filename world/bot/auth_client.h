// Blocking HTTP calls to the auth service and Mailpit, used to obtain real access tokens for bots.
//
// TEST ONLY: every call carries X-Forwarded-For with a per-bot address. The default stack doesn't trust it (no
// TRUST_PROXY on auth); the test-only docker/test.override.yml, which tests/integration.sh and tests/load.sh always
// add, sets TRUST_PROXY=true so auth's per-IP rate limits (register 5/h, login 20/min) apply per bot instead of to
// the whole harness. Never enable TRUST_PROXY on an auth service that is reachable without a trusted proxy in front.
#pragma once
#include <optional>
#include <string>

namespace chthonic::bot {

struct AuthEndpoints {
    std::string authUrl = "http://localhost:3100";
    std::string mailpitUrl = "http://localhost:8026";
};

struct Account {
    std::string email;
    std::string password;
    std::string forwardedFor; // fake client IP, see above
};

class AuthClient {
public:
    explicit AuthClient(AuthEndpoints ep) : ep_(std::move(ep)) {}

    // Logs in; on invalid_credentials registers, verifies through Mailpit and logs in again.
    // Returns the access token, or nullopt with `error` set.
    std::optional<std::string> ensure_login(const Account& a, std::string& error);
    std::optional<std::string> login(const Account& a, std::string& error, int* status = nullptr);
    std::optional<std::string> jwks(std::string& error); // raw JWKS document

private:
    bool register_and_verify(const Account& a, std::string& error);
    AuthEndpoints ep_;
};

} // namespace chthonic::bot
