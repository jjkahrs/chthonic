#include "systems.h"
#include "../core/log.h"
#include "components.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace chthonic::world {
namespace {
entt::entity from_wire(uint32_t id) { return entt::entity{id}; }

// The players that currently see e (see audience).
std::vector<entt::entity> viewers_of(const entt::registry& reg, entt::entity e) {
    std::vector<entt::entity> out;
    if (const auto* viewers = reg.try_get<Viewers>(e)) {
        for (uint32_t id : viewers->ids) out.push_back(from_wire(id));
        return out;
    }
    for (uint32_t id : reg.get<Visible>(e).ids)
        if (reg.all_of<SessionLink>(from_wire(id))) out.push_back(from_wire(id));
    return out;
}

void publish_area(EventBus& bus, const char* event, entt::entity e, uint16_t map, const map::Area& a) {
    bus.publish(event, {{"entity", wire_id(e)}, {"map", map}, {"area", a.id}, {"name", a.name}, {"flags", a.flags}});
}
} // namespace

std::vector<uint32_t> audience(const entt::registry& reg, entt::entity e) {
    std::vector<uint32_t> sessions;
    for (entt::entity viewer : viewers_of(reg, e)) sessions.push_back(reg.get<SessionLink>(viewer).sessionId);
    return sessions;
}

proto::Spawn spawn_message(const entt::registry& reg, entt::entity e) {
    const auto& tf = reg.get<Transform>(e);
    if (const auto* npc = reg.try_get<Npc>(e))
        return {wire_id(e), npc->name, tf.pos, tf.heading, proto::SpawnKind::npc, npc->templateName};
    return {wire_id(e), reg.get<CharacterRef>(e).name, tf.pos, tf.heading, proto::SpawnKind::player, ""};
}

// Validates each queued Move against map bounds, the navmesh (if the map has one and rules.navCheck), speed and the
// ground; a rejected move snaps the client back with Correction{pos, its seq, a new epoch}, and so does an accepted one
// whose y validate_move changed (lifted onto the ground, clamped to MIN_Y/MAX_Y), with the adjusted pos. Moves of
// another epoch are dropped without a Correction: an older one was sent before the client heard of the last
// Correction (the client replays it on top of the Correction itself), a newer one names an epoch the server never
// started. A MoveLock'd character (chthonic.setMovable) has every move rejected.
// ponytail: off_mesh corrects at once, so a player cannot drop off a cliff face; a grace period if falls are wanted.
void movement_system(entt::registry& reg, const MoveRules& rules, const Maps& maps, const Send& send, TimePoint now) {
    for (auto [e, pending, tf, mr, ms, link] : reg.view<PendingMoves, Transform, MapRef, MoveState, SessionLink>().each()) {
        if (pending.moves.empty()) continue;
        std::optional<uint32_t> rejected; // the seq of the rejected (or y-adjusted) move
        for (const auto& m : pending.moves) {
            if (m.epoch != ms.epoch) continue;
            if (reg.all_of<MoveLock>(e)) { rejected = m.seq; break; }
            proto::Vec3 to = m.pos;
            const auto ground = maps.ground(mr.map, to.x, to.z); // nullopt: off the map
            const bool onMesh = !rules.navCheck || !maps.nav().has(mr.map) || maps.nav().on_mesh(mr.map, to, rules.maxAir);
            if (ground && validate_move(rules, ms.budget, tf.pos, to, *ground, now, onMesh) == MoveVerdict::accepted) {
                tf.pos = to;
                tf.heading = m.heading;
                reg.emplace_or_replace<Moved>(e);
                reg.emplace_or_replace<Dirty>(e);
                if (to.y != m.pos.y) {
                    rejected = m.seq;
                    break; // the client has the wrong y: corrected, the rest of the batch is of the old epoch now
                }
            } else {
                rejected = m.seq;
                break; // the rest of the batch is of the old epoch now
            }
        }
        if (rejected) send(link.sessionId, proto::Correction{tf.pos, *rejected, ++ms.epoch});
        pending.moves.clear();
    }
}

