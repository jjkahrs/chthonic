#include "runtime.h"
#include "../core/log.h"
#include "../world/components.h"
#include "lua_state_machines.h"
#include "npcs.h"
#include "sandbox.h"

#include <messages.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <ranges>

namespace chthonic::lua {
namespace {

// Raises a Lua error at the calling script's file:line. sol2 turns the exception into lua_error.
[[noreturn]] void raise(lua_State* L, const std::string& message) {
    luaL_where(L, 1);
    std::string where = lua_tostring(L, -1);
    lua_pop(L, 1);
    throw std::runtime_error(where + message);
}

// The read-only built-ins (Character on players, Npc on NPCs, Spawn on spawn-file NPCs, Transform on both; Transform is
// writable on NPCs), plus the Lua component types.
enum class Builtin { none, character, transform, npc, spawn };
struct Type {
    Builtin builtin = Builtin::none;
    const ComponentDef* def = nullptr;
    // def's storage, resolved once by type(). EnTT keeps each storage in a shared_ptr (registry.hpp pool_container_type,
    // assure), so the reference stays valid when later pools are created; only registry::reset(id) would drop it.
    entt::storage_for_t<ComponentData>* pool = nullptr;
    std::string_view name() const {
        if (def) return def->name;
        switch (builtin) {
        case Builtin::character: return "Character";
        case Builtin::npc: return "Npc";
        case Builtin::spawn: return "Spawn";
        default: return "Transform";
        }
    }
};

constexpr size_t kMaxNpcNameBytes = 255; // for name and template, each

struct System {
    std::string path; // "systems/regen.lua"
    std::filesystem::path file;
    sol::environment env;
    sol::protected_function update, shutdown, onEnter, onLeave, onMapLeave, onMapEnter, validateCharacter, instanceKey,
        layerFor;
    std::vector<EventBus::Subscription> subscriptions; // chthonic.subscribe; cleared when the system is disabled
    bool ok = false;
    std::string error;
};

uint32_t wire(entt::entity e) { return entt::to_integral(e); }

// chthonic.definitions' folder: "/"-separated, relative, no empty, "." or ".." part (so it stays under LUA_ROOT).
bool relative_folder(std::string_view folder) {
    if (folder.empty() || folder.find_first_of("\\:") != std::string_view::npos) return false;
    for (const auto part : std::views::split(folder, '/')) {
        const std::string_view p(part.begin(), part.end());
        if (p.empty() || p == "." || p == "..") return false;
    }
    return true;
}

// The writable StateMachine built-in, stored like a Lua component that is never persisted or replicated: chthonic.add/set/
// get/has/remove/query and their checks work on it unchanged, and World (persistence, replication) never sees it.
Defs with_state_machine(Defs defs) {
    auto def = std::make_shared<ComponentDef>();
    def->name = def->displayName = kStateMachineComponent;
    def->path = "(built-in)";
    for (const char* name : {"machine", "initialState"}) {
        FieldDesc& field = def->fields.emplace_back();
        field.name = name;
        field.kind = FieldKind::String;
        field.defaultValue = "";
    }
    defs.push_back(std::move(def));
    return defs;
}

} // namespace

struct Runtime::Impl {
    entt::registry& reg;
    Defs defs;
    NpcHooks npcs;
    sol::state lua;
    sol::table api;
    sol::table fsmApi;                     // chthonic.fsm.*
    sol::table mapApi;                     // chthonic.map.*
    sol::table worldApi;                   // chthonic.world.*
    EventBus bus;                          // after lua: Lua subscribers hold functions of that state
    StateMachines fsm{bus, reg};           // after lua: its definitions hold Lua callbacks
    std::vector<System> systems;           // after bus: their subscriptions unsubscribe from it
    std::vector<ClientEvent> clientEvents; // chthonic.sendTo/sendNearby/broadcast since the last take
    ItemDefs items;                        // items/, loaded before the NPCs (loot tables name them)
    NpcDefs npcDefs;                       // npcs/, loaded after the machines (StateMachine data names one)
    std::filesystem::path luaRoot;         // LUA_ROOT, set by load
    struct Definitions {
        std::vector<nlohmann::json> tables;
        std::vector<std::string> paths; // LUA_ROOT-relative
    };
    std::map<std::string, Definitions, std::less<>> definitions; // chthonic.definitions, by folder, read on first use
    std::vector<sol::table> spawns; // chthonic.get(e, "Spawn") by SpawnRef record, built on first use; after lua
    System* current = nullptr;
    bool returnedTrue = false; // the last call()'s first result was true
    bool keepResult = false;   // call() keeps its first result in kept (onMapLeave)
    sol::object kept;
    std::string callbackPath; // the definition file whose enter/exit is running, if no system is

    Impl(entt::registry& r, Defs d, NpcHooks n) : reg(r), defs(with_state_machine(std::move(d))), npcs(std::move(n)) {
        open_sandbox(lua);
        bind_api();
        bind_fsm_api();
        bind_map_api();
        bind_world_api();
    }

    std::string who() const { return current ? current->path : callbackPath.empty() ? std::string("lua") : callbackPath; }

    // Runs fn; returns the error ("path:line: message"), empty on success.
    template <class... Args>
    std::string call(System& system, const sol::protected_function& fn, std::string_view what, Args&&... args) {
        current = &system;
        returnedTrue = false;
        std::string error;
        try {
            const ScriptBudget budget(lua.lua_state());
            sol::protected_function_result result = fn(std::forward<Args>(args)...);
            if (!result.valid()) error = result.get<sol::error>().what();
            else {
                returnedTrue = result.return_count() > 0 && result.get_type(0) == sol::type::boolean && result.get<bool>(0);
                if (keepResult && result.return_count() > 0) kept = result.get<sol::object>(0);
            }
        } catch (const std::exception& e) { // sol2 conversions; nothing may escape to the tick
            error = std::format("{}: {}: {}", system.path, what, e.what());
        }
        current = nullptr;
        return error;
    }

    // A callback after startup: an error disables the system, the server and the other systems keep running.
    template <class... Args>
    void run(System& system, const sol::protected_function& fn, std::string_view what, Args&&... args) {
        if (!system.ok || !fn.valid()) return;
        std::string error = call(system, fn, what, std::forward<Args>(args)...);
        if (error.empty()) return;
        disable(system, std::move(error));
        log::error("Lua system disabled", {{"system", system.path}, {"callback", what}, {"error", system.error}});
    }

    void disable(System& system, std::string error) {
        system.ok = false;
        system.error = std::move(error);
        system.subscriptions.clear();
    }

    std::string load(System& system) {
        std::string error;
        system.env = environment();
        sol::protected_function body = load_sandboxed(lua, system.file, "@" + system.path, system.env, error);
        if (!body.valid()) return error.starts_with("cannot open") ? std::format("{}: {}", system.path, error) : error;
        current = &system;
        sol::protected_function_result result = [&] {
            const ScriptBudget budget(lua.lua_state());
            return body();
        }();
        current = nullptr;
        if (!result.valid()) return result.get<sol::error>().what();
        if (result.get_type() != sol::type::table)
            return std::format("{}: must return a table with an update function", system.path);
        sol::table table = result;
        sol::object update = table["update"];
        if (update.get_type() != sol::type::function)
            return std::format("{}: returned table has no update function", system.path);
        system.update = update;
        for (auto [name, slot] : {std::pair{"init", static_cast<sol::protected_function*>(nullptr)},
                                  std::pair{"shutdown", &system.shutdown}, std::pair{"onEnter", &system.onEnter},
                                  std::pair{"onLeave", &system.onLeave}, std::pair{"onMapLeave", &system.onMapLeave},
                                  std::pair{"onMapEnter", &system.onMapEnter},
                                  std::pair{"validateCharacter", &system.validateCharacter},
                                  std::pair{"instanceKey", &system.instanceKey},
                                  std::pair{"layerFor", &system.layerFor}}) {
            sol::object fn = table[name];
            if (fn.get_type() == sol::type::lua_nil) continue;
            if (fn.get_type() != sol::type::function) return std::format("{}: \"{}\" must be a function", system.path, name);
            if (slot) *slot = fn;
        }
        system.ok = true;
        if (sol::object init = table["init"]; init.get_type() == sol::type::function)
            if (std::string e = call(system, init, "init"); !e.empty()) {
                disable(system, e);
                return e;
            }
        return {};
    }

