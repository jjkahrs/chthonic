// Baked navmeshes (docs/world/maps.html#navmesh): one Detour dtNavMesh per map, built from the owned tiles' navtiles
// (<map>/.server/nav/<tx>_<tz>.navtile, tools/mapbake). The only server code that sees Detour.
#pragma once
#include <map.h>
#include <messages.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace chthonic::world {

class Nav {
public:
    Nav();
    ~Nav();
    Nav(Nav&&) noexcept;
    Nav& operator=(Nav&&) noexcept;

    // Adds one navtile for `tile` of map; the map's dtNavMesh is made on its first tile, sized for tileCount tiles
    // (the map's declared tiles). false with err set on a bad magic, navtile or Detour version, a tile baked for another
    // tile, or more polygons than 32-bit poly refs leave per tile for that many tiles.
    bool add_tile(uint16_t map, int tileCount, map::TileKey tile, std::string_view bytes, std::string& err);
    // Whether the map has a navmesh (any navtile loaded).
    bool has(uint16_t map) const;
    // The straight path along the navmesh from `from` to `to`, each snapped onto the mesh (within 2 m across, 4 m up or
    // down): the snapped start first, the snapped end last, the corners between. Empty if the map has no navmesh,
    // either end is not near it, or `to` cannot be reached.
    std::vector<proto::Vec3> path(uint16_t map, proto::Vec3 from, proto::Vec3 to) const;
    // Whether pos is on the map's navmesh: a polygon within 0.5 m across and max(maxAir, 1) m up or down (1 m: the
    // detail mesh's height error). False if the map has no navmesh.
    bool on_mesh(uint16_t map, proto::Vec3 pos, float maxAir) const;

private:
    struct Mesh;
    std::map<uint16_t, std::unique_ptr<Mesh>> meshes_;
};

} // namespace chthonic::world
