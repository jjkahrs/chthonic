#include "lua_state_machines.h"
#include "sandbox.h"

#include <algorithm>
#include <format>
#include <optional>

namespace chthonic::lua {
namespace {

using Invalid = InvalidDefinition;

std::string text(const sol::table& table, const char* key, const std::string& what) {
    const sol::object value = table[key];
    if (value.get_type() != sol::type::string || value.as<std::string>().empty())
        throw Invalid(std::format("{}\"{}\" must be a non-empty string", what, key));
    return value.as<std::string>();
}

StateCallback callback(const sol::table& state, const char* key, const std::string& stateName, const std::string& path,
                       const std::string& what, const CallbackBinder& bind) {
    const sol::object fn = state[key];
    if (fn.get_type() == sol::type::lua_nil) return nullptr;
    if (fn.get_type() != sol::type::function) throw Invalid(std::format("{}\"{}\" must be a function", what, key));
    return bind(fn.as<sol::protected_function>(), path, std::format("{} \"{}\"", key, stateName));
}

StateMachineDef parse_definition(sol::state& lua, const std::filesystem::path& file, const std::string& path,
                                 const sol::environment& env, const CallbackBinder& bind, std::string& declared) {
    std::string error;
    sol::protected_function body = load_sandboxed(lua, file, "@" + path, env, error);
    if (!body.valid()) throw Invalid(error.starts_with("cannot open") ? std::format("{}: {}", path, error) : error);
    sol::protected_function_result result = [&] {
        const ScriptBudget budget(lua.lua_state());
        return body();
    }();
    if (!result.valid()) throw Invalid(result.get<sol::error>().what()); // "path:line: message"
    if (result.return_count() != 1 || result.get_type() != sol::type::table)
        throw Invalid(std::format("{}: must return exactly one table {{name = ..., initial = ..., states = {{...}}}}", path));
    const sol::table table = result;
    const std::string prefix = path + ": ";
    check_plain_data(table, prefix);
    StateMachineDef def;
    def.path = path;
    def.name = text(table, "name", prefix);
    declared = def.name;
    check_keys(table, {"name", "initial", "states"}, prefix);

    const sol::object states = table["states"];
    if (states.get_type() != sol::type::table)
        throw Invalid(std::format("{}\"states\" must be a table {{name = {{...}}, ...}}", prefix));
    std::vector<std::pair<std::string, sol::table>> entries; // sorted by name, so the first error doesn't vary
    for (const auto& [key, value] : states.as<sol::table>()) {
        if (key.get_type() != sol::type::string || key.as<std::string>().empty() || key.as<std::string>() == kAnyState)
            throw Invalid(std::format("{}state names must be non-empty strings other than \"*\", got {}", prefix,
                                      to_string(lua.lua_state(), key)));
        if (value.get_type() != sol::type::table)
            throw Invalid(std::format("{}state \"{}\" must be a table", prefix, key.as<std::string>()));
        entries.emplace_back(key.as<std::string>(), value.as<sol::table>());
    }
    if (entries.empty()) throw Invalid(std::format("{}\"states\" must not be empty", prefix));
    std::ranges::sort(entries, {}, &std::pair<std::string, sol::table>::first);
    for (const auto& [name, state] : entries) def.states[name];
    for (const auto& [name, state] : entries) {
        const std::string what = std::format("{}state \"{}\": ", prefix, name);
        check_keys(state, {"allowedFrom", "enter", "exit"}, what);
        StateDef& rules = def.states[name];
        if (const sol::object from = state["allowedFrom"]; from.get_type() != sol::type::lua_nil) {
            rules.fromAny = false;
            for (const sol::object& item : sequence(from, what + "\"allowedFrom\"")) {
                if (item.get_type() != sol::type::string)
                    throw Invalid(std::format("{}\"allowedFrom\" must hold state names, got {}", what,
                                              to_string(lua.lua_state(), item)));
                const std::string source = item.as<std::string>();
                if (source == kAnyState) rules.fromAny = true;
                else if (!def.states.contains(source))
                    throw Invalid(std::format("{}\"allowedFrom\" names unknown state \"{}\"", what, source));
                else rules.from.insert(source);
            }
        }
        rules.enter = callback(state, "enter", name, path, what, bind);
        rules.exit = callback(state, "exit", name, path, what, bind);
    }

    const sol::object initial = table["initial"];
    if (initial.get_type() != sol::type::string || !def.states.contains(initial.as<std::string>()))
        throw Invalid(std::format("{}\"initial\" must name one of its states, got {}", prefix,
                                  to_string(lua.lua_state(), initial)));
    def.initial = initial.as<std::string>();
    return def;
}

} // namespace

StateMachineDefinitions parse_state_machine_definitions(sol::state& lua, const std::filesystem::path& root,
                                                        const std::function<sol::environment()>& environment,
                                                        const CallbackBinder& bind) {
    const std::vector<std::filesystem::path> files = lua_files(root / kStateMachinesDir, true);

    struct Parsed {
        std::string path, name, error; // name: as declared, even if the file was rejected later
        std::optional<StateMachineDef> def;
    };
    std::vector<Parsed> parsed;
    for (const auto& file : files) {
        Parsed p;
        p.path = lua_path(file, root);
        try {
            p.def = parse_definition(lua, file, p.path, environment(), bind, p.name);
        } catch (const Invalid& e) {
            p.error = e.what();
        } catch (const std::exception& e) { // sol2 conversions
            p.error = std::format("{}: {}", p.path, e.what());
        }
        parsed.push_back(std::move(p));
    }
    // Name clashes reject every file of a name, so the result doesn't depend on scan order.
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (!parsed[i].def) continue;
        std::string others;
        for (size_t j = 0; j < parsed.size(); ++j)
            if (j != i && parsed[j].name == parsed[i].def->name)
                others += std::format("{}{}", others.empty() ? "" : ", ", parsed[j].path);
        if (!others.empty())
            parsed[i].error = std::format("{}: name \"{}\" is also declared in {}", parsed[i].path, parsed[i].def->name, others);
    }
    StateMachineDefinitions out;
    for (Parsed& p : parsed) {
        if (p.error.empty()) out.valid.push_back(std::move(*p.def));
        else out.errors.push_back(std::move(p.error));
    }
    return out;
}

} // namespace chthonic::lua