    // A metatable whose __newindex raises message, hidden from getmetatable.
    sol::table guard(const char* message) {
        sol::table meta = lua.create_table();
        meta["__newindex"] = [message](sol::this_state L, sol::object, sol::object, sol::object) {
            if (message) raise(L, message); // never null: the branch keeps MSVC's C4702 (unreachable code in sol2) quiet
        };
        meta["__metatable"] = false;
        return meta;
    }

    sol::table read_only(const sol::table& source, const char* message = "chthonic is read-only") {
        // A fresh copy behind a proxy: writes raise an error, and even rawset only changes this system's copy.
        sol::table copy = lua.create_table();
        for (const auto& [key, value] : source) copy[key] = value;
        sol::table meta = guard(message);
        meta["__index"] = copy;
        sol::table proxy = lua.create_table();
        proxy[sol::metatable_key] = meta;
        return proxy;
    }

    sol::environment environment() {
        sol::environment env = sandbox_environment(lua);
        sol::table chthonic = lua.create_table();
        for (const auto& [key, value] : api) chthonic[key] = value;
        chthonic["fsm"] = read_only(fsmApi);
        chthonic["map"] = read_only(mapApi);
        chthonic["world"] = read_only(worldApi);
        chthonic["items"] = item_table();
        env["chthonic"] = read_only(chthonic);
        return env;
    }

    // Loads the definitions (callbacks bound to this state) and lists the game machines; every error.
    std::vector<std::string> load_machines(const std::filesystem::path& root, std::vector<std::string> gameMachines) {
        StateMachineDefinitions loaded = parse_state_machine_definitions(
            lua, root, [this] { return environment(); },
            [this](const sol::protected_function& fn, const std::string& path, const std::string& what) {
                return bind_callback(fn, path, what);
            });
        for (std::string& e : fsm.setup(std::move(loaded.valid), std::move(gameMachines)))
            loaded.errors.push_back(std::move(e));
        return std::move(loaded.errors);
    }

    bool machine_exists(const std::string& name) const { return fsm.definition(name) != nullptr; }

    // chthonic.items: {[id] = {id =, name =, ...}}, read-only all the way down, a fresh copy per environment.
    sol::table item_table() {
        sol::table table = lua.create_table();
        for (const auto& [id, item] : items) table[id] = read_only(json_to_lua(lua, nlohmann::json(item)).as<sol::table>());
        return read_only(table);
    }

    std::vector<std::string> load_items(const std::filesystem::path& root) {
        ItemDefsResult loaded = load_item_defs(lua, root);
        items = std::move(loaded.defs);
        return std::move(loaded.errors);
    }

    std::vector<std::string> load_npcs(const std::filesystem::path& root) {
        NpcDefsResult loaded = load_npc_defs(lua, root, defs, [this](const std::string& n) { return machine_exists(n); }, items);
        npcDefs = std::move(loaded.defs);
        return std::move(loaded.errors);
    }

    // An enter/exit callback: an error (with its traceback) is logged, and no system is disabled.
    StateCallback bind_callback(const sol::protected_function& fn, const std::string& path, const std::string& what) {
        return [this, callback = fn, path, what](const Transition& t) {
            sol::table ctx = lua.create_table();
            ctx["machine"] = t.machine;
            if (t.entity) ctx["entity"] = *t.entity;
            if (t.from) ctx["from"] = *t.from;
            ctx["to"] = t.to;
            System* caller = std::exchange(current, nullptr); // a system's chthonic.fsm.change may have called this
            std::string callerPath = std::exchange(callbackPath, path);
            std::string error;
            try {
                const ScriptBudget budget(lua.lua_state());
                sol::protected_function_result result = callback(ctx);
                if (!result.valid()) error = result.get<sol::error>().what();
            } catch (const std::exception& e) { // sol2 conversions; nothing may escape to the tick
                error = std::format("{}: {}", path, e.what());
            }
            current = caller;
            callbackPath = std::move(callerPath);
            if (!error.empty()) log::error(std::format("State machine {}: {} failed: {}", path, what, error));
        };
    }

    // chthonic.get(e, "Spawn"): a spawn record's data never changes (it points into Maps, which outlives the World), so its
    // table is built once and shared, read-only. path stays a plain list behind __newindex so # and ipairs work (LuaJIT
    // ignores __len and __index there): a new key raises, but its existing entries can't be guarded.
    sol::table spawn_table(uint32_t record) {
        if (record >= spawns.size()) spawns.resize(record + 1);
        if (spawns[record].valid()) return spawns[record];
        constexpr const char* kReadOnly = "\"Spawn\" is read-only";
        const map::Spawn& s = *npcs.spawn(record);
        const auto point = [&](const std::array<float, 3>& p) {
            return read_only(lua.create_table_with("x", p[0], "y", p[1], "z", p[2]), kReadOnly);
        };
        sol::table table = lua.create_table_with("id", s.id, "pos", point(s.pos), "heading", s.heading);
        if (s.path) {
            sol::table points = lua.create_table(static_cast<int>(s.path->points.size()), 0);
            for (const auto& p : s.path->points) points.add(point(p));
            points[sol::metatable_key] = guard(kReadOnly);
            table.set("path", points, "wait", s.path->wait, "loop", s.path->loop);
        }
        return spawns[record] = read_only(table, kReadOnly);
    }

    // chthonic.fsm.*: thin wrappers over StateMachines, whose std::invalid_argument (a bad target) becomes a Lua error at
    // the calling line.
    void bind_fsm_api() {
        using Target = StateMachines::Target;
        // value: a sol::object, or a sol::stack_object (no registry reference) on the hot chthonic.fsm.state.
        const auto target = [](lua_State* L, const char* function, const auto& value) -> Target {
            if (value.get_type() == sol::type::string) return value.template as<std::string>();
            if (value.get_type() == sol::type::number) {
                const double id = value.template as<double>();
                if (id >= 0.0 && id <= 4294967295.0 && id == std::floor(id)) return static_cast<EntityId>(id);
                raise(L, std::format("{}: invalid entity id {}", function, to_string(L, sol::object(value))));
            }
            raise(L, std::format("{}: target must be a state machine name or an entity id, got {}", function,
                                 to_string(L, sol::object(value))));
        };
        const auto stateName = [](lua_State* L, const char* function, const sol::object& value) {
            if (value.get_type() != sol::type::string)
                raise(L, std::format("{}: state must be a string, got {}", function, to_string(L, value)));
            return value.as<std::string>();
        };
        // Runs body; a bad target raises at the calling line.
        const auto guarded = [](lua_State* L, const char* function, auto&& body) {
            try {
                return body();
            } catch (const std::invalid_argument& e) {
                raise(L, std::format("{}: {}", function, e.what()));
            }
        };

        fsmApi = lua.create_table();
        fsmApi.set_function("state", [this, target, guarded](sol::this_state L, const sol::stack_object& t) {
            const Target machine = target(L, "chthonic.fsm.state", t);
            return guarded(L, "chthonic.fsm.state", [&] { return fsm.state(machine); });
        });
        fsmApi.set_function("change", [this, target, stateName, guarded](sol::this_state L, const sol::object& t,
                                                                         const sol::object& state) {
            const Target machine = target(L, "chthonic.fsm.change", t);
            const std::string to = stateName(L, "chthonic.fsm.change", state);
            return guarded(L, "chthonic.fsm.change", [&] { return fsm.change(machine, to); });
        });
        fsmApi.set_function("canChange", [this, target, stateName, guarded](sol::this_state L, const sol::object& t,
                                                                            const sol::object& state) {
            const Target machine = target(L, "chthonic.fsm.canChange", t);
            const std::string to = stateName(L, "chthonic.fsm.canChange", state);
            return guarded(L, "chthonic.fsm.canChange", [&] { return fsm.canChange(machine, to); });
        });
        fsmApi.set_function("states", [this, target, guarded](sol::this_state L, const sol::object& t) {
            const Target machine = target(L, "chthonic.fsm.states", t);
            return sol::as_table(guarded(L, "chthonic.fsm.states", [&] { return fsm.states(machine); }));
        });
    }

