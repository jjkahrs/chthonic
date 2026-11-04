#include "char_repo.h"
#include "../core/utc.h"

#include <nlohmann/json.hpp>

#include <string>

namespace chthonic::db {
namespace {

std::string now_utc() { return utc_mysql(std::chrono::system_clock::now()); }

// The options column: a JSON object of strings, as create wrote it; anything else reads as none.
proto::CharOptions to_options(const std::optional<std::string>& column) {
    proto::CharOptions out;
    if (!column) return out;
    const nlohmann::json json = nlohmann::json::parse(*column, nullptr, false);
    if (!json.is_object()) return out;
    for (const auto& [key, value] : json.items())
        if (value.is_string()) out.emplace(key, value.get<std::string>());
    return out;
}

CharRow to_row(const Row& r) {
    CharRow c;
    c.id = std::stoull(*r[0]);
    c.accountId = std::stoull(*r[1]);
    c.name = *r[2];
    c.mapId = static_cast<uint16_t>(std::stoul(*r[3]));
    c.pos = {std::stof(*r[4]), std::stof(*r[5]), std::stof(*r[6])};
    c.heading = std::stof(*r[7]);
    c.options = to_options(r[8]);
    c.instanceId = static_cast<uint32_t>(std::stoul(*r[9]));
    if (r.size() > 10 && r[10]) c.components = *r[10];
    return c;
}

constexpr const char* kColumns = "id, account_id, name, map_id, pos_x, pos_y, pos_z, heading, options, instance_id";

} // namespace

std::vector<CharRow> CharRepo::list(uint64_t accountId) {
    auto res = c_.exec(std::string("SELECT ") + kColumns +
                           " FROM characters WHERE realm_id = ? AND account_id = ? AND deleted_at IS NULL ORDER BY id",
                       {uint64_t{realm_}, accountId});
    std::vector<CharRow> out;
    for (const auto& r : res.rows) out.push_back(to_row(r));
    return out;
}

CreateResult CharRepo::create(uint64_t accountId, const std::string& name, const proto::CharOptions& options,
                              uint16_t mapId, proto::Vec3 spawn, float heading, uint32_t maxChars) {
    CreateResult result;
    auto count = c_.exec("SELECT COUNT(*) FROM characters WHERE realm_id = ? AND account_id = ? AND deleted_at IS NULL",
                         {uint64_t{realm_}, accountId});
    if (std::stoull(*count.rows[0][0]) >= maxChars) { result.code = proto::Code::char_limit; return result; }
    try {
        const Param optionsColumn = options.empty() ? Param{nullptr} : Param{nlohmann::json(options).dump()};
        auto ins = c_.exec("INSERT INTO characters (realm_id, account_id, name, map_id, pos_x, pos_y, pos_z, heading, "
                           "options, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           {uint64_t{realm_}, accountId, name, uint64_t{mapId}, double{spawn.x}, double{spawn.y},
                            double{spawn.z}, double{heading}, optionsColumn, now_utc()});
        result.row = {ins.insertId, accountId, name, mapId, spawn, heading, {}, options};
    } catch (const DbError& e) {
        if (e.code != kErDupEntry) throw;
        result.code = proto::Code::name_taken;
    }
    return result;
}

bool CharRepo::soft_delete(uint64_t accountId, uint64_t charId) {
    auto res = c_.exec("UPDATE characters SET deleted_at = ? WHERE id = ? AND realm_id = ? AND account_id = ? "
                       "AND deleted_at IS NULL",
                       {now_utc(), charId, uint64_t{realm_}, accountId});
    return res.affectedRows == 1;
}

std::optional<CharRow> CharRepo::enter(uint64_t accountId, uint64_t charId) {
    auto res = c_.exec(std::string("SELECT ") + kColumns +
                           ", components FROM characters WHERE id = ? AND realm_id = ? AND account_id = ? AND deleted_at IS NULL",
                       {charId, uint64_t{realm_}, accountId});
    if (res.rows.empty()) return std::nullopt;
    c_.exec("UPDATE characters SET last_login_at = ? WHERE id = ?", {now_utc(), charId});
    return to_row(res.rows[0]);
}

void CharRepo::save(const CharSave& s) {
    if (!s.components) {
        c_.exec("UPDATE characters SET map_id = ?, instance_id = ?, pos_x = ?, pos_y = ?, pos_z = ?, heading = ? "
                "WHERE id = ? AND realm_id = ?",
                {uint64_t{s.mapId}, uint64_t{s.instanceId}, double{s.pos.x}, double{s.pos.y}, double{s.pos.z},
                 double{s.heading}, s.charId, uint64_t{realm_}});
        return;
    }
    c_.exec("UPDATE characters SET map_id = ?, instance_id = ?, pos_x = ?, pos_y = ?, pos_z = ?, heading = ?, "
            "components = ? WHERE id = ? AND realm_id = ?",
            {uint64_t{s.mapId}, uint64_t{s.instanceId}, double{s.pos.x}, double{s.pos.y}, double{s.pos.z},
             double{s.heading}, *s.components, s.charId, uint64_t{realm_}});
}

uint32_t CharRepo::max_instance_id() {
    auto res = c_.exec("SELECT COALESCE(MAX(instance_id), 0) FROM characters WHERE realm_id = ?", {uint64_t{realm_}});
    return static_cast<uint32_t>(std::stoul(*res.rows[0][0]));
}

} // namespace chthonic::db
