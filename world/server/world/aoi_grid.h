// Uniform grid for interest management: which ids are within view radius of a point (x/z plane) on the same map.
// Maps share one grid; the map is part of every cell key, so entities on different maps never meet.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chthonic {

struct CellKey {
    uint16_t map = 0;
    int32_t cx = 0, cz = 0;
    bool operator==(const CellKey&) const = default;
};

class AoiGrid {
public:
    AoiGrid(float cellSize, float viewRadius) : cell_(cellSize), radius_(viewRadius) {}

    CellKey cell_of(uint16_t map, float x, float z) const;
    // insert and remove also touch c.
    void insert(uint32_t id, CellKey c);
    void remove(uint32_t id, CellKey c);
    // out = ids on map within view radius of (x, z), sorted ascending; `self` is excluded. posOf(id, x, z) gives
    // current positions. out keeps its capacity: no allocation once it is big enough.
    template <typename PosOf>
    void query(uint16_t map, float x, float z, uint32_t self, PosOf&& posOf, std::vector<uint32_t>& out) const;

    // fn(id) for every id in the cells overlapping [x0, x1) x [z0, z1) on map; a cell straddling the edge may add ids
    // just outside it.
    template <typename Fn>
    void for_each_in(uint16_t map, float x0, float z0, float x1, float z1, Fn&& fn) const {
        const CellKey lo = cell_of(map, x0, z0), hi = cell_of(map, std::nextafter(x1, x0), std::nextafter(z1, z0));
        for (int32_t cx = lo.cx; cx <= hi.cx; ++cx)
            for (int32_t cz = lo.cz; cz <= hi.cz; ++cz)
                if (auto it = cells_.find(pack({map, cx, cz})); it != cells_.end())
                    for (uint32_t id : it->second) fn(id);
    }

    // Touched cells: every cell an insert / remove / touch hit since the last clear_touched. A view query at (x, z)
    // can only change if a cell of its window was touched (or the viewer itself moved, which touches its own cell).
    void touch(CellKey c) {
        touched_.push_back(pack(c));
        sorted_ = false;
    }
    // Whether any cell query(map, x, z, ...) scans was touched.
    bool touched_near(uint16_t map, float x, float z);
    void clear_touched() { touched_.clear(); }

private:
    // The cells query scans: lo..hi inclusive on both axes.
    std::pair<CellKey, CellKey> window(uint16_t map, float x, float z) const {
        return {cell_of(map, x - radius_, z - radius_), cell_of(map, x + radius_, z + radius_)};
    }
    // map:16 | cx:24 | cz:24 (cells wrap past +-2^23, far beyond any map at a sane cell size).
    static uint64_t pack(CellKey c) {
        return (uint64_t(c.map) << 48) | (uint64_t(uint32_t(c.cx) & 0xFFFFFF) << 24) | (uint32_t(c.cz) & 0xFFFFFF);
    }
    float cell_, radius_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
    // ponytail: a flat vector (sorted + deduplicated on the first touched_near), so it allocates only while it grows
    // past its high-water mark; a per-cell flag if touched sets ever get large.
    std::vector<uint64_t> touched_;
    bool sorted_ = true;
};

// a and b sorted ascending. entered = b \ a, left = a \ b.
void diff_sorted(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, std::vector<uint32_t>& entered,
                 std::vector<uint32_t>& left);

// ids sorted ascending: adds id if absent / removes it if present, keeping the order.
inline void insert_sorted(std::vector<uint32_t>& ids, uint32_t id) {
    if (auto it = std::lower_bound(ids.begin(), ids.end(), id); it == ids.end() || *it != id) ids.insert(it, id);
}
inline void erase_sorted(std::vector<uint32_t>& ids, uint32_t id) {
    if (auto it = std::lower_bound(ids.begin(), ids.end(), id); it != ids.end() && *it == id) ids.erase(it);
}

template <typename PosOf>
void AoiGrid::query(uint16_t map, float x, float z, uint32_t self, PosOf&& posOf, std::vector<uint32_t>& out) const {
    out.clear();
    const auto [lo, hi] = window(map, x, z);
    const float r2 = radius_ * radius_;
    for (int32_t cx = lo.cx; cx <= hi.cx; ++cx)
        for (int32_t cz = lo.cz; cz <= hi.cz; ++cz) {
            auto it = cells_.find(pack({map, cx, cz}));
            if (it == cells_.end()) continue;
            for (uint32_t id : it->second) {
                if (id == self) continue;
                float ox = 0, oz = 0;
                posOf(id, ox, oz);
                if ((ox - x) * (ox - x) + (oz - z) * (oz - z) <= r2) out.push_back(id);
            }
        }
    std::sort(out.begin(), out.end());
}

} // namespace chthonic
