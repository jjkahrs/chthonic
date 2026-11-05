#include "map_context.h"
#include "../core/log.h"
#include "components.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <type_traits>

namespace chthonic::world {

namespace {
// Marks every replicated component e has, for component_replication_system to send (writers outside chthonic mark here).
void mark_replicated(entt::registry& reg, entt::entity e, const lua::Defs& defs) {
    for (const auto& def : defs)
        if (def->replicate != lua::Replicate::none && lua::pool(reg, def->name).contains(e)) mark_dirty(reg, e, def->name);
}

// Every C++ component a player or NPC may have (components.h, lua/components.h, lua/state_machines.h).
template <typename... T>
struct Components {};
using AllComponents =
    Components<SessionLink, CharacterRef, Transform, MapRef, AreaRef, MoveState, PendingMoves, PendingSay, ChatLimit,
               PendingClientEvents, ClientEventLimit, PendingInteracts, MoveLock, AoiCell, Visible, Moved, Dirty, SaveTimer,
               ReplicatedState, ReplicationDirty, ScriptsLeft, Npc, SpawnRef, Viewers, TileRef, Asleep, Awake,
               lua::PreservedComponents, lua::StateMachineRuntime>;

template <typename... T>
void move_typed(entt::registry& from, entt::registry& to, entt::entity e, Components<T...>) {
    (
        [&] {
            if (!from.all_of<T>(e)) return;
            if constexpr (std::is_empty_v<T>) to.emplace<T>(e);
            else to.emplace<T>(e, std::move(from.get<T>(e)));
            from.remove<T>(e);
        }(),
        ...);
}

void move_pool(entt::registry& from, entt::registry& to, entt::entity e, const std::string& name) {
    auto& pool = lua::pool(from, name);
    if (!pool.contains(e)) return;
    lua::pool(to, name).emplace(e, std::move(pool.get(e)));
    pool.remove(e);
}
} // namespace

void move_entity(entt::registry& from, entt::registry& to, entt::entity e, const lua::Defs& defs) {
    if (to.create(e) != e) throw std::logic_error("move_entity: id taken in the destination");
    move_typed(from, to, e, AllComponents{});
    for (const auto& def : defs) move_pool(from, to, e, def->name);
    move_pool(from, to, e, lua::kStateMachineComponent);
    for (auto [id, set] : from.storage()) // a component added to components.h but not to AllComponents
        if (set.contains(e)) log::error("component left behind on a map change", {{"type", std::string(set.info().name())}});
    from.destroy(e);
}

namespace {
constexpr uint32_t kIndices = entt::entt_traits<entt::entity>::entity_mask; // index tombstone excluded
}

std::pair<uint32_t, uint32_t> Shared::block(uint32_t size) {
    std::lock_guard lk(mu_);
    if (size < kIdBlock && !ranges_.empty()) {
        const auto range = ranges_.back();
        ranges_.pop_back();
        return range;
    }
    if (kIndices - next_ < size) return {};
    next_ += size;
    return {next_ - size, next_};
}

bool Shared::room_for_copy() {
    std::lock_guard lk(mu_);
    return ranges_.size() >= 2 || kIndices - next_ >= kReserve;
}

void Shared::give_back(std::vector<entt::entity> freed, std::pair<uint32_t, uint32_t> a, std::pair<uint32_t, uint32_t> b) {
    std::lock_guard lk(mu_);
    freed_.insert(freed_.end(), freed.begin(), freed.end());
    for (const auto& range : {a, b})
        if (range.first != range.second) ranges_.push_back(range);
}

std::vector<entt::entity> Shared::take_freed() {
    std::lock_guard lk(mu_);
    const size_t n = std::min<size_t>(freed_.size(), kIdBlock);
    std::vector<entt::entity> out(freed_.end() - std::ptrdiff_t(n), freed_.end());
    freed_.resize(freed_.size() - n);
    return out;
}

entt::entity IdPool::generate(Shared& shared) {
    if (!freed_.empty()) {
        const entt::entity e = freed_.back();
        freed_.pop_back();
        return e;
    }
    // ponytail: a context past Shared::kIdBlock new ids in one update step takes its block mid-update, so which block
    // depends on the other contexts there; ids stay unique, only the parallel = serial guarantee is gone for them.
    if (block_.first == block_.second) block_ = std::exchange(spare_, {});
    if (block_.first == block_.second) block_ = shared.block(blockSize_);
    if (block_.first == block_.second) throw std::length_error("out of entity ids");
    return entt::entity{block_.first++};
}

proto::Vec3 clamp_to_map(const Maps& maps, const MoveRules& rules, uint16_t map, proto::Vec3 pos) {
    const auto [x, z] = maps.info(map).clamp(pos.x, pos.z);
    // ponytail: no flying NPCs; add a flag when one exists
    return {x, std::clamp(*maps.ground(map, x, z), rules.minY, rules.maxY), z};
}

MapContext::MapContext(uint16_t map, uint32_t instance, std::optional<std::string> key, const WorldRules& rules,
                       const Maps& maps, Shared& shared, Send send, SendMany sendMany, Save save, lua::Defs defs,
                       TimePoint now)
    : map_(map), instance_(instance), key_(std::move(key)), rules_(rules), maps_(maps), shared_(shared), out_(std::move(send)), outMany_(std::move(sendMany)),
      outSave_(std::move(save)),
      send_([this](uint32_t sid, const proto::Message& m) {
          if (!holding_) return out_(sid, m);
          held_.msgs.push_back({uint32_t(held_.sessions.size()), 1, false, m});
          held_.sessions.push_back(sid);
      }),
      sendMany_([this](std::span<const uint32_t> sids, const proto::Message& m) {
          if (!holding_) return outMany_(sids, m);
          held_.msgs.push_back({uint32_t(held_.sessions.size()), uint32_t(sids.size()), true, m});
          held_.sessions.insert(held_.sessions.end(), sids.begin(), sids.end());
      }),
      save_([this](const db::CharSave& s) {
          if (!holding_) return outSave_(s);
          held_.saves.push_back(s);
      }),
      defs_(defs), grid_(rules.cellSize, rules.viewRadius),
      lua_(reg_, std::move(defs),
           {.create = [this](std::string name, std::string templateName, std::optional<uint16_t> to,
                             std::optional<proto::Vec3> pos, float heading, std::string npc, const lua::Json& components) {
                return lua_create(std::move(name), std::move(templateName), to, pos, heading, std::move(npc), components);
            },
            .destroy = [this](entt::entity e) { destroy_npc(e); },
            .clamp = [this](uint16_t on, proto::Vec3 pos) { return clamp_to_map(maps_, rules_.move, on, pos); },
            .map = [this](uint16_t id) { return maps_.by_id(id); },
            .ground = [this](uint16_t on, float x, float z) { return maps_.ground(on, x, z); },
            .path = [this](uint16_t on, proto::Vec3 from, proto::Vec3 to) { return maps_.nav().path(on, from, to); },
            .transfer = [this](entt::entity e, uint16_t to, proto::Vec3 pos, float heading, std::optional<std::string> key,
                               std::optional<uint32_t> layer) {
                return request_transfer(e, {to, pos, heading}, std::move(key), layer);
            },
            .instance = [this]() -> std::optional<lua::InstanceInfo> {
                const bool layered = maps_.info(map_).layerCap.has_value();
                if (!instance_ && !layered) return std::nullopt;
                return lua::InstanceInfo{instance_, key_, map_, layered};
            },
            .spawn = [this](uint32_t record) { return spawner_.records.at(record).spawn; },
            .count = [this] { return npc_count(); },
            .post = [this](lua::Post post) { outbox_.posts.push_back(std::move(post)); },
            .max = rules.maxNpcs}) {
    now_ = now;
    activation_.radius = rules.activationTiles;
    for (const auto& [id, info] : maps_.all())
        for (const map::Spawn& s : info.spawns) {
            if (id == map_)
                activation_.unloaded[Activation::key(id, map::tile_of(s.pos[0], s.pos[2]))].push_back(
                    uint32_t(spawner_.records.size()));
            spawner_.records.push_back({id, &s});
        }
    lua_.fsm().forward_game_machines(
        [this](const std::string& machine, const std::string& to) { outbox_.gameChanges.emplace_back(machine, to); });
    if (instance_)
        log::info(maps_.info(map_).instanced() ? "instance created" : "layer created",
                  {{"map", map_}, {"name", maps_.info(map_).name}, {"instance", instance_},
                   {"key", key_ ? nlohmann::json(*key_) : nlohmann::json()}});
    else log::debug("map context started", {{"map", map_}, {"name", maps_.info(map_).name}});
}

std::vector<std::string> MapContext::load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines,
                                                  const std::map<std::string, std::string>& gameStates) {
    lua_.fsm().mirror(gameStates);
    std::vector<std::string> errors = lua_.load(luaRoot, std::move(gameMachines));
    for (const SpawnRecord& r : spawner_.records)
        if (!lua_.npc(r.spawn->npc))
            errors.push_back(std::format("{}: spawn \"{}\": unknown npc \"{}\" (no definition in npcs/)", r.spawn->file,
                                         r.spawn->id, r.spawn->npc));
    return errors;
}

