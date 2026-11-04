#include "npcs.h"
#include "sandbox.h"
#include "state_machines.h"

#include <algorithm>
#include <format>

namespace chthonic::lua {
namespace {

using Invalid = InvalidDefinition;

constexpr size_t kMaxNameBytes = 255; // name and template, as chthonic.create allows
constexpr const char* kReadOnly[] = {"Character", "Npc", "Spawn", "Transform"};
constexpr int64_t kMaxLootCount = 1000000; // gold or items one loot entry may drop

std::string text(const sol::table& table, const char* key, const std::string& what) {
    const sol::object value = table[key];
    if (value.get_type() != sol::type::string || value.as<std::string>().empty() ||
        value.as<std::string>().size() > kMaxNameBytes)
        throw Invalid(std::format("{}\"{}\" must be a non-empty string of at most {} bytes", what, key, kMaxNameBytes));
    return value.as<std::string>();
}

// An integer in [lo, hi] at json[key] of a loot part; what names that part.
int64_t loot_count(const Json& json, const char* key, int64_t lo, int64_t hi, const std::string& what) {
    if (!json.contains(key) || !json[key].is_number_integer() || json[key].get<int64_t>() < lo || json[key].get<int64_t>() > hi)
        throw Invalid(std::format("{}\"{}\" must be an integer in [{}, {}]", what, key, lo, hi));
    return json[key].get<int64_t>();
}

void loot_keys(const Json& json, std::initializer_list<const char*> allowed, const std::string& what) {
    if (!json.is_object()) throw Invalid(what + "must be a table");
    for (const auto& [key, value] : json.items())
        if (std::ranges::none_of(allowed, [&](const char* a) { return key == a; }))
            throw Invalid(std::format("{}unknown key {}", what, key));
}

// loot = {gold = {min =, max =}, items = {{item =, chance =, min =, max =}, ...}}, both parts optional: null if nil.
Json parse_loot(const sol::object& value, const ItemDefs& items, const std::string& prefix) {
    if (value.get_type() == sol::type::lua_nil) return nullptr;
    nlohmann::json plain;
    std::string error;
    if (!lua_to_json(value, plain, error)) throw Invalid(prefix + "\"loot\": " + error);
    const Json loot = plain;
    const std::string what = prefix + "\"loot\": ";
    loot_keys(loot, {"gold", "items"}, what);
    if (loot.contains("gold")) {
        const std::string at = what + "\"gold\": ";
        loot_keys(loot["gold"], {"min", "max"}, at);
        const int64_t min = loot_count(loot["gold"], "min", 0, kMaxLootCount, at);
        loot_count(loot["gold"], "max", min, kMaxLootCount, at);
    }
    if (loot.contains("items")) {
        if (!loot["items"].is_array()) throw Invalid(what + "\"items\" must be a sequence {{item = ..., chance = ..., min = ..., max = ...}, ...}");
        for (size_t i = 0; i < loot["items"].size(); ++i) {
            const Json& entry = loot["items"][i];
            const std::string at = std::format("{}\"items\"[{}]: ", what, i + 1);
            loot_keys(entry, {"item", "chance", "min", "max"}, at);
            if (!entry.contains("item") || !entry["item"].is_string() || !items.contains(entry["item"].get<std::string>()))
                throw Invalid(at + "\"item\" must name an item definition in " + std::string(kItemsDir) + "/");
            if (!entry.contains("chance") || !entry["chance"].is_number() || entry["chance"].get<double>() <= 0 ||
                entry["chance"].get<double>() > 1)
                throw Invalid(at + "\"chance\" must be a number in (0, 1]");
            const int64_t min = loot_count(entry, "min", 1, kMaxLootCount, at);
            loot_count(entry, "max", min, kMaxLootCount, at);
        }
    }
    return loot;
}

NpcDef parse_definition(sol::state& lua, const std::filesystem::path& file, const std::string& path, const Defs& defs,
                        const MachineExists& machine, const ItemDefs& items, std::string& declared) {
    const sol::table table =
        run_definition_file(lua, file, path, "{name = ..., template = ..., components = {...}, loot = {...}}");
    const std::string prefix = path + ": ";
    std::string error;
    NpcDef def;
    def.name = text(table, "name", prefix);
    declared = def.name;
    check_keys(table, {"name", "template", "components", "loot"}, prefix);
    def.templateName = text(table, "template", prefix);
    std::optional<Json> components = merge_components(table["components"], Json::object(), defs, machine, error);
    if (!components) throw Invalid(prefix + error);
    def.components = std::move(*components);
    def.loot = parse_loot(table["loot"], items, prefix);
    return def;
}

} // namespace

std::optional<Json> merge_components(const sol::object& components, Json base, const Defs& defs,
                                     const MachineExists& machine, std::string& error) {
    if (components.get_type() == sol::type::lua_nil) return base;
    lua_State* L = components.lua_state();
    if (components.get_type() != sol::type::table) {
        error = std::format("\"components\" must be a table {{Type = {{field = value, ...}}, ...}}, got {}",
                            to_string(L, components));
        return std::nullopt;
    }
    std::vector<std::pair<std::string, sol::object>> entries; // sorted by name, so the first error doesn't vary
    for (const auto& [key, fields] : components.as<sol::table>()) {
        if (key.get_type() != sol::type::string) {
            error = std::format("\"components\": component names must be strings, got {}", to_string(L, key));
            return std::nullopt;
        }
        entries.emplace_back(key.as<std::string>(), fields);
    }
    std::ranges::sort(entries, {}, &std::pair<std::string, sol::object>::first);
    for (const auto& [name, fields] : entries) {
        if (std::ranges::any_of(kReadOnly, [&](const char* n) { return name == n; })) {
            error = std::format("\"{}\" is read-only on the server", name);
            return std::nullopt;
        }
        const auto def = std::ranges::find_if(defs, [&](const auto& d) { return d->name == name; });
        if (def == defs.end()) {
            error = std::format("unknown component \"{}\"", name);
            return std::nullopt;
        }
        if (fields.get_type() != sol::type::table) {
            error = std::format("\"{}\" must be a table {{field = value, ...}}, got {}", name, to_string(L, fields));
            return std::nullopt;
        }
        std::optional<Json> value = set_fields(**def, base.contains(name) ? base[name] : default_component(**def),
                                               fields.as<sol::table>(), error);
        if (!value) return std::nullopt;
        if (name == kStateMachineComponent && !machine((*value)["machine"].get<std::string>())) {
            error = std::format("\"{}\": \"machine\" {} names no valid state machine definition in {}/", name,
                                (*value)["machine"].dump(), kStateMachinesDir);
            return std::nullopt;
        }
        base[name] = std::move(*value);
    }
    return base;
}

NpcDefsResult load_npc_defs(sol::state& lua, const std::filesystem::path& root, const Defs& defs,
                            const MachineExists& machine, const ItemDefs& items) {
    const std::vector<std::filesystem::path> files = definition_files(root, kNpcsDir);

    struct Parsed {
        std::string path, name, error; // name: as declared, even if the file was rejected later
        std::optional<NpcDef> def;
    };
    std::vector<Parsed> parsed;
    for (const auto& file : files) {
        Parsed p;
        p.path = lua_path(file, root);
        try {
            p.def = parse_definition(lua, file, p.path, defs, machine, items, p.name);
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
    NpcDefsResult out;
    for (Parsed& p : parsed) {
        if (p.error.empty()) out.defs.emplace(p.def->name, std::move(*p.def));
        else out.errors.push_back(std::move(p.error));
    }
    return out;
}

} // namespace chthonic::lua
