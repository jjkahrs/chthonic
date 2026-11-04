#include "world.h"
#include "components.h"
#include "../core/log.h"

#include <algorithm>
#include <format>
#include <set>
#include <stdexcept>

namespace chthonic::world {

World::World(WorldRules rules, const Maps& maps, Send send, SendMany sendMany, Save save, lua::Defs defs)
    : rules_(rules), maps_(maps), send_(std::move(send)), sendMany_(std::move(sendMany)), save_(std::move(save)),
      defs_(std::move(defs)), nextInstance_(std::max<uint64_t>(rules.firstInstanceId, 1)),
      realm_(realmReg_, {},
             {.create = {}, // the realm has no entities: nothing to create, destroy, transfer or spawn
              .destroy = {},
              .clamp = [this](uint16_t map, proto::Vec3 pos) { return clamp_to_map(map, pos); },
              .map = [this](uint16_t id) { return maps_.by_id(id); },
              .ground = [this](uint16_t map, float x, float z) { return maps_.ground(map, x, z); },
              .path = [this](uint16_t map, proto::Vec3 from, proto::Vec3 to) { return maps_.nav().path(map, from, to); },
              .transfer = {},
              .instance = {},
              .spawn = {},
              .count = [this] { return size_t(std::max<int64_t>(0, npcs())); },
              .post = [this](lua::Post post) { realmPosts_.push_back(std::move(post)); },
              .max = rules.maxNpcs}) {
    // Delivered on each map's bus at its next drain, as when every Lua system shared the game machines' bus.
    realmChanged_ = realm_.bus().subscribe(lua::kStateChangedEvent, [this](const nlohmann::json& p) {
        for (auto& [id, ctx] : contexts_) ctx->bus().publish(lua::kStateChangedEvent, p, "realm");
    });
    if (rules_.mapThreads > 1) pool_ = std::make_unique<WorkerPool>(size_t(rules_.mapThreads));
    log::debug("realm context started", {{"mapThreads", rules_.mapThreads}});
}

World::~World() = default;

MapContext& World::context(uint16_t map) {
    if (auto it = contexts_.find({map, 0}); it != contexts_.end()) return *it->second;
    if (maps_.info(map).instanced())
        throw std::logic_error(std::format("map {} is an instance map: it has no single context", map));
    return create(map, 0, std::nullopt);
}

MapContext& World::create(uint16_t map, uint32_t instance, std::optional<std::string> key) {
    if (instance && key) shared_.copies[{map, *key}] = {instance, 0};
    MapContext& ctx = *contexts_
                           .emplace(Key{map, instance},
                                    std::make_unique<MapContext>(map, instance, std::move(key), rules_, maps_, shared_,
                                                                 send_, sendMany_, save_, defs_, now_))
                           .first->second;
    ctx.ids().adopt(shared_); // a destroyed copy's ids first
    share_npcs(); // before its scripts' init can create any
    if (luaRoot_) // the same files every context loaded: any error was a startup error already
        for (const std::string& e : ctx.load_scripts(*luaRoot_, gameMachines_, realm_.fsm().game_states()))
            log::error("Lua error", {{"map", map}, {"instance", instance}, {"error", e}});
    return ctx;
}

MapContext* World::copy_for(uint16_t map, const std::optional<std::string>& key) {
    if (key)
        if (const auto it = shared_.copies.find({map, *key}); it != shared_.copies.end())
            return contexts_.at({map, it->second.instance}).get();
    return new_copy(map, key); // null: refused like a full copy, never a crash
}

MapContext* World::new_copy(uint16_t map, const std::optional<std::string>& key) {
    // ponytail: ids never wrap (a saved one could name a new copy), so after 2^32 - 1 copies ever made in the realm no
    // more are made; reset characters.instance_id to 0 (players then enter at the exits) if a realm ever gets there.
    if (nextInstance_ > UINT32_MAX) return nullptr;
    if (!shared_.room_for_copy()) return nullptr; // entity ids low
    return &create(map, static_cast<uint32_t>(nextInstance_++), key);
}

const char* World::no_copy_reason() const { return nextInstance_ > UINT32_MAX ? "instance ids exhausted" : "entity ids low"; }

// ponytail: fill the fullest; no rebalancing, so a layer emptied by logouts only closes once its last player leaves.
MapContext& World::layer_for(uint16_t map, std::optional<uint32_t> hint) {
    const uint32_t cap = *maps_.info(map).layerCap;
    MapContext* fullest = &context(map); // the base: always live
    MapContext* roomy = nullptr;         // the fullest with room
    for (auto it = contexts_.lower_bound({map, 0}); it != contexts_.end() && it->first.first == map; ++it) {
        MapContext* ctx = it->second.get();
        if (ctx->population() < cap) {
            if (hint == ctx->instance()) return *ctx;
            if (!roomy || ctx->population() > roomy->population()) roomy = ctx;
        }
        if (ctx->population() > fullest->population()) fullest = ctx;
    }
    if (roomy) return *roomy;
    if (MapContext* fresh = new_copy(map, std::nullopt)) return *fresh;
    log::warn("no new layer: overfilling the fullest",
              {{"map", map}, {"instance", fullest->instance()}, {"players", fullest->population()}, {"cap", cap},
               {"reason", no_copy_reason()}});
    return *fullest;
}

MapContext* World::layer_dest(entt::entity e, MapContext& from, const Destination& to) {
    context(to.map); // the base, always live
    const auto it = to.layer ? contexts_.find({to.map, *to.layer}) : contexts_.end();
    MapContext* asked = it == contexts_.end() ? nullptr : it->second.get();
    if (!from.registry().all_of<SessionLink>(e)) return asked ? asked : &context(to.map); // NPCs: no cap, no new layer
    if (!to.layer || to.layerHint) return &layer_for(to.map, asked ? to.layer : std::nullopt);
    if (asked && asked->population() < *maps_.info(to.map).layerCap) return asked;
    from.refuse_layer(e, to.map, *to.layer); // filled up or closed since the request
    return nullptr;
}

MapContext* World::copy(uint16_t map, uint32_t instance) const {
    const auto it = instance ? contexts_.find({map, instance}) : contexts_.end();
    return it == contexts_.end() ? nullptr : it->second.get();
}

std::vector<World::ContextInfo> World::contexts() const {
    std::vector<ContextInfo> out;
    const SysTime wallNow = std::chrono::system_clock::now();
    const TimePoint steadyNow = SteadyClock::now();
    for (const auto& [key, ctx] : contexts_) {
        ContextInfo& c = out.emplace_back();
        c.map = key.first;
        c.instance = key.second;
        c.key = ctx->key();
        c.players = ctx->population();
        c.npcs = ctx->npcs_here();
        c.createdAt = ctx->created_at();
        if (const auto since = ctx->empty_since(); since && c.instance)
            c.emptySince = wallNow - std::chrono::duration_cast<SysTime::duration>(steadyNow - *since);
    }
    return out;
}

MapContext* World::find(entt::entity e) const {
    for (const auto& [id, ctx] : contexts_)
        if (ctx->registry().valid(e)) return ctx.get();
    return nullptr;
}

std::vector<World::Key> World::expire_copies() {
    std::vector<Key> dying;
    if (scriptsShutDown_) return dying; // the server is going down: every context goes with it
    for (auto& [key, ctx] : contexts_) {
        if (!key.second) continue;
        if (ctx->population() > 0) {
            ctx->set_empty_since(std::nullopt);
            continue;
        }
        if (!ctx->empty_since()) ctx->set_empty_since(now_);
        if (now_ - *ctx->empty_since() < std::chrono::seconds(maps_.info(key.first).emptyTimeoutSec)) continue;
        log::info(maps_.info(key.first).instanced() ? "instance destroyed" : "layer destroyed",
                  {{"map", key.first}, {"instance", key.second},
                   {"key", ctx->key() ? nlohmann::json(*ctx->key()) : nlohmann::json()}, {"npcs", ctx->npcs_here()}});
        ctx->shutdown_scripts();
        ctx->remove_npcs();
        if (ctx->key()) shared_.copies.erase({key.first, *ctx->key()});
        dying.push_back(key);
    }
    return dying;
}

void World::refresh_copies() {
    for (auto& [key, copy] : shared_.copies) copy.players = contexts_.at({key.first, copy.instance})->population();
    shared_.layers.clear();
    for (const auto& [key, ctx] : contexts_)
        if (maps_.info(key.first).layerCap) shared_.layers.emplace(key, ctx->population());
}

std::vector<std::string> World::load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines) {
    context(rules_.startMap); // created before luaRoot_ is set: loaded below with the others
    for (const auto& [id, info] : maps_.all())
        if (info.layerCap) context(id); // a layered map's base layer is always live
    refresh_copies();
    luaRoot_ = luaRoot;
    gameMachines_ = std::move(gameMachines);
    std::vector<std::string> errors = realm_.load_game_machines(luaRoot, gameMachines_);
    for (auto& [id, ctx] : contexts_)
        for (std::string& e : ctx->load_scripts(luaRoot, gameMachines_, realm_.fsm().game_states()))
            errors.push_back(std::move(e));
    std::vector<std::string> unique; // every context reads the same files, and the realm the same fsms/
    for (std::string& e : errors)
        if (std::ranges::find(unique, e) == unique.end()) unique.push_back(std::move(e));
    return unique;
}

