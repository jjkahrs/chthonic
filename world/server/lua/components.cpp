#include "components.h"
#include "sandbox.h"

#include <entt/core/hashed_string.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <format>
#include <set>

namespace chthonic::lua {
namespace {

using Invalid = InvalidDefinition;

constexpr std::pair<const char*, FieldKind> kKinds[] = {
    {"float", FieldKind::Float}, {"int", FieldKind::Int},   {"bool", FieldKind::Bool}, {"string", FieldKind::String},
    {"vec3", FieldKind::Vec3},   {"quat", FieldKind::Quat}, {"enum", FieldKind::Enum},
};
constexpr const char* kScalarKinds = "float, int, bool, string, vec3, quat, enum";
constexpr std::pair<const char*, Replicate> kReplicateModes[] = {
    {"none", Replicate::none}, {"owner", Replicate::owner}, {"aoi", Replicate::aoi}};

// --- values ---

// A float as the shortest decimal that reads back as the same float so values
// round-trip byte-identically between server, MySQL and client.
double json_number(float value) {
    char buffer[32];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    double result = 0.0;
    std::from_chars(buffer, written.ptr, result);
    return result;
}

Json numbers(std::initializer_list<float> values) {
    Json out = Json::array();
    for (float v : values) out.push_back(json_number(v));
    return out;
}

float finite_float(double value) {
    const auto f = static_cast<float>(value);
    if (!std::isfinite(f)) throw Invalid("must be a finite number within float range");
    return f;
}

template <size_t N>
std::array<float, N> floats(const Json& json) {
    if (!json.is_array() || json.size() != N) throw Invalid(std::format("must be an array of {} numbers", N));
    std::array<float, N> out{};
    for (size_t i = 0; i < N; ++i) {
        if (!json[i].is_number()) throw Invalid(std::format("must be an array of {} numbers", N));
        out[i] = finite_float(json[i].get<double>());
    }
    return out;
}

// Normalized; already-unit values (to float rounding) are kept bit-exact so round trips stay byte-identical.
std::array<float, 4> unit_quaternion(const Json& json) {
    auto q = floats<4>(json);
    const float lengthSquared = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(lengthSquared >= 1e-6f && std::isfinite(lengthSquared))) throw Invalid("must be a nonzero quaternion");
    if (std::abs(lengthSquared - 1.0f) > 1e-5f) {
        const float inv = 1.0f / std::sqrt(lengthSquared);
        for (float& c : q) c *= inv;
    }
    return q;
}

void check_range(const FieldDesc& field, double value) {
    if (field.min && value < *field.min) throw Invalid(std::format("must be >= {}", *field.min));
    if (field.max && value > *field.max) throw Invalid(std::format("must be <= {}", *field.max));
}

Json scalar_from_json(const FieldDesc& field, const Json& json) {
    switch (field.kind) {
    case FieldKind::Float: {
        if (!json.is_number()) throw Invalid("must be a number");
        const float value = finite_float(json.get<double>());
        check_range(field, value);
        return json_number(value);
    }
    case FieldKind::Int: {
        if (!json.is_number_integer()) throw Invalid("must be an integer");
        const bool fits = json.is_number_unsigned() ? json.get<uint64_t>() <= INT_MAX
                                                    : json.get<int64_t>() >= INT_MIN && json.get<int64_t>() <= INT_MAX;
        if (!fits) throw Invalid("must be a 32-bit integer");
        const int value = json.get<int>();
        check_range(field, value);
        return value;
    }
    case FieldKind::Bool:
        if (!json.is_boolean()) throw Invalid("must be true or false");
        return json;
    case FieldKind::String:
        if (!json.is_string()) throw Invalid("must be a string");
        return json;
    case FieldKind::Vec3: {
        const auto v = floats<3>(json);
        return numbers({v[0], v[1], v[2]});
    }
    case FieldKind::Quat: {
        const auto q = unit_quaternion(json);
        return numbers({q[0], q[1], q[2], q[3]});
    }
    case FieldKind::Enum: {
        std::string expected;
        for (const std::string& value : field.enumValues) {
            if (json.is_string() && json.get<std::string>() == value) return json;
            expected += std::format("{}\"{}\"", expected.empty() ? "" : ", ", value);
        }
        throw Invalid(std::format("unknown value {} (expected one of {})", json.dump(), expected));
    }
    }
    throw std::logic_error("unknown field kind");
}

Json field_value(const FieldDesc& field, const Json& json, bool element) {
    if (!field.array || element) return scalar_from_json(field, json);
    if (!json.is_array()) throw Invalid("must be an array");
    Json out = Json::array();
    for (size_t i = 0; i < json.size(); ++i) {
        try {
            out.push_back(scalar_from_json(field, json[i]));
        } catch (const Invalid& e) {
            throw Invalid(std::format("element {} {}", i + 1, e.what()));
        }
    }
    return out;
}

bool known_field(const ComponentDef& def, const std::string& name) {
    return std::ranges::any_of(def.fields, [&](const FieldDesc& f) { return f.name == name; });
}

// --- definition files ---

std::string identifier(const sol::table& table, const char* key, const std::string& what) {
    const sol::object value = table[key];
    if (value.get_type() != sol::type::string || !is_identifier(value.as<std::string>()))
        throw Invalid(std::format("{}\"{}\" must be a name (letters, digits, _; not starting with a digit)", what, key));
    return value.as<std::string>();
}

std::optional<double> bound(const sol::table& table, const char* key, const FieldDesc& field, const std::string& what) {
    const sol::object value = table[key];
    if (value.get_type() == sol::type::lua_nil) return std::nullopt;
    if (field.kind != FieldKind::Float && field.kind != FieldKind::Int)
        throw Invalid(std::format("{}\"{}\" is only allowed on float and int fields", what, key));
    if (value.get_type() != sol::type::number || !std::isfinite(value.as<double>()))
        throw Invalid(std::format("{}\"{}\" must be a finite number", what, key));
    const double number = value.as<double>();
    if (field.kind == FieldKind::Int && number != std::floor(number))
        throw Invalid(std::format("{}\"{}\" must be an integer on an int field", what, key));
    return number;
}

FieldDesc parse_field(lua_State* L, const sol::object& object, size_t index, std::set<std::string>& names) {
    std::string what = std::format("fields[{}]: ", index);
    if (object.get_type() != sol::type::table)
        throw Invalid(std::format("{}must be a table {{name = ..., type = ...}}", what));
    const sol::table table = object.as<sol::table>();
    FieldDesc field;
    field.name = identifier(table, "name", what);
    what = std::format("fields[{}] (\"{}\"): ", index, field.name);
    if (!names.insert(field.name).second) throw Invalid(std::format("{}duplicate field name", what));
    check_keys(table, {"name", "type", "of", "values", "min", "max", "color", "default"}, what);

    const auto kindOf = [&](const char* key) {
        const sol::object type = table[key];
        if (type.get_type() == sol::type::string)
            for (const auto& [name, kind] : kKinds)
                if (type.as<std::string>() == name) return kind;
        const bool of = std::string_view(key) == "of";
        throw Invalid(std::format("{}unknown \"{}\" {} (expected one of {}{})", what, key, to_string(L, type), kScalarKinds,
                                  of ? "; arrays can't nest" : ", array"));
    };
    const sol::object type = table["type"];
    field.array = type.get_type() == sol::type::string && type.as<std::string>() == "array";
    if (field.array) field.kind = kindOf("of");
    else if (sol::object of = table["of"]; of.get_type() != sol::type::lua_nil)
        throw Invalid(std::format("{}\"of\" is only allowed on array fields", what));
    else field.kind = kindOf("type");

    const sol::object values = table["values"];
    if (field.kind == FieldKind::Enum) {
        for (const sol::object& value : sequence(values, what + "\"values\"")) {
            if (value.get_type() != sol::type::string || value.as<std::string>().empty())
                throw Invalid(std::format("{}\"values\" must be non-empty strings", what));
            if (std::ranges::find(field.enumValues, value.as<std::string>()) != field.enumValues.end())
                throw Invalid(std::format("{}duplicate value \"{}\" in \"values\"", what, value.as<std::string>()));
            field.enumValues.push_back(value.as<std::string>());
        }
        if (field.enumValues.empty()) throw Invalid(std::format("{}\"values\" must not be empty", what));
    } else if (values.get_type() != sol::type::lua_nil) {
        throw Invalid(std::format("{}\"values\" is only allowed on enum fields", what));
    }

    field.min = bound(table, "min", field, what);
    field.max = bound(table, "max", field, what);
    if (field.min && field.max && *field.min > *field.max)
        throw Invalid(std::format("{}\"min\" must not be greater than \"max\"", what));

    const sol::object color = table["color"];
    if (color.get_type() != sol::type::lua_nil) {
        if (field.kind != FieldKind::Vec3 || color.get_type() != sol::type::boolean)
            throw Invalid(std::format("{}\"color\" must be true or false, on vec3 fields only", what));
        field.color = color.as<bool>();
    }

    const sol::object value = table["default"];
    Json json = zero_field_value(field);
    std::string error;
    if (value.get_type() != sol::type::lua_nil && !lua_to_field_json(field, value, json, error))
        throw Invalid(std::format("{}default: {}", what, error));
    const std::optional<Json> canonical = field_from_json(field, json, error);
    if (!canonical) throw Invalid(std::format("{}default {} {}", what, json.dump(), error));
    field.defaultValue = *canonical;
    return field;
}

std::shared_ptr<ComponentDef> parse_definition(sol::state& lua, const std::filesystem::path& file,
                                               const std::string& path, std::string& declared) {
    std::string error;
    sol::environment env = sandbox_environment(lua);
    sol::protected_function body = load_sandboxed(lua, file, "@" + path, env, error);
    if (!body.valid()) throw Invalid(error.starts_with("cannot open") ? std::format("{}: {}", path, error) : error);
    sol::protected_function_result result = [&] {
        const ScriptBudget budget(lua.lua_state());
        return body();
    }();
    if (!result.valid()) throw Invalid(result.get<sol::error>().what()); // "path:line: message"
    if (result.return_count() != 1 || result.get_type() != sol::type::table)
        throw Invalid(std::format("{}: must return exactly one table {{name = ..., fields = {{...}}}}", path));
    const sol::table table = result;
    check_plain_data(table, path + ": ");
    auto def = std::make_shared<ComponentDef>();
    def->path = path;
    def->name = identifier(table, "name", path + ": ");
    declared = def->name;
    check_keys(table, {"name", "displayName", "fields", "persist", "replicate"}, path + ": ");
    const sol::object displayName = table["displayName"];
    if (displayName.get_type() == sol::type::lua_nil) def->displayName = def->name;
    else if (displayName.get_type() == sol::type::string && !displayName.as<std::string>().empty())
        def->displayName = displayName.as<std::string>();
    else throw Invalid(std::format("{}: \"displayName\" must be a non-empty string", path));

    const sol::object persist = table["persist"];
    if (persist.get_type() == sol::type::boolean) def->persist = persist.as<bool>();
    else if (persist.get_type() != sol::type::lua_nil) throw Invalid(std::format("{}: \"persist\" must be true or false", path));
    const sol::object replicate = table["replicate"];
    if (replicate.get_type() != sol::type::lua_nil) {
        const auto mode = std::ranges::find_if(kReplicateModes, [&](const auto& m) {
            return replicate.get_type() == sol::type::string && replicate.as<std::string>() == m.first;
        });
        if (mode == std::end(kReplicateModes))
            throw Invalid(std::format("{}: \"replicate\" must be \"none\", \"owner\" or \"aoi\", got {}", path,
                                      to_string(lua.lua_state(), replicate)));
        def->replicate = mode->second;
    }

    std::set<std::string> names;
    const std::vector<sol::object> fields = sequence(table["fields"], path + ": \"fields\"");
    for (size_t i = 0; i < fields.size(); ++i) {
        try {
            def->fields.push_back(parse_field(lua.lua_state(), fields[i], i + 1, names));
        } catch (const Invalid& e) {
            throw Invalid(std::format("{}: {}", path, e.what()));
        }
    }
    return def;
}

} // namespace

