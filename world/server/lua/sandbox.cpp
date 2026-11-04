#include "sandbox.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace chthonic::lua {
namespace {

// cache_lua_files: what was read, by path. Locked: chthonic.definitions reads on first use, during a parallel update.
struct FileCache {
    std::mutex mu;
    bool on = false;
    std::map<std::string, std::optional<std::string>> texts;               // nullopt: could not be opened
    std::map<std::pair<std::string, bool>, std::vector<std::filesystem::path>> lists; // (folder, recursive)
    std::map<std::pair<std::string, std::string>, std::string> paths;                 // (file, root)
};
FileCache& file_cache() {
    static FileCache cache;
    return cache;
}

std::optional<std::string> read_text(const std::filesystem::path& file) {
    FileCache& c = file_cache();
    std::unique_lock lk(c.mu);
    if (const auto it = c.texts.find(file.string()); c.on && it != c.texts.end()) return it->second;
    lk.unlock();
    std::optional<std::string> text;
    if (std::ifstream in(file, std::ios::binary); in) {
        std::ostringstream bytes;
        bytes << in.rdbuf();
        text = bytes.str();
    }
    lk.lock();
    if (c.on) c.texts[file.string()] = text;
    return text;
}

constexpr const char* kUnsafeGlobals[] = {"dofile", "loadfile", "load", "loadstring", "getfenv", "setfenv", "require", "module"};
constexpr const char* kVectorKeys[] = {"x", "y", "z", "w"};
constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53: Lua numbers are doubles

constexpr int kHookInstructions = 10000; // between clock checks
using Clock = std::chrono::steady_clock;
// One per thread, shared by every Lua state (each map context has one): a thread enters one state at a time, and an
// entry never reaches into another context's state, so the outermost entry's budget is the one that counts.
thread_local Clock::time_point g_deadline = Clock::time_point::max();

void watchdog(lua_State* L, lua_Debug*) {
    if (Clock::now() < g_deadline) return;
    lua_sethook(L, watchdog, LUA_MASKCOUNT, 1); // every instruction until the entry point returns: pcall can't hide it
    luaL_where(L, 0);
    lua_pushfstring(L, "script ran longer than %d seconds (infinite loop?)", static_cast<int>(kScriptBudget.count()));
    lua_concat(L, 2);
    lua_error(L);
}

bool exact_integer(double number) { return number == std::floor(number) && std::abs(number) <= kMaxExactInteger; }

size_t vector_size(FieldKind kind) { return kind == FieldKind::Vec3 ? 3 : kind == FieldKind::Quat ? 4 : 0; }

// name and element (its 1-based index in an array field, 0 for none) only label errors.
bool scalar_to_json(FieldKind kind, const std::string& name, size_t element, const sol::object& value, Json& out,
                    std::string& error) {
    const auto label = [&] {
        return element ? std::format("\"{}\" element {}", name, element) : std::format("\"{}\"", name);
    };
    if (const size_t count = vector_size(kind)) {
        out = Json::array();
        if (value.get_type() == sol::type::table) {
            const sol::table table = value.as<sol::table>();
            for (size_t i = 0; i < count; ++i) {
                const sol::object c = table[kVectorKeys[i]];
                if (c.get_type() == sol::type::number && std::isfinite(c.as<double>())) out.push_back(c.as<double>());
            }
        }
        if (out.size() == count) return true;
        error = std::format("{} must be a table {{x=, y=, z={}}} of finite numbers", label(), count == 4 ? ", w=" : "");
        return false;
    }
    switch (value.get_type()) {
    case sol::type::number: {
        const double number = value.as<double>();
        if (!std::isfinite(number)) {
            error = std::format("{} must be a finite number", label());
            return false;
        }
        if (exact_integer(number)) out = static_cast<int64_t>(number); // an integer, so int fields accept it
        else out = number;
        return true;
    }
    case sol::type::boolean: out = value.as<bool>(); return true;
    case sol::type::string: out = value.as<std::string>(); return true;
    default: out = nullptr; return true; // rejected by the JSON checks with the field's expected type
    }
}

sol::object scalar_to_lua(sol::state_view lua, FieldKind kind, const Json& value) {
    if (const size_t count = vector_size(kind)) {
        sol::table table = lua.create_table(0, static_cast<int>(count));
        for (size_t i = 0; i < count; ++i) table.raw_set(kVectorKeys[i], value[i].get<double>());
        return table;
    }
    if (value.is_boolean()) return sol::make_object(lua, value.get<bool>());
    if (value.is_number()) return sol::make_object(lua, value.get<double>());
    return sol::make_object(lua, value.get<std::string>()); // strings and enum names
}

// label names the value in errors: "payload", "payload.id", "payload[2]". path holds the tables being converted,
// outermost first, to catch cycles.
bool payload_to_json(const sol::object& value, const std::string& label, std::vector<const void*>& path,
                     nlohmann::json& out, std::string& error) {
    switch (value.get_type()) {
    case sol::type::lua_nil:
    case sol::type::none: out = nullptr; return true;
    case sol::type::boolean: out = value.as<bool>(); return true;
    case sol::type::string: out = value.as<std::string>(); return true;
    case sol::type::number: {
        const double number = value.as<double>();
        if (!std::isfinite(number)) {
            error = std::format("{} must be a finite number", label);
            return false;
        }
        if (exact_integer(number))
            out = static_cast<int64_t>(number);
        else
            out = number;
        return true;
    }
    case sol::type::table: break;
    default:
        error = std::format("{}: a {} can't be sent in an event (only nil, booleans, numbers, strings and tables)",
                            label, sol::type_name(value.lua_state(), value.get_type()));
        return false;
    }
    const sol::table table = value.as<sol::table>();
    if (std::ranges::find(path, table.pointer()) != path.end()) {
        error = std::format("{}: the table contains itself (cycle)", label);
        return false;
    }
    if (path.size() >= static_cast<size_t>(kMaxPayloadDepth)) {
        error = std::format("{}: tables are nested more than {} deep", label, kMaxPayloadDepth);
        return false;
    }
    // Keys exactly 1..n: n distinct positive integers whose largest is n.
    size_t count = 0, strings = 0;
    double largest = 0.0;
    bool positiveIntegers = true;
    for (const auto& [key, unused] : table) {
        ++count;
        if (key.get_type() == sol::type::string) {
            ++strings;
            positiveIntegers = false;
            continue;
        }
        const double k = key.get_type() == sol::type::number ? key.as<double>() : 0.0;
        positiveIntegers = positiveIntegers && k >= 1.0 && k == std::floor(k);
        largest = std::max(largest, k);
    }
    path.push_back(table.pointer());
    bool ok = true;
    if (count > 0 && positiveIntegers && largest == static_cast<double>(count)) {
        out = nlohmann::json::array();
        for (size_t i = 1; ok && i <= count; ++i)
            ok = payload_to_json(table[i], std::format("{}[{}]", label, i), path, out.emplace_back(), error);
    } else if (strings == count) { // also the empty table
        out = nlohmann::json::object();
        for (const auto& [key, item] : table) {
            const std::string name = key.as<std::string>();
            if (!(ok = payload_to_json(item, std::format("{}.{}", label, name), path, out[name], error))) break;
        }
    } else {
        error = std::format("{}: table keys must be exactly 1..n (an array) or all strings (an object)", label);
        ok = false;
    }
    path.pop_back();
    return ok;
}

} // namespace

