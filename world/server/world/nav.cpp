#include "nav.h"

#include <navtile.h>

#include <DetourAlloc.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace chthonic::world {
namespace {
constexpr int kMaxNodes = 4096;       // A* search nodes per query
constexpr int kMaxPathPolys = 1024;   // polygons in a path corridor
constexpr int kMaxPathPoints = 256;   // corners in a straight path
constexpr float kOnMeshRadius = 0.5f; // m across: covers the bake's agentRadius erosion at walkable edges
} // namespace

struct Nav::Mesh {
    dtNavMesh* mesh = dtAllocNavMesh();
    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    dtQueryFilter filter; // every polygon the bake flagged walkable (flag 1)
    int maxPolys = 0;
    // findPath uses the query's node pool: one search at a time (map contexts update on several threads, and a
    // chthonic path may ask for another map's). findNearestPoly and findStraightPath only read.
    // ponytail: one lock per map; a dtNavMeshQuery per context if cross-map paths ever contend.
    std::mutex pathMu;
    ~Mesh() {
        dtFreeNavMeshQuery(query);
        dtFreeNavMesh(mesh);
    }

    // The nearest polygon to p within halfExtents, and the nearest point on it; 0 if none.
    dtPolyRef nearest(const proto::Vec3& p, const float* halfExtents, float* onPoly) const {
        const float c[3] = {p.x, p.y, p.z};
        dtPolyRef ref = 0;
        query->findNearestPoly(c, halfExtents, &filter, &ref, onPoly);
        return ref;
    }
};

Nav::Nav() = default;
Nav::~Nav() = default;
Nav::Nav(Nav&&) noexcept = default;
Nav& Nav::operator=(Nav&&) noexcept = default;

bool Nav::add_tile(uint16_t map, int tileCount, map::TileKey tile, std::string_view bytes, std::string& err) {
    const auto header = map::parse_navtile_header(bytes, err);
    if (!header) return false;
    if (header->version != map::kNavtileVersion) {
        err = "navtile version " + std::to_string(header->version) + ", this server reads " +
              std::to_string(map::kNavtileVersion) + " (rebake with chthonic_mapbake)";
        return false;
    }
    if (header->dtVersion != DT_NAVMESH_VERSION) {
        err = "Detour version " + std::to_string(header->dtVersion) + ", this server has " +
              std::to_string(DT_NAVMESH_VERSION) + " (rebake with chthonic_mapbake)";
        return false;
    }
    const std::string_view blob = bytes.substr(map::kNavtileHeaderBytes);
    dtMeshHeader dt{};
    if (blob.size() < sizeof dt) { err = "truncated Detour tile"; return false; }
    std::memcpy(&dt, blob.data(), sizeof dt);
    if (dt.magic != DT_NAVMESH_MAGIC || dt.version != DT_NAVMESH_VERSION) {
        err = "not a Detour tile (bad tile magic or version)";
        return false;
    }
    if (dt.x != tile.x || dt.y != tile.z) {
        err = "baked for tile " + std::to_string(dt.x) + "_" + std::to_string(dt.y);
        return false;
    }

    std::unique_ptr<Mesh> fresh;
    Mesh* m = meshes_.contains(map) ? meshes_.at(map).get() : nullptr;
    if (!m) {
        fresh = std::make_unique<Mesh>();
        m = fresh.get();
        m->maxPolys = map::max_navtile_polys(tileCount); // 32-bit poly refs: many tiles leave few polys
        dtNavMeshParams p{};
        p.tileWidth = p.tileHeight = map::kTileSize; // orig (0, 0, 0): Detour's tile (x, y) is our tile (x, z)
        p.maxTiles = std::max(tileCount, 1);
        p.maxPolys = m->maxPolys;
        if (!m->mesh || !m->query || dtStatusFailed(m->mesh->init(&p)) || dtStatusFailed(m->query->init(m->mesh, kMaxNodes))) {
            err = "Detour: navmesh init failed (" + std::to_string(tileCount) + " tiles)";
            return false;
        }
    }
    if (dt.polyCount > m->maxPolys) {
        err = std::to_string(dt.polyCount) + " polygons, more than the " + std::to_string(m->maxPolys) +
              " per tile 32-bit poly refs allow on a map of " + std::to_string(tileCount) + " tiles";
        return false;
    }
    auto* data = static_cast<unsigned char*>(dtAlloc(int(blob.size()), DT_ALLOC_PERM));
    std::memcpy(data, blob.data(), blob.size());
    if (const dtStatus s = m->mesh->addTile(data, int(blob.size()), DT_TILE_FREE_DATA, 0, nullptr); dtStatusFailed(s)) {
        dtFree(data);
        err = "Detour: addTile failed (status " + std::to_string(s) + ")";
        return false;
    }
    if (fresh) meshes_[map] = std::move(fresh);
    return true;
}

bool Nav::has(uint16_t map) const { return meshes_.contains(map); }

std::vector<proto::Vec3> Nav::path(uint16_t map, proto::Vec3 from, proto::Vec3 to) const {
    const auto it = meshes_.find(map);
    if (it == meshes_.end()) return {};
    Mesh& m = *it->second;
    static constexpr float kSnap[3] = {2, 4, 2};
    float start[3], end[3];
    const dtPolyRef startRef = m.nearest(from, kSnap, start), endRef = m.nearest(to, kSnap, end);
    // The box finds polygons whose bounds touch it; the snapped points must be within it across too.
    const auto near = [](const float* p, const proto::Vec3& q) { return std::hypot(p[0] - q.x, p[2] - q.z) <= kSnap[0]; };
    if (!startRef || !endRef || !near(start, from) || !near(end, to)) return {};
    dtPolyRef polys[kMaxPathPolys];
    int npolys = 0;
    dtStatus s;
    {
        std::lock_guard lk(m.pathMu);
        s = m.query->findPath(startRef, endRef, start, end, &m.filter, polys, &npolys, kMaxPathPolys);
    }
    if (dtStatusFailed(s) || npolys == 0 || polys[npolys - 1] != endRef) return {}; // partial: `to` is unreachable
    float points[kMaxPathPoints * 3];
    int npoints = 0;
    if (dtStatusFailed(m.query->findStraightPath(start, end, polys, npolys, points, nullptr, nullptr, &npoints,
                                                 kMaxPathPoints)))
        return {};
    std::vector<proto::Vec3> out;
    for (int i = 0; i < npoints; ++i) out.push_back({points[3 * i], points[3 * i + 1], points[3 * i + 2]});
    return out;
}

bool Nav::on_mesh(uint16_t map, proto::Vec3 pos, float maxAir) const {
    const auto it = meshes_.find(map);
    if (it == meshes_.end()) return false;
    const float extents[3] = {kOnMeshRadius, std::max(maxAir, 1.0f), kOnMeshRadius};
    float at[3];
    if (!it->second->nearest(pos, extents, at)) return false;
    // The box finds polygons whose bounds touch it; the nearest point on them must be within the radius too.
    return std::hypot(at[0] - pos.x, at[2] - pos.z) <= kOnMeshRadius;
}

} // namespace chthonic::world
