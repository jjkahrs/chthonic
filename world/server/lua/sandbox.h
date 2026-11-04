// Lua sandbox and value conversion shared by component definitions (components.cpp) and systems (runtime.cpp).
// Kept the same as the client's sandbox so scripts behave the same on client and server. Only lua/*.cpp include this
// (sol2 stays out of every other header).
#pragma once
#include "components.h"

#include <sol/sol.hpp>

#include <chrono>
#include <initializer_list>
#include <stdexcept>

namespace chthonic::lua {

// Opens only base, table, string and math and removes the base functions that load code or reach outside an
// environment: no io, os, ffi, debug, jit, bit, package, require, dofile, loadfile, load, loadstring.
// Also installs the runaway-script watchdog (see ScriptBudget).
void open_sandbox(sol::state& lua);
// A fresh environment with its own copies of the globals and library tables, so one file's globals never reach another.
sol::environment sandbox_environment(sol::state& lua);
// Reads file and loads it as text (bytecode can break out of the sandbox) into env. chunkName "@path" gives
// "path:line: ..." errors. On failure returns an invalid function and sets error.
sol::protected_function load_sandboxed(sol::state& lua, const std::filesystem::path& file, const std::string& chunkName,
                                       const sol::environment& env, std::string& error);

// Each entry into Lua from C++ holds a ScriptBudget and may run kScriptBudget of wall-clock time before a count hook
// raises a Lua error. The jit library is never opened, which leaves LuaJIT's JIT compiler (where hooks don't fire) off.
constexpr std::chrono::seconds kScriptBudget{2};
class ScriptBudget {
public:
    explicit ScriptBudget(lua_State* L);
    ~ScriptBudget();
    ScriptBudget(const ScriptBudget&) = delete;
    ScriptBudget& operator=(const ScriptBudget&) = delete;

private:
    lua_State* L_;
    bool outer_;
};

std::string to_string(lua_State* L, const sol::object& value); // Lua tostring

// Definition-file checks; each throws InvalidDefinition, its message starting with what.
struct InvalidDefinition : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void check_keys(const sol::table& table, std::initializer_list<const char*> allowed, const std::string& what);
// No reachable table has a metatable, so reading the definition runs no Lua code.
void check_plain_data(const sol::table& table, const std::string& what);
// Every .lua file under root/dir (recursively; none if the folder is missing), sorted.
std::vector<std::filesystem::path> definition_files(const std::filesystem::path& root, const char* dir);
// Every .lua file in folder (with recursive, under it too), sorted; none if the folder is missing. Kept after
// cache_lua_files (runtime.h), as the file texts load_sandboxed reads.
std::vector<std::filesystem::path> lua_files(const std::filesystem::path& folder, bool recursive);
// file relative to root, "/"-separated (std::filesystem::relative, which stats every part of both paths: kept too).
std::string lua_path(const std::filesystem::path& file, const std::filesystem::path& root);
// Runs a definition file (path: root-relative, for messages) in a fresh sandboxed environment; it must return exactly one
// plain-data table, else throws with "<path>: must return exactly one table <shape>".
sol::table run_definition_file(sol::state& lua, const std::filesystem::path& file, const std::string& path,
                               const std::string& shape);
// value is a sequence {v1, v2, ...}; returns its items.
std::vector<sol::object> sequence(const sol::object& value, const std::string& what);

// A Lua value as the JSON a component field takes ({x=,y=,z=[,w=]} -> array, sequences -> arrays, integral numbers ->
// integers). Shape errors set error and return false; type and range checks are left to field_from_json.
bool lua_to_field_json(const FieldDesc& field, const sol::object& value, Json& out, std::string& error,
                       bool element = false);
// The reverse, for a valid canonical value: arrays become 1-based sequences.
sol::object field_json_to_lua(sol::state_view lua, const FieldDesc& field, const Json& value, bool element = false);
// chthonic.set's partial update: each listed field converted and checked like component_from_json, in definition order.
// nullopt and error (e.g. "\"Health\": unknown field \"x\"") on any bad field.
struct CheckedField {
    const FieldDesc* field;
    Json value; // canonical
};
std::optional<std::vector<CheckedField>> checked_fields(const ComponentDef& def, const sol::table& fields,
                                                       std::string& error);
// The listed fields over value, then the whole of it checked by component_from_json; errors as checked_fields'.
std::optional<Json> set_fields(const ComponentDef& def, Json value, const sol::table& fields, std::string& error);

// An event payload (plain data) as JSON: nil -> null, booleans, finite numbers (integral ones as integers), strings,
// and tables whose keys are exactly 1..n (arrays) or all strings (objects; an empty table is {}), nested at most
// kMaxPayloadDepth tables deep. Anything else (functions, userdata, threads, cycles, mixed keys) sets error and
// returns false.
constexpr int kMaxPayloadDepth = 32;
bool lua_to_json(const sol::object& value, nlohmann::json& out, std::string& error);
// The reverse: arrays become 1-based sequences, objects string-keyed tables, null nil. Always a fresh value.
sol::object json_to_lua(sol::state_view lua, const nlohmann::json& value);

} // namespace chthonic::lua