const map::Area* set_area(entt::registry& reg, const Maps& maps, EventBus& bus, entt::entity e) {
    const auto& tf = reg.get<Transform>(e);
    const uint16_t mapId = reg.get<MapRef>(e).map;
    const auto& areas = maps.info(mapId).areas;
    const int16_t now = map::area_at(areas, tf.pos.x, tf.pos.y, tf.pos.z);
    const auto& ref = reg.get_or_emplace<AreaRef>(e);
    if (ref.area != now || ref.map != mapId) {
        leave_area(reg, maps, bus, e, mapId);
        if (now >= 0) publish_area(bus, "area.enter", e, mapId, areas.list[now]);
        reg.get<AreaRef>(e).area = now;
    }
    return now >= 0 ? &areas.list[now] : nullptr;
}

void leave_area(entt::registry& reg, const Maps& maps, EventBus& bus, entt::entity e, uint16_t map) {
    auto& ref = reg.get_or_emplace<AreaRef>(e);
    if (ref.area >= 0) publish_area(bus, "area.leave", e, ref.map, maps.info(ref.map).areas.list[ref.area]);
    ref = {map, -1};
}

std::vector<std::pair<entt::entity, map::Portal>> area_system(entt::registry& reg, const Maps& maps, EventBus& bus) {
    std::vector<std::pair<entt::entity, map::Portal>> portals;
    for (auto e : reg.view<Moved, SessionLink>())
        if (const auto* area = set_area(reg, maps, bus, e); area && area->portal) portals.emplace_back(e, *area->portal);
    return portals;
}

void aoi_rebucket(entt::registry& reg, AoiGrid& grid) {
    for (auto [e, tf, mr, cell] : reg.view<Moved, Transform, MapRef, AoiCell>().each()) {
        const CellKey c = grid.cell_of(mr.map, tf.pos.x, tf.pos.z);
        if (c == cell.cell) {
            grid.touch(c); // a move within the cell still changes who is in range
            continue;
        }
        grid.remove(wire_id(e), cell.cell);
        grid.insert(wire_id(e), c);
        cell.cell = c;
    }
}

// Re-buckets movers, then diffs the view of every player whose query window holds a touched cell: Spawn for entities
// that came into range, Despawn for those that left. Range is symmetric, so each player only handles its own view; an
// NPC's Viewers follow the players'. The touched cells (AoiGrid::touch) cover every grid change since the last pass:
// moves (this tick's and the Lua moves re-bucketed after the last pass), spawns, despawns and transfers. A player
// whose window holds none of them, itself included, would get the same view again: it is skipped with an empty
// entered. The query buffer is swapped into vis.ids and the old one reused: no allocation once buffers have grown.
// ponytail: the post-Lua re-bucket touches this tick's player movers' cells again, so their neighbours recompute once
// more next tick (same result); a separate Lua-moved tag if that ever shows in a profile.
void aoi_system(entt::registry& reg, AoiGrid& grid, const Send& send) {
    aoi_rebucket(reg, grid);
    auto posOf = [&](uint32_t id, float& x, float& z) {
        const auto& t = reg.get<Transform>(from_wire(id));
        x = t.pos.x;
        z = t.pos.z;
    };
    thread_local std::vector<uint32_t> now, left; // scratch kept across calls
    for (auto [e, tf, mr, vis, link] : reg.view<Transform, MapRef, Visible, SessionLink>().each()) {
        if (!grid.touched_near(mr.map, tf.pos.x, tf.pos.z)) {
            vis.entered.clear();
            continue;
        }
        grid.query(mr.map, tf.pos.x, tf.pos.z, wire_id(e), posOf, now);
        diff_sorted(vis.ids, now, vis.entered, left);
        for (uint32_t id : vis.entered) {
            const auto other = from_wire(id);
            if (auto* viewers = reg.try_get<Viewers>(other)) insert_sorted(viewers->ids, wire_id(e));
            send(link.sessionId, spawn_message(reg, other));
        }
        for (uint32_t id : left) {
            if (auto* viewers = reg.try_get<Viewers>(from_wire(id))) erase_sorted(viewers->ids, wire_id(e));
            send(link.sessionId, proto::Despawn{id});
        }
        vis.ids.swap(now); // the old ids become the next player's query buffer
    }
    grid.clear_touched();
}

