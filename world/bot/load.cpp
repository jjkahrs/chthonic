#include "load.h"

#include <map.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

namespace chthonic::bot {
namespace {

struct Stats {
    std::atomic<int> tokens{0}, connected{0}, entered{0}, unexpectedDisconnects{0}, expectedDisconnects{0}, errors{0},
        onTarget{0}; // --maps: bots that reached their map
    std::atomic<uint64_t> movesSent{0}, entityMoves{0}, entityMovesMsgs{0}, spawns{0}, despawns{0}, corrections{0}, mapChanges{0}, chats{0}, says{0},
        componentUpdates{0}, componentBytes{0}, componentRemoves{0}, serverEvents{0};
    std::mutex mu;
    std::vector<std::string> errorSamples;
    std::vector<double> enterMs; // per bot: connect -> EnterWorldOk

    void entered_after(double ms) {
        std::lock_guard lk(mu);
        enterMs.push_back(ms);
    }

    void error(const std::string& e) {
        ++errors;
        std::lock_guard lk(mu);
        if (errorSamples.size() < 10) errorSamples.push_back(e);
    }
};

// A map as --maps sees it (map.json and areas.json under --map-root): its bounds and its portals.
struct BotMap {
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    std::vector<map::Area> portals;
    bool instanced = false;
    uint32_t maxPlayers = 0; // an instance map's; 0: no limit
    // Within 5 m of a portal's bounds: a wanderer turns back there.
    bool near_portal(const proto::Vec3& p) const {
        for (const auto& a : portals)
            if (p.x > a.bounds[0] - 5 && p.x < a.bounds[2] + 5 && p.z > a.bounds[1] - 5 && p.z < a.bounds[3] + 5) return true;
        return false;
    }
};
using BotMaps = std::map<uint16_t, BotMap>;

std::optional<nlohmann::json> read_json(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

// Every <folder>/map.json under root; error names the first file that does not parse.
BotMaps load_bot_maps(const std::string& root, std::string& error) {
    BotMaps maps;
    std::error_code ec;
    for (const auto& dir : std::filesystem::directory_iterator(root, ec)) {
        const auto manifest = read_json(dir.path() / "map.json");
        if (!manifest) continue;
        const auto info = map::parse_manifest(*manifest, error);
        if (!info) {
            error = dir.path().string() + ": " + error;
            return {};
        }
        BotMap& m = maps[info->id];
        m.instanced = info->instanced();
        m.maxPlayers = info->maxPlayers.value_or(0);
        m.x0 = m.z0 = 1e9f;
        m.x1 = m.z1 = -1e9f;
        for (const auto& t : info->tiles) {
            m.x0 = std::min(m.x0, t.x0 * map::kTileSize);
            m.z0 = std::min(m.z0, t.z0 * map::kTileSize);
            m.x1 = std::max(m.x1, (t.x1 + 1) * map::kTileSize);
            m.z1 = std::max(m.z1, (t.z1 + 1) * map::kTileSize);
        }
        if (const auto areas = read_json(dir.path() / "areas.json")) {
            const auto parsed = map::parse_areas(*areas, error);
            if (!parsed) {
                error = dir.path().string() + "/areas.json: " + error;
                return {};
            }
            for (const auto& a : parsed->list)
                if (a.portal) m.portals.push_back(a);
        }
    }
    if (ec) error = root + ": " + ec.message();
    return maps;
}

// The portal on map `from` that starts the shortest portal route to map `to` (breadth first); null if none.
const map::Area* next_portal(const BotMaps& maps, uint16_t from, uint16_t to) {
    std::map<uint16_t, const map::Area*> first{{from, nullptr}}; // map -> the portal on `from` its route starts with
    std::deque<uint16_t> queue{from};
    while (!queue.empty()) {
        const uint16_t at = queue.front();
        queue.pop_front();
        const auto it = maps.find(at);
        if (it == maps.end()) continue;
        for (const auto& a : it->second.portals) {
            if (first.contains(a.portal->map)) continue;
            first[a.portal->map] = at == from ? &a : first[at];
            if (a.portal->map == to) return first[to];
            queue.push_back(a.portal->map);
        }
    }
    return nullptr;
}

// One bot's random walk: straight segments of 3-8 s at 9 m/s, bouncing off the bounds (and, with --maps, off the
// portals: the bot stays on its map).
struct Wander {
    std::mt19937 rng{std::random_device{}()};
    float heading = 0;
    int stepsLeft = 0;
    proto::Vec3 step(proto::Vec3 p, float x0, float z0, float x1, float z1, const BotMap* avoid = nullptr) {
        if (stepsLeft-- <= 0) {
            heading = std::uniform_real_distribution<float>(0, 6.2831853f)(rng);
            stepsLeft = std::uniform_int_distribution<int>(30, 80)(rng);
        }
        const float d = 0.9f; // 9 m/s at 10 Hz
        proto::Vec3 n{p.x + d * std::sin(heading), p.y, p.z + d * std::cos(heading)};
        if (n.x < x0 + 5 || n.x > x1 - 5 || n.z < z0 + 5 || n.z > z1 - 5 || (avoid && avoid->near_portal(n))) {
            heading += 3.14159265f;
            n = {p.x + d * std::sin(heading), p.y, p.z + d * std::cos(heading)};
        }
        return n;
    }
};

// target: with --maps, the map this bot walks to through the portals and then wanders on (maps: --map-root's).
// instanceKey: for an instance map target, the key its copy is picked by (sent to the sample test_hooks.lua first).
// terrain (--terrain): a wanderer without target walks at its ground height, as a client would.
asio::awaitable<void> run_bot(const Options& o, asio::ssl::context& tls, Stats& st, std::string token,
                              Millis startDelay, std::chrono::steady_clock::time_point end, const BotMaps& maps,
                              std::optional<uint16_t> target, std::optional<std::string> instanceKey,
                              const Terrain* terrain) {
    co_await sleep_for(startDelay);
    const auto connectStart = std::chrono::steady_clock::now();
    std::string err;
    auto c = co_await connect_and_login(o, tls, token, err);
    if (!c) { st.error(err); co_return; }
    ++st.connected;

    if (o.behaviour == "malformed") {
        c->send_raw({0, 0, 0x77, 0x77});
        const bool closed = co_await c->wait_closed(Millis(5000));
        if (closed && c->disconnectReason == proto::Code::unknown_opcode) ++st.expectedDisconnects;
        else st.error("malformed bot was not disconnected with unknown_opcode");
        co_return;
    }

    proto::EnterWorldOk ok;
    if (!co_await enter_world(*c, random_name(), ok, err, o.charOptions)) {
        st.error("enter: " + err);
        c->close();
        co_return;
    }
    ++st.entered;
    st.entered_after(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - connectStart).count());
    if (instanceKey) c->send(proto::ClientEvent{"test.instance_key", nlohmann::json{{"key", *instanceKey}}.dump()});

    auto pos = std::make_shared<proto::Vec3>(ok.pos);
    // A wanderer that walks into a portal (docs/world/areas.html) is parked on the other map: it stops moving, and the
    // Corrections for its moves still in flight from the old map are not counted. With --maps a bot never parks: it
    // walks on to its target map (onMap follows each MapChange), then wanders there clear of the portals.
    // ponytail: parked, not wandering on; give the bot the destination map's bounds if load tests need cross-map moves.
    auto parked = std::make_shared<bool>(false);
    auto onMap = std::make_shared<uint16_t>(ok.map);
    // Steps to sit out after a MapChange: a transfer resets the move budget (validate_move), so a full 0.9 m step in
    // the first 100 ms on the new map would be corrected, as for any client that moves before its loading screen ends.
    auto settle = std::make_shared<int>(0);
    c->onMessage = [&st, pos, parked, onMap, settle, target](const proto::Message& m) {
        if (auto* mv = std::get_if<proto::EntityMoves>(&m)) {
            ++st.entityMovesMsgs;
            st.entityMoves += mv->entries.size(); // entries: the moves seen, as before batching
        } else if (std::holds_alternative<proto::Spawn>(m)) ++st.spawns;
        else if (std::holds_alternative<proto::Despawn>(m)) ++st.despawns;
        else if (std::holds_alternative<proto::ChatMsg>(m)) ++st.chats;
        else if (auto* cu = std::get_if<proto::ComponentUpdate>(&m)) {
            ++st.componentUpdates;
            st.componentBytes += cu->json.size();
        } else if (std::holds_alternative<proto::ComponentRemove>(m)) ++st.componentRemoves;
        else if (std::holds_alternative<proto::ServerEvent>(m)) ++st.serverEvents;
        else if (auto* corr = std::get_if<proto::Correction>(&m)) {
            if (!*parked) ++st.corrections;
            *pos = corr->pos;
        } else if (auto* mc = std::get_if<proto::MapChange>(&m)) {
            ++st.mapChanges;
            *parked = !target;
            *pos = mc->pos;
            *onMap = mc->map;
            *settle = 3;
        }
    };

    Wander wander;
    const float zone = o.extra.count("zone") ? std::stof(o.extra.at("zone")) : 2048.f;
    bool reached = false;
    asio::steady_timer t(co_await asio::this_coro::executor);
    auto next = std::chrono::steady_clock::now();
    uint32_t tick = 0;
    while (std::chrono::steady_clock::now() < end && !c->closed()) {
        next += Millis(100);
        // A stalled bot skips steps instead of bursting them: a burst would look like a speedhack.
        if (std::chrono::steady_clock::now() > next + Millis(300)) next = std::chrono::steady_clock::now();
        t.expires_at(next);
        co_await t.async_wait(asio::use_awaitable);
        ++tick;
        if (*parked) continue;
        if (*settle > 0) { // 300 ms on the new map before the next step
            --*settle;
            continue;
        }
        if (target && *onMap != *target) { // on the way: straight at the next portal's middle, 9 m/s
            const map::Area* portal = next_portal(maps, *onMap, *target);
            if (!portal) {
                st.error("no portal route from map " + std::to_string(*onMap) + " to " + std::to_string(*target));
                break;
            }
            const float gx = (portal->bounds[0] + portal->bounds[2]) / 2, gz = (portal->bounds[1] + portal->bounds[3]) / 2;
            const float dx = gx - pos->x, dz = gz - pos->z, dist = std::hypot(dx, dz);
            if (dist > 0) {
                const float d = std::min(0.9f, dist);
                *pos = {pos->x + dx / dist * d, pos->y, pos->z + dz / dist * d};
            }
            c->send(proto::Move{*pos, std::atan2(dx, dz), tick * 100});
            ++st.movesSent;
            continue;
        }
        if (target && !reached) {
            reached = true;
            ++st.onTarget;
        }
        if (o.behaviour == "wander" || o.behaviour == "chat") {
            if (target) {
                const BotMap& m = maps.at(*target);
                *pos = wander.step(*pos, m.x0, m.z0, m.x1, m.z1, &m);
            } else {
                *pos = wander.step(*pos, 0, 0, zone, zone);
                if (const float y = terrain ? terrain->ground(pos->x, pos->z) : NAN; !std::isnan(y)) pos->y = y;
            }
            c->send(proto::Move{*pos, wander.heading, tick * 100});
            ++st.movesSent;
        } else if (o.behaviour == "speedhack") {
            pos->x = std::min(pos->x + 5.f, zone - 1); // 50 m/s
            c->send(proto::Move{*pos, 0, tick * 100});
            ++st.movesSent;
        }
        if (o.behaviour == "chat" && tick % 40 == 0) { // every 4 s: under the 5 per 10 s limit
            c->send(proto::Say{proto::ChatKind::say, "hello from a load bot"});
            ++st.says;
        }
    }
    if (c->closed()) {
        ++st.unexpectedDisconnects;
        st.error(std::string("disconnected: ") + (c->disconnectReason ? proto::code_name(*c->disconnectReason) : "closed"));
    }
    c->close();
}

} // namespace

int run_load(const Options& o) {
    Stats st;
    // --maps: bot i heads for the i-th listed map (round robin) through the portals of --map-root's maps. Onto an
    // instance map, every --instance-size bots (default: its maxPlayers, else 10) share an instance key: one copy each.
    std::vector<uint16_t> targets;
    BotMaps maps;
    if (const auto it = o.extra.find("maps"); it != o.extra.end()) {
        for (size_t at = 0; at < it->second.size();) {
            const size_t comma = std::min(it->second.find(',', at), it->second.size());
            targets.push_back(static_cast<uint16_t>(std::stoi(it->second.substr(at, comma - at))));
            at = comma + 1;
        }
        std::string err;
        maps = load_bot_maps(o.mapRoot, err);
        for (uint16_t t : targets)
            if (!maps.contains(t)) err += " map " + std::to_string(t) + " is not under --map-root '" + o.mapRoot + "';";
        if (!err.empty() || targets.empty()) {
            std::fprintf(stderr, "--maps: %s\n", err.empty() ? "no map ids" : err.c_str());
            return 2;
        }
    }
    std::optional<Terrain> terrain;
    if (const auto it = o.extra.find("terrain"); it != o.extra.end()) {
        std::string err;
        terrain = load_terrain(it->second, err);
        if (!terrain) {
            std::fprintf(stderr, "--terrain: %s\n", err.c_str());
            return 2;
        }
    }
    std::vector<std::optional<std::string>> keys(o.count);
    std::map<uint16_t, int> onInstance; // bots so far per instance map target
    std::set<std::string> copies;
    for (int i = 0; i < o.count && !targets.empty(); ++i) {
        const uint16_t t = targets[i % targets.size()];
        const BotMap& m = maps.at(t);
        if (!m.instanced) continue;
        const int size = o.extra.count("instance-size") ? std::max(1, std::stoi(o.extra.at("instance-size")))
                                                        : m.maxPlayers ? int(m.maxPlayers) : 10;
        keys[i] = "load-" + o.runId + "-" + std::to_string(t) + "-" + std::to_string(onInstance[t]++ / size);
        copies.insert(*keys[i]);
    }
    // 1. Real tokens from auth, in parallel (register + Mailpit verify on first run, then just login).
    std::vector<std::string> tokens(o.count);
    {
        std::atomic<int> nextIdx{0};
        std::vector<std::thread> pool;
        for (int w = 0; w < std::min(16, o.count); ++w)
            pool.emplace_back([&] {
                AuthClient auth(o.auth);
                for (int i = nextIdx++; i < o.count; i = nextIdx++) {
                    std::string err;
                    if (auto t = auth.ensure_login(bot_account(o, "load-" + std::to_string(i)), err)) {
                        tokens[i] = *t;
                        ++st.tokens;
                    } else {
                        st.error(err);
                    }
                }
            });
        for (auto& th : pool) th.join();
    }
    std::printf("{\"phase\":\"tokens\",\"ok\":%d,\"of\":%d}\n", st.tokens.load(), o.count);
    std::fflush(stdout);

    // 2. Sessions spread over a few io threads, connecting at connectRatePerSec.
    const auto start = std::chrono::steady_clock::now();
    const auto rampMs = static_cast<int64_t>(1000.0 * o.count / std::max(1, o.connectRatePerSec));
    const auto end = start + Millis(rampMs) + std::chrono::seconds(o.durationSec);
    std::vector<std::unique_ptr<asio::io_context>> ios;
    for (int i = 0; i < o.threads; ++i) ios.push_back(std::make_unique<asio::io_context>(1));
    asio::ssl::context tls = make_client_tls(o);
    for (int i = 0; i < o.count; ++i) {
        if (tokens[i].empty()) continue;
        const Millis delay(1000LL * i / std::max(1, o.connectRatePerSec));
        const auto target = targets.empty() ? std::nullopt : std::optional(targets[i % targets.size()]);
        asio::co_spawn(*ios[i % o.threads], run_bot(o, tls, st, tokens[i], delay, end, maps, target, keys[i], terrain ? &*terrain : nullptr), [&st](std::exception_ptr e) {
            if (!e) return;
            try { std::rethrow_exception(e); } catch (const std::exception& ex) { st.error(ex.what()); }
        });
    }
    std::vector<std::thread> threads;
    for (auto& io : ios) threads.emplace_back([&io] { io->run(); });

    // progress line every 30 s
    std::thread progress([&] {
        while (std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            std::printf("{\"phase\":\"running\",\"entered\":%d,\"unexpected_disconnects\":%d,\"errors\":%d,"
                        "\"moves_sent\":%llu,\"entity_moves_recv\":%llu,\"entity_moves_msgs_recv\":%llu}\n",
                        st.entered.load(), st.unexpectedDisconnects.load(), st.errors.load(),
                        static_cast<unsigned long long>(st.movesSent.load()),
                        static_cast<unsigned long long>(st.entityMoves.load()),
                        static_cast<unsigned long long>(st.entityMovesMsgs.load()));
            std::fflush(stdout);
        }
    });
    for (auto& th : threads) th.join();
    progress.join();

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::sort(st.enterMs.begin(), st.enterMs.end());
    const auto enterPct = [&](double q) {
        return st.enterMs.empty() ? 0.0 : std::round(st.enterMs[std::min(st.enterMs.size() - 1, size_t(q * st.enterMs.size()))]);
    };
    nlohmann::json summary = {{"phase", "summary"},
                              {"behaviour", o.behaviour},
                              {"bots", o.count},
                              {"seconds", std::round(secs)},
                              {"tokens", st.tokens.load()},
                              {"connected", st.connected.load()},
                              {"entered", st.entered.load()},
                              {"enter_ms_p50", enterPct(0.50)},
                              {"enter_ms_p99", enterPct(0.99)},
                              {"unexpected_disconnects", st.unexpectedDisconnects.load()},
                              {"expected_disconnects", st.expectedDisconnects.load()},
                              {"errors", st.errors.load()},
                              {"moves_sent", st.movesSent.load()},
                              {"entity_moves_recv", st.entityMoves.load()},
                              {"entity_moves_msgs_recv", st.entityMovesMsgs.load()},
                              {"spawns_recv", st.spawns.load()},
                              {"despawns_recv", st.despawns.load()},
                              {"corrections_recv", st.corrections.load()},
                              {"map_changes_recv", st.mapChanges.load()},
                              {"maps", o.extra.count("maps") ? o.extra.at("maps") : ""},
                              {"on_target_map", st.onTarget.load()},
                              {"instance_keys", copies.size()}, // the instance copies the bots spread over
                              {"says_sent", st.says.load()},
                              {"chat_recv", st.chats.load()},
                              {"component_updates_recv", st.componentUpdates.load()},
                              {"component_json_bytes_recv", st.componentBytes.load()},
                              {"component_removes_recv", st.componentRemoves.load()},
                              {"server_events_recv", st.serverEvents.load()},
                              {"error_samples", st.errorSamples}};
    std::printf("%s\n", summary.dump().c_str());

    bool ok = st.errors == 0 && st.unexpectedDisconnects == 0 && st.connected == o.count;
    if (o.behaviour == "malformed") ok = ok && st.expectedDisconnects == o.count;
    else ok = ok && st.entered == o.count;
    if (o.behaviour == "speedhack") ok = ok && st.corrections > 0;
    if (o.behaviour == "wander") ok = ok && st.corrections == 0; // honest movers are never corrected
    if (!targets.empty()) ok = ok && st.onTarget == st.entered;   // --maps: every bot got to its map
    std::printf("%s load %s x%d\n", ok ? "PASS" : "FAIL", o.behaviour.c_str(), o.count);
    return ok ? 0 : 1;
}

} // namespace chthonic::bot
