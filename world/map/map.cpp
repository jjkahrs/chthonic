#include "map.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace chthonic::map {
namespace {

using Json = nlohmann::json;

bool finite_number(const Json& v) { return v.is_number() && std::isfinite(v.get<double>()); }

// A JSON array of n finite numbers.
bool numbers(const Json& v, size_t n) {
    return v.is_array() && v.size() == n && std::all_of(v.begin(), v.end(), finite_number);
}

bool int32_value(const Json& v) {
    return v.is_number_integer() && v.get<int64_t>() >= std::numeric_limits<int32_t>::min() &&
           v.get<int64_t>() <= std::numeric_limits<int32_t>::max();
}

using Point = std::array<float, 2>;

// Ray cast along +x; a point exactly on an edge may land either side.
bool inside(const std::vector<Point>& poly, float x, float z) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const auto [xi, zi] = poly[i];
        const auto [xj, zj] = poly[j];
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
    }
    return in;
}

float cross(Point o, Point a, Point b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); }

float segment_distance(Point p, Point a, Point b) {
    const float dx = b[0] - a[0], dz = b[1] - a[1], len2 = dx * dx + dz * dz;
    const float t = len2 == 0 ? 0 : std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / len2, 0.f, 1.f);
    return std::hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dz);
}

// Inside and not on (within 1 mm of) an edge, so areas that only share a border do not overlap.
bool strictly_inside(const std::vector<Point>& poly, Point p) {
    if (!inside(poly, p[0], p[1])) return false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if (segment_distance(p, poly[j], poly[i]) < 1e-3f) return false;
    return true;
}

Point centroid(const std::vector<Point>& poly) {
    Point c{};
    for (const Point& p : poly) {
        c[0] += p[0] / poly.size();
        c[1] += p[1] / poly.size();
    }
    return c;
}

// Whether two areas share any volume: their y ranges overlap, and an edge of one properly crosses an edge of the
// other, or a vertex or the vertex centroid of one is strictly inside the other.
// ponytail: misses only non-convex polygons with exactly coincident edges and outside centroids; a real polygon
// clipper if hand-made areas ever hit that.
bool overlap(const Area& a, const Area& b) {
    if (a.bounds[0] >= b.bounds[2] || b.bounds[0] >= a.bounds[2] || a.bounds[1] >= b.bounds[3] || b.bounds[1] >= a.bounds[3])
        return false;
    if (a.y && b.y && ((*a.y)[0] >= (*b.y)[1] || (*b.y)[0] >= (*a.y)[1])) return false;
    const auto& pa = a.polygon;
    const auto& pb = b.polygon;
    const auto opposite = [](float u, float v) { return (u > 0 && v < 0) || (u < 0 && v > 0); };
    for (size_t i = 0, j = pa.size() - 1; i < pa.size(); j = i++)
        for (size_t k = 0, l = pb.size() - 1; k < pb.size(); l = k++)
            if (opposite(cross(pa[j], pa[i], pb[l]), cross(pa[j], pa[i], pb[k])) &&
                opposite(cross(pb[l], pb[k], pa[j]), cross(pb[l], pb[k], pa[i])))
                return true;
    for (const Point& p : pa) if (strictly_inside(pb, p)) return true;
    for (const Point& p : pb) if (strictly_inside(pa, p)) return true;
    return strictly_inside(pb, centroid(pa)) || strictly_inside(pa, centroid(pb));
}

