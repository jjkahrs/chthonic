// Every map under MAP_ROOT (<root>/<folder>/map.json + areas.json, see map/map.h), loaded once at startup.
#pragma once
#include "nav.h"

#include <heights.h>
#include <map.h>
#include <objects.h>

#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace chthonic::world {

class Maps {
public:
    // Every map.json (and its optional areas.json) under root; dot-folders are skipped. Then the spawn files of every
    // owned tile (<folder>/.server/spawns/<tx>_<tz>.json, map::parse_spawns) into MapInfo::spawns, its heights
    // (<folder>/heights/<tx>_<tz>.r16 or .png, map::decode_heights) and its navtile
    // (<folder>/.server/nav/<tx>_<tz>.navtile, Nav::add_tile; a map with none has no navmesh). Errors (one line each) on
    // a bad or missing manifest, a bad areas.json, a duplicate id, no maps at all, a portal to an unknown map, off its
    // tiles, or into another portal, a spawn file with a bad name, for a tile not on the map or that fails to parse, a
    // spawn id used twice on a map, a heights file that fails to decode, a navtile Nav rejects, or an interactables
    // file (<folder>/.server/interactables/<tx>_<tz>.json, map::parse_interactables) with a bad name, for a tile not
    // on the map, that fails to parse or repeats an id used on the map, or an instance map whose "exit" is not on a
    // loaded world map's tiles or is inside a portal there (map::exit_error). A map with a bad manifest or areas.json
    // is not loaded.
    static Maps load(const std::filesystem::path& root, std::vector<std::string>& errors);
    void add(map::MapInfo info) { maps_[info.id] = std::move(info); } // tests and load

    const map::MapInfo* by_id(uint16_t id) const;
    const map::MapInfo& info(uint16_t id) const { return maps_.at(id); } // id must be loaded
    const std::map<uint16_t, map::MapInfo>& all() const { return maps_; }
    bool has_tile(uint16_t map, float x, float z) const;
    // The seam for multi-process: whether this process runs the tile. Every tile of every loaded map, for now.
    bool owns(uint16_t map, map::TileKey tile) const;
    // Height of the ground at (x, z) on the map (map::ground_at: flat at heightMin on a tile with no heights file);
    // nullopt off the map.
    std::optional<float> ground(uint16_t map, float x, float z) const;
    void set_heights(uint16_t map, map::TileKey tile, map::Heights h) { heights_[map][tile] = std::move(h); } // tests
    // The baked navmeshes (Nav::path, Nav::on_mesh).
    const Nav& nav() const { return nav_; }
    // Nav::add_tile for a loaded map (load and tests).
    bool add_navtile(uint16_t map, map::TileKey tile, std::string_view bytes, std::string& err);
    // The interactable with this id on the map (an owned tile's), or null.
    const map::Interactable* interactable(uint16_t map, uint32_t id) const;
    void add_interactable(uint16_t map, map::Interactable it) { interactables_[map][it.id] = std::move(it); } // tests and load

private:
    void load_spawns(uint16_t id, const std::filesystem::path& folder, std::vector<std::string>& errors);
    void load_heights(uint16_t id, const std::filesystem::path& folder, std::vector<std::string>& errors);
    void load_nav(uint16_t id, const std::filesystem::path& folder, std::vector<std::string>& errors);
    void load_interactables(uint16_t id, const std::filesystem::path& folder, std::vector<std::string>& errors);

    std::map<uint16_t, map::MapInfo> maps_;
    std::map<uint16_t, map::TileHeights> heights_; // owned tiles with a heights file
    Nav nav_;
    std::map<uint16_t, std::unordered_map<uint32_t, map::Interactable>> interactables_; // map -> id -> one on an owned tile
};

} // namespace chthonic::world
