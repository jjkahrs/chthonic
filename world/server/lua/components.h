// Lua components: data-only component types defined by Lua files under LUA_ROOT/components/ (scanned recursively), one
// type per file. The file format is the client's plus two server keys, persist and replicate, so a game client's
// components/ folder can be shared with the server (docs/world/lua.html).
// Instances live in EnTT named storage (one pool per type, holding the canonical JSON).
// Keep error messages and drift rules identical to the client's.
#pragma once
#include <entt/entity/registry.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace chthonic::lua {

using Json = nlohmann::ordered_json;

enum class FieldKind { Float, Int, Bool, String, Vec3, Quat, Enum };

struct FieldDesc {
    std::string name; // JSON key
    FieldKind kind = FieldKind::Float;
    bool array = false;
    bool color = false;
    std::optional<double> min, max; // Float / Int, per element for arrays
    std::vector<std::string> enumValues;
    Json defaultValue; // canonical JSON
};

// Who gets the component's value (see systems.h component_replication_system).
enum class Replicate { none, owner, aoi };

struct ComponentDef {
    std::string name;
    std::string displayName;
    std::string path; // LUA_ROOT-relative definition file
    std::vector<FieldDesc> fields;
    bool persist = false;
    Replicate replicate = Replicate::none;
};
using Defs = std::vector<std::shared_ptr<const ComponentDef>>; // sorted by name

// A valid type or field name: letters, digits, _; not starting with a digit.
bool is_identifier(const std::string& name);

struct DefsResult {
    Defs defs;                       // the valid ones
    std::vector<std::string> errors; // one per rejected file, each starting with its path
};
// Loads every definition under root/components/ (none if the folder is missing). Each file runs in its own sandboxed
// environment. reserved: names no definition may take (the server's built-in components).
DefsResult load_component_defs(const std::filesystem::path& root, std::span<const char* const> reserved);

// --- values (canonical JSON) ---

// The zero value of a field moved into min/max; for an array field (unless element) an empty array.
Json zero_field_value(const FieldDesc& field, bool element = false);
Json default_component(const ComponentDef& def);
// One field value checked strictly; canonical JSON, or nullopt + error ("must be ...").
std::optional<Json> field_from_json(const FieldDesc& field, const Json& json, std::string& error, bool element = false);
// Strict: unknown, missing and invalid fields are errors.
std::optional<Json> component_from_json(const ComponentDef& def, const Json& json, std::string& error);
// Lenient (schema drift): missing fields get defaults, unknown fields are dropped, invalid values are reset to the
// field default. Appends one description per change to fixes.
Json migrate_component(const ComponentDef& def, const Json& json, std::vector<std::string>& fixes);

// --- storage ---

struct ComponentData {
    Json value;
};
entt::id_type storage_id(std::string_view name);
inline auto& pool(entt::registry& reg, std::string_view name) { return reg.storage<ComponentData>(storage_id(name)); }

// --- persistence (the characters.components column: {"<Name>": {...}, ...}) ---

// Column entries that are not restored (no definition, or not persist), written back unchanged on every save.
struct PreservedComponents {
    Json byName = Json::object();
    bool unreadable = false; // the column isn't a JSON object: never overwrite it
};

// Restores e's persisted components from the column text (empty: none) with the drift rules; every fix and an
// unreadable column go to warn. Adds PreservedComponents.
void restore_components(entt::registry& reg, entt::entity e, const Defs& defs, const std::string& column,
                        const std::function<void(const std::string&)>& warn);
// The column to save for e: its persist components plus the preserved entries. nullopt: leave the column untouched.
std::optional<std::string> components_column(entt::registry& reg, entt::entity e, const Defs& defs);

} // namespace chthonic::lua