// One areas.json entry, without parent or overlap checks.
std::optional<Area> parse_area(const Json& a, const std::string& where, std::string& err) {
    const auto fail = [&](std::string message) -> std::optional<Area> { err = std::move(message); return std::nullopt; };
    if (!a.is_object()) return fail(where + ": must be an object");
    Area area;
    if (!a.value("id", Json()).is_string() || a["id"].get<std::string>().empty())
        return fail(where + ": \"id\" must be a non-empty string");
    area.id = a["id"].get<std::string>();
    const std::string at = "area \"" + area.id + "\"";
    for (auto [key, out] : {std::pair{"name", &area.name}, {"parent", &area.parent}, {"music", &area.music}}) {
        if (!a.contains(key)) continue;
        if (!a[key].is_string()) return fail(at + ": \"" + key + "\" must be a string");
        *out = a[key].get<std::string>();
    }
    if (area.name.empty()) area.name = area.id;
    if (a.contains("priority")) {
        if (!int32_value(a["priority"])) return fail(at + ": \"priority\" must be an integer");
        area.priority = a["priority"].get<int>();
    }
    if (a.contains("rect") == a.contains("polygon")) return fail(at + ": needs exactly one of \"rect\" and \"polygon\"");
    if (a.contains("rect")) {
        const Json& r = a["rect"];
        if (!numbers(r, 4) || r[0] >= r[2] || r[1] >= r[3])
            return fail(at + ": \"rect\" must be [x0, z0, x1, z1] with x0 < x1 and z0 < z1");
        const float x0 = r[0].get<float>(), z0 = r[1].get<float>(), x1 = r[2].get<float>(), z1 = r[3].get<float>();
        area.polygon = {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}};
    } else {
        const Json& p = a["polygon"];
        if (!p.is_array() || p.size() < 3 || !std::all_of(p.begin(), p.end(), [](const Json& v) { return numbers(v, 2); }))
            return fail(at + ": \"polygon\" must be a list of at least 3 [x, z] points");
        for (const Json& v : p) area.polygon.push_back({v[0].get<float>(), v[1].get<float>()});
    }
    area.bounds = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const Point& p : area.polygon)
        area.bounds = {std::min(area.bounds[0], p[0]), std::min(area.bounds[1], p[1]), std::max(area.bounds[2], p[0]),
                       std::max(area.bounds[3], p[1])};
    const TileKey lo = tile_of(area.bounds[0], area.bounds[1]), hi = tile_of(area.bounds[2], area.bounds[3]);
    if (int64_t(hi.x - lo.x + 1) * (hi.z - lo.z + 1) > 65536) return fail(at + ": spans more than 65536 tiles");
    if (a.contains("y")) {
        if (!numbers(a["y"], 2) || a["y"][0] > a["y"][1]) return fail(at + ": \"y\" must be [min, max] with min <= max");
        area.y = {a["y"][0].get<float>(), a["y"][1].get<float>()};
    }
    if (a.contains("flags")) {
        if (!a["flags"].is_object()) return fail(at + ": \"flags\" must be an object");
        area.flags = a["flags"];
    }
    if (a.contains("portal")) {
        const Json& p = a["portal"];
        const Json map = p.is_object() ? p.value("map", Json()) : Json();
        if (!map.is_number_unsigned() || map.get<uint64_t>() > 0xFFFF || !numbers(p.value("pos", Json()), 3) ||
            (p.contains("heading") && !finite_number(p["heading"])))
            return fail(at + ": \"portal\" must be {\"map\": id, \"pos\": [x, y, z], \"heading\": h}");
        area.portal = Portal{static_cast<uint16_t>(map.get<uint64_t>()),
                             {p["pos"][0].get<float>(), p["pos"][1].get<float>(), p["pos"][2].get<float>()},
                             p.value("heading", 0.f)};
    }
    return area;
}

std::optional<PatrolPath> parse_path(const Json& p, const std::string& at, std::string& err) {
    const auto fail = [&](std::string message) -> std::optional<PatrolPath> { err = at + ": " + std::move(message); return std::nullopt; };
    if (!p.is_object()) return fail("must be an object");
    PatrolPath path;
    const Json& points = p.value("points", Json());
    if (!points.is_array() || points.empty() || !std::all_of(points.begin(), points.end(), [](const Json& v) { return numbers(v, 3); }))
        return fail("\"points\" must be a non-empty list of [x, y, z]");
    for (const Json& v : points) path.points.push_back({v[0].get<float>(), v[1].get<float>(), v[2].get<float>()});
    if (p.contains("wait")) {
        if (!finite_number(p["wait"]) || p["wait"].get<double>() < 0) return fail("\"wait\" must be a number >= 0");
        path.wait = p["wait"].get<float>();
    }
    if (p.contains("loop")) {
        if (!p["loop"].is_boolean()) return fail("\"loop\" must be true or false");
        path.loop = p["loop"].get<bool>();
    }
    return path;
}