// Accepted player moves and script NPC moves go to every player that sees the mover; never back to the mover. Each
// player gets every mover in its view in one EntityMoves (chunks of kMaxEntries past that). Range is symmetric and an
// NPC's Viewers follow the players' views, so these are exactly the movers' audiences. Batches differ per viewer: Send.
// ponytail: payload bytes stay O(movers x viewers); quantized/delta entries or a lower rate for far viewers if they matter.
void replication_system(entt::registry& reg, const Send& send) {
    constexpr size_t kMax = proto::EntityMoves::kMaxEntries;
    std::vector<proto::EntityMoves::Entry> moves;
    for (auto [e, vis, link] : reg.view<Visible, SessionLink>().each()) {
        moves.clear();
        for (uint32_t id : vis.ids)
            if (const auto other = from_wire(id); reg.all_of<Moved>(other)) {
                const auto& tf = reg.get<Transform>(other);
                moves.push_back({id, tf.pos, tf.heading});
            }
        for (size_t i = 0; i < moves.size(); i += kMax)
            send(link.sessionId, proto::EntityMoves{{moves.begin() + i, moves.begin() + std::min(i + kMax, moves.size())}});
    }
}

namespace {
// ponytail: a yell scans every player (O(players), at most CHAT_RATE_COUNT per window each); an AoiGrid query with the
// yell radius if yells ever get hot.
std::vector<uint32_t> yell_audience(const entt::registry& reg, entt::entity e, float radius) {
    std::vector<uint32_t> sessions;
    const auto& from = reg.get<Transform>(e).pos;
    const uint16_t map = reg.get<MapRef>(e).map;
    for (auto [other, tf, mr, link] : reg.view<Transform, MapRef, SessionLink>().each())
        if (other != e && mr.map == map && std::hypot(tf.pos.x - from.x, tf.pos.z - from.z) <= radius)
            sessions.push_back(link.sessionId);
    return sessions;
}
} // namespace

void chat_system(entt::registry& reg, const ChatRules& rules, const Mutes& mutes, const Send& send,
                 const SendMany& sendMany, TimePoint now, SysTime wallNow) {
    for (auto [e, pending, limit, ch, link] : reg.view<PendingSay, ChatLimit, CharacterRef, SessionLink>().each()) {
        for (auto& say : pending.says) {
            if (!limit.limiter.allow(now)) {
                log::debug("chat rate limited", {{"char", ch.charId}});
                continue;
            }
            auto clean = sanitize_chat(say.text, rules.maxBytes);
            if (!clean) {
                log::debug("chat rejected", {{"char", ch.charId}, {"bytes", say.text.size()}});
                continue;
            }
            if (const auto left = mutes.minutes_left(ch.name, wallNow)) {
                log::debug("chat muted", {{"char", ch.charId}});
                send(link.sessionId, proto::ChatNotice{proto::ChatNoticeCode::muted, "", *left});
                continue;
            }
            auto to = say.kind == proto::ChatKind::yell ? yell_audience(reg, e, rules.yellRadius) : audience(reg, e);
            to.insert(to.begin(), link.sessionId); // the sender first
            sendMany(to, proto::ChatMsg{say.kind, ch.name, std::move(*clean)});
        }
        pending.says.clear();
    }
}

void client_event_in_system(entt::registry& reg, EventBus& bus, const Send& send, TimePoint now) {
    for (auto [e, pending, limit, ch, link] :
         reg.view<PendingClientEvents, ClientEventLimit, CharacterRef, SessionLink>().each()) {
        for (auto& event : pending.events) {
            if (!limit.limiter.allow(now)) {
                ++limit.dropped;
                log::debug("client event rate limited", {{"char", ch.charId}, {"event", event.name}});
                if (event.requestId) send(link.sessionId, proto::Reply{event.requestId, false, R"("rate_limited")"});
                continue;
            }
            nlohmann::json payload{{"entity", wire_id(e)}, {"data", std::move(event.data)}};
            if (event.requestId) payload["requestId"] = event.requestId; // answered with chthonic.reply
            bus.publish("client." + event.name, std::move(payload));
        }
        pending.events.clear();
    }
}