bool is_identifier(const std::string& name) {
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    return !name.empty() && alpha(name[0]) &&
           std::ranges::all_of(name, [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}

DefsResult load_component_defs(const std::filesystem::path& root, std::span<const char* const> reserved) {
    DefsResult out;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    const std::filesystem::path dir = root / "components";
    if (std::filesystem::is_directory(dir, ec))
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".lua") files.push_back(it->path());
    std::ranges::sort(files);

    sol::state lua;
    open_sandbox(lua);
    struct Parsed {
        std::string path, name, error;
        std::shared_ptr<ComponentDef> def;
    };
    std::vector<Parsed> parsed;
    for (const auto& file : files) {
        Parsed p;
        p.path = std::filesystem::relative(file, root, ec).generic_string();
        try {
            p.def = parse_definition(lua, file, p.path, p.name);
        } catch (const Invalid& e) {
            p.error = e.what();
        } catch (const std::exception& e) { // sol2 conversions
            p.error = std::format("{}: {}", p.path, e.what());
        }
        parsed.push_back(std::move(p));
    }

    // Name clashes. Lua-vs-Lua rejects every file of a name, so the result doesn't depend on scan order.
    for (size_t i = 0; i < parsed.size(); ++i) {
        Parsed& p = parsed[i];
        if (!p.def) continue;
        if (std::ranges::any_of(reserved, [&](const char* n) { return p.def->name == n; })) {
            p.error = std::format("{}: name \"{}\" is taken by a built-in component", p.path, p.def->name);
            continue;
        }
        std::string others;
        for (size_t j = 0; j < parsed.size(); ++j)
            if (j != i && parsed[j].name == p.def->name)
                others += std::format("{}{}", others.empty() ? "" : ", ", parsed[j].path);
        if (!others.empty()) p.error = std::format("{}: name \"{}\" is also declared in {}", p.path, p.def->name, others);
    }
    for (Parsed& p : parsed) {
        if (p.error.empty()) out.defs.push_back(std::move(p.def));
        else out.errors.push_back(std::move(p.error));
    }
    std::ranges::sort(out.defs, {}, [](const auto& def) { return def->name; });
    return out;
}

Json zero_field_value(const FieldDesc& field, bool element) {
    if (field.array && !element) return Json::array();
    const double lowest = field.min.value_or(-HUGE_VAL);
    const double zero = std::clamp(0.0, lowest, std::max(lowest, field.max.value_or(HUGE_VAL)));
    switch (field.kind) {
    case FieldKind::Float: return json_number(static_cast<float>(zero));
    case FieldKind::Int: return static_cast<int>(zero);
    case FieldKind::Bool: return false;
    case FieldKind::String: return "";
    case FieldKind::Vec3: return numbers({0.0f, 0.0f, 0.0f});
    case FieldKind::Quat: return numbers({0.0f, 0.0f, 0.0f, 1.0f});
    case FieldKind::Enum: return field.enumValues.empty() ? Json("") : Json(field.enumValues.front());
    }
    return nullptr;
}

Json default_component(const ComponentDef& def) {
    Json json = Json::object();
    for (const FieldDesc& field : def.fields) json[field.name] = field.defaultValue;
    return json;
}

std::optional<Json> field_from_json(const FieldDesc& field, const Json& json, std::string& error, bool element) {
    try {
        return field_value(field, json, element);
    } catch (const Invalid& e) {
        error = e.what();
    }
    return std::nullopt;
}

std::optional<Json> component_from_json(const ComponentDef& def, const Json& json, std::string& error) {
    try {
        if (!json.is_object()) throw Invalid("must be an object");
        for (const auto& [key, value] : json.items())
            if (!known_field(def, key)) throw Invalid(std::format("unknown field \"{}\"", key));
        Json out = Json::object();
        for (const FieldDesc& field : def.fields) {
            if (!json.contains(field.name)) throw Invalid(std::format("missing field \"{}\"", field.name));
            try {
                out[field.name] = field_value(field, json[field.name], false);
            } catch (const Invalid& e) {
                throw Invalid(std::format("\"{}\" {}", field.name, e.what()));
            }
        }
        return out;
    } catch (const Invalid& e) {
        error = e.what();
    }
    return std::nullopt;
}

Json migrate_component(const ComponentDef& def, const Json& json, std::vector<std::string>& fixes) {
    if (!json.is_object()) {
        fixes.push_back(std::format("{} is not an object; reset to defaults", json.dump()));
        return default_component(def);
    }
    for (const auto& [key, value] : json.items())
        if (!known_field(def, key)) fixes.push_back(std::format("unknown field \"{}\" dropped", key));
    Json out = Json::object();
    for (const FieldDesc& field : def.fields) {
        if (!json.contains(field.name)) {
            fixes.push_back(std::format("missing field \"{}\" set to default {}", field.name, field.defaultValue.dump()));
            out[field.name] = field.defaultValue;
            continue;
        }
        try {
            out[field.name] = field_value(field, json[field.name], false);
        } catch (const Invalid& e) {
            fixes.push_back(std::format("\"{}\" {} {}; reset to default {}", field.name, json[field.name].dump(), e.what(),
                                        field.defaultValue.dump()));
            out[field.name] = field.defaultValue;
        }
    }
    return out;
}

entt::id_type storage_id(std::string_view name) {
    std::string key = "lua/";
    key += name;
    return entt::hashed_string::value(key.c_str(), key.size());
}

void restore_components(entt::registry& reg, entt::entity e, const Defs& defs, const std::string& column,
                        const std::function<void(const std::string&)>& warn) {
    auto& preserved = reg.emplace_or_replace<PreservedComponents>(e);
    if (column.empty()) return;
    Json stored = Json::parse(column, nullptr, false);
    if (!stored.is_object()) {
        preserved.unreadable = true; // never overwritten: someone has to look at it
        warn("components column is not a JSON object; it is left untouched and nothing is restored");
        return;
    }
    for (auto& [name, value] : stored.items()) {
        const auto def = std::ranges::find_if(defs, [&](const auto& d) { return d->name == name && d->persist; });
        if (def == defs.end()) {
            preserved.byName[name] = std::move(value); // kept verbatim, written back on every save
            continue;
        }
        std::vector<std::string> fixes;
        pool(reg, name).emplace(e, ComponentData{migrate_component(**def, value, fixes)});
        for (const std::string& fix : fixes) warn(std::format("{}: {}", name, fix));
    }
}

std::optional<std::string> components_column(entt::registry& reg, entt::entity e, const Defs& defs) {
    const auto* preserved = reg.try_get<PreservedComponents>(e);
    if (preserved && preserved->unreadable) return std::nullopt;
    Json out = preserved ? preserved->byName : Json::object();
    for (const auto& def : defs) {
        if (!def->persist) continue;
        auto& p = pool(reg, def->name);
        if (p.contains(e)) out[def->name] = p.get(e).value;
    }
    return out.dump();
}

} // namespace chthonic::lua
