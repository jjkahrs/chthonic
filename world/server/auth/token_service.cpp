#include "token_service.h"
#include "../core/log.h"

#include <httplib.h>

namespace chthonic {
namespace {

// "http://auth:3000/.well-known/jwks.json" -> {"http://auth:3000", "/.well-known/jwks.json"}
std::pair<std::string, std::string> split_url(const std::string& url) {
    const auto hostStart = url.find("://") + 3;
    const auto pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) return {url, "/"};
    return {url.substr(0, pathStart), url.substr(pathStart)};
}

} // namespace

TokenService::TokenService(TokenServiceOptions opts, Post post)
    : opts_(std::move(opts)), post_(std::move(post)),
      cache_(
          [this]() -> std::optional<std::string> {
              const auto [base, path] = split_url(opts_.jwksUrl);
              httplib::Client cli(base);
              const auto secs = std::chrono::duration_cast<std::chrono::seconds>(opts_.httpTimeout).count();
              cli.set_connection_timeout(static_cast<time_t>(secs), 0);
              cli.set_read_timeout(static_cast<time_t>(secs), 0);
              auto res = cli.Get(path);
              if (!res || res->status != 200) {
                  log::warn("JWKS fetch failed", {{"url", opts_.jwksUrl},
                                                   {"status", res ? res->status : 0},
                                                   {"error", res ? "" : httplib::to_string(res.error())}});
                  return std::nullopt;
              }
              log::info("JWKS fetched", {{"url", opts_.jwksUrl}});
              return res->body;
          },
          opts_.refreshAfter, opts_.minRefetch) {}

void TokenService::verify(std::string token, std::function<void(TokenResult)> done) {
    thread_.push([this, token = std::move(token), done = std::move(done)] {
        const auto now = SteadyClock::now();
        auto keyFor = [&](const std::string& kid) { return cache_.key(kid, now); };
        const TokenResult r = verify_access_token(token, keyFor, opts_.rules, std::chrono::system_clock::now());
        post_([done, r] { done(r); });
    });
}

} // namespace chthonic