void interact_system(entt::registry& reg, const Maps& maps, EventBus& bus) {
    for (auto [e, pending, ch, tf, mr] : reg.view<PendingInteracts, CharacterRef, Transform, MapRef>().each()) {
        for (const uint32_t id : pending.objectIds) {
            const map::Interactable* it = maps.interactable(mr.map, id);
            const float distance = it ? map::footprint_distance(it->shape, it->pos, it->yaw, tf.pos.x, tf.pos.z) : 0;
            if (!it || distance > it->range) {
                log::debug("interact rejected", {{"char", ch.charId}, {"map", mr.map}, {"object", id},
                                                 {"reason", it ? "out of range" : "not on this map"}});
                continue;
            }
            nlohmann::json payload{{"entity", wire_id(e)}, {"map", mr.map}, {"object", id}};
            if (!it->data.is_null()) payload["data"] = it->data;
            bus.publish(it->event, std::move(payload));
        }
        pending.objectIds.clear();
    }
}

// Runs after the Lua systems, so a change made this tick is sent this tick. Spawn always goes first: aoi_system sent
// it earlier in the tick, and a viewer that just got it receives the full set instead of this tick's changes.
// Only components a writer marked dirty are diffed; each change is serialized once, whatever the number of viewers.
// ponytail: a change is still sent as the component's full JSON; delta or binary encoding if bandwidth matters.
void component_replication_system(entt::registry& reg, const lua::Defs& defs, const Send& send,
                                  const SendMany& sendMany) {
    struct Replicated {
        const lua::ComponentDef* def;
        entt::storage_for_t<lua::ComponentData>* pool;
    };
    std::vector<Replicated> types;
    for (const auto& def : defs)
        if (def->replicate != lua::Replicate::none) types.push_back({def.get(), &lua::pool(reg, def->name)});
    if (types.empty()) return;

    // The owner first, then (aoi) every viewer. An NPC has no owner: its owner components reach nobody, its aoi ones
    // only the players that see it.
    std::vector<uint32_t> to;
    const auto fanOut = [&](entt::entity e, const SessionLink* link, bool aoi, const proto::Message& msg) {
        to.clear();
        if (link) to.push_back(link->sessionId);
        if (aoi)
            for (entt::entity viewer : viewers_of(reg, e)) {
                const auto& entered = reg.get<Visible>(viewer).entered;
                if (std::binary_search(entered.begin(), entered.end(), wire_id(e))) continue; // gets the full set below
                to.push_back(reg.get<SessionLink>(viewer).sessionId);
            }
        sendMany(to, msg);
    };
    for (auto [e, rep] : reg.view<ReplicationDirty, ReplicatedState>().each()) {
        const auto* link = reg.try_get<SessionLink>(e);
        for (const auto& [def, pool] : types) {
            if (!rep.dirty.contains(def->name)) continue;
            const auto sent = rep.sent.find(def->name);
            const bool aoi = def->replicate == lua::Replicate::aoi;
            if (!pool->contains(e)) {
                if (sent == rep.sent.end()) continue;
                rep.sent.erase(sent);
                rep.oversize.erase(def->name);
                fanOut(e, link, aoi, proto::ComponentRemove{wire_id(e), def->name});
                continue;
            }
            const auto& value = pool->get(e).value;
            if (sent != rep.sent.end() && sent->second.value == value) continue; // set back to what was sent
            auto& latest = rep.sent[def->name];
            latest.value = value;
            latest.json = value.dump();
            const std::string& json = latest.json;
            if (!proto::ComponentUpdate::fits(def->name.size(), json.size())) {
                if (rep.oversize.insert(def->name).second)
                    log::error("component too large to replicate; not sent until it fits",
                               {{link ? "char" : "npc", link ? nlohmann::json(reg.get<CharacterRef>(e).charId)
                                                              : nlohmann::json(reg.get<Npc>(e).name)},
                                {"component", def->name},
                                {"bytes", json.size()}});
                continue;
            }
            rep.oversize.erase(def->name);
            fanOut(e, link, aoi, proto::ComponentUpdate{wire_id(e), def->name, json});
        }
        rep.dirty.clear();
    }
    reg.clear<ReplicationDirty>();
    // Viewers that saw an entity come into view this tick (after its Spawn): every aoi component it has, as sent.
    for (auto [viewer, vis, link] : reg.view<Visible, SessionLink>().each()) {
        for (uint32_t id : vis.entered) {
            const auto other = from_wire(id);
            const auto* rep = reg.try_get<ReplicatedState>(other);
            if (!rep) continue;
            for (const auto& [def, pool] : types) {
                if (def->replicate != lua::Replicate::aoi || rep->oversize.contains(def->name)) continue;
                const auto sent = rep->sent.find(def->name);
                if (sent != rep->sent.end()) send(link.sessionId, proto::ComponentUpdate{id, def->name, sent->second.json});
            }
        }
    }
}

