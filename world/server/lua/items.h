// Item definition files (LUA_ROOT/items/**/*.lua), one item each. The client loads the same files
// (chthonic.items), so both sides agree on names, stack sizes and qualities. See docs/world/lua.html#items.
// Only lua/*.cpp include this (sol2 stays out of every other header).
#pragma once
#include "components.h"

#include <sol/sol.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace chthonic::lua {

constexpr const char* kItemsDir = "items";

// By id: {"id", "name", "icon", "quality", "maxStack", "sellPrice", "description"}, every field checked.
using ItemDefs = std::map<std::string, Json, std::less<>>;

struct ItemDefsResult {
    ItemDefs defs;
    std::vector<std::string> errors; // one per rejected file, each starting with its path, sorted by path
};

// Loads and checks every .lua file under root/items/ (none if the folder is missing), each in its own sandboxed
// environment. Duplicate ids reject every file of that id.
ItemDefsResult load_item_defs(sol::state& lua, const std::filesystem::path& root);

} // namespace chthonic::lua