void open_sandbox(sol::state& lua) {
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math);
    for (const char* name : kUnsafeGlobals) lua.globals()[name] = sol::lua_nil;
    // Every string shares one metatable whose __index is the real string table: hide it so no file can edit the
    // methods every other file uses. Method calls still work.
    lua_pushliteral(lua.lua_state(), "");
    lua_getmetatable(lua.lua_state(), -1);
    lua_pushboolean(lua.lua_state(), 0);
    lua_setfield(lua.lua_state(), -2, "__metatable");
    lua_pop(lua.lua_state(), 2);
    lua_sethook(lua.lua_state(), watchdog, LUA_MASKCOUNT, kHookInstructions);
}

ScriptBudget::ScriptBudget(lua_State* L) : L_(L), outer_(g_deadline == Clock::time_point::max()) {
    if (outer_) g_deadline = Clock::now() + kScriptBudget;
}

ScriptBudget::~ScriptBudget() {
    if (!outer_) return;
    g_deadline = Clock::time_point::max();
    lua_sethook(L_, watchdog, LUA_MASKCOUNT, kHookInstructions);
}

sol::environment sandbox_environment(sol::state& lua) {
    sol::environment env(lua, sol::create);
    for (const auto& [key, value] : lua.globals()) {
        if (value.get_type() != sol::type::table) {
            env[key] = value;
            continue;
        }
        sol::table copy = lua.create_table();
        for (const auto& [k, v] : value.as<sol::table>()) copy[k] = v;
        env[key] = copy;
    }
    env["_G"] = env;
    return env;
}

sol::protected_function load_sandboxed(sol::state& lua, const std::filesystem::path& file, const std::string& chunkName,
                                       const sol::environment& env, std::string& error) {
    const std::optional<std::string> text = read_text(file);
    if (!text) {
        error = std::format("cannot open '{}'", file.string());
        return {};
    }
    sol::load_result chunk = lua.load(*text, chunkName, sol::load_mode::text);
    if (!chunk.valid()) {
        error = chunk.get<sol::error>().what();
        return {};
    }
    sol::protected_function body = chunk;
    sol::set_environment(env, body);
    return body;
}

