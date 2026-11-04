// Map zones (zones.json): named sets of a map's tiles. Every declared tile is in exactly one zone and a zone's tiles are
// connected through shared edges (diagonal-only does not count). Pure data like map.h; clients and map tools can use
// it, the world server and chthonic_mapbake ignore the file. Format: docs/world/maps.html#zones.
#pragma once
#include "map.h"

#include <string_view>

namespace chthonic::map {

struct Zone {
    std::string id, name;                             // name defaults to id
    std::vector<TileRect> tiles;                      // the zone = union of these (inclusive, like map.json)
    nlohmann::json flags = nlohmann::json::object();  // passed through untouched
    nlohmann::ordered_json raw = nlohmann::ordered_json::object(); // the zone's object as read: unknown keys, key order
};

struct Zones {
    std::vector<Zone> list;
    bool implicit = false; // no zones.json: one zone of every declared tile
    std::unordered_map<TileKey, int16_t, TileKeyHash> byTile; // filled by validate_zones
};

// A zone id (and a map folder): letters, digits, _ and -, not starting with -.
bool valid_zone_id(std::string_view id);

// No zones.json: one zone, id = the map's folder, name = the map's name (or the folder), every declared tile.
Zones implicit_zones(const MapInfo& map, const std::string& folder);

// Checks a zone set against its map and fills zones.byTile. Every problem, each naming its zone ("zone \"a\": not
// contiguous ..."): bad or duplicate id, no tiles, a tile not declared in map.json, two zones on one tile, a declared
// tile in no zone, a zone in pieces (4-neighbour). Empty: valid. Works on in-memory zones too.
std::vector<std::string> validate_zones(Zones& zones, const MapInfo& map);

// The zones of a zones.json document as written, not validated (validate_zones): nullopt with err set to the first
// structural problem (not an object, "version" not 1, a zone's bad field), unprefixed.
std::optional<Zones> read_zones(const nlohmann::ordered_json& j, std::string& err);

// Parses and validates maps/<folder>/zones.json. nullopt with err set to the first problem, prefixed with the file
// ("maps/meadow/zones.json: zone \"a\": no tiles").
std::optional<Zones> parse_zones(std::string_view text, const MapInfo& map, const std::string& folder, std::string& err);

// The index in zones.list of the zone holding the tile / the point (x, z), or -1 off the map's tiles.
int16_t zone_at(const Zones& zones, TileKey tile);
int16_t zone_at(const Zones& zones, float x, float z);

} // namespace chthonic::map