void MapContext::shutdown_scripts() {
    const auto view = reg_.view<CharacterRef>();
    for (const std::vector<entt::entity> chars(view.begin(), view.end()); entt::entity e : chars) {
        lua_.leave(e);
        reg_.emplace<ScriptsLeft>(e);
    }
    lua_.shutdown();
    send_lua_client_events(); // before the despawns
}

void MapContext::release_held() {
    // holding_ off first: a send that drops a slow client can reach World again (on_closed, despawn) and send more.
    if (!std::exchange(holding_, false)) return; // held_ is what an earlier release passed on already
    for (const Held::Msg& m : held_.msgs) {
        if (m.many) outMany_(std::span(held_.sessions).subspan(m.first, m.count), m.msg);
        else out_(held_.sessions[m.first], m.msg);
    }
    for (const db::CharSave& s : held_.saves) outSave_(s);
}

void MapContext::send_lua_client_events() {
    std::vector<lua::ClientEvent> events = lua_.take_client_events();
    for (const auto& event : events)
        if (event.to == lua::ClientEvent::To::all) outbox_.broadcasts.push_back(event);
    client_event_system(reg_, std::move(events), send_, sendMany_, shared_.sessions);
}

void MapContext::send_client_events(std::vector<lua::ClientEvent> events) {
    client_event_system(reg_, std::move(events), send_, sendMany_, shared_.sessions);
}

