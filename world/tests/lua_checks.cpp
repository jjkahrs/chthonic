// Assert-style checks for the Lua layer: component definitions (every type, option and error), reserved and duplicate
// names, the sandbox, chthonic.* semantics, hooks, error handling, persistence + drift, component replication routing, the
// event bus (chthonic.publish/subscribe/unsubscribe), client events (chthonic.sendTo/sendNearby/broadcast) and state machines
// (definitions, rules, queueing, callback errors, chthonic.fsm, the StateMachine built-in, C++ / Lua parity) and NPCs
// (chthonic.create / chthonic.destroy, Transform rules, AOI and replication routing, shutdown, the sample lua/), NPC spawns
// (parse_spawns, spawn file loading errors, respawn timing, MAX_NPCS, the Spawn built-in, tile activation) and maps (tile math,
// manifests and Maps::load errors, bounds, per-map AOI, World::transfer, chthonic.transfer / Transform.map), ground and the
// navmesh (an in-memory bake, Nav::path / on_mesh, navtile load errors, off_mesh moves, chthonic.map.path).
// Lua files are written to a temp folder; the World runs with captured Send/Save, so there is no network or database.
#include "core/log.h"
#include "lua/components.h"
#include "world/components.h"
#include "world/maps.h"
#include "world/systems.h"
#include "world/world.h"
#include <bake.h>
#include <heights.h>
#include <navtile.h>
#include <zones.h>

#include <codec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <numbers>
#include <random>
#include <set>
#include <source_location>

using namespace chthonic;
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

// world_self_check --timing (ctest world_timing_bench, label perf): the timing checks' speed limits fail the run.
// Without it they only print, so a busy machine can't fail a build.
bool g_timing_gates = false;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what, std::source_location where = std::source_location::current()) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAILED lua_checks.cpp line %u: %s\n", static_cast<unsigned>(where.line()), what.c_str());
}

const TimePoint T0{std::chrono::seconds(1000)};

// A LUA_ROOT in the temp folder, removed afterwards.
struct LuaRoot {
    fs::path root;
    LuaRoot() {
        root = fs::temp_directory_path() / ("chthonic_lua_check_" + std::to_string(std::random_device{}()));
        fs::create_directories(root);
    }
    ~LuaRoot() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    LuaRoot& write(const std::string& rel, const std::string& text) {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel, std::ios::binary) << text;
        return *this;
    }
    lua::DefsResult defs() const { return lua::load_component_defs(root, world::kReservedComponentNames); }
};

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// The single error for a definitions folder holding one file with this body.
std::string def_error(const std::string& body, const std::string& file = "components/c.lua") {
    LuaRoot r;
    r.write(file, body);
    const auto d = r.defs();
    return d.errors.size() == 1 && d.defs.empty() ? d.errors[0] : "<" + std::to_string(d.errors.size()) + " errors>";
}

std::string def_with_field(const std::string& field) { return "return {name = 'C', fields = {" + field + "}}"; }

// --- definitions ---

void checkDefinitionParsing() {
    LuaRoot r;
    r.write("components/sub/health.lua", R"(return {
        name = "Health", displayName = "Hit Points", persist = true, replicate = "aoi",
        fields = {
            { name = "current", type = "float", default = 100, min = 0 },
            { name = "lives", type = "int", default = 3, min = 0, max = 9 },
            { name = "regen", type = "bool", default = true },
            { name = "title", type = "string", default = "Hero" },
            { name = "tint", type = "vec3", color = true, default = {x = 1, y = 0, z = 0} },
            { name = "facing", type = "quat" },
            { name = "faction", type = "enum", values = {"neutral", "ally"}, default = "ally" },
            { name = "drops", type = "array", of = "string", default = {"gold", "potion"} },
            { name = "low", type = "float", min = 5 },
        } })");
    r.write("components/wallet.lua", "return { name = 'Wallet', replicate = 'owner', fields = {} }");
    r.write("components/plain.lua", "return { name = 'Plain', persist = false, replicate = 'none', fields = {} }");
    r.write("components/notes.txt", "not lua");
    const auto d = r.defs();
    check(d.errors.empty(), "valid definitions load: " + (d.errors.empty() ? "" : d.errors[0]));
    check(d.defs.size() == 3, "three definitions (recursive scan, .lua only)");
    if (d.defs.size() != 3) return;
    const auto& h = *d.defs[0];
    check(h.name == "Health" && h.displayName == "Hit Points" && h.path == "components/sub/health.lua", "name/displayName/path");
    check(h.persist && h.replicate == lua::Replicate::aoi, "persist + replicate aoi");
    check(d.defs[1]->name == "Plain" && !d.defs[1]->persist && d.defs[1]->replicate == lua::Replicate::none, "explicit defaults");
    check(d.defs[2]->name == "Wallet" && !d.defs[2]->persist && d.defs[2]->replicate == lua::Replicate::owner,
          "persist defaults to false; replicate owner");
    const Json def = lua::default_component(h);
    check(def.dump() == R"({"current":100.0,"lives":3,"regen":true,"title":"Hero","tint":[1.0,0.0,0.0],)"
                        R"("facing":[0.0,0.0,0.0,1.0],"faction":"ally","drops":["gold","potion"],"low":5.0})",
          "default instance in field order: " + def.dump());
    check(h.fields[4].color && h.fields[7].array && h.fields[7].kind == lua::FieldKind::String, "color, array of");
}

void checkDefinitionErrors() {
    const std::pair<std::string, std::string> cases[] = {
        {"return 5", "components/c.lua: must return exactly one table {name = ..., fields = {...}}"},
        {"return {name='C', fields={}}, 2", "must return exactly one table"},
        {"return setmetatable({name='C', fields={}}, {})", "components/c.lua: tables must be plain data (no metatables)"},
        {"return {name='9C', fields={}}", "components/c.lua: \"name\" must be a name (letters, digits, _; not starting with a digit)"},
        {"return {fields={}}", "\"name\" must be a name"},
        {"return {name='C', fields={}, colour=1}", "components/c.lua: unknown key colour"},
        {"return {name='C', displayName='', fields={}}", "components/c.lua: \"displayName\" must be a non-empty string"},
        {"return {name='C', fields=5}", "components/c.lua: \"fields\" must be a list {...}"},
        {"return {name='C', fields={[2]={name='a', type='int'}}}", "\"fields\" must be a list {...}"},
        {"return {name='C', persist='yes', fields={}}", "components/c.lua: \"persist\" must be true or false"},
        {"return {name='C', replicate='all', fields={}}", "components/c.lua: \"replicate\" must be \"none\", \"owner\" or \"aoi\", got all"},
        {"return {name='C', replicate=true, fields={}}", "\"replicate\" must be \"none\", \"owner\" or \"aoi\", got true"},
        {def_with_field("5"), "components/c.lua: fields[1]: must be a table {name = ..., type = ...}"},
        {def_with_field("{name='1a', type='int'}"), "fields[1]: \"name\" must be a name"},
        {def_with_field("{name='a', type='int'}, {name='a', type='int'}"), "fields[2] (\"a\"): duplicate field name"},
        {def_with_field("{name='a', type='int', step=1}"), "fields[1] (\"a\"): unknown key step"},
        {def_with_field("{name='a', type='double'}"), "fields[1] (\"a\"): unknown \"type\" double (expected one of float, int, bool, string, vec3, quat, enum, array)"},
        {def_with_field("{name='a', type='array', of='array'}"), "unknown \"of\" array (expected one of float, int, bool, string, vec3, quat, enum; arrays can't nest)"},
        {def_with_field("{name='a', type='int', of='int'}"), "\"of\" is only allowed on array fields"},
        {def_with_field("{name='a', type='enum'}"), "fields[1] (\"a\"): \"values\" must be a list {...}"},
        {def_with_field("{name='a', type='enum', values={}}"), "\"values\" must not be empty"},
        {def_with_field("{name='a', type='enum', values={'x', 'x'}}"), "duplicate value \"x\" in \"values\""},
        {def_with_field("{name='a', type='enum', values={''}}"), "\"values\" must be non-empty strings"},
        {def_with_field("{name='a', type='int', values={'x'}}"), "\"values\" is only allowed on enum fields"},
        {def_with_field("{name='a', type='bool', min=0}"), "\"min\" is only allowed on float and int fields"},
        {def_with_field("{name='a', type='float', max='9'}"), "\"max\" must be a finite number"},
        {def_with_field("{name='a', type='int', min=0.5}"), "\"min\" must be an integer on an int field"},
        {def_with_field("{name='a', type='float', min=2, max=1}"), "\"min\" must not be greater than \"max\""},
        {def_with_field("{name='a', type='float', color=true}"), "\"color\" must be true or false, on vec3 fields only"},
        {def_with_field("{name='a', type='int', default='x'}"), "fields[1] (\"a\"): default \"x\" must be an integer"},
        {def_with_field("{name='a', type='int', default=20, max=10}"), "default 20 must be <= 10"},
        {def_with_field("{name='a', type='vec3', default=5}"), "default: \"a\" must be a table {x=, y=, z=} of finite numbers"},
        {def_with_field("{name='a', type='enum', values={'x'}, default='y'}"), "unknown value \"y\" (expected one of \"x\")"},
        {def_with_field("{name='a', type='array', of='int', default={1, 'b'}}"), "default [1,\"b\"] element 2 must be an integer"},
        {"return {name='C', fields={}", "components/c.lua:1:"},       // syntax error, with line
        {"\n\nerror('boom')", "components/c.lua:3: boom"},             // runtime error, with line
        {"while true do end", "script ran longer than 2 seconds"},     // watchdog
    };
    for (const auto& [body, expected] : cases) {
        const std::string error = def_error(body);
        check(contains(error, expected), "definition error: want [" + expected + "] got [" + error + "]");
    }
}

void checkNameRules() {
    for (const char* reserved : {"Character", "Transform", "StateMachine", "Npc", "Viewers", "Dirty", "SessionLink", "Visible",
                                 "StateMachineRuntime"}) {
        const std::string e = def_error(std::string("return {name='") + reserved + "', fields={}}");
        check(e == std::string("components/c.lua: name \"") + reserved + "\" is taken by a built-in component",
              "reserved name " + std::string(reserved) + ": " + e);
    }
    LuaRoot r;
    r.write("components/a.lua", "return {name='Dup', fields={}}");
    r.write("components/z/b.lua", "return {name='Dup', fields={}}");
    r.write("components/ok.lua", "return {name='Ok', fields={}}");
    r.write("components/bad.lua", "return 1");
    const auto d = r.defs();
    check(d.defs.size() == 1 && d.defs[0]->name == "Ok", "only the unique valid definition loads");
    check(d.errors.size() == 3, "every error is reported, not just the first");
    const auto has = [&](const std::string& text) {
        return std::ranges::any_of(d.errors, [&](const std::string& e) { return e == text; });
    };
    check(has("components/a.lua: name \"Dup\" is also declared in components/z/b.lua"), "dup names the other file (a)");
    check(has("components/z/b.lua: name \"Dup\" is also declared in components/a.lua"), "dup names the other file (b)");

    LuaRoot none;
    check(none.defs().defs.empty() && none.defs().errors.empty(), "no components/ folder: no definitions, no errors");
}

void checkSandbox() {
    for (const char* escape : {"io.open('x')", "os.exit(1)", "ffi.cdef('')", "debug.getinfo(1)", "package.loaded",
                               "require('x')", "loadfile('x')", "dofile('x')", "load('return 1')", "loadstring('x')",
                               "jit.on()", "setfenv(1, {})", "getmetatable('').__index.rep = nil"}) {
        const std::string e = def_error(std::string("local x = ") + escape + "\nreturn {name='C', fields={}}");
        check(contains(e, "components/c.lua:1:"), std::string("definition can't reach ") + escape + ": " + e);
    }
    check(def_error("x = string.rep('a', 3) .. math.floor(2.5) .. table.concat({'b'})\nreturn {name='C', fields={}}") ==
              "<0 errors>",
          "base/table/string/math work");
}

// --- systems ---

struct Sent {
    std::map<uint32_t, std::vector<proto::Message>> bySession;
    std::vector<db::CharSave> saves;
    void clear() { bySession.clear(); saves.clear(); }

    template <typename T>
    std::vector<T> of(uint32_t sid) const {
        std::vector<T> out;
        if (auto it = bySession.find(sid); it != bySession.end())
            for (const auto& m : it->second)
                if (auto* t = std::get_if<T>(&m)) out.push_back(*t);
        return out;
    }
    // Every EntityMoves entry sid got, in order.
    std::vector<proto::EntityMoves::Entry> moves(uint32_t sid) const {
        std::vector<proto::EntityMoves::Entry> out;
        for (const auto& m : of<proto::EntityMoves>(sid)) out.insert(out.end(), m.entries.begin(), m.entries.end());
        return out;
    }
    // Component updates for entity on sid, by name.
    std::vector<proto::ComponentUpdate> updates(uint32_t sid, uint32_t entity, const std::string& name = "") const {
        std::vector<proto::ComponentUpdate> out;
        for (const auto& u : of<proto::ComponentUpdate>(sid))
            if (u.entityId == entity && (name.empty() || u.name == name)) out.push_back(u);
        return out;
    }
};

// Map 1 "start": tiles 0..7 (the old 2048 m zone), spawn (1024, 0, 1024). Map 2 "cave": tiles 0..1, spawn
// (256, 0, 256).
world::Maps test_maps() {
    world::Maps maps;
    map::MapInfo start;
    start.id = 1;
    start.name = "start";
    start.tiles = {{0, 0, 7, 7}};
    start.spawn = {1024, 0, 1024};
    maps.add(start);
    map::MapInfo cave;
    cave.id = 2;
    cave.name = "cave";
    cave.tiles = {{0, 0, 1, 1}};
    cave.spawn = {256, 0, 256};
    cave.spawnHeading = 1;
    maps.add(cave);
    return maps;
}

struct TestWorld {
    Sent sent;
    world::Maps maps; // before world: it holds a reference
    world::World world;
    explicit TestWorld(lua::Defs defs, world::WorldRules rules = {}, world::Maps m = test_maps())
        : maps(std::move(m)), world(rules, maps, [this](uint32_t sid, const proto::Message& m) { sent.bySession[sid].push_back(m); },
                [this](std::span<const uint32_t> sids, const proto::Message& m) {
                    for (uint32_t sid : sids) sent.bySession[sid].push_back(m);
                },
                [this](const db::CharSave& s) { sent.saves.push_back(s); }, std::move(defs)) {}
    entt::entity spawn(uint32_t sid, uint64_t charId, const std::string& name, float x, float z,
                       const std::string& components = "", uint16_t map = 1, proto::CharOptions options = {}) {
        db::CharRow row{charId, 1, name, map, {x, 0, z}, 0.5f, components, std::move(options)};
        return world.spawn(sid, row, T0);
    }
    // The registry of the map context e is in (each map has its own); START_MAP's if e is gone.
    entt::registry& reg(entt::entity e) {
        entt::registry* r = world.registry_of(e);
        return r ? *r : world.registry();
    }
    // The state machines of the map context e is in (e must be in the world).
    lua::StateMachines& fsm(entt::entity e) { return world.fsm(world.map_of(e)); }
    // Every NPC in every world map's context, in map order. Creates the contexts not there yet: for worlds with a player on
    // every map.
    std::vector<std::pair<entt::entity, world::Npc>> npcs() {
        std::vector<std::pair<entt::entity, world::Npc>> out;
        for (const auto& [id, info] : maps.all())
            if (!info.instanced()) // a copy's NPCs: World::copy
                for (auto [e, n] : world.registry(id).view<world::Npc>().each()) out.emplace_back(e, n);
        return out;
    }
    // A Move of e's current epoch, as from a client that has heard every Correction so far.
    void move(entt::entity e, proto::Move m) {
        m.epoch = reg(e).get<world::MoveState>(e).epoch;
        world.push_move(e, m);
    }
    Json value(entt::entity e, const std::string& name) {
        auto& p = lua::pool(reg(e), name);
        return p.contains(e) ? p.get(e).value : Json();
    }
};

uint32_t wire(entt::entity e) { return entt::to_integral(e); }

// A watcher player (session 900 + i) on every spawn file entry's spot: the tiles load and stay awake, as every spawn
// did before tile activation, for checks about something else.
void watch_spawns(TestWorld& w) {
    uint32_t i = 0;
    for (const auto& [id, info] : w.maps.all())
        for (const auto& s : info.spawns) {
            if (info.instanced()) continue; // no copy to watch
            w.spawn(900 + i, 900 + i, "Watcher" + std::to_string(i), s.pos[0], s.pos[2], "", id);
            ++i;
        }
}

constexpr const char* kStats = R"(return { name = "Stats", fields = {
    { name = "hp", type = "int", default = 10, min = 0, max = 100 },
    { name = "flag", type = "bool" },
    { name = "kind", type = "enum", values = {"a", "b"}, default = "b" },
    { name = "pos", type = "vec3", default = {x = 1, y = 2, z = 3} },
    { name = "tags", type = "array", of = "string", default = {"a"} },
} })";
constexpr const char* kReport = R"(return { name = "Report", fields = { { name = "lines", type = "array", of = "string" } } })";

// Lua-side asserts: each failure becomes a line of the Report component on the last character.
constexpr const char* kApiSystem = R"(
local fails = {}
local function expect(ok, what) if not ok then fails[#fails + 1] = what end end
local function raises(want, f, ...)
    local ok, err = pcall(f, ...)
    if ok then fails[#fails + 1] = "no error, wanted: " .. want
    elseif not string.find(err, want, 1, true) then fails[#fails + 1] = "got [" .. tostring(err) .. "] wanted [" .. want .. "]" end
end
local done = false
leak = "from api"
return { update = function(dt)
    if done then return end
    done = true
    local ids = {}
    for e in chthonic.query("Character", "Transform") do ids[#ids + 1] = e end
    expect(#ids == 2 and ids[1] < ids[2], "query finds both characters in wire id order")
    -- The API has one global, chthonic; no other name is an alias.
    expect(_G["k" .. "ast"] == nil, "no global but chthonic for the API")
    raises("attempt to index", function() return _G["k" .. "ast"].query("Character") end)
    local e = ids[1]
    local ch = chthonic.get(e, "Character")
    expect(ch.name == "Alice" and ch.charId == 11, "Character {charId, name}")
    local tf = chthonic.get(e, "Transform")
    expect(tf.position.x == 1000 and tf.position.y == 0 and tf.position.z == 1000 and tf.heading == 0.5, "Transform")
    expect(chthonic.has(e, "Character") and chthonic.has(e, "Transform"), "built-ins always present")
    raises('"Transform" is read-only on the server', chthonic.set, e, "Transform", {heading = 1})
    raises('"Character" is read-only on the server', chthonic.add, e, "Character")
    raises('"Transform" cannot be removed', chthonic.remove, e, "Transform")
    raises('unknown component "Nope"', chthonic.get, e, "Nope")
    raises('unknown component "Nope"', chthonic.query, "Nope")
    raises('component name must be a string, got 5', chthonic.query, 5)
    raises('invalid entity id 999999', chthonic.get, 999999, "Character")
    raises('invalid entity id 1.5', chthonic.has, 1.5, "Character")
    raises('invalid entity id x', chthonic.add, "x", "Stats")
    expect(not chthonic.has(e, "Stats"), "no Stats yet")
    raises('entity ' .. e .. ' has no "Stats"', chthonic.get, e, "Stats")
    raises('entity ' .. e .. ' has no "Stats"', chthonic.set, e, "Stats", {hp = 1})
    chthonic.add(e, "Stats")
    chthonic.add(e, "Stats") -- already there: nothing happens
    local s = chthonic.get(e, "Stats")
    expect(s.hp == 10 and s.flag == false and s.kind == "b" and s.pos.x == 1 and s.pos.z == 3 and s.tags[1] == "a" and #s.tags == 1, "defaults")
    chthonic.set(e, "Stats", {hp = 20, tags = {"x", "y"}})
    s = chthonic.get(e, "Stats")
    expect(s.hp == 20 and s.kind == "b" and s.tags[2] == "y", "partial set keeps the other fields")
    raises('"Stats": unknown field "nope"', chthonic.set, e, "Stats", {hp = 30, nope = 1})
    raises('"Stats": field names must be strings, got 1', chthonic.set, e, "Stats", {5})
    raises('"Stats": "hp" must be <= 100', chthonic.set, e, "Stats", {hp = 101})
    raises('"Stats": "hp" must be >= 0', chthonic.set, e, "Stats", {hp = -1})
    raises('"Stats": "hp" must be an integer', chthonic.set, e, "Stats", {hp = 1.5})
    raises('"Stats": "kind" unknown value "zzz" (expected one of "a", "b")', chthonic.set, e, "Stats", {kind = "zzz"})
    raises('"pos" must be a table {x=, y=, z=} of finite numbers', chthonic.set, e, "Stats", {hp = 30, pos = 5})
    raises('"tags" must be a sequence table {v1, v2, ...}', chthonic.set, e, "Stats", {tags = {x = 1}})
    raises('"Stats": "tags" element 2 must be a string', chthonic.set, e, "Stats", {hp = 30, tags = {"a", 3}})
    raises('"Stats": "flag" must be true or false', chthonic.set, e, "Stats", {hp = 50, flag = "x"})
    raises('"hp" must be a finite number', chthonic.set, e, "Stats", {hp = 1 / 0})
    s = chthonic.get(e, "Stats")
    expect(s.hp == 20 and s.flag == false and s.kind == "b" and s.tags[1] == "x" and s.pos.x == 1, "every failed set left Stats unchanged")
    chthonic.remove(e, "Stats")
    chthonic.remove(e, "Stats") -- not there: nothing happens
    expect(not chthonic.has(e, "Stats"), "removed")
    local n = 0
    for _ in chthonic.query("Stats") do n = n + 1 end
    expect(n == 0, "query Stats after remove")
    raises("chthonic is read-only", function() chthonic.get = nil end)
    expect(io == nil and os == nil and ffi == nil and debug == nil and package == nil and jit == nil and require == nil
        and loadfile == nil and dofile == nil and load == nil and loadstring == nil, "systems are sandboxed")
    chthonic.add(ids[2], "Report")
    chthonic.set(ids[2], "Report", {lines = fails})
end })";

constexpr const char* kIsolationSystem = R"(
local seen = leak
return { update = function() if seen ~= nil then chthonic.log("leak") ; error("globals leaked between systems") end end })";

void checkChthonicApi() {
    LuaRoot r;
    r.write("components/stats.lua", kStats).write("components/report.lua", kReport);
    r.write("systems/a_api.lua", kApiSystem).write("systems/b_isolation.lua", kIsolationSystem);
    TestWorld w(r.defs().defs);
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "API systems load" + (errors.empty() ? "" : ": " + errors[0]));
    w.spawn(1, 11, "Alice", 1000, 1000);
    const auto bob = w.spawn(2, 12, "Bob", 1010, 1000);
    w.world.tick(T0);
    const Json report = w.value(bob, "Report");
    check(report.is_object(), "API system wrote its report");
    for (const auto& line : report.value("lines", Json::array())) check(false, "chthonic.*: " + line.get<std::string>());
    for (const auto& s : w.world.script_status()) check(s.ok, "system still enabled: " + s.path + " " + s.error);
}

// Hook order: init, onEnter (after the persisted components are restored), update, onLeave (before the save), and
// on shutdown onLeave for everyone, then shutdown, then the saves.
void checkHooks() {
    LuaRoot r;
    r.write("components/health.lua", R"(return { name = "Health", persist = true, replicate = "aoi",
        fields = { { name = "current", type = "float", default = 100 } } })");
    r.write("components/report.lua", kReport);
    r.write("systems/hooks.lua", R"(
local events = {}
local function note(s) events[#events + 1] = s end
return {
    init = function() note("init") ; for _ in chthonic.query("Character") do note("char at init") end end,
    onEnter = function(e)
        note("enter " .. chthonic.get(e, "Character").name .. " health " .. (chthonic.has(e, "Health") and chthonic.get(e, "Health").current or "none"))
        if not chthonic.has(e, "Report") then chthonic.add(e, "Report") end
    end,
    update = function(dt)
        note("update " .. dt)
        for e in chthonic.query("Report") do chthonic.set(e, "Report", {lines = events}) end
    end,
    onLeave = function(e) chthonic.set(e, "Health", {current = 42}) end,
    shutdown = function() for e in chthonic.query("Health") do chthonic.set(e, "Health", {current = chthonic.get(e, "Health").current + 1}) end end,
})");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "hooks system loads");
    const auto a = w.spawn(1, 21, "Alice", 1000, 1000, R"({"Health":{"current":55}})");
    w.world.tick(T0);
    const Json lines = w.value(a, "Report").value("lines", Json::array());
    check(lines.size() == 3 && lines[0] == "init" && lines[1] == "enter Alice health 55" &&
              lines[2].get<std::string>().starts_with("update 0.05"),
          "init, then onEnter after restore, then update(dt): " + lines.dump());

    w.sent.clear();
    w.world.despawn(a);
    check(w.sent.saves.size() == 1 && w.sent.saves[0].charId == 21 && w.sent.saves[0].components &&
              Json::parse(*w.sent.saves[0].components)["Health"]["current"] == 42.0,
          "onLeave runs before the despawn save");

    const auto b = w.spawn(2, 22, "Bob", 1000, 1000, R"({"Health":{"current":1}})");
    w.sent.clear();
    w.world.shutdown_scripts();
    check(w.sent.saves.empty(), "shutdown_scripts saves nothing itself");
    w.world.despawn(b);
    check(w.sent.saves.size() == 1 && Json::parse(*w.sent.saves[0].components)["Health"]["current"] == 43.0,
          "shutdown: onLeave (42), then shutdown (+1), then the final save; onLeave not run twice");
}

// Character creation options: validateCharacter(name, options) in any system decides; without one only empty options
// pass. Every hook must return true; an error rejects and disables its system. chthonic.get(e, "Character").options.
void checkCharacterCreation() {
    {
        LuaRoot r;
        r.write("systems/plain.lua", "return { update = function() end }");
        TestWorld w({});
        check(w.world.load_scripts(r.root).empty(), "creation: plain system loads");
        check(w.world.validate_character("Ann", {}) && !w.world.validate_character("Ann", {{"class", "mage"}}),
              "creation: no hook -> empty options pass, any option is refused");
    }
    LuaRoot r;
    r.write("components/report.lua", kReport);
    r.write("systems/a_class.lua", R"(return {
    update = function() end,
    validateCharacter = function(name, options) return options.class == "mage" and name ~= "Bad" end,
    onEnter = function(e)
        local o = chthonic.get(e, "Character").options
        chthonic.add(e, "Report")
        chthonic.set(e, "Report", {lines = {tostring(o.class), tostring(o.hair)}})
    end,
})");
    r.write("systems/b_hair.lua", R"(return {
    update = function() end,
    validateCharacter = function(name, options)
        if options.hair == "boom" then error("bad hair") end
        if options.hair == "yes" then return "yes" end
        return options.hair ~= "green"
    end,
})");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "creation: hook systems load");
    check(w.world.validate_character("Ann", {{"class", "mage"}}) && w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "red"}}),
          "creation: accepted when every hook returns true");
    check(!w.world.validate_character("Ann", {}) && !w.world.validate_character("Ann", {{"class", "rogue"}}) &&
              !w.world.validate_character("Bad", {{"class", "mage"}}),
          "creation: the first hook refuses a missing class, another class and the name Bad");
    check(!w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "green"}}) &&
              !w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "yes"}}),
          "creation: the second hook refuses too (false, and a non-boolean)");
    const auto a = w.spawn(1, 41, "Ann", 1000, 1000, "", 1, {{"class", "mage"}, {"hair", "red"}});
    const auto b = w.spawn(2, 42, "Old", 1010, 1000);
    check(w.value(a, "Report")["lines"] == Json::parse(R"(["mage", "red"])") &&
              w.value(b, "Report")["lines"] == Json::parse(R"(["nil", "nil"])"),
          "creation: chthonic.get(e, \"Character\").options as created; none for an old character");
    check(!w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "boom"}}), "creation: an error in a hook refuses");
    const auto status = w.world.script_status();
    check(status.size() == 2 && status[0].ok && !status[1].ok && contains(status[1].error, "systems/b_hair.lua:4: bad hair"),
          "creation: the failing hook's system is disabled: " + status[1].error);
    check(w.world.validate_character("Ann", {{"class", "mage"}, {"hair", "green"}}), "creation: a disabled system's hook no longer runs");
}

void checkErrorsDisableOneSystem() {
    LuaRoot r;
    r.write("components/report.lua", kReport);
    r.write("systems/a_bad.lua", "local n = 0\nreturn { update = function()\n n = n + 1\n if n == 2 then error('boom') end\nend }");
    r.write("systems/b_good.lua", R"(local n = 0
return { update = function()
    n = n + 1
    for e in chthonic.query("Report") do chthonic.set(e, "Report", {lines = {tostring(n)}}) end
end, onEnter = function(e) chthonic.add(e, "Report") end })");
    r.write("systems/c_enter.lua", "return { update = function() end, onEnter = function(e)\n chthonic.get(e, 'Nope')\nend }");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "error systems load");
    const auto a = w.spawn(1, 31, "Alice", 1000, 1000);
    for (int i = 0; i < 3; ++i) w.world.tick(T0 + std::chrono::milliseconds(50 * i));
    check(w.value(a, "Report")["lines"] == Json::parse(R"(["3"])"), "the good system kept running every tick");
    const auto status = w.world.script_status();
    check(status.size() == 3 && !status[0].ok && contains(status[0].error, "systems/a_bad.lua:4: boom"),
          "runtime error disables that system, logged with path:line: " + status[0].error);
    check(status[1].ok, "other system still enabled");
    check(!status[2].ok && contains(status[2].error, "systems/c_enter.lua:2: unknown component \"Nope\""),
          "an onEnter error disables only its system: " + status[2].error);
    check(w.value(a, "Report").is_object(), "the next system's onEnter still ran");
}

void checkStartupErrors() {
    LuaRoot r;
    r.write("systems/a_syntax.lua", "return {");
    r.write("systems/b_noupdate.lua", "return {}");
    r.write("systems/c_number.lua", "return 5");
    r.write("systems/d_hook.lua", "return { update = function() end, onEnter = 5 }");
    r.write("systems/e_init.lua", "return { update = function() end, init = function()\n error('init failed') end }");
    r.write("systems/f_io.lua", "io.write('x')\nreturn { update = function() end }");
    r.write("systems/g_ok.lua", "return { update = function() end }");
    r.write("systems/sub/ignored.lua", "return 5"); // not recursive
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    const auto has = [&](const std::string& part) {
        return std::ranges::any_of(errors, [&](const std::string& e) { return contains(e, part); });
    };
    check(errors.size() == 6, "every bad system is reported: " + std::to_string(errors.size()));
    check(has("systems/a_syntax.lua:1:"), "syntax error with line");
    check(has("systems/b_noupdate.lua: returned table has no update function"), "no update");
    check(has("systems/c_number.lua: must return a table with an update function"), "not a table");
    check(has("systems/d_hook.lua: \"onEnter\" must be a function"), "bad hook");
    check(has("systems/e_init.lua:2: init failed"), "init error refuses the start");
    check(has("systems/f_io.lua:1:"), "io is not reachable");
    const auto status = w.world.script_status();
    check(status.size() == 7 && status[6].path == "systems/g_ok.lua" && status[6].ok, "alphabetical, the good one ok");

    LuaRoot empty;
    TestWorld e({});
    check(e.world.load_scripts(empty.root).empty() && e.world.script_status().empty(), "no systems/ folder is fine");
}

// --- persistence ---

void checkPersistence() {
    LuaRoot r;
    r.write("components/health.lua", R"(return { name = "Health", persist = true, fields = {
        { name = "current", type = "float", default = 100, min = 0 }, { name = "max", type = "float", default = 100 } } })");
    r.write("components/temp.lua", "return { name = 'Temp', fields = { { name = 'v', type = 'int' } } }");
    TestWorld w(r.defs().defs);
    const auto a = w.spawn(1, 41, "Alice", 1000, 1000,
                           R"({"Health":{"current":"bad","max":50,"extra":1},"Old":{"x":[1,2],"s":"keep"},"Temp":{"v":9}})");
    check(w.value(a, "Health") == Json::parse(R"({"current":100.0,"max":50.0})"),
          "drift on load: bad value reset to default, unknown field dropped: " + w.value(a, "Health").dump());
    check(w.value(a, "Temp").is_null(), "a non-persist component in the column is not restored");
    auto& reg = w.world.registry();
    lua::pool(reg, "Temp").emplace(a, lua::ComponentData{Json::parse(R"({"v":1})")});
    lua::pool(reg, "Health").get(a).value["current"] = 12.5;
    const auto column = lua::components_column(reg, a, r.defs().defs);
    const Json saved = column ? Json::parse(*column) : Json();
    check(saved["Health"] == Json::parse(R"({"current":12.5,"max":50.0})"), "persist component saved");
    check(saved["Old"] == Json::parse(R"({"x":[1,2],"s":"keep"})"), "unknown component written back verbatim");
    check(saved["Temp"] == Json::parse(R"({"v":9})"), "non-persist entry kept verbatim, not the runtime value");

    // Round trip: what was saved restores to the same values.
    const auto b = w.spawn(2, 42, "Bob", 1000, 1000, *column);
    check(w.value(b, "Health") == w.value(a, "Health"), "save -> load round trip");

    const auto c = w.spawn(3, 43, "Carol", 1000, 1000, "[1,2]");
    check(!lua::components_column(reg, c, r.defs().defs), "unreadable column is never overwritten");
    const auto d = w.spawn(4, 44, "Dan", 1000, 1000);
    check(lua::components_column(reg, d, r.defs().defs) == std::optional<std::string>("{}"), "empty column -> {}");

    // chthonic changes to a persisted component mark the character Dirty, so save_system picks it up.
    r.write("systems/s.lua", "return { update = function() for e in chthonic.query('Health') do chthonic.set(e, 'Health', {max = 77}) end end }");
    TestWorld w2(r.defs().defs);
    check(w2.world.load_scripts(r.root).empty(), "persist system loads");
    const auto e = w2.spawn(1, 45, "Eve", 1000, 1000, R"({"Health":{"current":1,"max":2}})");
    check(!w2.world.registry().all_of<world::Dirty>(e), "not dirty after spawn");
    w2.world.tick(T0 + std::chrono::seconds(120)); // past the save interval
    check(w2.sent.saves.size() == 1 && Json::parse(*w2.sent.saves[0].components)["Health"]["max"] == 77.0,
          "a chthonic.set on a persist component is saved by the periodic save");
}

// --- replication ---

void checkReplication() {
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', replicate = 'aoi', fields = { { name = 'hp', type = 'int', default = 5 } } }");
    r.write("components/wallet.lua", "return { name = 'Wallet', replicate = 'owner', fields = { { name = 'gold', type = 'int' } } }");
    r.write("components/secret.lua", "return { name = 'Secret', fields = { { name = 'v', type = 'int' } } }");
    r.write("components/big.lua", "return { name = 'Big', replicate = 'aoi', fields = { { name = 's', type = 'string' } } }");
    TestWorld w(r.defs().defs);
    auto& reg = w.world.registry();
    // Writes storage directly, so it marks the component dirty itself, as chthonic.add/set/remove do.
    const auto set = [&](entt::entity e, const char* name, const char* json) {
        auto& p = lua::pool(reg, name);
        if (p.contains(e)) p.get(e).value = Json::parse(json);
        else p.emplace(e, lua::ComponentData{Json::parse(json)});
        world::mark_dirty(reg, e, name);
    };
    const auto a = w.spawn(1, 51, "Alice", 1000, 1000);
    const auto b = w.spawn(2, 52, "Bob", 1050, 1000);
    const auto c = w.spawn(3, 53, "Carol", 1800, 1800);
    const uint32_t A = wire(a);
    set(a, "Health", R"({"hp":5})");
    set(a, "Wallet", R"({"gold":7})");
    set(a, "Secret", R"({"v":1})");
    w.world.tick(T0);

    check(w.sent.updates(1, A, "Health").size() == 1 && w.sent.updates(1, A, "Health")[0].json == R"({"hp":5})", "owner gets aoi");
    check(w.sent.updates(1, A, "Wallet").size() == 1 && w.sent.updates(1, A, "Wallet")[0].json == R"({"gold":7})", "owner gets owner");
    check(w.sent.updates(2, A, "Health").size() == 1, "observer gets aoi");
    check(w.sent.updates(2, A, "Wallet").empty(), "observer never gets owner");
    check(w.sent.updates(1, A, "Secret").empty() && w.sent.updates(2, A, "Secret").empty(), "none is never sent");
    check(w.sent.updates(3, A).empty(), "out of range gets nothing");
    // Spawn before any component data for that entity.
    const auto& bMsgs = w.sent.bySession[2];
    const auto spawnAt = std::ranges::find_if(bMsgs, [&](const auto& m) { auto* s = std::get_if<proto::Spawn>(&m); return s && s->entityId == A; });
    const auto updateAt = std::ranges::find_if(bMsgs, [&](const auto& m) { auto* u = std::get_if<proto::ComponentUpdate>(&m); return u && u->entityId == A; });
    check(spawnAt != bMsgs.end() && updateAt != bMsgs.end() && spawnAt < updateAt, "Spawn arrives before ComponentUpdate");

    w.sent.clear();
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(w.sent.of<proto::ComponentUpdate>(1).empty() && w.sent.of<proto::ComponentUpdate>(2).empty(), "unchanged is not resent");

    w.sent.clear();
    set(a, "Health", R"({"hp":6})");
    lua::pool(reg, "Wallet").remove(a);
    world::mark_dirty(reg, a, "Wallet");
    w.world.tick(T0 + std::chrono::milliseconds(100));
    check(w.sent.updates(1, A).size() == 1 && w.sent.updates(2, A).size() == 1 && w.sent.updates(2, A)[0].json == R"({"hp":6})",
          "a change goes to owner + observers once");
    check(w.sent.of<proto::ComponentRemove>(1).size() == 1 && w.sent.of<proto::ComponentRemove>(1)[0].name == "Wallet" &&
              w.sent.of<proto::ComponentRemove>(2).empty(),
          "owner component removal goes to the owner only");

    // Carol walks into range in the same tick Alice's Health changes: one full-set update, after Spawn.
    w.sent.clear();
    set(a, "Health", R"({"hp":7})");
    reg.get<world::Transform>(c).pos = {1000, 0, 1060};
    reg.emplace_or_replace<world::Moved>(c);
    w.world.tick(T0 + std::chrono::milliseconds(150));
    const auto cMsgs = w.sent.bySession[3];
    check(w.sent.updates(3, A).size() == 1 && w.sent.updates(3, A)[0].json == R"({"hp":7})", "AOI enter sends the full set once");
    check(!cMsgs.empty() && std::holds_alternative<proto::Spawn>(cMsgs.front()), "Spawn first for the new viewer");
    check(w.sent.updates(2, A).size() == 1, "existing observer gets the change once");

    // Big component: not sent, logged once; Despawn means nothing more for that id.
    w.sent.clear();
    set(a, "Big", (R"({"s":")" + std::string(70000, 'x') + R"("})").c_str());
    w.world.tick(T0 + std::chrono::milliseconds(200));
    check(w.sent.updates(1, A, "Big").empty() && w.sent.updates(2, A, "Big").empty(), "too-large component not sent");
    w.world.despawn(a);
    w.sent.clear();
    w.world.tick(T0 + std::chrono::milliseconds(250));
    for (uint32_t sid : {1u, 2u, 3u}) check(w.sent.updates(sid, A).empty(), "nothing after Despawn");
    (void)b;
}

// Idle cost of component_replication_system: 1000 entities (one viewer + 999 NPCs it sees), 3 replicated components
// each, then ticks with no writes.
void checkReplicationIdleTiming() {
    LuaRoot r;
    for (const char* n : {"RepA", "RepB", "RepC"})
        r.write(std::string("components/") + n + ".lua",
                std::string("return { name = '") + n + "', replicate = 'aoi', fields = { { name = 'v', type = 'int' } } }");
    const lua::Defs defs = r.defs().defs;
    TestWorld w(defs);
    w.spawn(1, 61, "Viewer", 1000, 1000);
    const Json comps = Json::parse(R"({"RepA":{"v":1},"RepB":{"v":2},"RepC":{"v":3}})");
    for (int i = 0; i < 999; ++i)
        w.world.create_npc("N", "", 1, proto::Vec3{1000.0f + i % 30, 0, 1000.0f + i / 30}, 0, "", comps);
    w.world.tick(T0);                                    // Spawns
    w.world.tick(T0 + std::chrono::milliseconds(50));    // nothing entered view any more
    check(w.sent.updates(1, wire(w.world.registry().view<world::Npc>().front())).size() == 3, "timing: full set sent once");
    size_t sends = 0;
    const world::Send send = [&](uint32_t, const proto::Message&) { ++sends; };
    const world::SendMany sendMany = [&](std::span<const uint32_t> sids, const proto::Message&) { sends += sids.size(); };
    constexpr int kTicks = 200;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kTicks; ++i) world::component_replication_system(w.world.registry(), defs, send, sendMany);
    const double perTickUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kTicks;
    check(sends == 0, "timing: idle ticks send nothing");
    std::printf("component_replication_system idle: %.1f us/tick (1000 entities x 3 components)\n", perTickUs);
#ifdef NDEBUG
    // Baseline before dirty flags (every component compared every tick): 59.0-61.7 us/tick over 5 runs, Release
    // (preset release, MSVC, the dev machine). Dirty flags must make idle ticks at least 5x cheaper. Checked only
    // with --timing in Release.
    constexpr double kBaselineUs = 60.0;
    if (g_timing_gates)
        check(perTickUs <= kBaselineUs / 5, std::format("timing: idle {:.1f} us/tick > baseline {} / 5", perTickUs, kBaselineUs));
#endif
}

// Cost of chthonic.query on a rare Lua component: 10000 NPCs, 10 of them with Health, queried from a Lua update. A
// Runtime of its own on the World's registry, so the timed update runs nothing else.
void checkQueryTiming() {
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', fields = { { name = 'hp', type = 'int' } } }");
    r.write("systems/q.lua", R"(local n = 0
return { update = function()
    for _ = 1, 100 do for _ in chthonic.query("Health") do n = n + 1 end end
end })");
    const lua::Defs defs = r.defs().defs;
    TestWorld w(defs);
    const Json health = Json::parse(R"({"Health":{"hp":1}})");
    for (int i = 0; i < 10000; ++i)
        w.world.create_npc("N", "", 1, proto::Vec3{1000.0f + i % 100, 0, 1000.0f + i / 100}, 0, "",
                           i % 1000 == 500 ? health : Json::object());
    lua::Runtime rt(w.world.registry(), defs);
    check(rt.load(r.root).empty(), "query timing: loads");
    constexpr int kUpdates = 10, kQueries = kUpdates * 100;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kUpdates; ++i) rt.update(0.05f);
    const double perQueryUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kQueries;
    check(lua::pool(w.world.registry(), "Health").size() == 10, "query timing: 10 Health");
    for (const auto& s : rt.status()) check(s.ok, "query timing: system still enabled " + s.error);
    std::printf("chthonic.query(\"Health\"): %.1f us/query (10000 NPCs, 10 with Health)\n", perQueryUs);
#ifdef NDEBUG
    // Baseline before walking the smallest pool (every Transform entity checked, its storage hashed per entity):
    // 240.9-244.7 us/query over 5 runs, Release (preset release, MSVC, the dev machine). Must be at least 10x cheaper.
    // Checked only with --timing in Release.
    constexpr double kBaselineUs = 241.0;
    if (g_timing_gates)
        check(perQueryUs <= kBaselineUs / 10, std::format("timing: {:.1f} us/query > baseline {} / 10", perQueryUs, kBaselineUs));
#endif
}

// --- events (docs/world/events.html) ---

std::vector<std::string> g_log; // log lines while a LogCapture lives; the log backend thread appends

// The captured lines, once the log backend has written everything logged so far.
const std::vector<std::string>& captured() {
    log::flush();
    return g_log;
}
void clear_log() {
    log::flush();
    g_log.clear();
}

struct LogCapture {
    LogCapture() {
        clear_log();
        log::set_test_sink([](const std::string& line) { g_log.push_back(line); });
    }
    ~LogCapture() {
        log::flush();
        log::set_test_sink(nullptr);
    }
};

// Index of the first captured log line containing part, or SIZE_MAX.
size_t log_index(const std::string& part) {
    const auto& lines = captured();
    for (size_t i = 0; i < lines.size(); ++i)
        if (contains(lines[i], part)) return i;
    return SIZE_MAX;
}
bool logged(const std::string& part) { return log_index(part) != SIZE_MAX; }

// Dirty-flag replication through the real writers (chthonic.add/set/remove, create_npc, login restore): exactly what each
// session receives, tick by tick.
void checkReplicationDirty() {
    LogCapture capture;
    LuaRoot r;
    r.write("components/a_health.lua", "return { name = 'Health', replicate = 'aoi', fields = { { name = 'hp', type = 'int', default = 5 } } }");
    r.write("components/b_wallet.lua", "return { name = 'Wallet', replicate = 'owner', fields = { { name = 'gold', type = 'int' } } }");
    r.write("components/c_big.lua", "return { name = 'Big', replicate = 'aoi', fields = { { name = 's', type = 'string' } } }");
    r.write("components/d_saved.lua",
            "return { name = 'Saved', replicate = 'owner', persist = true, fields = { { name = 'n', type = 'int' } } }");
    r.write("systems/s.lua", R"(
local t = 0
local function alice() for e in chthonic.query('Character') do if chthonic.get(e, 'Character').name == 'Alice' then return e end end end
return { update = function()
    t = t + 1
    local a = alice()
    if t == 2 then chthonic.add(a, 'Health'); chthonic.set(a, 'Health', {hp = 6}); chthonic.add(a, 'Wallet')
    elseif t == 4 then chthonic.set(a, 'Health', {hp = 9}); chthonic.set(a, 'Health', {hp = 6})
    elseif t == 5 then chthonic.remove(a, 'Health')
    elseif t == 6 then chthonic.remove(a, 'Wallet'); chthonic.add(a, 'Wallet'); chthonic.set(a, 'Wallet', {gold = 3})
    elseif t == 7 then chthonic.remove(a, 'Wallet')
    elseif t == 8 then chthonic.add(a, 'Wallet'); chthonic.add(a, 'Health')
    elseif t == 10 then chthonic.add(a, 'Big'); chthonic.set(a, 'Big', {s = string.rep('x', 70000)})
    elseif t == 11 then chthonic.set(a, 'Big', {s = string.rep('y', 70000)})
    elseif t == 12 then chthonic.set(a, 'Big', {s = 'ok'})
    elseif t == 14 then for n in chthonic.query('Npc') do chthonic.set(n, 'Health', {hp = 3}); chthonic.set(n, 'Wallet', {gold = 2}) end
    end
end })");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "dirty: system loads");
    auto& reg = w.world.registry();
    const auto a = w.spawn(1, 71, "Alice", 1000, 1000);
    w.spawn(2, 72, "Bob", 1050, 1000);
    const auto c = w.spawn(3, 73, "Carol", 1800, 1800);
    const uint32_t A = wire(a);
    using V = std::vector<std::string>;
    const auto comps = [&](uint32_t sid) {
        V out;
        if (const auto it = w.sent.bySession.find(sid); it != w.sent.bySession.end())
            for (const auto& m : it->second) {
                if (const auto* u = std::get_if<proto::ComponentUpdate>(&m))
                    out.push_back(std::format("update {} {} {}", u->entityId, u->name, u->json));
                if (const auto* x = std::get_if<proto::ComponentRemove>(&m)) out.push_back(std::format("remove {} {}", x->entityId, x->name));
            }
        return out;
    };
    const auto text = [](const V& v) {
        std::string out;
        for (const auto& s : v) out += s.substr(0, 60) + "; ";
        return out;
    };
    int tick = 0;
    const auto step = [&] {
        w.sent.clear();
        w.world.tick(T0 + std::chrono::milliseconds(50 * tick++));
    };
    const auto expect = [&](const V& s1, const V& s2, const V& s3, const std::string& what) {
        check(comps(1) == s1 && comps(2) == s2 && comps(3) == s3,
              "dirty: " + what + ": " + text(comps(1)) + " | " + text(comps(2)) + " | " + text(comps(3)));
    };
    const auto upd = [](uint32_t id, const char* name, const char* json) { return std::format("update {} {} {}", id, name, json); };
    const auto rem = [](uint32_t id, const char* name) { return std::format("remove {} {}", id, name); };

    step(); // t1: Spawns only
    expect({}, {}, {}, "nothing written, nothing sent");
    step();
    expect({upd(A, "Health", R"({"hp":6})"), upd(A, "Wallet", R"({"gold":0})")}, {upd(A, "Health", R"({"hp":6})")}, {},
           "a change goes once to the owner (owner + aoi) and to viewers (aoi)");
    step();
    expect({}, {}, {}, "untouched components send nothing");
    step();
    expect({}, {}, {}, "set then restored in the same tick sends nothing");
    step();
    expect({rem(A, "Health")}, {rem(A, "Health")}, {}, "remove sends ComponentRemove");
    step();
    expect({upd(A, "Wallet", R"({"gold":3})")}, {}, {}, "remove + re-add + set in one tick sends the new value");
    step();
    expect({rem(A, "Wallet")}, {}, {}, "remove (owner component)");
    step();
    expect({upd(A, "Health", R"({"hp":5})"), upd(A, "Wallet", R"({"gold":0})")}, {upd(A, "Health", R"({"hp":5})")}, {},
           "re-added after a remove: sent again");

    reg.get<world::Transform>(c).pos = {1000, 0, 1060}; // Carol walks into range
    reg.emplace_or_replace<world::Moved>(c);
    step();
    expect({}, {}, {upd(A, "Health", R"({"hp":5})")}, "a new viewer gets the aoi set (cached), nobody else anything");

    const auto oversizeLogs = [&] {
        return std::ranges::count_if(captured(), [](const std::string& l) { return contains(l, "component too large"); });
    };
    step();
    expect({}, {}, {}, "oversize: not sent");
    step();
    expect({}, {}, {}, "oversize changed again: still not sent");
    check(oversizeLogs() == 1, "dirty: oversize logged once per episode: " + std::to_string(oversizeLogs()));
    step();
    const V big{upd(A, "Big", R"({"s":"ok"})")};
    expect(big, big, big, "oversize fits again: sent");

    const auto n = w.world.create_npc("Guard", "", 1, proto::Vec3{1020, 0, 1000}, 0, "",
                                      Json::parse(R"({"Health":{"hp":4},"Wallet":{"gold":1}})"));
    const V npcNew{upd(wire(n), "Health", R"({"hp":4})")};
    step();
    expect(npcNew, npcNew, npcNew, "NPC: aoi component reaches its viewers, owner component nobody");
    step();
    const V npcSet{upd(wire(n), "Health", R"({"hp":3})")};
    expect(npcSet, npcSet, npcSet, "NPC: changes to its aoi component only");

    const auto d = w.spawn(4, 74, "Dan", 200, 200, R"({"Saved":{"n":8}})");
    step();
    check(comps(4) == V{upd(wire(d), "Saved", R"({"n":8})")}, "dirty: a component restored on login is sent on the first tick: " + text(comps(4)));
}

void checkEventBus() {
    LogCapture capture;
    EventBus bus;
    std::vector<std::string> seen;
    const auto record = [&](std::string tag) {
        return [&seen, tag](const nlohmann::json& payload) { seen.push_back(tag + payload.dump()); };
    };
    EventBus::Subscription a1 = bus.subscribe("a", record("a1"));
    EventBus::Subscription b1 = bus.subscribe("b", record("b1"));
    EventBus::Subscription a2 = bus.subscribe("a", record("a2"));
    bus.publish("b", 1);
    bus.publish("a", 2);
    check(seen.empty(), "bus: publish only queues");
    bus.drain();
    check(seen == std::vector<std::string>{"b11", "a12", "a22"}, "bus: publish order, then subscription order");

    // A subscriber that re-publishes, subscribes and unsubscribes during a drain.
    seen.clear();
    std::vector<EventBus::Subscription> added;
    EventBus::Subscription cut;
    EventBus::Subscription actor = bus.subscribe("act", [&](const nlohmann::json&) {
        seen.push_back("actor");
        bus.publish("act", "again");                      // next drain
        added.push_back(bus.subscribe("act", record("late"))); // from the next event
        cut.unsubscribe();                                  // immediately, even for this event
        cut.unsubscribe();                                  // twice: no-op
    });
    cut = bus.subscribe("act", record("cut"));
    bus.publish("act", nullptr);
    bus.drain();
    check(seen == std::vector<std::string>{"actor"}, "bus: unsubscribe mid-drain is immediate; a new subscription waits");
    seen.clear();
    bus.drain();
    check(seen == std::vector<std::string>{"actor", "late\"again\""}, "bus: an event published in a drain arrives in the next");
    actor.unsubscribe();
    added.clear();
    bus.drain(); // the last "again"
    seen.clear();

    // A subscriber publishing mid-delivery grows the queue the event being delivered sits in: the subscribers after it
    // must still get that event.
    {
        EventBus grow;
        std::vector<std::string> got;
        EventBus::Subscription publisher = grow.subscribe("spawn", [&](const nlohmann::json&) {
            for (int i = 0; i < 100; ++i) grow.publish("spawned", i);
        });
        EventBus::Subscription after =
            grow.subscribe("spawn", [&](const nlohmann::json& payload) { got.push_back(payload.get<std::string>()); });
        grow.publish("spawn", "Guard");
        grow.publish("spawn", "Bat");
        grow.drain();
        check(got == std::vector<std::string>{"Guard", "Bat"},
              "bus: a later subscriber still gets the event after an earlier one published mid-delivery");
    }

    bool after = false;
    EventBus::Subscription thrower = bus.subscribe("t", [](const nlohmann::json&) { throw std::runtime_error("bad"); });
    EventBus::Subscription next = bus.subscribe("t", [&](const nlohmann::json&) { after = true; });
    bus.publish("t", nullptr);
    bus.drain();
    check(after && logged("Event 't': C++ subscriber failed: bad"), "bus: a throwing C++ subscriber is logged, the rest run");

    {
        EventBus::Subscription scoped = bus.subscribe("s", record("scoped"));
        EventBus::Subscription moved = std::move(scoped);
    }
    bus.publish("s", nullptr);
    bus.drain();
    check(seen.empty(), "bus: a destroyed handle unsubscribes (after a move too)");

    bus.publish("a", 3);
    bus.dropQueued();
    bus.drain();
    check(seen.empty(), "bus: dropQueued discards undelivered events");

    const uint64_t staleId = a1.id();
    a1.unsubscribe();
    a1.unsubscribe(); // stale: no-op
    EventBus::Subscription fresh = bus.subscribe("a", record("fresh"));
    check(fresh.id() > staleId, "bus: ids are never reused");
    a2.unsubscribe();
    bus.publish("a", 6);
    bus.drain();
    check(seen == std::vector<std::string>{"fresh6"}, "bus: stale handles never remove newer subscriptions");

    // Name buckets: per-name subscription order across interleaved names, a self- and another name's unsubscribe
    // mid-drain, a mid-drain subscription to a new name getting that name's later events, buckets emptied and refilled.
    {
        EventBus names;
        std::vector<std::string> got;
        const auto tag = [&got](std::string t) { return [&got, t](const nlohmann::json&) { got.push_back(t); }; };
        EventBus::Subscription x1 = names.subscribe("x", tag("x1"));
        EventBus::Subscription y1 = names.subscribe("y", tag("y1"));
        EventBus::Subscription x2 = names.subscribe("x", tag("x2"));
        EventBus::Subscription z1 = names.subscribe("z", tag("z1"));
        EventBus::Subscription y2 = names.subscribe("y", tag("y2"));
        EventBus::Subscription x3 = names.subscribe("x", tag("x3"));
        names.publish("y", nullptr);
        names.publish("x", nullptr);
        names.publish("none", nullptr);
        names.drain();
        check(got == std::vector<std::string>{"y1", "y2", "x1", "x2", "x3"},
              "bus: each name keeps subscription order across interleaved subscribes; other names are never called");

        got.clear();
        EventBus::Subscription self;
        EventBus::Subscription w;
        self = names.subscribe("self", [&](const nlohmann::json&) {
            got.push_back("self");
            self.unsubscribe(); // itself: not even the second "self" of this drain
            y2.unsubscribe();   // another name's subscriber: nothing from the "y" later in this drain
            w = names.subscribe("w", tag("w")); // a new name: its later events in this drain arrive
        });
        names.publish("self", nullptr);
        names.publish("self", nullptr);
        names.publish("y", nullptr);
        names.publish("w", nullptr);
        names.drain();
        check(got == std::vector<std::string>{"self", "y1", "w"},
              "bus: a self- or other-name unsubscribe mid-drain is immediate; a new name subscribed mid-drain gets its next events");

        got.clear();
        self.unsubscribe(); // stale: no-op
        y2.unsubscribe();   // stale: no-op
        x1.unsubscribe();
        x2.unsubscribe();
        x3.unsubscribe(); // "x" emptied outside a drain
        EventBus::Subscription again = names.subscribe("self", tag("again")); // "self" emptied by the drain
        EventBus::Subscription x4 = names.subscribe("x", tag("x4"));
        names.publish("self", nullptr);
        names.publish("x", nullptr);
        names.publish("y", nullptr);
        names.drain();
        check(got == std::vector<std::string>{"again", "x4", "y1"},
              "bus: stale unsubscribes are no-ops; emptied names take new subscribers");
    }
}

// Lua -> Lua and Lua -> C++: same tick after every update, one subscription order across Lua and C++, independent
// payload copies, payload conversion.
void checkLuaEvents() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/a.lua", R"(local tick = 0
assert(type(chthonic.publish) == "function" and type(chthonic.subscribe) == "function" and type(chthonic.unsubscribe) == "function")
assert(not pcall(function() chthonic.publish = nil end), "chthonic.publish can't be overwritten")
return { update = function(dt)
    tick = tick + 1
    chthonic.log("ev a update " .. tick)
    if tick == 1 then
        chthonic.publish("first", {n = 1})
        chthonic.publish("data", {list = {1, 2, 3}, obj = {a = "x"}, empty = {}, flag = true, s = "str", f = 1.5, i = -7})
        chthonic.publish("bare")
    end
end })");
    r.write("systems/b.lua", R"(return {
    init = function()
        chthonic.subscribe("first", function(p) chthonic.log("ev b first " .. p.n); p.n = 99; p.extra = true end)
        chthonic.subscribe("data", function(p) chthonic.log("ev b data") end)
    end,
    update = function(dt) chthonic.log("ev b update") end,
})");
    r.write("systems/d.lua", R"(local subscribed = false
return { update = function(dt)
    if not subscribed then
        subscribed = true
        chthonic.subscribe("first", function(p)
            assert(p.n == 1 and p.extra == nil, "each subscriber gets its own copy")
            chthonic.log("ev d first")
        end)
    end
end })");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "event systems load" + (errors.empty() ? "" : ": " + errors[0]));
    nlohmann::json first, data, bare = "unset";
    EventBus::Subscription s1 = w.world.bus().subscribe("first", [&](const nlohmann::json& p) {
        log::info("ev C++ first");
        first = p;
    });
    EventBus::Subscription s2 = w.world.bus().subscribe("data", [&](const nlohmann::json& p) { data = p; });
    EventBus::Subscription s3 = w.world.bus().subscribe("bare", [&](const nlohmann::json& p) { bare = p; });
    w.world.tick(T0);
    for (const auto& s : w.world.script_status()) check(s.ok, "events: system still enabled: " + s.path + " " + s.error);
    check(log_index("ev a update 1") < log_index("ev b update") && log_index("ev b update") < log_index("ev b first 1"),
          "events: Lua -> Lua in the same tick, after every update");
    check(log_index("ev b first 1") < log_index("ev C++ first") && log_index("ev C++ first") < log_index("ev d first") &&
              log_index("ev d first") < log_index("ev b data") && logged("ev b data"),
          "events: publish order, then one subscription order across Lua and C++");
    check(first == nlohmann::json{{"n", 1}}, "events: Lua -> C++ payload, unchanged by Lua subscribers");
    check(data == nlohmann::json::parse(
                      R"({"list": [1, 2, 3], "obj": {"a": "x"}, "empty": {}, "flag": true, "s": "str", "f": 1.5, "i": -7})"),
          "events: payload conversion (arrays, objects, empty table, scalars)");
    check(data["i"].is_number_integer() && bare.is_null(), "events: integral numbers are integers; no payload is null");
}

// C++ -> Lua, JSON -> Lua conversion, re-publish and subscribe during a drain, unsubscribe during a drain.
void checkLuaEventsFromCpp() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/r.lua", R"(local late = 0
local second
return {
    init = function()
        chthonic.subscribe("cpp", function(p)
            assert(#p.arr == 2 and p.arr[1] == 1 and p.arr[2] == 2, "array -> sequence")
            assert(p.o.k == "v" and p.b == false and p.nul == nil and p.i == 7 and p.s == "x", "object -> table")
            assert(next(p.e) == nil, "empty object -> empty table")
            chthonic.log("ev r cpp")
            chthonic.publish("pong", {from = "r"}) -- next tick
            chthonic.subscribe("cpp", function() late = late + 1; chthonic.log("ev r late " .. late) end) -- from the next event
        end)
        chthonic.subscribe("cut", function()
            chthonic.unsubscribe(second)
            chthonic.unsubscribe(second) -- twice: no-op
            chthonic.log("ev r cutter")
        end)
        second = chthonic.subscribe("cut", function() chthonic.log("ev r second") end)
    end,
    update = function(dt) end,
})");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "C++ -> Lua system loads");
    int pongs = 0;
    EventBus::Subscription thrower =
        w.world.bus().subscribe("pong", [](const nlohmann::json&) { throw std::runtime_error("pong failed"); });
    EventBus::Subscription counter = w.world.bus().subscribe("pong", [&](const nlohmann::json&) { ++pongs; });
    const nlohmann::json payload =
        nlohmann::json::parse(R"({"arr": [1, 2], "o": {"k": "v"}, "b": false, "nul": null, "i": 7, "s": "x", "e": {}})");
    w.world.bus().publish("cpp", payload);
    w.world.bus().publish("cut", nullptr);
    w.world.tick(T0);
    check(w.world.script_status()[0].ok, "events: C++ -> Lua payload converts to tables");
    check(logged("ev r cpp") && pongs == 0 && !logged("ev r late"),
          "events: published during a drain waits; a subscription added during a drain misses the current event");
    check(logged("ev r cutter") && !logged("ev r second"), "events: unsubscribe during a drain is immediate");
    w.world.bus().publish("cpp", payload);
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(pongs == 1 && logged("Event 'pong': C++ subscriber failed: pong failed"),
          "events: re-published event arrives next tick; a throwing C++ subscriber doesn't stop the rest");
    check(logged("ev r late 1") && !logged("ev r late 2"), "events: the late subscription gets later events only");
    check(w.world.script_status()[0].ok, "events: the system is still fine");
}

// Rejections raise at the calling file:line and queue nothing; the same for the client sends.
void checkLuaEventErrors() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/rej.lua", R"(local cyc = {}; cyc.self = cyc
local deep = {}; local t = deep; for i = 1, 32 do t.x = {}; t = t.x end
local ok32 = {}; t = ok32; for i = 1, 31 do t.x = {}; t = t.x end
local function try(f) local ok, err = pcall(f); chthonic.log("rej " .. tostring(ok) .. " " .. tostring(err)) end
return { update = function(dt)
    try(function() chthonic.publish(42) end)
    try(function() chthonic.publish("") end)
    try(function() chthonic.publish("e", {f = print}) end)
    try(function() chthonic.publish("e", {u = newproxy()}) end)
    try(function() chthonic.publish("e", {co = coroutine.create(print)}) end)
    try(function() chthonic.publish("e", cyc) end)
    try(function() chthonic.publish("e", deep) end)
    try(function() chthonic.publish("e", {1, a = 2}) end)
    try(function() chthonic.publish("e", {1, nil, 3}) end)
    try(function() chthonic.publish("e", {[0] = 1}) end)
    try(function() chthonic.publish("e", {x = 0 / 0}) end)
    try(function() chthonic.subscribe("e", 5) end)
    try(function() chthonic.subscribe("", print) end)
    try(function() chthonic.unsubscribe(5) end)
    try(function() chthonic.unsubscribe({}) end)
    chthonic.publish("ok32", ok32)
    try(function() chthonic.sendTo("x", "e") end)
    try(function() chthonic.sendTo(1.5, "e") end)
    try(function() chthonic.sendNearby(-1, "e") end)
    try(function() chthonic.sendTo(0, 5) end)
    try(function() chthonic.sendNearby(0, "") end)
    try(function() chthonic.broadcast(nil) end)
    try(function() chthonic.sendTo(0, "e", {f = print}) end)
    try(function() chthonic.broadcast("e", {x = 1 / 0}) end)
    try(function() chthonic.broadcast("e", cyc) end)
    try(function() chthonic.broadcast("big", string.rep("x", 65527)) end)
    chthonic.broadcast("big", string.rep("x", 65526))
end })");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "rejection system loads");
    int queued = 0, ok32 = 0;
    EventBus::Subscription e = w.world.bus().subscribe("e", [&](const nlohmann::json&) { ++queued; });
    EventBus::Subscription deep = w.world.bus().subscribe("ok32", [&](const nlohmann::json&) { ++ok32; });
    const auto sid = 9u;
    w.spawn(sid, 61, "Watcher", 1000, 1000);
    w.sent.clear();
    w.world.tick(T0);
    check(w.world.script_status()[0].ok, "events: rejections are catchable Lua errors");
    const std::pair<int, const char*> cases[] = {
        {6, "chthonic.publish: event name must be a non-empty string, got 42"},
        {7, "chthonic.publish: event name must be a non-empty string"},
        {8, "chthonic.publish: payload.f: a function can't be sent in an event"},
        {9, "chthonic.publish: payload.u: a userdata can't be sent in an event"},
        {10, "chthonic.publish: payload.co: a thread can't be sent in an event"},
        {11, "chthonic.publish: payload.self: the table contains itself (cycle)"},
        {12, "chthonic.publish: payload.x.x.x"}, // ... 32 levels: checked below
        {13, "chthonic.publish: payload: table keys must be exactly 1..n (an array) or all strings (an object)"},
        {14, "chthonic.publish: payload: table keys must be exactly 1..n"},
        {15, "chthonic.publish: payload: table keys must be exactly 1..n"},
        {16, "chthonic.publish: payload.x must be a finite number"},
        {17, "chthonic.subscribe: callback must be a function, got 5"},
        {18, "chthonic.subscribe: event name must be a non-empty string"},
        {19, "chthonic.unsubscribe: expected a handle from chthonic.subscribe, got 5"},
        {20, "chthonic.unsubscribe: expected a handle from chthonic.subscribe, got table"},
        {22, "chthonic.sendTo: invalid entity id x"},
        {23, "chthonic.sendTo: invalid entity id 1.5"},
        {24, "chthonic.sendNearby: invalid entity id -1"},
        {25, "chthonic.sendTo: event name must be a non-empty string, got 5"},
        {26, "chthonic.sendNearby: event name must be a non-empty string"},
        {27, "chthonic.broadcast: event name must be a non-empty string, got nil"},
        {28, "chthonic.sendTo: payload.f: a function can't be sent in an event"},
        {29, "chthonic.broadcast: payload.x must be a finite number"},
        {30, "chthonic.broadcast: payload.self: the table contains itself (cycle)"},
        {31, "chthonic.broadcast: event \\\"big\\\" is too large to send: 65532 bytes of name and JSON payload, at most 65531"},
    };
    for (const auto& [line, message] : cases)
        check(logged(std::format("rej false systems/rej.lua:{}: {}", line, message)),
              std::format("events: rejected at systems/rej.lua:{}: {}", line, message));
    check(logged("x: tables are nested more than 32 deep"), "events: nesting deeper than 32 is rejected");
    check(queued == 0 && ok32 == 1, "events: rejected calls queue nothing; 32 nested tables are fine");
    const auto events = w.sent.of<proto::ServerEvent>(sid);
    check(events.size() == 1 && events[0].name == "big" && events[0].json.size() == 65528 &&
              proto::encode(events[0]).size() == proto::kHeaderSize + proto::kMaxPayloadBytes,
          "events: rejected sends send nothing; one at the frame limit goes out");
}

// A failing subscriber disables only its system and removes its subscriptions.
void checkLuaEventFailures() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/bad.lua", R"(return {
    init = function()
        chthonic.subscribe("hit", function(p)
            error("subscriber failed")
        end)
        chthonic.subscribe("hit", function(p) chthonic.log("ev bad second") end)
        chthonic.subscribe("other", function(p) chthonic.log("ev bad other") end)
    end,
    update = function(dt) end,
})");
    r.write("systems/good.lua", R"(return {
    init = function() chthonic.subscribe("hit", function(p) chthonic.log("ev good " .. p.n) end) end,
    update = function(dt) end,
})");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "failure systems load");
    w.world.bus().publish("hit", {{"n", 1}});
    w.world.tick(T0);
    const auto status = w.world.script_status();
    check(!status[0].ok && contains(status[0].error, "systems/bad.lua:4: subscriber failed") &&
              logged(R"("msg":"Lua system disabled")") && logged(R"("callback":"event 'hit'")") &&
              logged(R"("error":"systems/bad.lua:4: subscriber failed)"),
          "events: a failing subscriber disables its system, logged with the callback and path:line");
    check(!logged("ev bad second") && logged("ev good 1") && status[1].ok,
          "events: the failed system's other subscribers stop; other systems' still run");
    w.world.bus().publish("other", nullptr);
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(!logged("ev bad other"), "events: disabling removes all of the system's subscriptions");
}

// Events published from init, onEnter, onLeave and subscribers wait for the next tick's drain; a graceful shutdown
// drops the queue.
void checkLuaEventHooks() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/h.lua", R"(return {
    init = function() chthonic.publish("hook", {from = "init"}) end,
    onEnter = function(e) chthonic.publish("hook", {from = "enter"}) end,
    onLeave = function(e) chthonic.publish("hook", {from = "leave"}) end,
    shutdown = function() chthonic.publish("hook", {from = "shutdown"}) end,
    update = function(dt) end,
})");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "hook event system loads");
    std::vector<std::string> got;
    EventBus::Subscription sub =
        w.world.bus().subscribe("hook", [&](const nlohmann::json& p) { got.push_back(p["from"].get<std::string>()); });
    const auto a = w.spawn(1, 71, "Alice", 1000, 1000);
    check(got.empty(), "events: init and onEnter events wait for the tick");
    w.world.tick(T0);
    check(got == std::vector<std::string>{"init", "enter"}, "events: init and onEnter events arrive in the next tick's drain");
    w.world.despawn(a);
    check(got.size() == 2, "events: onLeave events wait for the tick");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(got.size() == 3 && got[2] == "leave", "events: onLeave events arrive in the next tick's drain");

    w.spawn(2, 72, "Bob", 1000, 1000);
    w.world.shutdown_scripts();
    w.world.bus().drain();
    check(got.size() == 3, "events: a graceful shutdown drops the queued events (onEnter, onLeave, shutdown)");
}

// chthonic.world.request's server side: "client.<name>" carries requestId (none for chthonic.world.send); chthonic.reply answers
// it to the owner alone as Reply{requestId, ok, data or the reason}; bad arguments raise.
void checkReply() {
    LuaRoot r;
    r.write("systems/ask.lua", R"(return {
    init = function()
        chthonic.subscribe("client.ask", function(e)
            if e.data.yes then chthonic.reply(e.entity, e.requestId, true, {echo = e.data.n})
            else chthonic.reply(e.entity, e.requestId, false, "nope") end
        end)
        chthonic.subscribe("client.plain", function(e) chthonic.sendTo(e.entity, "plain", {request = tostring(e.requestId)}) end)
        chthonic.subscribe("client.bad", function(e)
            local errors = {}
            for _, a in ipairs({{e.entity, 0, true}, {e.entity, "x", true}, {e.entity, e.requestId, "yes"},
                                {e.entity, e.requestId, false, 5}, {e.entity, e.requestId, false, nil}}) do
                local ok, err = pcall(chthonic.reply, a[1], a[2], a[3], a[4])
                errors[#errors + 1] = ok and "no error" or err
            end
            chthonic.reply(e.entity, e.requestId, true) -- nil data: {}
            chthonic.sendTo(e.entity, "errors", {list = errors})
        end)
    end,
    update = function(dt) end,
})");
    TestWorld w({});
    const auto loadErrors = w.world.load_scripts(r.root);
    check(loadErrors.empty(), "reply system loads " + (loadErrors.empty() ? "" : loadErrors[0]));
    const auto a = w.spawn(1, 91, "Alice", 1000, 1000);
    w.spawn(2, 92, "Bob", 1010, 1000);
    w.sent.clear();
    w.world.push_client_event(a, "ask", nlohmann::json{{"yes", true}, {"n", 4}}, 11);
    w.world.push_client_event(a, "ask", nlohmann::json::object(), 12);
    w.world.push_client_event(a, "plain", nlohmann::json::object());
    w.world.push_client_event(a, "bad", nlohmann::json::object(), 13);
    w.world.tick(T0);
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(w.sent.of<proto::Reply>(1) == std::vector<proto::Reply>{{11, true, R"({"echo":4})"}, {12, false, R"("nope")"},
                                                                  {13, true, "{}"}},
          "chthonic.reply: Reply{requestId, ok, data JSON / the reason as a JSON string}, in call order");
    check(w.sent.of<proto::Reply>(2).empty(), "chthonic.reply: the owner alone");
    const auto events = w.sent.of<proto::ServerEvent>(1);
    check(events.size() == 2 && events[0].json == R"({"request":"nil"})", "chthonic.world.send's client event has no requestId");
    const Json errors = events.size() == 2 ? Json::parse(events[1].json)["list"] : Json();
    const auto has = [&](size_t i, const std::string& part) {
        return errors.is_array() && i < errors.size() && errors[i].get<std::string>().find(part) != std::string::npos;
    };
    check(has(0, "chthonic.reply: requestId must be a client event's requestId, got 0") &&
              has(1, "chthonic.reply: requestId must be a client event's requestId, got x") &&
              has(2, "chthonic.reply: ok must be a boolean, got yes") &&
              has(3, "chthonic.reply: a rejection's reason must be a string, got 5") &&
              has(4, "chthonic.reply: a rejection's reason must be a string, got nil"),
          "chthonic.reply: bad arguments raise " + errors.dump());
}

// A chthonic.reply reaches the requester wherever the character is now (here: transferred to map 2 before the reply);
// one for a character that left the world sends nothing.
void checkReplyAfterMapChange() {
    LuaRoot r;
    r.write("systems/later.lua", R"(local pending = {}
return {
    init = function()
        chthonic.subscribe("client.ask", function(e)
            pending[#pending + 1] = {e.entity, e.requestId}
            if e.data.go then chthonic.transfer(e.entity, 2, {x = 300, y = 0, z = 310}) end
        end)
    end,
    update = function(dt)
        for _, p in ipairs(pending) do chthonic.reply(p[1], p[2], true, {late = true}) end
        pending = {}
    end,
})");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "reply after map change: loads");
    const auto a = w.spawn(1, 93, "Alice", 1000, 1000);
    const auto b = w.spawn(2, 94, "Bob", 1010, 1000);
    w.sent.clear();
    w.world.push_client_event(a, "ask", Json{{"go", true}}, 21);
    w.world.push_client_event(b, "ask", Json::object(), 22);
    w.world.tick(T0); // handled after update: Alice transferred at the serial point, both replies still pending
    w.world.despawn(b);
    w.world.tick(T0 + std::chrono::milliseconds(50)); // map 1's update replies
    check(w.world.map_of(a) == 2, "reply after map change: Alice is on map 2");
    check(w.sent.of<proto::Reply>(1) == std::vector<proto::Reply>{{21, true, R"({"late":true})"}},
          "reply after map change: the Reply reaches the requester on another map");
    check(w.sent.of<proto::Reply>(2).empty(), "reply after map change: a character that left the world gets nothing");
}

// A ClientEvent dropped by the rate limit: Reply{requestId, false, "rate_limited"} if it has a requestId, else nothing.
void checkReplyRateLimited() {
    TestWorld w({});
    const auto a = w.spawn(1, 95, "Alice", 1000, 1000);
    w.sent.clear();
    for (int i = 0; i < 10; ++i) w.world.push_client_event(a, "x", Json::object()); // the window's 10
    w.world.push_client_event(a, "x", Json::object(), 77);
    w.world.push_client_event(a, "x", Json::object());
    w.world.tick(T0);
    check(w.sent.of<proto::Reply>(1) == std::vector<proto::Reply>{{77, false, R"("rate_limited")"}},
          "rate-dropped request: one Reply{77, false, \"rate_limited\"}, none for requestId 0");
    check(w.world.registry().get<world::ClientEventLimit>(a).dropped == 2, "rate-dropped request: still counted");
}

// ChatServer is the message right after EnterWorldOk, before the MapChange of an onEnter transfer; none without one.
void checkChatServerOrder() {
    LuaRoot r;
    r.write("systems/away.lua", R"(return {
    onEnter = function(e) chthonic.transfer(e, 2, {x = 300, y = 0, z = 310}) end,
    update = function(dt) end,
})");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "chat server order: loads");
    w.world.spawn(1, {96, 1, "Alice", 1, {1000, 0, 1000}, 0.5f, "", {}}, T0, proto::ChatServer{"chat.example.com", 7778});
    const auto& got = w.sent.bySession[1];
    check(got.size() >= 3 && std::holds_alternative<proto::EnterWorldOk>(got[0]) &&
              std::get_if<proto::ChatServer>(&got[1]) &&
              *std::get_if<proto::ChatServer>(&got[1]) == proto::ChatServer{"chat.example.com", 7778} &&
              std::holds_alternative<proto::MapChange>(got[2]),
          "chat server order: EnterWorldOk, ChatServer, then the onEnter MapChange");
    w.world.spawn(2, {97, 1, "Bob", 1, {1000, 0, 1000}, 0.5f, "", {}}, T0);
    check(w.sent.of<proto::ChatServer>(2).empty() && std::holds_alternative<proto::EnterWorldOk>(w.sent.bySession[2][0]),
          "chat server order: none without a chat host");
}

// Moves whose y validate_move adjusted (lifted onto the ground, clamped to MIN_Y/MAX_Y) are corrected like a rejection:
// Correction{adjusted pos, its seq, epoch + 1}, the rest of the batch dropped. y unchanged: no Correction.
void checkMoveYAdjusted() {
    {
        TestWorld w({});
        const auto a = w.spawn(1, 42, "Alice", 1000, 1000);
        w.world.tick(T0);
        w.sent.clear();
        const auto& tf = w.world.registry().get<world::Transform>(a);
        w.world.push_move(a, {{1001, -1, 1000}, 0, 0, 1, 0}); // 1 m under the flat ground (tolerance 0.5)
        w.world.push_move(a, {{1002, 0, 1000}, 0, 0, 2, 0});
        w.world.tick(T0 + std::chrono::milliseconds(500));
        const auto corr = w.sent.of<proto::Correction>(1);
        check(corr == std::vector<proto::Correction>{{{1001, 0, 1000}, 1, 1}} && tf.pos == proto::Vec3{1001, 0, 1000},
              "y adjusted: lifted onto the ground -> Correction{lifted pos, its seq, epoch 1}, the rest of the batch dropped");
    }
    world::WorldRules rules;
    rules.move.minY = 1;
    rules.move.maxY = 2;
    TestWorld w({}, rules);
    const auto a = w.spawn(1, 43, "Alice", 1000, 1000); // spawned at y 1 (MIN_Y)
    w.world.tick(T0);
    w.sent.clear();
    const auto& tf = w.world.registry().get<world::Transform>(a);
    w.world.push_move(a, {{1001, 1.5f, 1000}, 0, 0, 1, 0});
    w.world.tick(T0 + std::chrono::milliseconds(500));
    check(w.sent.of<proto::Correction>(1).empty() && tf.pos.y == 1.5f, "y adjusted: y within the bounds, no Correction");
    w.world.push_move(a, {{1002, 3, 1000}, 0, 0, 2, 0});
    w.world.push_move(a, {{1003, 1.5f, 1000}, 0, 0, 3, 0});
    w.world.tick(T0 + std::chrono::milliseconds(1000));
    auto corr = w.sent.of<proto::Correction>(1);
    check(corr == std::vector<proto::Correction>{{{1002, 2, 1000}, 2, 1}} && tf.pos.x == 1002,
          "y adjusted: clamped to MAX_Y -> Correction{clamped pos, its seq, epoch 1}, the rest of the batch dropped");
    w.sent.clear();
    w.world.push_move(a, {{1003, 0.2f, 1000}, 0, 0, 4, 1});
    w.world.tick(T0 + std::chrono::milliseconds(1500));
    corr = w.sent.of<proto::Correction>(1);
    check(corr == std::vector<proto::Correction>{{{1003, 1, 1000}, 4, 2}}, "y adjusted: clamped to MIN_Y -> Correction, epoch 2");
}

// chthonic.sendTo / sendNearby / broadcast: audiences, call order, after component replication, before Despawn.
void checkClientEvents() {
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', replicate = 'aoi', fields = { { name = 'hp', type = 'int' } } }");
    r.write("systems/send.lua", R"(local ids, tick = {}, 0
local gone
return {
    onEnter = function(e) ids[chthonic.get(e, "Character").name] = e ; chthonic.add(e, "Health") end,
    onLeave = function(e)
        chthonic.sendNearby(e, "bye", {who = e})
        gone = e
    end,
    update = function(dt)
        tick = tick + 1
        if tick == 1 then
            chthonic.set(ids.Alice, "Health", {hp = 5}) -- replicated this tick, before the events
            chthonic.sendTo(ids.Alice, "to", {n = 1})
            chthonic.sendNearby(ids.Alice, "near", {n = 2})
            chthonic.broadcast("all", {n = 3})
            chthonic.sendTo(ids.Bob, "bob")
            chthonic.sendTo(4000000000, "nobody") -- a valid id not in the world: nothing
        elseif gone then
            chthonic.sendTo(gone, "late")    -- left: nothing
            chthonic.sendNearby(gone, "late")
            gone = nil
        end
    end,
})");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "client event system loads");
    const auto a = w.spawn(1, 81, "Alice", 1000, 1000);
    w.spawn(2, 82, "Bob", 1010, 1000);
    w.spawn(3, 83, "Carol", 1800, 1800); // out of view
    const uint32_t A = wire(a);
    w.sent.clear();
    w.world.tick(T0);
    const auto names = [&](uint32_t sid) {
        std::vector<std::string> out;
        for (const auto& e : w.sent.of<proto::ServerEvent>(sid)) out.push_back(e.name);
        return out;
    };
    check(names(1) == std::vector<std::string>{"to", "near", "all"}, "sendTo/sendNearby/broadcast reach the owner, in call order");
    check(names(2) == std::vector<std::string>{"near", "all", "bob"}, "sendNearby reaches a viewer; sendTo only its target");
    check(names(3) == std::vector<std::string>{"all"}, "broadcast reaches a player out of view; nothing else does");
    check(w.sent.of<proto::ServerEvent>(1)[0].json == R"({"n":1})", "the payload is compact JSON");
    const auto index = [&](uint32_t sid, auto pred) {
        const auto& msgs = w.sent.bySession[sid];
        for (size_t i = 0; i < msgs.size(); ++i)
            if (pred(msgs[i])) return i;
        return msgs.size();
    };
    const auto isEvent = [](const proto::Message& m) { return std::holds_alternative<proto::ServerEvent>(m); };
    const auto isHealthA = [&](const proto::Message& m) {
        auto* u = std::get_if<proto::ComponentUpdate>(&m);
        return u && u->entityId == A && u->json == R"({"hp":5})";
    };
    const auto isSpawnA = [&](const proto::Message& m) {
        auto* s = std::get_if<proto::Spawn>(&m);
        return s && s->entityId == A;
    };
    check(index(1, isHealthA) < index(1, isEvent) && index(2, isHealthA) < index(2, isEvent) &&
              index(2, isSpawnA) < index(2, isEvent),
          "client events go out after Spawn and component replication");

    w.sent.clear();
    w.world.despawn(a);
    const auto isDespawnA = [&](const proto::Message& m) {
        auto* d = std::get_if<proto::Despawn>(&m);
        return d && d->entityId == A;
    };
    check(names(1) == std::vector<std::string>{"bye"} && names(2) == std::vector<std::string>{"bye"} && names(3).empty(),
          "onLeave sends go out at the despawn, to the leaver and its viewers");
    check(index(2, isEvent) < index(2, isDespawnA) && index(2, isDespawnA) < w.sent.bySession[2].size(),
          "onLeave sends go out before the Despawn");
    w.sent.clear();
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(names(1).empty() && names(2).empty() && names(3).empty(), "a target that left is a silent no-op");
    for (const auto& s : w.world.script_status()) check(s.ok, "client events: system still enabled: " + s.error);

    // Graceful shutdown: onLeave sends go out before the despawns.
    w.sent.clear();
    w.world.shutdown_scripts();
    check(names(2) == std::vector<std::string>{"bye"} && names(3) == std::vector<std::string>{"bye"},
          "shutdown: onLeave sends go out before the despawns");
}

void checkCodec() {
    for (const proto::Message& m : {proto::Message{proto::ComponentUpdate{7, "Health", R"({"current":1.5})"}},
                                    proto::Message{proto::ComponentRemove{7, "Health"}},
                                    proto::Message{proto::ServerEvent{"quest.update", R"({"quest":7})"}}}) {
        const auto frame = proto::encode(m);
        const auto d = proto::decode_frame(frame, proto::kMaxPayloadBytes);
        check(d.status == proto::DecodeStatus::ok && d.msg == m, std::string("codec round trip ") + proto::opcode_name(proto::opcode_of(m)));
    }
    check(proto::ComponentUpdate::fits(6, 65535 - 14) && !proto::ComponentUpdate::fits(6, 65535 - 13), "frame size limit");
    check(proto::ServerEvent::fits(3, 65535 - 7) && !proto::ServerEvent::fits(3, 65535 - 6), "ServerEvent frame size limit");
}

// --- state machines (docs/world/state-machines.html) ---

// Indexes of the captured log lines whose "msg" contains part (log lines are JSON: quotes in msg are escaped).
std::vector<size_t> said_at(const std::string& part) {
    std::vector<size_t> out;
    const auto& lines = captured();
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto line = nlohmann::json::parse(lines[i], nullptr, false);
        if (line.is_object() && contains(line.value("msg", ""), part)) out.push_back(i);
    }
    return out;
}
bool said(const std::string& part) { return !said_at(part).empty(); }
size_t said_first(const std::string& part) {
    const auto at = said_at(part);
    return at.empty() ? SIZE_MAX : at[0];
}

// Every "FAIL ..." a Lua expect/raises logged becomes a failed check; done must have been logged.
void check_lua_expectations(const std::string& what, const std::string& done) {
    for (size_t i : said_at("FAIL ")) check(false, what + ": " + nlohmann::json::parse(g_log[i]).value("msg", ""));
    check(said(done), what + ": the Lua checks ran (" + done + ")");
}

constexpr const char* kLuaExpect = R"(local function expect(ok, what) if not ok then chthonic.error("FAIL " .. what) end end
local function raises(want, f)
    local ok, err = pcall(f)
    if ok then chthonic.error("FAIL no error, wanted: " .. want)
    elseif not string.find(err, want, 1, true) then chthonic.error("FAIL got [" .. tostring(err) .. "] wanted [" .. want .. "]") end
end
)";

// chthonic.debug/log/warn/error(message [, fields]): fields become JSON on the line; bad fields raise at file:line.
void checkLuaLogging() {
    LogCapture capture;
    log::set_level(log::Level::debug);
    LuaRoot r;
    r.write("systems/logs.lua", std::string(kLuaExpect) + R"lua(local t = {}
t.self = t
return {
    init = function()
        chthonic.debug("dbg line")
        chthonic.log("with fields", {player = 7, zone = "x"})
        chthonic.warn("nested", {pos = {x = 1, y = 2.5}, tags = {"a", "b"}})
        chthonic.error("no fields", {})
        raises("chthonic.log: fields must be a table, got 5", function() chthonic.log("m", 5) end)
        raises("chthonic.log: fields must be a table of named fields", function() chthonic.log("m", {1, 2}) end)
        raises("chthonic.warn: payload.cb: a function can't be sent", function() chthonic.warn("m", {cb = print}) end)
        raises("chthonic.error: payload.self: the table contains itself (cycle)", function() chthonic.error("m", t) end)
        raises('chthonic.log: "ts" is reserved', function() chthonic.log("m", {ts = 1}) end)
        raises('chthonic.debug: "level" is reserved', function() chthonic.debug("m", {level = "x"}) end)
        raises('chthonic.log: "msg" is reserved', function() chthonic.log("m", {nested = {}, msg = "x"}) end)
        local ok, err = pcall(function() chthonic.log("m", true) end)
        expect(not ok and string.find(err, "systems/logs.lua:%d+: chthonic.log: fields") ~= nil, "error at file:line: " .. tostring(err))
        chthonic.log("logging checks done")
    end,
    update = function() chthonic.debug("tick debug"); chthonic.log("tick info"); chthonic.warn("tick warn") end,
})lua");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "lua logging: loads" + (errors.empty() ? "" : ": " + errors[0]));
    check_lua_expectations("lua logging", "logging checks done");

    const auto line = [](const std::string& msg) {
        const size_t i = said_first("systems/logs.lua: " + msg);
        return i == SIZE_MAX ? nlohmann::ordered_json() : nlohmann::ordered_json::parse(g_log[i]);
    };
    const auto dbg = line("dbg line");
    check(dbg.value("level", "") == "debug", "chthonic.debug logs at debug");
    std::vector<std::string> keys;
    for (const auto& [key, unused] : dbg.items()) keys.push_back(key);
    check(keys == std::vector<std::string>{"ts", "level", "msg"}, "a line starts ts, level, msg");
    const std::string ts = dbg.value("ts", "");
    check(ts.size() == 24 && ts[10] == 'T' && ts[19] == '.' && ts.back() == 'Z', "ts is ISO 8601 UTC with ms: " + ts);
    const size_t withFields = said_first("with fields");
    check(withFields != SIZE_MAX &&
              contains(g_log[withFields], R"("msg":"systems/logs.lua: with fields","player":7,"zone":"x"})"),
          "chthonic.log fields: \"player\":7,\"zone\":\"x\" after msg");
    const auto nested = line("nested");
    check(nested.value("level", "") == "warn" && nested["pos"] == nlohmann::ordered_json{{"x", 1}, {"y", 2.5}} &&
              nested["tags"] == nlohmann::ordered_json{"a", "b"},
          "nested tables become nested JSON");
    check(line("no fields").size() == 3, "an empty fields table adds nothing");

    log::set_level(log::Level::warn);
    clear_log();
    w.world.tick(T0);
    check(!said("tick debug") && !said("tick info") && said("tick warn"), "LOG_LEVEL warn: info and debug are dropped");
    log::set_level(log::Level::debug);
    clear_log();
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(said("tick debug") && said("tick info"), "LOG_LEVEL debug: chthonic.debug shows");
    log::set_level(log::Level::info);
}

// Every callback logs "<enter|exit> <from>-><to> entity <id>".
constexpr const char* kDoor = R"(local function note(what) return function(ctx)
    chthonic.log(what .. " " .. tostring(ctx.from) .. "->" .. ctx.to .. " entity " .. tostring(ctx.entity))
end end
return {
    name = "door",
    initial = "closed",
    states = {
        closed = { allowedFrom = {"open"}, enter = note("enter"), exit = note("exit") },
        open   = { allowedFrom = {"closed", "locked"}, enter = note("enter"), exit = note("exit") },
        locked = { enter = note("enter"), exit = note("exit") },
        broken = { allowedFrom = {"*"}, enter = note("enter"), exit = note("exit") },
    },
})";
constexpr const char* kPlain = "return { name = 'plain', initial = 'a', states = { a = {}, b = { allowedFrom = {'a'} } } }";

void checkFsmDefinitions() {
    LuaRoot r;
    r.write("fsms/sub/door.lua", kDoor).write("fsms/plain.lua", kPlain).write("fsms/notes.txt", "not lua");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "fsm: valid definitions load" + (errors.empty() ? "" : ": " + errors[0]));
    const lua::StateMachineDef* door = w.world.fsm().definition("door");
    check(door && door->path == "fsms/sub/door.lua" && door->initial == "closed" &&
              door->stateNames() == std::vector<std::string>{"broken", "closed", "locked", "open"},
          "fsm: recursive scan; name, path, initial, sorted states");
    check(door && door->states.at("locked").fromAny && door->states.at("broken").fromAny &&
              !door->states.at("open").fromAny && door->states.at("open").from == std::set<std::string>{"closed", "locked"} &&
              door->states.at("closed").enter && door->states.at("closed").exit && !w.world.fsm().definition("notes"),
          "fsm: allowedFrom omitted / \"*\" / listed; callbacks bound; .lua files only");
    check(w.world.fsm().definition("plain") && !w.world.fsm().definition("plain")->states.at("a").enter,
          "fsm: a state without callbacks");

    const auto machine = [](const std::string& states, const std::string& extra = "") {
        return "return {name='m', initial='a', states={" + states + "}" + extra + "}";
    };
    const std::pair<std::string, std::string> cases[] = {
        {"return {", "fsms/m.lua:1:"},
        {"error('nope')", "fsms/m.lua:1: nope"},
        {"io.write('x')", "fsms/m.lua:1:"},
        {"return 5", "fsms/m.lua: must return exactly one table {name = ..., initial = ..., states = {...}}"},
        {machine("a={}") + ", 2", "fsms/m.lua: must return exactly one table"},
        {"return setmetatable({name='m', initial='a', states={a={}}}, {})", "fsms/m.lua: tables must be plain data"},
        {"return {initial='a', states={a={}}}", "fsms/m.lua: \"name\" must be a non-empty string"},
        {"return {name=5, initial='a', states={a={}}}", "fsms/m.lua: \"name\" must be a non-empty string"},
        {"return {name='m', initial='z', states={a={}}}", "fsms/m.lua: \"initial\" must name one of its states, got z"},
        {"return {name='m', states={a={}}}", "\"initial\" must name one of its states, got nil"},
        {machine("a={}", ", colour=1"), "fsms/m.lua: unknown key colour"},
        {"return {name='m', initial='a', states=5}", "fsms/m.lua: \"states\" must be a table {name = {...}, ...}"},
        {"return {name='m', initial='a', states={}}", "fsms/m.lua: \"states\" must not be empty"},
        {machine("['*']={}"), "fsms/m.lua: state names must be non-empty strings other than \"*\", got *"},
        {machine("a=5"), "fsms/m.lua: state \"a\" must be a table"},
        {machine("a={allowedFrom={'q'}}"), "fsms/m.lua: state \"a\": \"allowedFrom\" names unknown state \"q\""},
        {machine("a={allowedFrom='a'}"), "fsms/m.lua: state \"a\": \"allowedFrom\" must be a list {...}"},
        {machine("a={allowedFrom={5}}"), "fsms/m.lua: state \"a\": \"allowedFrom\" must hold state names, got 5"},
        {machine("a={enter=5}"), "fsms/m.lua: state \"a\": \"enter\" must be a function"},
        {machine("a={exit='x'}"), "fsms/m.lua: state \"a\": \"exit\" must be a function"},
        {machine("a={update=function() end}"), "fsms/m.lua: state \"a\": unknown key update"},
    };
    for (const auto& [body, want] : cases) {
        LuaRoot c;
        c.write("fsms/m.lua", body);
        TestWorld cw({});
        const auto e = cw.world.load_scripts(c.root);
        check(e.size() == 1 && contains(e[0], want) && !cw.world.fsm().definition("m"),
              "fsm: rejected with \"" + want + "\" (got " + (e.empty() ? "nothing" : e[0]) + ")");
    }

    LuaRoot d;
    d.write("fsms/a.lua", kPlain).write("fsms/z/b.lua", kPlain).write("fsms/door.lua", kDoor);
    TestWorld dw({});
    const auto dup = dw.world.load_scripts(d.root);
    check(dup.size() == 2 && dup[0] == "fsms/a.lua: name \"plain\" is also declared in fsms/z/b.lua" &&
              dup[1] == "fsms/z/b.lua: name \"plain\" is also declared in fsms/a.lua",
          "fsm: two files with the same name are both rejected, each naming the other");
    check(!dw.world.fsm().definition("plain") && dw.world.fsm().definition("door"), "fsm: the other definitions still load");

    // GAME_STATE_MACHINES: an unknown name (or one whose file was rejected, or with no fsms/ folder) refuses the start.
    TestWorld gw({});
    const auto unknown = gw.world.load_scripts(d.root, {"door", "nope", "plain"});
    check(unknown.size() == 4 && unknown[2] == "GAME_STATE_MACHINES: game state machine \"nope\" has no valid definition in fsms/" &&
              unknown[3] == "GAME_STATE_MACHINES: game state machine \"plain\" has no valid definition in fsms/",
          "fsm: listed game machines without a valid definition are startup errors");
    LuaRoot none;
    TestWorld nw({});
    check(nw.world.load_scripts(none.root).empty(), "fsm: no fsms/ folder and no game machines is fine");
    TestWorld lw({});
    const auto listed = lw.world.load_scripts(none.root, {"door"});
    check(listed.size() == 1 && contains(listed[0], "\"door\" has no valid definition"),
          "fsm: a listed game machine with no fsms/ folder refuses the start");
}

// Transition rules, callback and event order, C++ API (game machine), and the Lua subscriber.
void checkFsmRules() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/door.lua", kDoor);
    r.write("systems/s.lua", R"(return {
    init = function()
        chthonic.subscribe("fsm.changed", function(p)
            chthonic.log("lua got " .. p.machine .. " " .. p.from .. "->" .. p.to .. " entity " .. tostring(p.entity))
        end)
    end,
    update = function(dt) end,
})");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root, {"door"});
    check(errors.empty(), "fsm rules: loads" + (errors.empty() ? "" : ": " + errors[0]));
    auto& fsm = w.world.realm_fsm(); // the game machines run in the realm context
    check(said_at("fsms/door.lua: enter nil->closed entity nil").size() == 1 && said_at("fsms/door.lua:").size() == 1,
          "fsm: a game machine enters its initial state after init, once, with from = nil (chthonic.log prefixed by path)");
    std::vector<nlohmann::json> changed;
    EventBus::Subscription sub = w.world.bus().subscribe("fsm.changed", [&](const nlohmann::json& p) { changed.push_back(p); });
    w.world.tick(T0);
    check(changed.empty() && fsm.state("door") == "closed", "fsm: no fsm.changed for the first entry");

    clear_log();
    check(fsm.canChange("door", "open") && fsm.canChange("door", "locked") && fsm.canChange("door", "broken") &&
              !fsm.canChange("door", "closed") && !fsm.canChange("door", "flying"),
          "fsm: canChange: listed, omitted and \"*\" allowed; self not listed and unknown denied");
    check(captured().empty() && fsm.state("door") == "closed", "fsm: canChange has no side effects and logs nothing");

    check(!fsm.change("door", "closed") &&
              said(R"(State machine "door": "closed" can't be entered from "closed" (allowedFrom); staying in "closed")"),
          "fsm: a self-transition not allowed by allowedFrom is denied with the client's warning");
    check(!fsm.change("door", "flying") && said(R"(State machine "door": unknown state "flying"; staying in "closed")"),
          "fsm: an unknown state is denied with a warning");
    check(said_at("fsms/door.lua:").empty() && fsm.state("door") == "closed", "fsm: a denial runs no callbacks");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(changed.empty(), "fsm: a denial publishes no event");

    clear_log();
    check(fsm.change("door", "open") && fsm.state("door") == "open", "fsm: an allowed change applies immediately");
    check(said_first("exit closed->open") < said_first("enter closed->open") && said("enter closed->open entity nil"),
          "fsm: exit(old), then enter(new) with the same ctx");
    check(changed.empty(), "fsm: fsm.changed waits for the drain");
    w.world.tick(T0 + std::chrono::milliseconds(100));
    check(changed.size() == 1 &&
              changed[0] == nlohmann::json{{"machine", "door"}, {"entity", nullptr}, {"from", "closed"}, {"to", "open"}},
          "fsm: fsm.changed {machine, entity = null, from, to} delivered to C++ at the drain");
    check(said("systems/s.lua: lua got door closed->open entity nil"), "fsm: ... and to Lua subscribers");

    changed.clear();
    check(fsm.change("door", "broken"), "fsm: \"*\" from open");
    clear_log();
    check(fsm.change("door", "broken") && said("exit broken->broken") && said("enter broken->broken"),
          "fsm: a self-transition allowed by \"*\" runs exit and enter");
    check(fsm.change("door", "locked") && fsm.change("door", "locked"), "fsm: a self-transition allowed by an omitted allowedFrom");
    check(fsm.change("door", "open") && fsm.change("door", "closed"), "fsm: locked -> open -> closed");
    check(fsm.states("door") == std::vector<std::string>{"broken", "closed", "locked", "open"}, "fsm: states() sorted");
    w.world.tick(T0 + std::chrono::milliseconds(150));
    check(changed.size() == 6 && changed[1]["from"] == "broken" && changed[1]["to"] == "broken" &&
              changed[3]["from"] == "locked" && changed[3]["to"] == "locked" && changed[5]["to"] == "closed",
          "fsm: one event per transition, self-transitions included, in order");
    for (const auto& s : w.world.script_status()) check(s.ok, "fsm rules: system still enabled: " + s.error);
}

// Changes requested inside enter/exit are queued and applied after the transition, in order; a queued denial warns.
void checkFsmQueue() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/q.lua", R"(return { name = "q", initial = "a", states = {
    a = { allowedFrom = {"c"}, exit = function(ctx) chthonic.log("exit a") end },
    b = {
        enter = function(ctx)
            chthonic.log("enter b")
            chthonic.log("queued c " .. tostring(chthonic.fsm.change("q", "c")))
            chthonic.log("queued y " .. tostring(chthonic.fsm.change("r", "y")))
            chthonic.log("still " .. chthonic.fsm.state("q"))
            chthonic.log("enter b done")
        end,
        exit = function(ctx) chthonic.log("exit b") end,
    },
    c = { allowedFrom = {"b"}, enter = function(ctx)
        chthonic.log("enter c")
        chthonic.log("queued c again " .. tostring(chthonic.fsm.change("q", "c")))
    end },
} })");
    r.write("fsms/r.lua", R"(return { name = "r", initial = "x", states = {
    x = {}, y = { enter = function(ctx) chthonic.log("enter y") end },
} })");
    TestWorld w({});
    check(w.world.load_scripts(r.root, {"q", "r"}).empty(), "fsm queue: loads");
    std::vector<nlohmann::json> changed;
    EventBus::Subscription sub = w.world.bus().subscribe("fsm.changed", [&](const nlohmann::json& p) { changed.push_back(p); });
    clear_log();
    check(w.world.realm_fsm().change("q", "b"), "fsm queue: change returns true");
    const std::vector<size_t> order = {said_first("exit a"),      said_first("enter b"),     said_first("queued c true"),
                                       said_first("queued y true"), said_first("still b"),   said_first("enter b done"),
                                       said_first("exit b"),      said_first("enter c"),     said_first("queued c again true"),
                                       said_first("enter y"),
                                       said_first(R"(State machine "q": "c" can't be entered from "c" (allowedFrom))")};
    check(std::ranges::is_sorted(order) && order.back() != SIZE_MAX,
          "fsm queue: requests inside callbacks return true, wait for the transition, then apply in request order; the "
          "denied one warns");
    check(w.world.realm_fsm().state("q") == "c" && w.world.realm_fsm().state("r") == "y", "fsm queue: final states");
    w.world.tick(T0);
    check(changed.size() == 3 && changed[0]["to"] == "b" && changed[1]["to"] == "c" && changed[2]["machine"] == "r",
          "fsm queue: one event per applied transition, in order");
}

// A failing or runaway callback is logged with path:line and a traceback; the transition commits and nothing is
// disabled, also when a system triggered it (the game machine's callback runs in the realm context).
void checkFsmCallbackErrors() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/err.lua", R"(return { name = "err", initial = "ok", states = {
    ok = {},
    bad = { enter = function(ctx)
        error("kaput")
    end },
    slow = { exit = function(ctx) chthonic.log("slow exit") end, enter = function(ctx) while true do end end },
} })");
    r.write("systems/a_count.lua", "local n = 0\nreturn { update = function() n = n + 1 ; chthonic.log('tick ' .. n) end }");
    r.write("systems/b_trigger.lua", R"(local go = false
return { update = function()
    if go then chthonic.fsm.change("err", "slow") end
end, onEnter = function(e) go = true end })");
    TestWorld w({});
    check(w.world.load_scripts(r.root, {"err"}).empty(), "fsm errors: loads");
    auto& fsm = w.world.realm_fsm();
    std::vector<nlohmann::json> changed;
    EventBus::Subscription sub = w.world.bus().subscribe("fsm.changed", [&](const nlohmann::json& p) { changed.push_back(p); });

    check(fsm.change("err", "bad") && fsm.state("err") == "bad", "fsm: a failing enter still commits; change returns true");
    check(said(R"(State machine fsms/err.lua: enter "bad" failed: fsms/err.lua:4: kaput)") && said("stack traceback"),
          "fsm: the callback error is logged with path:line and a traceback");
    check(fsm.change("err", "ok") && fsm.state("err") == "ok", "fsm: the machine stays usable");

    const auto start = std::chrono::steady_clock::now();
    check(fsm.change("err", "slow") && fsm.state("err") == "slow", "fsm: a runaway enter commits too");
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(5) &&
              said(R"(State machine fsms/err.lua: enter "slow" failed: fsms/err.lua:6: script ran longer than 2 seconds)"),
          "fsm: a runaway callback is stopped by the script budget and logged");
    check(fsm.change("err", "ok") && said("slow exit"), "fsm: Lua runs again after the timeout; the machine still works");
    w.world.tick(T0);
    check(changed.size() == 4, "fsm: failing callbacks still publish fsm.changed");
    check(said("systems/a_count.lua: tick 1"), "fsm: systems keep running after a callback error");
    for (const auto& s : w.world.script_status()) check(s.ok, "fsm errors: no system disabled: " + s.error);

    // From inside a system's update: a game machine runs in the realm context (map contexts), so the change is
    // applied there at the end of the tick and the runaway callback uses up the realm's own budget: stopped and logged
    // as above, and the system that asked keeps running.
    w.spawn(1, 91, "Alice", 1000, 1000);
    clear_log();
    w.world.tick(T0 + std::chrono::milliseconds(50));
    const auto status = w.world.script_status();
    check(status.size() == 2 && status[0].ok && status[1].ok &&
              said(R"(State machine fsms/err.lua: enter "slow" failed: fsms/err.lua:6: script ran longer than 2 seconds)"),
          "fsm: a runaway callback a system triggered runs (and is stopped) in the realm; no system is disabled");
    check(fsm.state("err") == "slow" && fsm.change("err", "ok"), "fsm: ... and the machine still works");
}

// chthonic.fsm misuse raises at the calling line; chthonic.fsm is read-only; an init touching a game machine enters it once.
void checkFsmLuaApi() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/door.lua", kDoor).write("fsms/plain.lua", kPlain);
    r.write("systems/a_line.lua", R"(local ok, err = pcall(function()
    local state = chthonic.fsm.state("nope") -- not a tail call: that would drop this frame and its line
end)
chthonic.log("line err " .. tostring(err))
return { init = function() chthonic.log("init sees " .. chthonic.fsm.state("door")) end, update = function() end })");
    r.write("systems/b_api.lua", std::string(kLuaExpect) + R"(local done = false
return { update = function(dt)
    if done then return end
    done = true
    local bob
    for e in chthonic.query("Character") do bob = e end
    raises('chthonic.fsm.state: no game state machine "nope" (GAME_STATE_MACHINES)', function() return chthonic.fsm.state("nope") end)
    raises('chthonic.fsm.change: no game state machine "plain"', function() return chthonic.fsm.change("plain", "b") end)
    raises('chthonic.fsm.state: invalid entity id 4000000000', function() return chthonic.fsm.state(4000000000) end)
    raises('chthonic.fsm.state: invalid entity id 1.5', function() return chthonic.fsm.state(1.5) end)
    raises('chthonic.fsm.canChange: invalid entity id -1', function() return chthonic.fsm.canChange(-1, "a") end)
    raises('chthonic.fsm.states: entity ' .. bob .. ' has no "StateMachine"', function() return chthonic.fsm.states(bob) end)
    raises('chthonic.fsm.state: target must be a state machine name or an entity id, got true', function() return chthonic.fsm.state(true) end)
    raises('chthonic.fsm.change: state must be a string, got 5', function() return chthonic.fsm.change("door", 5) end)
    raises('chthonic.fsm.canChange: state must be a string, got nil', function() return chthonic.fsm.canChange("door") end)
    expect(chthonic.fsm.change("door", "nowhere") == false, "an unknown state is a denial, not an error")
    expect(chthonic.fsm.canChange("door", "open") == true and chthonic.fsm.state("door") == "closed", "canChange / state")
    local names = chthonic.fsm.states("door")
    expect(#names == 4 and names[1] == "broken" and names[4] == "open", "states: a sorted table")
    raises("chthonic is read-only", function() chthonic.fsm.change = nil end)
    raises("chthonic is read-only", function() chthonic.fsm = nil end)
    raises("chthonic is read-only", function() chthonic.fsm.x = 1 end)
    expect(type(chthonic.fsm.change) == "function", "chthonic.fsm unchanged")
    chthonic.log("api checks done")
end })");
    r.write("systems/c_raise.lua", "return { update = function()\n chthonic.fsm.state('nope')\nend }");
    TestWorld w({});
    check(w.world.load_scripts(r.root, {"door"}).empty(), "fsm api: loads");
    check(said_first("fsms/door.lua: enter nil->closed") < said_first("systems/a_line.lua: init sees closed") &&
              said_at("fsms/door.lua: enter").size() == 1,
          "fsm: a game machine touched during init enters right then, and only once");
    check(said(R"(systems/a_line.lua: line err systems/a_line.lua:2: chthonic.fsm.state: no game state machine "nope")"),
          "fsm: a misuse raises at the calling line");
    w.spawn(1, 92, "Bob", 1000, 1000);
    w.world.tick(T0);
    check_lua_expectations("fsm api", "api checks done");
    const auto status = w.world.script_status();
    check(status.size() == 3 && status[1].ok && !status[2].ok &&
              contains(status[2].error, "systems/c_raise.lua:2: chthonic.fsm.state: no game state machine \"nope\""),
          "fsm: an uncaught misuse in a system follows the system error policy");
}

// The StateMachine built-in on characters: add, set machine / initialState, changes, remove, despawn, shutdown, not
// persisted or replicated; and C++ / Lua parity on entity machines.
void checkFsmEntities() {
    LogCapture capture;
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', persist = true, replicate = 'aoi', fields = { { name = 'hp', type = 'int' } } }");
    r.write("fsms/door.lua", kDoor).write("fsms/plain.lua", kPlain);
    r.write("systems/sm.lua", std::string(kLuaExpect) + R"lua(local parity = {}
local function run(e)
    local out = {}
    local function add(v) out[#out + 1] = tostring(v) end
    add(chthonic.fsm.state(e)); add(chthonic.fsm.canChange(e, "open")); add(chthonic.fsm.canChange(e, "closed"))
    add(chthonic.fsm.change(e, "closed")); add(chthonic.fsm.change(e, "flying")); add(chthonic.fsm.change(e, "open"))
    add(chthonic.fsm.state(e)); add(chthonic.fsm.change(e, "broken")); add(chthonic.fsm.change(e, "broken"))
    add(table.concat(chthonic.fsm.states(e), ","))
    return table.concat(out, " ")
end
return {
    onEnter = function(e)
        chthonic.add(e, "Health")
        local name = chthonic.get(e, "Character").name
        chthonic.add(e, "StateMachine")
        if name ~= "Alice" then
            chthonic.set(e, "StateMachine", {machine = "door"})
            parity[e] = name == "Carol"
            return
        end
        expect(chthonic.has(e, "StateMachine"), "has after add")
        local sm = chthonic.get(e, "StateMachine")
        expect(sm.machine == "" and sm.initialState == "", "add leaves machine and initialState empty")
        raises('chthonic.fsm.state: entity ' .. e .. ': StateMachine has no "machine"', function() return chthonic.fsm.state(e) end)
        chthonic.set(e, "StateMachine", {machine = "door", initialState = "bogus"})
        expect(chthonic.fsm.state(e) == "closed", "machine set: enters the definition's initial (bad initialState)")
        chthonic.set(e, "StateMachine", {initialState = "open"})
        expect(chthonic.fsm.state(e) == "closed", "changing only initialState does nothing")
        expect(chthonic.fsm.change(e, "open"), "change on an entity machine")
        chthonic.set(e, "StateMachine", {machine = "plain", initialState = "b"})
        expect(chthonic.fsm.state(e) == "b", "changing machine enters the new machine's initialState")
        chthonic.set(e, "StateMachine", {machine = "nope"})
        raises('chthonic.fsm.state: entity ' .. e .. ': no valid state machine definition "nope"', function() return chthonic.fsm.state(e) end)
        chthonic.set(e, "StateMachine", {initialState = ""})
        chthonic.set(e, "StateMachine", {machine = "door"})
        expect(chthonic.fsm.state(e) == "closed", "back to door: its initial (empty initialState)")
        chthonic.remove(e, "StateMachine")
        raises('chthonic.fsm.state: entity ' .. e .. ' has no "StateMachine"', function() return chthonic.fsm.state(e) end)
        chthonic.add(e, "StateMachine")
        chthonic.set(e, "StateMachine", {machine = "door", initialState = "locked"})
        expect(chthonic.fsm.state(e) == "locked", "re-added: enters a valid initialState")
        raises('"StateMachine": unknown field "current"', function() chthonic.set(e, "StateMachine", {current = "x"}) end)
        raises('must be a string', function() chthonic.set(e, "StateMachine", {machine = 5}) end)
        chthonic.log("entity checks done")
    end,
    update = function(dt)
        for e, lua in pairs(parity) do
            if lua then chthonic.log("parity " .. run(e)) end
            parity[e] = nil
        end
    end,
})lua");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "fsm entities: loads");
    const auto alice = w.spawn(1, 93, "Alice", 1000, 1000);
    const uint32_t A = wire(alice);
    check_lua_expectations("fsm entities", "entity checks done");
    const std::string who = "entity " + std::to_string(A);
    check(said(who + R"(: StateMachine "door": initialState "bogus" is not one of its states; using "closed")"),
          "fsm: a bad initialState falls back to initial with a warning");
    check(said(who + R"(: StateMachine "door": initialState is empty; using "closed")"), "fsm: an empty initialState warns");
    check(said_at(who + R"(: StateMachine: no valid state machine definition "nope")").size() == 1,
          "fsm: a machine with no valid definition is logged once");
    check(said("fsms/door.lua: enter nil->closed entity " + std::to_string(A)) &&
              said("fsms/door.lua: enter nil->locked entity " + std::to_string(A)),
          "fsm: entity machines enter with ctx.entity = the wire id and from = nil");
    check(said_at("fsms/door.lua: exit").size() == 1, "fsm: only the explicit change ran an exit (machine swap and remove don't)");

    // C++ / Lua parity: Carol is driven from Lua, Dave from C++, with the same operations.
    const auto carol = w.spawn(2, 94, "Carol", 1010, 1000);
    const auto dave = w.spawn(3, 95, "Dave", 1020, 1000);
    std::vector<nlohmann::json> changed;
    EventBus::Subscription sub = w.world.bus().subscribe("fsm.changed", [&](const nlohmann::json& p) { changed.push_back(p); });
    clear_log();
    w.world.tick(T0);
    auto& fsm = w.world.fsm();
    const lua::StateMachines::Target d = wire(dave);
    std::string cpp = fsm.state(d) + " " + (fsm.canChange(d, "open") ? "true" : "false") + " " +
                      (fsm.canChange(d, "closed") ? "true" : "false");
    for (const char* to : {"closed", "flying", "open"}) cpp += std::string(" ") + (fsm.change(d, to) ? "true" : "false");
    cpp += " " + fsm.state(d);
    for (const char* to : {"broken", "broken"}) cpp += std::string(" ") + (fsm.change(d, to) ? "true" : "false");
    std::string names;
    for (const auto& n : fsm.states(d)) names += (names.empty() ? "" : ",") + n;
    cpp += " " + names;
    check(cpp == "closed true false false false true open true true broken,closed,locked,open", "fsm C++: " + cpp);
    check(said("systems/sm.lua: parity " + cpp), "fsm: Lua gives the same results as C++");
    const auto warnings = [&](entt::entity e) {
        return said_at("State machine \"door\" (entity " + std::to_string(wire(e)) + ")").size();
    };
    check(warnings(carol) == 2 && warnings(dave) == 2, "fsm: the same warnings from Lua and C++");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    const auto events_of = [&](entt::entity e) {
        return std::ranges::count_if(changed, [&](const nlohmann::json& p) { return p["entity"] == wire(e); });
    };
    check(events_of(carol) == 3 && events_of(dave) == 3, "fsm: the same events from Lua and C++, entity = wire id");

    try {
        fsm.state(std::string("nope"));
        check(false, "fsm C++: an unknown game machine throws");
    } catch (const std::invalid_argument& e) {
        check(contains(e.what(), "no game state machine \"nope\""), std::string("fsm C++: ") + e.what());
    }
    for (const lua::StateMachines::Target& bad : {lua::StateMachines::Target{std::string("door")}, lua::StateMachines::Target{999999u}}) {
        bool threw = false;
        try { fsm.change(bad, "open"); } catch (const std::invalid_argument&) { threw = true; }
        check(threw, "fsm C++: bad targets throw std::invalid_argument");
    }

    // Not persisted, not replicated; despawn and shutdown run no exit.
    bool replicated = false;
    for (const auto& [sid, msgs] : w.sent.bySession)
        for (const auto& m : msgs)
            if (auto* u = std::get_if<proto::ComponentUpdate>(&m); u && u->name == "StateMachine") replicated = true;
    check(!replicated, "fsm: StateMachine is never replicated");
    check(w.sent.of<proto::ComponentUpdate>(1).size() > 0, "fsm: other components still replicate");
    clear_log();
    w.sent.clear();
    w.world.despawn(alice);
    check(w.sent.saves.size() == 1 && w.sent.saves[0].components && contains(*w.sent.saves[0].components, "Health") &&
              !contains(*w.sent.saves[0].components, "StateMachine"),
          "fsm: StateMachine is not persisted");
    w.world.shutdown_scripts();
    w.world.despawn(carol);
    w.world.despawn(dave);
    check(said_at("fsms/door.lua: exit").empty(), "fsm: despawn and shutdown run no exit");
    check(w.world.registry().view<lua::StateMachineRuntime>().empty(), "fsm: machines go away with their characters");
}

// chthonic.fsm.state through enter callbacks that change the component: a lazy first enter (chthonic.create's components), an
// enter that queues a change, one that swaps the machine, one that removes it, one that reads its own state.
void checkFsmStateFlows() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/hop.lua", R"(return { name = "hop", initial = "a", states = {
        a = { enter = function(ctx) chthonic.fsm.change(ctx.entity, "b") end }, b = {} } })");
    r.write("fsms/vanish.lua", R"(return { name = "vanish", initial = "x", states = {
        x = { enter = function(ctx) chthonic.remove(ctx.entity, "StateMachine") end } } })");
    r.write("fsms/swap.lua", R"(return { name = "swap", initial = "s", states = {
        s = { enter = function(ctx) chthonic.set(ctx.entity, "StateMachine", {machine = "hop"}) end } } })");
    r.write("fsms/peek.lua", R"(return { name = "peek", initial = "p", states = {
        p = { enter = function(ctx) chthonic.log("peek " .. chthonic.fsm.state(ctx.entity)) end }, q = {} } })");
    r.write("fsms/plain.lua", kPlain);
    r.write("systems/flows.lua", std::string(kLuaExpect) + R"lua(local done = false
local function state(e)
    local ok, s = pcall(chthonic.fsm.state, e)
    return ok and s or ("error: " .. s)
end
return { update = function()
    if done then return end
    done = true
    local out = {}
    local function add(v) out[#out + 1] = tostring(v) end
    for _, m in ipairs({"plain", "hop", "vanish", "swap", "peek"}) do
        local lazy = chthonic.create(m, {template = "T", components = {StateMachine = {machine = m}}})
        add(m .. "=" .. state(lazy)); add(state(lazy))
        local set = chthonic.create(m, {template = "T"})
        chthonic.add(set, "StateMachine")
        chthonic.set(set, "StateMachine", {machine = m})
        add(state(set))
    end
    local e = chthonic.create("e", {template = "T"})
    chthonic.add(e, "StateMachine")
    chthonic.set(e, "StateMachine", {machine = "plain", initialState = "b"})
    add(state(e)); add(chthonic.fsm.change(e, "a")); add(state(e)); add(chthonic.fsm.change(e, "b")); add(state(e))
    chthonic.set(e, "StateMachine", {machine = "peek"})
    add(state(e)); add(chthonic.fsm.change(e, "q")); add(state(e))
    chthonic.log("flows " .. table.concat(out, " "))
end })lua");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "fsm flows: loads");
    w.world.tick(T0);
    check_lua_expectations("fsm flows", "flows ");
    std::string got;
    for (size_t i : said_at("flows ")) got = nlohmann::json::parse(g_log[i]).value("msg", "");
    // Captured on the code before the chthonic.fsm.state changes (lua-bridge-churn).
    const std::string want = "systems/flows.lua: flows plain=a a a hop=b b b vanish=error: chthonic.fsm.state: entity 4 has no "
                             "\"StateMachine\" error: chthonic.fsm.state: entity 4 has no \"StateMachine\" error: chthonic.fsm.state: "
                             "entity 5 has no \"StateMachine\" swap=b b b peek=p p p b true a true b p true q";
    check(got == want, "fsm flows: [" + got + "]");
    check(said_at("fsms/peek.lua: peek p").size() == 3,
          "fsm flows: an enter reading its own state gets the state it enters");
}

// --- chthonic.get / chthonic.set on a bare registry: a Runtime of its own (spawn records from hooks.spawn), so a timed update
// runs nothing else ---

constexpr const char* kSix = R"(return { name = "Six", replicate = "aoi", persist = true, fields = {
    { name = "hp", type = "int", default = 10, min = 0, max = 100 },
    { name = "flag", type = "bool" },
    { name = "kind", type = "enum", values = {"a", "b"}, default = "b" },
    { name = "pos", type = "vec3", default = {x = 1, y = 2, z = 3} },
    { name = "tags", type = "array", of = "string", default = {"a"} },
    { name = "speed", type = "float", default = 1.5 },
} })";

// Entity 0: character Alice (Six added by the system's init). 1 and 2: NPCs of spawn record 0 ("guard_1", an 8-point
// path); 3: record 1 ("bat_1", no path). The system file is kLuaExpect + system.
struct Bench {
    LuaRoot r;
    entt::registry reg;
    std::vector<map::Spawn> spawns;
    std::unique_ptr<lua::Runtime> rt;
    explicit Bench(const std::string& system) {
        r.write("components/six.lua", kSix).write("fsms/plain.lua", kPlain);
        r.write("systems/t.lua", std::string(kLuaExpect) + system);
        map::PatrolPath path{{}, 1.5f, false};
        for (int i = 0; i < 8; ++i) path.points.push_back({1000.0f + i, 0.25f * i, 2000.0f - i});
        spawns = {{"guard_1", "Guard", {1024, 3, 1030}, 0.5f, 30, path, ""},
                  {"bat_1", "Bat", {5, 6, 7}, 0, -1, std::nullopt, ""}};
        const auto alice = reg.create();
        reg.emplace<world::CharacterRef>(alice, 11u, "Alice");
        reg.emplace<world::Transform>(alice, proto::Vec3{1000, 0, 1000}, 0.5f);
        reg.emplace<world::MapRef>(alice, uint16_t{1});
        reg.emplace<world::ReplicatedState>(alice);
        for (uint32_t record : {0u, 0u, 1u}) {
            const auto e = reg.create();
            reg.emplace<world::Npc>(e, "N", "T", "");
            reg.emplace<world::Transform>(e, proto::Vec3{1020, 0, 1020}, 0.f);
            reg.emplace<world::MapRef>(e, uint16_t{1});
            reg.emplace<world::SpawnRef>(e, record);
        }
        lua::NpcHooks hooks;
        hooks.spawn = [this](uint32_t i) { return &spawns.at(i); };
        rt = std::make_unique<lua::Runtime>(reg, r.defs().defs, std::move(hooks));
        check(rt->load(r.root).empty(), "bench: loads");
    }
    // us per call in the fastest of `updates` updates (the least disturbed by the rest of the machine), each making
    // `calls` of them.
    double time(int updates, int calls) {
        double best = HUGE_VAL;
        for (int i = 0; i < updates; ++i) {
            const auto start = std::chrono::steady_clock::now();
            rt->update(0.05f);
            best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
        }
        for (const auto& s : rt->status()) check(s.ok, "bench: system still enabled " + s.error);
        return best / calls;
    }
};

// Every chthonic.set failure kind on a Lua component, message for message; each leaves the component unchanged.
void checkSetErrors() {
    LogCapture capture;
    Bench b(R"lua(local function err(f) local ok, e = pcall(f) ; return ok and "no error" or e end
return { init = function() chthonic.add(0, "Six") end, update = function()
    local cases = {
        {5},
        {nope = 1},
        {hp = 1.5},
        {flag = "x"},
        {speed = "fast"},
        {kind = 3},
        {pos = {x = 1, y = 2, z = "z"}},
        {hp = 101},
        {hp = -1},
        {kind = "zzz"},
        {tags = {"a", 3}},
        {tags = {x = 1}},
        {tags = "a"},
        {pos = 5},
        {hp = 1 / 0},
        {hp = 2 ^ 40},
        {flag = "x", hp = 101},
        {hp = 101, pos = 5},
        {hp = 50, kind = "zzz"},
        {hp = 50, nope = 1},
    }
    for i, c in ipairs(cases) do
        chthonic.log("SET " .. i .. " " .. err(function() chthonic.set(0, "Six", c) end))
    end
    chthonic.log("set errors done")
end })lua");
    const Json before = lua::pool(b.reg, "Six").get(entt::entity{0}).value;
    b.rt->update(0.05f);
    check_lua_expectations("set errors", "set errors done");
    std::vector<std::string> got;
    for (size_t i : said_at("systems/t.lua: SET ")) got.push_back(nlohmann::json::parse(g_log[i]).value("msg", ""));
    // Captured on the code before the chthonic.set change (set_fields + component_from_json, lua-bridge-churn).
    const std::vector<std::string> want = {
        R"(systems/t.lua: SET 1 systems/t.lua:32: "Six": field names must be strings, got 1)",
        R"(systems/t.lua: SET 2 systems/t.lua:32: "Six": unknown field "nope")",
        R"(systems/t.lua: SET 3 systems/t.lua:32: "Six": "hp" must be an integer)",
        R"(systems/t.lua: SET 4 systems/t.lua:32: "Six": "flag" must be true or false)",
        R"(systems/t.lua: SET 5 systems/t.lua:32: "Six": "speed" must be a number)",
        R"(systems/t.lua: SET 6 systems/t.lua:32: "Six": "kind" unknown value 3 (expected one of "a", "b"))",
        R"(systems/t.lua: SET 7 systems/t.lua:32: "pos" must be a table {x=, y=, z=} of finite numbers)",
        R"(systems/t.lua: SET 8 systems/t.lua:32: "Six": "hp" must be <= 100)",
        R"(systems/t.lua: SET 9 systems/t.lua:32: "Six": "hp" must be >= 0)",
        R"(systems/t.lua: SET 10 systems/t.lua:32: "Six": "kind" unknown value "zzz" (expected one of "a", "b"))",
        R"(systems/t.lua: SET 11 systems/t.lua:32: "Six": "tags" element 2 must be a string)",
        R"(systems/t.lua: SET 12 systems/t.lua:32: "tags" must be a sequence table {v1, v2, ...})",
        R"(systems/t.lua: SET 13 systems/t.lua:32: "tags" must be a sequence table {v1, v2, ...})",
        R"(systems/t.lua: SET 14 systems/t.lua:32: "pos" must be a table {x=, y=, z=} of finite numbers)",
        R"(systems/t.lua: SET 15 systems/t.lua:32: "hp" must be a finite number)",
        R"(systems/t.lua: SET 16 systems/t.lua:32: "Six": "hp" must be a 32-bit integer)",
        R"(systems/t.lua: SET 17 systems/t.lua:32: "Six": "hp" must be <= 100)",
        R"(systems/t.lua: SET 18 systems/t.lua:32: "pos" must be a table {x=, y=, z=} of finite numbers)",
        R"(systems/t.lua: SET 19 systems/t.lua:32: "Six": "kind" unknown value "zzz" (expected one of "a", "b"))",
        R"(systems/t.lua: SET 20 systems/t.lua:32: "Six": unknown field "nope")",
    };
    check(got.size() == want.size(), std::format("set errors: {} messages, want {}", got.size(), want.size()));
    for (size_t i = 0; i < std::min(got.size(), want.size()); ++i)
        check(got[i] == want[i], std::format("set error {}: got [{}] want [{}]", i + 1, got[i], want[i]));
    check(lua::pool(b.reg, "Six").get(entt::entity{0}).value == before, "set errors: Six unchanged by every failed set");
}

// The chthonic.set / chthonic.get / chthonic.fsm.state hot paths, called from Lua as scripts do. Spawn, set and fsm.state assert a
// 3x speedup over the code before lua-bridge-churn; Transform and Lua component gets only print.
void checkBridgeTiming() {
    constexpr int kUpdates = 20, kCalls = 2000;
    // before: the us/call on the code before lua-bridge-churn, printed with the new number.
    const auto run = [&](const char* what, const std::string& init, const std::string& body, const char* before) {
        LogCapture capture;
        Bench b("return { init = function() " + init + " end, update = function() for i = 1, " + std::to_string(kCalls) +
                " do " + body + " end end }");
        const double us = b.time(kUpdates, kCalls);
        std::printf("%s: %.3f us/call (before, Release: %s)\n", what, us, before);
        return us;
    };
    const double spawn =
        run("chthonic.get(e, \"Spawn\") (8-point path)", "", "local s = chthonic.get(1, \"Spawn\")", "2.214-2.242");
    const double set = run("chthonic.set one field of a 6-field component", "chthonic.add(0, \"Six\")",
                           "chthonic.set(0, \"Six\", {hp = i % 100})", "2.254-2.299");
    const double state = run("chthonic.fsm.state(e)",
                             "chthonic.add(1, \"StateMachine\") ; chthonic.set(1, \"StateMachine\", {machine = \"plain\"})",
                             "local s = chthonic.fsm.state(1)", "0.389-0.393");
    run("chthonic.get(e, \"Transform\")", "", "local t = chthonic.get(1, \"Transform\")", "0.466-0.468");
    run("chthonic.get(e, \"Six\") (6 fields)", "chthonic.add(0, \"Six\")", "local t = chthonic.get(0, \"Six\")", "2.076-2.090");
#ifdef NDEBUG
    // Baselines before lua-bridge-churn (a fresh Spawn table per call, set through set_fields + component_from_json,
    // fsm.state re-hashing the StateMachine pool and copying strings): the fastest of 20 updates, 5 runs, Release
    // (preset release, MSVC, the dev machine); the spreads are above. Each must be at least 3x faster. Checked only
    // with --timing in Release.
    constexpr double kSpawnUs = 2.23, kSetUs = 2.27, kStateUs = 0.39;
    if (g_timing_gates) {
        check(spawn <= kSpawnUs / 3, std::format("timing: Spawn get {:.3f} us > baseline {} / 3", spawn, kSpawnUs));
        check(set <= kSetUs / 3, std::format("timing: set {:.3f} us > baseline {} / 3", set, kSetUs));
        check(state <= kStateUs / 3, std::format("timing: fsm.state {:.3f} us > baseline {} / 3", state, kStateUs));
    }
#else
    (void)spawn;
    (void)set;
    (void)state;
#endif
}

// chthonic.get(e, "Spawn"): one shared, read-only table per spawn record, with the values it always had.
void checkSpawnCache() {
    LogCapture capture;
    Bench b(R"lua(return { update = function()
    local a, b = chthonic.get(1, "Spawn"), chthonic.get(3, "Spawn")
    expect(rawequal(a, chthonic.get(1, "Spawn")), "two calls: the same table")
    expect(rawequal(a, chthonic.get(2, "Spawn")), "two NPCs of one record: the same table")
    expect(not rawequal(a, b), "another record: another table")
    expect(a.id == "guard_1" and a.pos.x == 1024 and a.pos.y == 3 and a.pos.z == 1030 and a.heading == 0.5, "id, pos, heading")
    expect(#a.path == 8 and a.wait == 1.5 and a.loop == false, "path, wait, loop")
    local n = 0
    for i, p in ipairs(a.path) do
        n = n + 1
        expect(p.x == 999 + i and p.y == 0.25 * (i - 1) and p.z == 2001 - i, "path point " .. i)
    end
    expect(n == 8, "ipairs walks the path")
    expect(b.id == "bat_1" and b.pos.x == 5 and b.pos.y == 6 and b.pos.z == 7 and b.heading == 0, "no path: id, pos, heading")
    expect(b.path == nil and b.wait == nil and b.loop == nil, "no path: no path, wait or loop")
    raises('"Spawn" is read-only', function() a.id = "x" end)
    raises('"Spawn" is read-only', function() a.extra = 1 end)
    raises('"Spawn" is read-only', function() a.pos.x = 1 end)
    raises('"Spawn" is read-only', function() a.path[9] = {x = 1, y = 2, z = 3} end)
    raises('"Spawn" is read-only', function() a.path.name = "p" end)
    raises('"Spawn" is read-only', function() a.path[1].x = 1 end)
    raises('"Spawn" is read-only', function() b.wait = 1 end)
    expect(getmetatable(a) == false and getmetatable(a.pos) == false and getmetatable(a.path) == false and
        getmetatable(a.path[1]) == false, "metatables hidden")
    expect(a.id == "guard_1" and a.pos.x == 1024 and #a.path == 8 and a.path[1].x == 1000, "failed writes changed nothing")
    chthonic.log("spawn cache done")
end })lua");
    b.rt->update(0.05f);
    check_lua_expectations("spawn cache", "spawn cache done");
}

// chthonic.set on a Lua component: an equal value marks nothing; a partial set changes only its fields.
void checkSetPartialAndNoOp() {
    LogCapture capture;
    Bench b(R"lua(local t = 0
return { init = function() chthonic.add(0, "Six") end, update = function()
    t = t + 1
    if t == 1 then chthonic.set(0, "Six", {hp = 10, flag = false, kind = "b", pos = {x = 1, y = 2, z = 3}, tags = {"a"}, speed = 1.5})
    else chthonic.set(0, "Six", {hp = 42, tags = {"x", "y"}}) end
end })lua");
    const entt::entity alice{0};
    auto& rep = b.reg.get<world::ReplicatedState>(alice);
    rep.dirty.clear(); // chthonic.add marked it
    b.reg.remove<world::Dirty>(alice);
    const Json& value = lua::pool(b.reg, "Six").get(alice).value;
    const Json before = value;
    b.rt->update(0.05f);
    check(value == before && rep.dirty.empty() && !b.reg.all_of<world::Dirty>(alice),
          "set no-op: an equal value is not replicated or saved");
    b.rt->update(0.05f);
    Json want = before;
    want["hp"] = 42;
    want["tags"] = Json::array({"x", "y"});
    check(value.dump() == want.dump(), "set partial: only the listed fields change: " + value.dump());
    check(rep.dirty.contains("Six") && b.reg.all_of<world::Dirty>(alice), "set: a change is replicated and saved");
}

} // namespace

// --- NPCs (docs/world/npcs.html) ---

// Every chthonic.create / chthonic.destroy / NPC Transform rule from Lua; each bad call raises and changes nothing.
constexpr const char* kNpcApiSystem = R"lua(
local EDGE = 2048 - 2 ^ -13 -- the largest float below 2048: still on tile 7
local npcs = function() local n = 0 ; for _ in chthonic.query("Npc") do n = n + 1 end ; return n end
local done = false
return { update = function()
    if done then return end
    done = true
    local players = {}
    for e in chthonic.query("Character") do players[#players + 1] = e end
    local p = players[1]
    local g = chthonic.create("Guard", {template = "GuardTpl"})
    expect(type(g) == "number" and g ~= p, "create returns a new wire id")
    expect(chthonic.has(g, "Npc") and chthonic.has(g, "Transform") and not chthonic.has(g, "Character"), "Npc + Transform, no Character")
    expect(not chthonic.has(p, "Npc"), "players have no Npc")
    local n = chthonic.get(g, "Npc")
    expect(n.name == "Guard" and n.template == "GuardTpl" and n.npc == "", "Npc {name, template, npc}; npc empty with a template")
    local tf = chthonic.get(g, "Transform")
    expect(tf.position.x == 1024 and tf.position.y == 0 and tf.position.z == 1024 and tf.heading == 0, "default: spawn point, heading 0")
    raises('entity ' .. g .. ' has no "Character"', function() chthonic.get(g, "Character") end)
    local h = chthonic.create("Far", {template = "T", position = {x = -5, y = 1000, z = 5000}, heading = 2})
    tf = chthonic.get(h, "Transform")
    expect(tf.position.x == 0 and tf.position.y == 0 and tf.position.z == EDGE and tf.heading == 2 and tf.map == 1, "position clamped onto the map (tile 7's far edge belongs to tile 8), y onto the ground")
    local ids, chars, all = {}, 0, {}
    for e in chthonic.query("Npc") do ids[#ids + 1] = e end
    for _ in chthonic.query("Character") do chars = chars + 1 end
    for e in chthonic.query() do all[#all + 1] = e end
    expect(#ids == 2 and ids[1] == g and ids[2] == h, "query Npc: NPCs only, in id order")
    expect(chars == 1, "query Character: players only")
    expect(#all == 3 and all[1] < all[2] and all[2] < all[3], "query(): players and NPCs in id order")

    local before = npcs()
    local long = string.rep("x", 256)
    local nameErr = "chthonic.create: name must be a non-empty string of at most 255 bytes, got "
    local tplErr = "chthonic.create: template must be a non-empty string of at most 255 bytes, got "
    local posErr = 'chthonic.create: "position" must be a table {x=, y=, z=} of finite numbers'
    for _, c in ipairs({
        {nameErr .. "nil", nil, {template = "T"}},
        {nameErr, "", {template = "T"}},
        {nameErr .. "5", 5, {template = "T"}},
        {nameErr .. "xxx", long, {template = "T"}},
        {"chthonic.create: opts must be a table {npc = ...} or {template = ...}, got nil", "N", nil},
        {"chthonic.create: opts must be a table {npc = ...} or {template = ...}, got 5", "N", 5},
        {tplErr, "N", {template = ""}},
        {tplErr .. "7", "N", {template = 7}},
        {tplErr .. "xxx", "N", {template = long}},
        {posErr, "N", {template = "T", position = 5}},
        {posErr, "N", {template = "T", position = {x = 1, y = 2}}},
        {posErr, "N", {template = "T", position = {x = 1 / 0, y = 0, z = 0}}},
        {posErr, "N", {template = "T", position = {x = 1e39, y = 0, z = 0}}},
        {'chthonic.create: "heading" must be a finite number', "N", {template = "T", heading = "x"}},
        {'chthonic.create: "heading" must be a finite number', "N", {template = "T", heading = 0 / 0}},
        {"chthonic.create: unknown option speed", "N", {template = "T", speed = 1}},
        {"chthonic.create: unknown option 1", "N", {template = "T", "extra"}},
    }) do raises(c[1], function() chthonic.create(c[2], c[3]) end) end
    expect(npcs() == before, "no failed create made an NPC")

    raises("chthonic.destroy: entity " .. p .. " is a player; only NPCs can be destroyed", function() chthonic.destroy(p) end)
    raises("chthonic.destroy: invalid entity id 1.5", function() chthonic.destroy(1.5) end)
    raises("chthonic.destroy: invalid entity id x", function() chthonic.destroy("x") end)
    raises("chthonic.destroy: invalid entity id nil", function() chthonic.destroy() end)
    raises("chthonic.destroy: invalid entity id 999999", function() chthonic.destroy(999999) end)
    expect(npcs() == before, "no failed destroy removed an NPC")
    chthonic.destroy(h)
    expect(npcs() == before - 1, "destroy removes the NPC")
    raises("chthonic.destroy: invalid entity id " .. h, function() chthonic.destroy(h) end)
    raises("invalid entity id " .. h, function() chthonic.get(h, "Transform") end)

    chthonic.set(g, "Transform", {heading = 1})
    tf = chthonic.get(g, "Transform")
    expect(tf.heading == 1 and tf.position.x == 1024, "partial Transform set: heading only")
    chthonic.set(g, "Transform", {position = {x = 3000, y = -500, z = 10}})
    tf = chthonic.get(g, "Transform")
    expect(tf.position.x == EDGE and tf.position.y == 0 and tf.position.z == 10 and tf.heading == 1, "Transform set clamps, y onto the ground")
    raises('"Transform": unknown field "pos"', function() chthonic.set(g, "Transform", {heading = 2, pos = {}}) end)
    raises('"Transform": field names must be strings, got 1', function() chthonic.set(g, "Transform", {5}) end)
    raises('"Transform": "position" must be a table {x=, y=, z=} of finite numbers', function() chthonic.set(g, "Transform", {heading = 2, position = {x = 1}}) end)
    raises('"Transform": "heading" must be a finite number', function() chthonic.set(g, "Transform", {position = {x = 1, y = 1, z = 1}, heading = 1 / 0}) end)
    tf = chthonic.get(g, "Transform")
    expect(tf.position.x == EDGE and tf.heading == 1, "every failed Transform set left it unchanged")
    raises('"Transform" is read-only on the server', function() chthonic.set(p, "Transform", {heading = 1}) end)
    raises('"Npc" is read-only on the server', function() chthonic.set(g, "Npc", {name = "x"}) end)
    raises('"Npc" is read-only on the server', function() chthonic.add(g, "Npc") end)
    raises('"Npc" cannot be removed', function() chthonic.remove(g, "Npc") end)
    raises('"Transform" cannot be removed', function() chthonic.remove(g, "Transform") end)
    raises('"Character" is read-only on the server', function() chthonic.add(g, "Character") end)

    chthonic.add(g, "Stats")
    chthonic.set(g, "Stats", {hp = 42})
    expect(chthonic.get(g, "Stats").hp == 42 and chthonic.has(g, "Stats"), "Lua components work on NPCs")
    chthonic.remove(g, "Stats")
    expect(not chthonic.has(g, "Stats"), "remove on an NPC")
    chthonic.add(g, "StateMachine")
    chthonic.set(g, "StateMachine", {machine = "plain"})
    expect(chthonic.fsm.state(g) == "a", "an NPC machine enters its initial state")
    expect(chthonic.fsm.change(g, "b") and chthonic.fsm.state(g) == "b", "chthonic.fsm.change on an NPC")
    chthonic.sendTo(g, "npc.hello", {x = 1}) -- an NPC has no client: nothing is sent, no error
    chthonic.log("npc api done")
end })lua";

void checkNpcApi() {
    LogCapture capture;
    LuaRoot r;
    r.write("components/stats.lua", kStats).write("fsms/plain.lua", kPlain);
    r.write("systems/npc.lua", std::string(kLuaExpect) + kNpcApiSystem);
    world::WorldRules rules;
    rules.activationTiles = 8; // "Far" is 4 tiles from Alice: awake too, so chthonic.query sees it
    TestWorld w(r.defs().defs, rules);
    check(w.world.load_scripts(r.root).empty(), "npc api: loads");
    w.spawn(1, 101, "Alice", 1000, 1000);
    w.sent.clear();
    w.world.tick(T0);
    check_lua_expectations("npc api", "npc api done");
    for (const auto& s : w.world.script_status()) check(s.ok, "npc api: system still enabled " + s.error);
    check(w.sent.of<proto::ServerEvent>(1).empty(), "sendTo(npc) sends nothing");
}

// chthonic.query walks the smallest storage among its types: exactly the in-world matches, in wire id order, whichever
// pool is smaller. Each update logs "q<tick> <names>: [ids]"; the second also destroys an NPC mid-loop.
void checkQueryPools() {
    LogCapture capture;
    LuaRoot r;
    for (const char* n : {"Health", "Mana", "Unused"})
        r.write(std::string("components/") + n + ".lua", std::string("return { name = '") + n + "', fields = {} }");
    r.write("systems/q.lua", R"(local function list(it)
    local out = {}
    for e in it do out[#out + 1] = e end
    return out
end
local queries = {
    {"Health", function() return chthonic.query("Health") end},
    {"Health+Mana", function() return chthonic.query("Health", "Mana") end},
    {"Mana+Health", function() return chthonic.query("Mana", "Health") end},
    {"Character+Health", function() return chthonic.query("Character", "Health") end},
    {"Npc+Health", function() return chthonic.query("Npc", "Health") end},
    {"Spawn", function() return chthonic.query("Spawn") end},
    {"all", function() return chthonic.query() end},
    {"Unused", function() return chthonic.query("Unused") end},
    {"Unused+Health", function() return chthonic.query("Unused", "Health") end},
    {"Transform+Mana", function() return chthonic.query("Transform", "Mana") end},
}
local tick = 0
return { update = function()
    tick = tick + 1
    for _, q in ipairs(queries) do chthonic.log("q" .. tick .. " " .. q[1] .. ": [" .. table.concat(list(q[2]()), ",") .. "]") end
    if tick ~= 2 then return end
    local all, seen = list(chthonic.query("Npc", "Health")), {}
    for e in chthonic.query("Npc", "Health") do
        seen[#seen + 1] = e
        if #seen == 1 then chthonic.destroy(all[#all]) end
    end
    chthonic.log("q2 midloop: [" .. table.concat(seen, ",") .. "]")
end })");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "query pools: loads");
    auto& reg = w.world.registry();
    const auto add = [&](entt::entity e, const char* name) { lua::pool(reg, name).emplace(e, lua::ComponentData{Json::object()}); };
    const entt::entity a = w.spawn(1, 201, "Alice", 1000, 1000), b = w.spawn(2, 202, "Bob", 1001, 1000);
    std::vector<entt::entity> npc;
    for (int i = 0; i < 30; ++i) {
        npc.push_back(w.world.create_npc("N", "T", 1, proto::Vec3{1000.0f + i, 0, 1010}, 0));
        if (i % 10 == 3) add(npc.back(), "Health");
        if (i % 3 == 0) add(npc.back(), "Mana");
    }
    reg.emplace<world::SpawnRef>(npc[5]);
    reg.emplace<world::SpawnRef>(npc[17]);
    add(a, "Health");
    // Not in the world: Transform but no Character/Npc, and no Transform at all.
    const entt::entity loose = reg.create(), bare = reg.create();
    reg.emplace<world::Transform>(loose);
    for (auto e : {loose, bare}) {
        add(e, "Health");
        add(e, "Mana");
    }

    const auto ids = [&](std::vector<entt::entity> es) {
        std::vector<uint32_t> v;
        for (auto e : es) v.push_back(wire(e));
        std::ranges::sort(v);
        std::string s = "[";
        for (auto id : v) s += (s.size() > 1 ? "," : "") + std::to_string(id);
        return s + "]";
    };
    const auto npcs = [&](auto keep) {
        std::vector<entt::entity> out;
        for (int i = 0; i < 30; ++i)
            if (keep(i)) out.push_back(npc[i]);
        return out;
    };
    const auto expect = [&](int tick, const std::string& names, const std::string& want) {
        const std::string line = "q" + std::to_string(tick) + " " + names + ": " + want;
        check(said(line), "query pools: " + line);
    };
    const auto both = [](std::vector<entt::entity> v, entt::entity e) { v.push_back(e); return v; };

    // Health (4) is smaller than Mana (12) here.
    w.world.tick(T0);
    const auto health1 = npcs([](int i) { return i % 10 == 3; });
    expect(1, "Health", ids(both(health1, a)));
    expect(1, "Health+Mana", ids({npc[3]}));
    expect(1, "Mana+Health", ids({npc[3]}));
    expect(1, "Character+Health", ids({a}));
    expect(1, "Npc+Health", ids(health1));
    expect(1, "Spawn", ids({npc[5], npc[17]}));
    expect(1, "all", ids(both(both(npcs([](int) { return true; }), a), b)));
    expect(1, "Unused", "[]");
    expect(1, "Unused+Health", "[]");
    expect(1, "Transform+Mana", ids(npcs([](int i) { return i % 3 == 0; })));

    // Health (19) is now larger than Mana (12); npc[3] is removed and re-added, so it is last in its pool.
    lua::pool(reg, "Health").remove(npc[3]);
    add(npc[3], "Health");
    for (int i = 0; i < 30; i += 2) add(npc[i], "Health");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    const auto health2 = npcs([](int i) { return i % 2 == 0 || i % 10 == 3; });
    const auto healthMana2 = npcs([](int i) { return i % 3 == 0 && (i % 2 == 0 || i % 10 == 3); });
    expect(2, "Health", ids(both(health2, a)));
    expect(2, "Health+Mana", ids(healthMana2));
    expect(2, "Mana+Health", ids(healthMana2));
    expect(2, "Npc+Health", ids(health2));
    auto midloop = health2;
    std::ranges::sort(midloop, {}, wire);
    midloop.pop_back(); // the last one, destroyed on the first iteration
    check(said("q2 midloop: " + ids(midloop)), "query pools: an entity destroyed mid-loop is skipped");
    check(!reg.valid(*std::ranges::max_element(health2, {}, wire)), "query pools: the last NPC was destroyed");
    for (const auto& s : w.world.script_status()) check(s.ok, "query pools: system still enabled " + s.error);
}

// MAX_NPCS: create past the cap raises and creates nothing; a destroy frees a slot.
void checkNpcCap() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/cap.lua", std::string(kLuaExpect) + R"lua(return { init = function()
    local a = chthonic.create("A", {template = "T"})
    chthonic.create("B", {template = "T"})
    raises("chthonic.create: too many NPCs (MAX_NPCS is 2)", function() chthonic.create("C", {template = "T"}) end)
    local n = 0 ; for _ in chthonic.query("Npc") do n = n + 1 end
    expect(n == 2, "the refused create made nothing")
    chthonic.destroy(a)
    chthonic.create("C", {template = "T"})
    chthonic.log("cap done")
end, update = function() end })lua");
    world::WorldRules rules;
    rules.maxNpcs = 2;
    TestWorld w({}, rules);
    w.spawn(1, 1, "Watcher", 1024, 1024); // the NPCs init creates at the spawn point are awake: chthonic.query sees them
    check(w.world.load_scripts(r.root).empty(), "npc cap: loads (create works from init)");
    check_lua_expectations("npc cap", "cap done");

    LuaRoot zero;
    zero.write("systems/z.lua", "return { init = function() chthonic.create('A', {template = 'T'}) end, update = function() end }");
    rules.maxNpcs = 0;
    TestWorld z({}, rules);
    const auto errors = z.world.load_scripts(zero.root);
    check(errors.size() == 1 && contains(errors[0], "systems/z.lua:1: chthonic.create: too many NPCs (MAX_NPCS is 0)"),
          "MAX_NPCS 0: create raises at the call: " + (errors.empty() ? "" : errors[0]));
}

// replication_system batching: 3 moving players, 2 still ones and a script-moved NPC, all in view of each other, plus
// a lone player far off. One tick: each in-view player gets exactly one EntityMoves holding every other mover's move
// (never its own); the lone one gets none.
void checkEntityMovesBatch() {
    LuaRoot r;
    r.write("systems/npc.lua", R"lua(local g
return {
    init = function()
        g = chthonic.create("Walker", {template = "Walker", position = {x = 1005, y = 0, z = 1005}})
        chthonic.subscribe("move", function() chthonic.set(g, "Transform", {position = {x = 1006, y = 0, z = 1005}}) end)
    end,
    update = function() end,
})lua");
    TestWorld w(r.defs().defs);
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "batch: loads" + (errors.empty() ? "" : ": " + errors[0]));
    const auto npc = w.world.registry().view<world::Npc>().front();
    std::vector<entt::entity> players;
    for (uint32_t sid = 1; sid <= 5; ++sid)
        players.push_back(w.spawn(sid, 300 + sid, "P" + std::to_string(sid), 1000.f + static_cast<float>(sid), 1000));
    w.spawn(6, 306, "Lone", 1800, 1800);
    w.world.tick(T0); // everyone sees everyone near (1000, 1000)
    w.sent.clear();
    for (size_t i = 0; i < 3; ++i) // players 1-3 move, 4-5 stand
        w.move(players[i], proto::Move{{1000.f + static_cast<float>(i + 1), 0, 1001}, 1, static_cast<uint32_t>(i + 1)});
    w.world.bus().publish("move", Json());
    w.world.tick(T0 + std::chrono::seconds(1)); // budget for the 1.4 m moves

    std::vector<uint32_t> movers = {wire(players[0]), wire(players[1]), wire(players[2]), wire(npc)};
    for (uint32_t sid = 1; sid <= 5; ++sid) {
        const auto msgs = w.sent.of<proto::EntityMoves>(sid);
        std::vector<uint32_t> want, got;
        for (uint32_t id : movers)
            if (id != wire(players[sid - 1])) want.push_back(id);
        if (msgs.size() == 1)
            for (const auto& e : msgs[0].entries) got.push_back(e.entityId);
        std::ranges::sort(got);
        std::ranges::sort(want);
        check(msgs.size() == 1, "batch: player " + std::to_string(sid) + " gets exactly one EntityMoves (got " +
                                    std::to_string(msgs.size()) + ")");
        std::string ids;
        for (uint32_t id : got) ids += " " + std::to_string(id);
        check(got == want, "batch: player " + std::to_string(sid) + "'s EntityMoves holds every other mover's move, never its own:" + ids);
    }
    const auto moves = w.sent.moves(4);
    const auto p1 = std::ranges::find(moves, wire(players[0]), &proto::EntityMoves::Entry::entityId);
    const auto n = std::ranges::find(moves, wire(npc), &proto::EntityMoves::Entry::entityId);
    check(p1 != moves.end() && p1->pos == proto::Vec3{1001, 0, 1001} && p1->heading == 1 && n != moves.end() &&
              n->pos == proto::Vec3{1006, 0, 1005},
          "batch: entries carry each mover's new position and heading");
    check(w.sent.of<proto::EntityMoves>(6).empty(), "batch: a player with no moving neighbours gets none");
}

// AOI, replication and send routing for an NPC moved by a script (driven through the bus from C++), plus hooks,
// persistence and destroy.
void checkNpcRouting() {
    LogCapture capture;
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', persist = true, replicate = 'aoi', fields = { { name = 'hp', type = 'int', default = 5 } } }");
    r.write("components/wallet.lua", "return { name = 'Wallet', persist = true, replicate = 'owner', fields = { { name = 'gold', type = 'int', default = 1 } } }");
    r.write("fsms/plain.lua", kPlain);
    r.write("systems/guard.lua", R"lua(local g
return {
    init = function()
        g = chthonic.create("Guard", {template = "GuardTpl", position = {x = 1010, y = 0, z = 1000}, heading = 0.5})
        chthonic.add(g, "Health") ; chthonic.add(g, "Wallet") ; chthonic.add(g, "StateMachine")
        chthonic.set(g, "StateMachine", {machine = "plain"})
        chthonic.subscribe("move", function(p) chthonic.set(g, "Transform", {position = {x = p.x, y = 0, z = p.z}}) end)
        chthonic.subscribe("hp", function(p) chthonic.set(g, "Health", {hp = p}) ; chthonic.set(g, "Wallet", {gold = p}) end)
        chthonic.subscribe("nearby", function() chthonic.sendNearby(g, "guard.says", "hi") ; chthonic.sendTo(g, "guard.private") end)
        chthonic.subscribe("destroy", function() chthonic.destroy(g) end)
    end,
    update = function() end,
    onEnter = function(e) chthonic.log("onEnter " .. e .. (chthonic.has(e, "Npc") and " npc" or "")) end,
    onLeave = function(e) chthonic.log("onLeave " .. e .. (chthonic.has(e, "Npc") and " npc" or "")) end,
})lua");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "npc routing: loads");
    auto& reg = w.world.registry();
    check(reg.view<world::Npc>().size() == 1, "init created the NPC");
    const auto g = reg.view<world::Npc>().front();
    const uint32_t G = wire(g);
    const auto a = w.spawn(1, 111, "Alice", 1000, 1000);
    const auto b = w.spawn(2, 112, "Bob", 1800, 1800);
    TimePoint t = T0;
    const auto tick = [&] {
        t += std::chrono::milliseconds(50);
        w.world.tick(t);
    };
    // What session sid got about the NPC, in order: "spawn;update Health;move;..."
    const auto about = [&](uint32_t sid) {
        std::string out;
        for (const auto& m : w.sent.bySession[sid]) {
            if (auto* s = std::get_if<proto::Spawn>(&m); s && s->entityId == G) out += "spawn;";
            if (auto* d = std::get_if<proto::Despawn>(&m); d && d->entityId == G) out += "despawn;";
            if (auto* mv = std::get_if<proto::EntityMoves>(&m))
                for (const auto& e : mv->entries) out += e.entityId == G ? "move;" : "";
            if (auto* u = std::get_if<proto::ComponentUpdate>(&m); u && u->entityId == G) out += "update " + u->name + ";";
            if (auto* x = std::get_if<proto::ComponentRemove>(&m); x && x->entityId == G) out += "remove " + x->name + ";";
        }
        return out;
    };

    w.sent.clear();
    tick();
    const auto spawns = w.sent.of<proto::Spawn>(1);
    const auto spawnG = std::ranges::find(spawns, G, &proto::Spawn::entityId);
    check(spawnG != spawns.end() && spawnG->kind == proto::SpawnKind::npc && spawnG->templateName == "GuardTpl" &&
              spawnG->name == "Guard" && spawnG->pos == proto::Vec3{1010, 0, 1000} && spawnG->heading == 0.5f,
          "a player in range gets Spawn{kind npc, template}");
    check(about(1) == "spawn;update Health;", "then the NPC's aoi components, never its owner ones: " + about(1));
    check(about(2).empty(), "a player out of range gets nothing about the NPC");
    check(reg.get<world::Viewers>(g).ids == std::vector<uint32_t>{wire(a)}, "the NPC's Viewers are the players that see it");
    check(!reg.all_of<world::Visible>(g), "an NPC has no Visible");

    w.sent.clear();
    for (int i = 1; i <= 3; ++i) {
        w.world.bus().publish("move", {{"x", 1010 + i}, {"z", 1000}});
        tick();
    }
    const auto moves = w.sent.moves(1);
    check(moves.size() == 3 && moves[0].pos.x == 1011 && moves[2].pos.x == 1013 && moves[2].entityId == G,
          "each script move reaches the viewer in an EntityMoves, in the same tick");
    check(about(2).empty(), "the out-of-range player gets no moves");

    w.sent.clear();
    w.world.bus().publish("hp", 9);
    tick();
    check(about(1) == "update Health;", "an aoi change on the NPC reaches the viewer; owner (Wallet) nobody: " + about(1));
    bool walletSent = false;
    for (const auto& [sid, msgs] : w.sent.bySession)
        for (const auto& m : msgs)
            if (auto* u = std::get_if<proto::ComponentUpdate>(&m)) walletSent |= u->name == "Wallet" && u->entityId == G;
    check(!walletSent, "the NPC's owner component is sent to nobody");
    check(!reg.all_of<world::Dirty>(g), "a persisted component change never marks an NPC dirty");

    w.sent.clear();
    w.world.bus().publish("nearby", nullptr);
    tick();
    const auto evA = w.sent.of<proto::ServerEvent>(1);
    check(evA.size() == 1 && evA[0].name == "guard.says", "sendNearby(npc) reaches viewing players; sendTo(npc) nothing");
    check(w.sent.of<proto::ServerEvent>(2).empty(), "sendNearby(npc) skips players out of view");

    // Out of A's range and, in one jump, into B's: the grid follows (A gets Despawn, B gets Spawn and the full set).
    w.sent.clear();
    w.world.bus().publish("move", {{"x", 1790}, {"z", 1800}});
    tick();
    check(about(1) == "move;", "the jump tick: the old viewer gets the move: " + about(1));
    w.sent.clear();
    tick();
    check(about(1) == "despawn;", "next tick: the NPC left A's view: Despawn: " + about(1));
    check(about(2) == "spawn;update Health;", "and entered B's: Spawn + aoi full set: " + about(2));
    check(reg.get<world::Viewers>(g).ids == std::vector<uint32_t>{wire(b)}, "Viewers follow");
    w.sent.clear();
    tick();
    tick();
    check(about(1).empty(), "nothing after Despawn");

    // Back to A: Spawn again.
    w.world.bus().publish("move", {{"x", 1005}, {"z", 1000}});
    tick();
    w.sent.clear();
    tick();
    check(about(1) == "spawn;update Health;", "coming back: Spawn again + full set: " + about(1));

    // Both log out: the NPC is left with no viewers; it is never saved.
    w.world.despawn(b);
    w.world.despawn(a);
    check(reg.get<world::Viewers>(g).ids.empty(), "a player's despawn removes it from the NPC's Viewers");
    check(w.sent.saves.size() == 2 && w.sent.saves[0].charId == 112 && w.sent.saves[1].charId == 111,
          "only the characters are saved");
    w.world.bus().publish("move", {{"x", 1010}, {"z", 1000}});
    tick(); // the NPC moves with nobody watching

    // Destroy: the viewer gets Despawn, then nothing; hooks never ran for the NPC.
    const auto c = w.spawn(3, 113, "Carol", 1000, 1000);
    tick();
    w.sent.clear();
    w.world.bus().publish("destroy", nullptr);
    tick();
    check(about(3) == "despawn;", "chthonic.destroy sends Despawn to every viewer: " + about(3));
    check(!reg.valid(g) && reg.view<world::Npc>().empty(), "the NPC is gone");
    check(reg.get<world::Visible>(c).ids.empty(), "and out of the viewer's Visible");
    w.sent.clear();
    tick();
    check(about(3).empty(), "nothing after the destroy's Despawn");
    check(said("onEnter " + std::to_string(wire(a))) && said("onLeave " + std::to_string(wire(a))) && !said(" npc"),
          "onEnter / onLeave fire for players, never for NPCs");
    for (const auto& s : w.world.script_status()) check(s.ok, "npc routing: system still enabled " + s.error);
}

// An NPC destroyed in the tick a player first sees it: Spawn, then Despawn, and no component data after it.
void checkNpcDestroyedOnEnter() {
    LuaRoot r;
    r.write("components/health.lua", "return { name = 'Health', replicate = 'aoi', fields = { { name = 'hp', type = 'int', default = 5 } } }");
    r.write("systems/s.lua", R"lua(local g
return { init = function() g = chthonic.create("G", {template = "T"}) ; chthonic.add(g, "Health") end,
         update = function() if g then chthonic.destroy(g) ; g = nil end end })lua");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "destroyed-on-enter: loads");
    w.spawn(1, 121, "Alice", 1024, 1024);
    w.sent.clear();
    w.world.tick(T0);
    const auto& msgs = w.sent.bySession[1];
    check(msgs.size() == 2 && std::holds_alternative<proto::Spawn>(msgs[0]) && std::holds_alternative<proto::Despawn>(msgs[1]),
          "Spawn then Despawn, no component data after it (" + std::to_string(msgs.size()) + " messages)");
}

// Graceful shutdown: the players leave first, then remove_npcs removes every NPC without a message or a save.
void checkNpcShutdown() {
    LuaRoot r;
    r.write("systems/s.lua", "return { init = function() chthonic.create('A', {template = 'T'}) ; chthonic.create('B', {template = 'T'}) end, update = function() end }");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "npc shutdown: loads");
    const auto a = w.spawn(1, 131, "Alice", 1024, 1024);
    w.world.tick(T0);
    w.world.shutdown_scripts();
    w.world.despawn(a);
    w.sent.clear();
    w.world.remove_npcs();
    check(w.world.registry().view<world::Npc>().empty(), "every NPC removed");
    check(w.sent.bySession.empty() && w.sent.saves.empty(), "silently: nothing sent, nothing saved");
}

// The sample lua/ (with lua/systems/npcs.lua): the Guard is created at the spawn point, patrols its waypoints through
// "walking" and "waiting", and a player there sees it move. 20 s of ticks.
#ifdef CHTHONIC_SAMPLE_MAPS
// The sample lua/ on the sample maps: the Guard and the Bat come from the spawn files; the Guard patrols its path; the
// test hook (systems/test_hooks.lua) destroys it for a "Respawntest..." character and it is back 5 s later.
void checkNpcSample() {
    LogCapture capture;
    const fs::path root = CHTHONIC_WORLD_LUA_SAMPLE;
    const auto defs = lua::load_component_defs(root, world::kReservedComponentNames);
    check(defs.errors.empty(), "sample definitions load");
    std::vector<std::string> mapErrors;
    auto maps = world::Maps::load(CHTHONIC_SAMPLE_MAPS, mapErrors);
    check(mapErrors.empty(), "sample maps load: " + (mapErrors.empty() ? "" : mapErrors[0]));
    TestWorld w(defs.defs, {}, std::move(maps));
    const auto errors = w.world.load_scripts(root, {"zone_cycle"});
    check(errors.empty(), "sample systems load: " + (errors.empty() ? "" : errors[0]));
    check(w.npcs().empty(), "no NPC before the first tick (no Lua creates any)");
    watch_spawns(w);
    const auto npc = [&](const std::string& name) {
        for (auto [e, n] : w.npcs())
            if (n.name == name) return e;
        return entt::entity{entt::null};
    };
    w.world.tick(T0);
    const auto g = npc("Guard"), bat = npc("Bat"), sentry = npc("Sentry");
    check(w.npcs().size() == 3 && g != entt::null && bat != entt::null && sentry != entt::null,
          "the spawn files place the Guard, the Bat and the Sentry");
    if (g == entt::null || bat == entt::null || sentry == entt::null) return;
    check(w.reg(sentry).get<world::MapRef>(sentry).map == 3, "the Sentry is on the hills (map 3)");
    check(w.reg(g).get<world::Npc>(g).templateName == "Guard" && w.reg(g).get<world::MapRef>(g).map == 1 && w.reg(g).all_of<world::SpawnRef>(g),
          "Guard, template Guard, map 1, from a spawn file");
    check(w.reg(bat).get<world::MapRef>(bat).map == 2 && w.reg(bat).get<world::Transform>(bat).pos == proto::Vec3{256, 0, 256},
          "Bat at map 2's spawn");
    w.spawn(1, 141, "Alice", 1024, 1024);
    // Their definitions (lua/npcs/): Health data, and the patrol machine for the two with a path.
    const auto health = [&](entt::entity e) { return w.value(e, "Health"); };
    check(w.reg(g).get<world::Npc>(g).npc == "Guard" && w.reg(sentry).get<world::Npc>(sentry).npc == "Sentry" && w.reg(bat).get<world::Npc>(bat).npc == "Bat",
          "each spawn's NPC is made from its definition");
    check(health(g)["current"] == 50 && health(g)["max"] == 50 && health(g)["lives"] == 3 && health(sentry)["max"] == 80 &&
              health(bat)["current"] == 10 && health(bat)["regen"] == false,
          "the definitions' Health data, unlisted fields at their defaults: " + health(g).dump());
    check(w.value(g, "StateMachine")["machine"] == "patrol" && w.value(sentry, "StateMachine")["machine"] == "patrol" &&
              !lua::pool(w.reg(bat), "StateMachine").contains(bat),
          "Guard and Sentry get the patrol machine from their definitions, the Bat none");
    std::set<std::string> states;
    float farthest = 0, maxStep = 0;
    proto::Vec3 last = w.reg(g).get<world::Transform>(g).pos;
    std::set<std::pair<int, int>> corners;
    // The Sentry (maps/hills/.server/spawns/0_1.json) patrols (150, 331) <-> (215, 331), through the mesa
    // (x 170..200, z 316..346) as the crow flies: on the hills' navmesh it walks round it, on the ground.
    float sentryEnd = 1e9f, sentryOffGround = 0, sentryInWall = 0;
    // And the crate fixture across that line at (160, 331) (maps/hills/objects/0_1.json, a 3 m box): round it too.
    map::Shape crate;
    crate.halfExtents = {1.5f, 1.5f, 1.5f};
    float sentryNearCrate = 1e9f;
    for (int i = 1; i <= 800; ++i) {
        w.world.tick(T0 + std::chrono::milliseconds(50 * i));
        const proto::Vec3 s = w.reg(sentry).get<world::Transform>(sentry).pos;
        sentryEnd = std::min(sentryEnd, std::hypot(s.x - 215, s.z - 331));
        sentryOffGround = std::max(sentryOffGround, std::fabs(s.y - *w.maps.ground(3, s.x, s.z)));
        sentryInWall = std::max(sentryInWall, std::min({s.x - 170, 200 - s.x, s.z - 316, 346 - s.z}));
        sentryNearCrate = std::min(sentryNearCrate, map::footprint_distance(crate, {160, 0, 331}, 0, s.x, s.z));
        const proto::Vec3 p = w.reg(g).get<world::Transform>(g).pos;
        farthest = std::max(farthest, std::hypot(p.x - 1024, p.z - 1024));
        maxStep = std::max(maxStep, std::hypot(p.x - last.x, p.z - last.z));
        if (std::abs(std::abs(p.x - 1024) - 10) < 1e-3f && std::abs(std::abs(p.z - 1024) - 10) < 1e-3f)
            corners.insert({int(std::lround(p.x)), int(std::lround(p.z))});
        last = p;
        states.insert(w.fsm(g).state(lua::EntityId{wire(g)}));
    }
    check(states == std::set<std::string>{"walking", "waiting"}, "the patrol machine walks and waits");
    check(corners == std::set<std::pair<int, int>>{{1034, 1034}, {1034, 1014}, {1014, 1014}, {1014, 1034}},
          "it visits the 4 points of gate_patrol (" + std::to_string(corners.size()) + ")");
    check(farthest > 10 && farthest < 15, "it patrols around the spawn point (farthest " + std::to_string(farthest) + " m)");
    check(maxStep <= 3 * 0.05f + 1e-3f, "at 3 m/s (largest step " + std::to_string(maxStep) + " m)");
    check(w.reg(bat).get<world::Transform>(bat).pos == proto::Vec3{256, 0, 256}, "the Bat (no path) never moves");
    check(sentryEnd < 1 && sentryInWall < 0.5f && sentryOffGround < 0.01f,
          std::format("the Sentry reaches (215, 331) round the mesa ({:.2f} m off; at most {:.2f} m into its 3 m wall's "
                      "foot), on the ground ({:.3f} m off)", sentryEnd, sentryInWall, sentryOffGround));
    check(sentryNearCrate > 0.2f, std::format("the Sentry walks round the obstacle crate at (160, 331) (closest {:.2f} m to its edge)",
                                              sentryNearCrate));
    const auto spawns = w.sent.of<proto::Spawn>(1);
    check(std::ranges::any_of(spawns, [](const proto::Spawn& s) { return s.kind == proto::SpawnKind::npc && s.templateName == "Guard"; }),
          "the player at the spawn point gets the Guard's Spawn");
    {
        const auto& toA = w.sent.bySession[1];
        size_t spawnAt = SIZE_MAX, healthAt = SIZE_MAX;
        for (size_t i = 0; i < toA.size(); ++i) {
            if (const auto* m = std::get_if<proto::Spawn>(&toA[i]); m && m->entityId == wire(g) && spawnAt == SIZE_MAX) spawnAt = i;
            if (const auto* u = std::get_if<proto::ComponentUpdate>(&toA[i]);
                u && u->entityId == wire(g) && u->name == "Health" && healthAt == SIZE_MAX) {
                healthAt = i;
                check(Json::parse(u->json)["current"] == 50, "the Guard's Health from its definition: " + u->json);
            }
        }
        check(spawnAt != SIZE_MAX && healthAt != SIZE_MAX && spawnAt < healthAt,
              "the player gets the Guard's Spawn, then its Health (aoi) in the AOI-enter full set");
    }
    size_t moves = 0;
    for (const auto& m : w.sent.moves(1)) moves += m.entityId == wire(g);
    check(moves > 400, "and its moves (" + std::to_string(moves) + ")");

    // The test hook: a "Respawntest..." character destroys the Guard; respawnSec 5 brings it back.
    const TimePoint t = T0 + std::chrono::milliseconds(50 * 801);
    w.world.tick(t);
    db::CharRow row{142, 1, "RespawntestAbc", 1, {1024, 0, 1024}, 0, ""};
    w.world.spawn(2, row, t);
    check(!w.world.registry_of(g) && npc("Guard") == entt::null, "test hook: the Guard is destroyed on entering");
    w.world.tick(t + std::chrono::milliseconds(4950));
    check(npc("Guard") == entt::null, "test hook: not back after 4.95 s");
    w.world.tick(t + std::chrono::milliseconds(5000));
    const auto again = npc("Guard");
    check(again != entt::null && std::hypot(w.reg(again).get<world::Transform>(again).pos.x - 1024, w.reg(again).get<world::Transform>(again).pos.z - 1024) < 0.2f,
          "test hook: back at its spawn point after 5 s");
    for (const auto& s : w.world.script_status()) check(s.ok, "sample system enabled: " + s.path + " " + s.error);
    check(!said("FAIL") && log_index("\"level\":\"error\"") == SIZE_MAX, "no Lua errors");
}
#endif

// --- NPC spawns (docs/world/npcs.html#npc-spawns) ---

void checkSpawnParsing() {
    std::string err;
    const auto good = map::parse_spawns(Json::parse(R"({"version": 2,
        "paths": {"p": {"loop": false, "wait": 1.5, "points": [[1, 2, 3], [4, 5, 6]]}, "q": {"points": [[0, 0, 0]]}},
        "spawns": [{"id": "a", "npc": "Guard", "pos": [300, 1, 260], "heading": 2, "respawnSec": 30,
                    "path": "p"},
                   {"id": "b", "npc": "Bat", "pos": [256, 0, 256], "respawnSec": -1}]})"),
                                        {1, 1}, err);
    check(good && good->size() == 2, "spawn file parses: " + err);
    if (good && good->size() == 2) {
        const auto& a = (*good)[0];
        check(a.id == "a" && a.npc == "Guard" && a.file.empty() && a.pos == std::array<float, 3>{300, 1, 260} &&
                  a.heading == 2 && a.respawnSec == 30 && a.path && a.path->points.size() == 2 &&
                  a.path->points[1] == std::array<float, 3>{4, 5, 6} && a.path->wait == 1.5f && !a.path->loop,
              "every spawn field and its path");
        const auto& b = (*good)[1];
        check(b.npc == "Bat" && b.heading == 0 && b.respawnSec == -1 && !b.path, "heading defaults to 0; no path");
    }
    const auto q = map::parse_spawns(Json::parse(R"({"version":2,"paths":{"q":{"points":[[0,0,0]]}},
        "spawns":[{"id":"c","npc":"T","pos":[1,0,1],"respawnSec":0,"path":"q"}]})"), {0, 0}, err);
    check(q && (*q)[0].path->wait == 0 && (*q)[0].path->loop, "path wait defaults to 0, loop to true");
    check(map::parse_spawns(Json::parse(R"({"version":2,"spawns":[]})"), {0, 0}, err).has_value(), "no spawns is fine");
    check(map::parse_spawns(Json::parse(R"({"version":2,"spawns":[{"id":"e","npc":"T","pos":[0,0,0],"respawnSec":0}]})"),
                            {0, 0}, err).has_value(), "the tile's low corner is inside it");

    const std::string s = R"("npc":"T","pos":[10,0,10],"respawnSec":5)";
    const std::pair<std::string, const char*> bad[] = {
        {R"([1])", "must be a JSON object"},
        {R"({"spawns":[]})", "\"version\" must be 2"},
        {R"({"version":3,"spawns":[]})", "\"version\" must be 2"},
        {R"({"version":1,"spawns":[]})", "\"version\" 1 is no longer supported: use 2, with \"npc\""},
        {R"({"version":2})", "\"spawns\" must be a list"},
        {R"({"version":2,"paths":[],"spawns":[]})", "\"paths\" must be an object"},
        {R"({"version":2,"paths":{"p":{"points":[]}},"spawns":[]})", "path \"p\": \"points\" must be a non-empty list"},
        {R"({"version":2,"paths":{"p":{"points":[[1,2]]}},"spawns":[]})", "path \"p\": \"points\" must be"},
        {R"({"version":2,"paths":{"p":{"points":[[1,2,3]],"wait":-1}},"spawns":[]})", "path \"p\": \"wait\" must be a number >= 0"},
        {R"({"version":2,"paths":{"p":{"points":[[1,2,3]],"loop":1}},"spawns":[]})", "path \"p\": \"loop\" must be true or false"},
        {R"({"version":2,"spawns":[7]})", "spawns[0]: must be an object"},
        {R"({"version":2,"spawns":[{)" + s + "}]}", "spawns[0]: \"id\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"",)" + s + "}]}", "spawns[0]: \"id\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"a",)" + s + R"(},{"id":"a",)" + s + "}]}", "spawn \"a\": duplicate id"},
        {R"({"version":2,"spawns":[{"id":"a","pos":[10,0,10],"respawnSec":5}]})", "spawn \"a\": \"npc\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"","pos":[10,0,10],"respawnSec":5}]})", "spawn \"a\": \"npc\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"a","npc":3,"pos":[10,0,10],"respawnSec":5}]})", "spawn \"a\": \"npc\" must be a non-empty string"},
        {R"({"version":2,"spawns":[{"id":"a","npc":")" + std::string(256, 'x') + R"(","pos":[10,0,10],"respawnSec":5}]})",
         "\"npc\" must be a non-empty string of at most 255 bytes"},
        {R"({"version":2,"spawns":[{"id":"a","name":"Guard",)" + s + "}]}", "spawn \"a\": unknown key \"name\""},
        {R"({"version":2,"spawns":[{"id":"a","template":"Guard",)" + s + "}]}", "spawn \"a\": unknown key \"template\""},
        {R"({"version":2,"spawns":[{"id":"a","components":{},)" + s + "}]}", "spawn \"a\": unknown key \"components\""},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[10,0],"respawnSec":5}]})", "spawn \"a\": \"pos\" must be [x, y, z]"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[256,0,10],"respawnSec":5}]})",
         "spawn \"a\": \"pos\" is outside tile 0_0"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[-0.5,0,10],"respawnSec":5}]})", "\"pos\" is outside tile 0_0"},
        {R"({"version":2,"spawns":[{"id":"a","heading":"n",)" + s + "}]}", "spawn \"a\": \"heading\" must be a number"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[10,0,10]}]})", "spawn \"a\": \"respawnSec\" must be a number"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[10,0,10],"respawnSec":-2}]})", "\"respawnSec\" must be a number in [0, 31536000]"},
        {R"({"version":2,"spawns":[{"id":"a","npc":"T","pos":[10,0,10],"respawnSec":31536001}]})", "\"respawnSec\" must be"},
        {R"({"version":2,"spawns":[{"id":"a","path":"nope",)" + s + "}]}", "spawn \"a\": unknown path \"nope\""},
        {R"({"version":2,"spawns":[{"id":"a","path":3,)" + s + "}]}", "spawn \"a\": unknown path 3"},
    };
    for (const auto& [text, want] : bad) {
        err.clear();
        const bool rejected = !map::parse_spawns(Json::parse(text), {0, 0}, err);
        check(rejected && contains(err, want), "spawn file rejected: want [" + std::string(want) + "] got [" + err + "]");
    }
}

void checkSpawnLoading() {
    LuaRoot r;
    const auto one = [](const std::string& pos) {
        return R"({"version":2,"spawns":[{"id":"x","npc":"T","pos":[)" + pos + R"(],"respawnSec":5}]})";
    };
    r.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,1]],"spawn":{"pos":[10,0,10]}})");
    r.write("a/.server/spawns/0_0.json", one("10,0,10"));
    r.write("a/.server/spawns/-1_0.json", one("-10,0,10"));
    r.write("a/.server/spawns/1_0.json", one("300,0,10"));
    r.write("a/.server/spawns/1_1.json", one("10,0,10"));
    r.write("a/.server/spawns/0_1.json", R"({"version":2,"spawns":[{"id":"y","npc":"T","pos":[10,0,300],"respawnSec":5}]})");
    r.write("a/.server/spawns/notes.txt", "hi");
    r.write("a/.server/spawns/0_0_old.json", one("10,0,10"));
    r.write("b/map.json", R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    r.write("b/.server/spawns/0_0.json", one("10,0,10")); // same id, other map: fine
    std::vector<std::string> errors;
    const auto maps = world::Maps::load(r.root, errors);
    const std::set<std::string> got(errors.begin(), errors.end());
    const std::set<std::string> want = {
        "maps/a/.server/spawns/-1_0.json: tile is not on the map",
        "maps/a/.server/spawns/1_0.json: spawn \"x\": duplicate id on this map",
        "maps/a/.server/spawns/1_1.json: spawn \"x\": \"pos\" is outside tile 1_1",
        "maps/a/.server/spawns/notes.txt: file name must be <tx>_<tz>.json",
        "maps/a/.server/spawns/0_0_old.json: file name must be <tx>_<tz>.json",
    };
    std::string all;
    for (const auto& e : errors) all += e + "; ";
    check(got == want, "every bad spawn file reported: " + all);
    check(maps.info(1).spawns.size() == 2 && maps.info(1).spawns[0].id == "x" && maps.info(1).spawns[1].id == "y" &&
              maps.info(2).spawns.size() == 1,
          "the good spawns load, in file order");

    LuaRoot none;
    none.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    errors.clear();
    check(world::Maps::load(none.root, errors).info(1).spawns.empty() && errors.empty(), "no .server/spawns: no spawns, no error");
}

// The definitions spawn_maps' spawns name: Guard (template GuardTpl) and Bat (template BatTpl).
void write_spawn_npcs(LuaRoot& r) {
    r.write("npcs/guard.lua", "return {name = 'Guard', template = 'GuardTpl'}");
    r.write("npcs/bat.lua", "return {name = 'Bat', template = 'BatTpl'}");
}

// test_maps with spawn "a" (respawn 30 s, a one-point path) and "b" (never respawns, no path) on map 1.
world::Maps spawn_maps() {
    world::Maps maps = test_maps();
    map::MapInfo start = maps.info(1);
    start.spawns = {
        {"a", "Guard", {1024, 0, 1024}, 0.5f, 30, map::PatrolPath{{{1030, 0, 1024}}, 1, false}, "spawns/a.json"},
        {"b", "Bat", {1030, 5000, 1030}, 0, -1, std::nullopt, "spawns/b.json"},
    };
    maps.add(start);
    return maps;
}

void checkRespawn() {
    LogCapture capture;
    LuaRoot r;
    write_spawn_npcs(r);
    TestWorld w({}, {}, spawn_maps());
    check(w.world.load_scripts(r.root).empty(), "respawn: definitions load");
    auto& reg = w.world.registry();
    const auto find = [&](const std::string& name) {
        for (auto [e, n] : reg.view<world::Npc>().each())
            if (n.name == name) return e;
        return entt::entity{entt::null};
    };
    check(reg.view<world::Npc>().empty(), "respawn: nothing before the first tick");
    watch_spawns(w);
    w.world.tick(T0);
    const auto a = find("Guard"), b = find("Bat");
    check(a != entt::null && b != entt::null, "respawn: every spawn is created on the first tick");
    if (a == entt::null || b == entt::null) return;
    check(reg.get<world::Npc>(a).templateName == "GuardTpl" && reg.get<world::Transform>(a).pos == proto::Vec3{1024, 0, 1024} &&
              reg.get<world::Transform>(a).heading == 0.5f && reg.get<world::MapRef>(a).map == 1 &&
              reg.get<world::SpawnRef>(a).record == 0 && reg.get<world::SpawnRef>(b).record == 1,
          "respawn: name, template, pos, heading, map, record");
    check(reg.get<world::Transform>(b).pos.y == 0, "respawn: pos on the ground like chthonic.create");
    const TimePoint killed = T0 + std::chrono::seconds(10);
    w.world.tick(killed);
    check(reg.view<world::Npc>().size() == 2, "respawn: alive spawns are not created again");

    w.world.destroy_npc(a);
    w.world.destroy_npc(b);
    w.world.tick(killed + std::chrono::milliseconds(29950));
    check(reg.view<world::Npc>().empty(), "respawn: not back before respawnSec");
    w.world.tick(killed + std::chrono::seconds(30));
    const auto a2 = find("Guard");
    check(a2 != entt::null && reg.get<world::Transform>(a2).pos == proto::Vec3{1024, 0, 1024} && reg.get<world::SpawnRef>(a2).record == 0,
          "respawn: back at its spawn point respawnSec (30 s) after it was destroyed");
    w.world.tick(killed + std::chrono::hours(24 * 400));
    check(reg.view<world::Npc>().size() == 1 && find("Bat") == entt::null, "respawn: respawnSec -1 never comes back");

    // MAX_NPCS: 1 of the 2 due spawns, one warning; the other once there is room.
    world::WorldRules rules;
    rules.maxNpcs = 1;
    TestWorld capped({}, rules, spawn_maps());
    capped.world.load_scripts(r.root);
    watch_spawns(capped);
    capped.world.tick(T0);
    capped.world.tick(T0 + std::chrono::milliseconds(50));
    auto& creg = capped.world.registry();
    check(creg.view<world::Npc>().size() == 1 && said_at("spawns deferred: MAX_NPCS reached").size() == 1,
          "respawn: MAX_NPCS caps the spawner, warned once");
    capped.world.destroy_npc(creg.view<world::Npc>().front());
    capped.world.tick(T0 + std::chrono::milliseconds(100));
    check(creg.view<world::Npc>().size() == 1 && creg.get<world::Npc>(creg.view<world::Npc>().front()).name == "Bat",
          "respawn: the deferred spawn is created once there is room");
}

constexpr const char* kSpawnLuaSystem = R"(
local done = false
return { update = function()
    if done then return end
    done = true
    local seen = {}
    for e in chthonic.query("Spawn") do
        expect(chthonic.has(e, "Npc") and chthonic.has(e, "Spawn"), "a Spawn entity is an NPC")
        local s = chthonic.get(e, "Spawn")
        seen[s.id] = s
    end
    local a, b = seen.a, seen.b
    expect(a and b, "query Spawn finds both spawns")
    expect(#a.path == 1 and a.path[1].x == 1030 and a.path[1].y == 0 and a.path[1].z == 1024, "path points as {x, y, z}")
    expect(a.wait == 1 and a.loop == false, "wait and loop")
    expect(a.pos.x == 1024 and a.pos.y == 0 and a.pos.z == 1024 and a.heading == 0.5, "pos and heading as in the file")
    expect(b.pos.y == 5000 and b.heading == 0, "pos is the file's, not clamped onto the ground")
    expect(b.path == nil and b.wait == nil and b.loop == nil, "no path: no path, wait or loop")
    local plain = chthonic.create("Plain", {template = "T"})
    expect(not chthonic.has(plain, "Spawn"), "chthonic.create NPCs have no Spawn")
    local n = 0
    for _ in chthonic.query("Spawn") do n = n + 1 end
    expect(n == 2, "query Spawn skips them")
    raises("has no \"Spawn\"", function() chthonic.get(plain, "Spawn") end)
    for e in chthonic.query("Spawn") do
        raises("\"Spawn\" is read-only on the server", function() chthonic.set(e, "Spawn", {id = "x"}) end)
        raises("\"Spawn\" cannot be removed", function() chthonic.remove(e, "Spawn") end)
    end
    raises("\"Spawn\" is read-only on the server", function() chthonic.add(plain, "Spawn") end)
    chthonic.log("spawn lua done")
end }
)";

void checkSpawnLua() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/spawns.lua", std::string(kLuaExpect) + kSpawnLuaSystem);
    write_spawn_npcs(r);
    TestWorld w({}, {}, spawn_maps());
    check(w.world.load_scripts(r.root).empty(), "spawn lua: loads");
    watch_spawns(w);
    w.world.tick(T0);
    check_lua_expectations("spawn lua", "spawn lua done");
    for (const auto& s : w.world.script_status()) check(s.ok, "spawn lua: system still enabled " + s.error);
}

// --- tile activation (docs/world/maps.html#tile-activation) ---

// The sample lua/ (Guard patrols, Sentry and Bat stand) plus a probe that logs the NPC names chthonic.query("Npc") returns
// whenever they change. Map 1 (tiles 0..7): the Guard on tile 4_4 patrols north from (1100, 1100) to (1100, 1180), the
// Sentry stands on 5_4 (respawnSec 2), a Bat on 7_7; map 2: a Bat on tile 0_0.
constexpr const char* kAwakeProbe = R"(local last
return { update = function()
    local names = {}
    for e in chthonic.query("Npc") do names[#names + 1] = chthonic.get(e, "Npc").name end
    table.sort(names)
    local now = table.concat(names, ",")
    if now ~= last then chthonic.log("awake npcs: " .. now .. "|") ; last = now end
end })";

world::Maps activation_maps() {
    world::Maps maps = test_maps();
    map::MapInfo start = maps.info(1);
    start.spawns = {
        {"guard", "Guard", {1100, 0, 1100}, 0, 30, map::PatrolPath{{{1100, 0, 1100}, {1100, 0, 1180}}, 1, true}, ""},
        {"sentry", "Sentry", {1300, 0, 1100}, 0, 2, std::nullopt, ""},
        {"far", "Bat", {1900, 0, 1900}, 0, 30, std::nullopt, ""},
    };
    maps.add(start);
    map::MapInfo cave = maps.info(2);
    cave.spawns = {{"cave", "Bat", {100, 0, 100}, 0, 30, std::nullopt, ""}};
    maps.add(cave);
    return maps;
}

void checkTileActivation() {
    LogCapture capture;
    LuaRoot r;
    const fs::path sample = CHTHONIC_WORLD_LUA_SAMPLE;
    for (const char* dir : {"components", "fsms", "npcs", "systems"}) fs::copy(sample / dir, r.root / dir, fs::copy_options::recursive);
    r.write("systems/zz_probe.lua", kAwakeProbe);
    const auto defs = lua::load_component_defs(r.root, world::kReservedComponentNames);
    TestWorld w(defs.defs, {}, activation_maps());
    check(defs.errors.empty() && w.world.load_scripts(r.root).empty(), "activation: loads");
    auto& reg = w.world.registry();
    auto& caveReg = w.world.registry(2); // each map has its own context and registry
    TimePoint t = T0;
    const auto run = [&](double seconds) {
        for (int i = 0; i < static_cast<int>(std::lround(seconds * 20)); ++i) w.world.tick(t += std::chrono::milliseconds(50));
    };
    // The NPC of a spawn record (Maps order: map 1's guard 0, sentry 1, far 2, then the cave's 3), or null.
    const auto npc = [&](uint32_t record) {
        for (auto* r : {&reg, &caveReg})
            for (auto [e, ref] : r->view<world::SpawnRef>().each())
                if (ref.record == record) return e;
        return entt::entity{entt::null};
    };
    const auto count = [&](uint32_t record) {
        size_t n = 0;
        for (auto* r : {&reg, &caveReg})
            for (auto [e, ref] : r->view<world::SpawnRef>().each()) n += ref.record == record;
        return n;
    };
    const auto npcs = [&] { return reg.view<world::Npc>().size() + caveReg.view<world::Npc>().size(); };
    const auto asleep = [&](entt::entity e) { return w.reg(e).all_of<world::Asleep>(e) && !w.reg(e).all_of<world::Awake>(e); };
    const auto awake = [&](entt::entity e) { return w.reg(e).all_of<world::Awake>(e) && !w.reg(e).all_of<world::Asleep>(e); };

    run(5);
    check(npcs() == 0, "activation: no player online: no spawn-file NPC, however long it runs");

    // Alice on tile 4_4: the Guard's tile and the Sentry's (5_4, a neighbour) load at her login; the Bat on 7_7 (3
    // tiles off) and the cave's do not.
    const auto alice = w.spawn(1, 61, "Alice", 1100, 1150);
    const auto guard = npc(0), sentry = npc(1);
    check(guard != entt::null && sentry != entt::null && npc(2) == entt::null && npc(3) == entt::null &&
              npcs() == 2,
          "activation: a login spawns its tile's and the neighbours' NPCs at once, no farther");
    if (guard == entt::null || sentry == entt::null) return;
    check(awake(guard) && awake(sentry), "activation: NPCs near a player are awake");
    run(3);
    check(logged("awake npcs: Guard,Sentry|"), "activation: chthonic.query returns the awake NPCs");
    const proto::Vec3 walked = reg.get<world::Transform>(guard).pos;
    check(walked.z > 1100.5f && w.fsm(guard).state(lua::EntityId{wire(guard)}) == "walking",
          "activation: the awake Guard patrols (waited 1 s at its first point, now walking north)");

    // Alice goes to the cave: map 1's tiles sleep. Nothing despawns; the cave's Bat loads.
    clear_log();
    check(w.world.transfer(alice, {2, {100, 0, 120}, 0}), "activation: transfer to the cave");
    const proto::Vec3 slept = reg.get<world::Transform>(guard).pos;
    run(3);
    const auto caveBat = npc(3);
    check(caveBat != entt::null && awake(caveBat) && asleep(guard) && asleep(sentry) && npcs() == 3,
          "activation: the tiles left behind sleep (nothing despawns); the cave's tile loads");
    check(logged("awake npcs: Bat|") && !logged("awake npcs: Bat,Guard"), "activation: chthonic.query skips sleeping NPCs");
    check(reg.get<world::Transform>(guard).pos == slept && w.fsm(guard).state(lua::EntityId{wire(guard)}) == "walking",
          "activation: a sleeping Guard does not patrol; its state machine stays where it was");

    // The sleeping Sentry is destroyed: it comes back respawnSec (2 s) later, asleep, still on a sleeping tile.
    w.world.destroy_npc(sentry);
    run(1.95);
    check(npc(1) == entt::null, "activation: a destroyed sleeping NPC is not back before respawnSec");
    run(0.05);
    const auto sentry2 = npc(1);
    check(sentry2 != entt::null && asleep(sentry2), "activation: it respawns on schedule while its tile sleeps, asleep");

    // Back on map 1: the same Guard wakes and walks on north from where it stopped; nothing spawns twice.
    clear_log();
    check(w.world.transfer(alice, {1, {1100, 0, 1150}, 0}), "activation: transfer back");
    run(0.05);
    check(awake(guard) && awake(sentry2) && asleep(caveBat), "activation: coming back wakes the tiles; the cave sleeps");
    run(1);
    const proto::Vec3 woke = reg.get<world::Transform>(guard).pos;
    check(woke.z > slept.z + 2 && std::fabs(woke.x - 1100) < 0.01f && w.fsm(guard).state(lua::EntityId{wire(guard)}) == "walking",
          "activation: the Guard goes on from where it was (still north, not back to its first point)");
    check(logged("awake npcs: Guard,Sentry|"), "activation: chthonic.query sees them again");
    check(count(0) == 1 && count(1) == 1 && count(2) == 0 && count(3) == 1 && npcs() == 3,
          "activation: leaving and coming back spawns nothing twice");

    // chthonic.create NPCs sleep and wake the same way.
    const auto far = w.world.create_npc("Far", "T", 1, proto::Vec3{1900, 0, 1900}, 0);
    const auto close = w.world.create_npc("Close", "T", 1, proto::Vec3{1150, 0, 1150}, 0);
    check(asleep(far) && awake(close), "activation: a script NPC far from players sleeps, one near is awake");
    reg.get<world::Transform>(far).pos = {1200, 0, 1150};
    reg.emplace_or_replace<world::Moved>(far);
    run(0.05);
    check(awake(far), "activation: an NPC moved onto a near tile wakes");

    // Logout: every tile around Alice sleeps.
    w.world.despawn(alice);
    check(asleep(guard) && asleep(sentry2) && asleep(close) && asleep(far), "activation: the last player leaving puts its tiles to sleep");

    // ACTIVATION_TILES 2: two tiles out load too.
    world::WorldRules two;
    two.activationTiles = 2;
    TestWorld w2(defs.defs, two, activation_maps());
    check(w2.world.load_scripts(r.root).empty(), "activation 2: loads");
    w2.spawn(1, 62, "Bob", 1300, 1300); // tile 5_5: the Bat on 7_7 is 2 tiles off
    check(w2.world.registry().view<world::Npc>().size() == 3, "activation 2: ACTIVATION_TILES 2 loads tiles 2 off");
}

// ClientEvent in (chthonic.world.send): "client.<name>" {entity, data} on the bus after this tick's update, in arrival
// order; past 10 a second per character the rest are dropped and counted (ClientEventLimit).
void checkClientEventsIn() {
    LogCapture capture;
    LuaRoot r;
    r.write("components/report.lua", kReport);
    r.write("systems/inbox.lua", R"(
local got = {}
return {
    init = function()
        chthonic.subscribe("client.combat.attack", function(e)
            got[#got + 1] = string.format("%d %s %s", e.entity, tostring(e.data.target), chthonic.get(e.entity, "Character").name)
        end)
    end,
    onEnter = function(e) chthonic.add(e, "Report") end,
    update = function(dt) for e in chthonic.query("Report") do chthonic.set(e, "Report", {lines = got}) end end,
})");
    TestWorld w(r.defs().defs);
    check(w.world.load_scripts(r.root).empty(), "client events: system loads");
    const auto alice = w.spawn(1, 31, "Alice", 1000, 1000);
    for (int i = 0; i < 15; ++i) w.world.push_client_event(alice, "combat.attack", Json{{"target", i}});
    w.world.tick(T0);
    w.world.tick(T0 + std::chrono::milliseconds(50)); // the report is written in the update after the drain
    Json lines = w.value(alice, "Report").value("lines", Json::array());
    const std::string id = std::to_string(wire(alice));
    check(lines.size() == 10 && lines[0] == id + " 0 Alice" && lines[9] == id + " 9 Alice",
          "client.<name> {entity = sender, data}, the first 10 a second, in order: " + lines.dump());
    auto& limit = w.world.registry().get<world::ClientEventLimit>(alice);
    check(limit.dropped == 5, "client events: the 5 over the limit are counted");
    for (int i = 20; i < 23; ++i) w.world.push_client_event(alice, "combat.attack", Json{{"target", i}});
    w.world.tick(T0 + std::chrono::seconds(1));
    w.world.tick(T0 + std::chrono::milliseconds(1050));
    lines = w.value(alice, "Report").value("lines", Json::array());
    check(lines.size() == 13 && lines[12] == id + " 22 Alice" && limit.dropped == 5,
          "client events: allowed again a second later: " + lines.dump());
    w.world.push_client_event(alice, "unheard", Json::object()); // nobody subscribed: dropped by the bus, no error
    w.world.tick(T0 + std::chrono::milliseconds(1100));
    w.world.despawn(alice);
    check(said_at("client events dropped").size() == 1, "client events: the dropped count is logged on despawn");
    for (const auto& s : w.world.script_status()) check(s.ok, "client events: system still enabled " + s.error);
}

// Move seq / epoch (protocol v6): a rejection sends one Correction{pos, the rejected seq, epoch + 1}; the rest of its
// batch and every later Move of the old epoch (in flight when the Correction went out) are dropped without another.
void checkMoveEpochs() {
    TestWorld w({});
    const auto alice = w.spawn(1, 41, "Alice", 1000, 1000);
    w.world.tick(T0);
    w.sent.clear();
    const auto& tf = w.world.registry().get<world::Transform>(alice);
    const auto& ms = w.world.registry().get<world::MoveState>(alice);
    const auto move = [&](float x, uint32_t seq, uint32_t epoch) { w.world.push_move(alice, {{x, 0, 1000}, 0, 0, seq, epoch}); };
    move(1001, 1, 0);
    w.world.tick(T0 + std::chrono::milliseconds(500));
    check(tf.pos.x == 1001 && w.sent.of<proto::Correction>(1).empty() && ms.epoch == 0, "epochs: a valid move, no Correction");
    move(1100, 2, 0); // a teleport
    move(1101, 3, 0); // sent right after it, same epoch
    w.world.tick(T0 + std::chrono::milliseconds(1000));
    auto corr = w.sent.of<proto::Correction>(1);
    check(corr.size() == 1 && corr[0].pos == proto::Vec3{1001, 0, 1000} && corr[0].ackSeq == 2 && corr[0].epoch == 1 &&
              ms.epoch == 1 && tf.pos.x == 1001,
          "epochs: a rejection -> one Correction{last accepted pos, ackSeq of the rejected move, epoch 1}; the rest of the batch dropped");
    w.sent.clear();
    move(1102, 4, 0); // in flight when the Correction went out
    move(1103, 5, 0);
    w.world.tick(T0 + std::chrono::milliseconds(1100));
    check(w.sent.of<proto::Correction>(1).empty() && tf.pos.x == 1001, "epochs: stale-epoch moves dropped silently, not applied");
    move(1003, 6, 1); // the client rebased: 1001 + the 2 m it walked since
    w.world.tick(T0 + std::chrono::milliseconds(1500));
    check(w.sent.of<proto::Correction>(1).empty() && tf.pos.x == 1003, "epochs: a move of the new epoch is accepted");
    move(1200, 7, 1);
    move(1004, 8, 0); // an older epoch than the current one in the same batch: ignored, not a second rejection
    w.world.tick(T0 + std::chrono::milliseconds(2000));
    corr = w.sent.of<proto::Correction>(1);
    check(corr.size() == 1 && corr[0].ackSeq == 7 && corr[0].epoch == 2 && corr[0].pos.x == 1003 && ms.epoch == 2,
          "epochs: the next rejection -> ackSeq 7, epoch 2");
    w.sent.clear();
    move(1004, 9, 3); // an epoch the server has not started: dropped like an older one
    w.world.tick(T0 + std::chrono::milliseconds(2500));
    check(w.sent.of<proto::Correction>(1).empty() && tf.pos.x == 1003 && ms.epoch == 2,
          "epochs: a future-epoch move dropped silently, not applied");
}

// chthonic.setMovable: a locked character's moves are all refused with one Correction a tick; unlocked, they work again.
// chthonic.map.spawn(map): the map's spawn point.
void checkMovableAndMapSpawn() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/lock.lua", std::string(kLuaExpect) + R"(
local ticks = 0
return { update = function()
    ticks = ticks + 1
    local e
    for c in chthonic.query("Character") do e = c end
    if ticks == 1 then
        chthonic.setMovable(e, false)
        local npc = chthonic.create("Statue", {template = "T"})
        raises("chthonic.setMovable: entity " .. npc .. " is an NPC; only players send moves", function() chthonic.setMovable(npc, false) end)
        raises("chthonic.setMovable: movable must be a boolean, got nil", function() chthonic.setMovable(e) end)
        raises("invalid entity id 999999", function() chthonic.setMovable(999999, true) end)
        local s = chthonic.map.spawn(2)
        expect(s.position.x == 256 and s.position.y == 0 and s.position.z == 256 and s.heading == 1, "chthonic.map.spawn")
        raises("chthonic.map.spawn: ", function() chthonic.map.spawn(99) end)
    elseif ticks == 2 then
        chthonic.setMovable(e, true)
        chthonic.log("lock lua done")
    end
end })");
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "movable: loads");
    const auto alice = w.spawn(1, 41, "Alice", 1000, 1000);
    w.world.tick(T0); // locks
    w.sent.clear();
    w.move(alice, proto::Move{{1001, 0, 1000}, 0, 0});
    w.move(alice, proto::Move{{1002, 0, 1000}, 0, 0});
    w.world.tick(T0 + std::chrono::milliseconds(500)); // refused, then unlocks
    const auto& tf = w.world.registry().get<world::Transform>(alice);
    check(tf.pos.x == 1000 && w.sent.of<proto::Correction>(1).size() == 1, "movable: locked moves -> one Correction, no move");
    w.move(alice, proto::Move{{1001, 0, 1000}, 0, 0});
    w.world.tick(T0 + std::chrono::milliseconds(1000));
    check(tf.pos.x == 1001, "movable: unlocked moves are accepted again");
    check_lua_expectations("movable", "lock lua done");
    for (const auto& s : w.world.script_status()) check(s.ok, "movable: system still enabled " + s.error);
}

// --- maps (docs/world/maps.html) ---

void checkTileMath() {
    using map::tile_of;
    check(tile_of(255.999f, 0) == map::TileKey{0, 0}, "255.999 is tile 0");
    check(tile_of(256, 256) == map::TileKey{1, 1}, "256 is tile 1");
    check(tile_of(-0.001f, -256) == map::TileKey{-1, -1}, "-0.001 and -256 are tile -1");
    check(tile_of(-256.001f, 0) == map::TileKey{-2, 0}, "-256.001 is tile -2");
    map::MapInfo m;
    m.tiles = {{0, 0, 1, 0}, {5, 5, 5, 5}};
    check(m.has_tile({1, 0}) && m.has_tile({5, 5}) && !m.has_tile({2, 0}) && !m.has_tile({0, 1}), "union of rects");
    check(m.contains(511.9f, 10) && !m.contains(512, 10) && !m.contains(-0.1f, 10), "far edge is the next tile");
    check(m.clamp(1300, 1300) == std::array<float, 2>{1300, 1300}, "clamp keeps an on-map point");
    check(m.clamp(600, 10)[0] < 512 && m.clamp(600, 10)[0] > 511.99f && m.clamp(600, 10)[1] == 10,
          "clamp pulls an off-map point to the nearest declared tile");
    const auto c = m.clamp(1600, 1600);
    check(c[0] < 1536 && c[1] < 1536 && m.contains(c[0], c[1]), "clamp lands on the nearest rect");
}

// map/heights.h links into the server too (stb's PNG decoder included); this covers the server's use of it.
void checkHeights() {
    std::string err;
    map::MapInfo info;
    info.heightRange = {{-10, 90}};
    std::string r16(size_t(map::kTileSamples) * map::kTileSamples * 2, '\0');
    const auto flat = map::decode_heights(r16, ".r16", err);
    check(flat && flat->size() == size_t(map::kTileSamples) * map::kTileSamples &&
              map::ground(*flat, info, 100.5f, 3.25f) == -10, "heights: a zero .r16 is flat at heightMin");
    check(!map::decode_heights(r16.substr(1), ".r16", err) && err.find("need 132098") != std::string::npos,
          "heights: a wrong-size .r16 is refused");
    check(!map::decode_heights("not a png", ".png", err) && err.rfind("png:", 0) == 0, "heights: a bad PNG is refused");
}

void checkManifests() {
    std::string err;
    const auto good = Json::parse(R"({"version": 1, "id": 7, "name": "Eastlands", "tiles": [[0,0,7,7], [-2,0,-1,0]],
        "heightRange": [-100, 500], "spawn": {"pos": [1024, 0, 1024], "heading": 0.5},
        "material": "materials/terrain_grass.mat",
        "nav": {"agentRadius": 0.4, "agentHeight": 1.8, "maxClimb": 0.5, "maxSlopeDeg": 45, "cellSize": 0.5}})");
    const auto m = map::parse_manifest(good, err);
    check(m && m->id == 7 && m->name == "Eastlands" && m->tiles.size() == 2 && m->spawn[0] == 1024 &&
              m->spawnHeading == 0.5f && m->heightRange && (*m->heightRange)[1] == 500 &&
              m->material == "materials/terrain_grass.mat" && m->nav && m->nav->maxSlopeDeg == 45,
          "full manifest parses: " + err);
    check(m && m->has_tile({-2, 0}) && !m->has_tile({-3, 0}), "negative tiles");
    const auto minimal =
        map::parse_manifest(Json::parse(R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})"), err);
    check(minimal && !minimal->heightRange && !minimal->nav && minimal->material.empty(), "heightRange/material/nav optional");

    const std::pair<const char*, const char*> bad[] = {
        {R"([1])", "must be a JSON object"},
        {R"({"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"version\" must be 1"},
        {R"({"version":2,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"version\" must be 1"},
        {R"({"version":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"id\" must be an integer in [0, 65535]"},
        {R"({"version":1,"id":65536,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"id\" must be an integer"},
        {R"({"version":1,"id":-1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})", "\"id\" must be an integer"},
        {R"({"version":1,"id":1,"tiles":[],"spawn":{"pos":[1,0,1]}})", "\"tiles\" must be a non-empty list"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0]],"spawn":{"pos":[1,0,1]}})", "tiles[0]: must be [x0, z0, x1, z1] integers"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0],[0,0,0.5,0]],"spawn":{"pos":[1,0,1]}})", "tiles[1]: must be"},
        {R"({"version":1,"id":1,"tiles":[[3,0,2,0]],"spawn":{"pos":[1,0,1]}})", "tiles[0]: x0 <= x1 and z0 <= z1 required"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]]})", "\"spawn\" must be {\"pos\": [x, y, z]"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0]}})", "\"spawn\" must be"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1],"heading":"n"}})", "\"spawn.heading\" must be a number"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[300,0,1]}})", "\"spawn.pos\" is not on a declared tile"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},"heightRange":[5,1]})", "\"heightRange\" must be [min, max]"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},"material":3})", "\"material\" must be a string"},
        {R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},"nav":{"cellSize":-1}})", "\"nav.cellSize\" must be a number >= 0"},
    };
    for (const auto& [text, want] : bad) {
        err.clear();
        const bool refused = !map::parse_manifest(Json::parse(text), err);
        check(refused && contains(err, want), std::string("manifest refused: ") + want + " (got: " + err + ")");
    }

    // Maps::load: every folder's map.json; one line per problem, naming the file.
    LuaRoot r;
    r.write("start/map.json", R"({"version":1,"id":1,"name":"start","tiles":[[0,0,7,7]],"spawn":{"pos":[1024,0,1024]}})");
    r.write("cave/map.json", R"({"version":1,"id":2,"name":"cave","tiles":[[0,0,1,1]],"spawn":{"pos":[256,0,256]}})");
    r.write(".server/map.json", "not even json"); // dot-folders are server data, never maps
    std::vector<std::string> errors;
    const auto ok = world::Maps::load(r.root, errors);
    check(errors.empty() && ok.all().size() == 2 && ok.info(2).name == "cave" && ok.has_tile(1, 2047, 0) &&
              !ok.has_tile(1, 2048, 0) && !ok.has_tile(3, 0, 0) && ok.owns(2, {1, 1}) && !ok.owns(2, {2, 1}),
          "two maps load; dot-folder skipped");
    r.write("dup/map.json", R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})");
    r.write("junk/map.json", "{not json");
    r.write("rect/map.json", R"({"version":1,"id":3,"tiles":[[1,0,0,0]],"spawn":{"pos":[1,0,1]}})");
    r.write("nospawn/map.json", R"({"version":1,"id":4,"tiles":[[0,0,0,0]]})");
    fs::create_directories(r.root / "empty");
    errors.clear();
    world::Maps::load(r.root, errors);
    const std::set<std::string> got(errors.begin(), errors.end());
    const std::set<std::string> want = {
        "maps/dup/map.json: id 2 already used by maps/cave",
        "maps/empty/map.json: missing",
        "maps/junk/map.json: not valid JSON",
        "maps/nospawn/map.json: \"spawn\" must be {\"pos\": [x, y, z], \"heading\": h}",
        "maps/rect/map.json: tiles[0]: x0 <= x1 and z0 <= z1 required",
    };
    std::string all;
    for (const auto& e : errors) all += e + "; ";
    check(got == want, "every bad map reported: " + all);
    errors.clear();
    world::Maps::load(r.root / "nowhere", errors);
    check(errors.size() == 1 && contains(errors[0], "nowhere"), "missing MAP_ROOT refused");
    LuaRoot none;
    errors.clear();
    world::Maps::load(none.root, errors);
    check(errors.size() == 1 && contains(errors[0], ": no maps"), "no maps refused");
}

// Index of the first message of type T on sid, or SIZE_MAX.
template <typename T>
size_t first_of(const Sent& sent, uint32_t sid) {
    const auto it = sent.bySession.find(sid);
    if (it == sent.bySession.end()) return SIZE_MAX;
    for (size_t i = 0; i < it->second.size(); ++i)
        if (std::holds_alternative<T>(it->second[i])) return i;
    return SIZE_MAX;
}

// Proximity chat (chat_system): say to the AOI audience, yell within CHAT_YELL_RADIUS on the same map (past the 128 m
// view radius, not past its own), emote to the say audience with its kind, and a muted character dropped with a notice.
void checkProximityChat() {
    world::WorldRules rules;
    rules.chatRateCount = 100;
    rules.chat.yellRadius = 300;
    TestWorld w({}, rules);
    const auto a = w.spawn(1, 1, "Alice", 1000, 1000);
    w.spawn(2, 2, "Bob", 1100, 1000);   // 100 m: in view
    w.spawn(3, 3, "Carol", 1250, 1000); // 250 m: past the view radius, in yell range
    w.spawn(4, 4, "Dave", 1310, 1000);  // 310 m: past the yell radius
    w.spawn(5, 5, "Eve", 300, 300, "", 2);   // map 2
    w.world.tick(T0);
    const auto heard = [&](proto::ChatKind kind, const std::string& text) {
        std::set<uint32_t> sids;
        for (uint32_t sid = 1; sid <= 5; ++sid)
            for (const auto& m : w.sent.of<proto::ChatMsg>(sid))
                if (m.kind == kind && m.fromName == "Alice" && m.text == text) sids.insert(sid);
        return sids;
    };
    w.sent.clear();
    w.world.push_say(a, {proto::ChatKind::say, "hi"});
    w.world.push_say(a, {proto::ChatKind::yell, "HEY"});
    w.world.push_say(a, {proto::ChatKind::emote, "waves"});
    w.world.tick(T0 + std::chrono::seconds(1));
    check(heard(proto::ChatKind::say, "hi") == std::set<uint32_t>{1, 2}, "say: the sender and the player in view only");
    check(heard(proto::ChatKind::yell, "HEY") == std::set<uint32_t>{1, 2, 3},
          "yell: in view and 250 m away (past the view radius), not 310 m away nor on map 2");
    check(heard(proto::ChatKind::emote, "waves") == std::set<uint32_t>{1, 2}, "emote: the say audience, kind emote");

    w.sent.clear();
    w.world.mutes().set("aLiCe", std::chrono::system_clock::now() + std::chrono::minutes(3));
    w.world.push_say(a, {proto::ChatKind::say, "muted"});
    w.world.push_say(a, {proto::ChatKind::yell, "muted"});
    w.world.push_say(a, {proto::ChatKind::emote, "muted"});
    w.world.tick(T0 + std::chrono::seconds(2));
    const auto notices = w.sent.of<proto::ChatNotice>(1);
    check(heard(proto::ChatKind::say, "muted").empty() && heard(proto::ChatKind::yell, "muted").empty() &&
              heard(proto::ChatKind::emote, "muted").empty(),
          "muted (any case of the name): say, yell and emote reach nobody");
    check(notices.size() == 3 && notices[0].code == proto::ChatNoticeCode::muted && notices[0].minutes == 3,
          "muted: a ChatNotice{muted, 3 minutes left} per message, to the sender only");
    w.world.mutes().set("Alice", std::chrono::system_clock::now() - std::chrono::seconds(1));
    w.sent.clear();
    w.world.push_say(a, {proto::ChatKind::say, "free"});
    w.world.tick(T0 + std::chrono::seconds(3));
    check(heard(proto::ChatKind::say, "free") == std::set<uint32_t>{1, 2}, "an expired mute no longer drops anything");
}

void checkMapWorld() {
    LogCapture capture;
    TestWorld w({});
    // spawn: saved map kept, unknown map / off-map position -> START_MAP spawn, 0 (before migration 003) -> START_MAP
    w.spawn(1, 1, "Onstart", 1000, 1000);
    w.spawn(2, 2, "Incave", 300, 300, "", 2);
    w.spawn(3, 3, "Lost", 1000, 1000, "", 99);
    w.spawn(4, 4, "Offcave", 1000, 1000, "", 2);
    w.spawn(5, 5, "Legacy", 900, 900, "", 0);
    const auto ok = [&](uint32_t sid) { return w.sent.of<proto::EnterWorldOk>(sid).at(0); };
    check(ok(1).map == 1 && ok(1).pos == proto::Vec3{1000, 0, 1000}, "EnterWorldOk map 1 at the saved position");
    check(ok(2).map == 2 && ok(2).pos == proto::Vec3{300, 0, 300}, "saved map 2 kept");
    check(ok(3).map == 1 && ok(3).pos == proto::Vec3{1024, 0, 1024} && ok(3).heading == 0, "unknown map 99 -> START_MAP spawn");
    check(ok(4).map == 1 && ok(4).pos == proto::Vec3{1024, 0, 1024}, "position off its map -> START_MAP spawn");
    check(ok(5).map == 1 && ok(5).pos == proto::Vec3{900, 0, 900}, "map 0 (pre-maps row) reads as START_MAP, position kept");
    check(logged("saved position not on a loaded map"), "fallback logged");
    const auto a = entt::entity{ok(1).entityId}, cave = entt::entity{ok(2).entityId};
    for (uint32_t sid = 3; sid <= 5; ++sid) w.world.despawn(entt::entity{ok(sid).entityId});
    w.sent.clear();

    // Same x/z, different maps: never see each other. A third player next to the first on map 1 does.
    w.spawn(6, 6, "Twin", 300, 300); // map 1 at the cave player's x/z
    const auto near = w.spawn(7, 7, "Near", 1010, 1000);
    w.world.tick(T0);
    const auto spawnsOf = [&](uint32_t sid) {
        std::set<uint32_t> ids;
        for (const auto& s : w.sent.of<proto::Spawn>(sid)) ids.insert(s.entityId);
        return ids;
    };
    check(spawnsOf(2).empty() && spawnsOf(6).empty(), "same x/z on different maps: no Spawn either way");
    check(spawnsOf(1) == std::set<uint32_t>{wire(near)} && spawnsOf(7) == std::set<uint32_t>{wire(a)}, "same map in range: seen");

    // Move onto an undeclared tile: Correction, not applied. A move on the map is.
    w.sent.clear();
    w.move(near, {{1015, 0, 1000}, 0, 0});
    w.world.tick(T0 + std::chrono::seconds(1));
    check(w.sent.of<proto::Correction>(7).empty() && w.world.registry().get<world::Transform>(near).pos.x == 1015, "on-map move accepted");
    w.sent.clear();
    const auto edger = w.spawn(8, 8, "Edger", 2045, 500);
    w.move(edger, {{2050, 0, 500}, 0, 0});
    w.world.tick(T0 + std::chrono::seconds(3));
    const auto corr = w.sent.of<proto::Correction>(8);
    check(corr.size() == 1 && corr[0].pos == proto::Vec3{2045, 0, 500}, "move onto undeclared tile 8 -> Correction");
    w.sent.clear();
    w.move(edger, {{2048, 0, 500}, 0, 0});
    w.world.tick(T0 + std::chrono::seconds(4));
    check(w.sent.of<proto::Correction>(8).size() == 1, "x = 2048 exactly is tile 8: Correction");
    w.world.despawn(edger);

    // Transfer A from map 1 (sees Near) to map 2 next to the cave player.
    w.sent.clear();
    check(w.world.transfer(a, {2, {310, 9000, 300}, 1.5f}), "transfer accepted");
    const auto mc = w.sent.of<proto::MapChange>(1);
    check(mc.size() == 1 && mc[0].map == 2 && mc[0].pos == proto::Vec3{310, 0, 300} && mc[0].heading == 1.5f,
          "MapChange{map, pos (y onto the ground), heading} sent at once");
    check(w.world.registry_of(a) == &w.world.registry(2) && w.reg(a).get<world::MapRef>(a).map == 2 && w.reg(a).all_of<world::Dirty>(a),
          "MapRef changed, Dirty, in map 2's context");
    w.world.tick(T0 + std::chrono::seconds(5));
    const size_t change = first_of<proto::MapChange>(w.sent, 1), despawn = first_of<proto::Despawn>(w.sent, 1),
                 spawn = first_of<proto::Spawn>(w.sent, 1);
    check(change == 0 && despawn != SIZE_MAX && spawn != SIZE_MAX && change < despawn && change < spawn,
          "mover: MapChange before the Despawn of old neighbours and the Spawn of new ones");
    check(w.sent.of<proto::Despawn>(1).size() == 1 && w.sent.of<proto::Despawn>(1)[0].entityId == wire(near), "mover: Despawn Near");
    check(w.sent.of<proto::Spawn>(1).size() == 1 && w.sent.of<proto::Spawn>(1)[0].entityId == wire(cave), "mover: Spawn the cave player");
    check(w.sent.of<proto::Despawn>(7).size() == 1 && w.sent.of<proto::Despawn>(7)[0].entityId == wire(a), "old neighbour: Despawn mover");
    check(w.sent.moves(7).empty(), "old neighbour never sees where it went");
    check(w.sent.of<proto::Spawn>(2).size() == 1 && w.sent.of<proto::Spawn>(2)[0].pos == proto::Vec3{310, 0, 300},
          "new neighbour: Spawn at the destination");
    check(spawnsOf(6).empty(), "the map-1 player at the same x/z still sees nothing");

    // A move from before the client got MapChange (old-map coordinates, the old epoch): dropped, no Correction; one of
    // MapChange's epoch from there is still validated (Correction to the new position).
    check(mc[0].epoch == 1, "MapChange carries the session's new epoch");
    w.sent.clear();
    w.world.push_move(a, {{1001, 0, 1000}, 0, 0, 1, 0}); // sent with the epoch before MapChange
    w.world.tick(T0 + std::chrono::seconds(6));
    check(w.sent.of<proto::Correction>(1).empty() &&
              w.reg(a).get<world::Transform>(a).pos == proto::Vec3{310, 0, 300},
          "stale old-map move (old epoch) dropped without a Correction");
    w.move(a, {{1001, 0, 1000}, 0, 0, 2});
    w.world.tick(T0 + std::chrono::milliseconds(6100));
    check(w.sent.of<proto::Correction>(1).size() == 1 && w.sent.of<proto::Correction>(1)[0].pos == proto::Vec3{310, 0, 300},
          "an old-map move of the new epoch is corrected");

    // Teleport within the map, next to the same neighbour: both get Despawn now and Spawn again on the next tick, so a
    // client that dropped its remotes on MapChange gets them back.
    const auto p = w.spawn(9, 9, "Porter", 1500, 1500), q = w.spawn(10, 10, "Stayer", 1510, 1500);
    w.world.tick(T0 + std::chrono::seconds(7));
    w.sent.clear();
    check(w.world.transfer(p, {1, {1505, 0, 1500}, 0}), "teleport within the map");
    check(first_of<proto::MapChange>(w.sent, 9) == 0 && w.sent.of<proto::Despawn>(9).size() == 1 &&
              w.sent.of<proto::Despawn>(10).size() == 1 && w.sent.of<proto::Despawn>(10)[0].entityId == wire(p),
          "teleport: MapChange then Despawn Stayer; Stayer gets Despawn Porter");
    w.world.tick(T0 + std::chrono::seconds(8));
    check(spawnsOf(9) == std::set<uint32_t>{wire(q)} && w.sent.of<proto::Spawn>(10).size() == 1 &&
              w.sent.of<proto::Spawn>(10)[0].pos == proto::Vec3{1505, 0, 1500},
          "teleport: both Spawn again on the next tick, at the new position");
    w.world.despawn(p);
    w.world.despawn(q);

    // Refused: not owned (undeclared tile, unknown map); nothing changes, nothing sent.
    w.sent.clear();
    check(!w.world.transfer(a, {2, {600, 0, 100}, 0}) && !w.world.transfer(a, {9, {1, 0, 1}, 0}), "unowned destination refused");
    check(w.sent.bySession.empty() && w.reg(a).get<world::MapRef>(a).map == 2, "refusal changes nothing");
    check(logged("transfer refused"), "refusal logged");

    // Save carries the map.
    w.world.despawn(a);
    check(!w.sent.saves.empty() && w.sent.saves.back().charId == 1 && w.sent.saves.back().mapId == 2, "save includes map");

    // NPCs: created on a map, transferred like players.
    w.sent.clear();
    const auto npc = w.world.create_npc("Bat", "Bat", uint16_t{2}, proto::Vec3{290, 0, 300}, 0);
    w.world.tick(T0 + std::chrono::seconds(9));
    check(spawnsOf(2).contains(wire(npc)) && !spawnsOf(6).contains(wire(npc)), "NPC on map 2 seen only there");
    w.sent.clear();
    check(w.world.transfer(npc, {1, {305, 0, 300}, 0}), "NPC transfer");
    w.world.tick(T0 + std::chrono::seconds(10));
    check(w.sent.of<proto::Despawn>(2).size() == 1 && spawnsOf(6).contains(wire(npc)), "NPC: Despawn on map 2, Spawn on map 1");
    check(w.sent.of<proto::MapChange>(2).empty() && w.sent.of<proto::MapChange>(6).empty(), "an NPC transfer sends no MapChange");
}

// chthonic.transfer and Transform.map from Lua (players and NPCs), with every argument check. Each map has its own context
// (docs/world/maps.html#map-contexts): chthonic.create and chthonic.transfer onto another map are done at the end of
// the tick, so this tick the entity is not (yet) in this map's context; the next tick map 2's copy of the system sees
// it there.
constexpr const char* kMapLuaSystem = R"lua(
local done = false
return { update = function()
    if done then return end
    local p
    for e in chthonic.query("Character") do p = e end
    if not p then return end
    done = true
    if chthonic.get(p, "Transform").map == 2 then -- map 2's copy, the tick after
        local tf = chthonic.get(p, "Transform")
        expect(tf.map == 2 and tf.position.x == 300 and tf.position.z == 310 and tf.heading == 0.25, "player moved to map 2")
        local npcs = {}
        for e in chthonic.query("Npc") do npcs[chthonic.get(e, "Npc").name] = e end
        tf = npcs.Cave and chthonic.get(npcs.Cave, "Transform")
        expect(tf and tf.map == 2 and tf.position.x == 256 and tf.position.z == 256, "create map = 2: at its spawn")
        expect(npcs.Far and chthonic.get(npcs.Far, "Transform").position.x < 512, "create clamps onto its map")
        local g = npcs.Bat
        tf = g and chthonic.get(g, "Transform")
        expect(tf and tf.map == 2 and tf.position.x == 100 and tf.position.z == 100, "chthonic.transfer NPC")
        expect(tf and tf.heading == 0, "heading defaults to the current one")
        chthonic.set(g, "Transform", {position = {x = 4000, y = 0, z = 100}})
        expect(chthonic.get(g, "Transform").position.x < 512, "NPC Transform set clamps onto its new map")
        chthonic.log("map lua done on map 2")
        return
    end
    expect(chthonic.get(p, "Transform").map == 1, "Transform.map: player on map 1")
    local g = chthonic.create("Bat", {template = "Bat"})
    expect(chthonic.get(g, "Transform").map == 1, "chthonic.create defaults to this map (START_MAP)")
    local c = chthonic.create("Cave", {template = "Bat", map = 2})
    expect(type(c) == "number", "create map = 2 returns its id now")
    raises("invalid entity id " .. c, function() chthonic.get(c, "Transform") end) -- in map 2's context, at the tick's end
    chthonic.create("Far", {template = "Bat", map = 2, position = {x = 5000, y = 0, z = 100}})
    raises('chthonic.create: "map" must be the id of a loaded map, got 9', function() chthonic.create("X", {template = "T", map = 9}) end)
    raises('chthonic.create: "map" must be the id of a loaded map, got 1.5', function() chthonic.create("X", {template = "T", map = 1.5}) end)

    expect(chthonic.transfer(p, 2, {x = 300, y = 0, z = 310}, 0.25) == true, "chthonic.transfer player returns true")
    expect(chthonic.get(p, "Transform").map == 1, "onto another map: still here until the end of the tick")
    expect(chthonic.transfer(g, 2, {x = 100, y = 0, z = 100}) and chthonic.get(g, "Transform").map == 1, "chthonic.transfer NPC")
    raises('"Transform": "map" is read-only; use chthonic.transfer', function() chthonic.set(g, "Transform", {map = 1}) end)
    raises('chthonic.transfer: "map" must be the id of a loaded map, got 9', function() chthonic.transfer(p, 9, {x = 1, y = 0, z = 1}) end)
    raises('chthonic.transfer: "map" must be the id of a loaded map, got nil', function() chthonic.transfer(p) end)
    raises('chthonic.transfer: position (600, 1) is not on a declared tile of map 2', function() chthonic.transfer(p, 2, {x = 600, y = 0, z = 1}) end)
    raises('chthonic.transfer: "position" must be a table', function() chthonic.transfer(p, 2, 5) end)
    raises('chthonic.transfer: "heading" must be a finite number', function() chthonic.transfer(p, 2, {x = 1, y = 0, z = 1}, "n") end)
    raises("chthonic.transfer: invalid entity id 999999", function() chthonic.transfer(999999, 1, {x = 1, y = 0, z = 1}) end)
    expect(chthonic.get(p, "Transform").map == 1 and chthonic.get(p, "Transform").position.x == 1000, "failed transfers changed nothing")
    chthonic.log("map lua done")
end })lua";

void checkMapLua() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/maps.lua", std::string(kLuaExpect) + kMapLuaSystem);
    TestWorld w({});
    check(w.world.load_scripts(r.root).empty(), "map lua: loads");
    w.spawn(1, 1, "Alice", 1000, 1000);
    w.sent.clear();
    w.world.tick(T0);
    const auto mc = w.sent.of<proto::MapChange>(1);
    check(mc.size() == 1 && mc[0].map == 2 && mc[0].pos == proto::Vec3{300, 0, 310}, "chthonic.transfer sends the player MapChange");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check_lua_expectations("map lua", "map lua done");
    check(said("map lua done on map 2"), "map lua: map 2's copy checked what arrived there");
    for (const auto& s : w.world.script_status()) check(s.ok, "map lua: system still enabled " + s.error);
}

// --- map contexts (docs/world/maps.html#map-contexts) ---

// test_maps plus map 3 "hills": tiles 0..1, spawn (100, 0, 100).
world::Maps three_maps() {
    world::Maps maps = test_maps();
    map::MapInfo hills;
    hills.id = 3;
    hills.name = "hills";
    hills.tiles = {{0, 0, 1, 1}};
    hills.spawn = {100, 0, 100};
    maps.add(hills);
    return maps;
}

// One context per map in use, each with its own registry, AOI grid and Lua state (a debug line each), plus the realm,
// the only one running the game machines. Entity ids are unique across the contexts: 1,000 NPCs created and destroyed
// in each of 3 (twice), and an NPC keeps its id when it moves to another context.
void checkMapContexts() {
    LogCapture capture;
    log::set_level(log::Level::debug);
    LuaRoot r;
    r.write("fsms/clock.lua", "return { name = 'clock', initial = 'on', states = { on = { enter = function() chthonic.log('clock entered') end } } }");
    r.write("systems/count.lua", "local n = 0\nreturn { update = function() n = n + 1 ; if n == 1 then chthonic.log('first update') end end }");
    TestWorld w({}, {}, three_maps());
    check(w.world.load_scripts(r.root, {"clock"}).empty(), "contexts: loads");
    check(said("realm context started") && said_at("map context started").size() == 1 &&
              contains(g_log[said_first("map context started")], R"("map":1)"),
          "contexts: the realm's and START_MAP's context start with the scripts");
    check(said_at("clock entered").size() == 1, "contexts: the game machine entered once, in the realm");
    for (const uint16_t map : {uint16_t{2}, uint16_t{3}}) w.world.create_npc("N", "T", map, std::nullopt, 0); // their contexts start on first use
    const auto started = said_at("map context started");
    check(started.size() == 3 && contains(g_log[started[1]], R"("map":2)") && contains(g_log[started[2]], R"("map":3)"),
          "contexts: one debug line per map context, each when first used");
    w.world.tick(T0);
    check(said_at("first update").size() == 3, "contexts: each map's Lua state runs its own copy of the system");
    check(said_at("clock entered").size() == 1, "contexts: no map context starts the game machines");
    check(&w.world.registry(1) != &w.world.registry(2) && &w.world.registry(2) != &w.world.registry(3) &&
              w.world.registry(1).view<world::Npc>().empty() && w.world.registry(2).view<world::Npc>().size() == 1 &&
              w.world.registry(3).view<world::Npc>().size() == 1,
          "contexts: each map's NPCs are in its own registry");
    log::set_level(log::Level::info);

    // 1,000 NPCs in each of the 3 contexts, interleaved: every id unique, on the wire (Spawn) and in the registries.
    std::vector<entt::entity> npcs;
    for (int round = 0; round < 2; ++round) {
        npcs.clear();
        for (int i = 0; i < 1000; ++i)
            for (const uint16_t map : {uint16_t{1}, uint16_t{2}, uint16_t{3}})
                npcs.push_back(w.world.create_npc("N", "T", map, proto::Vec3{float(i % 400), 0, float(i % 400)}, 0));
        std::set<uint32_t> ids;
        for (entt::entity e : npcs) ids.insert(wire(e));
        size_t inOne = 0;
        for (entt::entity e : npcs)
            inOne += (w.world.registry(1).valid(e) + w.world.registry(2).valid(e) + w.world.registry(3).valid(e)) == 1;
        check(ids.size() == 3000 && inOne == 3000, std::format("contexts: 3000 NPCs, 3000 ids, each in one context (round {})", round));
        for (entt::entity e : npcs) w.world.destroy_npc(e);
        check(w.world.registry(1).view<world::Npc>().empty() && w.world.registry(2).view<world::Npc>().size() == 1 &&
                  w.world.registry(3).view<world::Npc>().size() == 1,
              "contexts: every NPC destroyed in its own context");
    }
    // Spawns on the wire carry those ids: a player on map 2 sees the NPCs there, each id once.
    for (int i = 0; i < 100; ++i) npcs[i] = w.world.create_npc("N", "T", uint16_t(1 + i % 3), proto::Vec3{300, 0, 300}, 0);
    w.spawn(1, 1, "Ann", 300, 300, "", 2);
    w.world.tick(T0 + std::chrono::milliseconds(50));
    std::set<uint32_t> seen;
    size_t spawns = 0;
    for (const auto& s : w.sent.of<proto::Spawn>(1)) spawns += seen.insert(s.entityId).second;
    check(spawns == 34 && seen.size() == 34, "contexts: map 2's player gets a Spawn per NPC there (33 + the first), ids unique");
    const entt::entity mover = npcs[1]; // on map 2
    check(w.world.transfer(mover, {3, {100, 0, 100}, 0}) && w.world.registry_of(mover) == &w.world.registry(3) &&
              !w.world.registry(2).valid(mover),
          "contexts: an NPC moved to map 3 keeps its id, in map 3's context only");
}

// chthonic.world.post (docs/world/lua.html#post): to a map, "realm" and "all", each delivered on the next tick on
// its targets' buses, in post order, as {from, data}; from the realm too (a game machine's callback). Bad targets and
// bad data raise.
void checkWorldPost() {
    LogCapture capture;
    LuaRoot r;
    r.write("fsms/beacon.lua", R"(return { name = "beacon", initial = "off", states = {
    off = {}, on = { enter = function() chthonic.world.post("all", "beacon", {n = 6}) end } } })");
    r.write("systems/post.lua", std::string(kLuaExpect) + R"lua(
local tick = 0
local function here() -- this context's map: its one character's
    for e in chthonic.query("Character") do return chthonic.get(e, "Transform").map end
end
return {
    init = function()
        for _, name in ipairs({"ping", "pong", "beacon"}) do
            chthonic.subscribe(name, function(p)
                chthonic.log(string.format("map %s got %s from %s n %s", tostring(here()), name, tostring(p.from), tostring(p.data.n)))
            end)
        end
    end,
    update = function()
        tick = tick + 1
        if tick ~= 1 or here() ~= 1 then return end -- map 1's copy posts, once
        chthonic.world.post(2, "ping", {n = 1})
        chthonic.world.post("all", "pong", {n = 2})
        chthonic.world.post(1, "ping", {n = 3})
        chthonic.world.post("realm", "ping", {n = 4})
        chthonic.world.post(2, "ping", {n = 5})
        local target = 'chthonic.world.post: target must be a loaded map\'s id, "realm" or "all", got '
        raises(target .. "9", function() chthonic.world.post(9, "x", {}) end)
        raises(target .. "1.5", function() chthonic.world.post(1.5, "x", {}) end)
        raises(target .. "nowhere", function() chthonic.world.post("nowhere", "x", {}) end)
        raises(target .. "nil", function() chthonic.world.post(nil, "x", {}) end)
        raises("chthonic.world.post: event name must be a non-empty string, got nil", function() chthonic.world.post(1) end)
        raises("chthonic.world.post: payload.x must be a finite number", function() chthonic.world.post(1, "x", {x = 1 / 0}) end)
        local cyc = {}
        cyc.self = cyc
        raises("chthonic.world.post: payload.self: the table contains itself (cycle)", function() chthonic.world.post("all", "x", cyc) end)
        chthonic.log("posted")
    end,
}
)lua");
    TestWorld w({});
    check(w.world.load_scripts(r.root, {"beacon"}).empty(), "post: loads");
    std::vector<nlohmann::json> realm;
    auto sub = w.world.realm_bus().subscribe("ping", [&](const nlohmann::json& p) { realm.push_back(p); });
    auto sub2 = w.world.realm_bus().subscribe("pong", [&](const nlohmann::json& p) { realm.push_back(p); });
    w.spawn(1, 1, "Ann", 1000, 1000);
    w.spawn(2, 2, "Bob", 300, 300, "", 2);
    w.world.tick(T0);
    check_lua_expectations("post", "posted");
    check(said_at(" got ").empty() && realm.empty(), "post: nothing delivered in the tick it was posted");
    w.world.tick(T0 + std::chrono::milliseconds(50));
    const std::vector<size_t> map1 = {said_first("map 1 got pong from 1 n 2"), said_first("map 1 got ping from 1 n 3")};
    const std::vector<size_t> map2 = {said_first("map 2 got ping from 1 n 1"), said_first("map 2 got pong from 1 n 2"),
                                      said_first("map 2 got ping from 1 n 5")};
    check(std::ranges::is_sorted(map1) && map1.back() != SIZE_MAX && std::ranges::is_sorted(map2) && map2.back() != SIZE_MAX &&
              said_at(" got ").size() == 5,
          "post: the next tick, each map gets what was posted to it or to all, in post order, from = map 1");
    check(realm.size() == 2 && realm[0] == nlohmann::json{{"from", 1}, {"data", {{"n", 2}}}} &&
              realm[1] == nlohmann::json{{"from", 1}, {"data", {{"n", 4}}}},
          "post: the realm's bus gets \"all\" and \"realm\", in post order, as {from, data}");

    // From the realm: a game machine's enter callback posts to all.
    clear_log();
    w.world.realm_fsm().change("beacon", "on");
    w.world.tick(T0 + std::chrono::milliseconds(100));
    check(said_at(" got beacon").empty(), "post: the realm's post waits for the end of the tick");
    w.world.tick(T0 + std::chrono::milliseconds(150));
    check(said("map 1 got beacon from realm n 6") && said("map 2 got beacon from realm n 6"),
          "post: from the realm, from = \"realm\"");
    for (const auto& s : w.world.script_status()) check(s.ok, "post: system still enabled " + s.error);
}

// --- areas (docs/world/areas.html) ---

std::optional<map::Areas> areas_of(const std::string& text, std::string& err) {
    err.clear();
    return map::parse_areas(nlohmann::json::parse(text), err);
}

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : "; ") + item;
    return out;
}

std::string area_id(const map::Areas& a, float x, float y, float z) {
    const int16_t i = map::area_at(a, x, y, z);
    return i < 0 ? "-" : a.list[i].id;
}

void checkAreaShapes() {
    std::string err;
    // world: 3x3 tiles. town: an L-shaped polygon, only for 0 <= y <= 10. tower: a rect inside town's corner.
    const auto a = areas_of(R"({"version": 1, "areas": [
        {"id": "world", "rect": [0, 0, 768, 768], "flags": {"pvp": false, "noMount": true, "zone": 1}, "music": "w.ogg"},
        {"id": "town", "name": "Town", "parent": "world", "priority": 5, "y": [0, 10], "flags": {"pvp": true},
         "polygon": [[10, 10], [60, 10], [60, 30], [30, 30], [30, 60], [10, 60]]},
        {"id": "tower", "parent": "town", "priority": 9, "rect": [12, 12, 20, 20], "flags": {"zone": 3}},
        {"id": "east", "rect": [700, 0, 800, 10], "priority": 1}
    ]})", err);
    check(a.has_value(), "areas parse: " + err);
    if (!a) return;
    check(area_id(*a, 500, 0, 500) == "world" && area_id(*a, 1, 0, 1) == "world", "rect: inside");
    check(area_id(*a, 800.5f, 0, 5) == "-" && area_id(*a, -1, 0, 5) == "-" && area_id(*a, 5, 0, 900) == "-", "outside all: none");
    check(area_id(*a, 750, 0, 5) == "east", "higher priority wins where areas overlap");
    check(area_id(*a, 20, 5, 50) == "town" && area_id(*a, 50, 5, 20) == "town", "polygon: both arms of the L");
    check(area_id(*a, 50, 5, 50) == "world", "polygon: the L's notch is outside (inside its bounds)");
    check(area_id(*a, 20, 10.5f, 50) == "world" && area_id(*a, 20, -0.5f, 50) == "world" && area_id(*a, 20, 0, 50) == "town",
          "y range: above and below fall through to the parent, the bound itself is inside");
    check(area_id(*a, 15, 100, 15) == "tower", "priority: tower over town over world (no y range on tower)");
    const auto& tower = a->list[2];
    check(tower.flags == nlohmann::json{{"pvp", true}, {"noMount", true}, {"zone", 3}},
          "flags: own over parent's over grandparent's: " + tower.flags.dump());
    check(a->list[1].name == "Town" && tower.name == "tower" && tower.music.empty() && a->list[0].music == "w.ogg",
          "name defaults to id; music is not inherited");
    check(a->byTile.at({0, 0}).size() == 3 && a->byTile.at({0, 0})[0] == 2 && !a->byTile.contains({4, 4}),
          "tile buckets: highest priority first, only tiles an area touches");

    // Not overlaps: a shared edge, disjoint y ranges, different priorities.
    check(areas_of(R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10]},{"id":"b","rect":[10,0,20,10]},
        {"id":"c","polygon":[[0,10],[10,10],[5,20]]}]})", err).has_value(), "equal priority sharing an edge: fine " + err);
    check(areas_of(R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10],"y":[0,5]},{"id":"b","rect":[0,0,10,10],"y":[5,9]}]})",
                   err).has_value(), "equal priority, same rect, disjoint y: fine " + err);
    check(areas_of(R"({"version":1,"areas":[]})", err).has_value(), "no areas: fine");

    const std::pair<const char*, const char*> bad[] = {
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10]},{"id":"b","rect":[5,5,15,15]}]})",
         "areas \"a\" and \"b\" overlap with equal priority 0"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10]},{"id":"b","rect":[0,0,10,10]}]})", "overlap with equal priority"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10],"priority":3},{"id":"b","polygon":[[-5,-5],[20,-5],[5,20]],"priority":3}]})",
         "areas \"a\" and \"b\" overlap with equal priority 3"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10]},{"id":"b","rect":[2,2,4,4],"y":[0,1]}]})", "overlap with equal priority"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,10,10],"parent":"nope"}]})", "area \"a\": unknown parent \"nope\""},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"parent":"b","priority":1},{"id":"b","rect":[5,5,6,6],"parent":"a"}]})",
         "parent cycle"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1]},{"id":"a","rect":[5,5,6,6]}]})", "area \"a\": duplicate id"},
        {R"({"version":1,"areas":[{"id":"a"}]})", "area \"a\": needs exactly one of \"rect\" and \"polygon\""},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"polygon":[[0,0],[1,0],[0,1]]}]})", "needs exactly one of"},
        {R"({"version":1,"areas":[{"id":"a","rect":[5,0,1,1]}]})", "\"rect\" must be [x0, z0, x1, z1] with x0 < x1"},
        {R"({"version":1,"areas":[{"id":"a","polygon":[[0,0],[1,0]]}]})", "\"polygon\" must be a list of at least 3"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"y":[5,1]}]})", "\"y\" must be [min, max]"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"flags":[1]}]})", "\"flags\" must be an object"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1,1],"portal":{"map":2}}]})", "\"portal\" must be {\"map\": id"},
        {R"({"version":1,"areas":[{"id":"a","rect":[0,0,1e9,1]}]})", "spans more than 65536 tiles"},
        {R"({"version":1,"areas":[{"rect":[0,0,1,1]}]})", "areas[0]: \"id\" must be a non-empty string"},
        {R"({"version":2,"areas":[]})", "\"version\" must be 1"},
        {R"({"version":1})", "\"areas\" must be a list"},
    };
    for (const auto& [text, want] : bad) {
        const bool refused = !areas_of(text, err);
        check(refused && contains(err, want), std::string("areas refused: ") + want + " (got: " + err + ")");
    }
}

// Maps::load: areas.json per map, and the portals between maps.
// --- zones (docs/world/maps.html#zones) ---

void checkZones() {
    std::string err;
    // An L of tiles: row z=0 is x 0..3, plus (0,1).
    const auto map = map::parse_manifest(nlohmann::json::parse(
        R"({"version":1,"id":3,"name":"Vale","tiles":[[0,0,3,0],[0,1,0,1]],"spawn":{"pos":[10,0,10]}})"), err);
    check(map.has_value(), "zones: map parses " + err);
    if (!map) return;
    const auto zones_of = [&](const std::string& text) {
        err.clear();
        return map::parse_zones(text, *map, "vale", err);
    };

    const auto z = zones_of(R"({"version":1,"zones":[
        {"id":"east-1","name":"East","tiles":[[2,0,3,0]],"flags":{"pvp":true,"deep":{"a":[1]}},"music":"e.ogg"},
        {"id":"west_0","tiles":[[0,0,1,0],[0,1,0,1],[0,0,0,0]]}]})");
    check(z.has_value(), "zones: valid file parses: " + err);
    if (z) {
        check(!z->implicit && z->list.size() == 2 && z->list[0].name == "East" && z->list[1].name == "west_0",
              "zones: name defaults to id");
        check(z->list[0].flags == nlohmann::json{{"pvp", true}, {"deep", {{"a", {1}}}}} && z->list[1].flags.empty(),
              "zones: flags passed through");
        check(z->list[0].raw.value("music", "") == "e.ogg" && z->list[0].raw.begin().key() == "id",
              "zones: unknown keys kept in raw, in file order");
        check(map::zone_at(*z, map::TileKey{3, 0}) == 0 && map::zone_at(*z, map::TileKey{0, 1}) == 1 &&
                  map::zone_at(*z, map::TileKey{1, 0}) == 1 && map::zone_at(*z, map::TileKey{1, 1}) == -1 &&
                  map::zone_at(*z, map::TileKey{9, 9}) == -1,
              "zones: zone_at by tile; off the map's tiles -1");
        check(map::zone_at(*z, 600.f, 10.f) == 0 && map::zone_at(*z, 511.9f, 255.f) == 1 &&
                  map::zone_at(*z, 10.f, 300.f) == 1 && map::zone_at(*z, -1.f, 10.f) == -1,
              "zones: zone_at by world x, z");
    }

    const auto imp = map::implicit_zones(*map, "vale");
    check(imp.implicit && imp.list.size() == 1 && imp.list[0].id == "vale" && imp.list[0].name == "Vale" &&
              imp.byTile.size() == 5 && map::zone_at(imp, 300.f, 10.f) == 0 && map::zone_at(imp, 300.f, 300.f) == -1,
          "zones: no file -> one implicit zone of every declared tile, named after the map");
    const auto unnamed = map::parse_manifest(nlohmann::json::parse(
        R"({"version":1,"id":4,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})"), err);
    check(unnamed && map::implicit_zones(*unnamed, "bare").list[0].name == "bare", "zones: implicit name falls back to the folder");

    const std::string file = "maps/vale/zones.json: ";
    const std::pair<const char*, std::string> bad[] = {
        {"{nope", "not valid JSON"},
        {R"([])", "must be a JSON object"},
        {R"({"zones":[]})", "\"version\" must be 1"},
        {R"({"version":1})", "\"zones\" must be a list"},
        {R"({"version":1,"zones":[{"tiles":[[0,0,3,0],[0,1,0,1]]}]})", "zones[0]: \"id\" must be a string"},
        {R"({"version":1,"zones":[{"id":"a","name":5,"tiles":[[0,0,3,0],[0,1,0,1]]}]})", "zone \"a\": \"name\" must be a string"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,3]]}]})", "zone \"a\": tiles[0]: must be [x0, z0, x1, z1] integers"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[3,0,0,0]]}]})", "zone \"a\": tiles[0]: x0 <= x1 and z0 <= z1 required"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,3,0],[0,1,0,1]],"flags":[1]}]})", "zone \"a\": \"flags\" must be an object"},
        {R"({"version":1,"zones":[{"id":"a b","tiles":[[0,0,3,0],[0,1,0,1]]}]})",
         "zone \"a b\": id must be letters, digits, _ and - (not starting with -)"},
        {R"({"version":1,"zones":[{"id":"-a","tiles":[[0,0,3,0],[0,1,0,1]]}]})", "zone \"-a\": id must be letters"},
        {R"({"version":1,"zones":[{"id":"","tiles":[[0,0,3,0],[0,1,0,1]]}]})", "zone \"\": id must be letters"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,1,0]]},{"id":"a","tiles":[[2,0,3,0],[0,1,0,1]]}]})", "zone \"a\": duplicate id"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,3,1]]}]})", "zone \"a\": tile 1_1 is not declared in map.json"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,0,2000000000]]}]})", "zone \"a\": tile 0_2 is not declared"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,2,0],[0,1,0,1]]},{"id":"b","tiles":[[2,0,3,0]]}]})",
         "zones \"a\" and \"b\" overlap at tile 2_0"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,2,0],[0,1,0,1]]}]})", "tile 3_0 is in no zone"},
        {R"({"version":1,"zones":[]})", "5 declared tiles are in no zone (first 0_0)"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,3,0],[0,1,0,1]]},{"id":"e","tiles":[]}]})", "zone \"e\": no tiles"},
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,0,0],[2,0,3,0]]},{"id":"b","tiles":[[1,0,1,0],[0,1,0,1]]}]})",
         "zone \"a\": not contiguous (tile "},
        // (1,0) and (0,1) touch only at a corner.
        {R"({"version":1,"zones":[{"id":"a","tiles":[[0,0,0,0],[2,0,3,0]]},{"id":"b","tiles":[[1,0,1,0]]},{"id":"c","tiles":[[0,1,0,1]]}]})",
         "zone \"a\": not contiguous"},
    };
    for (const auto& [text, want] : bad) {
        const auto got = zones_of(text);
        check(!got && err.starts_with(file) && err.find(want) != std::string::npos,
              std::string("zones rejected: ") + text + " -> " + err + " (want " + want + ")");
    }
    // Every problem, on an in-memory set: a = (0,0) + (2,0)-(3,0) is cut by d, and d = (1,0) +
    // (0,1) touch only at a corner (diagonal-only is not contiguous).
    map::Zones mem;
    mem.list.push_back({"a", "a", {{0, 0, 0, 0}, {2, 0, 3, 0}}});
    mem.list.push_back({"d", "d", {{1, 0, 1, 0}, {0, 1, 0, 1}}});
    const auto problems = map::validate_zones(mem, *map);
    check(problems.size() == 2 && problems[0].starts_with("zone \"a\": not contiguous") &&
              problems[1].starts_with("zone \"d\": not contiguous"),
          "zones: validate_zones lists every problem on an in-memory set: " + joined(problems));
    mem.list = {{"a", "a", {{0, 0, 3, 0}}}, {"c", "c", {{0, 1, 0, 1}}}};
    check(map::validate_zones(mem, *map).empty() && map::zone_at(mem, map::TileKey{0, 1}) == 1,
          "zones: validate_zones on a valid in-memory set fills byTile");
}

void checkAreaLoading() {
    std::vector<std::string> errors;
    LuaRoot r;
    r.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,1]],"spawn":{"pos":[10,0,10]}})");
    r.write("b/map.json", R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    r.write("a/zones.json", "{nope"); // the server never reads zones.json: no error for it
    r.write("b/areas.json", R"({"version":1,"areas":[{"id":"door","rect":[0,0,20,20],"portal":{"map":1,"pos":[300,0,300]}}]})");
    r.write("a/areas.json", R"({"version":1,"areas":[
        {"id":"ok","rect":[0,0,20,20],"portal":{"map":2,"pos":[100,0,100],"heading":1}},
        {"id":"lost","rect":[30,0,40,10],"portal":{"map":7,"pos":[1,0,1]}},
        {"id":"off","rect":[50,0,60,10],"portal":{"map":2,"pos":[300,0,1]}},
        {"id":"bounce","rect":[70,0,80,10],"portal":{"map":2,"pos":[5,0,5]}}]})");
    errors.clear();
    const auto maps = world::Maps::load(r.root, errors);
    const std::set<std::string> got(errors.begin(), errors.end());
    const std::set<std::string> want = {
        "maps/a/areas.json: area \"lost\": portal to unknown map 7",
        "maps/a/areas.json: area \"off\": portal destination is not on a tile of map 2",
        "maps/a/areas.json: area \"bounce\": portal destination is inside portal area \"door\" of map 2",
    };
    std::string all;
    for (const auto& e : errors) all += e + "; ";
    check(got == want, "every bad portal reported: " + all);
    const auto& ok = maps.info(1).areas.list[0];
    check(ok.portal && ok.portal->map == 2 && ok.portal->pos == std::array<float, 3>{100, 0, 100} && ok.portal->heading == 1,
          "portal parsed");

    r.write("b/areas.json", R"({"version":1,"areas":[{"id":"x","rect":[0,0,1,1],"parent":"y"}]})");
    r.write("a/areas.json", "{nope");
    errors.clear();
    world::Maps::load(r.root, errors);
    const std::set<std::string> got2(errors.begin(), errors.end());
    check(got2 == std::set<std::string>{"maps/a/areas.json: not valid JSON", "maps/b/areas.json: area \"x\": unknown parent \"y\""},
          "bad areas.json reported per file, its map not loaded");
}

#ifdef CHTHONIC_SAMPLE_MAPS
// The sample maps and lua/: area events on spawn and on crossing a border, then a walk into the cave portal.
void checkAreaWorld() {
    {
        std::vector<std::string> errors;
        const auto sample = world::Maps::load(CHTHONIC_SAMPLE_MAPS, errors);
        check(errors.empty() && sample.info(1).areas.list.size() == 7 && sample.info(2).areas.list.size() == 2 &&
                  sample.info(6).instanced() && sample.info(6).maxPlayers == 2u && sample.info(6).areas.list.size() == 2,
              "sample maps and their areas load" + (errors.empty() ? "" : ": " + errors[0]));
    }
    LogCapture capture;
    std::vector<std::string> mapErrors;
    // The sample components: the sample NPC definitions give their NPCs Health.
    TestWorld w(lua::load_component_defs(CHTHONIC_WORLD_LUA_SAMPLE, world::kReservedComponentNames).defs, {},
                world::Maps::load(CHTHONIC_SAMPLE_MAPS, mapErrors));
    const auto errors = w.world.load_scripts(CHTHONIC_WORLD_LUA_SAMPLE, {"zone_cycle"});
    check(mapErrors.empty() && errors.empty(), "area world: sample maps and lua load");
    std::vector<std::string> events;
    const auto record = [&](const char* what) {
        return [&events, what](const nlohmann::json& p) {
            events.push_back(std::format("{} {} {} {} pvp={}", what, p["entity"].get<uint32_t>(), p["map"].get<int>(),
                                         p["area"].get<std::string>(), p["flags"].value("pvp", false)));
        };
    };
    auto enter = w.world.bus().subscribe("area.enter", record("enter"));
    auto leave = w.world.bus().subscribe("area.leave", record("leave"));
    // Each map has its own context and bus: map 1's are delivered before map 2's in a tick.
    auto caveEnter = w.world.bus(2).subscribe("area.enter", record("enter"));
    auto caveLeave = w.world.bus(2).subscribe("area.leave", record("leave"));
    auto& reg = w.world.registry();

    // Cora waits on map 2 next to the portal destination.
    const auto a = w.spawn(1, 1, "Alice", 1024, 1024), b = w.spawn(2, 2, "Bob", 1046, 1010);
    const auto c = w.spawn(3, 3, "Cora", 256, 230, "", 2);
    const uint32_t A = wire(a), B = wire(b), C = wire(c);
    w.world.tick(T0);
    check(events == std::vector<std::string>{std::format("enter {} 1 goldshire pvp=true", A),
                                             std::format("enter {} 1 eastvale pvp=false", B),
                                             std::format("enter {} 2 cave pvp=false", C)},
          "spawn: area.enter only, flags inherited (pvp from goldshire)");
    check(reg.get<world::AreaRef>(a).area == 1 && reg.get<world::AreaRef>(b).area == 0, "AreaRef set on spawn");
    check(logged(std::format("area.enter entity {} map 1 area goldshire (Goldshire) flags {{noMount=false, pvp=true}}", A)),
          "events.lua logs area.enter");

    // A walks toward the portal in steps under the speed limit; it leaves goldshire at x = 1035.
    const proto::Vec3 path[] = {{1030, 0, 1018}, {1036, 0, 1012}, {1042, 0, 1006}};
    std::vector<std::vector<std::string>> perStep;
    for (int i = 0; i < 3; ++i) {
        events.clear();
        w.move(a, {path[i], 2.4f, 0});
        w.world.tick(T0 + std::chrono::seconds(i + 1));
        perStep.push_back(events);
    }
    check(perStep[0].empty() && perStep[2].empty(), "no events while inside one area");
    check(perStep[1] == std::vector<std::string>{std::format("leave {} 1 goldshire pvp=true", A),
                                                 std::format("enter {} 1 eastvale pvp=false", A)},
          "crossing a border: area.leave then area.enter, once each");
    check(std::ranges::any_of(w.sent.of<proto::Spawn>(1), [&](const proto::Spawn& s) { return s.entityId == B; }),
          "A sees B before the portal");

    // Into cave_mouth: transferred in the same tick, before the aoi pass.
    w.sent.clear();
    events.clear();
    w.move(a, {{1045, 0, 998}, 2.8f, 0});
    w.world.tick(T0 + std::chrono::seconds(4));
    const auto& tf = w.reg(a).get<world::Transform>(a);
    check(w.world.registry_of(a) == &w.world.registry(2) && w.reg(a).get<world::MapRef>(a).map == 2 && tf.pos == proto::Vec3{256, 0, 240} && tf.heading == 3.14f,
          "portal: on map 2 at the portal pos and heading");
    check(events == std::vector<std::string>{std::format("leave {} 1 eastvale pvp=false", A),
                                             std::format("enter {} 1 cave_mouth pvp=false", A),
                                             std::format("leave {} 1 cave_mouth pvp=false", A),
                                             std::format("enter {} 2 cave pvp=false", A)},
          "portal: leave eastvale, enter cave_mouth, leave it, enter the cave on map 2: " + joined(events));
    check(w.reg(a).get<world::AreaRef>(a).map == 2 && w.reg(a).get<world::AreaRef>(a).area == 0, "AreaRef: cave on map 2");
    const auto& toA = w.sent.bySession[1];
    const size_t change = first_of<proto::MapChange>(w.sent, 1), despawn = first_of<proto::Despawn>(w.sent, 1),
                 spawn = first_of<proto::Spawn>(w.sent, 1);
    check(change == 0 && std::get<proto::MapChange>(toA[0]).map == 2 && change < despawn && despawn < spawn &&
              spawn != SIZE_MAX,
          "portal: MapChange, then Despawn, then Spawn, all in the portal tick");
    bool despawnedB = false, sawBat = false;
    for (const auto& d : w.sent.of<proto::Despawn>(1)) despawnedB |= d.entityId == B;
    for (const auto& s : w.sent.of<proto::Spawn>(1)) sawBat |= s.name == "Bat";
    check(despawnedB && sawBat, "portal: A loses B and sees the Bat on map 2");
    bool bLostA = false;
    for (const auto& d : w.sent.of<proto::Despawn>(2)) bLostA |= d.entityId == A;
    check(bLostA, "B gets A's Despawn");
    bool leaked = false;
    for (uint32_t sid : {2u, 3u})
        for (const auto& m : w.sent.moves(sid)) leaked |= m.entityId == A;
    check(!leaked, "no EntityMoves entry for A's portal step, to B on map 1 or Cora on map 2");
    check(std::ranges::any_of(w.sent.of<proto::Spawn>(3), [&](const proto::Spawn& s) { return s.entityId == A; }),
          "Cora gets A's Spawn in the portal tick");

    // No ping-pong: the destination is not in a portal, and staying put fires nothing more.
    events.clear();
    w.move(a, {{256, 0, 241}, 0, 0});
    w.world.tick(T0 + std::chrono::seconds(5));
    check(w.reg(a).get<world::MapRef>(a).map == 2 && events.empty(), "moving on the cave map: no transfer, no events");
    w.world.despawn(a);
    check(!w.sent.saves.empty() && w.sent.saves.back().mapId == 2, "save keeps map 2");
}

// chthonic.map.area / chthonic.map.areaAt over the sample maps.
constexpr const char* kAreaLuaSystem = R"lua(
local done = false
return { update = function()
    if done then return end
    done = true
    local p
    for e in chthonic.query("Character") do p = e end
    local a = chthonic.map.area(p)
    expect(a and a.id == "goldshire" and a.name == "Goldshire", "area(player): goldshire")
    expect(a.flags.pvp == true and a.flags.noMount == false, "area flags inherited")
    expect(a.music == nil, "music not inherited")
    local e = chthonic.map.areaAt(1, 1100, 0, 1100)
    expect(e.id == "eastvale" and e.music == "sounds/eastvale.ogg" and e.flags.pvp == false, "areaAt eastvale with music")
    expect(chthonic.map.areaAt(1, 1024, 300, 1024).id == "eastvale", "areaAt above goldshire's y range: eastvale")
    expect(chthonic.map.areaAt(1, 1045, 0, 995).id == "cave_mouth", "areaAt the portal")
    expect(chthonic.map.areaAt(2, 256, 0, 256).flags.noMount == true, "areaAt map 2")
    expect(chthonic.map.areaAt(1, 5000, 0, 5000) == nil, "areaAt outside every area: nil")
    -- An NPC created on map 2 would be in map 2's context, at the end of the tick (maps.html#map-contexts): one here.
    local bat = chthonic.create("Bat2", {template = "Bat", position = {x = 1045, y = 0, z = 995}})
    expect(chthonic.map.area(bat).id == "cave_mouth", "area(NPC)")
    expect(chthonic.map.areaAt(2, 256, 0, 275).id == "cave_exit", "areaAt the cave's exit")
    raises('chthonic.map.areaAt: "map" must be the id of a loaded map, got 9', function() chthonic.map.areaAt(9, 1, 1, 1) end)
    raises('chthonic.map.areaAt: "y" must be a finite number', function() chthonic.map.areaAt(1, 1, "y", 1) end)
    raises("chthonic.map.area: invalid entity id 999999", function() chthonic.map.area(999999) end)
    raises("chthonic is read-only", function() chthonic.map.area = nil end)
    chthonic.log("area lua done")
end })lua";

void checkAreaLua() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/areas.lua", std::string(kLuaExpect) + kAreaLuaSystem);
    for (const char* name : {"Guard", "Sentry", "Bat"}) // the sample spawn files' NPCs
        r.write(std::format("npcs/{}.lua", name), std::format("return {{name = '{}', template = 'Guard'}}", name));
    std::vector<std::string> mapErrors;
    TestWorld w({}, {}, world::Maps::load(CHTHONIC_SAMPLE_MAPS, mapErrors));
    check(w.world.load_scripts(r.root).empty(), "area lua: loads");
    w.spawn(1, 1, "Alice", 1024, 1024);
    w.world.tick(T0);
    check_lua_expectations("area lua", "area lua done");
    for (const auto& s : w.world.script_status()) check(s.ok, "area lua: system still enabled " + s.error);
}
#endif

// --- ground (docs/world/maps.html#ground) ---

float ramp(float x, float z) { return 0.1f * x + 0.05f * z; }

// The ramp as tile (tx, tz)'s .r16 on a map with heightRange [0, 100].
std::string ramp_r16(int tx, int tz) {
    constexpr int n = map::kTileSamples;
    std::string bytes(size_t(n) * n * 2, '\0');
    for (int v = 0; v < n; ++v)
        for (int u = 0; u < n; ++u) {
            const auto sample = uint16_t(std::lround(ramp(float(tx * 256 + u), float(tz * 256 + v)) / 100 * 65535));
            bytes[2 * (size_t(v) * n + u)] = char(sample & 0xFF);
            bytes[2 * (size_t(v) * n + u) + 1] = char(sample >> 8);
        }
    return bytes;
}

constexpr float kGroundTol = 0.002f; // 16-bit samples over a 100 m range: 0.8 mm steps
bool on_ramp(std::optional<float> y, float x, float z) { return y && std::fabs(*y - ramp(x, z)) < kGroundTol; }

constexpr const char* kGroundLuaSystem = R"lua(
local done = false
return { update = function()
    if done then return end
    done = true
    local y = chthonic.map.ground(1, 10.3, 20.7)
    expect(y ~= nil and math.abs(y - (0.1 * 10.3 + 0.05 * 20.7)) < 0.002, "chthonic.map.ground on the ramp: " .. tostring(y))
    expect(chthonic.map.ground(1, 300, 300) == 0, "chthonic.map.ground on a tile without heights: heightMin")
    expect(chthonic.map.ground(1, 512, 10) == nil, "chthonic.map.ground off the map: nil")
    raises('chthonic.map.ground: "map" must be the id of a loaded map, got 9', function() chthonic.map.ground(9, 0, 0) end)
    raises('chthonic.map.ground: "x" must be a finite number', function() chthonic.map.ground(1, 0 / 0, 0) end)
    chthonic.log("ground lua done")
end }
)lua";

void checkGround() {
    // A 2 x 2 tile ramp: three tiles with a heights file, tile 1_1 without (flat at heightMin).
    LuaRoot r;
    r.write("ramp/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,1]],"heightRange":[0,100],"spawn":{"pos":[100,0,100]}})");
    r.write("ramp/heights/0_0.r16", ramp_r16(0, 0));
    r.write("ramp/heights/1_0.r16", ramp_r16(1, 0));
    r.write("ramp/heights/0_1.r16", ramp_r16(0, 1));
    r.write("ramp/.server/spawns/0_0.json",
            R"({"version":2,"spawns":[{"id":"high","npc":"T","pos":[50,100,60],"respawnSec":-1}]})");
    std::vector<std::string> errors;
    world::Maps maps = world::Maps::load(r.root, errors);
    check(errors.empty(), "ground: the ramp map loads " + joined(errors));
    check(on_ramp(maps.ground(1, 10, 20), 10, 20), "ground: at a sample");
    check(on_ramp(maps.ground(1, 10.3f, 20.7f), 10.3f, 20.7f) && on_ramp(maps.ground(1, 10.8f, 20.1f), 10.8f, 20.1f),
          "ground: between samples, in both triangles");
    check(on_ramp(maps.ground(1, 255.999f, 100), 255.999f, 100) && on_ramp(maps.ground(1, 256, 100), 256, 100) &&
              on_ramp(maps.ground(1, 100, 256), 100, 256),
          "ground: either side of the x tile edge and on the z tile edge");
    check(maps.ground(1, 300, 300) == 0.f, "ground: a tile without a heights file is flat at heightMin");
    check(!maps.ground(1, 512, 10) && !maps.ground(1, -0.1f, 10) && !maps.ground(9, 10, 10),
          "ground: nullopt off the map's tiles and on an unknown map");
    std::string err;
    const auto tile = map::decode_heights(ramp_r16(1, 0), ".r16", err);
    bool same = tile.has_value();
    for (float x = 256; same && x < 512; x += 7.37f)
        for (float z = 0; same && z < 256; z += 11.13f) same = *maps.ground(1, x, z) == map::ground(*tile, maps.info(1), x - 256, z);
    check(same, "ground: Maps::ground is the shared map::ground on the tile's heights");

    LuaRoot bad;
    bad.write("b/map.json", R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    bad.write("b/heights/0_0.r16", "short");
    bad.write("b/heights/5_5.r16", "not a tile of the map: never read");
    errors.clear();
    world::Maps::load(bad.root, errors);
    check(errors == std::vector<std::string>{"maps/b/heights/0_0.r16: r16: 5 bytes, need 132098 (257 x 257 x 2)"},
          "ground: a bad heights file is a load error: " + joined(errors));

    LogCapture capture;
    LuaRoot scripts;
    scripts.write("systems/ground.lua", std::string(kLuaExpect) + kGroundLuaSystem);
    scripts.write("npcs/t.lua", "return {name = 'T', template = 'T'}");
    TestWorld w({}, {}, std::move(maps));
    const auto luaErrors = w.world.load_scripts(scripts.root);
    check(luaErrors.empty(), "ground lua: loads " + joined(luaErrors));
    const auto& reg = w.world.registry();
    const auto at = [](int s) { return T0 + std::chrono::seconds(s); };

    // Entering: a saved position under the ground is lifted onto it; above it is kept (the client falls).
    const auto under = w.world.spawn(1, {1, 1, "Under", 1, {100, -50, 100}, 0, ""}, T0);
    const auto sky = w.world.spawn(2, {2, 1, "Sky", 1, {120, ramp(120, 100) + 50, 100}, 0, ""}, T0);
    check(on_ramp(w.sent.of<proto::EnterWorldOk>(1).at(0).pos.y, 100, 100), "saved under the ground: enters on it");
    check(on_ramp(w.sent.of<proto::EnterWorldOk>(2).at(0).pos.y - 50, 120, 100), "saved 50 m above the ground: kept");
    w.world.tick(T0);
    check_lua_expectations("ground lua", "ground lua done");

    // Moves: under the ground lifted (and corrected), rising into the air corrected, falling accepted.
    w.sent.clear();
    w.move(under, {{101, ramp(101, 100) - 5, 100}, 0, 0});
    w.world.tick(at(1));
    const auto lift = w.sent.of<proto::Correction>(1);
    check(lift.size() == 1 && lift[0].pos == reg.get<world::Transform>(under).pos &&
              on_ramp(reg.get<world::Transform>(under).pos.y, 101, 100),
          "move 5 m under the ground: accepted, lifted onto it, Correction with the lifted pos");
    w.sent.clear();
    w.move(under, {{102, ramp(102, 100) + 10, 100}, 0, 0});
    w.world.tick(at(2));
    const auto fly = w.sent.of<proto::Correction>(1);
    check(fly.size() == 1 && fly[0].pos == reg.get<world::Transform>(under).pos && fly[0].pos.x == 101,
          "rising 10 m above the ground: Correction to the last position");
    w.move(sky, {{121, ramp(121, 100) + 49, 100}, 0, 0});
    w.world.tick(at(3));
    check(w.sent.of<proto::Correction>(2).empty() && on_ramp(reg.get<world::Transform>(sky).pos.y - 49, 121, 100),
          "falling from 50 m above the ground: accepted");

    // NPCs stand on the ground: chthonic.create / create_npc, spawn files. Transfers land on it.
    const auto npc = w.world.create_npc("Up", "T", 1, proto::Vec3{200, 100, 30}, 0);
    check(on_ramp(reg.get<world::Transform>(npc).pos.y, 200, 30), "NPC created at y = 100 stands on the ground");
    bool spawned = false;
    for (auto [e, ref, tf] : reg.view<world::SpawnRef, world::Transform>().each()) spawned = on_ramp(tf.pos.y, 50, 60);
    check(spawned, "spawn file pos y = 100: the NPC stands on the ground");
    w.sent.clear();
    check(w.world.transfer(under, {1, {400, 999, 50}, 0}), "ground: transfer accepted");
    const auto mc = w.sent.of<proto::MapChange>(1);
    check(mc.size() == 1 && on_ramp(mc[0].pos.y, 400, 50), "transfer to y = 999: MapChange on the ground");
}

// --- navmesh (docs/world/maps.html#navmesh) ---

// A 2 x 1 tile fixture: a gentle ramp up x (~3 degrees) and a block across the straight line from (100, 100) to
// (450, 100): x 310..350, z 0..200, 22 m high, its sides 8 m wide 70 degree slopes. The way round is z > 200.
float cliff(float x, float z) {
    const float inside = std::min({x - 310, 350 - x, 200 - z});
    return 0.05f * x + 22 * std::clamp(inside / 8, 0.0f, 1.0f);
}

std::string r16_of(float (*height)(float, float), int tx, int tz) {
    constexpr int n = map::kTileSamples;
    std::string bytes(size_t(n) * n * 2, '\0');
    for (int v = 0; v < n; ++v)
        for (int u = 0; u < n; ++u) {
            const auto sample = uint16_t(std::lround(height(float(tx * 256 + u), float(tz * 256 + v)) / 100 * 65535));
            bytes[2 * (size_t(v) * n + u)] = char(sample & 0xFF);
            bytes[2 * (size_t(v) * n + u) + 1] = char(sample >> 8);
        }
    return bytes;
}

constexpr const char* kCliffManifest =
    R"({"version":1,"id":1,"tiles":[[0,0,1,0]],"heightRange":[0,100],"nav":{"maxSlopeDeg":45},"spawn":{"pos":[100,0,100]}})";

constexpr const char* kNavLuaSystem = R"lua(
local done = false
return { update = function()
    if done then return end
    done = true
    local from = {x = 100, y = chthonic.map.ground(1, 100, 100), z = 100}
    local to = {x = 450, y = chthonic.map.ground(1, 450, 100), z = 100}
    local path = chthonic.map.path(1, from, to)
    expect(type(path) == "table" and #path > 2, "chthonic.map.path: a list of more than 2 points")
    local first, last = path[1], path[#path]
    expect(math.abs(first.x - 100) < 1 and math.abs(first.z - 100) < 1, "chthonic.map.path: starts at from")
    expect(math.abs(last.x - 450) < 1 and math.abs(last.z - 100) < 1, "chthonic.map.path: ends at to")
    local around = false
    for _, p in ipairs(path) do
        expect(type(p.x) == "number" and type(p.y) == "number" and type(p.z) == "number", "chthonic.map.path: {x, y, z} points")
        around = around or p.z > 200
    end
    expect(around, "chthonic.map.path: goes round the block (z > 200)")
    expect(chthonic.map.path(2, {x = 10, y = 0, z = 10}, {x = 20, y = 0, z = 20}) == nil, "chthonic.map.path: nil without a navmesh")
    expect(chthonic.map.path(1, from, {x = 330, y = 1000, z = 100}) == nil, "chthonic.map.path: nil when to is not near the mesh")
    raises('chthonic.map.path: "from" must be a table {x=, y=, z=} of finite numbers', function() chthonic.map.path(1, 5, to) end)
    raises('chthonic.map.path: "map" must be the id of a loaded map, got 9', function() chthonic.map.path(9, from, to) end)
    chthonic.log("nav lua done")
end }
)lua";

void checkNavmesh() {
    std::string err;
    const auto info = map::parse_manifest(nlohmann::json::parse(kCliffManifest), err);
    map::TileHeights heights;
    for (int tx : {0, 1}) heights[{tx, 0}] = *map::decode_heights(r16_of(cliff, tx, 0), ".r16", err);
    check(mapbake::seam_errors(*info, heights).empty(), "bake: the fixture's seam matches");
    map::TileHeights torn = heights;
    torn.at({1, 0})[size_t(40) * map::kTileSamples] += 4000; // column 0 of tile 1_0 at z 40
    const auto seams = mapbake::seam_errors(*info, torn);
    check(seams == std::vector<std::string>{"seam 0_0 | 1_0: 1 samples differ, worst 6.10 m at z 40"},
          "bake: a torn seam is reported: " + joined(seams));

    LuaRoot r;
    r.write("cliff/map.json", kCliffManifest);
    r.write("flat/map.json", R"({"version":1,"id":2,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    std::map<int, std::string> navtiles;
    std::map<int, int> polys;
    for (int tx : {0, 1}) {
        r.write(std::format("cliff/heights/{}_0.r16", tx), r16_of(cliff, tx, 0));
        const auto baked = mapbake::bake_tile(*info, heights, {tx, 0}, err);
        check(baked && baked->polys > 0 && baked->navtile.starts_with("KNAV"),
              std::format("bake: tile {}_0 in memory ({} polys) {}", tx, baked ? baked->polys : 0, err));
        if (baked) r.write(std::format("cliff/.server/nav/{}_0.navtile", tx), navtiles[tx] = baked->navtile);
        polys[tx] = baked ? baked->polys : 0;
    }
    // The per-tile poly limit (32-bit poly refs), shared by the bake and the server: over it, the bake fails on the tile.
    static_assert(map::max_navtile_polys(1) == 1 << 22 && map::max_navtile_polys(64) == 65536 &&
                  map::max_navtile_polys(65) == 32768 && map::max_navtile_polys(16 * 16) == 16384 &&
                  map::max_navtile_polys(32 * 32) == 4096 && map::max_navtile_polys(64 * 64) == 1024);
    map::MapInfo crowded = *info; // far-off tiles (not 0_0's neighbours, so its geometry is unchanged): 2 polys per tile
    crowded.tiles.push_back({0, 2, 1023, 1025});
    const int crowdedTiles = crowded.tile_count(), limit = map::max_navtile_polys(crowdedTiles);
    check(crowdedTiles == 1048578 && limit == 2 && polys[0] > limit,
          std::format("bake: {} tiles, limit {}, tile 0_0 has {} polys", crowdedTiles, limit, polys[0]));
    err.clear();
    const auto over = mapbake::bake_tile(crowded, heights, {0, 0}, err);
    const std::string overWant = std::format(
        "{} polygons, more than the 2 per tile 32-bit poly refs allow on a map of 1048578 tiles", polys[0]);
    check(!over && err == overWant, "bake: a tile over the poly limit fails: " + err);
    err.clear();
    std::vector<std::string> errors;
    world::Maps maps = world::Maps::load(r.root, errors);
    check(errors.empty() && maps.nav().has(1) && !maps.nav().has(2),
          "nav: navtiles load; only the baked map has a navmesh " + joined(errors));

    const auto at = [&](float x, float z) { return proto::Vec3{x, *maps.ground(1, x, z), z}; };
    const auto path = maps.nav().path(1, at(100, 100), at(450, 100));
    float length = 0;
    bool around = false, throughBlock = false;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i) length += std::hypot(path[i].x - path[i - 1].x, path[i].z - path[i - 1].z);
        around |= path[i].z > 200;
        throughBlock |= path[i].x > 310 && path[i].x < 350 && path[i].z < 200;
    }
    check(path.size() > 2 && std::hypot(path.back().x - 450, path.back().z - 100) < 1,
          std::format("nav: a path from tile 0_0 to tile 1_0 ({} points)", path.size()));
    check(around && !throughBlock && length > 400,
          std::format("nav: it goes round the block, not through it ({:.0f} m against 350 m straight)", length));
    check(maps.nav().path(1, at(100, 100), {330, 1000, 100}).empty() && maps.nav().path(2, {1, 0, 1}, {9, 0, 9}).empty(),
          "nav: no path to a point off the mesh, or on a map without one");
    const float slopeDeg = std::atan(cliff(314.5f, 100) - cliff(313.5f, 100)) * 180 / std::numbers::pi_v<float>;
    check(std::fabs(slopeDeg - 70) < 0.5f && !maps.nav().on_mesh(1, at(314, 100), 6),
          std::format("nav: on_mesh false on the {:.1f} degree slope", slopeDeg));
    check(maps.nav().on_mesh(1, at(100, 100), 6) && maps.nav().on_mesh(1, at(256, 230), 6) &&
              maps.nav().on_mesh(1, {100, at(100, 100).y + 5, 100}, 6),
          "nav: on_mesh true on walkable ground, at the tile seam and 5 m above it (jumping)");
    check(!maps.nav().on_mesh(1, {100, at(100, 100).y + 20, 100}, 6) && !maps.nav().on_mesh(2, {10, 0, 10}, 6),
          "nav: on_mesh false 20 m above (maxAir 6) and on a map without a navmesh");

    // Bad navtiles: load errors naming the file.
    const auto load_error = [&](const std::string& bytes) {
        LuaRoot bad;
        bad.write("cliff/map.json", kCliffManifest).write("cliff/.server/nav/0_0.navtile", bytes);
        std::vector<std::string> errs;
        world::Maps::load(bad.root, errs);
        return joined(errs);
    };
    const std::string body = navtiles[0].substr(map::kNavtileHeaderBytes);
    const std::string badMagic = load_error("XNAV" + navtiles[0].substr(4));
    check(badMagic == "maps/cliff/.server/nav/0_0.navtile: not a navtile (bad magic, want 'KNAV')",
          "nav: bad magic rejected: " + badMagic);
    std::string v99 = navtiles[0];
    v99[4] = 99;
    const std::string badVersion = load_error(v99);
    check(badVersion == "maps/cliff/.server/nav/0_0.navtile: navtile version 99, this server reads 1 (rebake with chthonic_mapbake)",
          "nav: bad navtile version rejected: " + badVersion);
    const std::string badDetour = load_error(map::navtile_header(6) + body);
    check(contains(badDetour, "0_0.navtile: Detour version 6, this server has "), "nav: bad Detour version rejected: " + badDetour);
    const std::string otherTile = load_error(navtiles[1]);
    check(otherTile == "maps/cliff/.server/nav/0_0.navtile: baked for tile 1_0", "nav: a tile baked for another rejected: " + otherTile);
    const std::string truncated = load_error(navtiles[0].substr(0, map::kNavtileHeaderBytes + 3));
    check(truncated == "maps/cliff/.server/nav/0_0.navtile: truncated Detour tile", "nav: a truncated navtile rejected: " + truncated);

    // Moves: off the navmesh -> Correction; MOVE_NAV_CHECK=0 or a map without navtiles: phase 5 rules only.
    const auto corrections = [&](bool navCheck, uint16_t map, proto::Vec3 from, proto::Vec3 to) {
        world::WorldRules rules;
        rules.move.navCheck = navCheck;
        std::vector<std::string> errs;
        TestWorld w({}, rules, world::Maps::load(r.root, errs));
        const auto e = w.world.spawn(1, {1, 1, "Walker", map, from, 0, ""}, T0);
        w.world.tick(T0);
        w.sent.clear();
        w.move(e, {to, 0, 0});
        w.world.tick(T0 + std::chrono::seconds(1));
        return w.sent.of<proto::Correction>(1).size();
    };
    check(corrections(true, 1, at(305, 100), at(314, 100)) == 1, "move onto the 70 degree slope: off_mesh -> Correction");
    check(corrections(true, 1, at(300, 100), at(308, 100)) == 0, "move along walkable ground: accepted");
    check(corrections(false, 1, at(305, 100), at(314, 100)) == 0, "MOVE_NAV_CHECK=0: the same move onto the slope accepted");
    check(corrections(true, 2, {10, 0, 10}, {18, 0, 10}) == 0 && corrections(true, 2, {10, 0, 10}, {10, 20, 10}) == 1,
          "a map without navtiles: phase 5 rules (walk accepted, flying corrected)");
    proto::Vec3 slope = at(314, 100);
    MoveBudget budget{11, T0};
    check(validate_move({}, budget, at(305, 100), slope, slope.y, T0, false) == MoveVerdict::off_mesh && budget.meters == 11,
          "validate_move: onMesh false is off_mesh, spending no budget");

    LogCapture capture;
    LuaRoot scripts;
    scripts.write("systems/nav.lua", std::string(kLuaExpect) + kNavLuaSystem);
    TestWorld w({}, {}, std::move(maps));
    const auto luaErrors = w.world.load_scripts(scripts.root);
    check(luaErrors.empty(), "nav lua: loads " + joined(luaErrors));
    w.world.tick(T0);
    check_lua_expectations("nav lua", "nav lua done");
}

// --- NPC definitions (docs/world/npcs.html#npc-definitions) ---

constexpr const char* kHp = R"(return { name = "Hp", replicate = "aoi", fields = {
    { name = "cur", type = "float", default = 10, min = 0 }, { name = "max", type = "float", default = 10 } } })";
constexpr const char* kSecret = R"(return { name = "Secret", replicate = "owner", fields = { { name = "v", type = "int" } } })";

// The component types (Stats, aoi Hp, owner Secret) and the plain machine the NPC definitions below use.
LuaRoot& npc_root(LuaRoot& r) {
    r.write("components/stats.lua", kStats).write("components/hp.lua", kHp).write("components/secret.lua", kSecret);
    r.write("fsms/plain.lua", kPlain);
    return r;
}

// Every error World::load_scripts reports for these files on top of npc_root.
std::vector<std::string> npc_errors(const std::vector<std::pair<std::string, std::string>>& files) {
    LuaRoot r;
    npc_root(r);
    for (const auto& [path, body] : files) r.write(path, body);
    TestWorld w(r.defs().defs);
    return w.world.load_scripts(r.root);
}

void checkNpcDefinitionErrors() {
    LogCapture capture;
    const std::string longName(256, 'x');
    const std::string name = "npcs/a.lua: \"name\" must be a non-empty string of at most 255 bytes";
    const std::string tpl = "npcs/a.lua: \"template\" must be a non-empty string of at most 255 bytes";
    const auto with = [](const std::string& c) { return "return {name = 'A', template = 'T', components = " + c + "}"; };
    for (const auto& [body, want] : std::vector<std::pair<std::string, std::string>>{
             {"return {template = 'T'}", name},
             {"return {name = '', template = 'T'}", name},
             {"return {name = 5, template = 'T'}", name},
             {"return {name = '" + longName + "', template = 'T'}", name},
             {"return {name = 'A'}", tpl},
             {"return {name = 'A', template = ''}", tpl},
             {"return {name = 'A', template = '" + longName + "'}", tpl},
             {"return {name = 'A', template = 'T', extra = 1}", "npcs/a.lua: unknown key extra"},
             {"return 5", "npcs/a.lua: must return exactly one table {name = ..., template = ..., components = {...}, loot = {...}}"},
             {with("5"), "npcs/a.lua: \"components\" must be a table {Type = {field = value, ...}, ...}, got 5"},
             {with("{'x'}"), "npcs/a.lua: \"components\": component names must be strings, got 1"},
             {with("{Nope = {}}"), "npcs/a.lua: unknown component \"Nope\""},
             {with("{Npc = {}}"), "npcs/a.lua: \"Npc\" is read-only on the server"},
             {with("{Transform = {}}"), "npcs/a.lua: \"Transform\" is read-only on the server"},
             {with("{Spawn = {}}"), "npcs/a.lua: \"Spawn\" is read-only on the server"},
             {with("{Character = {}}"), "npcs/a.lua: \"Character\" is read-only on the server"},
             {with("{Stats = 5}"), "npcs/a.lua: \"Stats\" must be a table {field = value, ...}, got 5"},
             {with("{Stats = {nope = 1}}"), "npcs/a.lua: \"Stats\": unknown field \"nope\""},
             {with("{Stats = {flag = 'x'}}"), "npcs/a.lua: \"Stats\": \"flag\" must be true or false"},
             {with("{Stats = {hp = 1.5}}"), "npcs/a.lua: \"Stats\": \"hp\" must be an integer"},
             {with("{Stats = {hp = 101}}"), "npcs/a.lua: \"Stats\": \"hp\" must be <= 100"},
             {with("{Stats = {hp = -1}}"), "npcs/a.lua: \"Stats\": \"hp\" must be >= 0"},
             {with("{Stats = {kind = 'zzz'}}"), "npcs/a.lua: \"Stats\": \"kind\" unknown value \"zzz\" (expected one of \"a\", \"b\")"},
             {with("{Stats = {tags = {'a', 3}}}"), "npcs/a.lua: \"Stats\": \"tags\" element 2 must be a string"},
             {with("{StateMachine = {machine = 'nope'}}"),
              "npcs/a.lua: \"StateMachine\": \"machine\" \"nope\" names no valid state machine definition in fsms/"},
             {with("{StateMachine = {}}"),
              "npcs/a.lua: \"StateMachine\": \"machine\" \"\" names no valid state machine definition in fsms/"},
         }) {
        const auto errors = npc_errors({{"npcs/a.lua", body}});
        const std::string got = errors.size() == 1 ? errors[0] : "<" + std::to_string(errors.size()) + " errors: " + joined(errors) + ">";
        check(got == want, "npc definition rejected: want [" + want + "] got [" + got + "]");
    }
    const auto boom = npc_errors({{"npcs/a.lua", "error('boom')"}});
    check(boom.size() == 1 && boom[0].starts_with("npcs/a.lua:1: boom"), "npc definition: a Lua error is path:line " + joined(boom));

    check(npc_errors({{"npcs/d1.lua", "return {name = 'D', template = 'T'}"}, {"npcs/sub/d2.lua", "return {name = 'D', template = 'T'}"}}) ==
              std::vector<std::string>{"npcs/d1.lua: name \"D\" is also declared in npcs/sub/d2.lua",
                                       "npcs/sub/d2.lua: name \"D\" is also declared in npcs/d1.lua"},
          "npc definitions: a duplicate name rejects both files, each naming the other");
    check(npc_errors({{"npcs/a.lua", "return {}"}, {"npcs/b.lua", "return {name = 'B'}"}, {"npcs/c.lua", "return {name = 'C', template = 'T'}"}})
                  .size() == 2,
          "npc definitions: every bad file is reported, not just the first");
    check(npc_errors({}).empty(), "npc definitions: no npcs/ folder is fine");
    check(npc_errors({{"npcs/readme.txt", "not lua"}}).empty(), "npc definitions: an npcs/ folder without .lua files is fine");
    check(npc_errors({{"npcs/sandbox.lua", "return {name = io and 'X' or 'S', template = os and 'X' or 'T'}"}}).empty(),
          "npc definitions: run sandboxed (no io, os)");
}

// --- Item definitions and NPC loot tables (docs/world/lua.html#items) ---

constexpr const char* kWing = "return {id = 'wing', name = 'Wing', icon = 'icons/wing.png', quality = 'poor', maxStack = 20, "
                              "sellPrice = 1, description = 'Leathery.'}";

void checkItemsAndLoot() {
    LogCapture capture;
    const char* parts[] = {"id = 'a'", "name = 'A'", "icon = 'i.png'", "quality = 'common'", "maxStack = 5", "sellPrice = 0",
                           "description = ''"};
    const auto item = [&](const std::string& drop, const std::string& extra) { // every field but drop, then extra
        std::string out;
        for (const std::string part : parts)
            if (!part.starts_with(drop + " ")) out += (out.empty() ? "" : ", ") + part;
        return "return {" + out + extra + "}";
    };
    for (const auto& [body, want] : std::vector<std::pair<std::string, std::string>>{
             {"return 5", "items/a.lua: must return exactly one table {id = ..., name = ..., icon = ..., quality = ..., maxStack = ..., sellPrice = ..., description = ...}"},
             {item("", ", extra = 1"), "items/a.lua: unknown key extra"},
             {item("id", ""), "items/a.lua: \"id\" must be a non-empty string of at most 64 bytes"},
             {item("id", ", id = 'Bad Id'"), "items/a.lua: \"id\" may only hold a-z, 0-9 and _"},
             {item("name", ""), "items/a.lua: \"name\" must be a non-empty string of at most 255 bytes"},
             {item("icon", ""), "items/a.lua: \"icon\" must be a non-empty string of at most 255 bytes"},
             {item("description", ""), "items/a.lua: \"description\" must be a string of at most 1024 bytes"},
             {item("quality", ", quality = 'shiny'"), "items/a.lua: \"quality\" must be one of \"poor\", \"common\", \"uncommon\", \"rare\", \"epic\""},
             {item("maxStack", ", maxStack = 0"), "items/a.lua: \"maxStack\" must be an integer in [1, 1000]"},
             {item("maxStack", ", maxStack = 1.5"), "items/a.lua: \"maxStack\" must be an integer in [1, 1000]"},
             {item("sellPrice", ", sellPrice = -1"), "items/a.lua: \"sellPrice\" must be an integer in [0, 1000000000]"},
         }) {
        LuaRoot r;
        r.write("items/a.lua", body);
        TestWorld w({});
        const auto errors = w.world.load_scripts(r.root);
        const std::string got = errors.size() == 1 ? errors[0] : "<" + joined(errors) + ">";
        check(got == want, "item definition rejected: want [" + want + "] got [" + got + "]");
    }
    {
        LuaRoot r;
        r.write("items/w1.lua", kWing).write("items/sub/w2.lua", kWing);
        TestWorld w({});
        const auto errors = w.world.load_scripts(r.root);
        check(errors == std::vector<std::string>{"items/sub/w2.lua: id \"wing\" is also declared in items/w1.lua",
                                                 "items/w1.lua: id \"wing\" is also declared in items/sub/w2.lua"},
              "item definitions: a duplicate id rejects both files " + joined(errors));
    }

    const auto loot = [](const std::string& l) { return "return {name = 'A', template = 'T', loot = " + l + "}"; };
    for (const auto& [body, want] : std::vector<std::pair<std::string, std::string>>{
             {loot("5"), "npcs/a.lua: \"loot\": must be a table"},
             {loot("{coins = 1}"), "npcs/a.lua: \"loot\": unknown key coins"},
             {loot("{gold = {min = 3, max = 1}}"), "npcs/a.lua: \"loot\": \"gold\": \"max\" must be an integer in [3, 1000000]"},
             {loot("{gold = {min = -1, max = 1}}"), "npcs/a.lua: \"loot\": \"gold\": \"min\" must be an integer in [0, 1000000]"},
             {loot("{items = 5}"), "npcs/a.lua: \"loot\": \"items\" must be a sequence {{item = ..., chance = ..., min = ..., max = ...}, ...}"},
             {loot("{items = {{item = 'nope', chance = 1, min = 1, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"item\" must name an item definition in items/"},
             {loot("{items = {{item = 'wing', chance = 0, min = 1, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"chance\" must be a number in (0, 1]"},
             {loot("{items = {{item = 'wing', chance = 1.5, min = 1, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"chance\" must be a number in (0, 1]"},
             {loot("{items = {{item = 'wing', chance = 1, min = 0, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"min\" must be an integer in [1, 1000000]"},
             {loot("{items = {{item = 'wing', chance = 1, min = 2, max = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: \"max\" must be an integer in [2, 1000000]"},
             {loot("{items = {{item = 'wing', chance = 1, min = 1, max = 1, x = 1}}}"), "npcs/a.lua: \"loot\": \"items\"[1]: unknown key x"},
         }) {
        const auto errors = npc_errors({{"items/wing.lua", kWing}, {"npcs/a.lua", body}});
        const std::string got = errors.size() == 1 ? errors[0] : "<" + joined(errors) + ">";
        check(got == want, "npc loot rejected: want [" + want + "] got [" + got + "]");
    }

    // chthonic.items and chthonic.npcDef from a system; both read-only or fresh copies.
    LuaRoot r;
    r.write("items/wing.lua", kWing);
    r.write("npcs/bat.lua", "return {name = 'Bat', template = 'BatT', loot = {gold = {min = 1, max = 3}, "
                            "items = {{item = 'wing', chance = 0.8, min = 1, max = 2}}}}");
    r.write("npcs/ghost.lua", "return {name = 'Ghost', template = 'G'}");
    r.write("systems/items.lua", std::string(kLuaExpect) + R"lua(
return { init = function()
    local w = chthonic.items.wing
    expect(w.id == "wing" and w.name == "Wing" and w.icon == "icons/wing.png" and w.quality == "poor" and w.maxStack == 20 and
           w.sellPrice == 1 and w.description == "Leathery.", "chthonic.items: every field")
    expect(chthonic.items.nope == nil, "chthonic.items: unknown id is nil")
    raises("chthonic is read-only", function() chthonic.items.wing.maxStack = 99 end)
    raises("chthonic is read-only", function() chthonic.items.x = {} end)
    local bat = chthonic.npcDef("Bat")
    expect(bat.name == "Bat" and bat.template == "BatT" and bat.loot.gold.min == 1 and bat.loot.gold.max == 3, "chthonic.npcDef: name, template, gold")
    local e = bat.loot.items[1]
    expect(e.item == "wing" and e.chance == 0.8 and e.min == 1 and e.max == 2, "chthonic.npcDef: loot items")
    bat.loot.gold.min = 50
    expect(chthonic.npcDef("Bat").loot.gold.min == 1, "chthonic.npcDef returns a fresh copy")
    expect(chthonic.npcDef("Ghost").loot == nil and chthonic.npcDef("Nope") == nil, "no loot / no definition: nil")
    raises("chthonic.npcDef: name must be a string, got 5", function() chthonic.npcDef(5) end)
    chthonic.log("items lua done")
end, update = function() end }
)lua");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "items lua: loads " + joined(errors));
    check_lua_expectations("items lua", "items lua done");
}

// chthonic.definitions(folder): every .lua file under it, sorted by path, as fresh plain-data copies plus their paths;
// a missing folder is none; bad folders and bad files raise.
void checkDefinitionsApi() {
    LogCapture capture;
    LuaRoot r;
    r.write(".server/quests/b.lua", "return {id = 'b', objectives = {{type = 'kill', count = 3}}, requires = {'a'}}");
    r.write(".server/quests/a.lua", "return {id = 'a', title = 'A'}");
    r.write(".server/quests/sub/c.lua", "return {id = 'c'}");
    r.write(".server/quests/notes.txt", "not lua");
    r.write("bad/fn.lua", "return {f = function() end}");
    r.write("bad2/two.lua", "return {}, {}");
    r.write("systems/defs.lua", std::string(kLuaExpect) + R"lua(
return { init = function()
    local list, paths = chthonic.definitions(".server/quests")
    expect(#list == 3 and #paths == 3, "chthonic.definitions: every .lua file, recursively")
    expect(paths[1] == ".server/quests/a.lua" and paths[2] == ".server/quests/b.lua" and paths[3] == ".server/quests/sub/c.lua",
           "chthonic.definitions: sorted LUA_ROOT-relative paths")
    expect(list[1].title == "A" and list[2].objectives[1].count == 3 and list[2].requires[1] == "a" and list[3].id == "c",
           "chthonic.definitions: the returned tables")
    list[1].title = "changed"
    expect(chthonic.definitions(".server/quests")[1].title == "A", "chthonic.definitions returns fresh copies")
    local none, nopaths = chthonic.definitions("nope/none")
    expect(#none == 0 and #nopaths == 0, "chthonic.definitions: a missing folder is none")
    for _, bad in ipairs({"", "/abs", "../x", "a/../b", "a//b", "./a", "a\\b", "c:x"}) do
        local ok, err = pcall(chthonic.definitions, bad)
        expect(not ok and string.find(err, "chthonic.definitions: folder must be a relative path", 1, true), "chthonic.definitions refuses folder [" .. bad .. "]")
    end
    raises("chthonic.definitions: folder must be a relative path", function() chthonic.definitions(5) end)
    raises("chthonic.definitions: bad/fn.lua", function() chthonic.definitions("bad") end)
    raises("chthonic.definitions: bad2/two.lua: must return exactly one table", function() chthonic.definitions("bad2") end)
    chthonic.log("definitions lua done")
end, update = function() end }
)lua");
    TestWorld w({});
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "definitions lua: loads " + joined(errors));
    check_lua_expectations("definitions lua", "definitions lua done");
}

constexpr const char* kNpcDefs[][2] = {
    {"npcs/guard.lua", "return {name = 'Guard', template = 'GuardTpl', components = {Stats = {hp = 50, tags = {'x', 'y'}}, "
                       "Hp = {cur = 7}, StateMachine = {machine = 'plain'}}}"},
    {"npcs/sub/rat.lua", "return {name = 'Rat', template = 'RatTpl', components = {Stats = {}}}"},
    {"npcs/ghost.lua", "return {name = 'Ghost', template = 'GhostTpl'}"},
};

void write_npc_defs(LuaRoot& r) {
    npc_root(r);
    for (const auto& [path, body] : kNpcDefs) r.write(path, body);
}

// chthonic.create with opts.npc / opts.components / opts.template; every bad call raises and creates nothing.
constexpr const char* kNpcCreateSystem = R"lua(
local g
local done = false
return {
init = function() g = chthonic.create("G1", {npc = "Guard", position = {x = 1010, y = 0, z = 1000}}) end,
update = function()
    if done then return end
    done = true
    local n = chthonic.get(g, "Npc")
    expect(n.name == "G1" and n.template == "GuardTpl" and n.npc == "Guard", "{npc}: name from the call, template and npc from the definition")
    local s = chthonic.get(g, "Stats")
    expect(s.hp == 50 and #s.tags == 2 and s.tags[2] == "y" and s.kind == "b" and s.flag == false and s.pos.x == 1, "{npc}: definition data over the field defaults")
    expect(chthonic.get(g, "Hp").cur == 7 and chthonic.get(g, "Hp").max == 10, "{npc}: unlisted fields at their defaults")
    expect(chthonic.fsm.state(g) == "a", "{npc}: the definition's StateMachine entered its initial state at create (in init)")
    expect(not chthonic.has(g, "Secret") and not chthonic.has(g, "Spawn"), "{npc}: only the listed components")
    local rat = chthonic.create("R", {npc = "Rat"})
    local rs = chthonic.get(rat, "Stats")
    expect(rs.hp == 10 and rs.kind == "b" and rs.tags[1] == "a" and not chthonic.has(rat, "Hp") and not chthonic.has(rat, "StateMachine"), "Stats = {}: all defaults")
    local ghost = chthonic.create("Gh", {npc = "Ghost"})
    expect(not chthonic.has(ghost, "Stats") and not chthonic.has(ghost, "Hp") and not chthonic.has(ghost, "StateMachine"), "no components: none")
    local o = chthonic.create("G2", {npc = "Guard", components = {Stats = {kind = "a"}, Secret = {v = 3}, StateMachine = {initialState = "b"}}})
    local os = chthonic.get(o, "Stats")
    expect(os.kind == "a" and os.hp == 50 and os.tags[2] == "y", "opts.components merges per field over the definition")
    expect(chthonic.get(o, "Secret").v == 3, "opts.components adds a type the definition lacks")
    expect(chthonic.fsm.state(o) == "b", "opts.components: StateMachine initialState over the definition")
    local again = chthonic.create("G3", {npc = "Guard"})
    expect(chthonic.get(again, "Stats").kind == "b" and not chthonic.has(again, "Secret"), "an override leaves the definition unchanged")
    local t = chthonic.create("T1", {template = "Tpl", components = {Stats = {hp = 5}}})
    local tn = chthonic.get(t, "Npc")
    expect(tn.npc == "" and tn.template == "Tpl" and chthonic.get(t, "Stats").hp == 5 and chthonic.get(t, "Stats").kind == "b", "{template, components}: npc empty, data over the defaults")
    local plain = chthonic.create("P", {template = "Tpl"})
    expect(chthonic.get(plain, "Npc").npc == "" and not chthonic.has(plain, "Stats"), "{template}: npc empty, no components")
    local count = function() local c = 0 ; for _ in chthonic.query("Npc") do c = c + 1 end ; return c end
    local before = count()
    local one = "chthonic.create: opts needs exactly one of npc (an npcs/ definition) and template"
    for _, c in ipairs({
        {one, {npc = "Guard", template = "T"}},
        {one, {}},
        {one, {components = {Stats = {}}}},
        {'chthonic.create: unknown npc "Nope" (no definition in npcs/)', {npc = "Nope"}},
        {"chthonic.create: npc must be a non-empty string of at most 255 bytes, got 5", {npc = 5}},
        {"chthonic.create: npc must be a non-empty string of at most 255 bytes, got ", {npc = ""}},
        {'chthonic.create: "Stats": "hp" must be <= 100', {npc = "Guard", components = {Stats = {hp = 101}}}},
        {'chthonic.create: "Stats": unknown field "nope"', {npc = "Guard", components = {Stats = {nope = 1}}}},
        {'chthonic.create: "Stats": "kind" unknown value "z"', {template = "T", components = {Stats = {kind = "z"}}}},
        {'chthonic.create: unknown component "Nope"', {npc = "Guard", components = {Nope = {}}}},
        {'chthonic.create: "Npc" is read-only on the server', {template = "T", components = {Npc = {}}}},
        {'chthonic.create: "Transform" is read-only on the server', {npc = "Guard", components = {Transform = {}}}},
        {'chthonic.create: "StateMachine": "machine" "nope" names no valid state machine definition in fsms/', {npc = "Guard", components = {StateMachine = {machine = "nope"}}}},
        {'chthonic.create: "components" must be a table {Type = {field = value, ...}, ...}, got 5', {npc = "Guard", components = 5}},
        {'chthonic.create: "position" must be a table', {npc = "Guard", components = {Stats = {hp = 1}}, position = 5}},
    }) do raises(c[1], function() chthonic.create("N", c[2]) end) end
    expect(count() == before, "no failed create made an NPC")
    chthonic.log("npc create done")
end }
)lua";

void checkNpcCreateFromDefinitions() {
    LogCapture capture;
    LuaRoot r;
    write_npc_defs(r);
    r.write("systems/create.lua", std::string(kLuaExpect) + kNpcCreateSystem);
    TestWorld w(r.defs().defs);
    const auto errors = w.world.load_scripts(r.root);
    check(errors.empty(), "npc create: loads " + joined(errors));
    w.spawn(1, 301, "Alice", 1000, 1000);
    auto& reg = w.world.registry();
    w.world.tick(T0);
    check_lua_expectations("npc create", "npc create done");
    for (const auto& s : w.world.script_status()) check(s.ok, "npc create: system still enabled " + s.error);
    w.world.tick(T0 + std::chrono::milliseconds(50)); // the NPCs update created come into view

    // AOI enter: the Spawn, then the aoi components of the full set; none / owner ones reach nobody.
    entt::entity g = entt::null, o = entt::null;
    for (auto [e, n] : reg.view<world::Npc>().each()) {
        if (n.name == "G1") g = e;
        if (n.name == "G2") o = e;
    }
    check(g != entt::null && o != entt::null, "npc create: G1 and G2 exist");
    if (g == entt::null || o == entt::null) return;
    const auto& toA = w.sent.bySession[1];
    size_t spawnAt = SIZE_MAX, hpAt = SIZE_MAX;
    for (size_t i = 0; i < toA.size(); ++i) {
        if (const auto* s = std::get_if<proto::Spawn>(&toA[i]); s && s->entityId == wire(g) && spawnAt == SIZE_MAX) spawnAt = i;
        if (const auto* u = std::get_if<proto::ComponentUpdate>(&toA[i]); u && u->entityId == wire(g) && u->name == "Hp" && hpAt == SIZE_MAX)
            hpAt = i;
    }
    check(spawnAt != SIZE_MAX && hpAt != SIZE_MAX && spawnAt < hpAt, "npc create: a player gets the NPC's Spawn, then its aoi Hp");
    const auto hp = w.sent.updates(1, wire(g), "Hp");
    check(hp.size() == 1 && Json::parse(hp[0].json) == Json::parse(R"({"cur":7.0,"max":10.0})"),
          "npc create: Hp sent once, with the definition's data: " + (hp.empty() ? std::string() : hp[0].json));
    check(w.sent.updates(1, wire(g), "Stats").empty() && w.sent.updates(1, wire(o), "Secret").empty() &&
              !w.sent.updates(1, wire(o), "Hp").empty(),
          "npc create: replicate none and owner components reach nobody; G2's Hp reaches the viewer");
}

// Spawn files instantiate definitions: exactly the definition's components and data, fresh on every respawn; an
// unknown "npc" refuses the start, naming the file and the spawn.
void checkNpcSpawnDefinitions() {
    LogCapture capture;
    LuaRoot lua, maps;
    write_npc_defs(lua);
    maps.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,7,7]],"spawn":{"pos":[1024,0,1024]}})");
    maps.write("a/.server/spawns/4_4.json", R"({"version":2,"spawns":[{"id":"g","npc":"Guard","pos":[1030,0,1030],"respawnSec":1}]})");
    std::vector<std::string> mapErrors;
    TestWorld w(lua.defs().defs, {}, world::Maps::load(maps.root, mapErrors));
    const auto errors = w.world.load_scripts(lua.root);
    check(mapErrors.empty() && errors.empty(), "npc spawn: loads " + joined(mapErrors) + joined(errors));
    auto& reg = w.world.registry();
    watch_spawns(w);
    w.world.tick(T0);
    const auto first = [&] { return reg.view<world::Npc>().empty() ? entt::entity{entt::null} : reg.view<world::Npc>().front(); };
    const auto g = first();
    check(g != entt::null, "npc spawn: placed on the first tick");
    if (g == entt::null) return;
    const auto& npc = reg.get<world::Npc>(g);
    check(npc.name == "Guard" && npc.templateName == "GuardTpl" && npc.npc == "Guard", "npc spawn: name, template, npc from the definition");
    const auto has = [&](entt::entity e, const char* type) { return lua::pool(reg, type).contains(e); };
    const Json stats = Json::parse(R"({"hp":50,"flag":false,"kind":"b","pos":[1.0,2.0,3.0],"tags":["x","y"]})");
    check(has(g, "Stats") && has(g, "Hp") && has(g, "StateMachine") && !has(g, "Secret") && w.value(g, "Stats") == stats &&
              w.value(g, "Hp") == Json::parse(R"({"cur":7.0,"max":10.0})"),
          "npc spawn: exactly the definition's components, with its data: " + w.value(g, "Stats").dump());
    check(w.fsm(g).state(lua::EntityId{wire(g)}) == "a", "npc spawn: StateMachine in its initial state");

    lua::pool(reg, "Stats").get(g).value["hp"] = 1;
    w.fsm(g).change(lua::EntityId{wire(g)}, "b");
    w.world.destroy_npc(g);
    w.world.tick(T0 + std::chrono::seconds(1));
    const auto again = first();
    check(again != entt::null && w.value(again, "Stats") == stats && w.fsm(again).state(lua::EntityId{wire(again)}) == "a",
          "npc spawn: a respawned NPC starts with fresh definition data and machine state");

    LuaRoot badMaps;
    badMaps.write("a/map.json", R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[10,0,10]}})");
    badMaps.write("a/.server/spawns/0_0.json", R"({"version":2,"spawns":[{"id":"x","npc":"Nope","pos":[10,0,10],"respawnSec":5},
        {"id":"y","npc":"Guard","pos":[20,0,10],"respawnSec":5}]})");
    TestWorld bad(lua.defs().defs, {}, world::Maps::load(badMaps.root, mapErrors));
    check(bad.world.load_scripts(lua.root) ==
              std::vector<std::string>{"maps/a/.server/spawns/0_0.json: spawn \"x\": unknown npc \"Nope\" (no definition in npcs/)"},
          "npc spawn: an npc with no definition refuses the start, naming the file and the spawn");
}

#ifdef CHTHONIC_SAMPLE_MAPS
// The sample hills map (id 3): its four PNG heightmaps load, and the ground at the portal destination is the
// terrain's, not hills_gate's y = 30.
void checkGroundSample() {
    std::vector<std::string> errors;
    const auto maps = world::Maps::load(CHTHONIC_SAMPLE_MAPS, errors);
    check(errors.empty(), "sample maps load with their heights " + joined(errors));
    const auto y = maps.ground(3, 256, 256);
    check(y && *y > 1 && *y < 79 && *y != 30, "hills: ground at (256, 256) from its heightmaps: " + std::to_string(y.value_or(-1)));
    check(maps.ground(1, 1024, 1024) == 0.f, "start: no heights files, flat at heightMin 0");
    // Its baked navtiles (maps/hills/.server/nav): the bake's slope limit is the client's nav.maxSlopeDeg (40).
    const auto at = [&](float x, float z) { return proto::Vec3{x, *maps.ground(3, x, z), z}; };
    check(maps.nav().has(3) && !maps.nav().has(1), "hills has a navmesh, start none");
    check(maps.nav().on_mesh(3, at(215, 331), 6) && maps.nav().on_mesh(3, at(256, 256), 6),
          "hills: on_mesh on a 32 degree slope (215, 331) and at the portal destination");
    check(!maps.nav().on_mesh(3, at(185, 317.5f), 6) && maps.nav().on_mesh(3, at(185, 331), 6) &&
              maps.nav().path(3, at(185, 310), at(185, 331)).empty(),
          "hills: on_mesh false on the mesa's 81 degree wall; its top is walkable but no path leads up");
    // Its zone object fixtures (maps/hills/objects, .server/interactables): the obstacles are cut out of the baked
    // navtiles, the visual is not, and the server knows the interactable.
    check(!maps.nav().on_mesh(3, at(266, 248), 6) && !maps.nav().on_mesh(3, at(160, 331), 6) &&
              maps.nav().on_mesh(3, at(262, 248), 6),
          "hills: the obstacle crates (266, 248) and (160, 331) are off the navmesh, 3 m beside them on");
    check(maps.nav().on_mesh(3, at(248, 244), 6), "hills: the visual crate (248, 244) stays walkable");
    const map::Interactable* shrine = maps.interactable(3, 1);
    check(shrine && shrine->event == "test.shrine" && shrine->data["msg"] == "hello" && shrine->name == "Test Shrine" &&
              !maps.interactable(1, 1),
          "hills: interactable 1 (test.shrine, data {msg = hello}) is on map 3 only");
}
#endif

// --- zone objects (docs/world/maps.html#objects) ---

constexpr const char* kVisualEntry = R"({"type":"visual","prefab":"prefabs/grass.prefab","pos":[10,0,10]})";

// objects/<tx>_<tz>.json: every field, then each problem as one "objects[i]: ..." error that skips only its entry.
void checkObjectParsing() {
    const auto parse = [](const std::string& text) { return map::parse_objects(nlohmann::json::parse(text), {0, 0}); };
    const auto good = parse(R"({"version":1,"objects":[
        {"type":"visual","prefab":"prefabs/grass.prefab","pos":[10,1,10],"yaw":1.5,"scale":0.5,"seed":3},
        {"type":"obstacle","prefab":"prefabs/rock.prefab","pos":[20,2,20],"shape":{"cylinder":{"radius":2,"height":3}}},
        {"type":"interactable","prefab":"prefabs/shrine.prefab","pos":[30,0,30],"id":7,"name":"Shrine",
         "shape":{"box":{"halfExtents":[1,2,1.5]}}}]})");
    check(good.errors.empty() && good.objects.size() == 3, "objects: a valid file parses: " + joined(good.errors));
    if (good.objects.size() == 3) {
        const auto& [v, o, i] = std::tie(good.objects[0], good.objects[1], good.objects[2]);
        check(v.type == map::ObjectType::Visual && v.prefab == "prefabs/grass.prefab" && v.pos == std::array<float, 3>{10, 1, 10} &&
                  v.yaw == 1.5f && v.scale == 0.5f && !v.shape,
              "objects: a visual's fields (unknown keys ignored, no shape)");
        check(o.type == map::ObjectType::Obstacle && o.scale == 1 && o.yaw == 0 && o.shape &&
                  o.shape->kind == map::Shape::Kind::Cylinder && o.shape->radius == 2 && o.shape->height == 3,
              "objects: an obstacle's cylinder, scale 1 and yaw 0 by default");
        check(i.type == map::ObjectType::Interactable && i.id == 7 && i.name == "Shrine" && i.shape &&
                  i.shape->kind == map::Shape::Kind::Box && i.shape->halfExtents == std::array<float, 3>{1, 2, 1.5f},
              "objects: an interactable's id, name and box");
    }
    const std::vector<std::pair<std::string, std::string>> bad = {
        {"5", "objects[1]: must be an object"},
        {R"({"type":"tree","prefab":"a.prefab","pos":[1,0,1]})", "objects[1]: \"type\" must be \"visual\", \"obstacle\" or \"interactable\""},
        {R"({"type":"visual","pos":[1,0,1]})", "objects[1]: \"prefab\" must be a project-relative .prefab path"},
        {R"({"type":"visual","prefab":"models/rock.glb","pos":[1,0,1]})", "objects[1]: \"prefab\" must be a project-relative .prefab path"},
        {R"({"type":"visual","prefab":"a.prefab"})", "objects[1]: \"pos\" must be [x, y, z]"},
        {R"({"type":"visual","prefab":"a.prefab","pos":[300,0,1]})", "objects[1]: \"pos\" is outside tile 0_0"},
        {R"({"type":"visual","prefab":"a.prefab","pos":[1,0,1],"yaw":"north"})", "objects[1]: \"yaw\" must be a number (radians)"},
        {R"({"type":"visual","prefab":"a.prefab","pos":[1,0,1],"scale":0})", "objects[1]: \"scale\" must be a number > 0"},
        {R"({"type":"obstacle","prefab":"a.prefab","pos":[1,0,1]})", "objects[1]: a blocking object needs \"shape\""},
        {R"({"type":"obstacle","prefab":"a.prefab","pos":[1,0,1],"shape":{"sphere":{"radius":1}}})",
         "objects[1]: \"shape\" must be {\"box\": {\"halfExtents\": [x, y, z]}} or {\"cylinder\": {\"radius\": r, \"height\": h}}"},
        {R"({"type":"obstacle","prefab":"a.prefab","pos":[1,0,1],"shape":{"box":{"halfExtents":[1,-1,1]}}})",
         "objects[1]: \"shape.box.halfExtents\" must be [x, y, z], each > 0"},
        {R"({"type":"obstacle","prefab":"a.prefab","pos":[1,0,1],"shape":{"cylinder":{"radius":1}}})",
         "objects[1]: \"shape.cylinder\" must be {\"radius\": r, \"height\": h}, each > 0"},
        {R"({"type":"interactable","prefab":"a.prefab","pos":[1,0,1],"shape":{"cylinder":{"radius":1,"height":1}}})",
         "objects[1]: \"id\" must be an integer in [1, 4294967295]"},
        {R"({"type":"interactable","prefab":"a.prefab","pos":[1,0,1],"id":0,"shape":{"cylinder":{"radius":1,"height":1}}})",
         "objects[1]: \"id\" must be an integer in [1, 4294967295]"},
    };
    for (const auto& [entry, want] : bad) {
        const auto got = parse(std::string(R"({"version":1,"objects":[)") + kVisualEntry + "," + entry + "]}");
        check(got.errors == std::vector<std::string>{want} && got.objects.size() == 1,
              "objects: bad entry skipped, the rest kept: want [" + want + "] got [" + joined(got.errors) + "]");
    }
    const std::string shrine = R"({"type":"interactable","prefab":"a.prefab","pos":[1,0,1],"id":4,"shape":{"cylinder":{"radius":1,"height":1}}})";
    const auto dup = parse(R"({"version":1,"objects":[)" + shrine + "," + shrine + "]}");
    check(dup.errors == std::vector<std::string>{"objects[1]: duplicate id 4"} && dup.objects.size() == 1,
          "objects: a repeated id is an error, the first kept: " + joined(dup.errors));
    for (const auto& [text, want] : std::vector<std::pair<std::string, std::string>>{
             {"[]", "must be a JSON object"}, {R"({"version":2,"objects":[]})", "\"version\" must be 1"},
             {R"({"version":1})", "\"objects\" must be a list"}}) {
        const auto got = parse(text);
        check(got.errors == std::vector<std::string>{want} && got.objects.empty(), "objects: file error " + want + ": " + joined(got.errors));
    }

    // .server/interactables/<tx>_<tz>.json: fail fast, like spawn files.
    const auto inter = [](const std::string& entries, std::string& err) {
        return map::parse_interactables(nlohmann::json::parse(R"({"version":1,"interactables":[)" + entries + "]}"), {0, 0}, err);
    };
    std::string err;
    const auto list = inter(R"({"id":7,"pos":[30,0,30],"yaw":0.5,"shape":{"box":{"halfExtents":[1,2,1]}},"range":4,
        "event":"shrine.activated","data":{"msg":"hi","n":[1,2]},"name":"Shrine","note":"kept"},
        {"id":8,"pos":[40,0,40],"shape":{"cylinder":{"radius":2,"height":1}},"event":"well.used"})", err);
    check(list && list->size() == 2, "interactables: a valid file parses " + err);
    if (list && list->size() == 2) {
        const auto& a = (*list)[0];
        check(a.id == 7 && a.yaw == 0.5f && a.range == 4 && a.event == "shrine.activated" && a.data["msg"] == "hi" &&
                  a.name == "Shrine" && a.shape.kind == map::Shape::Kind::Box,
              "interactables: every field");
        check((*list)[1].range == 3 && (*list)[1].data.is_null() && (*list)[1].name.empty(),
              "interactables: range 3 by default, no data, no name");
    }
    const std::string ok = R"("pos":[1,0,1],"shape":{"cylinder":{"radius":1,"height":1}},"event":"e")";
    std::string deep = "1";
    for (int i = 0; i < 32; ++i) deep = "{\"a\":" + deep + "}";
    for (const auto& [entries, want] : std::vector<std::pair<std::string, std::string>>{
             {"7", "interactables[0]: must be an object"},
             {R"({"pos":[1,0,1]})", "interactables[0]: \"id\" must be an integer in [1, 4294967295]"},
             {R"({"id":5,)" + ok + R"(},{"id":5,)" + ok + "}", "interactable 5: duplicate id"},
             {R"({"id":5,"pos":[1,0,999],"shape":{"cylinder":{"radius":1,"height":1}},"event":"e"})", "interactable 5: \"pos\" is outside tile 0_0"},
             {R"({"id":5,"pos":[1,0,1],"event":"e"})",
              "interactable 5: \"shape\" must be {\"box\": {\"halfExtents\": [x, y, z]}} or {\"cylinder\": {\"radius\": r, \"height\": h}}"},
             {R"({"id":5,"range":0,)" + ok + "}", "interactable 5: \"range\" must be a number > 0 (metres)"},
             {R"({"id":5,"pos":[1,0,1],"shape":{"cylinder":{"radius":1,"height":1}},"event":""})",
              "interactable 5: \"event\" must be a non-empty string of at most 255 bytes"},
             {R"({"id":5,"data":[1],)" + ok + "}", "interactable 5: \"data\" must be an object"},
             {R"({"id":5,"data":)" + deep + "," + ok + "}", "interactable 5: \"data\" is nested more than 31 deep"},
             {R"({"id":5,"name":3,)" + ok + "}", "interactable 5: \"name\" must be a string"}}) {
        err.clear();
        check(!inter(entries, err) && err == want, "interactables: want [" + want + "] got [" + err + "]");
    }

    // Footprints and the horizontal distance to their edge.
    map::Shape wall;
    wall.halfExtents = {2, 1, 1};
    map::Shape post;
    post.kind = map::Shape::Kind::Cylinder;
    post.radius = 2;
    const std::array<float, 3> o{0, 5, 0};
    const auto close_to = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
    check(close_to(map::footprint_distance(wall, o, 0, 3, 0), 1) && close_to(map::footprint_distance(wall, o, 0, 0, 3), 2) &&
              map::footprint_distance(wall, o, 0, 1.9f, 0.9f) == 0 && close_to(map::footprint_distance(wall, o, 0, 5, 5), 5),
          "footprint_distance: to a box's edge, 0 inside, to its corner");
    const float quarter = std::numbers::pi_v<float> / 2;
    check(close_to(map::footprint_distance(wall, o, quarter, 0, 3), 1) && close_to(map::footprint_distance(wall, o, quarter, 3, 0), 2),
          "footprint_distance: a box turned by yaw (its long side now along z)");
    check(close_to(map::footprint_distance(post, o, 0, 5, 0), 3) && map::footprint_distance(post, o, 0, 1, 1) == 0,
          "footprint_distance: to a cylinder's edge, 0 inside");
    const auto corners = map::footprint(wall, o, quarter);
    check(corners.size() == 4 && close_to(std::fabs(corners[0][1]), 2) && close_to(std::fabs(corners[0][0]), 1) &&
              map::footprint(post, o, 0).size() == 16,
          "footprint: a box's 4 corners turned by yaw, a cylinder's 16 points");
}

constexpr const char* kObjectsManifest = R"({"version":1,"id":1,"tiles":[[0,0,1,0]],"spawn":{"pos":[10,0,10]}})";
constexpr const char* kShrineFile = R"({"version":1,"interactables":[
    {"id":7,"pos":[100,0,100],"shape":{"box":{"halfExtents":[1,1,1]}},"event":"shrine.activated","data":{"msg":"hi"}}]})";

// Maps::load: interactables files of owned tiles, every problem refusing the start with the file.
void checkInteractableLoading() {
    const auto errors = [](const std::vector<std::pair<std::string, std::string>>& files) {
        LuaRoot r;
        r.write("m/map.json", kObjectsManifest);
        for (const auto& [path, body] : files) r.write("m/.server/interactables/" + path, body);
        std::vector<std::string> errs;
        world::Maps::load(r.root, errs);
        return joined(errs);
    };
    LuaRoot r;
    r.write("m/map.json", kObjectsManifest).write("m/.server/interactables/0_0.json", kShrineFile);
    std::vector<std::string> errs;
    const auto maps = world::Maps::load(r.root, errs);
    const auto* shrine = maps.interactable(1, 7);
    check(errs.empty() && shrine && shrine->event == "shrine.activated" && !maps.interactable(1, 8) && !maps.interactable(2, 7),
          "interactables load by map and id " + joined(errs));
    const std::string well = R"({"version":1,"interactables":[{"id":7,"pos":[300,0,10],"shape":{"cylinder":{"radius":1,"height":1}},"event":"w"}]})";
    check(errors({{"0_0.json", kShrineFile}, {"1_0.json", well}}) ==
              "maps/m/.server/interactables/1_0.json: interactable 7: duplicate id on this map",
          "interactables: an id used twice on a map is refused: " + errors({{"0_0.json", kShrineFile}, {"1_0.json", well}}));
    check(errors({{"shrine.json", kShrineFile}}) == "maps/m/.server/interactables/shrine.json: file name must be <tx>_<tz>.json",
          "interactables: a bad file name is refused");
    check(errors({{"5_5.json", kShrineFile}}) == "maps/m/.server/interactables/5_5.json: tile is not on the map",
          "interactables: a file for a tile not on the map is refused");
    check(errors({{"0_0.json", "{nope"}}) == "maps/m/.server/interactables/0_0.json: not valid JSON", "interactables: bad JSON refused");
    check(errors({{"1_0.json", kShrineFile}}) == "maps/m/.server/interactables/1_0.json: interactable 7: \"pos\" is outside tile 1_0",
          "interactables: a parse error is refused with the file: " + errors({{"1_0.json", kShrineFile}}));
}

// Interact: in range on the player's map publishes the object's event {entity, map, object, data}; unknown id, another
// map's object and out of range publish nothing and log at debug.
void checkInteract() {
    LogCapture capture;
    log::set_level(log::Level::debug);
    LuaRoot r;
    r.write("components/report.lua", kReport);
    r.write("systems/shrine.lua", R"(
local got = {}
return {
    init = function()
        chthonic.subscribe("shrine.activated", function(e)
            got[#got + 1] = string.format("%d %d %d %s", e.entity, e.map, e.object, e.data and e.data.msg or "nil")
        end)
        chthonic.subscribe("well.used", function(e)
            got[#got + 1] = string.format("%d %d %d %s", e.entity, e.map, e.object, tostring(e.data))
        end)
    end,
    onEnter = function(e) chthonic.add(e, "Report") end,
    update = function(dt) for e in chthonic.query("Report") do chthonic.set(e, "Report", {lines = got}) end end,
})");
    world::Maps maps = test_maps();
    map::Interactable shrine;
    shrine.id = 7;
    shrine.pos = {100, 0, 100};
    shrine.shape.halfExtents = {1, 1, 1};
    shrine.event = "shrine.activated";
    shrine.data = {{"msg", "hi"}};
    maps.add_interactable(1, shrine);
    map::Interactable well = shrine;
    well.id = 8;
    well.pos = {200, 0, 100};
    well.shape.kind = map::Shape::Kind::Cylinder;
    well.shape.radius = 2;
    well.range = 1;
    well.event = "well.used";
    well.data = nullptr;
    maps.add_interactable(1, well);
    map::Interactable cave = shrine;
    cave.id = 9;
    maps.add_interactable(2, cave);
    TestWorld w(r.defs().defs, {}, std::move(maps));
    check(w.world.load_scripts(r.root).empty(), "interact: system loads");
    const auto alice = w.spawn(1, 41, "Alice", 103.9f, 100);  // 2.9 m from the shrine's edge: in range 3
    const auto bob = w.spawn(2, 42, "Bob", 104.1f, 100);      // 3.1 m: out of range (and far from the well)
    w.world.tick(T0);
    w.world.push_interact(alice, 7);
    w.world.push_interact(alice, 9);    // map 2's
    w.world.push_interact(alice, 1234); // nobody's
    w.world.push_interact(bob, 7);
    w.world.push_interact(bob, 8);      // 94 m from the well
    w.world.tick(T0 + std::chrono::milliseconds(50));
    w.world.tick(T0 + std::chrono::milliseconds(100));
    const Json lines = w.value(bob, "Report").value("lines", Json::array());
    check(lines == Json::array({std::to_string(wire(alice)) + " 1 7 hi"}),
          "interact: only Alice's in-range activation publishes {entity, map, object, data}: " + lines.dump());
    check(said_at("interact rejected").size() == 4 && logged("\"reason\":\"not on this map\"") && logged("\"object\":1234") &&
              logged("\"reason\":\"out of range\""),
          "interact: the 4 rejections are logged at debug");
    // The well (no data): data nil in Lua.
    const auto carol = w.spawn(3, 43, "Carol", 202.5f, 100);
    w.world.tick(T0 + std::chrono::milliseconds(150));
    w.world.push_interact(carol, 8);
    w.world.tick(T0 + std::chrono::milliseconds(200));
    w.world.tick(T0 + std::chrono::milliseconds(250));
    const Json more = w.value(carol, "Report").value("lines", Json::array());
    check(more.size() == 2 && more[1] == std::to_string(wire(carol)) + " 1 8 nil", "interact: no data -> e.data nil: " + more.dump());
    log::set_level(log::Level::info);
    for (const auto& s : w.world.script_status()) check(s.ok, "interact: system still enabled " + s.error);
}

// The bake cuts obstacles and interactables (box and cylinder, a neighbour tile's too) out of the navmesh, with
// agentRadius clearance; visuals stay walkable. Then a move into one is off_mesh, and paths go round.
void checkObstacleNavmesh() {
    std::string err;
    const auto info = map::parse_manifest(nlohmann::json::parse(
        R"({"version":1,"id":1,"tiles":[[0,0,1,0]],"heightRange":[0,10],"spawn":{"pos":[10,0,10]}})"), err);
    const auto objects = map::parse_objects(nlohmann::json::parse(R"({"version":1,"objects":[
        {"type":"obstacle","prefab":"w.prefab","pos":[128,0,128],"shape":{"box":{"halfExtents":[2,2,40]}}},
        {"type":"interactable","prefab":"s.prefab","pos":[60,0,60],"id":1,"shape":{"cylinder":{"radius":3,"height":2}}},
        {"type":"visual","prefab":"g.prefab","pos":[200,0,200]},
        {"type":"obstacle","prefab":"r.prefab","pos":[254,0,60],"yaw":0.785,"shape":{"box":{"halfExtents":[4,1,4]}}}]})"), {0, 0});
    check(objects.errors.empty() && objects.objects.size() == 4, "obstacle bake: fixture objects parse " + joined(objects.errors));
    const map::TileHeights flat;
    LuaRoot r;
    r.write("m/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,0]],"heightRange":[0,10],"spawn":{"pos":[10,0,10]}})");
    int plainPolys = 0;
    for (int tx : {0, 1}) {
        const auto plain = mapbake::bake_tile(*info, flat, {tx, 0}, err);
        // Tile 1_0 gets tile 0_0's objects too: the rock at x 254 reaches over the seam (the mapbake CLI passes neighbours').
        const auto baked = mapbake::bake_tile(*info, flat, {tx, 0}, err, objects.objects);
        check(plain && baked && baked->polys > plain->polys, std::format("obstacle bake: tile {}_0 has more polys with its "
              "obstacles cut out ({} against {}) {}", tx, baked ? baked->polys : 0, plain ? plain->polys : 0, err));
        if (tx == 0 && plain) plainPolys = plain->polys;
        if (baked) r.write(std::format("m/.server/nav/{}_0.navtile", tx), baked->navtile);
    }
    std::vector<std::string> errors;
    world::Maps maps = world::Maps::load(r.root, errors);
    check(errors.empty() && plainPolys > 0, "obstacle bake: navtiles load " + joined(errors));
    const auto on = [&](float x, float z) { return maps.nav().on_mesh(1, {x, 0, z}, 6); };
    check(!on(128, 128) && !on(129.5f, 160) && on(133, 128), "obstacle bake: the wall's box is off the mesh, 3 m beside it on");
    check(!on(60, 60) && !on(62, 60) && on(66, 60), "obstacle bake: the interactable's cylinder is off the mesh, beyond it on");
    check(on(200, 200) && on(100, 30), "obstacle bake: a visual and open ground stay walkable");
    check(!on(254, 60) && !on(257, 60) && on(266, 60), "obstacle bake: a turned box over the tile seam is cut from both tiles");
    const auto path = maps.nav().path(1, {100, 0, 128}, {156, 0, 128});
    bool around = false, through = false;
    for (const auto& p : path) {
        around |= std::fabs(p.z - 128) > 40;
        through |= std::fabs(p.x - 128) < 2 && std::fabs(p.z - 128) < 40;
    }
    check(path.size() > 2 && around && !through, std::format("obstacle bake: the path goes round the wall ({} points)", path.size()));

    const auto corrections = [&](proto::Vec3 from, proto::Vec3 to) {
        std::vector<std::string> errs;
        TestWorld w({}, {}, world::Maps::load(r.root, errs));
        const auto e = w.world.spawn(1, {1, 1, "Walker", 1, from, 0, ""}, T0);
        w.world.tick(T0);
        w.sent.clear();
        w.move(e, {to, 0, 0});
        w.world.tick(T0 + std::chrono::seconds(1));
        return w.sent.of<proto::Correction>(1).size();
    };
    check(corrections({120, 0, 128}, {127, 0, 128}) == 1, "move into the wall: off_mesh -> Correction");
    check(corrections({120, 0, 100}, {124, 0, 100}) == 0, "move beside the wall: accepted");
}

// --- instance maps (docs/world/instances.html) ---

// map.json "type" / "maxPlayers" / "emptyTimeoutSec" / "exit": parsed in chthonic_map, every bad value or misplaced field
// refused with the field named; Maps::load checks the exit against the other maps, naming the file.
void checkInstanceManifests() {
    std::string err;
    const auto inst = map::parse_manifest(Json::parse(R"({"version":1,"id":6,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},
        "type":"instance","maxPlayers":5,"emptyTimeoutSec":0,"exit":{"map":1,"pos":[10,0,20],"heading":1.5}})"), err);
    check(inst && inst->instanced() && inst->maxPlayers == 5u && inst->emptyTimeoutSec == 0 && inst->exit &&
              inst->exit->map == 1 && inst->exit->pos == std::array<float, 3>{10, 0, 20} && inst->exit->heading == 1.5f,
          "instance manifest parses: " + err);
    const auto defaults = map::parse_manifest(Json::parse(R"({"version":1,"id":6,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},
        "type":"instance","exit":{"map":1,"pos":[10,0,20]}})"), err);
    check(defaults && !defaults->maxPlayers && defaults->emptyTimeoutSec == 300 && defaults->exit->heading == 0,
          "instance defaults: no player limit, emptyTimeoutSec 300, exit heading 0");
    const auto world = map::parse_manifest(Json::parse(R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},"type":"world"})"), err);
    check(world && !world->instanced() && !world->exit, "\"type\": \"world\" is the default kind");

    const std::string base = R"("version":1,"id":6,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]})";
    const std::string exit = R"("exit":{"map":1,"pos":[10,0,20]})";
    const std::pair<std::string, const char*> bad[] = {
        {R"("type":"dungeon")", "\"type\" must be \"world\" or \"instance\""},
        {R"("type":3)", "\"type\" must be \"world\" or \"instance\""},
        {R"("maxPlayers":5)", "\"maxPlayers\" is only for instance maps"},
        {R"("type":"world","emptyTimeoutSec":5)", "\"emptyTimeoutSec\" is only for instance maps"},
        {exit, "\"exit\" is only for instance maps"},
        {R"("type":"instance")", "\"exit\" must be {\"map\": id, \"pos\": [x, y, z], \"heading\": h} (required on an instance map)"},
        {R"("type":"instance","exit":{"map":1})", "\"exit\" must be {"},
        {R"("type":"instance","exit":{"map":-1,"pos":[1,0,1]})", "\"exit\" must be {"},
        {R"("type":"instance","exit":{"map":1,"pos":[1,0,1],"heading":"n"})", "\"exit\" must be {"},
        {R"("type":"instance","maxPlayers":0,)" + exit, "\"maxPlayers\" must be an integer >= 1 (leave it out for no limit)"},
        {R"("type":"instance","maxPlayers":1.5,)" + exit, "\"maxPlayers\" must be an integer >= 1"},
        {R"("type":"instance","maxPlayers":"2",)" + exit, "\"maxPlayers\" must be an integer >= 1"},
        {R"("type":"instance","emptyTimeoutSec":-1,)" + exit, "\"emptyTimeoutSec\" must be an integer in [0, 31536000] (a year)"},
        {R"("type":"instance","emptyTimeoutSec":31536001,)" + exit, "\"emptyTimeoutSec\" must be an integer in [0, 31536000]"},
        {R"("type":"instance","emptyTimeoutSec":2.5,)" + exit, "\"emptyTimeoutSec\" must be an integer"},
    };
    for (const auto& [fields, want] : bad) {
        err.clear();
        const bool refused = !map::parse_manifest(Json::parse("{" + base + "," + fields + "}"), err);
        check(refused && contains(err, want), std::string("instance manifest refused: ") + want + " (got: " + err + ")");
    }

    // Maps::load: the exit is on a loaded world map's tiles, not in a portal there; START_MAP's file named.
    LuaRoot r;
    r.write("start/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,1]],"spawn":{"pos":[256,0,256]}})");
    r.write("start/areas.json", R"({"version":1,"areas":[{"id":"door","rect":[100,100,110,110],"portal":{"map":6,"pos":[50,0,50]}}]})");
    r.write("crypt/map.json", R"({"version":1,"id":6,"tiles":[[0,0,0,0]],"spawn":{"pos":[50,0,50]},"type":"instance",
        "maxPlayers":2,"exit":{"map":1,"pos":[200,0,200]}})");
    std::vector<std::string> errors;
    const auto ok = world::Maps::load(r.root, errors);
    check(errors.empty() && ok.info(6).instanced() && ok.info(6).maxPlayers == 2u, "instance map loads, a portal into it too: " + joined(errors));
    const std::pair<const char*, const char*> exits[] = {
        {R"({"map":9,"pos":[200,0,200]})", "maps/crypt/map.json: \"exit\": to unknown map 9"},
        {R"({"map":1,"pos":[900,0,200]})", "maps/crypt/map.json: \"exit\": position is not on a tile of map 1"},
        {R"({"map":1,"pos":[105,0,105]})", "maps/crypt/map.json: \"exit\": position is inside portal area \"door\" of map 1"},
        {R"({"map":6,"pos":[50,0,50]})", "maps/crypt/map.json: \"exit\": map 6 is an instance map; an exit must be on a world map"},
    };
    for (const auto& [to, want] : exits) {
        r.write("crypt/map.json", std::string(R"({"version":1,"id":6,"tiles":[[0,0,0,0]],"spawn":{"pos":[50,0,50]},"type":"instance","exit":)") + to + "}");
        errors.clear();
        world::Maps::load(r.root, errors);
        check(errors.size() == 1 && errors[0] == want, std::string("bad exit refused: ") + want + " (got: " + joined(errors) + ")");
    }
    r.write("crypt/map.json", R"({"version":1,"id":6,"tiles":[[0,0,0,0]],"spawn":{"pos":[50,0,50]},"maxPlayers":2})");
    errors.clear();
    world::Maps::load(r.root, errors);
    check(!errors.empty() && errors[0] == "maps/crypt/map.json: \"maxPlayers\" is only for instance maps (\"type\": \"instance\")",
          "misplaced field refused with the file named: " + joined(errors));
}

// Map 1 "start" and 2 "cave" (test_maps), 6 "crypt" (instance: 1 tile, maxPlayers 2, emptyTimeoutSec 10, exit on map
// 1 at (1000, 0, 1000) heading 1, a Bat spawn) and 7 "vault" (instance: no limit, emptyTimeoutSec 0, same exit).
world::Maps instance_maps() {
    world::Maps maps = test_maps();
    for (const auto& [id, name, extra] :
         {std::tuple{6, "crypt", R"("maxPlayers":2,"emptyTimeoutSec":10,)"}, std::tuple{7, "vault", R"("emptyTimeoutSec":0,)"}}) {
        std::string err;
        auto m = map::parse_manifest(Json::parse(std::format(
            R"({{"version":1,"id":{},"name":"{}","tiles":[[0,0,0,0]],"spawn":{{"pos":[128,0,128]}},"type":"instance",{}
                "exit":{{"map":1,"pos":[1000,0,1000],"heading":1}}}})", id, name, extra)), err);
        check(m.has_value(), "instance_maps: " + err);
        if (id == 6) m->spawns.push_back({"bat_1", "Bat", {100, 0, 100}, 0, -1, {}, "maps/crypt/.server/spawns/0_0.json"});
        maps.add(std::move(*m));
    }
    return maps;
}

constexpr const char* kInstanceSystem = R"lua(
local keys = {} -- by entity: what the test set with a "key" client event
return {
    init = function()
        local i = chthonic.map.instance()
        if i then chthonic.log(string.format("init copy %d key %s map %d", i.id, tostring(i.key), i.map)) end
        chthonic.subscribe("client.key", function(e) keys[e.entity] = e.data.key end)
        chthonic.subscribe("client.go", function(e) -- chthonic.transfer onto the crypt, with the client's instance = if any
            local ok = chthonic.transfer(e.entity, {map = e.data.map or 6, position = {x = 128, y = 0, z = 128}, instance = e.data.instance})
            chthonic.log(string.format("go %d %s", e.entity, tostring(ok)))
        end)
        chthonic.subscribe("client.whoami", function(e)
            local i = chthonic.map.instance()
            if i then chthonic.log(string.format("whoami %d: copy %d key %s map %d", e.entity, i.id, tostring(i.key), i.map))
            else chthonic.log(string.format("whoami %d: world map %d", e.entity, chthonic.get(e.entity, "Transform").map)) end
        end)
        chthonic.subscribe("instance.full", function(p)
            chthonic.log(string.format("full %d map %d key %s", p.entity, p.map, tostring(p.key)))
        end)
        chthonic.subscribe("client.api", function(e)
            raises("chthonic.world.post: map 6 is an instance map", function() chthonic.world.post(6, "x", {}) end)
            raises("chthonic.transfer: \"instance\" is only for instance maps; map 2 is a world map",
                   function() chthonic.transfer(e.entity, {map = 2, position = {x = 1, y = 0, z = 1}, instance = "a"}) end)
            raises('chthonic.transfer: "instance" must be a non-empty string of at most 255 bytes, got ',
                   function() chthonic.transfer(e.entity, {map = 6, position = {x = 1, y = 0, z = 1}, instance = ""}) end)
            raises('chthonic.transfer: "instance" must be a non-empty string of at most 255 bytes, got 5',
                   function() chthonic.transfer(e.entity, {map = 6, position = {x = 1, y = 0, z = 1}, instance = 5}) end)
            raises("chthonic.transfer: unknown option where", function() chthonic.transfer(e.entity, {map = 6, where = 1}) end)
            if chthonic.map.instance() then
                local npc = chthonic.create("Made", {template = "T", map = 6, position = {x = 50, y = 0, z = 50}})
                expect(chthonic.get(npc, "Transform").map == 6, "chthonic.create onto the copy's own map")
            else
                raises("chthonic.create: map 6 is an instance map; create its NPCs from inside the copy",
                       function() chthonic.create("X", {template = "T", map = 6}) end)
            end
            chthonic.log("api checked")
        end)
    end,
    instanceKey = function(e, map)
        chthonic.log(string.format("asked %d %d", e, map))
        return keys[e]
    end,
    shutdown = function()
        local i = chthonic.map.instance()
        if i then chthonic.log(string.format("shutdown copy %d key %s", i.id, tostring(i.key))) end
    end,
    update = function() end,
}
)lua";

// Entering an instance map: no context at startup; by key the same copy or a new one (nil: always new), each with its
// own NPCs and Lua state (init / shutdown per copy); chthonic.transfer's instance = skips the hook; a full copy refuses
// (instance.full, no MapChange, chthonic.transfer false); relog rejoins a live copy with room, else the map's exit; the empty
// timer destroys a copy (re-entry cancels it; 0: at the next serial point); chthonic.map.instance; ids recycled.
void checkInstances() {
    LogCapture capture;
    LuaRoot r;
    r.write("npcs/bat.lua", "return { name = 'Bat', template = 'BatTpl' }");
    r.write("systems/inst.lua", std::string(kLuaExpect) + kInstanceSystem);
    world::WorldRules rules;
    rules.firstInstanceId = 1;
    TestWorld w({}, rules, instance_maps());
    check(w.world.load_scripts(r.root).empty(), "instances: loads");
    const auto contexts = [&] {
        std::vector<std::string> out;
        for (const auto& c : w.world.contexts())
            out.push_back(std::format("{}/{}{}:{}", c.map, c.instance, c.key ? "/" + *c.key : "", c.players));
        return joined(out);
    };
    check(contexts() == "1/0:0", "instances: only START_MAP's context at startup, no copy: " + contexts());
    TimePoint now = T0;
    const auto tick = [&] { w.world.tick(now += std::chrono::milliseconds(50)); };
    const auto event = [&](entt::entity e, const std::string& name, nlohmann::json data = nlohmann::json::object()) {
        w.world.push_client_event(e, name, std::move(data));
    };
    const auto inst = [&](entt::entity e) {
        entt::registry* reg = w.world.registry_of(e);
        return reg ? std::format("{}/{}", reg->get<world::MapRef>(e).map, reg->get<world::MapRef>(e).instance) : "gone";
    };
    const auto ann = w.spawn(1, 1, "Ann", 1000, 1000), bob = w.spawn(2, 2, "Bob", 1001, 1000),
               cid = w.spawn(3, 3, "Cid", 1002, 1000), dan = w.spawn(4, 4, "Dan", 1003, 1000);
    for (const auto& [e, key] : {std::pair{ann, "a"}, {bob, "a"}, {cid, "b"}, {dan, "a"}}) event(e, "key", {{"key", key}});
    tick();
    w.sent.clear();

    // Ann: the hook says "a": a new copy, id 1, its own init and Bat.
    event(ann, "go");
    tick();
    check(said(std::format("asked {} 6", wire(ann))) && said("init copy 1 key a map 6"), "instances: hook asked; copy 1 created, its init ran");
    check(inst(ann) == "6/1" && contexts() == "1/0:3; 6/1/a:1", "instances: Ann in copy 1 (key a): " + contexts());
    const auto annMc = w.sent.of<proto::MapChange>(1);
    check(annMc.size() == 1 && annMc[0].map == 6 && annMc[0].pos == proto::Vec3{128, 0, 128}, "instances: MapChange{6}");
    tick();
    const auto bats = [&](uint32_t instance) {
        std::vector<uint32_t> out;
        if (auto* c = w.world.copy(6, instance))
            for (auto [e, npc] : c->registry().view<world::Npc>().each()) out.push_back(wire(e));
        return out;
    };
    check(bats(1).size() == 1, "instances: copy 1 spawned its own Bat");

    // Bob, same key: the same copy, and Ann and Bob see each other. Cid, key b: a new copy, apart, with its own Bat.
    event(bob, "go");
    event(cid, "go");
    tick();
    tick();
    check(inst(bob) == "6/1" && inst(cid) == "6/2" && contexts() == "1/0:1; 6/1/a:2; 6/2/b:1",
          "instances: same key -> same copy, other key -> a new one: " + contexts());
    const auto seen = [&](uint32_t sid, entt::entity other) {
        for (const auto& s : w.sent.of<proto::Spawn>(sid)) if (s.entityId == wire(other)) return true;
        return false;
    };
    check(seen(1, bob) && seen(2, ann) && !seen(3, ann) && !seen(3, bob) && !seen(1, cid), "instances: copies share no players");
    check(bats(2).size() == 1 && bats(2) != bats(1), "instances: copy 2 has its own Bat, not copy 1's");
    for (uint32_t bat : bats(2)) check(!seen(1, entt::entity{bat}) && !seen(2, entt::entity{bat}), "instances: copies share no NPCs");

    // Dan, key a: copy 1 is full (2): refused at the request, no MapChange, still on map 1, instance.full.
    w.sent.clear();
    event(dan, "go");
    tick();
    tick();
    check(said(std::format("go {} false", wire(dan))) && said(std::format("full {} map 6 key a", wire(dan))) &&
              logged("transfer refused: instance full") && w.sent.of<proto::MapChange>(4).empty() && inst(dan) == "1/0",
          "instances: a full copy refuses: chthonic.transfer false, instance.full, no MapChange, the player stays");
    // instance = "b": the hook is not asked; copy 2.
    clear_log();
    event(dan, "go", {{"instance", "b"}});
    tick();
    check(!said(std::format("asked {}", wire(dan))) && said(std::format("go {} true", wire(dan))) && inst(dan) == "6/2",
          "instances: chthonic.transfer instance = picks the copy without asking the hook");

    // No key: always a new copy (Eve, Fay). The C++ transfer refuses a full copy too.
    const auto eve = w.spawn(5, 5, "Eve", 1000, 1000), fay = w.spawn(6, 6, "Fay", 1000, 1000);
    event(eve, "go");
    event(fay, "go");
    tick();
    const std::string eveAt = inst(eve); // copies 3 and 4: Eve's and Fay's events reach Lua in either order
    check(std::set{eveAt, inst(fay)} == std::set<std::string>{"6/3", "6/4"}, "instances: nil key -> a new keyless copy each: " + contexts());
    check(!w.world.transfer(eve, {6, {128, 0, 128}, 0, "a"}) && inst(eve) == eveAt, "instances: World::transfer into a full copy refused");

    // chthonic.map.instance(), and the Lua surface's refusals, in a copy and on a world map.
    clear_log();
    const auto gus = w.spawn(7, 7, "Gus", 1000, 1000);
    for (auto e : {ann, eve, gus}) {
        event(e, "whoami");
        event(e, "api");
    }
    tick();
    check(said(std::format("whoami {}: copy 1 key a map 6", wire(ann))) && said(std::format("whoami {}: copy {} key nil map 6", wire(eve), eveAt.substr(2))) &&
              said(std::format("whoami {}: world map 1", wire(gus))),
          "instances: chthonic.map.instance() is {id, key, map} in a copy, nil on a world map");
    check(said_at("api checked").size() == 3, "instances: Lua refusals checked in 3 contexts");
    check_lua_expectations("instances", "api checked");

    // Relog: Ann's save names copy 1; she rejoins it. Saved in a full copy, a gone one or none at all: the exit.
    w.sent.clear();
    w.world.despawn(ann);
    const db::CharSave annSave = w.sent.saves.back();
    check(annSave.mapId == 6 && annSave.instanceId == 1, "instances: the save carries map 6, instance 1");
    const auto row = [](uint64_t id, const std::string& name, uint16_t map, uint32_t instance) {
        db::CharRow r{id, 1, name, map, {120, 0, 120}, 0, "", {}};
        r.instanceId = instance;
        return r;
    };
    const entt::entity ann2 = w.world.spawn(11, row(1, "Ann", 6, 1), now);
    const auto ok11 = w.sent.of<proto::EnterWorldOk>(11);
    check(inst(ann2) == "6/1" && ok11.size() == 1 && ok11[0].map == 6 && ok11[0].pos == proto::Vec3{120, 0, 120},
          "instances: relog into a live copy with room rejoins it at the saved position");
    const auto exitOf = [&](uint32_t sid) {
        const auto ok = w.sent.of<proto::EnterWorldOk>(sid);
        return ok.size() == 1 && ok[0].map == 1 && ok[0].pos == proto::Vec3{1000, 0, 1000} && ok[0].heading == 1;
    };
    clear_log();
    const entt::entity hal = w.world.spawn(12, row(8, "Hal", 6, 1), now); // copy 1 has Ann and Bob
    const entt::entity ivy = w.world.spawn(13, row(9, "Ivy", 6, 999), now); // a previous run's, or a destroyed copy's
    const entt::entity jon = w.world.spawn(14, row(10, "Jon", 6, 0), now);  // an old row
    check(exitOf(12) && exitOf(13) && exitOf(14) && inst(hal) == "1/0" && inst(ivy) == "1/0" && inst(jon) == "1/0",
          "instances: relog into a full, an unknown or no copy -> the map's exit");
    check(logged(R"("reason":"instance full")") && logged(R"("reason":"instance gone")"), "instances: the exit is logged with why");

    // The empty timer (10 s on the crypt): Cid and Dan leave copy 2; Cid comes back after 5 s (cancelled); both
    // leave, and 10 s later copy 2 is destroyed: shutdown, NPCs gone, key b free (the next b is a new copy).
    for (auto e : {cid, dan}) w.world.transfer(e, {1, {1000, 0, 1000}, 0});
    tick();
    check(w.world.copy(6, 2) && w.world.copy(6, 2)->empty_since(), "instances: copy 2 empty: its timer runs");
    now += std::chrono::seconds(5);
    check(w.world.transfer(cid, {6, {128, 0, 128}, 0, "b"}) && inst(cid) == "6/2", "instances: back into copy 2 by key before the timeout");
    tick();
    check(!w.world.copy(6, 2)->empty_since(), "instances: re-entry cancels the timer");
    now += std::chrono::seconds(8);
    tick();
    check(w.world.copy(6, 2) != nullptr, "instances: 13 s after the first leave, the cancelled timer has not fired");
    w.world.transfer(cid, {1, {1000, 0, 1000}, 0});
    now += std::chrono::milliseconds(9900);
    tick();
    check(w.world.copy(6, 2) != nullptr, "instances: not yet 10 s empty");
    clear_log();
    now += std::chrono::milliseconds(200);
    tick();
    check(w.world.copy(6, 2) == nullptr && said("shutdown copy 2 key b") && logged("instance destroyed"),
          "instances: 10 s empty -> destroyed at the serial point, its shutdown run");
    check(w.world.transfer(cid, {6, {128, 0, 128}, 0, "b"}) && inst(cid) == "6/5", "instances: key b again: a new copy (5)");

    // emptyTimeoutSec 0: gone at the next serial point after the last player leaves.
    check(w.world.transfer(gus, {7, {128, 0, 128}, 0}) && inst(gus) == "7/6", "instances: the vault copy 6");
    check(w.world.transfer(gus, {1, {1000, 0, 1000}, 0}) && w.world.copy(7, 6) == nullptr,
          "instances: emptyTimeoutSec 0 -> destroyed at the serial point right after the last player left");

    // 600 copies made and destroyed one after another: their entity ids go back, so the 2^20 indices never run out.
    bool threw = false;
    try {
        for (int i = 0; i < 600; ++i) {
            w.world.transfer(gus, {7, {128, 0, 128}, 0});
            tick(); // the copy updates: it takes its id blocks
            w.world.create_npc("n", "T", 1, std::nullopt, 0); // ids used on the world map meanwhile too
            w.world.transfer(gus, {1, {1000, 0, 1000}, 0});
        }
    } catch (const std::exception& e) {
        threw = true;
        check(false, std::string("instances: 600 copies: ") + e.what());
    }
    check(!threw && inst(gus) == "1/0" && contexts() == "1/0:5; 6/1/a:2; 6/3:1; 6/4:1; 6/5/b:1",
          "instances: 600 vault copies come and go: " + contexts());
    for (const auto& s : w.world.script_status()) check(s.ok, "instances: system still enabled " + s.error);
}

// cache_lua_files (chthonic_world's): a second World (as a new copy would) loads what the first read, even after the file
// changed on disk; off again, the disk.
void checkLuaFileCache() {
    LogCapture capture;
    LuaRoot r;
    r.write("systems/a.lua", "return { init = function() chthonic.log('version one') end, update = function() end }");
    lua::cache_lua_files();
    { TestWorld w({}); check(w.world.load_scripts(r.root).empty(), "file cache: loads"); }
    r.write("systems/a.lua", "return { init = function() chthonic.log('version two') end, update = function() end }");
    r.write("systems/b.lua", "return { init = function() chthonic.log('new file') end, update = function() end }");
    { TestWorld w({}); check(w.world.load_scripts(r.root).empty(), "file cache: loads again"); }
    check(said_at("version one").size() == 2 && !said("version two") && !said("new file"),
          "file cache: on, the second load reads the kept text and folder listing");
    lua::cache_lua_files(false);
    { TestWorld w({}); check(w.world.load_scripts(r.root).empty(), "file cache: loads off"); }
    check(said("version two") && said("new file"), "file cache: off, the disk again");
}

// instanceKey errors and bad results disable only that system; the next system's key is used.
void checkInstanceKeyErrors() {
    LogCapture capture;
    LuaRoot r;
    r.write("npcs/bat.lua", "return { name = 'Bat', template = 'BatTpl' }");
    r.write("systems/a_bad.lua", "return { instanceKey = function(e, map) return 5 end, update = function() end }");
    r.write("systems/b_long.lua", "return { instanceKey = function(e, map) return string.rep('k', 256) end, update = function() end }");
    r.write("systems/c_good.lua", R"(return {
    init = function() chthonic.subscribe("client.go", function(e) chthonic.transfer(e.entity, 6, {x = 128, y = 0, z = 128}) end) end,
    instanceKey = function(e, map) return "good" end, update = function() end })");
    world::WorldRules rules;
    rules.firstInstanceId = 40;
    TestWorld w({}, rules, instance_maps());
    check(w.world.load_scripts(r.root).empty(), "instanceKey errors: loads");
    const auto a = w.spawn(1, 1, "Ann", 1000, 1000);
    w.world.push_client_event(a, "go", nlohmann::json::object());
    w.world.tick(T0);
    const auto status = w.world.script_status();
    check(status.size() == 3 && !status[0].ok && contains(status[0].error, "instanceKey must return nil or a non-empty string") &&
              !status[1].ok && status[2].ok,
          "instanceKey errors: a number and a 256-byte key disable their systems");
    check(w.world.copy(6, 40) && w.world.copy(6, 40)->key() == "good", "instanceKey errors: the next system's key picks the copy (id 40)");
}

// Instance ids never wrap: the copy past UINT32_MAX is refused like a full one (instance.full, the player stays put).
void checkInstanceIdsExhausted() {
    LogCapture capture;
    LuaRoot r;
    r.write("npcs/bat.lua", "return { name = 'Bat', template = 'BatTpl' }");
    r.write("systems/go.lua", R"(return {
    init = function()
        chthonic.subscribe("client.go", function(e) chthonic.transfer(e.entity, 6, {x = 128, y = 0, z = 128}) end)
        chthonic.subscribe("instance.full", function(e) chthonic.log("full " .. e.entity) end)
    end,
    update = function() end })");
    world::WorldRules rules;
    rules.firstInstanceId = UINT32_MAX;
    TestWorld w({}, rules, instance_maps());
    check(w.world.load_scripts(r.root).empty(), "instance ids exhausted: loads");
    const auto a = w.spawn(1, 1, "Ann", 1000, 1000);
    const auto b = w.spawn(2, 2, "Bob", 1010, 1000);
    w.world.push_client_event(a, "go", nlohmann::json::object());
    w.world.tick(T0);
    check(w.world.copy(6, UINT32_MAX) && w.world.map_of(a) == 6, "instance ids exhausted: the last id makes a copy");
    w.world.push_client_event(b, "go", nlohmann::json::object());
    w.world.tick(T0 + std::chrono::milliseconds(50));
    w.world.tick(T0 + std::chrono::milliseconds(100));
    check(w.world.map_of(b) == 1 && w.world.contexts().size() == 2, "instance ids exhausted: no copy past UINT32_MAX");
    check(log_index("instance ids exhausted") != SIZE_MAX && log_index("full ") != SIZE_MAX,
          "instance ids exhausted: logged, instance.full published");
}

constexpr const char* kGoSystem = R"(return {
    init = function()
        chthonic.subscribe("client.go", function(e) chthonic.transfer(e.entity, 6, {x = 128, y = 0, z = 128}) end)
        chthonic.subscribe("instance.full", function(e) chthonic.log("full " .. e.entity) end)
    end,
    update = function() end })";

// Keyless copies take small id blocks: 300 live at once fit (4096-id blocks ran out of entity ids at ~128).
void checkManyCopies() {
    LogCapture capture;
    LuaRoot r;
    r.write("npcs/bat.lua", "return { name = 'Bat', template = 'BatTpl' }");
    r.write("systems/go.lua", kGoSystem);
    TestWorld w({}, {}, instance_maps());
    check(w.world.load_scripts(r.root).empty(), "many copies: loads");
    std::vector<entt::entity> players;
    for (uint32_t i = 0; i < 300; ++i) {
        players.push_back(w.spawn(i + 1, i + 1, "P" + std::to_string(i), 1000.0f + float(i % 20), 1000.0f + float(i / 20)));
        w.world.push_client_event(players.back(), "go", nlohmann::json::object());
    }
    w.world.tick(T0);
    check(w.world.contexts().size() == 301 && w.world.map_of(players.back()) == 6, "many copies: 300 keyless copies live");
}

// Entity ids low: a new copy is refused like a full one (logged, instance.full), never a crash; the world map goes on.
void checkEntityIdsLow() {
    LogCapture capture;
    LuaRoot r;
    r.write("npcs/bat.lua", "return { name = 'Bat', template = 'BatTpl' }");
    r.write("systems/go.lua", kGoSystem);
    TestWorld w({}, {}, instance_maps());
    check(w.world.load_scripts(r.root).empty(), "entity ids low: loads");
    const auto a = w.spawn(1, 1, "Ann", 1000, 1000);
    while (w.world.shared().room_for_copy()) w.world.shared().block(world::Shared::kIdBlock);
    w.world.push_client_event(a, "go", nlohmann::json::object());
    w.world.tick(T0);
    w.world.tick(T0 + std::chrono::milliseconds(50));
    check(w.world.map_of(a) == 1 && w.world.contexts().size() == 1, "entity ids low: no new copy");
    check(log_index("entity ids low") != SIZE_MAX && log_index("full ") != SIZE_MAX,
          "entity ids low: logged, instance.full published");
    const auto b = w.spawn(2, 2, "Bob", 1010, 1000);
    w.world.tick(T0 + std::chrono::milliseconds(100));
    check(w.world.map_of(b) == 1, "entity ids low: the world map still takes players");
}

// --- layers (docs/world/maps.html#layers) ---

// map.json "layerCap": world maps only, an integer >= 1; emptyTimeoutSec allowed with it; errors name the field (and,
// from Maps::load, the file).
void checkLayerManifests() {
    std::string err;
    const auto layered = map::parse_manifest(Json::parse(R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},
        "layerCap":500,"emptyTimeoutSec":60})"), err);
    check(layered && !layered->instanced() && layered->layerCap == 500u && layered->emptyTimeoutSec == 60,
          "layer manifest parses: " + err);
    const auto defaults = map::parse_manifest(Json::parse(R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]},"layerCap":1})"), err);
    check(defaults && defaults->layerCap == 1u && defaults->emptyTimeoutSec == 300, "layer defaults: emptyTimeoutSec 300");
    const auto plain = map::parse_manifest(Json::parse(R"({"version":1,"id":1,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]}})"), err);
    check(plain && !plain->layerCap, "no layerCap: no layers");

    const std::string base = R"("version":1,"id":6,"tiles":[[0,0,0,0]],"spawn":{"pos":[1,0,1]})";
    const std::pair<std::string, const char*> bad[] = {
        {R"("layerCap":0)", "\"layerCap\" must be an integer >= 1 (leave it out for one layer)"},
        {R"("layerCap":1.5)", "\"layerCap\" must be an integer >= 1"},
        {R"("layerCap":"2")", "\"layerCap\" must be an integer >= 1"},
        {R"("layerCap":4294967296)", "\"layerCap\" must be an integer >= 1"},
        {R"("type":"instance","layerCap":2,"exit":{"map":1,"pos":[10,0,20]})", "\"layerCap\" is only for world maps; an instance map uses \"maxPlayers\""},
        {R"("emptyTimeoutSec":5)", "\"emptyTimeoutSec\" is only for instance maps (\"type\": \"instance\") or layered world maps (\"layerCap\")"},
        {R"("layerCap":2,"emptyTimeoutSec":-1)", "\"emptyTimeoutSec\" must be an integer in [0, 31536000] (a year)"},
        {R"("layerCap":2,"maxPlayers":2)", "\"maxPlayers\" is only for instance maps"},
    };
    for (const auto& [fields, want] : bad) {
        err.clear();
        const bool refused = !map::parse_manifest(Json::parse("{" + base + "," + fields + "}"), err);
        check(refused && contains(err, want), std::string("layer manifest refused: ") + want + " (got: " + err + ")");
    }
    LuaRoot r;
    r.write("start/map.json", R"({"version":1,"id":1,"tiles":[[0,0,1,1]],"spawn":{"pos":[256,0,256]},"layerCap":2})");
    r.write("crypt/map.json", R"({"version":1,"id":6,"tiles":[[0,0,0,0]],"spawn":{"pos":[50,0,50]},"type":"instance",
        "layerCap":2,"exit":{"map":1,"pos":[200,0,200]}})");
    std::vector<std::string> errors;
    world::Maps::load(r.root, errors);
    check(errors.size() == 1 && errors[0] == "maps/crypt/map.json: \"layerCap\" is only for world maps; an instance map uses \"maxPlayers\"",
          "layerCap on an instance map refused with the file named: " + joined(errors));
}

// test_maps plus map 3 "fields": layered (layerCap 2, emptyTimeoutSec 10), 2x2 tiles, a Bat spawn at (100, 0, 100).
world::Maps layer_maps() {
    world::Maps maps = test_maps();
    std::string err;
    auto m = map::parse_manifest(Json::parse(R"({"version":1,"id":3,"name":"fields","tiles":[[0,0,1,1]],
        "spawn":{"pos":[256,0,256]},"layerCap":2,"emptyTimeoutSec":10})"), err);
    check(m.has_value(), "layer_maps: " + err);
    m->spawns.push_back({"bat_1", "Bat", {100, 0, 100}, 0, -1, {}, "maps/fields/.server/spawns/0_0.json"});
    maps.add(std::move(*m));
    return maps;
}

constexpr const char* kLayerSystem = R"lua(
local picks, loginPick = {}, nil -- layerFor's answers: by entity (a "pick" client event), and for a login (e nil)
return {
    init = function()
        local i = chthonic.map.instance()
        if i and i.layer then chthonic.log(string.format("init layer %d base %s map %d", i.id, tostring(i.base), i.map)) end
        chthonic.subscribe("client.pick", function(e) picks[e.entity] = e.data.layer end)
        chthonic.subscribe("client.loginpick", function(e) loginPick = e.data.layer end)
        chthonic.subscribe("client.go", function(e) -- onto map 3 (or data.map), with the client's layer = if any
            local ok = chthonic.transfer(e.entity, {map = e.data.map or 3, position = {x = 100, y = 0, z = 110}, layer = e.data.layer})
            chthonic.log(string.format("go %d %s", e.entity, tostring(ok)))
        end)
        chthonic.subscribe("client.whoami", function(e)
            local i = chthonic.map.instance()
            if i then chthonic.log(string.format("whoami %d: map %d id %d layer %s base %s", e.entity, i.map, i.id, tostring(i.layer), tostring(i.base)))
            else chthonic.log(string.format("whoami %d: nil", e.entity)) end
        end)
        chthonic.subscribe("instance.full", function(p)
            chthonic.log(string.format("full %d map %d layer %s", p.entity, p.map, tostring(p.layer)))
        end)
        chthonic.subscribe("client.api", function(e)
            raises('chthonic.transfer: "layer" is only for layered world maps ("layerCap"); map 2 has none',
                   function() chthonic.transfer(e.entity, {map = 2, position = {x = 1, y = 0, z = 1}, layer = 0}) end)
            raises('chthonic.transfer: "layer" must be an integer in [0, 4294967295], got -1',
                   function() chthonic.transfer(e.entity, {map = 3, position = {x = 1, y = 0, z = 1}, layer = -1}) end)
            raises('chthonic.transfer: "layer" must be an integer in [0, 4294967295], got x',
                   function() chthonic.transfer(e.entity, {map = 3, position = {x = 1, y = 0, z = 1}, layer = "x"}) end)
            raises('chthonic.transfer: "instance" is only for instance maps; map 3 is a world map',
                   function() chthonic.transfer(e.entity, {map = 3, position = {x = 1, y = 0, z = 1}, instance = "a"}) end)
            chthonic.log("layer api checked")
        end)
    end,
    layerFor = function(e, map)
        chthonic.log(string.format("layerFor %s %d", tostring(e), map))
        if e == nil then return loginPick end
        return picks[e]
    end,
    shutdown = function()
        local i = chthonic.map.instance()
        if i and i.layer then chthonic.log(string.format("shutdown layer %d", i.id)) end
    end,
    update = function() end,
}
)lua";

// A layered map (cap 2): its base layer from startup; logins and transfers fill the fullest layer with room, a new one
// opens only when all are full, each with its own NPCs and Lua state, apart from the others (no Spawn, no say across);
// relog rejoins the saved layer, else placement at the saved position; layerFor picks a live layer with room, ignored
// otherwise (e nil at a login); chthonic.transfer layer = moves there, refused when full or gone; within the map a transfer
// stays in the layer; an empty extra layer closes after emptyTimeoutSec, the base never; chthonic.map.instance.
void checkLayers() {
    LogCapture capture;
    LuaRoot r;
    r.write("npcs/bat.lua", "return { name = 'Bat', template = 'BatTpl' }");
    r.write("systems/layers.lua", std::string(kLuaExpect) + kLayerSystem);
    world::WorldRules rules;
    rules.firstInstanceId = 1;
    TestWorld w({}, rules, layer_maps());
    check(w.world.load_scripts(r.root).empty(), "layers: loads");
    const auto contexts = [&] {
        std::vector<std::string> out;
        for (const auto& c : w.world.contexts()) out.push_back(std::format("{}/{}:{}", c.map, c.instance, c.players));
        return joined(out);
    };
    check(contexts() == "1/0:0; 3/0:0" && said("init layer 0 base true map 3"),
          "layers: START_MAP's context and map 3's base layer from startup: " + contexts());
    TimePoint now = T0;
    const auto tick = [&] { w.world.tick(now += std::chrono::milliseconds(50)); };
    const auto event = [&](entt::entity e, const std::string& name, nlohmann::json data = nlohmann::json::object()) {
        w.world.push_client_event(e, name, std::move(data));
    };
    const auto inst = [&](entt::entity e) {
        entt::registry* reg = w.world.registry_of(e);
        return reg ? std::format("{}/{}", reg->get<world::MapRef>(e).map, reg->get<world::MapRef>(e).instance) : "gone";
    };
    const auto to1 = [&](entt::entity e) { return w.world.transfer(e, {1, {1000, 0, 1000}, 0}); };
    const auto to3 = [&](entt::entity e) { return w.world.transfer(e, {3, {103, 0, 100}, 0}); };

    // 1. Ann and Bob log in: the base holds 2, no new layer. Cid: every layer full -> layer 1. Dan, from map 1: the
    // fullest with room, layer 1.
    const auto ann = w.spawn(1, 1, "Ann", 100, 100, "", 3), bob = w.spawn(2, 2, "Bob", 101, 100, "", 3);
    check(inst(ann) == "3/0" && inst(bob) == "3/0" && contexts() == "1/0:0; 3/0:2", "layers: below the cap no new layer: " + contexts());
    clear_log();
    const auto cid = w.spawn(3, 3, "Cid", 102, 100, "", 3);
    check(inst(cid) == "3/1" && logged("\"msg\":\"layer created\"") && logged("\"instance\":1") &&
              said("init layer 1 base false map 3") && contexts() == "1/0:0; 3/0:2; 3/1:1",
          "layers: at the cap a new layer opens (id 1, logged, its own init): " + contexts());
    const auto dan = w.spawn(4, 4, "Dan", 1000, 1000);
    check(to3(dan) && inst(dan) == "3/1" && contexts() == "1/0:0; 3/0:2; 3/1:2",
          "layers: a transfer from another map joins the fullest layer with room: " + contexts());
    tick();
    tick();
    const auto bats = [&](uint32_t layer) {
        std::vector<uint32_t> out;
        entt::registry& reg = layer ? w.world.copy(3, layer)->registry() : w.world.registry(3);
        for (auto [e, npc] : reg.view<world::Npc>().each()) out.push_back(wire(e));
        return out;
    };
    check(bats(0).size() == 1 && bats(1).size() == 1 && bats(0) != bats(1), "layers: each layer spawned its own Bat");
    const auto seen = [&](uint32_t sid, uint32_t id) {
        for (const auto& s : w.sent.of<proto::Spawn>(sid)) if (s.entityId == id) return true;
        return false;
    };
    check(seen(1, wire(bob)) && seen(3, wire(dan)) && !seen(3, wire(ann)) && !seen(3, wire(bob)) && !seen(1, wire(cid)) &&
              !seen(1, bats(1)[0]) && !seen(3, bats(0)[0]),
          "layers: layers share no players and no NPCs");
    event(ann, "whoami");
    event(cid, "whoami");
    tick();
    check(said(std::format("whoami {}: map 3 id 0 layer true base true", wire(ann))) &&
              said(std::format("whoami {}: map 3 id 1 layer true base false", wire(cid))),
          "layers: chthonic.map.instance() is {id, map, layer = true, base}");

    // 7. Zone chat: every layer reports map 3 (what GameServer::report_maps sends the chat link); say and yell stay in
    // the layer.
    check(w.world.map_of(ann) == 3 && w.world.map_of(cid) == 3, "layers: zone chat: players in different layers are on the same map (zone)");
    w.sent.clear();
    w.world.push_say(ann, {proto::ChatKind::say, "hello base"});
    w.world.push_say(cid, {proto::ChatKind::yell, "HEY layer one"});
    tick();
    const auto heard = [&](const std::string& text) {
        std::set<uint32_t> out;
        for (uint32_t sid : {1u, 2u, 3u, 4u})
            for (const auto& m : w.sent.of<proto::ChatMsg>(sid)) if (m.text == text) out.insert(sid);
        return out;
    };
    check(heard("hello base") == std::set<uint32_t>{1, 2} && heard("HEY layer one") == std::set<uint32_t>{3, 4},
          "layers: say and yell reach only their own layer");

    // Eve: all full -> layer 2. Ann and Bob leave the base; Fay fills layer 2 (1 player), not the emptier base.
    const auto eve = w.spawn(5, 5, "Eve", 100, 100, "", 3);
    check(inst(eve) == "3/2", "layers: all full again -> layer 2");
    to1(ann);
    to1(bob);
    const auto fay = w.spawn(6, 6, "Fay", 100, 100, "", 3);
    check(inst(fay) == "3/2" && contexts() == "1/0:2; 3/0:0; 3/1:2; 3/2:2",
          "layers: an arrival fills the fullest layer with room, not the emptier base: " + contexts());
    check(to3(ann) && inst(ann) == "3/0", "layers: then the base, the only one with room");

    // 2. Relog: Dan's save names layer 1; it has room after he left: he rejoins it. Saved in a full layer, a gone one or
    // none: placement, at the saved position.
    w.sent.clear();
    w.world.despawn(dan);
    check(w.sent.saves.back().mapId == 3 && w.sent.saves.back().instanceId == 1, "layers: the save carries map 3, layer 1");
    const auto row = [](uint64_t id, const std::string& name, uint32_t instance) {
        db::CharRow r{id, 1, name, 3, {120, 0, 120}, 0, "", {}};
        r.instanceId = instance;
        return r;
    };
    const auto dan2 = w.world.spawn(14, row(4, "Dan", 1), now);
    const auto ok14 = w.sent.of<proto::EnterWorldOk>(14);
    check(inst(dan2) == "3/1" && ok14.size() == 1 && ok14[0].map == 3 && ok14[0].pos == proto::Vec3{120, 0, 120},
          "layers: relog into a live layer with room rejoins it at the saved position");
    clear_log();
    const auto gus = w.world.spawn(17, row(7, "Gus", 1), now); // layer 1: Cid and Dan
    const auto ok17 = w.sent.of<proto::EnterWorldOk>(17);
    check(inst(gus) == "3/0" && ok17.size() == 1 && ok17[0].pos == proto::Vec3{120, 0, 120} &&
              logged(R"("reason":"layer full")"),
          "layers: relog into a full layer -> placement (the base, with room) at the saved position, logged");
    const auto hal = w.world.spawn(18, row(8, "Hal", 999), now); // a previous run's, or a closed layer's
    check(inst(hal) == "3/3" && logged(R"("reason":"layer gone")"), "layers: relog into a gone layer -> placement (all full: layer 3)");
    clear_log();
    const auto ivy = w.world.spawn(19, row(9, "Ivy", 0), now);
    check(inst(ivy) == "3/3" && !logged("saved layer not joinable"), "layers: saved with instance 0 -> placement, nothing logged");
    check(contexts() == "1/0:1; 3/0:2; 3/1:2; 3/2:2; 3/3:2", "layers: 4 full layers: " + contexts());

    // 3. layerFor: Eve and Ann leave (layers 2 and 0 have room; placement would take the base, the lower id). Bob's
    // hook picks 2: layer 2. Ann's picks the full layer 1: ignored -> the base. Eve's picks 999: ignored -> all full,
    // a new layer 4.
    to1(eve);
    to1(ann);
    clear_log();
    event(bob, "pick", {{"layer", 2}});
    event(bob, "go");
    tick();
    check(said(std::format("layerFor {} 3", wire(bob))) && said(std::format("go {} true", wire(bob))) && inst(bob) == "3/2",
          "layers: layerFor's live layer with room is joined, over the placement's pick");
    event(ann, "pick", {{"layer", 1}});
    event(ann, "go");
    tick();
    check(inst(ann) == "3/0", "layers: layerFor naming a full layer is ignored: " + inst(ann));
    event(eve, "pick", {{"layer", 999}});
    event(eve, "go");
    tick();
    check(inst(eve) == "3/4", "layers: layerFor naming a gone layer is ignored: " + inst(eve));
    // At a login: the base layer's hook, e nil. Fay leaves layer 2 (layers 2 and 4 have room, the placement's pick 2).
    to1(fay);
    event(gus, "loginpick", {{"layer", 4}});
    tick();
    const auto jon = w.spawn(10, 10, "Jon", 100, 100, "", 3);
    check(said("layerFor nil 3") && inst(jon) == "3/4", "layers: at a login layerFor(nil, map) picks the layer: " + inst(jon));

    // 4. chthonic.transfer layer =: Fay into layer 2 (room); Kim into the now full layer 2: false, instance.full, no
    // MapChange, she stays; into 999: false. Within the map: Ann to layer 3 (Hal left it), then without layer = she
    // stays in layer 3.
    event(fay, "go", {{"layer", 2}});
    tick();
    check(said(std::format("go {} true", wire(fay))) && inst(fay) == "3/2", "layers: chthonic.transfer layer = moves there");
    const auto kim = w.spawn(11, 11, "Kim", 1000, 1000);
    tick();
    w.sent.clear();
    clear_log();
    event(kim, "go", {{"layer", 2}});
    event(kim, "go", {{"layer", 999}});
    tick();
    tick();
    check(said_at(std::format("go {} false", wire(kim))).size() == 2 && said(std::format("full {} map 3 layer 2", wire(kim))) &&
              said(std::format("full {} map 3 layer 999", wire(kim))) && logged("transfer refused: layer full or gone") &&
              w.sent.of<proto::MapChange>(11).empty() && inst(kim) == "1/0",
          "layers: chthonic.transfer layer = into a full or gone layer: false, instance.full {layer}, no MapChange, stays");
    to1(hal);
    event(ann, "go", {{"layer", 3}});
    tick();
    check(inst(ann) == "3/3", "layers: chthonic.transfer layer = to another layer of the same map: " + inst(ann));
    w.sent.clear();
    event(ann, "go");
    tick();
    const auto annMc = w.sent.of<proto::MapChange>(1);
    check(inst(ann) == "3/3" && annMc.size() == 1 && annMc[0].pos.x == 100 && annMc[0].pos.z == 110,
          "layers: a transfer within the map stays in the layer");
    event(ann, "api");
    tick();
    check_lua_expectations("layers", "layer api checked");

    // 5. Layer 4 empties (Eve, Jon out) and the base too (Ann and Gus): 10 s later layer 4 is gone (shutdown, logged), the
    // base stays.
    for (auto e : {eve, jon, gus}) to1(e);
    to1(ann);
    tick();
    check(w.world.copy(3, 4) && w.world.copy(3, 4)->empty_since(), "layers: layer 4 empty: its timer runs");
    now += std::chrono::milliseconds(9800); // the timer started at the transfers' serial point, a tick before
    tick();
    check(w.world.copy(3, 4) != nullptr, "layers: not yet 10 s empty");
    clear_log();
    now += std::chrono::milliseconds(200);
    tick();
    check(w.world.copy(3, 4) == nullptr && said("shutdown layer 4") && logged("\"msg\":\"layer destroyed\""),
          "layers: 10 s empty -> layer 4 destroyed, its shutdown run, logged");
    now += std::chrono::seconds(60);
    tick();
    check(contexts() == "1/0:6; 3/0:0; 3/1:2; 3/2:2; 3/3:1", "layers: the empty base never closes: " + contexts());
    for (const auto& s : w.world.script_status()) check(s.ok, "layers: system still enabled " + s.error);
}

// 6. No new layer possible (entity ids low): the player overfills the fullest layer, logged; nobody is refused, at a
// login or a transfer. layerFor errors and bad results disable only that system.
void checkLayerOverflow() {
    LogCapture capture;
    LuaRoot r;
    r.write("npcs/bat.lua", "return { name = 'Bat', template = 'BatTpl' }");
    r.write("systems/a_bad.lua", "return { layerFor = function(e, map) return 'one' end, update = function() end }");
    r.write("systems/b_neg.lua", "return { layerFor = function(e, map) return -1 end, update = function() end }");
    r.write("systems/c_err.lua", "return { layerFor = function(e, map) error('boom') end, update = function() end }");
    r.write("systems/d_good.lua", "return { layerFor = function(e, map) return 0 end, update = function() end }");
    TestWorld w({}, {}, layer_maps());
    check(w.world.load_scripts(r.root).empty(), "layer overflow: loads");
    const auto a = w.spawn(1, 1, "Ann", 100, 100, "", 3), b = w.spawn(2, 2, "Bob", 100, 100, "", 3);
    const auto status = w.world.script_status();
    check(status.size() == 4 && !status[0].ok && contains(status[0].error, "layerFor must return nil or a layer id") &&
              !status[1].ok && !status[2].ok && contains(status[2].error, "boom") && status[3].ok,
          "layerFor: a string, a negative number and an error disable their systems; the next one answers");
    while (w.world.shared().room_for_copy()) w.world.shared().block(world::Shared::kIdBlock);
    clear_log();
    const auto c = w.spawn(3, 3, "Cid", 100, 100, "", 3);
    const auto d = w.spawn(4, 4, "Dan", 1000, 1000);
    check(w.world.transfer(d, {3, {100, 0, 100}, 0}), "layer overflow: the transfer is not refused");
    const auto contexts = w.world.contexts();
    check(contexts.size() == 2 && contexts[1].map == 3 && contexts[1].players == 4 && w.world.map_of(c) == 3 &&
              w.world.map_of(d) == 3 && w.world.map_of(a) == 3 && w.world.map_of(b) == 3,
          "layer overflow: no new layer; the base holds 4 over its cap of 2");
    check(logged("no new layer: overfilling the fullest") && logged(R"("reason":"entity ids low")"),
          "layer overflow: logged with why");
}

int lua_checks(int& checks) {
    checkCodec();
    checkDefinitionParsing();
    checkDefinitionErrors();
    checkNameRules();
    checkSandbox();
    checkChthonicApi();
    checkHooks();
    checkCharacterCreation();
    checkErrorsDisableOneSystem();
    checkStartupErrors();
    checkPersistence();
    checkReplication();
    checkReplicationIdleTiming();
    checkQueryTiming();
    checkReplicationDirty();
    checkEventBus();
    checkLuaEvents();
    checkLuaEventsFromCpp();
    checkLuaEventErrors();
    checkLuaEventFailures();
    checkLuaEventHooks();
    checkClientEvents();
    checkReply();
    checkReplyAfterMapChange();
    checkReplyRateLimited();
    checkChatServerOrder();
    checkLuaLogging();
    checkFsmDefinitions();
    checkFsmRules();
    checkFsmQueue();
    checkFsmCallbackErrors();
    checkFsmLuaApi();
    checkFsmEntities();
    checkFsmStateFlows();
    checkSetErrors();
    checkSpawnCache();
    checkSetPartialAndNoOp();
    checkBridgeTiming();
    checkNpcApi();
    checkQueryPools();
    checkNpcCap();
    checkNpcRouting();
    checkEntityMovesBatch();
    checkNpcDestroyedOnEnter();
    checkNpcShutdown();
    checkSpawnParsing();
    checkSpawnLoading();
    checkRespawn();
    checkSpawnLua();
    checkTileActivation();
    checkClientEventsIn();
    checkMoveEpochs();
    checkMoveYAdjusted();
    checkMovableAndMapSpawn();
    checkNpcDefinitionErrors();
    checkItemsAndLoot();
    checkDefinitionsApi();
    checkNpcCreateFromDefinitions();
    checkNpcSpawnDefinitions();
    checkTileMath();
    checkManifests();
    checkHeights();
    checkMapWorld();
    checkProximityChat();
    checkMapLua();
    checkMapContexts();
    checkWorldPost();
    checkInstanceManifests();
    checkInstances();
    checkInstanceKeyErrors();
    checkInstanceIdsExhausted();
    checkManyCopies();
    checkEntityIdsLow();
    checkLayerManifests();
    checkLayers();
    checkLayerOverflow();
    checkLuaFileCache();
    checkAreaShapes();
    checkZones();
    checkAreaLoading();
    checkGround();
    checkNavmesh();
    checkObjectParsing();
    checkInteractableLoading();
    checkInteract();
    checkObstacleNavmesh();
#ifdef CHTHONIC_SAMPLE_MAPS
    checkGroundSample();
    checkNpcSample();
    checkAreaWorld();
    checkAreaLua();
#else
    std::printf("sample maps not in this build: NPC sample, area world and Lua checks skipped (integration scenarios "
                "16-18 cover them)\n");
#endif
    checks += g_checks;
    return g_failures;
}

// --- tile activation cost (ctest activation_bench): median World::tick ms, 100 idle players on one tile of a 32x32
// tile map running the sample lua/, (a) alone and (b) with 10,000 patrolling spawn-file Guards on 100 tiles no player
// is near. A scout logs in on every Guard tile first (spawning its NPCs) and leaves, so (b) measures 10,000 NPCs that
// exist and sleep, not tiles that never loaded. -1 if the setup is wrong.
namespace {
double activation_tick_ms(int npcs) {
    world::Maps maps;
    map::MapInfo big;
    big.id = 1;
    big.name = "big";
    big.tiles = {{0, 0, 31, 31}};
    big.spawn = {300, 0, 300};
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> in(8, 248);
    for (int i = 0; i < npcs; ++i) {
        const float x0 = (10 + i % 10) * map::kTileSize, z0 = (10 + i / 10 % 10) * map::kTileSize; // tiles 10..19
        const float x = x0 + in(rng), z = z0 + in(rng);
        big.spawns.push_back({"g" + std::to_string(i), "Guard", {x, 0, z}, 0, 30,
                              map::PatrolPath{{{x, 0, z}, {x0 + in(rng), 0, z0 + in(rng)}}, 1, true}, ""});
    }
    maps.add(big);
    world::WorldRules rules;
    rules.maxNpcs = 20000;
    TestWorld w(lua::load_component_defs(CHTHONIC_WORLD_LUA_SAMPLE, world::kReservedComponentNames).defs, rules, std::move(maps));
    if (const auto errors = w.world.load_scripts(CHTHONIC_WORLD_LUA_SAMPLE); !errors.empty()) {
        std::fprintf(stderr, "activation bench: %s\n", errors.front().c_str());
        return -1;
    }
    for (int t = 0; t < 100 && npcs > 0; ++t) // the scout: its login spawns the tiles' NPCs, its logout puts them to sleep
        w.world.despawn(w.spawn(9999, 9999, "Scout", (10 + t % 10) * map::kTileSize + 128, (10 + t / 10) * map::kTileSize + 128));
    auto& reg = w.world.registry();
    if (reg.view<world::Npc>().size() != size_t(npcs) || reg.view<world::Asleep>().size() != size_t(npcs)) {
        std::fprintf(stderr, "activation bench: %zu NPCs, %zu asleep; wanted %d of each\n", reg.view<world::Npc>().size(),
                     reg.view<world::Asleep>().size(), npcs);
        return -1;
    }
    std::uniform_real_distribution<float> home(260, 500); // tile (1, 1)
    for (uint32_t p = 1; p <= 100; ++p) w.spawn(p, p, "P" + std::to_string(p), home(rng), home(rng));
    TimePoint now = T0;
    const auto tick = [&] {
        now += std::chrono::milliseconds(50);
        const auto start = std::chrono::steady_clock::now();
        w.world.tick(now);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    for (int i = 0; i < 50; ++i) tick(); // warm up
    std::vector<double> ms;
    for (int i = 0; i < 200; ++i) ms.push_back(tick());
    std::ranges::nth_element(ms, ms.begin() + ms.size() / 2);
    return ms[ms.size() / 2];
}
} // namespace

// --- instance copy cost (world_self_check --instance-bench): median ms to make a copy with the sample lua/ (a C++
// transfer into a new keyless copy: the context, its Lua state, its systems' init), and to destroy one. ---
int instance_bench() {
    log::set_level(log::Level::error);
    world::Maps maps = test_maps();
    std::string err;
    auto crypt = map::parse_manifest(Json::parse(R"({"version":1,"id":6,"name":"crypt","tiles":[[0,0,0,0]],
        "spawn":{"pos":[128,0,128]},"type":"instance","emptyTimeoutSec":0,"exit":{"map":1,"pos":[1000,0,1000]}})"), err);
    maps.add(std::move(*crypt));
    TestWorld w(lua::load_component_defs(CHTHONIC_WORLD_LUA_SAMPLE, world::kReservedComponentNames).defs, {}, std::move(maps));
    if (const auto errors = w.world.load_scripts(CHTHONIC_WORLD_LUA_SAMPLE); !errors.empty()) {
        std::fprintf(stderr, "instance bench: %s\n", errors.front().c_str());
        return 1;
    }
    const auto e = w.spawn(1, 1, "Ann", 1000, 1000);
    std::vector<double> make, destroy;
    const auto ms = [](auto start) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    for (int i = 0; i < 60; ++i) {
        auto start = std::chrono::steady_clock::now();
        w.world.transfer(e, {6, {128, 0, 128}, 0});
        make.push_back(ms(start));
        start = std::chrono::steady_clock::now();
        w.world.transfer(e, {1, {1000, 0, 1000}, 0}); // emptyTimeoutSec 0: destroyed at this serial point
        destroy.push_back(ms(start));
    }
    std::ranges::nth_element(make, make.begin() + make.size() / 2);
    std::ranges::nth_element(destroy, destroy.begin() + destroy.size() / 2);
    std::fprintf(stderr, "instance bench: sample lua/: make a copy %.2f ms, destroy it %.2f ms (medians of 60)\n",
                 make[make.size() / 2], destroy[destroy.size() / 2]);

    // Arrivals as in load.sh: 2 players a tick, each pair into its own new keyed copy (60 copies), 4 map threads.
    world::WorldRules rules;
    rules.mapThreads = 4;
    world::Maps arrivalMaps = test_maps();
    auto keyed = map::parse_manifest(Json::parse(R"({"version":1,"id":6,"name":"crypt","tiles":[[0,0,0,0]],
        "spawn":{"pos":[128,0,128]},"type":"instance","maxPlayers":2,"exit":{"map":1,"pos":[1000,0,1000]}})"), err);
    keyed->spawns.push_back({"bat", "Bat", {140, 0, 128}, 0, -1, {}, "maps/crypt/.server/spawns/0_0.json"});
    arrivalMaps.add(std::move(*keyed));
    TestWorld b(lua::load_component_defs(CHTHONIC_WORLD_LUA_SAMPLE, world::kReservedComponentNames).defs, rules,
                std::move(arrivalMaps));
    if (!b.world.load_scripts(CHTHONIC_WORLD_LUA_SAMPLE).empty()) return 1;
    std::vector<entt::entity> players;
    for (uint32_t i = 0; i < 120; ++i)
        players.push_back(b.spawn(10 + i, 10 + i, "P" + std::to_string(i), 1000 + float(i % 10), 1000 + float(i / 10)));
    TimePoint now = T0;
    std::vector<double> idle, arriving;
    for (int t = 0; t < 120; ++t) {
        const bool arrival = t >= 30 && t < 90;
        const auto start = std::chrono::steady_clock::now();
        if (arrival)
            for (size_t k = size_t(t - 30) * 2; k < size_t(t - 30) * 2 + 2; ++k)
                b.world.transfer(players[k], {6, {128, 0, 128}, 0, "key" + std::to_string(t)});
        b.world.tick(now += std::chrono::milliseconds(50));
        (arrival ? arriving : idle).push_back(ms(start));
    }
    const auto pct = [](std::vector<double> v, double p) {
        std::ranges::sort(v);
        return v[size_t(p * double(v.size() - 1))];
    };
    std::fprintf(stderr, "instance bench: a tick with 2 arrivals into a new copy: p50 %.2f ms, max %.2f ms; without: p50 %.2f ms\n",
                 pct(arriving, 0.5), pct(arriving, 1), pct(idle, 0.5));
    return 0;
}

int activation_bench() {
    log::set_level(log::Level::error); // the sample's per-entity warnings are not what is measured
    const double a = activation_tick_ms(0), b = activation_tick_ms(10000);
    std::fprintf(stderr, "activation bench: median tick (a) 100 players: %.4f ms, (b) + 10000 NPCs on far tiles: %.4f ms, "
                         "b/a = %.1f%%\n", a, b, 100 * b / a);
    if (a < 0 || b < 0 || b > a * 1.10) {
        std::fprintf(stderr, "activation bench: FAIL, (b) must be at most 110%% of (a)\n");
        return 1;
    }
    return 0;
}