    // Entities are server wire ids: player characters and NPCs.
    static bool in_world(const entt::registry& reg, entt::entity e) {
        return reg.valid(e) && reg.any_of<world::CharacterRef, world::Npc>(e);
    }

    // id: a sol::object, or a sol::stack_object (no registry reference) on the hot chthonic.get/set.
    entt::entity entity(lua_State* L, const auto& id, const char* function = nullptr) {
        if (id.get_type() == sol::type::number) {
            const double value = id.template as<double>();
            if (value >= 0.0 && value <= 4294967295.0 && value == std::floor(value)) {
                const auto e = entt::entity{static_cast<uint32_t>(value)};
                if (in_world(reg, e)) return e;
            }
        }
        raise(L, std::format("{}invalid entity id {}", function ? std::string(function) + ": " : "",
                             to_string(L, sol::object(id))));
    }

    // A client event's target: any wire id; one not in the world is skipped when the events are sent.
    uint32_t target(lua_State* L, const char* function, const sol::object& id) {
        if (id.get_type() == sol::type::number) {
            const double value = id.as<double>();
            if (value >= 0.0 && value <= 4294967295.0 && value == std::floor(value)) return static_cast<uint32_t>(value);
        }
        raise(L, std::format("{}: invalid entity id {}", function, to_string(L, id)));
    }

    std::string event_name(lua_State* L, const char* function, const sol::object& name) {
        if (name.get_type() != sol::type::string || name.as<std::string>().empty())
            raise(L, std::format("{}: event name must be a non-empty string, got {}", function, to_string(L, name)));
        return name.as<std::string>();
    }

    nlohmann::json payload(lua_State* L, const char* function, const sol::object& value) {
        nlohmann::json json;
        if (std::string error; !lua_to_json(value, json, error)) raise(L, std::format("{}: {}", function, error));
        return json;
    }

    void queue_client_event(lua_State* L, const char* function, ClientEvent::To to, uint32_t entity,
                            const sol::object& name, const sol::object& value) {
        std::string event = event_name(L, function, name);
        // replace: a Lua string may hold bytes that aren't UTF-8, which JSON can't carry
        std::string json = payload(L, function, value).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        if (!proto::ServerEvent::fits(event.size(), json.size()))
            raise(L, std::format("{}: event \"{}\" is too large to send: {} bytes of name and JSON payload, at most {}",
                                 function, event, event.size() + json.size(), proto::kMaxPayloadBytes - 4));
        clientEvents.push_back({to, entity, std::move(event), std::move(json)});
    }

    Type type(lua_State* L, const std::string& name) {
        if (name == "Character") return {Builtin::character, nullptr};
        if (name == "Transform") return {Builtin::transform, nullptr};
        if (name == "Npc") return {Builtin::npc, nullptr};
        if (name == "Spawn") return {Builtin::spawn, nullptr};
        for (const auto& def : defs)
            if (def->name == name) return {Builtin::none, def.get(), &pool(reg, def->name)};
        raise(L, std::format("unknown component \"{}\"", name));
    }

    bool has(entt::entity e, const Type& t) {
        switch (t.builtin) {
        case Builtin::character: return reg.all_of<world::CharacterRef>(e);
        case Builtin::npc: return reg.all_of<world::Npc>(e);
        case Builtin::spawn: return reg.all_of<world::SpawnRef>(e);
        case Builtin::transform: return true;
        case Builtin::none: break;
        }
        return t.pool->contains(e);
    }

    // The entities that may have t: its storage, or nullptr for Transform (every entity in the view).
    const entt::sparse_set* candidates(const Type& t) {
        switch (t.builtin) {
        case Builtin::character: return &reg.storage<world::CharacterRef>();
        case Builtin::npc: return &reg.storage<world::Npc>();
        case Builtin::spawn: return &reg.storage<world::SpawnRef>();
        case Builtin::transform: return nullptr;
        case Builtin::none: break;
        }
        return t.pool;
    }

    const ComponentDef& writable(lua_State* L, const Type& t) {
        if (!t.def) raise(L, std::format("\"{}\" is read-only on the server", t.name()));
        return *t.def;
    }

    void changed(entt::entity e, const ComponentDef& def) {
        if (def.replicate != Replicate::none) // component_replication_system diffs only what is marked
            world::mark_dirty(reg, e, def.name);
        // NPCs are never saved
        if (def.persist && reg.all_of<world::CharacterRef>(e)) reg.emplace_or_replace<world::Dirty>(e);
        if (def.name == kStateMachineComponent) fsm.syncEntity(e); // a new "machine" enters its initial state now
    }

    // A number that is finite as the wire's f32, or raises "<prefix>"<name>" must be a finite number".
    static float finite(lua_State* L, std::string_view prefix, std::string_view name, const sol::object& value) {
        const double number = value.get_type() == sol::type::number ? value.as<double>() : NAN;
        if (!std::isfinite(number) || std::fabs(number) > std::numeric_limits<float>::max())
            raise(L, std::format("{}\"{}\" must be a finite number", prefix, name));
        return static_cast<float>(number);
    }

    // {x=, y=, z=}, each finite as an f32, or raises "<prefix>"<name>" must be a table ...".
    static proto::Vec3 position(lua_State* L, std::string_view prefix, const sol::object& value,
                                std::string_view name = "position") {
        const auto bad = [&] {
            raise(L, std::format("{}\"{}\" must be a table {{x=, y=, z=}} of finite numbers", prefix, name));
        };
        if (value.get_type() != sol::type::table) bad();
        const sol::table table = value.as<sol::table>();
        float xyz[3];
        for (int i = 0; i < 3; ++i) {
            const sol::object c = table[std::string(1, "xyz"[i])];
            const double number = c.get_type() == sol::type::number ? c.as<double>() : NAN;
            if (!std::isfinite(number) || std::fabs(number) > std::numeric_limits<float>::max()) bad();
            xyz[i] = static_cast<float>(number);
        }
        return {xyz[0], xyz[1], xyz[2]};
    }

    // A loaded map's id, or raises "<prefix>"map" must be ...".
    uint16_t map_id(lua_State* L, std::string_view prefix, const sol::object& value) {
        const double number = value.get_type() == sol::type::number ? value.as<double>() : NAN;
        if (!(number >= 0 && number <= 65535 && number == std::floor(number)) || !npcs.map(static_cast<uint16_t>(number)))
            raise(L, std::format("{}\"map\" must be the id of a loaded map, got {}", prefix, to_string(L, value)));
        return static_cast<uint16_t>(number);
    }