entt::entity MapContext::create(entt::entity id) {
    if (reg_.create(id) != id) // only an entity made outside the World (a test's reg.create()) can hold it
        throw std::logic_error(std::format("entity id {} is taken in map {}'s registry", wire_id(id), map_));
    return id;
}

void MapContext::destroy(entt::entity e) {
    reg_.destroy(e);
    ids_.release(e);
}

entt::entity MapContext::spawn(uint32_t sessionId, const db::CharRow& row, proto::Vec3 pos, float heading, TimePoint now,
                              const std::optional<proto::ChatServer>& chat) {
    now_ = now;
    const auto e = create(ids_.generate(shared_));
    reg_.emplace<SessionLink>(e, sessionId);
    reg_.emplace<CharacterRef>(e, row.id, row.name, row.options);
    reg_.emplace<Transform>(e, pos, heading);
    reg_.emplace<MapRef>(e, map_, instance_);
    reg_.emplace<MoveState>(e, MoveBudget{0, now});
    reg_.emplace<PendingMoves>(e);
    reg_.emplace<PendingSay>(e);
    reg_.emplace<ChatLimit>(e, WindowLimiter(rules_.chatRateCount, rules_.chatRateWindow));
    reg_.emplace<PendingClientEvents>(e);
    reg_.emplace<ClientEventLimit>(e, WindowLimiter(rules_.clientEventRateCount, rules_.clientEventRateWindow));
    reg_.emplace<PendingInteracts>(e);
    const CellKey cell = grid_.cell_of(map_, pos.x, pos.z);
    reg_.emplace<AoiCell>(e, cell);
    reg_.emplace<Visible>(e);
    reg_.emplace<SaveTimer>(e, now);
    reg_.emplace<ReplicatedState>(e);
    grid_.insert(wire_id(e), cell);
    set_area(reg_, maps_, lua_.bus(), e);
    ++population_;
    send_(sessionId, proto::EnterWorldOk{wire_id(e), map_, pos, heading});
    if (chat) send_(sessionId, *chat); // right after EnterWorldOk, before anything onEnter causes
    activation_enter(reg_, activation_, spawner_, grid_, e);
    spawn_due(now);
    lua::restore_components(reg_, e, defs_, row.components, [&](const std::string& fix) {
        log::warn("persisted component fixed on load", {{"char", row.id}, {"fix", fix}});
    });
    mark_replicated(reg_, e, defs_);
    lua_.enter(e);
    return e;
}

