// Tile heightmaps (<map>/heights/<tx>_<tz>.r16 or .png) and the ground they describe. Pure: bytes in, no file IO.
// A tile has kTileSamples x kTileSamples samples, 1 m apart, row-major with rows along +z ([v][u]); column 256 of
// tile tx is column 0 of tile tx+1. Sample s is at y = heightMin + s / 65535 * (heightMax - heightMin).
// The ground is each 1 m quad split along the (u, v)-(u+1, v+1) diagonal, the same triangles as Jolt's
// HeightFieldShape, so the renderer, client physics, server checks and the navmesh bake all agree.
#pragma once
#include "map.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chthonic::map {

using Heights = std::vector<uint16_t>; // kTileSamples * kTileSamples samples, index v * kTileSamples + u

// A tile with no heights file: every sample 0 (flat at heightMin).
inline Heights flat_heights() { return Heights(size_t(kTileSamples) * kTileSamples, 0); }

// ext ".r16": exactly 257 * 257 little-endian uint16. ".png": a 257 x 257 16-bit grayscale PNG. nullopt with err set
// ("png: 8-bit, need 16-bit grayscale") otherwise.
std::optional<Heights> decode_heights(std::string_view bytes, std::string_view ext, std::string& err);

// The map's [heightMin, heightMax]; [0, 0] if it declares none.
std::array<float, 2> height_range(const MapInfo& info);

// Sample s in metres.
float sample_height(uint16_t s, const MapInfo& info);

// Height of the ground at tile-local (u, v), clamped to [0, 256]: the triangle under the point, not bilinear.
float ground(const Heights& h, const MapInfo& info, float u, float v);

// A map's loaded tiles' heights. A tile of the map with no entry has no heights file: flat at heightMin.
using TileHeights = std::unordered_map<TileKey, Heights, TileKeyHash>;

// Height of the ground at map (x, z): ground() on the tile under the point; nullopt if (x, z) is not on the map.
std::optional<float> ground_at(const MapInfo& info, const TileHeights& tiles, float x, float z);

// Smooth unit normal at sample (u, v) from its neighbours (one-sided at the tile edge), for vertex shading.
// ponytail: edge normals ignore the neighbour tile, so lighting can show a faint seam on steep borders; pass the
// neighbours' heights in if that shows.
std::array<float, 3> normal_at(const Heights& h, const MapInfo& info, int u, int v);

// Calls fn(i0, i1, i2) with sample indices for every ground triangle of a tile, two per quad, counter-clockwise seen
// from above (+y), in Jolt's order and diagonal.
template <typename Fn>
void for_each_triangle(Fn&& fn) {
    constexpr uint32_t n = kTileSamples;
    for (uint32_t v = 0; v + 1 < n; ++v)
        for (uint32_t u = 0; u + 1 < n; ++u) {
            const uint32_t i00 = v * n + u, i10 = i00 + 1, i01 = i00 + n, i11 = i01 + 1;
            fn(i00, i01, i11);
            fn(i00, i11, i10);
        }
}

} // namespace chthonic::map
