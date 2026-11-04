// Zone objects (docs/world/maps.html#objects): what a map places on its tiles. objects/<tx>_<tz>.json (client
// and mapbake) lists every object of the tile, .server/interactables/<tx>_<tz>.json (server only) what the server needs
// to check an activation. An object belongs to the tile its pos is in. Shapes stand on pos: a box is turned by yaw about
// +y, a cylinder is upright. Yaw is radians, like heading (atan2(dx, dz)). Pure data, no file IO, like map.h.
#pragma once
#include "map.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chthonic::map {

// visual: client only, blocks nothing. obstacle: blocks (client physics, the navmesh). interactable: blocks, and a
// player can activate it (Interact).
enum class ObjectType { Visual, Obstacle, Interactable };

const char* object_type_name(ObjectType type); // "visual", "obstacle", "interactable"

// {"box": {"halfExtents": [x, y, z]}} or {"cylinder": {"radius": r, "height": h}}, in metres (scale does not apply).
struct Shape {
    enum class Kind { Box, Cylinder } kind = Kind::Box;
    std::array<float, 3> halfExtents{0.5f, 0.5f, 0.5f}; // box
    float radius = 0.5f, height = 1;                      // cylinder
};

struct ZoneObject {
    ObjectType type = ObjectType::Visual;
    std::string prefab; // project-relative .prefab
    std::array<float, 3> pos{};
    float yaw = 0;
    float scale = 1;            // uniform, the prefab's only
    std::optional<Shape> shape; // obstacle and interactable (required); a visual's is not read
    uint32_t id = 0;            // interactable: 1..2^32-1, unique per map
    std::string name;           // interactable, optional: what the client's query reports
};

// One tile's objects file. A problem with one entry ("objects[3]: \"scale\" must be a number > 0") skips that entry;
// a problem with the file itself (not an object, bad version, no list) yields no objects. Ids are checked unique within
// the file (a repeat is an error, its entry skipped); across a map's files by the loaders.
struct ParsedObjects {
    std::vector<ZoneObject> objects;
    std::vector<std::string> errors;
};
ParsedObjects parse_objects(const nlohmann::json& j, TileKey tile);

// What the server knows of an interactable: where it is and what activating it publishes.
struct Interactable {
    uint32_t id = 0;
    std::array<float, 3> pos{};
    float yaw = 0;
    Shape shape;
    float range = 3;     // metres from the shape's edge (horizontal) a player may activate it from
    std::string event;   // the server bus event, non-empty
    nlohmann::json data; // an object passed as the event's data; null if the file has none
    std::string name;
};

// One tile's .server/interactables file: every entry valid (pos on tile, a shape, range > 0, an event, data an object
// at most 31 deep, so the event payload is at most 32), ids unique within the file. nullopt with err set on the first
// problem, as parse_spawns.
std::optional<std::vector<Interactable>> parse_interactables(const nlohmann::json& j, TileKey tile, std::string& err);

// Horizontal distance from (x, z) to the shape's footprint at pos turned by yaw; 0 inside it.
float footprint_distance(const Shape& shape, const std::array<float, 3>& pos, float yaw, float x, float z);

// The footprint outline on x/z: a box's 4 corners, a cylinder as `segments` points on its circle.
std::vector<std::array<float, 2>> footprint(const Shape& shape, const std::array<float, 3>& pos, float yaw, int segments = 16);

} // namespace chthonic::map
