#include "objects.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace chthonic::map {
namespace {

using Json = nlohmann::json;

bool finite_number(const Json& v) { return v.is_number() && std::isfinite(v.get<double>()); }
bool positive(const Json& v) { return finite_number(v) && v.get<double>() > 0; }
bool numbers(const Json& v, size_t n) { return v.is_array() && v.size() == n && std::all_of(v.begin(), v.end(), finite_number); }

constexpr size_t kMaxEventBytes = 255;
constexpr int kMaxDataDepth = 31; // the event payload, data inside it, at most 32 deep as the bus allows

int depth(const Json& v) {
    int deepest = 0;
    if (v.is_structured())
        for (const Json& child : v) deepest = std::max(deepest, depth(child));
    return v.is_structured() ? deepest + 1 : 0;
}

// at: "objects[2]: " or "interactable 7: ". Fails with err set.
std::optional<Shape> parse_shape(const Json& s, const std::string& at, std::string& err) {
    const auto fail = [&](const char* message) -> std::optional<Shape> { err = at + message; return std::nullopt; };
    if (!s.is_object() || s.size() != 1 || (!s.contains("box") && !s.contains("cylinder")))
        return fail("\"shape\" must be {\"box\": {\"halfExtents\": [x, y, z]}} or {\"cylinder\": {\"radius\": r, \"height\": h}}");
    Shape shape;
    if (s.contains("box")) {
        const Json& h = s["box"].is_object() ? s["box"].value("halfExtents", Json()) : Json();
        if (!numbers(h, 3) || !std::all_of(h.begin(), h.end(), positive))
            return fail("\"shape.box.halfExtents\" must be [x, y, z], each > 0");
        shape.halfExtents = {h[0].get<float>(), h[1].get<float>(), h[2].get<float>()};
    } else {
        const Json& c = s["cylinder"];
        if (!c.is_object() || !positive(c.value("radius", Json())) || !positive(c.value("height", Json())))
            return fail("\"shape.cylinder\" must be {\"radius\": r, \"height\": h}, each > 0");
        shape.kind = Shape::Kind::Cylinder;
        shape.radius = c["radius"].get<float>();
        shape.height = c["height"].get<float>();
    }
    return shape;
}

bool parse_pos(const Json& e, TileKey tile, const std::string& at, std::array<float, 3>& pos, std::string& err) {
    if (!numbers(e.value("pos", Json()), 3)) {
        err = at + "\"pos\" must be [x, y, z]";
        return false;
    }
    pos = {e["pos"][0].get<float>(), e["pos"][1].get<float>(), e["pos"][2].get<float>()};
    if (tile_of(pos[0], pos[2]) != tile) {
        err = at + "\"pos\" is outside tile " + std::to_string(tile.x) + "_" + std::to_string(tile.z);
        return false;
    }
    return true;
}

bool parse_yaw(const Json& e, const std::string& at, float& yaw, std::string& err) {
    if (!e.contains("yaw")) return true;
    if (!finite_number(e["yaw"])) {
        err = at + "\"yaw\" must be a number (radians)";
        return false;
    }
    yaw = e["yaw"].get<float>();
    return true;
}

bool parse_id(const Json& e, const std::string& at, uint32_t& id, std::string& err) {
    const Json& v = e.value("id", Json());
    if (!v.is_number_unsigned() || v.get<uint64_t>() == 0 || v.get<uint64_t>() > 0xFFFFFFFFull) {
        err = at + "\"id\" must be an integer in [1, 4294967295]";
        return false;
    }
    id = static_cast<uint32_t>(v.get<uint64_t>());
    return true;
}

std::optional<ZoneObject> parse_object(const Json& e, TileKey tile, const std::string& at, std::string& err) {
    if (!e.is_object()) {
        err = at + "must be an object";
        return std::nullopt;
    }
    ZoneObject o;
    const Json& type = e.value("type", Json());
    if (type == "visual") o.type = ObjectType::Visual;
    else if (type == "obstacle") o.type = ObjectType::Obstacle;
    else if (type == "interactable") o.type = ObjectType::Interactable;
    else {
        err = at + "\"type\" must be \"visual\", \"obstacle\" or \"interactable\"";
        return std::nullopt;
    }
    Json prefab = e.value("prefab", Json());
    if (prefab.is_object()) prefab = prefab.value("path", Json()); // an editor's asset ref {"id", "path"}
    if (!prefab.is_string() || !prefab.get<std::string>().ends_with(".prefab")) {
        err = at + "\"prefab\" must be a project-relative .prefab path";
        return std::nullopt;
    }
    o.prefab = prefab.get<std::string>();
    if (!parse_pos(e, tile, at, o.pos, err) || !parse_yaw(e, at, o.yaw, err)) return std::nullopt;
    if (e.contains("scale")) {
        if (!positive(e["scale"])) {
            err = at + "\"scale\" must be a number > 0";
            return std::nullopt;
        }
        o.scale = e["scale"].get<float>();
    }
    if (o.type == ObjectType::Visual) return o;
    if (!e.contains("shape")) {
        err = at + "a blocking object needs \"shape\"";
        return std::nullopt;
    }
    o.shape = parse_shape(e["shape"], at, err);
    if (!o.shape) return std::nullopt;
    if (o.type == ObjectType::Interactable) {
        if (!parse_id(e, at, o.id, err)) return std::nullopt;
        if (e.contains("name")) {
            if (!e["name"].is_string()) {
                err = at + "\"name\" must be a string";
                return std::nullopt;
            }
            o.name = e["name"].get<std::string>();
        }
    }
    return o;
}

} // namespace

