#include "maps.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace chthonic::world {
namespace {

// The file's bytes; nullopt if it does not exist or cannot be read.
std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream bytes;
    bytes << in.rdbuf();
    return bytes.str();
}

// The JSON in path; nullopt with err set if it is missing or malformed.
std::optional<nlohmann::json> read_json(const std::filesystem::path& path, std::string& err) {
    const auto text = read_file(path);
    if (!text) { err = "missing"; return std::nullopt; }
    auto json = nlohmann::json::parse(*text, nullptr, false);
    if (json.is_discarded()) { err = "not valid JSON"; return std::nullopt; }
    return json;
}

// A portal's destination must be on a loaded map's tiles and not inside a portal there (no instant bounce).
std::optional<std::string> portal_error(const Maps& maps, const map::Area& area) {
    const map::Portal& p = *area.portal;
    const std::string at = "area \"" + area.id + "\": portal ";
    const auto* to = maps.by_id(p.map);
    if (!to) return at + "to unknown map " + std::to_string(p.map);
    if (!to->contains(p.pos[0], p.pos[2])) return at + "destination is not on a tile of map " + std::to_string(p.map);
    if (const int16_t i = map::area_at(to->areas, p.pos[0], p.pos[1], p.pos[2]); i >= 0 && to->areas.list[i].portal)
        return at + "destination is inside portal area \"" + to->areas.list[i].id + "\" of map " + std::to_string(p.map);
    return std::nullopt;
}

// "<tx>_<tz>.json" -> the tile; nullopt for any other name (spawn and interactables files).
std::optional<map::TileKey> spawn_file_tile(const std::string& name) {
    static const std::regex re(R"((-?\d{1,9})_(-?\d{1,9})\.json)");
    std::smatch m;
    if (!std::regex_match(name, m, re)) return std::nullopt;
    return map::TileKey{std::stoi(m[1]), std::stoi(m[2])};
}

} // namespace

void Maps::load_heights(uint16_t id, const std::filesystem::path& folder, std::vector<std::string>& errors) {
    for (const map::TileRect& r : maps_.at(id).tiles)
        for (int32_t tz = r.z0; tz <= r.z1; ++tz)
            for (int32_t tx = r.x0; tx <= r.x1; ++tx) {
                if (!owns(id, {tx, tz})) continue; // another process runs it
                // .r16 first, as the client's terrain does.
                for (const char* ext : {".r16", ".png"}) {
                    const std::string name = std::to_string(tx) + "_" + std::to_string(tz) + ext;
                    const auto bytes = read_file(folder / "heights" / name);
                    if (!bytes) continue;
                    std::string err;
                    if (auto h = map::decode_heights(*bytes, ext, err)) set_heights(id, {tx, tz}, std::move(*h));
                    else errors.push_back("maps/" + folder.filename().string() + "/heights/" + name + ": " + err);
                    break;
                }
            }
}

void Maps::load_nav(uint16_t id, const std::filesystem::path& folder, std::vector<std::string>& errors) {
    for (const map::TileRect& r : maps_.at(id).tiles)
        for (int32_t tz = r.z0; tz <= r.z1; ++tz)
            for (int32_t tx = r.x0; tx <= r.x1; ++tx) {
                if (!owns(id, {tx, tz})) continue; // another process runs it
                const std::string name = std::to_string(tx) + "_" + std::to_string(tz) + ".navtile";
                const auto bytes = read_file(folder / ".server" / "nav" / name);
                if (!bytes) continue; // not baked, or nothing walkable on it
                std::string err;
                if (!add_navtile(id, {tx, tz}, *bytes, err))
                    errors.push_back("maps/" + folder.filename().string() + "/.server/nav/" + name + ": " + err);
            }
}

bool Maps::add_navtile(uint16_t map, map::TileKey tile, std::string_view bytes, std::string& err) {
    return nav_.add_tile(map, info(map).tile_count(), tile, bytes, err);
}

void Maps::load_spawns(uint16_t id, const std::filesystem::path& folder, std::vector<std::string>& errors) {
    const std::filesystem::path dir = folder / ".server" / "spawns";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return; // no spawns
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) files.push_back(entry.path());
    std::sort(files.begin(), files.end()); // stable order: errors and spawn records
    map::MapInfo& info = maps_.at(id);
    std::set<std::string> ids;
    for (const auto& file : files) {
        const std::string where = "maps/" + folder.filename().string() + "/.server/spawns/" + file.filename().string();
        const auto tile = spawn_file_tile(file.filename().string());
        if (!tile) { errors.push_back(where + ": file name must be <tx>_<tz>.json"); continue; }
        if (!info.has_tile(*tile)) { errors.push_back(where + ": tile is not on the map"); continue; }
        if (!owns(id, *tile)) continue; // another process runs it
        std::string err;
        const auto json = read_json(file, err);
        auto spawns = json ? map::parse_spawns(*json, *tile, err) : std::nullopt;
        if (!spawns) { errors.push_back(where + ": " + err); continue; }
        for (auto& spawn : *spawns) {
            if (!ids.insert(spawn.id).second) errors.push_back(where + ": spawn \"" + spawn.id + "\": duplicate id on this map");
            else {
                spawn.file = where;
                info.spawns.push_back(std::move(spawn));
            }
        }
    }
    if (ec) errors.push_back("maps/" + folder.filename().string() + "/.server/spawns: " + ec.message());
}