std::string to_string(lua_State* L, const sol::object& value) {
    sol::protected_function tostring = sol::state_view(L)["tostring"];
    sol::protected_function_result result = tostring(value);
    return result.valid() && result.get_type() == sol::type::string ? result.get<std::string>() : "?";
}

void check_keys(const sol::table& table, std::initializer_list<const char*> allowed, const std::string& what) {
    for (const auto& [key, value] : table) {
        const bool known = key.get_type() == sol::type::string &&
                           std::ranges::any_of(allowed, [&](const char* k) { return key.as<std::string>() == k; });
        if (!known) throw InvalidDefinition(std::format("{}unknown key {}", what, to_string(table.lua_state(), key)));
    }
}

void check_plain_data(const sol::table& table, const std::string& what) {
    lua_State* L = table.lua_state();
    std::unordered_set<const void*> seen;
    std::vector<sol::table> pending{table}; // not recursive: nesting depth is up to the file
    while (!pending.empty()) {
        const sol::table next = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(next.pointer()).second) continue;
        next.push();
        const int hasMeta = lua_getmetatable(L, -1);
        lua_pop(L, hasMeta ? 2 : 1);
        if (hasMeta) throw InvalidDefinition(what + "tables must be plain data (no metatables)");
        for (const auto& [key, value] : next) // lua_next: raw, runs no metamethods
            for (const sol::object& item : {key, value})
                if (item.get_type() == sol::type::table) pending.push_back(item.as<sol::table>());
    }
}

std::vector<std::filesystem::path> definition_files(const std::filesystem::path& root, const char* dir) {
    return lua_files(root / dir, true);
}

std::vector<std::filesystem::path> lua_files(const std::filesystem::path& folder, bool recursive) {
    FileCache& c = file_cache();
    std::unique_lock lk(c.mu);
    if (const auto it = c.lists.find({folder.string(), recursive}); c.on && it != c.lists.end()) return it->second;
    lk.unlock();
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    const auto add = [&](const std::filesystem::directory_entry& e) {
        if (e.is_regular_file(ec) && e.path().extension() == ".lua") files.push_back(e.path());
    };
    if (std::filesystem::is_directory(folder, ec)) {
        if (recursive)
            for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                add(*it);
        else
            for (auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator();
                 it.increment(ec))
                add(*it);
    }
    std::ranges::sort(files);
    lk.lock();
    if (c.on) c.lists[{folder.string(), recursive}] = files;
    return files;
}

std::string lua_path(const std::filesystem::path& file, const std::filesystem::path& root) {
    FileCache& c = file_cache();
    std::unique_lock lk(c.mu);
    const std::pair key{file.string(), root.string()};
    if (const auto it = c.paths.find(key); c.on && it != c.paths.end()) return it->second;
    lk.unlock();
    std::error_code ec;
    std::string path = std::filesystem::relative(file, root, ec).generic_string();
    lk.lock();
    if (c.on) c.paths[key] = path;
    return path;
}

void cache_lua_files(bool on) {
    FileCache& c = file_cache();
    std::lock_guard lk(c.mu);
    c.on = on;
    if (!on) {
        c.texts.clear();
        c.lists.clear();
        c.paths.clear();
    }
}

sol::table run_definition_file(sol::state& lua, const std::filesystem::path& file, const std::string& path,
                               const std::string& shape) {
    std::string error;
    sol::protected_function body = load_sandboxed(lua, file, "@" + path, sandbox_environment(lua), error);
    if (!body.valid())
        throw InvalidDefinition(error.starts_with("cannot open") ? std::format("{}: {}", path, error) : error);
    sol::protected_function_result result = [&] {
        const ScriptBudget budget(lua.lua_state());
        return body();
    }();
    if (!result.valid()) throw InvalidDefinition(result.get<sol::error>().what()); // "path:line: message"
    if (result.return_count() != 1 || result.get_type() != sol::type::table)
        throw InvalidDefinition(std::format("{}: must return exactly one table {}", path, shape));
    const sol::table table = result;
    check_plain_data(table, path + ": ");
    return table;
}

std::vector<sol::object> sequence(const sol::object& value, const std::string& what) {
    if (value.get_type() != sol::type::table) throw InvalidDefinition(std::format("{} must be a list {{...}}", what));
    const sol::table table = value.as<sol::table>();
    size_t count = 0;
    for (auto it = table.begin(); it != table.end(); ++it) ++count;
    std::vector<sol::object> items;
    for (size_t i = 1; i <= count; ++i) {
        sol::object item = table[i];
        if (item.get_type() == sol::type::lua_nil) throw InvalidDefinition(std::format("{} must be a list {{...}}", what));
        items.push_back(item);
    }
    return items;
}

