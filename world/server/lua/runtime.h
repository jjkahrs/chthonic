// Server Lua systems: LUA_ROOT/systems/*.lua, run in alphabetical filename order on the tick thread, one Lua state per
// map context (and one for the realm's game machines), each file in its own sandboxed environment. A file returns
// { init?, update(dt), shutdown?, onEnter?(e), onLeave?(e), onMapLeave?(e), onMapEnter?(e, carried),
// validateCharacter?(name, options), instanceKey?(e, map), layerFor?(e, map) }
// and reaches the world only through chthonic.* over its context's registry, event bus and state machines
// (docs/world/lua.html). sol2 stays inside runtime.cpp: World holds a Runtime without seeing Lua types.
#pragma once
#include "components.h"
#include "event_bus.h"
#include "state_machines.h"

#include <map.h>
#include <messages.h>

#include <entt/entity/registry.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chthonic::lua {

struct SystemStatus {
    std::string path; // LUA_ROOT-relative
    bool ok = false;
    std::string error; // why it is disabled ("path:line: ..."), empty when ok
};

// A chthonic.sendTo/sendNearby/broadcast call, queued until World sends it as ServerEvent; or a chthonic.reply (To::reply),
// sent as Reply{requestId, ok, json} to entity's owner (name unused).
struct ClientEvent {
    enum class To { owner, nearby, all, reply };
    To to = To::all;
    uint32_t entity = 0; // wire id; unused for all
    std::string name;
    std::string json;
    uint32_t requestId = 0; // reply only
    bool ok = false;        // reply only
};

// An NPC type (LUA_ROOT/npcs/, docs/world/npcs.html#npc-definitions): the name and template its NPCs get, and the components they
// start with.
struct NpcDef {
    std::string name, templateName;
    Json components = Json::object(); // {"<Type>": canonical value, ...}: the listed fields over the defaults
    Json loot; // null, or {"gold": {"min", "max"}?, "items": [{"item", "chance", "min", "max"}, ...]?} (checked)
};
using NpcDefs = std::map<std::string, NpcDef, std::less<>>; // by name

// A chthonic.world.post(target, event, data), checked: for the event bus of one map's context, the realm's, or every
// running context's.
struct Post {
    enum class To { map, realm, all };
    To to = To::map;
    uint16_t map = 0; // To::map: a loaded map
    std::string event;
    nlohmann::json data;
};

// What each system's onMapLeave returned (plain data), by system path, for its onMapEnter in the next context.
using Carried = std::map<std::string, nlohmann::json>;

// chthonic.map.instance(): the instance copy a context runs (docs/world/instances.html), or its layer of a layered
// world map (docs/world/maps.html#layers; id 0: the base layer).
struct InstanceInfo {
    uint32_t id = 0;
    std::optional<std::string> key; // nullopt: a keyless copy, or a layer
    uint16_t map = 0;
    bool layer = false;
};
constexpr size_t kMaxInstanceKeyBytes = 255; // an instance key: a non-empty string of at most this many bytes

// How chthonic.create / chthonic.destroy / chthonic.transfer / an NPC Transform set reach the world, which owns the maps, the AOI
// grid and the sends.
// Runtime checks every argument first; these only run for valid ones.
struct NpcHooks {
    std::function<entt::entity(std::string name, std::string templateName, std::optional<uint16_t> map,
                               std::optional<proto::Vec3> pos, float heading, std::string npc, const Json& components)>
        create; // map empty: the context's own; pos empty: its spawn point; npc: the definition ("" for none). Unset: the
                // realm context, which has no NPCs
    std::function<void(entt::entity)> destroy;                // an existing NPC
    std::function<proto::Vec3(uint16_t map, proto::Vec3)> clamp; // onto the (loaded) map's ground, within MIN_Y/MAX_Y
    std::function<const map::MapInfo*(uint16_t id)> map;      // nullptr: not loaded
    std::function<std::optional<float>(uint16_t map, float x, float z)> ground; // Maps::ground; nullopt: off the map
    std::function<std::vector<proto::Vec3>(uint16_t map, proto::Vec3 from, proto::Vec3 to)> path; // Nav::path
    // Within the map at once, onto another at the serial point (World::transfer); false: not owned, or into a full
    // instance copy, or a full or gone layer (logged). instance: chthonic.transfer's instance = key, layer its layer =
    // (nullopt: none given).
    std::function<bool(entt::entity, uint16_t map, proto::Vec3 pos, float heading, std::optional<std::string> instance,
                       std::optional<uint32_t> layer)>
        transfer;
    std::function<std::optional<InstanceInfo>()> instance;    // chthonic.map.instance; unset or nullopt: not a copy
    std::function<const map::Spawn*(uint32_t record)> spawn;  // a SpawnRef's spawn file entry
    std::function<size_t()> count;                            // NPCs in every context (unset: this registry's)
    std::function<void(Post)> post;                           // chthonic.world.post
    size_t max = 0;                                           // MAX_NPCS
};