void World::shutdown_scripts() {
    scriptsShutDown_ = true;
    for (auto& [id, ctx] : contexts_) ctx->shutdown_scripts();
    realm_.shutdown();
    serial(false); // the broadcasts reach the other maps' players before the despawns
}

std::vector<lua::SystemStatus> World::script_status() const {
    std::vector<lua::SystemStatus> merged = context(rules_.startMap).script_status();
    for (const auto& [id, ctx] : contexts_)
        for (const lua::SystemStatus& s : ctx->script_status())
            for (lua::SystemStatus& m : merged)
                if (m.path == s.path && m.ok && !s.ok) m = s;
    return merged;
}

entt::entity World::spawn(uint32_t sessionId, const db::CharRow& row, TimePoint now) {
    now_ = now;
    uint16_t map = row.mapId == 0 ? rules_.startMap : row.mapId; // 0: saved before maps (migration 003)
    proto::Vec3 pos = row.pos;
    float heading = row.heading;
    MapContext* ctx = nullptr;
    if (const map::MapInfo* info = maps_.by_id(map); info && info->instanced()) {
        MapContext* saved = copy(map, row.instanceId);
        const char* why = !saved                                                       ? "instance gone"
                          : info->maxPlayers && saved->population() >= *info->maxPlayers ? "instance full"
                          : !info->contains(pos.x, pos.z)                               ? "position off the map"
                                                                                        : nullptr;
        if (!why) {
            ctx = saved;
        } else { // a restart, the copy's timeout, or its cap: the way out
            log::info("saved instance not joinable; entering at the map's exit",
                      {{"char", row.id}, {"map", map}, {"instance", row.instanceId}, {"reason", why}});
            map = info->exit->map;
            pos = {info->exit->pos[0], info->exit->pos[1], info->exit->pos[2]};
            heading = info->exit->heading;
        }
    }
    if (!maps_.has_tile(map, pos.x, pos.z)) { // map removed or shrunk since the save
        const auto& start = maps_.info(rules_.startMap);
        log::warn("saved position not on a loaded map; using START_MAP spawn",
                  {{"char", row.id}, {"map", map}, {"x", pos.x}, {"z", pos.z}});
        map = start.id;
        pos = {start.spawn[0], start.spawn[1], start.spawn[2]};
        heading = start.spawnHeading;
    }
    if (!ctx && maps_.info(map).layerCap) ctx = &login_layer(row, map);
    if (const float ground = *maps_.ground(map, pos.x, pos.z); pos.y < ground - rules_.move.groundTolerance)
        pos.y = ground; // saved under the ground (heights edited since): onto it
    pos.y = std::clamp(pos.y, rules_.move.minY, rules_.move.maxY);
    const entt::entity e = (ctx ? *ctx : context(map)).spawn(sessionId, row, pos, heading, now);
    serial(false);
    return e;
}