    // chthonic.set(npc, "Transform", {position=, heading=}): partial and all-or-nothing like any set. The position is
    // clamped onto the NPC's map, on its ground (y is ignored); script moves are trusted (no speed check) and replicate in EntityMoves like an
    // accepted player move. The map is read-only: chthonic.transfer changes it.
    void set_npc_transform(lua_State* L, entt::entity e, const sol::table& fields) {
        auto& tf = reg.get<world::Transform>(e);
        proto::Vec3 pos = tf.pos;
        float heading = tf.heading;
        for (const auto& [key, value] : fields) {
            if (key.get_type() != sol::type::string)
                raise(L, std::format("\"Transform\": field names must be strings, got {}", to_string(L, key)));
            const std::string field = key.as<std::string>();
            if (field == "position") pos = position(L, "\"Transform\": ", value);
            else if (field == "heading") heading = finite(L, "\"Transform\": ", "heading", value);
            else if (field == "map") raise(L, "\"Transform\": \"map\" is read-only; use chthonic.transfer");
            else raise(L, std::format("\"Transform\": unknown field \"{}\"", field));
        }
        pos = npcs.clamp(reg.get<world::MapRef>(e).map, pos);
        if (pos == tf.pos && heading == tf.heading) return;
        tf.pos = pos;
        tf.heading = heading;
        reg.emplace_or_replace<world::Moved>(e);
    }

    // chthonic.create(name, {npc= | template=, components=?, map=?, position=?, heading=?}): every argument is checked
    // before anything is created.
    uint32_t create_npc(lua_State* L, const sol::object& name, const sol::object& opts) {
        const auto text = [&](std::string_view what, const sol::object& value) {
            if (value.get_type() != sol::type::string || value.as<std::string>().empty() ||
                value.as<std::string>().size() > kMaxNpcNameBytes)
                raise(L, std::format("chthonic.create: {} must be a non-empty string of at most {} bytes, got {}", what,
                                     kMaxNpcNameBytes, to_string(L, value)));
            return value.as<std::string>();
        };
        std::string npcName = text("name", name);
        if (opts.get_type() != sol::type::table)
            raise(L, std::format("chthonic.create: opts must be a table {{npc = ...}} or {{template = ...}}, got {}",
                                 to_string(L, opts)));
        const sol::table table = opts.as<sol::table>();
        for (const auto& [key, unused] : table) {
            const std::string k = key.get_type() == sol::type::string ? key.as<std::string>() : "";
            if (k != "npc" && k != "template" && k != "components" && k != "map" && k != "position" && k != "heading")
                raise(L, std::format("chthonic.create: unknown option {}", to_string(L, key)));
        }
        const sol::object npcOpt = table["npc"], templateOpt = table["template"];
        if ((npcOpt.get_type() == sol::type::lua_nil) == (templateOpt.get_type() == sol::type::lua_nil))
            raise(L, "chthonic.create: opts needs exactly one of npc (an npcs/ definition) and template");
        const NpcDef* def = nullptr;
        if (npcOpt.get_type() != sol::type::lua_nil) {
            const std::string wanted = text("npc", npcOpt);
            const auto it = npcDefs.find(wanted);
            if (it == npcDefs.end())
                raise(L, std::format("chthonic.create: unknown npc \"{}\" (no definition in {}/)", wanted, kNpcsDir));
            def = &it->second;
        }
        std::string templateName = def ? def->templateName : text("template", templateOpt);
        std::string error;
        std::optional<Json> components =
            merge_components(table["components"], def ? def->components : Json::object(), defs,
                             [this](const std::string& n) { return machine_exists(n); }, error);
        if (!components) raise(L, "chthonic.create: " + error);
        std::optional<uint16_t> map;
        if (const sol::object m = table["map"]; m.get_type() != sol::type::lua_nil) {
            map = map_id(L, "chthonic.create: ", m);
            const auto own = own_instance();
            if (npcs.map(*map)->instanced() && !(own && own->map == *map))
                raise(L, std::format("chthonic.create: map {} is an instance map; create its NPCs from inside the copy", *map));
        }
        std::optional<proto::Vec3> pos;
        if (const sol::object p = table["position"]; p.get_type() != sol::type::lua_nil)
            pos = position(L, "chthonic.create: ", p);
        float heading = 0;
        if (const sol::object h = table["heading"]; h.get_type() != sol::type::lua_nil)
            heading = finite(L, "chthonic.create: ", "heading", h);
        if (!npcs.create) raise(L, "chthonic.create: the realm context has no NPCs; chthonic.world.post to a map instead");
        if ((npcs.count ? npcs.count() : reg.view<world::Npc>().size()) >= npcs.max)
            raise(L, std::format("chthonic.create: too many NPCs (MAX_NPCS is {})", npcs.max));
        return wire(npcs.create(std::move(npcName), std::move(templateName), map, pos, heading, def ? def->name : "",
                                *components));
    }

    // An instance key: a non-empty string of at most kMaxInstanceKeyBytes, or raises "<prefix>...".
    static std::string instance_key(lua_State* L, std::string_view prefix, const sol::object& value) {
        if (value.get_type() != sol::type::string || value.as<std::string>().empty() ||
            value.as<std::string>().size() > kMaxInstanceKeyBytes)
            raise(L, std::format("{}\"instance\" must be a non-empty string of at most {} bytes, got {}", prefix,
                                 kMaxInstanceKeyBytes, to_string(L, value)));
        return value.as<std::string>();
    }

    // chthonic.transfer(e, map, position, heading?) or chthonic.transfer(e, {map =, position =, heading =?, instance =?,
    // layer =?}): a player or NPC to a point on a declared tile of a loaded map (the same map too). heading defaults to
    // the current one. instance: the instance key, onto an instance map (the instanceKey hooks are not asked). layer: a
    // layer's instanceId, onto a layered world map (the layerFor hooks are not asked). false: this process does not own
    // the destination, or the instance copy is full, or the layer full or gone (logged).
    bool transfer(lua_State* L, const sol::object& id, const sol::object& mapValue, const sol::object& posValue,
                  const sol::object& headingValue) {
        const entt::entity e = entity(L, id, "chthonic.transfer");
        sol::object mapArg = mapValue, posArg = posValue, headingArg = headingValue;
        std::optional<std::string> key;
        std::optional<uint32_t> layer;
        if (mapValue.get_type() == sol::type::table) {
            const sol::table opts = mapValue.as<sol::table>();
            for (const auto& [k, unused] : opts) {
                const std::string name = k.get_type() == sol::type::string ? k.as<std::string>() : "";
                if (name != "map" && name != "position" && name != "heading" && name != "instance" && name != "layer")
                    raise(L, std::format("chthonic.transfer: unknown option {}", to_string(L, k)));
            }
            mapArg = opts["map"];
            posArg = opts["position"];
            headingArg = opts["heading"];
            if (const sol::object k = opts["instance"]; k.get_type() != sol::type::lua_nil)
                key = instance_key(L, "chthonic.transfer: ", k);
            if (const sol::object l = opts["layer"]; l.get_type() != sol::type::lua_nil && !(layer = layer_id(l)))
                raise(L, std::format("chthonic.transfer: \"layer\" must be an integer in [0, {}], got {}", UINT32_MAX,
                                     to_string(L, l)));
        }
        const uint16_t map = map_id(L, "chthonic.transfer: ", mapArg);
        const proto::Vec3 pos = position(L, "chthonic.transfer: ", posArg);
        if (!npcs.map(map)->contains(pos.x, pos.z))
            raise(L, std::format("chthonic.transfer: position ({}, {}) is not on a declared tile of map {}", pos.x, pos.z, map));
        if (key && !npcs.map(map)->instanced())
            raise(L, std::format("chthonic.transfer: \"instance\" is only for instance maps; map {} is a world map", map));
        if (layer && !npcs.map(map)->layerCap)
            raise(L, std::format("chthonic.transfer: \"layer\" is only for layered world maps (\"layerCap\"); map {} has none", map));
        const float heading = headingArg.get_type() == sol::type::lua_nil
                                  ? reg.get<world::Transform>(e).heading
                                  : finite(L, "chthonic.transfer: ", "heading", headingArg);
        return npcs.transfer(e, map, pos, heading, std::move(key), layer);
    }