void MapContext::despawn(entt::entity e) {
    if (!reg_.valid(e)) return;
    if (!reg_.all_of<ScriptsLeft>(e)) {
        lua_.leave(e);
        send_lua_client_events(); // before the Despawn, while e can still be seen
    }
    const auto& ch = reg_.get<CharacterRef>(e);
    const auto& tf = reg_.get<Transform>(e);
    if (const uint64_t dropped = reg_.get<ClientEventLimit>(e).dropped)
        log::info("client events dropped", {{"char", ch.charId}, {"count", dropped}});
    save_({ch.charId, map_, tf.pos, tf.heading, lua::components_column(reg_, e, defs_), instance_});
    leave_views(e);
    activation_leave(reg_, activation_, spawner_, grid_, e);
    grid_.remove(wire_id(e), reg_.get<AoiCell>(e).cell);
    destroy(e);
    --population_;
}

proto::Vec3 MapContext::clamp(proto::Vec3 pos) const { return clamp_to_map(maps_, rules_.move, map_, pos); }

bool MapContext::request_transfer(entt::entity e, Destination to, std::optional<std::string> key,
                                  std::optional<uint32_t> layer) {
    if (!maps_.owns(to.map, map::tile_of(to.pos.x, to.pos.z))) {
        // ponytail: one process owns every tile; future = save + Redirect{host,port,ticket} to the owning process.
        log::warn("transfer refused: destination tile not owned by this process",
                  {{"entity", wire_id(e)}, {"map", to.map}, {"x", to.pos.x}, {"z", to.pos.z}});
        return false;
    }
    const map::MapInfo& info = maps_.info(to.map);
    if (to.map == map_ && (!info.instanced() || !key || key == key_) && (!layer || *layer == instance_)) {
        transfer(e, to);
        return true;
    }
    const bool player = reg_.all_of<SessionLink>(e);
    if (info.instanced()) {
        to.key = key ? std::move(key) : lua_.instance_key(e, to.map);
        if (!reg_.valid(e)) return false; // the hook destroyed it
        if (to.key && player && full(to.map, *to.key)) {
            refuse_full(e, to.map, *to.key);
            return false;
        }
    } else if (layer) {
        if (!layer_room(to.map, *layer, player)) {
            refuse_layer(e, to.map, *layer);
            return false;
        }
        to.layer = layer;
    } else if (info.layerCap && player) {
        to.layer = lua_.layer_for(e, to.map);
        if (!reg_.valid(e)) return false; // the hook destroyed it
        if (to.layer && !layer_room(to.map, *to.layer, true)) to.layer.reset(); // full or gone: World places it
        to.layerHint = true;
    }
    outbox_.transfers.emplace_back(e, std::move(to));
    return true;
}

bool MapContext::layer_room(uint16_t map, uint32_t layer, bool player) const {
    const auto it = shared_.layers.find({map, layer});
    if (it == shared_.layers.end()) return false;
    if (!player) return true; // the cap counts players only
    size_t players = it->second;
    for (const auto& [q, to] : outbox_.transfers)
        players += to.map == map && to.layer == layer && reg_.valid(q) && reg_.all_of<SessionLink>(q);
    return players < *maps_.info(map).layerCap;
}

size_t MapContext::npcs_here() const {
    const auto* npcs = reg_.storage<Npc>(); // a const registry: null before the first NPC
    return npcs ? npcs->size() : 0;
}