MapContext& World::login_layer(const db::CharRow& row, uint16_t map) {
    const uint32_t saved = row.mapId == map ? row.instanceId : 0; // a save on another map names none of these layers
    if (MapContext* layer = saved ? copy(map, saved) : nullptr; layer && layer->population() < *maps_.info(map).layerCap)
        return *layer;
    if (saved)
        log::info("saved layer not joinable; placing as a new arrival",
                  {{"char", row.id}, {"map", map}, {"instance", saved}, {"reason", copy(map, saved) ? "layer full" : "layer gone"}});
    return layer_for(map, context(map).layer_for(entt::null, map)); // no entity yet: the base layer's hooks, e nil
}

void World::despawn(entt::entity e) {
    if (MapContext* ctx = find(e)) {
        ctx->despawn(e);
        serial(false);
    }
}

entt::entity World::create_npc(std::string name, std::string templateName, std::optional<uint16_t> map,
                               std::optional<proto::Vec3> pos, float heading, std::string npc,
                               const lua::Json& components) {
    const entt::entity e = context(map.value_or(rules_.startMap))
                               .create_npc(std::move(name), std::move(templateName), pos, heading, std::move(npc),
                                           components);
    serial(false);
    return e;
}

void World::destroy_npc(entt::entity e) {
    if (MapContext* ctx = find(e)) {
        ctx->destroy_npc(e);
        serial(false);
    }
}

