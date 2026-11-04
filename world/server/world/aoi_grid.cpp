#include "aoi_grid.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace chthonic {

CellKey AoiGrid::cell_of(uint16_t map, float x, float z) const {
    return {map, static_cast<int32_t>(std::floor(x / cell_)), static_cast<int32_t>(std::floor(z / cell_))};
}

void AoiGrid::insert(uint32_t id, CellKey c) {
    cells_[pack(c)].push_back(id);
    touch(c);
}

void AoiGrid::remove(uint32_t id, CellKey c) {
    touch(c);
    auto it = cells_.find(pack(c));
    if (it == cells_.end()) return;
    auto& v = it->second;
    if (auto pos = std::find(v.begin(), v.end(), id); pos != v.end()) {
        *pos = v.back();
        v.pop_back();
    }
    if (v.empty()) cells_.erase(it);
}

bool AoiGrid::touched_near(uint16_t map, float x, float z) {
    if (touched_.empty()) return false;
    if (!sorted_) {
        std::sort(touched_.begin(), touched_.end());
        touched_.erase(std::unique(touched_.begin(), touched_.end()), touched_.end());
        sorted_ = true;
    }
    const auto [lo, hi] = window(map, x, z);
    for (int32_t cx = lo.cx; cx <= hi.cx; ++cx)
        for (int32_t cz = lo.cz; cz <= hi.cz; ++cz)
            if (std::binary_search(touched_.begin(), touched_.end(), pack({map, cx, cz}))) return true;
    return false;
}

void diff_sorted(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, std::vector<uint32_t>& entered,
                 std::vector<uint32_t>& left) {
    entered.clear();
    left.clear();
    std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(entered));
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(left));
}

} // namespace chthonic