constexpr size_t kMaxNameBytes = 255;     // an NPC definition name, as npcs/ allows
constexpr double kMaxRespawnSec = 31536000; // a year: keeps now + respawnSec far from TimePoint overflow

} // namespace

std::optional<std::vector<Spawn>> parse_spawns(const Json& j, TileKey tile, std::string& err) {
    const auto fail = [&](std::string message) -> std::optional<std::vector<Spawn>> { err = std::move(message); return std::nullopt; };
    if (!j.is_object()) return fail("must be a JSON object");
    if (j.contains("version") && j["version"] == 1)
        return fail("\"version\" 1 is no longer supported: use 2, with \"npc\" (an npcs/ definition) in place of \"name\" and \"template\"");
    if (!j.contains("version") || j["version"] != 2) return fail("\"version\" must be 2");

    std::map<std::string, PatrolPath> paths;
    if (j.contains("paths")) {
        if (!j["paths"].is_object()) return fail("\"paths\" must be an object of named paths");
        for (const auto& [name, p] : j["paths"].items()) {
            auto path = parse_path(p, "path \"" + name + "\"", err);
            if (!path) return std::nullopt;
            paths.emplace(name, std::move(*path));
        }
    }
    const Json& list = j.value("spawns", Json());
    if (!list.is_array()) return fail("\"spawns\" must be a list");

    std::vector<Spawn> out;
    std::set<std::string> ids;
    for (size_t i = 0; i < list.size(); ++i) {
        const Json& s = list[i];
        if (!s.is_object()) return fail("spawns[" + std::to_string(i) + "]: must be an object");
        const Json& id = s.value("id", Json());
        if (!id.is_string() || id.get<std::string>().empty())
            return fail("spawns[" + std::to_string(i) + "]: \"id\" must be a non-empty string");
        Spawn spawn;
        spawn.id = id.get<std::string>();
        const std::string at = "spawn \"" + spawn.id + "\": ";
        if (!ids.insert(spawn.id).second) return fail(at + "duplicate id");
        for (const auto& [key, value] : s.items())
            if (key != "id" && key != "npc" && key != "pos" && key != "heading" && key != "respawnSec" && key != "path")
                return fail(at + "unknown key \"" + key + "\"");
        const Json& npc = s.value("npc", Json());
        if (!npc.is_string() || npc.get<std::string>().empty() || npc.get<std::string>().size() > kMaxNameBytes)
            return fail(at + "\"npc\" must be a non-empty string of at most 255 bytes");
        spawn.npc = npc.get<std::string>();
        if (!numbers(s.value("pos", Json()), 3)) return fail(at + "\"pos\" must be [x, y, z]");
        spawn.pos = {s["pos"][0].get<float>(), s["pos"][1].get<float>(), s["pos"][2].get<float>()};
        if (tile_of(spawn.pos[0], spawn.pos[2]) != tile)
            return fail(at + "\"pos\" is outside tile " + std::to_string(tile.x) + "_" + std::to_string(tile.z));
        if (s.contains("heading")) {
            if (!finite_number(s["heading"])) return fail(at + "\"heading\" must be a number");
            spawn.heading = s["heading"].get<float>();
        }
        const Json& respawn = s.value("respawnSec", Json());
        if (!finite_number(respawn) || respawn.get<double>() > kMaxRespawnSec ||
            (respawn.get<double>() < 0 && respawn.get<double>() != -1))
            return fail(at + "\"respawnSec\" must be a number in [0, 31536000] (a year), or -1 for never");
        spawn.respawnSec = respawn.get<float>();
        if (s.contains("path")) {
            const auto it = s["path"].is_string() ? paths.find(s["path"].get<std::string>()) : paths.end();
            if (it == paths.end()) return fail(at + "unknown path " + s["path"].dump());
            spawn.path = it->second;
        }
        out.push_back(std::move(spawn));
    }
    return out;
}

TileKey tile_of(float x, float z) {
    return {static_cast<int32_t>(std::floor(x / kTileSize)), static_cast<int32_t>(std::floor(z / kTileSize))};
}

bool MapInfo::has_tile(TileKey t) const {
    return std::any_of(tiles.begin(), tiles.end(),
                       [t](const TileRect& r) { return t.x >= r.x0 && t.x <= r.x1 && t.z >= r.z0 && t.z <= r.z1; });
}