// From now on every Lua file and folder listing read under LUA_ROOT stays in memory for the life of the process, so the
// next map context or instance copy loads without touching the disk (a Docker Desktop bind mount takes ~30 ms a
// context to read the sample lua/; from memory under 1 ms). chthonic_world turns it on at startup: Lua edits need a restart
// anyway. Tests leave it off (they rewrite their files); off also forgets what was kept.
// ponytail: never invalidated; key it by mtime if Lua hot reload ever comes.
void cache_lua_files(bool on = true);

class Runtime {
public:
    Runtime(entt::registry& reg, Defs defs, NpcHooks npcs = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Loads the state machine definitions under root/fsms/, the item definitions under root/items/ (chthonic.items), then
    // the NPC definitions under root/npcs/ (whose loot names items), then every
    // system under root/systems/ and runs each init, then the game machines (gameMachines, by name) enter their initial
    // state. Returns every error (a rejected definition, a listed machine without one, a bad system); non-empty means
    // the server must not start. A missing folder is no machines / items / NPC definitions / systems.
    std::vector<std::string> load(const std::filesystem::path& root, std::vector<std::string> gameMachines = {});
    // The realm context's load: only the state machine definitions under root/fsms/, then the game machines enter
    // their initial state. Errors as load's.
    std::vector<std::string> load_game_machines(const std::filesystem::path& root, std::vector<std::string> gameMachines);
    // A runtime error in any of these disables only that system (logged with path:line) until restart.
    void update(float dt);
    void enter(entt::entity e); // onEnter: after the character spawned and its persisted components were restored
    void leave(entt::entity e); // onLeave: before it is saved and despawned
    // onMapLeave: e is about to move to another map's context. Returns what each system returned (plain data, as an
    // event payload; anything else disables that system), for enter_map there.
    Carried leave_map(entt::entity e);
    // onMapEnter(e, carried): e just moved here from another map's context; carried is what this system's onMapLeave
    // returned there (nil for none).
    void enter_map(entt::entity e, const Carried& carried);
    // The instance key for e's transfer onto instance map `map`: the first string a system's instanceKey(e, map)
    // returns, in system order; nullopt if none does (or none has one). Returning anything else, or a string that is
    // empty or longer than kMaxInstanceKeyBytes, disables that system like any callback error. Callable from inside
    // another system's callback (chthonic.transfer).
    std::optional<std::string> instance_key(entt::entity e, uint16_t map);
    // The layer a system's layerFor(e, map) picks for e's arrival on layered world map `map`: the first integer in
    // [0, UINT32_MAX] one returns, in system order; nullopt if none does. e null (a login: no entity yet) is nil in
    // Lua. Anything else returned disables that system, as for instance_key.
    std::optional<uint32_t> layer_for(entt::entity e, uint16_t map);
    // Whether a character may be created with these (C++-checked) options: every system with a validateCharacter must
    // return true for (name, {key = value, ...}). No system has one: only empty options pass. An error rejects, and
    // disables that system like any callback's.
    bool validate_character(const std::string& name, const proto::CharOptions& options);
    void shutdown(); // also drops the events still queued on the bus
    std::vector<SystemStatus> status() const;
    // The NPC definition named name (loaded by load), or nullptr.
    const NpcDef* npc(std::string_view name) const;

    // Delivers the bus events queued so far (see EventBus::drain). A failing Lua subscriber disables its system.
    void drain_events();
    EventBus& bus();
    StateMachines& fsm();
    // The client events queued since the last call, in call order.
    std::vector<ClientEvent> take_client_events();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chthonic::lua
