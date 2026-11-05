// Systems: free functions over the registry, run once per tick in the order World::tick lists them.
#pragma once
#include "../core/clock.h"
#include "aoi_grid.h"
#include "maps.h"
#include "rules.h"

#include <db/char_repo.h>
#include <lua/components.h>
#include <lua/runtime.h>
#include <messages.h>

#include <entt/entity/registry.hpp>

#include <functional>
#include <span>
#include <unordered_map>

namespace chthonic::world {

using Send = std::function<void(uint32_t sessionId, const proto::Message&)>;
// One message to several sessions, each getting it where a Send would have put it: encoded once, not per session.
using SendMany = std::function<void(std::span<const uint32_t> sessionIds, const proto::Message&)>;
using Save = std::function<void(const db::CharSave&)>;

inline uint32_t wire_id(entt::entity e) { return entt::to_integral(e); }

struct ChatRules {
    size_t maxBytes = 256;
    float yellRadius = 300; // CHAT_YELL_RADIUS: m (x/z) on the yeller's map; may exceed the AOI view radius
};

// The session id of every player that currently sees e: for a player, the players in its Visible (range is
// symmetric; the NPCs there see nothing), for an NPC, its Viewers.
std::vector<uint32_t> audience(const entt::registry& reg, entt::entity e);
// Spawn for entity e as a viewer gets it: a player's character name, or an NPC's name and template.
proto::Spawn spawn_message(const entt::registry& reg, entt::entity e);

// A spawn file entry (map::Spawn) and when its NPC is due: alive while the NPC exists, else at respawnAt.
struct SpawnRecord {
    uint16_t map = 0;
    const map::Spawn* spawn = nullptr; // in Maps, which outlives the World
    TimePoint respawnAt{};             // epoch: due at the first tick
    bool alive = false;
};
struct Spawner {
    std::vector<SpawnRecord> records;
    // Records on loaded tiles whose NPC is not alive and may come back, in the order they got here: a tile's
    // records when it loads (Activation), a destroyed NPC's when it has a respawnSec.
    std::vector<uint32_t> pending;
    bool capped = false; // MAX_NPCS hit: warned once until spawning works again
};

// Creates the NPC of every due pending record (create, then SpawnRef{record, instance}: instance is the context's copy
// id, 0 on a world map). At maxNpcs NPCs (npcs: the context's MAX_NPCS count, MapContext::npc_count, which create
// counts up) it creates nothing more this tick and tries again the next.
void spawner_system(entt::registry& reg, Spawner& spawner, const std::function<size_t()>& npcs, size_t maxNpcs, TimePoint now,
                    const std::function<entt::entity(const SpawnRecord&)>& create, uint32_t instance = 0);

// Tile activation (docs/world/maps.html#tile-activation). A tile is near a player when it is within radius
// tiles of the player's tile (Chebyshev) on its map. The first time a tile is near, it loads: its spawn records join
// Spawner::pending, for good. An NPC is Awake while its tile is near, else Asleep. Kept per (map, tile), so it can
// split per map. Nothing here runs for an entity that stays on its tile.
// ponytail: tiles never unload (nor does AzerothCore master), so sleeping NPCs cost memory, not ticks; despawn idle
// tiles on a timer if memory ever matters.
struct Activation {
    int radius = 1;                                               // ACTIVATION_TILES
    std::unordered_map<uint64_t, uint32_t> nearCount;             // (map, tile) -> players it is near; absent: none
    std::unordered_map<uint64_t, std::vector<uint32_t>> unloaded; // (map, tile) -> its spawn records, until it loads
    static uint64_t key(uint16_t map, map::TileKey t) { // map:16 | tx:24 | tz:24, as AoiGrid packs cells
        return (uint64_t(map) << 48) | (uint64_t(uint32_t(t.x) & 0xFFFFFF) << 24) | (uint32_t(t.z) & 0xFFFFFF);
    }
};
// A player or NPC entered the world (Transform, MapRef, in the grid): gives it its TileRef. A player makes the tiles
// around it near (loading and waking them as needed); an NPC starts Awake or Asleep.
void activation_enter(entt::registry& reg, Activation& act, Spawner& spawner, const AoiGrid& grid, entt::entity e);
// A player is about to leave the world: the tiles only it kept near fall asleep.
void activation_leave(entt::registry& reg, Activation& act, Spawner& spawner, const AoiGrid& grid, entt::entity e);
// e may be on another tile or map than its TileRef says (a move or a transfer; already in its new grid cell): a
// player's near tiles follow it (those near both before and after never sleep), an NPC wakes or sleeps by its tile.
void activation_move(entt::registry& reg, Activation& act, Spawner& spawner, const AoiGrid& grid, entt::entity e);
// activation_move for every entity that moved this tick (Moved), once the grid holds their new cells.
void activation_system(entt::registry& reg, Activation& act, Spawner& spawner, const AoiGrid& grid);
// A move off the player's map (onto an undeclared tile) is rejected like a speedhack.
void movement_system(entt::registry& reg, const MoveRules& rules, const Maps& maps, const Send& send, TimePoint now);
// Updates player e's AreaRef from its map and position. On a change it publishes "area.leave" for the old area, then
// "area.enter" for the new one ({entity, map, area, name, flags}; none for "no area"). Returns the area it is in now.
const map::Area* set_area(entt::registry& reg, const Maps& maps, EventBus& bus, entt::entity e);
// Publishes "area.leave" for player e's area, if any, and forgets it (AreaRef {map, -1}): e leaves map's context, and
// the next set_area, in the destination's, publishes only "area.enter".
void leave_area(entt::registry& reg, const Maps& maps, EventBus& bus, entt::entity e, uint16_t map);
// set_area for every player that moved this tick; returns those now in a portal area, with the portal, for the
// caller to transfer.
std::vector<std::pair<entt::entity, map::Portal>> area_system(entt::registry& reg, const Maps& maps, EventBus& bus);
// Moves every entity that moved this tick (Moved) to its new grid cell and touches its cells (old and new, or the one
// it moved within). aoi_system runs it first; World runs it again after Lua, whose NPC moves would otherwise miss the
// next tick's view diff: the cells touched then stay touched until that pass.
void aoi_rebucket(entt::registry& reg, AoiGrid& grid);
// Diffs the view of each player near a touched grid cell (see systems.cpp); the others keep theirs, entered empty.
void aoi_system(entt::registry& reg, AoiGrid& grid, const Send& send);
void replication_system(entt::registry& reg, const Send& send);
// Each player's Says: rate limited (ChatLimit), sanitized, dropped with ChatNotice{muted} while the character is muted
// (wallNow against mutes), then ChatMsg{kind} to the sender and: say / emote the players that see it, yell every player
// on its map within rules.yellRadius.
void chat_system(entt::registry& reg, const ChatRules& rules, const Mutes& mutes, const Send& send,
                 const SendMany& sendMany, TimePoint now, SysTime wallNow);
// Publishes each character's ClientEvents on the Lua bus as "client.<name>" {entity, data}, in arrival order, plus
// requestId for a ClientEvent with a non-zero requestId; past the character's ClientEventLimit they are dropped and
// counted, and a dropped one with a requestId is answered at once with Reply{requestId, false, "rate_limited"}.
void client_event_in_system(entt::registry& reg, EventBus& bus, const Send& send, TimePoint now);
// Each player's Interacts (docs/world/events.html#interactables): one on the player's map (Maps::interactable)
// with the player within its range of the shape's edge (map::footprint_distance, horizontal) publishes the object's
// event {entity, map, object, data (absent if the file has none)}. Anything else is dropped with a debug log.
void interact_system(entt::registry& reg, const Maps& maps, EventBus& bus);
// Sends replicated Lua components (docs/world/lua.html): what changed since the last send to the
// owner (owner, aoi) and observers (aoi), and every aoi component of an entity to a viewer it came into view of.
void component_replication_system(entt::registry& reg, const lua::Defs& defs, const Send& send,
                                  const SendMany& sendMany);
// Sends the client events Lua queued (chthonic.sendTo/sendNearby/broadcast) as ServerEvent, in call order. The audience is
// taken now: owner = the character's session, nearby = it and every session that sees it, all = every player. A
// target no longer in the world is skipped, and an NPC target has no client of its own. A chthonic.reply goes as Reply to
// its entity's session alone, looked up in sessions (Shared::sessions: on any map), so it reaches a requester who
// changed maps since; none if the character left the world.
void client_event_system(entt::registry& reg, std::vector<lua::ClientEvent> events, const Send& send,
                         const SendMany& sendMany, const std::unordered_map<entt::entity, uint32_t>& sessions);
void save_system(entt::registry& reg, const lua::Defs& defs, const Save& save, Millis interval, TimePoint now);
void end_of_tick_system(entt::registry& reg);

} // namespace chthonic::world