void client_event_system(entt::registry& reg, std::vector<lua::ClientEvent> events, const Send& send,
                         const SendMany& sendMany, const std::unordered_map<entt::entity, uint32_t>& sessions) {
    for (auto& event : events) {
        if (event.to == lua::ClientEvent::To::reply) { // chthonic.reply: the requester alone, on any map
            if (const auto it = sessions.find(from_wire(event.entity)); it != sessions.end())
                send(it->second, proto::Reply{event.requestId, event.ok, std::move(event.json)});
            continue;
        }
        const proto::Message msg = proto::ServerEvent{std::move(event.name), std::move(event.json)};
        std::vector<uint32_t> to;
        if (event.to == lua::ClientEvent::To::all) {
            for (auto [e, link] : reg.view<SessionLink>().each()) to.push_back(link.sessionId);
            sendMany(to, msg);
            continue;
        }
        const auto e = from_wire(event.entity);
        if (!reg.valid(e) || !reg.any_of<Visible, Viewers>(e)) continue; // left already: nothing to send
        if (event.to == lua::ClientEvent::To::nearby) to = audience(reg, e);
        if (const auto* link = reg.try_get<SessionLink>(e)) to.insert(to.begin(), link->sessionId); // an NPC has no client
        sendMany(to, msg);
    }
}

// Periodic save of characters whose position or persisted Lua components changed.
void save_system(entt::registry& reg, const lua::Defs& defs, const Save& save, Millis interval, TimePoint now) {
    std::vector<entt::entity> saved;
    for (auto [e, timer, ch, tf, mr] : reg.view<Dirty, SaveTimer, CharacterRef, Transform, MapRef>().each()) {
        if (now - timer.lastSave < interval) continue;
        save({ch.charId, mr.map, tf.pos, tf.heading, lua::components_column(reg, e, defs), mr.instance});
        timer.lastSave = now;
        saved.push_back(e);
    }
    reg.remove<Dirty>(saved.begin(), saved.end());
}

void spawner_system(entt::registry& reg, Spawner& spawner, const std::function<size_t()>& npcs, size_t maxNpcs, TimePoint now,
                    const std::function<entt::entity(const SpawnRecord&)>& create, uint32_t instance) {
    bool capped = false;
    for (size_t i = 0; i < spawner.pending.size();) {
        const uint32_t record = spawner.pending[i];
        SpawnRecord& r = spawner.records[record];
        if (now < r.respawnAt) { ++i; continue; }
        if (npcs() >= maxNpcs) { capped = true; break; }
        spawner.pending.erase(spawner.pending.begin() + i); // before create: an FSM enter callback may destroy an NPC
        reg.emplace<SpawnRef>(create(r), record, instance);
        r.alive = true;
    }
    if (capped && !spawner.capped) log::warn("spawns deferred: MAX_NPCS reached", {{"max", maxNpcs}});
    spawner.capped = capped;
}