int MapInfo::tile_count() const {
    int n = 0;
    for (const TileRect& r : tiles) n += (r.x1 - r.x0 + 1) * (r.z1 - r.z0 + 1);
    return n;
}

std::array<float, 2> MapInfo::clamp(float x, float z) const {
    std::array<float, 2> best{x, z};
    float bestDist = std::numeric_limits<float>::infinity();
    for (const TileRect& r : tiles) {
        // The far edge belongs to the next tile, so stop one float short of it.
        const float hiX = std::nextafter(static_cast<float>(r.x1 + 1) * kTileSize, -std::numeric_limits<float>::infinity());
        const float hiZ = std::nextafter(static_cast<float>(r.z1 + 1) * kTileSize, -std::numeric_limits<float>::infinity());
        const float cx = std::clamp(x, static_cast<float>(r.x0) * kTileSize, hiX);
        const float cz = std::clamp(z, static_cast<float>(r.z0) * kTileSize, hiZ);
        const float d = (cx - x) * (cx - x) + (cz - z) * (cz - z);
        if (d < bestDist) { bestDist = d; best = {cx, cz}; }
    }
    return best;
}

std::optional<MapInfo> parse_manifest(const Json& j, std::string& err) {
    const auto fail = [&](std::string message) -> std::optional<MapInfo> { err = std::move(message); return std::nullopt; };
    if (!j.is_object()) return fail("must be a JSON object");
    if (!j.contains("version") || j["version"] != 1) return fail("\"version\" must be 1");

    MapInfo m;
    const Json& id = j.value("id", Json());
    if (!id.is_number_unsigned() || id.get<uint64_t>() > 0xFFFF)
        return fail("\"id\" must be an integer in [0, 65535]");
    m.id = static_cast<uint16_t>(id.get<uint64_t>());
    if (const Json& name = j.value("name", Json()); name.is_string()) m.name = name.get<std::string>();
    else if (!name.is_null()) return fail("\"name\" must be a string");

    const Json& tiles = j.value("tiles", Json());
    if (!tiles.is_array() || tiles.empty()) return fail("\"tiles\" must be a non-empty list of [x0, z0, x1, z1]");
    for (size_t i = 0; i < tiles.size(); ++i) {
        const Json& t = tiles[i];
        const std::string where = "tiles[" + std::to_string(i) + "]";
        if (!t.is_array() || t.size() != 4 || !std::all_of(t.begin(), t.end(), int32_value))
            return fail(where + ": must be [x0, z0, x1, z1] integers");
        const TileRect r{t[0].get<int32_t>(), t[1].get<int32_t>(), t[2].get<int32_t>(), t[3].get<int32_t>()};
        if (r.x0 > r.x1 || r.z0 > r.z1) return fail(where + ": x0 <= x1 and z0 <= z1 required");
        m.tiles.push_back(r);
    }

    const Json& spawn = j.value("spawn", Json());
    if (!spawn.is_object() || !numbers(spawn.value("pos", Json()), 3))
        return fail("\"spawn\" must be {\"pos\": [x, y, z], \"heading\": h}");
    for (size_t i = 0; i < 3; ++i) m.spawn[i] = spawn["pos"][i].get<float>();
    if (spawn.contains("heading")) {
        if (!finite_number(spawn["heading"])) return fail("\"spawn.heading\" must be a number");
        m.spawnHeading = spawn["heading"].get<float>();
    }
    if (!m.contains(m.spawn[0], m.spawn[2])) return fail("\"spawn.pos\" is not on a declared tile");

    if (j.contains("heightRange")) {
        if (!numbers(j["heightRange"], 2) || j["heightRange"][0] > j["heightRange"][1])
            return fail("\"heightRange\" must be [min, max] with min <= max");
        m.heightRange = {j["heightRange"][0].get<float>(), j["heightRange"][1].get<float>()};
    }
    if (j.contains("material")) {
        // A map editor may write an asset ref {"id", "path"}: the path is what loads.
        const Json& material = j["material"].is_object() ? j["material"].value("path", Json()) : j["material"];
        if (!material.is_string()) return fail("\"material\" must be a string");
        m.material = material.get<std::string>();
    }
    if (j.contains("terrainUv")) {
        const Json& uv = j["terrainUv"];
        if (uv == "map") m.terrainUv = TerrainUv::Map;
        else if (uv != "tiled") return fail("\"terrainUv\" must be \"tiled\" or \"map\"");
    }
    if (j.contains("type")) {
        if (j["type"] == "instance") m.type = MapType::Instance;
        else if (j["type"] != "world") return fail("\"type\" must be \"world\" or \"instance\"");
    }
    if (j.contains("layerCap")) {
        const Json& v = j["layerCap"];
        if (m.instanced()) return fail("\"layerCap\" is only for world maps; an instance map uses \"maxPlayers\"");
        if (!v.is_number_unsigned() || v.get<uint64_t>() < 1 || v.get<uint64_t>() > 0xFFFFFFFF)
            return fail("\"layerCap\" must be an integer >= 1 (leave it out for one layer)");
        m.layerCap = static_cast<uint32_t>(v.get<uint64_t>());
    }
    for (const char* key : {"maxPlayers", "exit"})
        if (!m.instanced() && j.contains(key))
            return fail(std::string("\"") + key + "\" is only for instance maps (\"type\": \"instance\")");
    if (!m.instanced() && !m.layerCap && j.contains("emptyTimeoutSec"))
        return fail("\"emptyTimeoutSec\" is only for instance maps (\"type\": \"instance\") or layered world maps (\"layerCap\")");
    if (j.contains("emptyTimeoutSec")) {
        const Json& v = j["emptyTimeoutSec"];
        if (!v.is_number_unsigned() || v.get<uint64_t>() > uint64_t(kMaxRespawnSec))
            return fail("\"emptyTimeoutSec\" must be an integer in [0, 31536000] (a year)");
        m.emptyTimeoutSec = static_cast<uint32_t>(v.get<uint64_t>());
    }
    if (m.instanced()) {
        if (j.contains("maxPlayers")) {
            const Json& v = j["maxPlayers"];
            if (!v.is_number_unsigned() || v.get<uint64_t>() < 1 || v.get<uint64_t>() > 0xFFFFFFFF)
                return fail("\"maxPlayers\" must be an integer >= 1 (leave it out for no limit)");
            m.maxPlayers = static_cast<uint32_t>(v.get<uint64_t>());
        }
        const Json& exit = j.value("exit", Json());
        const Json map = exit.is_object() ? exit.value("map", Json()) : Json();
        if (!map.is_number_unsigned() || map.get<uint64_t>() > 0xFFFF || !numbers(exit.value("pos", Json()), 3) ||
            (exit.contains("heading") && !finite_number(exit["heading"])))
            return fail("\"exit\" must be {\"map\": id, \"pos\": [x, y, z], \"heading\": h} (required on an instance map)");
        m.exit = Portal{static_cast<uint16_t>(map.get<uint64_t>()),
                        {exit["pos"][0].get<float>(), exit["pos"][1].get<float>(), exit["pos"][2].get<float>()},
                        exit.value("heading", 0.f)};
    }
    if (j.contains("nav")) {
        const Json& nav = j["nav"];
        if (!nav.is_object()) return fail("\"nav\" must be an object");
        NavParams p;
        for (auto [key, out] : {std::pair{"agentRadius", &p.agentRadius}, {"agentHeight", &p.agentHeight},
                                {"maxClimb", &p.maxClimb}, {"maxSlopeDeg", &p.maxSlopeDeg}, {"cellSize", &p.cellSize}}) {
            if (!nav.contains(key)) continue;
            if (!finite_number(nav[key]) || nav[key].get<double>() < 0)
                return fail(std::string("\"nav.") + key + "\" must be a number >= 0");
            *out = nav[key].get<float>();
        }
        m.nav = p;
    }
    return m;
}

