// chthonic_mapbake <mapDir> [--out <dir>]: lints a map folder (height seams, spawn, object and interactable files) and
// bakes a navtile per declared tile into <mapDir>/.server/nav (or --out), with its obstacles and interactables cut out.
// Exit 0: all baked; 1: lint or bake errors (nothing written); 2: usage.
#include "bake.h"
#include "crash_dialogs.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace chthonic;

namespace {

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

std::string tile_name(map::TileKey t) { return std::to_string(t.x) + "_" + std::to_string(t.z); }

// Every declared tile's heights file (.r16 first, as the server), decoded.
map::TileHeights load_heights(const fs::path& dir, const map::MapInfo& info, std::vector<std::string>& errors) {
    map::TileHeights out;
    for (const auto& r : info.tiles)
        for (int32_t tz = r.z0; tz <= r.z1; ++tz)
            for (int32_t tx = r.x0; tx <= r.x1; ++tx)
                for (const char* ext : {".r16", ".png"}) {
                    const fs::path file = dir / "heights" / (tile_name({tx, tz}) + ext);
                    const auto bytes = read_file(file);
                    if (!bytes) continue;
                    std::string err;
                    if (auto h = map::decode_heights(*bytes, ext, err)) out[{tx, tz}] = std::move(*h);
                    else errors.push_back(file.string() + ": " + err);
                    break;
                }
    return out;
}

// "<tx>_<tz>.json" -> the tile; nullopt for any other name.
std::optional<map::TileKey> tile_of_file(const fs::path& file) {
    static const std::regex re(R"((-?\d{1,9})_(-?\d{1,9})\.json)");
    std::smatch m;
    const std::string name = file.filename().string();
    if (!std::regex_match(name, m, re)) return std::nullopt;
    return map::TileKey{std::stoi(m[1]), std::stoi(m[2])};
}

// The spawn files, checked as the server loads them (map::parse_spawns, tile on the map, ids unique on the map).
void lint_spawns(const fs::path& dir, const map::MapInfo& info, std::vector<std::string>& errors) {
    std::error_code ec;
    std::set<std::string> ids;
    for (const auto& entry : fs::directory_iterator(dir / ".server" / "spawns", ec)) {
        const std::string where = entry.path().string();
        const auto tile = tile_of_file(entry.path());
        if (!tile) { errors.push_back(where + ": file name must be <tx>_<tz>.json"); continue; }
        const map::TileKey t = *tile;
        if (!info.has_tile(t)) { errors.push_back(where + ": tile is not on the map"); continue; }
        std::string err;
        const auto json = nlohmann::json::parse(read_file(entry.path()).value_or(""), nullptr, false);
        const auto spawns = json.is_discarded() ? std::nullopt : map::parse_spawns(json, t, err);
        if (!spawns) { errors.push_back(where + ": " + (json.is_discarded() ? "not valid JSON" : err)); continue; }
        for (const auto& s : *spawns)
            if (!ids.insert(s.id).second) errors.push_back(where + ": spawn \"" + s.id + "\": duplicate id on this map");
    }
}

// Every objects/<tx>_<tz>.json, parsed (map::parse_objects; any entry error is a lint error), by tile; interactable ids
// unique on the map.
std::map<std::pair<int32_t, int32_t>, std::vector<map::ZoneObject>> load_objects(const fs::path& dir, const map::MapInfo& info,
                                                                              std::vector<std::string>& errors) {
    std::map<std::pair<int32_t, int32_t>, std::vector<map::ZoneObject>> out;
    std::set<uint32_t> ids;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir / "objects", ec)) {
        if (entry.path().extension() == ".meta") continue; // an editor's asset id sidecar
        const std::string where = entry.path().string();
        const auto t = tile_of_file(entry.path());
        if (!t) { errors.push_back(where + ": file name must be <tx>_<tz>.json"); continue; }
        if (!info.has_tile(*t)) { errors.push_back(where + ": tile is not on the map"); continue; }
        const auto json = nlohmann::json::parse(read_file(entry.path()).value_or(""), nullptr, false);
        if (json.is_discarded()) { errors.push_back(where + ": not valid JSON"); continue; }
        map::ParsedObjects parsed = map::parse_objects(json, *t);
        for (const auto& e : parsed.errors) errors.push_back(where + ": " + e);
        for (const auto& o : parsed.objects)
            if (o.type == map::ObjectType::Interactable && !ids.insert(o.id).second)
                errors.push_back(where + ": interactable " + std::to_string(o.id) + ": duplicate id on this map");
        out[{t->x, t->z}] = std::move(parsed.objects);
    }
    return out;
}