namespace {
void set_awake(entt::registry& reg, entt::entity e, bool awake) {
    if (awake) {
        reg.remove<Asleep>(e);
        reg.emplace_or_replace<Awake>(e);
    } else {
        reg.remove<Awake>(e);
        reg.emplace_or_replace<Asleep>(e);
    }
}

// Every NPC whose TileRef is (map, t), found through the grid cells over the tile.
void set_tile_awake(entt::registry& reg, const AoiGrid& grid, uint16_t map, map::TileKey t, bool awake) {
    const float x0 = t.x * map::kTileSize, z0 = t.z * map::kTileSize;
    grid.for_each_in(map, x0, z0, x0 + map::kTileSize, z0 + map::kTileSize, [&](uint32_t id) {
        const auto e = from_wire(id);
        const auto* ref = reg.try_get<TileRef>(e);
        if (ref && ref->map == map && ref->tile == t && reg.all_of<Npc>(e)) set_awake(reg, e, awake);
    });
}

// One player more (+1) or less (-1) near every tile within radius of center.
void add_near(entt::registry& reg, Activation& act, Spawner& spawner, const AoiGrid& grid, TileRef center, int delta) {
    for (int32_t dx = -act.radius; dx <= act.radius; ++dx)
        for (int32_t dz = -act.radius; dz <= act.radius; ++dz) {
            const map::TileKey t{center.tile.x + dx, center.tile.z + dz};
            const uint64_t key = Activation::key(center.map, t);
            if (delta > 0) {
                if (act.nearCount[key]++ > 0) continue;
                if (auto it = act.unloaded.find(key); it != act.unloaded.end()) { // near for the first time: load
                    spawner.pending.insert(spawner.pending.end(), it->second.begin(), it->second.end());
                    act.unloaded.erase(it);
                }
                set_tile_awake(reg, grid, center.map, t, true);
            } else if (auto it = act.nearCount.find(key); it != act.nearCount.end() && --it->second == 0) {
                act.nearCount.erase(it);
                set_tile_awake(reg, grid, center.map, t, false);
            }
        }
}

TileRef tile_now(const entt::registry& reg, entt::entity e) {
    const auto& tf = reg.get<Transform>(e);
    return {reg.get<MapRef>(e).map, map::tile_of(tf.pos.x, tf.pos.z)};
}
} // namespace

void activation_enter(entt::registry& reg, Activation& act, Spawner& spawner, const AoiGrid& grid, entt::entity e) {
    const TileRef ref = reg.emplace_or_replace<TileRef>(e, tile_now(reg, e));
    if (reg.all_of<Npc>(e)) set_awake(reg, e, act.nearCount.contains(Activation::key(ref.map, ref.tile)));
    else add_near(reg, act, spawner, grid, ref, +1);
}

void activation_leave(entt::registry& reg, Activation& act, Spawner& spawner, const AoiGrid& grid, entt::entity e) {
    if (const auto* ref = reg.try_get<TileRef>(e); ref && !reg.all_of<Npc>(e)) add_near(reg, act, spawner, grid, *ref, -1);
}

void activation_move(entt::registry& reg, Activation& act, Spawner& spawner, const AoiGrid& grid, entt::entity e) {
    auto& ref = reg.get<TileRef>(e);
    const TileRef now = tile_now(reg, e);
    if (now.map == ref.map && now.tile == ref.tile) return;
    const TileRef old = std::exchange(ref, now);
    if (reg.all_of<Npc>(e)) {
        set_awake(reg, e, act.nearCount.contains(Activation::key(now.map, now.tile)));
        return;
    }
    add_near(reg, act, spawner, grid, now, +1); // the new tiles first: one near before and after never sleeps
    add_near(reg, act, spawner, grid, old, -1);
}

void activation_system(entt::registry& reg, Activation& act, Spawner& spawner, const AoiGrid& grid) {
    for (auto e : reg.view<Moved, TileRef>()) activation_move(reg, act, spawner, grid, e);
}

void end_of_tick_system(entt::registry& reg) { reg.clear<Moved>(); }

} // namespace chthonic::world