bool MapContext::full(uint16_t map, const std::string& key) const {
    const auto max = maps_.info(map).maxPlayers;
    const auto it = shared_.copies.find({map, key});
    if (!max || it == shared_.copies.end()) return false; // no limit, or a new copy
    size_t players = it->second.players;
    for (const auto& [q, to] : outbox_.transfers)
        players += to.map == map && to.key == key && reg_.valid(q) && reg_.all_of<SessionLink>(q);
    return players >= *max;
}

void MapContext::refuse_full(entt::entity e, uint16_t map, const std::string& key) {
    log::info("transfer refused: instance full", {{"entity", wire_id(e)}, {"map", map}, {"key", key}});
    lua_.bus().publish("instance.full", {{"entity", wire_id(e)}, {"map", map}, {"key", key}}, "world");
}

void MapContext::refuse_layer(entt::entity e, uint16_t map, uint32_t layer) {
    log::info("transfer refused: layer full or gone", {{"entity", wire_id(e)}, {"map", map}, {"layer", layer}});
    lua_.bus().publish("instance.full", {{"entity", wire_id(e)}, {"map", map}, {"layer", layer}}, "world");
}

void MapContext::transfer(entt::entity e, const Destination& to) {
    const uint32_t id = wire_id(e);
    const proto::Vec3 pos = clamp(to.pos); // owned, so on the map: only y changes, onto the ground
    reg_.get<Transform>(e) = {pos, to.heading};
    auto& cell = reg_.get<AoiCell>(e);
    grid_.remove(id, cell.cell);
    cell.cell = grid_.cell_of(map_, pos.x, pos.z);
    grid_.insert(id, cell.cell);
    activation_move(reg_, activation_, spawner_, grid_, e);
    // Moves in flight from the old place are of the old epoch: dropped without a Correction (MapChange tells the new one).
    uint32_t epoch = 0;
    if (auto* ms = reg_.try_get<MoveState>(e)) {
        ms->budget = {0, now_};
        epoch = ++ms->epoch;
    }
    reg_.remove<Moved>(e); // a move this tick must not reach the new neighbours in an EntityMoves
    // Out of every view at once, both ways: the mover gets MapChange, then a Despawn per old neighbour; they get its
    // Despawn. The next aoi pass sends Spawns for whoever is in range now: the same neighbours again at the new
    // position. No Moved: no EntityMoves may show anyone where it went.
    if (const auto* link = reg_.try_get<SessionLink>(e)) {
        reg_.emplace_or_replace<Dirty>(e);
        send_(link->sessionId, proto::MapChange{map_, pos, to.heading, epoch});
        for (uint32_t other : reg_.get<Visible>(e).ids) send_(link->sessionId, proto::Despawn{other});
        set_area(reg_, maps_, lua_.bus(), e);
    }
    leave_views(e);
}

lua::Carried MapContext::leave_map(entt::entity e) {
    lua::Carried carried = lua_.leave_map(e);
    send_lua_client_events(); // while e can still be seen here
    return carried;
}

// As transfer, but out of this map: the MapChange (to.pos already on the destination's ground) and the Despawns go
// out here, the Spawns come from the destination's aoi pass.
void MapContext::depart(entt::entity e, const Destination& to) {
    uint32_t epoch = 0;
    if (auto* ms = reg_.try_get<MoveState>(e)) {
        ms->budget = {0, now_};
        epoch = ++ms->epoch;
    }
    reg_.remove<Moved>(e);
    if (const auto* link = reg_.try_get<SessionLink>(e)) {
        reg_.emplace_or_replace<Dirty>(e);
        send_(link->sessionId, proto::MapChange{to.map, to.pos, to.heading, epoch});
        for (uint32_t other : reg_.get<Visible>(e).ids) send_(link->sessionId, proto::Despawn{other});
        leave_area(reg_, maps_, lua_.bus(), e, to.map);
        --population_;
    }
    leave_views(e);
    activation_leave(reg_, activation_, spawner_, grid_, e);
    grid_.remove(wire_id(e), reg_.get<AoiCell>(e).cell);
}

void MapContext::arrive(entt::entity e, const Destination& to) {
    reg_.get<MapRef>(e) = {map_, instance_};
    reg_.get<Transform>(e) = {to.pos, to.heading};
    auto& cell = reg_.get<AoiCell>(e);
    cell.cell = grid_.cell_of(map_, to.pos.x, to.pos.z);
    grid_.insert(wire_id(e), cell.cell);
    activation_enter(reg_, activation_, spawner_, grid_, e);
    if (reg_.all_of<SessionLink>(e)) {
        set_area(reg_, maps_, lua_.bus(), e);
        ++population_;
    }
}

