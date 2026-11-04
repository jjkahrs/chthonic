// State machine definition files (LUA_ROOT/fsms/**/*.lua) read with Lua. Same format, checks and error
// messages as the client's; no scan cache or file-time tracking for hot reload.
// Only lua/*.cpp include this (sol2 stays out of every other header).
#pragma once
#include "state_machines.h"

#include <sol/sol.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace chthonic::lua {

struct StateMachineDefinitions {
    std::vector<StateMachineDef> valid;
    std::vector<std::string> errors; // one per rejected file, each starting with its path, sorted by path
};

// Turns a state's enter or exit function into a callback. what is e.g. "enter \"open\"".
using CallbackBinder =
    std::function<StateCallback(sol::protected_function fn, const std::string& path, const std::string& what)>;

// Loads and checks every .lua file under root/fsms/ (none if the folder is missing), each run in its own
// environment() (a sandboxed one). Duplicate names reject every file of that name.
StateMachineDefinitions parse_state_machine_definitions(sol::state& lua, const std::filesystem::path& root,
                                                        const std::function<sol::environment()>& environment,
                                                        const CallbackBinder& bind);

} // namespace chthonic::lua
