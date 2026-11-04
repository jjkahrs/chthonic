// Navtile files (<map>/.server/nav/<tx>_<tz>.navtile, baked by tools/mapbake, loaded by the server's nav.cpp): this
// 12-byte little-endian header, then one Detour tile (dtCreateNavMeshData) for tile (tx, tz) of the map.
#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace chthonic::map {

constexpr char kNavtileMagic[4] = {'K', 'N', 'A', 'V'};
constexpr uint32_t kNavtileVersion = 1; // bump when the bake's input or layout changes meaning
constexpr size_t kNavtileHeaderBytes = 12;

struct NavtileHeader {
    uint32_t version = kNavtileVersion;
    uint32_t dtVersion = 0; // DT_NAVMESH_VERSION of the Detour that baked it
};

// Polygons one navtile may have on a map of tileCount tiles, for the server's dtNavMesh (nav.cpp) and the bake's check
// (mapbake) alike. 32-bit dtPolyRef: tile bits (log2 of tileCount rounded up to a power of 2) + poly bits <= 22, as
// Detour keeps >= 10 salt bits: 65,536 at 8 x 8 tiles, 16,384 at 16 x 16, 4,096 at 32 x 32, 1,024 at 64 x 64.
// ponytail: 32-bit refs; DT_POLYREF64 (Detour rebuilt, this formula changed) if baked tiles outgrow this.
constexpr int max_navtile_polys(int tileCount) {
    const int tileBits = std::bit_width(std::bit_ceil(unsigned(std::max(tileCount, 1)))) - 1;
    return 1 << std::max(22 - tileBits, 0);
}

inline std::string navtile_header(uint32_t dtVersion) {
    std::string out(kNavtileMagic, 4);
    for (uint32_t v : {kNavtileVersion, dtVersion})
        for (int i = 0; i < 4; ++i) out += char((v >> (8 * i)) & 0xff);
    return out;
}

// The header of a navtile's bytes; nullopt with err set if it is too short or not a navtile. Versions are the reader's
// to check.
inline std::optional<NavtileHeader> parse_navtile_header(std::string_view bytes, std::string& err) {
    if (bytes.size() < kNavtileHeaderBytes || std::memcmp(bytes.data(), kNavtileMagic, 4) != 0) {
        err = "not a navtile (bad magic, want 'KNAV')";
        return std::nullopt;
    }
    const auto u32 = [&](size_t at) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(bytes[at + i])) << (8 * i);
        return v;
    };
    return NavtileHeader{u32(4), u32(8)};
}

} // namespace chthonic::map
