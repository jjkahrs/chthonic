#include "heights.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// stb_image for 16-bit PNGs, private to this file: STB_IMAGE_STATIC keeps its symbols out of the link, so it cannot
// clash with another stb_image implementation linked into the same program.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

namespace chthonic::map {
namespace {

constexpr size_t kSamples = size_t(kTileSamples) * kTileSamples;

std::optional<Heights> decode_r16(std::string_view bytes, std::string& err) {
    if (bytes.size() != kSamples * 2) {
        err = "r16: " + std::to_string(bytes.size()) + " bytes, need " + std::to_string(kSamples * 2) + " (257 x 257 x 2)";
        return std::nullopt;
    }
    Heights h(kSamples);
    for (size_t i = 0; i < kSamples; ++i)
        h[i] = uint16_t(uint8_t(bytes[2 * i]) | uint16_t(uint8_t(bytes[2 * i + 1])) << 8);
    return h;
}

std::optional<Heights> decode_png(std::string_view bytes, std::string& err) {
    const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int len = int(bytes.size());
    int w = 0, hgt = 0, comp = 0;
    if (!stbi_info_from_memory(data, len, &w, &hgt, &comp)) {
        err = std::string("png: ") + stbi_failure_reason();
        return std::nullopt;
    }
    if (w != kTileSamples || hgt != kTileSamples) {
        err = "png: " + std::to_string(w) + " x " + std::to_string(hgt) + ", need 257 x 257";
        return std::nullopt;
    }
    if (!stbi_is_16_bit_from_memory(data, len) || comp != 1) {
        err = std::string("png: ") + (stbi_is_16_bit_from_memory(data, len) ? "16" : "8") + "-bit with " +
              std::to_string(comp) + " channel(s), need 16-bit grayscale";
        return std::nullopt;
    }
    stbi_us* pixels = stbi_load_16_from_memory(data, len, &w, &hgt, &comp, 1);
    if (!pixels) {
        err = std::string("png: ") + stbi_failure_reason();
        return std::nullopt;
    }
    Heights h(pixels, pixels + kSamples);
    stbi_image_free(pixels);
    return h;
}

} // namespace

std::optional<Heights> decode_heights(std::string_view bytes, std::string_view ext, std::string& err) {
    if (ext == ".r16") return decode_r16(bytes, err);
    if (ext == ".png") return decode_png(bytes, err);
    err = "unknown heights format \"" + std::string(ext) + "\" (need .r16 or .png)";
    return std::nullopt;
}

std::array<float, 2> height_range(const MapInfo& info) { return info.heightRange.value_or(std::array<float, 2>{0, 0}); }

float sample_height(uint16_t s, const MapInfo& info) {
    const auto [lo, hi] = height_range(info);
    return lo + float(s) / 65535.f * (hi - lo);
}

float ground(const Heights& h, const MapInfo& info, float u, float v) {
    constexpr int n = kTileSamples;
    u = std::clamp(u, 0.f, kTileSize);
    v = std::clamp(v, 0.f, kTileSize);
    const int iu = std::min(int(u), n - 2), iv = std::min(int(v), n - 2);
    const float fu = u - float(iu), fv = v - float(iv);
    const auto at = [&](int du, int dv) { return sample_height(h[size_t(iv + dv) * n + size_t(iu + du)], info); };
    const float h00 = at(0, 0), h11 = at(1, 1);
    if (fv >= fu) { // triangle (u,v) (u,v+1) (u+1,v+1)
        const float h01 = at(0, 1);
        return h00 + fu * (h11 - h01) + fv * (h01 - h00);
    }
    const float h10 = at(1, 0); // triangle (u,v) (u+1,v+1) (u+1,v)
    return h00 + fu * (h10 - h00) + fv * (h11 - h10);
}

std::optional<float> ground_at(const MapInfo& info, const TileHeights& tiles, float x, float z) {
    const TileKey t = tile_of(x, z);
    if (!info.has_tile(t)) return std::nullopt;
    const auto it = tiles.find(t);
    if (it == tiles.end()) return height_range(info)[0];
    return ground(it->second, info, x - float(t.x) * kTileSize, z - float(t.z) * kTileSize);
}

std::array<float, 3> normal_at(const Heights& h, const MapInfo& info, int u, int v) {
    constexpr int n = kTileSamples;
    const auto at = [&](int x, int z) {
        return sample_height(h[size_t(std::clamp(z, 0, n - 1)) * n + size_t(std::clamp(x, 0, n - 1))], info);
    };
    const int u0 = std::max(u - 1, 0), u1 = std::min(u + 1, n - 1), v0 = std::max(v - 1, 0), v1 = std::min(v + 1, n - 1);
    const float dx = (at(u1, v) - at(u0, v)) / float(u1 - u0), dz = (at(u, v1) - at(u, v0)) / float(v1 - v0);
    const float len = std::sqrt(dx * dx + 1 + dz * dz);
    return {-dx / len, 1 / len, -dz / len};
}

} // namespace chthonic::map
