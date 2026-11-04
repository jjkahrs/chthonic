#include "bake.h"
#include <navtile.h>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace chthonic::mapbake {
namespace {

template <typename T, void (*Free)(T*)>
struct Deleter {
    void operator()(T* p) const { Free(p); }
};
template <typename T, void (*Free)(T*)>
using Owned = std::unique_ptr<T, Deleter<T, Free>>;

const map::Heights& heights_of(const map::TileHeights& heights, map::TileKey t) {
    static const map::Heights flat = map::flat_heights();
    const auto it = heights.find(t);
    return it == heights.end() ? flat : it->second;
}

// The ground triangles of `tile` and of its neighbours on the map that touch [lo, hi] on x/z, as Recast input.
struct Geometry {
    std::vector<float> verts; // 3 per vertex, 3 vertices per triangle (unshared: Recast only rasterizes)
    std::vector<int> tris;
};

Geometry gather(const map::MapInfo& info, const map::TileHeights& heights, map::TileKey tile, const float lo[2],
                const float hi[2]) {
    Geometry g;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) {
            const map::TileKey t{tile.x + dx, tile.z + dz};
            if (!info.has_tile(t)) continue;
            const map::Heights& h = heights_of(heights, t);
            const float ox = t.x * map::kTileSize, oz = t.z * map::kTileSize;
            map::for_each_triangle([&](uint32_t i0, uint32_t i1, uint32_t i2) {
                // Every triangle lies in one 1 m quad whose corner is i0 (lowest u and v).
                const float qx = ox + float(i0 % map::kTileSamples), qz = oz + float(i0 / map::kTileSamples);
                if (qx + 1 < lo[0] || qx > hi[0] || qz + 1 < lo[1] || qz > hi[1]) return;
                for (uint32_t i : {i0, i1, i2}) {
                    g.verts.push_back(ox + float(i % map::kTileSamples));
                    g.verts.push_back(map::sample_height(h[i], info));
                    g.verts.push_back(oz + float(i / map::kTileSamples));
                    g.tris.push_back(int(g.tris.size()));
                }
            });
        }
    return g;
}

} // namespace