void World::remove_npcs() {
    for (auto& [id, ctx] : contexts_) ctx->remove_npcs();
}

bool World::transfer(entt::entity e, const Destination& to) {
    const bool moved = move_to(e, to);
    serial(false);
    return moved;
}

bool World::move_to(entt::entity e, const Destination& to) {
    MapContext* from = find(e);
    if (!from) return false;
    if (!maps_.owns(to.map, map::tile_of(to.pos.x, to.pos.z))) {
        // ponytail: one process owns every tile; future = save + Redirect{host,port,ticket} to the owning process.
        log::warn("transfer refused: destination tile not owned by this process",
                  {{"entity", wire_id(e)}, {"map", to.map}, {"x", to.pos.x}, {"z", to.pos.z}});
        return false;
    }
    const map::MapInfo& info = maps_.info(to.map);
    if (to.map == from->map() && (!info.instanced() || to.key == from->key()) && (!to.layer || *to.layer == from->instance())) {
        from->transfer(e, to);
        return true;
    }
    MapContext* const layer = info.layerCap ? layer_dest(e, *from, to) : nullptr;
    if (info.layerCap && !layer) return false;
    if (info.instanced() && to.key && info.maxPlayers && from->registry().all_of<SessionLink>(e))
        if (const auto it = shared_.copies.find({to.map, *to.key});
            it != shared_.copies.end() && contexts_.at({to.map, it->second.instance})->population() >= *info.maxPlayers) {
            from->refuse_full(e, to.map, *to.key); // filled up since the request, by others' transfers or logins
            return false;
        }
    MapContext* const inst = info.instanced() ? copy_for(to.map, to.key) : nullptr;
    if (info.instanced() && !inst) { // refused like a full copy
        log::error("transfer refused: no new instance copy",
                   {{"entity", wire_id(e)}, {"map", to.map}, {"reason", no_copy_reason()}});
        from->refuse_full(e, to.map, to.key.value_or(""));
        return false;
    }
    MapContext& dest = inst ? *inst : layer ? *layer : context(to.map);
    const lua::Carried carried = from->leave_map(e);
    if (!from->registry().valid(e)) return false; // onMapLeave destroyed it
    const Destination at{to.map, clamp_to_map(to.map, to.pos), to.heading}; // owned, so on the map: y onto the ground
    from->depart(e, at);
    move_entity(from->registry(), dest.registry(), e, defs_);
    dest.arrive(e, at);
    dest.enter_map(e, carried);
    return true;
}