// The server's interactables files, checked as the server loads them (map::parse_interactables, ids unique on the map).
void lint_interactables(const fs::path& dir, const map::MapInfo& info, std::vector<std::string>& errors) {
    std::error_code ec;
    std::set<uint32_t> ids;
    for (const auto& entry : fs::directory_iterator(dir / ".server" / "interactables", ec)) {
        const std::string where = entry.path().string();
        const auto t = tile_of_file(entry.path());
        if (!t) { errors.push_back(where + ": file name must be <tx>_<tz>.json"); continue; }
        if (!info.has_tile(*t)) { errors.push_back(where + ": tile is not on the map"); continue; }
        std::string err;
        const auto json = nlohmann::json::parse(read_file(entry.path()).value_or(""), nullptr, false);
        const auto list = json.is_discarded() ? std::nullopt : map::parse_interactables(json, *t, err);
        if (!list) { errors.push_back(where + ": " + (json.is_discarded() ? "not valid JSON" : err)); continue; }
        for (const auto& it : *list)
            if (!ids.insert(it.id).second)
                errors.push_back(where + ": interactable " + std::to_string(it.id) + ": duplicate id on this map");
    }
}

int run(const fs::path& dir, fs::path out) {
    std::vector<std::string> errors;
    std::string err;
    const auto text = read_file(dir / "map.json");
    const auto json = nlohmann::json::parse(text.value_or(""), nullptr, false);
    const auto info = json.is_discarded() ? std::nullopt : map::parse_manifest(json, err);
    if (!info) {
        std::fprintf(stderr, "%s: %s\n", (dir / "map.json").string().c_str(), text ? (err.empty() ? "not valid JSON" : err.c_str()) : "missing");
        return 1;
    }
    const map::TileHeights heights = load_heights(dir, *info, errors);
    for (auto& e : mapbake::seam_errors(*info, heights)) errors.push_back((dir / "heights").string() + ": " + e);
    lint_spawns(dir, *info, errors);
    lint_interactables(dir, *info, errors);
    const auto objects = load_objects(dir, *info, errors);
    if (!errors.empty()) {
        for (const auto& e : errors) std::fprintf(stderr, "error: %s\n", e.c_str());
        std::fprintf(stderr, "%zu error(s), nothing baked\n", errors.size());
        return 1;
    }

    if (out.empty()) out = dir / ".server" / "nav";
    std::vector<std::pair<std::string, std::string>> files; // written only once every tile baked
    for (const auto& r : info->tiles)
        for (int32_t tz = r.z0; tz <= r.z1; ++tz)
            for (int32_t tx = r.x0; tx <= r.x1; ++tx) {
                const auto t0 = std::chrono::steady_clock::now();
                std::vector<map::ZoneObject> nearby; // the tile's and its neighbours', for the bake border
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (const auto it = objects.find({tx + dx, tz + dz}); it != objects.end())
                            nearby.insert(nearby.end(), it->second.begin(), it->second.end());
                const auto tile = mapbake::bake_tile(*info, heights, {tx, tz}, err, nearby);
                const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const std::string name = tile_name({tx, tz}) + ".navtile";
                if (!tile) { std::fprintf(stderr, "error: %s: %s\n", name.c_str(), err.c_str()); return 1; }
                std::printf("%s: %d polys, %zu bytes, %.2f s%s\n", name.c_str(), tile->polys, tile->navtile.size(), sec,
                            tile->navtile.empty() ? " (nothing walkable: no file)" : "");
                if (!tile->navtile.empty()) files.emplace_back(name, tile->navtile);
            }
    std::error_code ec;
    fs::create_directories(out, ec);
    for (const auto& [name, bytes] : files) {
        std::ofstream f(out / name, std::ios::binary | std::ios::trunc);
        if (!f.write(bytes.data(), std::streamsize(bytes.size()))) {
            std::fprintf(stderr, "error: cannot write %s\n", (out / name).string().c_str());
            return 1;
        }
    }
    std::printf("%zu navtile(s) -> %s\n", files.size(), out.string().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    chthonic::disableCrashDialogsIfRequested();
    fs::path dir, out;
    bool usage = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (dir.empty() && !a.starts_with("--")) dir = a;
        else usage = true;
    }
    if (usage || dir.empty()) {
        std::fprintf(stderr, "usage: chthonic_mapbake <mapDir> [--out <dir>]\n");
        return 2;
    }
    return run(dir, out);
}
