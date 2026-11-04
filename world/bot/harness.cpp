#include "harness.h"

#include <cmath>
#include <fstream>
#include <functional>
#include <random>

namespace chthonic::bot {

using asio::use_awaitable;

Account bot_account(const Options& o, const std::string& tag) {
    Account a;
    a.email = o.runId + "-" + tag + "@bots.chthonic.test";
    a.password = "bot-password-" + o.runId;
    // A stable fake client address per account, so auth rate limits count per bot (TRUST_PROXY, test only).
    const size_t h = std::hash<std::string>{}(a.email);
    a.forwardedFor = "10." + std::to_string((h >> 16) & 0xFF) + "." + std::to_string((h >> 8) & 0xFF) + "." +
                     std::to_string(1 + (h & 0xFD));
    return a;
}

asio::ssl::context make_client_tls(const Options& o) {
    asio::ssl::context tls(asio::ssl::context::tls_client);
    tls.set_options(asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
    if (!o.caFile.empty()) tls.load_verify_file(o.caFile);
    else tls.set_default_verify_paths();
    return tls;
}

asio::awaitable<std::shared_ptr<WorldClient>> connect_and_login(const Options& o, asio::ssl::context& tls,
                                                                const std::string& token, std::string& error) {
    auto c = std::make_shared<WorldClient>(co_await asio::this_coro::executor, tls);
    if (!co_await c->connect(o.host, o.port)) {
        error = "connect: " + c->lastError;
        co_return nullptr;
    }
    if (!co_await c->login(token)) {
        error = std::string("login: ") +
                (c->disconnectReason ? proto::code_name(*c->disconnectReason) : "no AuthOk") + " " + c->lastError;
        c->close();
        co_return nullptr;
    }
    co_return c;
}

asio::awaitable<bool> enter_world(WorldClient& c, const std::string& name, proto::EnterWorldOk& out, std::string& error,
                                  const proto::CharOptions& options) {
    c.send(proto::CharListReq{});
    auto list = co_await c.wait_for<proto::CharList>(Millis(10000));
    if (!list) { error = "no CharList"; co_return false; }
    uint64_t charId = 0;
    if (list->chars.empty()) {
        c.send(proto::CharCreate{name, options});
        auto created = co_await c.wait_for<proto::CharCreated>(Millis(10000));
        if (!created) { error = "CharCreate failed"; co_return false; }
        charId = created->character.id;
    } else {
        charId = list->chars.front().id;
    }
    c.send(proto::EnterWorld{charId});
    auto ok = co_await c.wait_for<proto::EnterWorldOk>(Millis(10000));
    if (!ok) { error = "no EnterWorldOk"; co_return false; }
    out = *ok;
    co_return true;
}

asio::awaitable<void> sleep_for(Millis d) {
    asio::steady_timer t(co_await asio::this_coro::executor, d);
    co_await t.async_wait(use_awaitable);
}

asio::awaitable<proto::Vec3> walk(WorldClient& c, proto::Vec3 from, float dx, float dz, float speed,
                                  const std::function<float(float x, float z)>& groundY) {
    const float dist = std::hypot(dx, dz);
    const int steps = static_cast<int>(std::ceil(dist / (speed * 0.1f)));
    const float heading = std::atan2(dx, dz);
    proto::Vec3 p = from;
    asio::steady_timer t(co_await asio::this_coro::executor);
    auto next = std::chrono::steady_clock::now();
    for (int i = 1; i <= steps; ++i) {
        next += Millis(100);
        t.expires_at(next);
        co_await t.async_wait(use_awaitable);
        p = {from.x + dx * i / steps, from.y, from.z + dz * i / steps};
        if (groundY) p.y = groundY(p.x, p.z);
        c.send(proto::Move{p, heading, static_cast<uint32_t>(i * 100)});
    }
    co_return p;
}

asio::awaitable<std::vector<proto::Message>> drain(WorldClient& c, Millis window) {
    std::vector<proto::Message> out;
    const auto deadline = std::chrono::steady_clock::now() + window;
    for (;;) {
        const auto left = std::chrono::duration_cast<Millis>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        auto m = co_await c.next(left);
        if (!m) break;
        out.push_back(std::move(*m));
    }
    co_return out;
}

std::string random_name(size_t len) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> letter(0, 25);
    std::string s;
    s += static_cast<char>('A' + letter(rng));
    for (size_t i = 1; i < len; ++i) s += static_cast<char>('a' + letter(rng));
    return s;
}

float distance_xz(const proto::Vec3& a, const proto::Vec3& b) { return std::hypot(a.x - b.x, a.z - b.z); }

std::optional<Terrain> load_terrain(const std::filesystem::path& folder, std::string& err) {
    const auto read = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    auto info = map::parse_manifest(nlohmann::json::parse(read(folder / "map.json"), nullptr, false), err);
    if (!info) { err = (folder / "map.json").string() + ": " + err; return std::nullopt; }
    Terrain t{std::move(*info), {}};
    for (const auto& r : t.info.tiles)
        for (int32_t tz = r.z0; tz <= r.z1; ++tz)
            for (int32_t tx = r.x0; tx <= r.x1; ++tx) {
                for (const char* ext : {".r16", ".png"}) {
                    const auto file = folder / "heights" / (std::to_string(tx) + "_" + std::to_string(tz) + ext);
                    if (!std::filesystem::exists(file)) continue;
                    auto h = map::decode_heights(read(file), ext, err);
                    if (!h) { err = file.string() + ": " + err; return std::nullopt; }
                    t.heights[{tx, tz}] = std::move(*h);
                    break;
                }
            }
    return t;
}

} // namespace chthonic::bot
