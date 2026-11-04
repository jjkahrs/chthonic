// Offline navmesh bake (chthonic_mapbake): one Recast tile per map tile from the same ground triangles the client renders
// and Jolt collides with (map::for_each_triangle), with the manifest's nav params. Pure: maps and heights in, bytes out,
// so world_self_check bakes fixtures in memory. Recast stays inside bake.cpp.
#pragma once
#include <heights.h>
#include <map.h>
#include <objects.h>

#include <optional>
#include <string>
#include <vector>

namespace chthonic::mapbake {

struct TileBake {
    std::string navtile; // map/navtile.h header + Detour tile; empty: nothing walkable on the tile (write no file)
    int polys = 0;
};

// Bakes tile `tile` of the map: its ground triangles plus a border of its neighbours' (tiles on the map only; a tile
// with no entry in `heights` is flat), so paths cross tile edges. The footprint of every obstacle and interactable in
// `objects` (the tile's and its neighbours'; visuals are skipped) is cut out, with agentRadius clearance around it.
// Params: info.nav, or NavParams' defaults if the manifest has none. nullopt with err set if Recast or Detour fail, or
// the tile has more polygons than map::max_navtile_polys allows for the map's tile count.
std::optional<TileBake> bake_tile(const map::MapInfo& info, const map::TileHeights& heights, map::TileKey tile,
                                  std::string& err, const std::vector<map::ZoneObject>& objects = {});

// Every declared tile's edge against its +x and +z neighbour's (the shared samples must be equal): one line per
// mismatching edge ("seam 0_0 | 1_0: 12 samples differ, worst 3.20 m at z 40").
std::vector<std::string> seam_errors(const map::MapInfo& info, const map::TileHeights& heights);

} // namespace chthonic::mapbake