std::optional<Areas> parse_areas(const Json& j, std::string& err) {
    const auto fail = [&](std::string message) -> std::optional<Areas> { err = std::move(message); return std::nullopt; };
    if (!j.is_object()) return fail("must be a JSON object");
    if (!j.contains("version") || j["version"] != 1) return fail("\"version\" must be 1");
    const Json& list = j.value("areas", Json());
    if (!list.is_array()) return fail("\"areas\" must be a list");
    if (list.size() > static_cast<size_t>(std::numeric_limits<int16_t>::max())) return fail("\"areas\": too many");

    Areas out;
    std::map<std::string, size_t> indexOf;
    for (size_t i = 0; i < list.size(); ++i) {
        auto area = parse_area(list[i], "areas[" + std::to_string(i) + "]", err);
        if (!area) return std::nullopt;
        if (!indexOf.emplace(area->id, i).second) return fail("area \"" + area->id + "\": duplicate id");
        out.list.push_back(std::move(*area));
    }

    // Flags: the parent's resolved flags, then the area's own on top. Walk up to the first resolved ancestor (or a
    // root), then resolve back down that chain.
    std::vector<bool> resolved(out.list.size());
    for (size_t i = 0; i < out.list.size(); ++i) {
        std::vector<size_t> chain;
        for (size_t k = i; !resolved[k];) {
            if (std::find(chain.begin(), chain.end(), k) != chain.end())
                return fail("area \"" + out.list[k].id + "\": parent cycle");
            chain.push_back(k);
            const std::string& parent = out.list[k].parent;
            if (parent.empty()) break;
            auto it = indexOf.find(parent);
            if (it == indexOf.end()) return fail("area \"" + out.list[k].id + "\": unknown parent \"" + parent + "\"");
            k = it->second;
        }
        for (auto k = chain.rbegin(); k != chain.rend(); ++k) {
            Area& area = out.list[*k];
            if (!area.parent.empty()) {
                Json flags = out.list[indexOf.at(area.parent)].flags;
                flags.update(area.flags);
                area.flags = std::move(flags);
            }
            resolved[*k] = true;
        }
    }

    // ponytail: O(n^2) pair check at load; fine for hundreds of areas per map, per-tile pairs if maps get thousands.
    for (size_t i = 0; i < out.list.size(); ++i)
        for (size_t k = i + 1; k < out.list.size(); ++k)
            if (out.list[i].priority == out.list[k].priority && overlap(out.list[i], out.list[k]))
                return fail("areas \"" + out.list[i].id + "\" and \"" + out.list[k].id +
                            "\" overlap with equal priority " + std::to_string(out.list[i].priority));

    for (size_t i = 0; i < out.list.size(); ++i) {
        const auto& b = out.list[i].bounds;
        const TileKey lo = tile_of(b[0], b[1]), hi = tile_of(b[2], b[3]);
        for (int32_t x = lo.x; x <= hi.x; ++x)
            for (int32_t z = lo.z; z <= hi.z; ++z) out.byTile[{x, z}].push_back(static_cast<int16_t>(i));
    }
    for (auto& [tile, ids] : out.byTile)
        std::stable_sort(ids.begin(), ids.end(), [&](int16_t a, int16_t b) { return out.list[a].priority > out.list[b].priority; });
    return out;
}