    // This context's instance copy (NpcHooks::instance), or nullopt.
    std::optional<InstanceInfo> own_instance() const { return npcs.instance ? npcs.instance() : std::nullopt; }

    // A layer's instanceId: an integer in [0, UINT32_MAX], else nullopt.
    static std::optional<uint32_t> layer_id(const sol::object& value) {
        const double number = value.get_type() == sol::type::number ? value.as<double>() : NAN;
        if (!(number >= 0 && number <= double(UINT32_MAX) && number == std::floor(number))) return std::nullopt;
        return static_cast<uint32_t>(number);
    }

    // The first value a system's hook (slot) returns that accept takes, in system order; nil skips the system, anything
    // else disables it ("<path>: <name> must return <want>, got ..."). Possibly inside another callback (chthonic.transfer
    // in an update): call()'s state is put back afterwards.
    template <class T, class Accept>
    std::optional<T> first_hook_result(sol::protected_function System::*slot, const char* name, const std::string& want,
                                       const sol::object& e, uint16_t map, Accept accept) {
        System* caller = current;
        const bool keep = std::exchange(keepResult, true);
        sol::object outerKept = std::exchange(kept, sol::object());
        const bool outerTrue = returnedTrue;
        std::optional<T> value;
        for (System& s : systems) {
            if (value) break;
            if (!s.ok || !(s.*slot).valid()) continue;
            run(s, s.*slot, name, e, map);
            const sol::object result = std::exchange(kept, sol::object());
            if (!s.ok || !result.valid() || result.get_type() == sol::type::lua_nil) continue;
            if ((value = accept(result))) continue;
            disable(s, std::format("{}: {} must return {}, got {}", s.path, name, want, to_string(lua.lua_state(), result)));
            log::error("Lua system disabled", {{"system", s.path}, {"callback", name}, {"error", s.error}});
        }
        current = caller;
        keepResult = keep;
        kept = std::move(outerKept);
        returnedTrue = outerTrue;
        return value;
    }

    std::optional<std::string> instance_key(entt::entity e, uint16_t map) {
        static const std::string want = std::format("nil or a non-empty string of at most {} bytes", kMaxInstanceKeyBytes);
        return first_hook_result<std::string>(&System::instanceKey, "instanceKey", want, sol::make_object(lua, wire(e)),
                                              map, [](const sol::object& r) -> std::optional<std::string> {
                                                  if (r.get_type() == sol::type::string && !r.as<std::string>().empty() &&
                                                      r.as<std::string>().size() <= kMaxInstanceKeyBytes)
                                                      return r.as<std::string>();
                                                  return std::nullopt;
                                              });
    }

    std::optional<uint32_t> layer_for(entt::entity e, uint16_t map) {
        static const std::string want = std::format("nil or a layer id (an integer in [0, {}])", UINT32_MAX);
        const sol::object id = e == entt::null ? sol::make_object(lua, sol::lua_nil) : sol::make_object(lua, wire(e));
        return first_hook_result<uint32_t>(&System::layerFor, "layerFor", want, id, map, layer_id);
    }

    // chthonic.map.area(e) / chthonic.map.areaAt(map, x, y, z): {id, name, flags, music} of the highest-priority area there
    // (flags include the parent's), or nil. Read-only: what an area's flags mean is up to the game's Lua.
    sol::object area_table(uint16_t map, float x, float y, float z) {
        const auto& areas = npcs.map(map)->areas;
        const int16_t i = map::area_at(areas, x, y, z);
        if (i < 0) return sol::make_object(lua, sol::lua_nil);
        const map::Area& a = areas.list[i];
        sol::table t = lua.create_table();
        t["id"] = a.id;
        t["name"] = a.name;
        t["flags"] = json_to_lua(lua, a.flags);
        if (!a.music.empty()) t["music"] = a.music;
        return t;
    }

    void bind_map_api() {
        mapApi = lua.create_table();
        mapApi.set_function("area", [this](sol::this_state L, const sol::object& id) {
            const entt::entity e = entity(L, id, "chthonic.map.area");
            const auto& pos = reg.get<world::Transform>(e).pos;
            return area_table(reg.get<world::MapRef>(e).map, pos.x, pos.y, pos.z);
        });
        mapApi.set_function("areaAt", [this](sol::this_state L, const sol::object& map, const sol::object& x,
                                             const sol::object& y, const sol::object& z) {
            const uint16_t id = map_id(L, "chthonic.map.areaAt: ", map);
            return area_table(id, finite(L, "chthonic.map.areaAt: ", "x", x), finite(L, "chthonic.map.areaAt: ", "y", y),
                              finite(L, "chthonic.map.areaAt: ", "z", z));
        });
        // chthonic.map.instance(): {id, key, map} of the instance copy this context runs (key nil for a keyless copy);
        // {id, map, layer = true, base} on a layered world map (base: id 0, the layer that never closes); nil on any
        // other world map.
        mapApi.set_function("instance", [this]() -> sol::object {
            const auto info = own_instance();
            if (!info) return sol::make_object(lua, sol::lua_nil);
            sol::table t = lua.create_table_with("id", info->id, "map", info->map);
            if (info->key) t["key"] = *info->key;
            if (info->layer) {
                t["layer"] = true;
                t["base"] = info->id == 0;
            }
            return t;
        });
        // chthonic.map.spawn(map): {position = {x, y, z}, heading} of the map's spawn point (map.json "spawn").
        mapApi.set_function("spawn", [this](sol::this_state L, const sol::object& map) {
            const map::MapInfo* info = npcs.map(map_id(L, "chthonic.map.spawn: ", map));
            return lua.create_table_with(
                "position", lua.create_table_with("x", info->spawn[0], "y", info->spawn[1], "z", info->spawn[2]),
                "heading", info->spawnHeading);
        });
        // chthonic.map.ground(map, x, z): the ground's height there, or nil off the map's tiles.
        mapApi.set_function("ground", [this](sol::this_state L, const sol::object& map, const sol::object& x,
                                             const sol::object& z) {
            const uint16_t id = map_id(L, "chthonic.map.ground: ", map);
            const auto y = npcs.ground(id, finite(L, "chthonic.map.ground: ", "x", x), finite(L, "chthonic.map.ground: ", "z", z));
            return y ? sol::make_object(lua, *y) : sol::make_object(lua, sol::lua_nil);
        });
        // chthonic.map.path(map, from, to): the navmesh path from `from` to `to` ({x=, y=, z=} each) as a list of points,
        // `from` snapped onto the mesh first and `to` last, or nil (no navmesh on the map, an end not near it, or no way).
        mapApi.set_function("path", [this](sol::this_state L, const sol::object& map, const sol::object& from,
                                           const sol::object& to) {
            const uint16_t id = map_id(L, "chthonic.map.path: ", map);
            const auto points = npcs.path(id, position(L, "chthonic.map.path: ", from, "from"),
                                          position(L, "chthonic.map.path: ", to, "to"));
            if (points.empty()) return sol::make_object(lua, sol::lua_nil);
            sol::table list = lua.create_table(int(points.size()), 0);
            for (const auto& p : points) list.add(lua.create_table_with("x", p.x, "y", p.y, "z", p.z));
            return sol::make_object(lua, list);
        });
    }