std::optional<TileBake> bake_tile(const map::MapInfo& info, const map::TileHeights& heights, map::TileKey tile,
                                  std::string& err, const std::vector<map::ZoneObject>& objects) {
    const map::NavParams nav = info.nav.value_or(map::NavParams{});
    rcConfig cfg{};
    cfg.cs = nav.cellSize;
    cfg.ch = nav.cellSize / 2; // ponytail: cell height fixed at cellSize / 2; a manifest field if maps need finer steps
    cfg.tileSize = int(std::lround(map::kTileSize / cfg.cs));
    if (std::fabs(float(cfg.tileSize) * cfg.cs - map::kTileSize) > 1e-3f) {
        err = "nav.cellSize " + std::to_string(nav.cellSize) + " must divide the 256 m tile";
        return std::nullopt;
    }
    cfg.walkableSlopeAngle = nav.maxSlopeDeg; // the client's CharacterVirtual max slope on terrain too
    cfg.walkableHeight = int(std::ceil(nav.agentHeight / cfg.ch));
    cfg.walkableClimb = int(std::floor(nav.maxClimb / cfg.ch));
    cfg.walkableRadius = int(std::ceil(nav.agentRadius / cfg.cs));
    cfg.maxEdgeLen = int(12 / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.detailSampleDist = cfg.cs * 6;
    cfg.detailSampleMaxError = cfg.ch;
    cfg.borderSize = cfg.walkableRadius + 3;
    cfg.width = cfg.height = cfg.tileSize + 2 * cfg.borderSize;
    const float border = float(cfg.borderSize) * cfg.cs;
    const auto range = map::height_range(info);
    cfg.bmin[0] = tile.x * map::kTileSize - border;
    cfg.bmin[1] = range[0] - 1;
    cfg.bmin[2] = tile.z * map::kTileSize - border;
    cfg.bmax[0] = (tile.x + 1) * map::kTileSize + border;
    cfg.bmax[1] = range[1] + nav.agentHeight + 1;
    cfg.bmax[2] = (tile.z + 1) * map::kTileSize + border;

    const float lo[2] = {cfg.bmin[0], cfg.bmin[2]}, hi[2] = {cfg.bmax[0], cfg.bmax[2]};
    const Geometry g = gather(info, heights, tile, lo, hi);
    const int nverts = int(g.verts.size() / 3), ntris = int(g.tris.size() / 3);

    rcContext ctx(false);
    const auto fail = [&](const char* step) {
        err = std::string("Recast: ") + step + " failed";
        return std::nullopt;
    };
    Owned<rcHeightfield, rcFreeHeightField> solid(rcAllocHeightfield());
    if (!solid || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
        return fail("heightfield");
    std::vector<unsigned char> areas(size_t(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, g.verts.data(), nverts, g.tris.data(), ntris, areas.data());
    if (!rcRasterizeTriangles(&ctx, g.verts.data(), nverts, g.tris.data(), areas.data(), ntris, *solid, cfg.walkableClimb))
        return fail("rasterize");
    // No rcFilterLedgeSpans: on a heightfield its steep-slope rule (walkable neighbours more than maxClimb apart) caps
    // slopes at atan(maxClimb / (2 * cellSize)), 27 degrees with the defaults, not maxSlopeDeg as the client's Jolt
    // character does. Cliffs are unwalkable anyway: their triangles are steeper than maxSlopeDeg.
    // Walkable neighbours connect only if within maxClimb: keep tan(maxSlopeDeg) * cellSize <= maxClimb.
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

    Owned<rcCompactHeightfield, rcFreeCompactHeightfield> chf(rcAllocCompactHeightfield());
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
        return fail("compact heightfield");
    solid.reset();
    // Before erosion, so the agent radius is kept clear around each object as round a cliff.
    // ponytail: the whole column under the footprint (bmin..bmax y); the shape's own y range if maps get bridges or caves.
    for (const map::ZoneObject& o : objects) {
        if (!o.shape) continue; // a visual
        if (o.shape->kind == map::Shape::Kind::Cylinder) {
            const float base[3] = {o.pos[0], cfg.bmin[1], o.pos[2]};
            rcMarkCylinderArea(&ctx, base, o.shape->radius, cfg.bmax[1] - cfg.bmin[1], RC_NULL_AREA, *chf);
            continue;
        }
        float verts[12];
        int n = 0;
        for (const auto& [x, z] : map::footprint(*o.shape, o.pos, o.yaw)) {
            verts[n++] = x;
            verts[n++] = 0;
            verts[n++] = z;
        }
        rcMarkConvexPolyArea(&ctx, verts, 4, cfg.bmin[1], cfg.bmax[1], RC_NULL_AREA, *chf);
    }
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) return fail("erode");
    if (!rcBuildDistanceField(&ctx, *chf)) return fail("distance field");
    if (!rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) return fail("regions");

    Owned<rcContourSet, rcFreeContourSet> cset(rcAllocContourSet());
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset)) return fail("contours");
    if (cset->nconts == 0) return TileBake{};
    Owned<rcPolyMesh, rcFreePolyMesh> pmesh(rcAllocPolyMesh());
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) return fail("poly mesh");
    Owned<rcPolyMeshDetail, rcFreePolyMeshDetail> dmesh(rcAllocPolyMeshDetail());
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
        return fail("detail mesh");
    if (pmesh->npolys == 0) return TileBake{};
    // The server refuses such a tile at load; say so now, before the rest of the map bakes.
    if (const int limit = map::max_navtile_polys(info.tile_count()); pmesh->npolys > limit) {
        err = std::to_string(pmesh->npolys) + " polygons, more than the " + std::to_string(limit) +
              " per tile 32-bit poly refs allow on a map of " + std::to_string(info.tile_count()) + " tiles";
        return std::nullopt;
    }
    for (int i = 0; i < pmesh->npolys; ++i) pmesh->flags[i] = pmesh->areas[i] == RC_WALKABLE_AREA ? 1 : 0;

    dtNavMeshCreateParams p{};
    p.verts = pmesh->verts;
    p.vertCount = pmesh->nverts;
    p.polys = pmesh->polys;
    p.polyAreas = pmesh->areas;
    p.polyFlags = pmesh->flags;
    p.polyCount = pmesh->npolys;
    p.nvp = pmesh->nvp;
    p.detailMeshes = dmesh->meshes;
    p.detailVerts = dmesh->verts;
    p.detailVertsCount = dmesh->nverts;
    p.detailTris = dmesh->tris;
    p.detailTriCount = dmesh->ntris;
    p.tileX = tile.x;
    p.tileY = tile.z;
    rcVcopy(p.bmin, pmesh->bmin);
    rcVcopy(p.bmax, pmesh->bmax);
    p.walkableHeight = nav.agentHeight;
    p.walkableRadius = nav.agentRadius;
    p.walkableClimb = nav.maxClimb;
    p.cs = cfg.cs;
    p.ch = cfg.ch;
    p.buildBvTree = true;
    unsigned char* data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&p, &data, &size)) {
        err = "Detour: dtCreateNavMeshData failed";
        return std::nullopt;
    }
    TileBake out{map::navtile_header(DT_NAVMESH_VERSION), pmesh->npolys};
    out.navtile.append(reinterpret_cast<const char*>(data), size_t(size));
    dtFree(data);
    return out;
}

std::vector<std::string> seam_errors(const map::MapInfo& info, const map::TileHeights& heights) {
    std::vector<std::string> errors;
    const auto name = [](map::TileKey t) { return std::to_string(t.x) + "_" + std::to_string(t.z); };
    for (const auto& r : info.tiles)
        for (int32_t tz = r.z0; tz <= r.z1; ++tz)
            for (int32_t tx = r.x0; tx <= r.x1; ++tx) {
                const map::TileKey t{tx, tz};
                const map::Heights& a = heights_of(heights, t);
                for (const map::TileKey n : {map::TileKey{tx + 1, tz}, map::TileKey{tx, tz + 1}}) {
                    if (!info.has_tile(n)) continue;
                    const map::Heights& b = heights_of(heights, n);
                    const bool alongX = n.x != tx; // the shared edge is a's column 256 = b's column 0
                    int bad = 0, worstAt = 0;
                    float worst = 0;
                    for (int i = 0; i < map::kTileSamples; ++i) {
                        const size_t ia = alongX ? size_t(i) * map::kTileSamples + 256 : size_t(256) * map::kTileSamples + i;
                        const size_t ib = alongX ? size_t(i) * map::kTileSamples : size_t(i);
                        if (a[ia] == b[ib]) continue;
                        ++bad;
                        const float d = std::fabs(map::sample_height(a[ia], info) - map::sample_height(b[ib], info));
                        if (d > worst) worst = d, worstAt = i;
                    }
                    if (bad == 0) continue;
                    char line[160];
                    std::snprintf(line, sizeof line, "seam %s | %s: %d samples differ, worst %.2f m at %s %d",
                                  name(t).c_str(), name(n).c_str(), bad, worst, alongX ? "z" : "x",
                                  int((alongX ? tz : tx) * map::kTileSize) + worstAt);
                    errors.push_back(line);
                }
            }
    return errors;
}

} // namespace chthonic::mapbake