bool lua_to_field_json(const FieldDesc& field, const sol::object& value, Json& out, std::string& error, bool element) {
    if (!field.array || element) return scalar_to_json(field.kind, field.name, 0, value, out, error);
    size_t count = 0;
    bool isSequence = value.get_type() == sol::type::table;
    if (isSequence) {
        for (const auto& [key, unused] : value.as<sol::table>()) {
            ++count;
            const double k = key.get_type() == sol::type::number ? key.as<double>() : 0.0;
            isSequence = isSequence && k >= 1.0 && k == std::floor(k);
        }
        for (size_t i = 1; isSequence && i <= count; ++i)
            isSequence = value.as<sol::table>()[i].get_type() != sol::type::lua_nil;
    }
    if (!isSequence) {
        error = std::format("\"{}\" must be a sequence table {{v1, v2, ...}}", field.name);
        return false;
    }
    const sol::table table = value.as<sol::table>();
    out = Json::array();
    for (size_t i = 1; i <= count; ++i) {
        Json item;
        if (!scalar_to_json(field.kind, field.name, i, table[i], item, error)) return false;
        out.push_back(std::move(item));
    }
    return true;
}

std::optional<std::vector<CheckedField>> checked_fields(const ComponentDef& def, const sol::table& fields,
                                                       std::string& error) {
    std::vector<CheckedField> out;
    // lua_next by hand: sol2's table iterator takes a registry reference for every key and value.
    lua_State* L = fields.lua_state();
    const int top = lua_gettop(L);
    fields.push();
    lua_pushnil(L);
    bool ok = true;
    while (ok && lua_next(L, top + 1)) {
        const sol::stack_object key(L, -2);
        if (key.get_type() != sol::type::string) {
            error = std::format("\"{}\": field names must be strings, got {}", def.name, to_string(L, sol::object(key)));
            ok = false;
        } else if (const auto desc = std::ranges::find(def.fields, key.as<std::string_view>(), &FieldDesc::name);
                   desc == def.fields.end()) {
            error = std::format("\"{}\": unknown field \"{}\"", def.name, key.as<std::string_view>());
            ok = false;
        } else {
            ok = lua_to_field_json(*desc, sol::object(L, -1), out.emplace_back(&*desc).value, error);
        }
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    if (!ok) return std::nullopt;
    // Shape errors above come in table order; the checks in definition order, as component_from_json reported them.
    std::ranges::sort(out, {}, &CheckedField::field);
    for (CheckedField& f : out) {
        std::optional<Json> valid = field_from_json(*f.field, f.value, error);
        if (!valid) {
            error = std::format("\"{}\": \"{}\" {}", def.name, f.field->name, error);
            return std::nullopt;
        }
        f.value = std::move(*valid);
    }
    return out;
}

std::optional<Json> set_fields(const ComponentDef& def, Json value, const sol::table& fields, std::string& error) {
    std::optional<std::vector<CheckedField>> listed = checked_fields(def, fields, error);
    if (!listed) return std::nullopt;
    for (CheckedField& f : *listed) value[f.field->name] = std::move(f.value);
    std::optional<Json> valid = component_from_json(def, value, error); // the unlisted fields are the caller's
    if (!valid) error = std::format("\"{}\": {}", def.name, error);
    return valid;
}

sol::object field_json_to_lua(sol::state_view lua, const FieldDesc& field, const Json& value, bool element) {
    if (!field.array || element) return scalar_to_lua(lua, field.kind, value);
    sol::table table = lua.create_table(static_cast<int>(value.size()), 0);
    for (size_t i = 0; i < value.size(); ++i) table.raw_set(i + 1, scalar_to_lua(lua, field.kind, value[i]));
    return table;
}

bool lua_to_json(const sol::object& value, nlohmann::json& out, std::string& error) {
    std::vector<const void*> path;
    return payload_to_json(value, "payload", path, out, error);
}

sol::object json_to_lua(sol::state_view lua, const nlohmann::json& value) {
    switch (value.type()) {
    case nlohmann::json::value_t::boolean: return sol::make_object(lua, value.get<bool>());
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
    case nlohmann::json::value_t::number_float: return sol::make_object(lua, value.get<double>());
    case nlohmann::json::value_t::string: return sol::make_object(lua, value.get_ref<const std::string&>());
    case nlohmann::json::value_t::array: {
        sol::table table = lua.create_table(static_cast<int>(value.size()), 0);
        for (size_t i = 0; i < value.size(); ++i) table[i + 1] = json_to_lua(lua, value[i]);
        return table;
    }
    case nlohmann::json::value_t::object: {
        sol::table table = lua.create_table(0, static_cast<int>(value.size()));
        for (const auto& [key, item] : value.items()) table[key] = json_to_lua(lua, item);
        return table;
    }
    default: return sol::make_object(lua, sol::lua_nil); // null (and discarded)
    }
}

} // namespace chthonic::lua