entt::entity MapContext::lua_create(std::string name, std::string templateName, std::optional<uint16_t> map,
                                    std::optional<proto::Vec3> pos, float heading, std::string npc,
                                    const lua::Json& components) {
    const uint16_t on = map.value_or(map_);
    if (on == map_) return create_npc(std::move(name), std::move(templateName), pos, heading, std::move(npc), components);
    const entt::entity id = ids_.generate(shared_); // the id chthonic.create returns now; the NPC is there at the serial point
    ++npcs_;
    outbox_.creates.push_back({id, std::move(name), std::move(templateName), on, pos, heading, std::move(npc), components});
    return id;
}

entt::entity MapContext::create_npc(std::string name, std::string templateName, std::optional<proto::Vec3> pos,
                                    float heading, std::string npc, const lua::Json& components, entt::entity id) {
    const auto& info = maps_.info(map_);
    const proto::Vec3 at = clamp(pos.value_or(proto::Vec3{info.spawn[0], info.spawn[1], info.spawn[2]}));
    if (id == entt::null) {
        id = ids_.generate(shared_);
        ++npcs_;
    }
    const auto e = create(id);
    reg_.emplace<Npc>(e, std::move(name), std::move(templateName), std::move(npc));
    reg_.emplace<Transform>(e, at, heading);
    reg_.emplace<MapRef>(e, map_, instance_);
    const CellKey cell = grid_.cell_of(map_, at.x, at.z);
    reg_.emplace<AoiCell>(e, cell);
    reg_.emplace<Viewers>(e);
    reg_.emplace<ReplicatedState>(e);
    grid_.insert(wire_id(e), cell);
    activation_enter(reg_, activation_, spawner_, grid_, e);
    for (const auto& [type, value] : components.items()) lua::pool(reg_, type).emplace(e, lua::ComponentData{value});
    mark_replicated(reg_, e, defs_);
    if (components.contains(lua::kStateMachineComponent)) lua_.fsm().syncEntity(e); // as chthonic.add + chthonic.set would
    return e;
}

void MapContext::spawn_destroyed(uint32_t record) {
    SpawnRecord& r = spawner_.records[record];
    r.alive = false;
    r.respawnAt = r.spawn->respawnSec < 0
                      ? TimePoint::max()
                      : now_ + std::chrono::duration_cast<TimePoint::duration>(std::chrono::duration<double>(r.spawn->respawnSec));
    if (r.spawn->respawnSec >= 0) spawner_.pending.push_back(record);
}

void MapContext::destroy_npc(entt::entity e) {
    if (const auto* ref = reg_.try_get<SpawnRef>(e)) {
        const uint16_t home = spawner_.records[ref->record].map;
        if (home == map_ && ref->instance == instance_) spawn_destroyed(ref->record);
        else outbox_.respawns.emplace_back(home, ref->instance, ref->record); // from another context: respawns there
    }
    leave_views(e);
    grid_.remove(wire_id(e), reg_.get<AoiCell>(e).cell);
    destroy(e);
    --npcs_;
}

void MapContext::leave_views(entt::entity e) {
    const uint32_t id = wire_id(e);
    const auto unsee = [&](uint32_t viewer) { // a player that saw e
        auto& vis = reg_.get<Visible>(entt::entity{viewer});
        erase_sorted(vis.ids, id);
        erase_sorted(vis.entered, id); // gone in the tick it came into view: no component full set either
        send_(reg_.get<SessionLink>(entt::entity{viewer}).sessionId, proto::Despawn{id});
    };
    if (auto* viewers = reg_.try_get<Viewers>(e)) { // an NPC
        for (uint32_t viewer : viewers->ids) unsee(viewer);
        viewers->ids.clear();
        return;
    }
    auto& vis = reg_.get<Visible>(e); // a player: range is symmetric, so the players it sees see it
    for (uint32_t other : vis.ids) {
        if (auto* viewers = reg_.try_get<Viewers>(entt::entity{other})) erase_sorted(viewers->ids, id); // an NPC it saw
        else unsee(other);
    }
    vis.ids.clear();
    vis.entered.clear();
}

