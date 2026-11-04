// Shared bot plumbing: options, test accounts, and the connect -> login -> character -> enter-world flow.
#pragma once
#include "auth_client.h"
#include "world_client.h"

#include <heights.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace chthonic::bot {

struct Options {
    AuthEndpoints auth;
    std::string host = "localhost";
    uint16_t port = 7777;
    std::string chatHost = "localhost"; // the chat service (same CA as the world server)
    uint16_t chatPort = 7778;
    uint16_t chatLinkPort = 7779;     // its internal world link: the chat-auth scenario checks its refusals
    uint16_t adminPort = 7780;        // the world server's admin port (admin scenarios; secret from WORLD_ADMIN_SECRET)
    std::string caFile;           // server certificate / CA to trust
    std::string runId = "run";    // namespaces the bot accounts of one run
    std::string mode = "scenario";
    std::string scenario;
    std::string behaviour = "wander";
    int count = 1;
    int durationSec = 60;
    int threads = 4;
    int connectRatePerSec = 50;
    std::string authKeyFile;      // auth's private key (test only): mints an expired token for scenario 8
    std::string expectPos;        // "x,y,z" for restart-verify
    std::string mapRoot;          // the server's maps/ folder (terrain scenario: ground heights)
    proto::CharOptions charOptions; // --char-option key=value: CharCreate options (a game's class, ...)
    std::map<std::string, std::string> extra;
};

Account bot_account(const Options& o, const std::string& tag);

asio::ssl::context make_client_tls(const Options& o);

// Connect + TLS + Hello + Auth. nullptr on failure (error says why).
asio::awaitable<std::shared_ptr<WorldClient>> connect_and_login(const Options& o, asio::ssl::context& tls,
                                                                const std::string& token, std::string& error);
// Lists characters, creates `name` (with options) if there are none, enters the first one.
asio::awaitable<bool> enter_world(WorldClient& c, const std::string& name, proto::EnterWorldOk& out, std::string& error,
                                  const proto::CharOptions& options = {});

// Walks in a straight line at `speed` m/s, one Move every 100 ms, at from.y or groundY(x, z) if given. Returns the
// final position.
asio::awaitable<proto::Vec3> walk(WorldClient& c, proto::Vec3 from, float dx, float dz, float speed = 9.0f,
                                  const std::function<float(float x, float z)>& groundY = nullptr);
// A map's manifest and heights, read the way the server reads them (the terrain scenarios, and load --terrain).
struct Terrain {
    map::MapInfo info;
    map::TileHeights heights;
    float ground(float x, float z) const { return map::ground_at(info, heights, x, z).value_or(NAN); }
};
// The map in folder (<maps>/<name>); nullopt with err set if map.json or a heights file doesn't load.
std::optional<Terrain> load_terrain(const std::filesystem::path& folder, std::string& err);

// Collects every message that arrives within `window`.
asio::awaitable<std::vector<proto::Message>> drain(WorldClient& c, Millis window);
asio::awaitable<void> sleep_for(Millis d);

std::string random_name(size_t len = 12);
float distance_xz(const proto::Vec3& a, const proto::Vec3& b);

// Prints "  ok: ..." / "  FAIL: ..." and remembers failures.
struct Checker {
    bool ok = true;
    void expect(bool cond, const std::string& what) {
        std::printf("  %s: %s\n", cond ? "ok" : "FAIL", what.c_str());
        std::fflush(stdout);
        ok = ok && cond;
    }
};

} // namespace chthonic::bot