    // chthonic.world.post(target, event, data): target a loaded world map's id, "realm" or "all"; event and data as
    // chthonic.publish's. An instance map has no single context to post to.
    void bind_world_api() {
        worldApi = lua.create_table();
        worldApi.set_function("post", [this](sol::this_state L, const sol::object& target, const sol::object& name,
                                             const sol::object& value) {
            Post post;
            const double id = target.get_type() == sol::type::number ? target.as<double>() : NAN;
            if (target.get_type() == sol::type::string && target.as<std::string>() == "realm") post.to = Post::To::realm;
            else if (target.get_type() == sol::type::string && target.as<std::string>() == "all") post.to = Post::To::all;
            else if (id >= 0 && id <= 65535 && id == std::floor(id) && npcs.map(static_cast<uint16_t>(id))) {
                post.map = static_cast<uint16_t>(id);
                if (npcs.map(post.map)->instanced())
                    raise(L, std::format("chthonic.world.post: map {} is an instance map, with no single context to post to; "
                                         "post to \"realm\" or \"all\"", post.map));
            } else
                raise(L, std::format("chthonic.world.post: target must be a loaded map's id, \"realm\" or \"all\", got {}",
                                     to_string(L, target)));
            post.event = event_name(L, "chthonic.world.post", name);
            post.data = payload(L, "chthonic.world.post", value);
            npcs.post(std::move(post));
        });
    }