const char* object_type_name(ObjectType type) {
    switch (type) {
    case ObjectType::Visual: return "visual";
    case ObjectType::Obstacle: return "obstacle";
    case ObjectType::Interactable: return "interactable";
    }
    return "visual";
}

ParsedObjects parse_objects(const Json& j, TileKey tile) {
    ParsedObjects out;
    if (!j.is_object()) return {{}, {"must be a JSON object"}};
    if (!j.contains("version") || j["version"] != 1) return {{}, {"\"version\" must be 1"}};
    const Json& list = j.value("objects", Json());
    if (!list.is_array()) return {{}, {"\"objects\" must be a list"}};
    std::set<uint32_t> ids;
    for (size_t i = 0; i < list.size(); ++i) {
        const std::string at = "objects[" + std::to_string(i) + "]: ";
        std::string err;
        auto object = parse_object(list[i], tile, at, err);
        if (object && object->type == ObjectType::Interactable && !ids.insert(object->id).second) {
            err = at + "duplicate id " + std::to_string(object->id);
            object.reset();
        }
        if (object) out.objects.push_back(std::move(*object));
        else out.errors.push_back(std::move(err));
    }
    return out;
}

std::optional<std::vector<Interactable>> parse_interactables(const Json& j, TileKey tile, std::string& err) {
    const auto fail = [&](std::string message) -> std::optional<std::vector<Interactable>> { err = std::move(message); return std::nullopt; };
    if (!j.is_object()) return fail("must be a JSON object");
    if (!j.contains("version") || j["version"] != 1) return fail("\"version\" must be 1");
    const Json& list = j.value("interactables", Json());
    if (!list.is_array()) return fail("\"interactables\" must be a list");
    std::vector<Interactable> out;
    std::set<uint32_t> ids;
    for (size_t i = 0; i < list.size(); ++i) {
        const Json& e = list[i];
        std::string at = "interactables[" + std::to_string(i) + "]: ";
        if (!e.is_object()) return fail(at + "must be an object");
        Interactable it;
        if (!parse_id(e, at, it.id, err)) return std::nullopt;
        at = "interactable " + std::to_string(it.id) + ": ";
        if (!ids.insert(it.id).second) return fail(at + "duplicate id");
        if (!parse_pos(e, tile, at, it.pos, err) || !parse_yaw(e, at, it.yaw, err)) return std::nullopt;
        const auto shape = parse_shape(e.value("shape", Json()), at, err);
        if (!shape) return std::nullopt;
        it.shape = *shape;
        if (e.contains("range")) {
            if (!positive(e["range"])) return fail(at + "\"range\" must be a number > 0 (metres)");
            it.range = e["range"].get<float>();
        }
        const Json& event = e.value("event", Json());
        if (!event.is_string() || event.get<std::string>().empty() || event.get<std::string>().size() > kMaxEventBytes)
            return fail(at + "\"event\" must be a non-empty string of at most 255 bytes");
        it.event = event.get<std::string>();
        if (e.contains("data")) {
            if (!e["data"].is_object()) return fail(at + "\"data\" must be an object");
            if (depth(e["data"]) > kMaxDataDepth) return fail(at + "\"data\" is nested more than 31 deep");
            it.data = e["data"];
        }
        if (e.contains("name")) {
            if (!e["name"].is_string()) return fail(at + "\"name\" must be a string");
            it.name = e["name"].get<std::string>();
        }
        out.push_back(std::move(it));
    }
    return out;
}

float footprint_distance(const Shape& shape, const std::array<float, 3>& pos, float yaw, float x, float z) {
    const float dx = x - pos[0], dz = z - pos[2];
    if (shape.kind == Shape::Kind::Cylinder) return std::max(std::hypot(dx, dz) - shape.radius, 0.0f);
    // Into the box's frame: the inverse of the yaw turn footprint() applies.
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float lx = dx * c - dz * s, lz = dx * s + dz * c;
    return std::hypot(std::max(std::fabs(lx) - shape.halfExtents[0], 0.0f), std::max(std::fabs(lz) - shape.halfExtents[2], 0.0f));
}

std::vector<std::array<float, 2>> footprint(const Shape& shape, const std::array<float, 3>& pos, float yaw, int segments) {
    std::vector<std::array<float, 2>> out;
    if (shape.kind == Shape::Kind::Cylinder) {
        for (int i = 0; i < segments; ++i) {
            const float a = 2 * std::numbers::pi_v<float> * float(i) / float(segments);
            out.push_back({pos[0] + shape.radius * std::cos(a), pos[2] + shape.radius * std::sin(a)});
        }
        return out;
    }
    // Local +z turns to (sin yaw, cos yaw), as heading; local +x to (cos yaw, -sin yaw).
    const float c = std::cos(yaw), s = std::sin(yaw), hx = shape.halfExtents[0], hz = shape.halfExtents[2];
    for (const auto& [sx, sz] : {std::pair{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}) {
        const float lx = float(sx) * hx, lz = float(sz) * hz;
        out.push_back({pos[0] + lx * c + lz * s, pos[2] - lx * s + lz * c});
    }
    return out;
}

} // namespace chthonic::map