std::optional<std::string> exit_error(const MapInfo& instance, const MapInfo* to) {
    if (!instance.exit) return std::nullopt;
    const Portal& e = *instance.exit;
    const std::string id = std::to_string(e.map);
    if (!to) return "\"exit\": to unknown map " + id;
    if (to->instanced()) return "\"exit\": map " + id + " is an instance map; an exit must be on a world map";
    if (!to->contains(e.pos[0], e.pos[2])) return "\"exit\": position is not on a tile of map " + id;
    if (const int16_t i = area_at(to->areas, e.pos[0], e.pos[1], e.pos[2]); i >= 0 && to->areas.list[i].portal)
        return "\"exit\": position is inside portal area \"" + to->areas.list[i].id + "\" of map " + id;
    return std::nullopt;
}

int16_t area_at(const Areas& areas, float x, float y, float z) {
    const auto it = areas.byTile.find(tile_of(x, z));
    if (it == areas.byTile.end()) return -1;
    for (int16_t i : it->second) {
        const Area& a = areas.list[i];
        if (a.y && (y < (*a.y)[0] || y > (*a.y)[1])) continue;
        if (x >= a.bounds[0] && x <= a.bounds[2] && z >= a.bounds[1] && z <= a.bounds[3] && inside(a.polygon, x, z))
            return i;
    }
    return -1;
}

} // namespace chthonic::map