    void bind_api() {
        api = lua.create_table();

        // Matches are captured when query is called, in wire id order: entities gone later in the loop are skipped.
        // Asleep NPCs (World's tile activation) are never matched.
        api.set_function("query", [this](sol::this_state L, sol::variadic_args names) {
            std::vector<Type> types;
            for (const sol::object name : names) {
                if (name.get_type() != sol::type::string)
                    raise(L, std::format("component name must be a string, got {}", to_string(L, name)));
                types.push_back(type(L, name.as<std::string>()));
            }
            // Walk the smallest storage among the types (Transform: every player and NPC).
            const auto& transforms = reg.storage<world::Transform>();
            const entt::sparse_set* walk = &transforms;
            for (const Type& t : types)
                if (const auto* c = candidates(t); c && c->size() < walk->size()) walk = c;
            std::vector<uint32_t> matches;
            const auto& asleep = reg.storage<world::Asleep>();
            const auto match = [&](entt::entity e) {
                if (transforms.contains(e) && !asleep.contains(e) && std::ranges::all_of(types, [&](const Type& t) { return has(e, t); }))
                    matches.push_back(wire(e));
            };
            // With any NPC asleep, World tags every NPC Awake or Asleep: the players (none for an NPC-only type) and
            // the Awake NPCs may be the fewest to walk, so sleeping NPCs cost a query nothing.
            const bool npcOnly = std::ranges::any_of(types, [](const Type& t) { return t.builtin == Builtin::npc || t.builtin == Builtin::spawn; });
            const entt::sparse_set& players = reg.storage<world::CharacterRef>();
            const entt::sparse_set& awake = reg.storage<world::Awake>();
            if (!asleep.empty() && (npcOnly ? 0 : players.size()) + awake.size() < walk->size()) {
                if (!npcOnly)
                    for (auto e : players) match(e);
                for (auto e : awake) match(e);
            } else {
                for (auto e : *walk) match(e);
            }
            std::ranges::sort(matches);
            // nil ends the generic for.
            return std::function<sol::object()>([this, matches = std::move(matches), next = size_t{0}]() mutable {
                while (next < matches.size()) {
                    const uint32_t id = matches[next++];
                    if (in_world(reg, entt::entity{id}))
                        return sol::make_object(lua, id);
                }
                return sol::make_object(lua, sol::lua_nil);
            });
        });
        api.set_function("create", [this](sol::this_state L, const sol::object& name, const sol::object& opts) {
            return create_npc(L, name, opts);
        });
        api.set_function("transfer", [this](sol::this_state L, const sol::object& id, const sol::object& map,
                                            const sol::object& pos, const sol::object& heading) {
            return transfer(L, id, map, pos, heading);
        });
        // chthonic.setMovable(e, movable): false refuses every Move of the player (Correction back to where it stands)
        // until set true again. Not saved: a character logs in movable.
        api.set_function("setMovable", [this](sol::this_state L, const sol::object& id, const sol::object& movable) {
            const entt::entity e = entity(L, id, "chthonic.setMovable");
            if (!reg.all_of<world::CharacterRef>(e))
                raise(L, std::format("chthonic.setMovable: entity {} is an NPC; only players send moves", to_string(L, id)));
            if (movable.get_type() != sol::type::boolean)
                raise(L, std::format("chthonic.setMovable: movable must be a boolean, got {}", to_string(L, movable)));
            if (movable.as<bool>()) reg.remove<world::MoveLock>(e);
            else reg.emplace_or_replace<world::MoveLock>(e);
        });
        // chthonic.definitions(folder): the plain-data tables returned by every .lua file under LUA_ROOT/folder (recursively,
        // sorted by path) and, as a second result, their LUA_ROOT-relative paths. Read once per folder (the first call),
        // then fresh copies. folder is "/"-separated and relative, without "." or ".." parts (".server/quests"); a
        // missing folder is none. A file that fails, or doesn't return exactly one plain-data table, raises.
        api.set_function("definitions", [this](sol::this_state L, const sol::object& folder) {
            if (folder.get_type() != sol::type::string || !relative_folder(folder.as<std::string>()))
                raise(L, std::format("chthonic.definitions: folder must be a relative path such as \".server/quests\" "
                                     "(\"/\"-separated, no \".\" or \"..\" parts), got {}", to_string(L, folder)));
            const std::string name = folder.as<std::string>();
            auto it = definitions.find(name);
            if (it == definitions.end()) {
                Definitions loaded;
                for (const auto& file : definition_files(luaRoot, name.c_str())) {
                    const std::string path = lua_path(file, luaRoot);
                    nlohmann::json json;
                    std::string error;
                    try {
                        const sol::table table = run_definition_file(lua, file, path, "{...}");
                        if (!lua_to_json(table, json, error)) error = path + ": " + error;
                    } catch (const InvalidDefinition& e) {
                        error = e.what();
                    }
                    if (!error.empty()) raise(L, "chthonic.definitions: " + error);
                    loaded.tables.push_back(std::move(json));
                    loaded.paths.push_back(path);
                }
                it = definitions.emplace(name, std::move(loaded)).first;
            }
            sol::table tables = lua.create_table(), paths = lua.create_table();
            for (size_t i = 0; i < it->second.tables.size(); ++i) {
                tables[i + 1] = json_to_lua(lua, it->second.tables[i]);
                paths[i + 1] = it->second.paths[i];
            }
            return std::make_tuple(tables, paths);
        });
        // Players leave only by logging out: destroy takes NPCs.
        // chthonic.npcDef(name): {name =, template =, loot = <table or nil>} of an npcs/ definition (a fresh copy), nil if none.
        api.set_function("npcDef", [this](sol::this_state L, const sol::object& name) -> sol::object {
            if (name.get_type() != sol::type::string)
                raise(L, std::format("chthonic.npcDef: name must be a string, got {}", to_string(L, name)));
            const auto it = npcDefs.find(name.as<std::string>());
            if (it == npcDefs.end()) return sol::lua_nil;
            sol::table def = lua.create_table();
            def["name"] = it->second.name;
            def["template"] = it->second.templateName;
            if (!it->second.loot.is_null()) def["loot"] = json_to_lua(lua, nlohmann::json(it->second.loot));
            return def;
        });
        api.set_function("destroy", [this](sol::this_state L, const sol::object& id) {
            const entt::entity e = entity(L, id, "chthonic.destroy");
            if (!reg.all_of<world::Npc>(e))
                raise(L, std::format("chthonic.destroy: entity {} is a player; only NPCs can be destroyed", to_string(L, id)));
            npcs.destroy(e);
        });
        api.set_function("has", [this](sol::this_state L, const sol::object& id, const std::string& name) {
            return has(entity(L, id), type(L, name));
        });
        api.set_function("add", [this](sol::this_state L, const sol::object& id, const std::string& name) {
            const entt::entity e = entity(L, id);
            const Type t = type(L, name);
            const ComponentDef& def = writable(L, t);
            auto& p = *t.pool;
            if (p.contains(e)) return;
            p.emplace(e, ComponentData{default_component(def)});
            changed(e, def);
        });
        api.set_function("remove", [this](sol::this_state L, const sol::object& id, const std::string& name) {
            const entt::entity e = entity(L, id);
            const Type t = type(L, name);
            if (!t.def) raise(L, std::format("\"{}\" cannot be removed", name));
            auto& p = *t.pool;
            if (!p.contains(e)) return;
            p.remove(e);
            changed(e, *t.def);
        });
        api.set_function("get", [this](sol::this_state L, const sol::stack_object& id, const std::string& name) {
            const entt::entity e = entity(L, id);
            const Type t = type(L, name);
            if (!has(e, t)) raise(L, std::format("entity {} has no \"{}\"", to_string(L, sol::object(id)), name));
            if (t.builtin == Builtin::spawn) return spawn_table(reg.get<world::SpawnRef>(e).record);
            if (t.builtin == Builtin::transform) {
                const auto& tf = reg.get<world::Transform>(e);
                sol::table position = lua.create_table(0, 3);
                position.raw_set("x", tf.pos.x, "y", tf.pos.y, "z", tf.pos.z);
                sol::table table = lua.create_table(0, 3);
                table.raw_set("position", position, "heading", tf.heading, "map", reg.get<world::MapRef>(e).map);
                return table;
            }
            if (t.def) {
                const Json& json = t.pool->get(e).value;
                sol::table table = lua.create_table(0, static_cast<int>(t.def->fields.size()));
                for (const FieldDesc& field : t.def->fields)
                    table.raw_set(field.name, field_json_to_lua(lua, field, json[field.name]));
                return table;
            }
            sol::table table = lua.create_table();
            if (t.builtin == Builtin::character) {
                const auto& ch = reg.get<world::CharacterRef>(e);
                table["charId"] = static_cast<double>(ch.charId);
                table["name"] = ch.name;
                sol::table options = lua.create_table();
                for (const auto& [key, value] : ch.options) options[key] = value;
                table["options"] = options;
            } else {
                const auto& npc = reg.get<world::Npc>(e);
                table["name"] = npc.name;
                table["template"] = npc.templateName;
                table["npc"] = npc.npc;
            }
            return table;
        });
        // Partial tables: listed fields change, the rest keep their values. All-or-nothing: any bad field, or a value
        // min/max / enum values reject, leaves the component unchanged.
        // ponytail: each listed field goes through JSON to reuse the definition checks (field_from_json), the stored
        // value isn't copied or re-checked; convert straight from Lua if it shows in profiles.
        api.set_function("set", [this](sol::this_state L, const sol::stack_object& id, const std::string& name,
                                       const sol::table& fields) {
            const entt::entity e = entity(L, id);
            const Type t = type(L, name);
            if (!has(e, t)) raise(L, std::format("entity {} has no \"{}\"", to_string(L, sol::object(id)), name));
            if (t.builtin == Builtin::transform && reg.all_of<world::Npc>(e)) return set_npc_transform(L, e, fields);
            const ComponentDef& def = writable(L, t);
            std::string error;
            std::optional<std::vector<CheckedField>> listed = checked_fields(def, fields, error);
            if (!listed) raise(L, error);
            Json& value = t.pool->get(e).value;
            bool differs = false;
            for (CheckedField& f : *listed)
                if (Json& stored = value[f.field->name]; stored != f.value) {
                    stored = std::move(f.value);
                    differs = true;
                }
            if (differs) changed(e, def);
        });

        // Events: queued, delivered by drain_events() after every update (docs/world/events.html). Subscriptions belong to
        // the calling system; a handle is a light userdata holding the subscription id (no metatable a script could
        // edit).
        api.set_function("publish", [this](sol::this_state L, const sol::object& name, const sol::object& value) {
            std::string event = event_name(L, "chthonic.publish", name);
            bus.publish(std::move(event), payload(L, "chthonic.publish", value), who());
        });
        api.set_function("subscribe", [this](sol::this_state L, const sol::object& name, const sol::object& fn) {
            std::string event = event_name(L, "chthonic.subscribe", name);
            if (fn.get_type() != sol::type::function)
                raise(L, std::format("chthonic.subscribe: callback must be a function, got {}", to_string(L, fn)));
            if (!current) raise(L, "chthonic.subscribe: only a system can subscribe");
            System* owner = current; // systems never move: load() reserved them
            std::string what = std::format("event '{}'", event);
            EventBus::Subscription subscription = bus.subscribe(
                std::move(event), [this, owner, what = std::move(what),
                                   // on the main thread: fn's own may be a coroutine, collected before the drain
                                   callback = sol::protected_function(lua.lua_state(), fn)](const nlohmann::json& p) {
                    run(*owner, callback, what, json_to_lua(lua, p)); // skipped once the owner is disabled
                });
            const uint64_t id = subscription.id();
            owner->subscriptions.push_back(std::move(subscription));
            return reinterpret_cast<void*>(static_cast<uintptr_t>(id)); // ponytail: 64-bit ids in a pointer, x64 only
        });
        api.set_function("unsubscribe", [this](sol::this_state L, const sol::object& handle) {
            if (handle.get_type() != sol::type::lightuserdata)
                raise(L, std::format("chthonic.unsubscribe: expected a handle from chthonic.subscribe, got {}", to_string(L, handle)));
            const uint64_t id = reinterpret_cast<uintptr_t>(handle.as<void*>());
            // Another system's handle, or one already unsubscribed, isn't in current's list: nothing happens.
            if (current)
                std::erase_if(current->subscriptions, [id](const EventBus::Subscription& s) { return s.id() == id; });
        });

        // Client events: queued, sent as ServerEvent after component replication (docs/world/events.html).
        api.set_function("sendTo", [this](sol::this_state L, const sol::object& id, const sol::object& name,
                                          const sol::object& value) {
            queue_client_event(L, "chthonic.sendTo", ClientEvent::To::owner, target(L, "chthonic.sendTo", id), name, value);
        });
        api.set_function("sendNearby", [this](sol::this_state L, const sol::object& id, const sol::object& name,
                                              const sol::object& value) {
            queue_client_event(L, "chthonic.sendNearby", ClientEvent::To::nearby, target(L, "chthonic.sendNearby", id), name,
                               value);
        });
        api.set_function("broadcast", [this](sol::this_state L, const sol::object& name, const sol::object& value) {
            queue_client_event(L, "chthonic.broadcast", ClientEvent::To::all, 0, name, value);
        });
        // chthonic.reply(entity, requestId, ok, dataOrReason): answers a "client.<name>" event's requestId (a client's
        // ClientEvent with requestId != 0) to entity's owner: ok true with data (a table, nil: {}), false with the
        // reason (a string).
        api.set_function("reply", [this](sol::this_state L, const sol::object& id, const sol::object& requestId,
                                         const sol::object& ok, const sol::object& value) {
            const uint32_t entity = target(L, "chthonic.reply", id);
            const uint32_t request = requestId.get_type() == sol::type::number ? target(L, "chthonic.reply", requestId) : 0;
            if (request == 0)
                raise(L, std::format("chthonic.reply: requestId must be a client event's requestId, got {}", to_string(L, requestId)));
            if (ok.get_type() != sol::type::boolean)
                raise(L, std::format("chthonic.reply: ok must be a boolean, got {}", to_string(L, ok)));
            nlohmann::json json;
            if (ok.as<bool>()) {
                json = value.get_type() == sol::type::lua_nil ? nlohmann::json::object() : payload(L, "chthonic.reply", value);
            } else if (value.get_type() == sol::type::string) {
                json = value.as<std::string>();
            } else {
                raise(L, std::format("chthonic.reply: a rejection's reason must be a string, got {}", to_string(L, value)));
            }
            std::string text = json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            if (!proto::ServerEvent::fits(3, text.size())) // Reply: u32 + u8 + str, the size of a 3-byte-named event
                raise(L, std::format("chthonic.reply: {} bytes of data is too large to send", text.size()));
            clientEvents.push_back({ClientEvent::To::reply, entity, {}, std::move(text), request, ok.as<bool>()});
        });

        // chthonic.log(message [, fields]): fields is plain data (as an event payload) and becomes JSON fields on the line.
        const auto logger = [this](const char* function, void (*write)(std::string_view, nlohmann::json)) {
            return [this, function, write](sol::this_state L, const sol::object& message, const sol::object& fields) {
                nlohmann::json json = nlohmann::json::object();
                if (fields.valid()) {
                    if (fields.get_type() != sol::type::table)
                        raise(L, std::format("{}: fields must be a table, got {}", function, to_string(L, fields)));
                    json = payload(L, function, fields);
                    if (!json.is_object()) raise(L, std::format("{}: fields must be a table of named fields", function));
                    for (const char* reserved : {"ts", "level", "msg"})
                        if (json.contains(reserved))
                            raise(L, std::format("{}: \"{}\" is reserved and can't be a field", function, reserved));
                }
                write(std::format("{}: {}", who(), to_string(L, message)), std::move(json));
            };
        };
        api.set_function("debug", logger("chthonic.debug", log::debug));
        api.set_function("log", logger("chthonic.log", log::info));
        api.set_function("warn", logger("chthonic.warn", log::warn));
        api.set_function("error", logger("chthonic.error", log::error));
    }
};

Runtime::Runtime(entt::registry& reg, Defs defs, NpcHooks npcs)
    : impl_(std::make_unique<Impl>(reg, std::move(defs), std::move(npcs))) {}
Runtime::~Runtime() = default;

std::vector<std::string> Runtime::load(const std::filesystem::path& root, std::vector<std::string> gameMachines) {
    impl_->luaRoot = root;
    // Definitions first: an init may already use chthonic.fsm.
    std::vector<std::string> errors = impl_->load_machines(root, std::move(gameMachines));
    for (std::string& e : impl_->load_items(root)) errors.push_back(std::move(e)); // before the NPCs: loot names items
    for (std::string& e : impl_->load_npcs(root)) errors.push_back(std::move(e)); // before init: it may chthonic.create one
    std::vector<std::filesystem::path> files = lua_files(root / "systems", false);
    std::ranges::sort(files, {}, [](const auto& f) { return f.filename(); });

    impl_->systems.reserve(files.size()); // subscriptions point at their System
    for (const auto& file : files) {
        System& system = impl_->systems.emplace_back();
        system.path = "systems/" + file.filename().generic_string();
        system.file = file;
        if (std::string error = impl_->load(system); !error.empty()) {
            impl_->disable(system, error);
            errors.push_back(std::move(error));
        }
    }
    if (errors.empty()) impl_->fsm.enterAll(); // the server won't start otherwise
    return errors;
}

std::vector<std::string> Runtime::load_game_machines(const std::filesystem::path& root, std::vector<std::string> gameMachines) {
    impl_->luaRoot = root;
    std::vector<std::string> errors = impl_->load_machines(root, std::move(gameMachines));
    if (errors.empty()) impl_->fsm.enterAll();
    return errors;
}

void Runtime::update(float dt) {
    for (System& s : impl_->systems) impl_->run(s, s.update, "update", dt);
}

void Runtime::enter(entt::entity e) {
    for (System& s : impl_->systems) impl_->run(s, s.onEnter, "onEnter", wire(e));
}

void Runtime::leave(entt::entity e) {
    for (System& s : impl_->systems) impl_->run(s, s.onLeave, "onLeave", wire(e));
}

Carried Runtime::leave_map(entt::entity e) {
    Carried carried;
    for (System& s : impl_->systems) {
        impl_->keepResult = true;
        impl_->run(s, s.onMapLeave, "onMapLeave", wire(e));
        impl_->keepResult = false;
        const sol::object data = std::exchange(impl_->kept, sol::object());
        if (!s.ok || !data.valid() || data.get_type() == sol::type::lua_nil) continue;
        nlohmann::json json;
        if (std::string error; !lua_to_json(data, json, error)) {
            impl_->disable(s, std::format("{}: onMapLeave must return plain data: {}", s.path, error));
            log::error("Lua system disabled", {{"system", s.path}, {"callback", "onMapLeave"}, {"error", s.error}});
            continue;
        }
        carried.emplace(s.path, std::move(json));
    }
    return carried;
}

void Runtime::enter_map(entt::entity e, const Carried& carried) {
    for (System& s : impl_->systems) {
        if (!s.ok || !s.onMapEnter.valid()) continue;
        const auto it = carried.find(s.path);
        impl_->run(s, s.onMapEnter, "onMapEnter", wire(e),
                   it == carried.end() ? sol::make_object(impl_->lua, sol::lua_nil) : json_to_lua(impl_->lua, it->second));
    }
}

std::optional<std::string> Runtime::instance_key(entt::entity e, uint16_t map) { return impl_->instance_key(e, map); }
std::optional<uint32_t> Runtime::layer_for(entt::entity e, uint16_t map) { return impl_->layer_for(e, map); }

bool Runtime::validate_character(const std::string& name, const proto::CharOptions& options) {
    bool hooked = false;
    for (System& s : impl_->systems) {
        if (!s.ok || !s.validateCharacter.valid()) continue;
        hooked = true;
        sol::table table = impl_->lua.create_table();
        for (const auto& [key, value] : options) table[key] = value;
        std::string error = impl_->call(s, s.validateCharacter, "validateCharacter", name, table);
        if (!error.empty()) {
            impl_->disable(s, std::move(error));
            log::error("Lua system disabled", {{"system", s.path}, {"callback", "validateCharacter"}, {"error", s.error}});
            return false;
        }
        if (!impl_->returnedTrue) return false;
    }
    return hooked || options.empty();
}

void Runtime::shutdown() {
    for (System& s : impl_->systems) impl_->run(s, s.shutdown, "shutdown");
    impl_->bus.dropQueued();
}

void Runtime::drain_events() { impl_->bus.drain(); }

EventBus& Runtime::bus() { return impl_->bus; }

StateMachines& Runtime::fsm() { return impl_->fsm; }

std::vector<ClientEvent> Runtime::take_client_events() { return std::exchange(impl_->clientEvents, {}); }

const NpcDef* Runtime::npc(std::string_view name) const {
    const auto it = impl_->npcDefs.find(name);
    return it == impl_->npcDefs.end() ? nullptr : &it->second;
}

std::vector<SystemStatus> Runtime::status() const {
    std::vector<SystemStatus> out;
    for (const System& s : impl_->systems) out.push_back({s.path, s.ok, s.error});
    return out;
}

} // namespace chthonic::lua