void World::serial(bool endOfTick) {
    const std::vector<Key> dying = expire_copies(); // their outboxes (shutdown's work) are done below like any other
    std::vector<std::pair<Key, MapContext::Outbox>> boxes; // taken first: what this work queues waits for the next
    for (auto& [id, ctx] : contexts_)
        if (MapContext::Outbox box = ctx->take_outbox(); !box.empty()) boxes.emplace_back(id, std::move(box));
    // Every context reads the same mirror, so the same change from several of them is one intent: applied once.
    std::set<std::pair<std::string, std::string>> gameChanges;
    for (auto& [from, box] : boxes) {
        for (const auto& [e, to] : box.transfers) move_to(e, to); // e may be gone since (logged out, destroyed)
        for (MapContext::Create& c : box.creates)
            context(c.map).create_npc(std::move(c.name), std::move(c.templateName), c.pos, c.heading, std::move(c.npc),
                                      c.components, c.id);
        for (const auto& [map, instance, record] : box.respawns) // a copy's NPC: gone with its copy
            if (MapContext* home = instance ? copy(map, instance) : &context(map)) home->spawn_destroyed(record);
        for (const auto& [machine, to] : box.gameChanges) {
            if (!gameChanges.emplace(machine, to).second) continue;
            try {
                realm_.fsm().change(machine, to); // checked against the mirror; the realm warns if no longer allowed
            } catch (const std::exception& ex) {
                log::warn(std::format("Game machine change to \"{}\" skipped: {}", to, ex.what()));
            }
        }
        for (const auto& [id, ctx] : contexts_)
            if (id != from && !box.broadcasts.empty()) ctx->send_client_events(box.broadcasts);
        for (lua::Post& post : box.posts) posts_.emplace_back(from.first, std::move(post));
    }
    if (std::vector<lua::ClientEvent> events = realm_.take_client_events(); !events.empty())
        for (auto& [id, ctx] : contexts_) ctx->send_client_events(events); // each context sends to its own players
    for (lua::Post& post : std::exchange(realmPosts_, {})) posts_.emplace_back("realm", std::move(post));
    if (endOfTick)
        for (const auto& [from, post] : std::exchange(posts_, {})) deliver(from, post);
    for (const Key& key : dying) {
        MapContext& ctx = *contexts_.at(key);
        retiredNpcs_ += ctx.npcs();
        ctx.ids().retire(shared_);
        contexts_.erase(key);
    }
    for (auto& [id, ctx] : contexts_) ctx->mirror_game_machines(realm_.fsm().game_states());
    refresh_copies();
    share_npcs();
}

int64_t World::npcs() const {
    int64_t n = retiredNpcs_;
    for (const auto& [id, ctx] : contexts_) n += ctx->npcs();
    return n;
}

void World::share_npcs() {
    if (contexts_.empty()) return;
    const int64_t total = npcs();
    const size_t left = total < int64_t(rules_.maxNpcs) ? rules_.maxNpcs - size_t(total) : 0, n = contexts_.size();
    size_t i = 0;
    for (auto& [id, ctx] : contexts_) ctx->set_npc_share(total, left, left / n + (i++ < left % n));
}

