// NPC definition files (LUA_ROOT/npcs/**/*.lua, server only: clients never read npcs/) read with Lua, and the
// components table they share with chthonic.create's opts.components. See docs/world/npcs.html#npc-definitions.
// Only lua/*.cpp include this (sol2 stays out of every other header).
#pragma once
#include "items.h"
#include "runtime.h"

#include <sol/sol.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace chthonic::lua {

constexpr const char* kNpcsDir = "npcs";

using MachineExists = std::function<bool(const std::string& name)>; // a valid fsms/ definition

struct NpcDefsResult {
    NpcDefs defs;
    std::vector<std::string> errors; // one per rejected file, each starting with its path, sorted by path
};

// Loads and checks every .lua file under root/npcs/ (none if the folder is missing), each run in its own sandboxed
// environment. defs: the writable component types (the Lua components and StateMachine); items: what a loot table may
// name. Duplicate names reject every file of that name.
NpcDefsResult load_npc_defs(sol::state& lua, const std::filesystem::path& root, const Defs& defs,
                            const MachineExists& machine, const ItemDefs& items);

// A components table {Type = {field = value, ...}, ...} (nil: none) merged per field over base ({"Type": value, ...}):
// a type not in base starts at its defaults. Every type must be in defs (the read-only built-ins are not), every field
// valid as for chthonic.set, and a StateMachine's "machine" a valid definition. nullopt and error on the first problem.
std::optional<Json> merge_components(const sol::object& components, Json base, const Defs& defs,
                                     const MachineExists& machine, std::string& error);

} // namespace chthonic::lua