void Maps::load_interactables(uint16_t id, const std::filesystem::path& folder, std::vector<std::string>& errors) {
    const std::filesystem::path dir = folder / ".server" / "interactables";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return; // none
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) files.push_back(entry.path());
    std::sort(files.begin(), files.end()); // stable error order
    const map::MapInfo& info = maps_.at(id);
    for (const auto& file : files) {
        const std::string where = "maps/" + folder.filename().string() + "/.server/interactables/" + file.filename().string();
        const auto tile = spawn_file_tile(file.filename().string());
        if (!tile) { errors.push_back(where + ": file name must be <tx>_<tz>.json"); continue; }
        if (!info.has_tile(*tile)) { errors.push_back(where + ": tile is not on the map"); continue; }
        if (!owns(id, *tile)) continue; // another process runs it
        std::string err;
        const auto json = read_json(file, err);
        auto list = json ? map::parse_interactables(*json, *tile, err) : std::nullopt;
        if (!list) { errors.push_back(where + ": " + err); continue; }
        for (auto& it : *list) {
            if (interactable(id, it.id))
                errors.push_back(where + ": interactable " + std::to_string(it.id) + ": duplicate id on this map");
            else add_interactable(id, std::move(it));
        }
    }
    if (ec) errors.push_back("maps/" + folder.filename().string() + "/.server/interactables: " + ec.message());
}

const map::Interactable* Maps::interactable(uint16_t map, uint32_t id) const {
    const auto m = interactables_.find(map);
    if (m == interactables_.end()) return nullptr;
    const auto it = m->second.find(id);
    return it == m->second.end() ? nullptr : &it->second;
}

Maps Maps::load(const std::filesystem::path& root, std::vector<std::string>& errors) {
    Maps maps;
    std::error_code ec;
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        if (entry.is_directory() && !entry.path().filename().string().starts_with('.')) folders.push_back(entry.path());
    if (ec) {
        errors.push_back("MAP_ROOT " + root.string() + ": " + ec.message());
        return maps;
    }
    std::sort(folders.begin(), folders.end()); // stable error order
    std::map<uint16_t, std::string> folderOf;
    for (const auto& folder : folders) {
        const std::string where = "maps/" + folder.filename().string() + "/map.json";
        std::string err;
        const auto json = read_json(folder / "map.json", err);
        if (!json) { errors.push_back(where + ": " + err); continue; }
        auto info = map::parse_manifest(*json, err);
        if (!info) { errors.push_back(where + ": " + err); continue; }
        if (std::filesystem::exists(folder / "areas.json")) {
            const std::string areasWhere = "maps/" + folder.filename().string() + "/areas.json";
            const auto areasJson = read_json(folder / "areas.json", err);
            auto areas = areasJson ? map::parse_areas(*areasJson, err) : std::nullopt;
            if (!areas) { errors.push_back(areasWhere + ": " + err); continue; }
            info->areas = std::move(*areas);
        }
        if (auto [it, fresh] = folderOf.emplace(info->id, folder.filename().string()); !fresh) {
            errors.push_back(where + ": id " + std::to_string(info->id) + " already used by maps/" + it->second);
            continue;
        }
        const uint16_t id = info->id;
        maps.add(std::move(*info));
        maps.load_spawns(id, folder, errors);
        maps.load_heights(id, folder, errors);
        maps.load_nav(id, folder, errors);
        maps.load_interactables(id, folder, errors);
    }
    // Portals once every map is in (a bad one is reported; its map stays loaded, but any error stops the server).
    for (const auto& [id, info] : maps.maps_)
        for (const auto& area : info.areas.list)
            if (area.portal)
                if (auto err = portal_error(maps, area))
                    errors.push_back("maps/" + folderOf.at(id) + "/areas.json: " + *err);
    // Instance exits likewise (map::exit_error).
    for (const auto& [id, info] : maps.maps_)
        if (auto err = map::exit_error(info, maps.by_id(info.exit ? info.exit->map : 0)))
            errors.push_back("maps/" + folderOf.at(id) + "/map.json: " + *err);
    if (maps.maps_.empty() && errors.empty()) errors.push_back("MAP_ROOT " + root.string() + ": no maps");
    return maps;
}

const map::MapInfo* Maps::by_id(uint16_t id) const {
    auto it = maps_.find(id);
    return it == maps_.end() ? nullptr : &it->second;
}

bool Maps::has_tile(uint16_t map, float x, float z) const {
    const auto* m = by_id(map);
    return m && m->contains(x, z);
}

bool Maps::owns(uint16_t map, map::TileKey tile) const {
    const auto* m = by_id(map);
    return m && m->has_tile(tile);
}

std::optional<float> Maps::ground(uint16_t map, float x, float z) const {
    const auto* m = by_id(map);
    if (!m) return std::nullopt;
    static const map::TileHeights none;
    const auto it = heights_.find(map);
    return map::ground_at(*m, it == heights_.end() ? none : it->second, x, z);
}

} // namespace chthonic::world
