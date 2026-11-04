#include "auth_client.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <regex>
#include <thread>

namespace chthonic::bot {
namespace {

httplib::Headers headers(const Account& a) { return {{"X-Forwarded-For", a.forwardedFor}}; }

std::unique_ptr<httplib::Client> client(const std::string& url) {
    auto c = std::make_unique<httplib::Client>(url);
    c->set_connection_timeout(5, 0);
    c->set_read_timeout(30, 0); // bcrypt under load can be slow
    return c;
}

// Auth under load (bcrypt) can drop or time out a connection: retry transport failures, never HTTP answers.
httplib::Result post_retry(const std::string& url, const std::string& path, const Account& a, const std::string& body) {
    httplib::Result r;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::seconds(1 << attempt));
        r = client(url)->Post(path, headers(a), body, "application/json");
        if (r) break;
    }
    return r;
}

std::string describe(const httplib::Result& r) {
    if (!r) return "http error: " + httplib::to_string(r.error());
    return "status " + std::to_string(r->status) + ": " + r->body.substr(0, 200);
}

std::string url_encode(const std::string& s) {
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += static_cast<char>(ch);
        else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", ch);
            out += buf;
        }
    }
    return out;
}

} // namespace

std::optional<std::string> AuthClient::login(const Account& a, std::string& error, int* status) {
    const nlohmann::json body = {{"email", a.email}, {"password", a.password}};
    auto r = post_retry(ep_.authUrl, "/v1/auth/login", a, body.dump());
    if (status) *status = r ? r->status : 0;
    if (!r || r->status != 200) {
        error = "login: " + describe(r);
        return std::nullopt;
    }
    return nlohmann::json::parse(r->body).at("accessToken").get<std::string>();
}

bool AuthClient::register_and_verify(const Account& a, std::string& error) {
    const nlohmann::json body = {{"email", a.email}, {"password", a.password}};
    auto r = post_retry(ep_.authUrl, "/v1/auth/register", a, body.dump());
    if (!r || r->status != 202) {
        error = "register: " + describe(r);
        return false;
    }
    // The verification mail arrives asynchronously; poll Mailpit for it.
    auto mail = client(ep_.mailpitUrl);
    const std::regex tokenRe("token=([A-Za-z0-9_-]+)");
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto list = mail->Get("/api/v1/search?query=" + url_encode("to:" + a.email));
        if (list && list->status == 200) {
            const auto doc = nlohmann::json::parse(list->body);
            if (!doc["messages"].empty()) {
                const std::string id = doc["messages"][0]["ID"];
                auto msg = mail->Get("/api/v1/message/" + id);
                std::smatch m;
                const std::string text = msg && msg->status == 200 ? nlohmann::json::parse(msg->body).value("Text", "") : "";
                if (std::regex_search(text, m, tokenRe)) {
                    const nlohmann::json vb = {{"token", m[1].str()}};
                    auto v = post_retry(ep_.authUrl, "/v1/auth/verify-email", a, vb.dump());
                    if (!v || v->status != 200) {
                        error = "verify-email: " + describe(v);
                        return false;
                    }
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    error = "verification email never arrived for " + a.email;
    return false;
}

std::optional<std::string> AuthClient::ensure_login(const Account& a, std::string& error) {
    int status = 0;
    if (auto t = login(a, error, &status)) return t;
    if (status != 401 && status != 403) return std::nullopt; // 401 unknown, 403 unverified: (re)register
    if (!register_and_verify(a, error)) {
        // A retried verify can fail after the first attempt already verified the account: try logging in anyway.
        std::string loginError;
        return login(a, loginError);
    }
    return login(a, error);
}

std::optional<std::string> AuthClient::jwks(std::string& error) {
    auto c = client(ep_.authUrl);
    auto r = c->Get("/.well-known/jwks.json");
    if (!r || r->status != 200) {
        error = "jwks: " + describe(r);
        return std::nullopt;
    }
    return r->body;
}

} // namespace chthonic::bot
