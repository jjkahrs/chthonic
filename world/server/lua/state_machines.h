// Finite state machines: named states with optional allowedFrom rules and enter/exit callbacks, defined by Lua files
// under LUA_ROOT/fsms/ (scanned recursively, one machine per file). Same rules, warnings and
// fsm.changed event as the client's, without display helpers (peek, gameStates), play sessions and scene
// switches (reset, attach, detach), hot reload (define replacing running definitions, rescans) and the per-frame
// syncAll. Game machines (GAME_STATE_MACHINES) run for the server's lifetime; entity machines come from the
// StateMachine built-in on characters. One per lua::Runtime (fsm()). See docs/world/lua.html.
#pragma once
#include "components.h"
#include "event_bus.h"

#include <entt/entity/registry.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace chthonic::lua {

constexpr const char* kStateMachinesDir = "fsms";
constexpr const char* kAnyState = "*";
constexpr const char* kStateChangedEvent = "fsm.changed";
constexpr const char* kStateMachineComponent = "StateMachine"; // the built-in: { machine, initialState }

using EntityId = uint32_t; // a character's wire id

// What enter/exit callbacks get. from is empty when a machine enters its first state.
struct Transition {
    std::string machine;
    std::optional<EntityId> entity; // empty for game machines
    std::optional<std::string> from;
    std::string to;
};
using StateCallback = std::function<void(const Transition&)>;

struct StateDef {
    bool fromAny = true;        // allowedFrom omitted or holding "*"
    std::set<std::string> from; // allowedFrom otherwise
    StateCallback enter, exit;  // optional
};

struct StateMachineDef {
    std::string name;
    std::string path; // LUA_ROOT-relative definition file
    std::string initial;
    std::map<std::string, StateDef> states; // by name

    // to is a state that may be entered from from (also when from == to).
    bool allows(const std::string& from, const std::string& to) const;
    std::vector<std::string> stateNames() const; // sorted
};

// The current states of the game machines and of every character's machine. No Lua dependency: callbacks are plain
// std::functions (Runtime binds the Lua ones). Entity machines read the StateMachine component's pool (components.h).
//
// Errors: a bad target (unknown or unavailable game machine, invalid entity, entity without StateMachine or with no
// valid machine) throws std::invalid_argument; chthonic.fsm turns it into a Lua error at the calling line.
class StateMachines {
public:
    using Target = std::variant<std::string, EntityId>; // a game machine name or an entity id

    StateMachines(EventBus& bus, entt::registry& reg)
        : bus_(bus), reg_(reg), machines_(pool(reg, kStateMachineComponent)) {}
    StateMachines(const StateMachines&) = delete;
    StateMachines& operator=(const StateMachines&) = delete;

    // The machine's current state. Enters its initial state first if it hasn't yet (a game machine touched during the
    // systems' init, before enterAll).
    std::string state(const Target& target);
    // Applies a transition now: exit(old), switch, enter(new), publish fsm.changed. False (with a warning, nothing
    // run) if the state is unknown or not allowed from the current one. Called during another transition's
    // enter/exit, it is queued, applied after that transition in request order, and returns true.
    bool change(const Target& target, const std::string& to);
    // The same check as change, without side effects or logging.
    bool canChange(const Target& target, const std::string& to);
    std::vector<std::string> states(const Target& target); // sorted

    // --- startup and sync (Runtime) ---

    // Once, before the systems' init: the valid definitions and the game machines to run. Returns one error per listed
    // name without a valid definition.
    std::vector<std::string> setup(std::vector<StateMachineDef> defs, std::vector<std::string> gameMachines);
    const StateMachineDef* definition(const std::string& name) const;
    // After the systems' init: listed game machines enter their initial state. No fsm.changed events.
    void enterAll();
    // A character that gained StateMachine, or whose "machine" changed, enters its initial state; one that lost it
    // forgets its machine. Called after chthonic.add/chthonic.set/chthonic.remove.
    void syncEntity(entt::entity entity);

    // --- map contexts (docs/world/maps.html#map-contexts) ---

    // The game machines run in the realm context only. A map context's StateMachines forwards instead: its game
    // machine states are a copy (mirror) the World refreshes at each serial point, enterAll does nothing, and an
    // allowed change of a game machine (checked and warned about as usual) goes to forward and returns true, for the
    // realm to apply at the next serial point; no callback runs here.
    using Forward = std::function<void(const std::string& machine, const std::string& to)>;
    void forward_game_machines(Forward forward) { forward_ = std::move(forward); }
    void mirror(const std::map<std::string, std::string>& states) { game_ = states; }
    const std::map<std::string, std::string>& game_states() const { return game_; } // the ones entered, by name

private:
    struct Queued {
        Target target;
        std::string to;
    };
    // One machine, valid until a callback runs (they may change the registry): copy what outlives one.
    struct Instance {
        const StateMachineDef* def;
        const std::string* current; // in game_ or StateMachineRuntime; null or empty: not entered yet
        std::optional<EntityId> entity;
        entt::entity handle = entt::null; // entity machines
    };

    // Throws std::invalid_argument for a bad target. enter: a machine not entered yet enters its initial state.
    Instance resolve(const Target& target, bool enter);
    // Where machine's current state is stored; null if it is gone (or the entity's StateMachine now names another).
    std::string* slot(const Target& target, const std::string& machine);
    void enterInitial(const Target& target, const StateMachineDef& def, const std::string& initial);
    bool apply(const Target& target, const std::string& to);
    void runQueue();
    void call(const StateCallback& callback, const StateMachineDef& def, const char* what, const std::string& state,
              const Transition& t);
    std::string label(const Instance& machine) const;
    std::string entityInitial(entt::entity entity, const StateMachineDef& def, bool warn);
    // What it enters first (entity: a valid initialState, else the definition's initial).
    std::string initialOf(const Instance& machine);
    entt::entity find(EntityId id) const; // entt::null if no such character
    // StateMachine "machine", in the component (copy it before a callback runs); null without one.
    const std::string* machineOf(entt::entity entity) const;

    EventBus& bus_;
    entt::registry& reg_;
    // The StateMachine pool, resolved once: EnTT keeps each storage in a shared_ptr (registry.hpp pool_container_type),
    // so the reference outlives later pools; only registry::reset(id), never called here, would drop it.
    entt::storage_for_t<ComponentData>& machines_;
    std::map<std::string, StateMachineDef> defs_;
    std::vector<std::string> gameList_;
    std::map<std::string, std::string> game_; // name -> current state, for game machines that have entered one
    std::deque<Queued> queue_;
    bool busy_ = false; // a transition's callbacks are running
    Forward forward_;   // set: a map context's (see forward_game_machines)
};

// Runtime-only (never saved or replicated): a character machine's definition name and current state.
struct StateMachineRuntime {
    std::string machine;
    std::string current; // empty if the definition was missing when it tried to enter
};

} // namespace chthonic::lua