void MapContext::spawn_due(TimePoint now) {
    spawner_system(reg_, spawner_, [this] { return npc_count(); }, rules_.maxNpcs, now, [this](const SpawnRecord& r) {
        const auto& s = *r.spawn;
        const lua::NpcDef* def = lua_.npc(s.npc); // load_scripts refused the start without one
        if (!def) throw std::logic_error("spawn \"" + s.id + "\": npc \"" + s.npc + "\" not loaded");
        return create_npc(def->name, def->templateName, proto::Vec3{s.pos[0], s.pos[1], s.pos[2]}, s.heading, def->name,
                          def->components); // fresh definition data on every respawn
    }, instance_);
}

void MapContext::remove_npcs() {
    const auto view = reg_.view<Npc>();
    for (const std::vector<entt::entity> npcs(view.begin(), view.end()); entt::entity e : npcs) destroy_npc(e);
}

void MapContext::push_move(entt::entity e, const proto::Move& m) {
    if (reg_.valid(e)) reg_.get<PendingMoves>(e).moves.push_back(m);
}

void MapContext::push_say(entt::entity e, proto::Say say) {
    if (reg_.valid(e)) reg_.get<PendingSay>(e).says.push_back(std::move(say));
}

void MapContext::push_client_event(entt::entity e, std::string name, nlohmann::json data, uint32_t requestId) {
    if (reg_.valid(e)) reg_.get<PendingClientEvents>(e).events.push_back({std::move(name), std::move(data), requestId});
}

void MapContext::push_interact(entt::entity e, uint32_t objectId) {
    if (reg_.valid(e)) reg_.get<PendingInteracts>(e).objectIds.push_back(objectId);
}

// area runs right after movement: a player who walked into a portal on this map is transferred before aoi, so this
// tick's view diff already sends the Spawns; one onto another map is moved at World's serial point that follows,
// before the destination's aoi pass.
void MapContext::tick_move(TimePoint now) {
    now_ = now;
    movement_system(reg_, rules_.move, maps_, send_, now);
    for (const auto& [e, portal] : area_system(reg_, maps_, lua_.bus()))
        request_transfer(e, {portal.map, {portal.pos[0], portal.pos[1], portal.pos[2]}, portal.heading});
}

// The spawner runs right before Lua, so a system's update already sees (and can move) the NPCs due this tick; like the
// ones Lua creates, players get their Spawn in the next tick's aoi diff.
// Activation runs once, after Lua's NPC moves are re-bucketed: every player and NPC that changed tile this tick (a
// tile loading now has its NPCs spawned by the next tick's spawner). Transfers and logins activate at once.
// Lua runs after movement and chat (it sees this tick's positions) and before replication and save (its changes go
// out and get saved this tick). aoi stays before both replications, so a Spawn always precedes component data; the
// NPCs Lua creates or moves out of a view show up in the next tick's aoi diff. The events Lua published are delivered
// right after update; client events go out after component replication, so a client already has the Spawn and the
// latest component values they may refer to.
void MapContext::tick_rest(TimePoint now, const Mutes& mutes, SysTime wallNow) {
    now_ = now;
    aoi_system(reg_, grid_, send_);
    chat_system(reg_, rules_.chat, mutes, send_, sendMany_, now, wallNow);
    client_event_in_system(reg_, lua_.bus(), send_, now); // delivered by the drain after this tick's Lua update
    interact_system(reg_, maps_, lua_.bus());      // likewise
    spawn_due(now);
    lua_.update(rules_.tickSeconds);
    lua_.drain_events();
    aoi_rebucket(reg_, grid_); // NPCs Lua moved: in their new cell for the next tick's view diff
    activation_system(reg_, activation_, spawner_, grid_);
    replication_system(reg_, send_);
    component_replication_system(reg_, defs_, send_, sendMany_);
    send_lua_client_events();
    save_system(reg_, defs_, save_, rules_.saveInterval, now);
    end_of_tick_system(reg_);
}

} // namespace chthonic::world
