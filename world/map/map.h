// Map manifests (map.json): a map is a set of 256 m tiles, joined to other maps only by transfers. Pure data: no file
// IO, no server deps, so the server, the bot and tools all share it. Coordinates: Y up, metres, the
// map's origin is the corner of tile (0,0), heading = atan2(dx, dz).
#pragma once
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace chthonic::map {

constexpr float kTileSize = 256;   // metres per tile edge
constexpr int kTileSamples = 257;  // height samples per tile edge (shared border)

struct TileKey {
    int32_t x = 0, z = 0;
    bool operator==(const TileKey&) const = default;
};

// The tile containing (x, z): floor(x / 256), floor(z / 256).
TileKey tile_of(float x, float z);

struct TileKeyHash {
    size_t operator()(TileKey t) const { return std::hash<uint64_t>{}((uint64_t(uint32_t(t.x)) << 32) | uint32_t(t.z)); }
};

struct TileRect {
    int32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0; // inclusive
};

struct NavParams {
    float agentRadius = 0.4f, agentHeight = 1.8f, maxClimb = 0.5f, maxSlopeDeg = 45, cellSize = 0.5f;
};

// Where a portal area sends whoever walks into it.
struct Portal {
    uint16_t map = 0;
    std::array<float, 3> pos{};
    float heading = 0;
};

// A named region of a map (areas.json): a 2D rect or polygon on x/z, optionally limited to a y range.
struct Area {
    std::string id, name, parent, music;
    int priority = 0;
    std::vector<std::array<float, 2>> polygon; // a rect is stored as its 4 corners
    std::array<float, 4> bounds{};             // x0, z0, x1, z1 of the polygon
    std::optional<std::array<float, 2>> y;     // [min, max]
    nlohmann::json flags = nlohmann::json::object(); // own flags over the parent's (resolved at parse)
    std::optional<Portal> portal;
};

struct Areas {
    std::vector<Area> list;
    // Every tile an area's bounds touch -> those areas' indices, highest priority first.
    std::unordered_map<TileKey, std::vector<int16_t>, TileKeyHash> byTile;
};

// A route an NPC walks (spawns/<tx>_<tz>.json "paths"): the points in order, wait seconds at each, then back to the
// first (loop) or back along the route (ping-pong).
struct PatrolPath {
    std::vector<std::array<float, 3>> points;
    float wait = 0;
    bool loop = true;
};

// One NPC a tile's spawn file places (server only: <map>/.server/spawns/). respawnSec -1: never comes back.
// npc names a server NPC definition (LUA_ROOT/npcs/), checked by the server; file is set by the loader, for errors.
struct Spawn {
    std::string id, npc;
    std::array<float, 3> pos{};
    float heading = 0;
    float respawnSec = -1;
    std::optional<PatrolPath> path; // the file's named path, copied in at parse
    std::string file;               // "maps/<folder>/.server/spawns/<tx>_<tz>.json"; empty from parse_spawns
};

// How terrain UVs run: Tiled = world xz / 8 (a texture repeating every 8 m); Map = one texture over the bounding box of
// the declared tiles (map.json "terrainUv": "tiled" | "map").
enum class TerrainUv { Tiled, Map };

// map.json "type": a world map runs as one context for everyone (with "layerCap": one per layer, opened as the map
// fills); an instance map has none until someone enters, then one copy per instance key
// (docs/world/instances.html).
enum class MapType { World, Instance };

struct MapInfo {
    uint16_t id = 0;
    std::string name;
    std::vector<TileRect> tiles; // the map = union of these
    std::array<float, 3> spawn{};
    float spawnHeading = 0;
    std::optional<std::array<float, 2>> heightRange; // phases 4-6
    std::string material;                            // phases 4-6
    TerrainUv terrainUv = TerrainUv::Tiled;
    std::optional<NavParams> nav;                    // phase 6
    Areas areas;                                     // areas.json, loaded separately (parse_areas); empty if none
    std::vector<Spawn> spawns;                       // every owned tile's spawn file (parse_spawns); empty if none
    MapType type = MapType::World;
    // Instance maps only: players per copy (nullopt: no limit), seconds an empty copy lives on (an extra layer too), and
    // where a character goes when its copy is gone (required; the loader checks it is on a loaded world map's tiles).
    std::optional<uint32_t> maxPlayers;
    uint32_t emptyTimeoutSec = 300;
    // World maps only: players per layer (nullopt: one context, no layers; docs/world/maps.html#layers).
    std::optional<uint32_t> layerCap;
    std::optional<Portal> exit;

    bool instanced() const { return type == MapType::Instance; }

    // ponytail: linear scan of the rects; a tile set if maps grow many rects.
    bool has_tile(TileKey t) const;
    bool contains(float x, float z) const { return has_tile(tile_of(x, z)); }
    // Tiles declared: the rects' areas summed (what the navmesh is sized for).
    int tile_count() const;
    // The nearest point to (x, z) on a declared tile (itself if already on one).
    std::array<float, 2> clamp(float x, float z) const;
};

// Parses and validates a map.json document. nullopt with err set ("tiles[1]: x0 > x1") on any problem.
std::optional<MapInfo> parse_manifest(const nlohmann::json& j, std::string& err);

// Parses and validates an areas.json document: shapes, unknown or cyclic parents, and equal-priority areas that
// overlap. Portals are checked against the other maps by the loader. nullopt with err set on any problem.
std::optional<Areas> parse_areas(const nlohmann::json& j, std::string& err);

// Parses and validates one tile's spawn file (.server/spawns/<tx>_<tz>.json, version 2): only known keys per spawn,
// every spawn's pos inside tile, ids unique within the file, every "path" one of the file's "paths". Ids across a map's
// files are checked by the loader, "npc" names by the server. nullopt with err set on any problem.
std::optional<std::vector<Spawn>> parse_spawns(const nlohmann::json& j, TileKey tile, std::string& err);

// An instance map's "exit" against the map it names (to: nullptr if that is not loaded), as the loaders check it: a
// world map, the position on its tiles and not inside one of its portal areas. The problem ("\"exit\": ..."), or
// nullopt.
std::optional<std::string> exit_error(const MapInfo& instance, const MapInfo* to);

// The index in areas.list of the highest-priority area containing (x, y, z), or -1. Edges: a point on a boundary may
// fall either side.
int16_t area_at(const Areas& areas, float x, float y, float z);

} // namespace chthonic::map
