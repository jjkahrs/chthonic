#include "items.h"
#include "sandbox.h"

#include <algorithm>
#include <format>

namespace chthonic::lua {
namespace {

using Invalid = InvalidDefinition;

constexpr size_t kMaxIdBytes = 64;
constexpr size_t kMaxTextBytes = 255;         // name, icon
constexpr size_t kMaxDescriptionBytes = 1024;
constexpr int64_t kMaxStack = 1000;
constexpr int64_t kMaxSellPrice = 1000000000;
constexpr const char* kQualities[] = {"poor", "common", "uncommon", "rare", "epic"};

void text(const Json& item, const char* key, size_t minBytes, size_t maxBytes, const std::string& what) {
    if (!item.contains(key) || !item[key].is_string() || item[key].get<std::string>().size() < minBytes ||
        item[key].get<std::string>().size() > maxBytes)
        throw Invalid(minBytes ? std::format("{}\"{}\" must be a non-empty string of at most {} bytes", what, key, maxBytes)
                               : std::format("{}\"{}\" must be a string of at most {} bytes", what, key, maxBytes));
}

void integer(const Json& item, const char* key, int64_t lo, int64_t hi, const std::string& what) {
    if (!item.contains(key) || !item[key].is_number_integer() || item[key].get<int64_t>() < lo || item[key].get<int64_t>() > hi)
        throw Invalid(std::format("{}\"{}\" must be an integer in [{}, {}]", what, key, lo, hi));
}

Json parse_item(sol::state& lua, const std::filesystem::path& file, const std::string& path, std::string& declared) {
    const sol::table table = run_definition_file(
        lua, file, path, "{id = ..., name = ..., icon = ..., quality = ..., maxStack = ..., sellPrice = ..., description = ...}");
    const std::string what = path + ": ";
    check_keys(table, {"id", "name", "icon", "quality", "maxStack", "sellPrice", "description"}, what);
    nlohmann::json plain;
    std::string error;
    if (!lua_to_json(table, plain, error)) throw Invalid(what + error);
    const Json item = plain;
    text(item, "id", 1, kMaxIdBytes, what);
    declared = item["id"];
    if (!std::ranges::all_of(declared, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
        throw Invalid(what + "\"id\" may only hold a-z, 0-9 and _");
    text(item, "name", 1, kMaxTextBytes, what);
    text(item, "icon", 1, kMaxTextBytes, what);
    text(item, "description", 0, kMaxDescriptionBytes, what);
    if (!item.contains("quality") || !item["quality"].is_string() ||
        std::ranges::none_of(kQualities, [&](const char* q) { return item["quality"] == q; }))
        throw Invalid(what + "\"quality\" must be one of \"poor\", \"common\", \"uncommon\", \"rare\", \"epic\"");
    integer(item, "maxStack", 1, kMaxStack, what);
    integer(item, "sellPrice", 0, kMaxSellPrice, what);
    return item;
}

} // namespace

ItemDefsResult load_item_defs(sol::state& lua, const std::filesystem::path& root) {
    struct Parsed {
        std::string path, id, error; // id: as declared, even if the file was rejected later
        Json item;
    };
    std::vector<Parsed> parsed;
    for (const auto& file : definition_files(root, kItemsDir)) {
        Parsed p;
        p.path = lua_path(file, root);
        try {
            p.item = parse_item(lua, file, p.path, p.id);
        } catch (const Invalid& e) {
            p.error = e.what();
        } catch (const std::exception& e) { // sol2 conversions
            p.error = std::format("{}: {}", p.path, e.what());
        }
        parsed.push_back(std::move(p));
    }
    // Id clashes reject every file of an id, so the result doesn't depend on scan order.
    for (Parsed& p : parsed) {
        if (!p.error.empty()) continue;
        std::string others;
        for (const Parsed& o : parsed)
            if (&o != &p && o.id == p.id) others += std::format("{}{}", others.empty() ? "" : ", ", o.path);
        if (!others.empty()) p.error = std::format("{}: id \"{}\" is also declared in {}", p.path, p.id, others);
    }
    ItemDefsResult out;
    for (Parsed& p : parsed) {
        if (p.error.empty()) out.defs.emplace(p.id, std::move(p.item));
        else out.errors.push_back(std::move(p.error));
    }
    return out;
}

} // namespace chthonic::lua