void World::update(const std::function<void(MapContext&)>& step) {
    stepContexts_.clear();
    for (auto& [id, ctx] : contexts_) {
        ctx->ids().reserve(shared_);
        stepContexts_.push_back(ctx.get());
    }
    share_npcs();
    const auto timed = [&](MapContext& ctx) {
        const auto start = SteadyClock::now();
        step(ctx);
        ctx.add_update_ms(std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count());
    };
    if (!pool_) {
        for (MapContext* ctx : stepContexts_) timed(*ctx);
        return;
    }
    std::exception_ptr error; // a job's exception (rethrown by run) comes out here as from the serial loop
    try {
        pool_->run(stepContexts_.size(), [&](size_t i) {
            stepContexts_[i]->hold();
            timed(*stepContexts_[i]);
        });
    } catch (...) {
        error = std::current_exception();
    }
    for (MapContext* ctx : stepContexts_) ctx->release_held();
    if (error) std::rethrow_exception(error);
}

void World::deliver(const nlohmann::json& from, const lua::Post& post) {
    const nlohmann::json payload{{"from", from}, {"data", post.data}};
    const std::string publisher = from.is_string() ? "realm" : "map " + from.dump();
    switch (post.to) {
    case lua::Post::To::map: // every layer of a layered map
        context(post.map);
        for (auto it = contexts_.lower_bound({post.map, 0}); it != contexts_.end() && it->first.first == post.map; ++it)
            it->second->bus().publish(post.event, payload, publisher);
        break;
    case lua::Post::To::realm: realm_.bus().publish(post.event, payload, publisher); break;
    case lua::Post::To::all:
        for (auto& [id, ctx] : contexts_) ctx->bus().publish(post.event, payload, publisher);
        realm_.bus().publish(post.event, payload, publisher);
        break;
    }
}

void World::push_move(entt::entity e, const proto::Move& m) {
    if (MapContext* ctx = find(e)) ctx->push_move(e, m);
}

void World::push_say(entt::entity e, proto::Say say) {
    if (MapContext* ctx = find(e)) ctx->push_say(e, std::move(say));
}

void World::push_client_event(entt::entity e, std::string name, nlohmann::json data, uint32_t requestId) {
    if (MapContext* ctx = find(e)) ctx->push_client_event(e, std::move(name), std::move(data), requestId);
}

void World::push_interact(entt::entity e, uint32_t objectId) {
    if (MapContext* ctx = find(e)) ctx->push_interact(e, objectId);
}

uint16_t World::map_of(entt::entity e) const { return find(e)->map(); }

size_t World::population() const {
    size_t n = 0;
    for (const auto& [id, ctx] : contexts_) n += ctx->population();
    return n;
}

// Each step runs for every context before the next step starts (update: one after another, or at once on the
// workers), with a serial point between:
// - the realm's bus first: C++ subscribers, and the game machines' fsm.changed for every map's drain this tick;
// - movement and areas: a portal onto another map is queued, and moved at the serial point right after, so the
//   destination's aoi pass sends its Spawns this tick (and its area events reach both buses this tick);
// - the rest of the tick (MapContext::tick_rest), in which Lua's chthonic.transfer, chthonic.create and game machine changes
//   reaching past its map are queued for the serial point at the end of the tick, and chthonic.world.post for the next.
void World::tick(TimePoint now) {
    now_ = now;
    realm_.drain_events();
    serial(false); // work C++ calls queued since the last tick
    update([&](MapContext& ctx) { ctx.tick_move(now); });
    serial(false);
    const SysTime wallNow = std::chrono::system_clock::now(); // one for every context: a mute ends for all alike
    update([&](MapContext& ctx) { ctx.tick_rest(now, mutes_, wallNow); });
    serial(true);
    contextMs_.clear();
    for (auto& [id, ctx] : contexts_) contextMs_.push_back(ctx->take_update_ms());
}

} // namespace chthonic::world
