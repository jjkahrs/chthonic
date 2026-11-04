#include "zones.h"

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_set>

namespace chthonic::map {
namespace {

using Json = nlohmann::ordered_json;
using TileSet = std::unordered_set<TileKey, TileKeyHash>;

std::string tile_name(TileKey t) { return std::to_string(t.x) + "_" + std::to_string(t.z); }
std::string in_quotes(const std::string& s) { return "\"" + s + "\""; }

// fn(tile) for every tile of the rects, until it returns false.
template <typename Fn>
void each_tile(const std::vector<TileRect>& rects, Fn&& fn) {
    for (const TileRect& r : rects)
        for (int64_t z = r.z0; z <= r.z1; ++z)
            for (int64_t x = r.x0; x <= r.x1; ++x)
                if (!fn(TileKey{int32_t(x), int32_t(z)})) return;
}

// A tile of tiles not reachable from the first through shared edges, or nullopt if they are all connected.
std::optional<TileKey> disconnected(const TileSet& tiles) {
    if (tiles.empty()) return std::nullopt;
    TileSet seen{*tiles.begin()};
    std::vector<TileKey> todo{*tiles.begin()};
    while (!todo.empty()) {
        const TileKey t = todo.back();
        todo.pop_back();
        for (const TileKey n : {TileKey{t.x - 1, t.z}, TileKey{t.x + 1, t.z}, TileKey{t.x, t.z - 1}, TileKey{t.x, t.z + 1}})
            if (tiles.contains(n) && seen.insert(n).second) todo.push_back(n);
    }
    for (const TileKey t : tiles)
        if (!seen.contains(t)) return t;
    return std::nullopt;
}

bool int32_value(const Json& v) {
    return v.is_number_integer() && v.get<int64_t>() >= std::numeric_limits<int32_t>::min() &&
           v.get<int64_t>() <= std::numeric_limits<int32_t>::max();
}

std::optional<Zone> parse_zone(const Json& z, const std::string& where, std::string& err) {
    const auto fail = [&](std::string message) -> std::optional<Zone> { err = std::move(message); return std::nullopt; };
    if (!z.is_object()) return fail(where + ": must be an object");
    Zone out;
    out.raw = z;
    const Json& id = z.value("id", Json());
    if (!id.is_string()) return fail(where + ": \"id\" must be a string");
    out.id = id.get<std::string>();
    const std::string zone = "zone " + in_quotes(out.id);
    if (const Json& name = z.value("name", Json()); name.is_string()) out.name = name.get<std::string>();
    else if (!name.is_null()) return fail(zone + ": \"name\" must be a string");
    if (out.name.empty()) out.name = out.id;
    const Json& tiles = z.value("tiles", Json());
    if (!tiles.is_array()) return fail(zone + ": \"tiles\" must be a list of [x0, z0, x1, z1]");
    for (size_t i = 0; i < tiles.size(); ++i) {
        const Json& t = tiles[i];
        const std::string at = zone + ": tiles[" + std::to_string(i) + "]";
        if (!t.is_array() || t.size() != 4 || !std::all_of(t.begin(), t.end(), int32_value))
            return fail(at + ": must be [x0, z0, x1, z1] integers");
        const TileRect r{t[0].get<int32_t>(), t[1].get<int32_t>(), t[2].get<int32_t>(), t[3].get<int32_t>()};
        if (r.x0 > r.x1 || r.z0 > r.z1) return fail(at + ": x0 <= x1 and z0 <= z1 required");
        out.tiles.push_back(r);
    }
    if (z.contains("flags")) {
        if (!z["flags"].is_object()) return fail(zone + ": \"flags\" must be an object");
        out.flags = nlohmann::json(z["flags"]);
    }
    return out;
}

} // namespace

bool valid_zone_id(std::string_view id) {
    return !id.empty() && id[0] != '-' && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

Zones implicit_zones(const MapInfo& map, const std::string& folder) {
    Zones out;
    out.implicit = true;
    Zone zone{folder, map.name.empty() ? folder : map.name, map.tiles};
    out.list.push_back(std::move(zone));
    validate_zones(out, map);
    return out;
}

std::vector<std::string> validate_zones(Zones& zones, const MapInfo& map) {
    std::vector<std::string> problems;
    zones.byTile.clear();
    if (zones.list.size() > static_cast<size_t>(std::numeric_limits<int16_t>::max())) return {"too many zones"};
    std::map<std::string, size_t> seen;
    for (size_t i = 0; i < zones.list.size(); ++i) {
        const Zone& z = zones.list[i];
        const std::string zone = "zone " + in_quotes(z.id);
        if (!valid_zone_id(z.id)) problems.push_back(zone + ": id must be letters, digits, _ and - (not starting with -)");
        if (!seen.emplace(z.id, i).second) problems.push_back(zone + ": duplicate id");
        TileSet own;
        std::optional<TileKey> undeclared, overlap;
        int16_t other = -1;
        each_tile(z.tiles, [&](TileKey t) {
            if (!map.has_tile(t)) {
                undeclared = t; // stops: a rect far off the map would take forever
                return false;
            }
            if (!own.insert(t).second) return true; // the zone's own rects may overlap
            const auto [it, fresh] = zones.byTile.emplace(t, static_cast<int16_t>(i));
            if (!fresh && !overlap) overlap = t, other = it->second;
            return true;
        });
        if (own.empty() && !undeclared) problems.push_back(zone + ": no tiles");
        if (undeclared) problems.push_back(zone + ": tile " + tile_name(*undeclared) + " is not declared in map.json");
        if (overlap)
            problems.push_back("zones " + in_quotes(zones.list[other].id) + " and " + in_quotes(z.id) + " overlap at tile " +
                               tile_name(*overlap));
        if (const auto apart = undeclared ? std::nullopt : disconnected(own)) // own is partial past an undeclared tile
            problems.push_back(zone + ": not contiguous (tile " + tile_name(*apart) +
                               " shares no edge path with the rest; diagonal neighbours do not connect)");
    }
    size_t unassigned = 0;
    std::optional<TileKey> first;
    TileSet counted; // map.json rects may overlap
    each_tile(map.tiles, [&](TileKey t) {
        if (zones.byTile.contains(t) || !counted.insert(t).second) return true;
        if (!first) first = t;
        ++unassigned;
        return true;
    });
    if (first)
        problems.push_back(unassigned == 1 ? "tile " + tile_name(*first) + " is in no zone"
                                           : std::to_string(unassigned) + " declared tiles are in no zone (first " +
                                                 tile_name(*first) + ")");
    return problems;
}

std::optional<Zones> read_zones(const Json& j, std::string& err) {
    const auto fail = [&](std::string message) -> std::optional<Zones> { err = std::move(message); return std::nullopt; };
    if (!j.is_object()) return fail("must be a JSON object");
    if (!j.contains("version") || j["version"] != 1) return fail("\"version\" must be 1");
    const Json& list = j.value("zones", Json());
    if (!list.is_array()) return fail("\"zones\" must be a list");
    Zones out;
    for (size_t i = 0; i < list.size(); ++i) {
        auto zone = parse_zone(list[i], "zones[" + std::to_string(i) + "]", err);
        if (!zone) return std::nullopt;
        out.list.push_back(std::move(*zone));
    }
    return out;
}

std::optional<Zones> parse_zones(std::string_view text, const MapInfo& map, const std::string& folder, std::string& err) {
    const std::string file = "maps/" + folder + "/zones.json: ";
    const auto fail = [&](std::string message) -> std::optional<Zones> { err = file + std::move(message); return std::nullopt; };
    const Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded()) return fail("not valid JSON");
    auto out = read_zones(j, err);
    if (!out) return fail(err);
    if (const auto problems = validate_zones(*out, map); !problems.empty()) return fail(problems.front());
    return out;
}

int16_t zone_at(const Zones& zones, TileKey tile) {
    const auto it = zones.byTile.find(tile);
    return it == zones.byTile.end() ? -1 : it->second;
}

int16_t zone_at(const Zones& zones, float x, float z) { return zone_at(zones, tile_of(x, z)); }

} // namespace chthonic::map
