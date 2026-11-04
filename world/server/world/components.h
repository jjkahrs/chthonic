// ECS components: plain data, no behaviour. Systems (systems.h) read and write them.
#pragma once
#include "../core/rate_limiter.h"
#include "aoi_grid.h"
#include "rules.h"

#include <map.h>
#include <messages.h>
#include <nlohmann/json.hpp>

#include <entt/entity/registry.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace chthonic::world {

struct SessionLink {
    uint32_t sessionId = 0;
};

struct CharacterRef {
    uint64_t charId = 0;
    std::string name;
    proto::CharOptions options = {}; // as created (CharCreate)
};

struct Transform {
    proto::Vec3 pos;
    float heading = 0;
};

// The map every player and NPC is on (map.json "id"), and on an instance map which copy (instanceId; 0 on a world
// map). Changed only by World::transfer.
struct MapRef {
    uint16_t map = 0;
    uint32_t instance = 0;
};

// The area a player is in: an index into map's areas (map::area_at), -1 for none. Kept by set_area; map is the map
// the index belongs to, which a transfer changes before set_area runs.
struct AreaRef {
    uint16_t map = 0;
    int16_t area = -1;
};

struct MoveState {
    MoveBudget budget;
    uint32_t epoch = 0; // bumped by each Correction; Moves of an older one are dropped (movement_system)
};

// Inbound this tick, filled from the network before systems run.
struct PendingMoves {
    std::vector<proto::Move> moves;
};
struct PendingSay {
    std::vector<proto::Say> says; // kind + text, unchecked
};

struct ChatLimit {
    WindowLimiter limiter;
};

// ClientEvents this tick, already checked by GameServer (name, size, JSON object); client_event_in_system rate limits
// them and publishes them to the Lua bus.
struct PendingClientEvent {
    std::string name;
    nlohmann::json data;
    uint32_t requestId = 0; // chthonic.world.request's, 0 for chthonic.world.send
};
struct PendingClientEvents {
    std::vector<PendingClientEvent> events;
};
struct ClientEventLimit {
    WindowLimiter limiter;
    uint64_t dropped = 0; // over the limit since login (logged on despawn)
};

// Interact object ids this tick (interact_system checks and publishes them).
struct PendingInteracts {
    std::vector<uint32_t> objectIds;
};

struct MoveLock {}; // tag: chthonic.setMovable(e, false); every Move is refused with a Correction

struct AoiCell {
    CellKey cell;
};

// Entities this one can currently see, sorted wire ids. entered: those that came into view this tick (sorted).
struct Visible {
    std::vector<uint32_t> ids;
    std::vector<uint32_t> entered;
};

struct Moved {};  // tag: accepted a move this tick
struct Dirty {};  // tag: position changed since the last save

struct SaveTimer {
    TimePoint lastSave{};
};

// The replicated Lua components as last sent to clients, by name, with the JSON text sent (component_replication_system
// diffs the dirty ones against it).
struct ReplicatedState {
    struct Sent {
        nlohmann::ordered_json value;
        std::string json; // value.dump(): serialized once, reused for every viewer
    };
    std::map<std::string, Sent, std::less<>> sent;
    std::set<std::string, std::less<>> oversize; // too big for a frame: logged once until it fits again
    std::set<std::string, std::less<>> dirty;    // replicated components written since the last run (mark_dirty)
};
struct ReplicationDirty {}; // tag: dirty is not empty, so component_replication_system only visits these
// How every writer marks a replicated component of e (one with a ReplicatedState) for sending.
inline void mark_dirty(entt::registry& reg, entt::entity e, const std::string& name) {
    if (auto* rep = reg.try_get<ReplicatedState>(e)) {
        rep->dirty.insert(name);
        reg.emplace_or_replace<ReplicationDirty>(e);
    }
}

struct ScriptsLeft {}; // tag: onLeave already ran (graceful shutdown runs it for everyone first)

// A server-created NPC (chthonic.create or a spawn file): no session, never saved. name and templateName go to clients in
// Spawn. npc: the NPC definition it was made from (lua::NpcDef), "" for chthonic.create with a template.
struct Npc {
    std::string name;
    std::string templateName;
    std::string npc;
};

// An NPC placed by a spawn file: record indexes World's spawn records (SpawnRecord). Read in Lua as "Spawn".
struct SpawnRef {
    uint32_t record = 0;
    uint32_t instance = 0; // the copy of the spawn's (instance) map it came from; 0: a world map
};

// The players that currently see this NPC, sorted wire ids. An NPC has no Visible: it sees nothing, and AOI is
// tracked from the players' side (aoi_system keeps this in step with their Visible).
struct Viewers {
    std::vector<uint32_t> ids;
};

// The tile a player or NPC is on, as tile activation last saw it (activation_system keeps it in step with Transform).
struct TileRef {
    uint16_t map = 0;
    map::TileKey tile;
};

// Every NPC is Awake or Asleep: awake while its tile is within ACTIVATION_TILES of a player on its map. chthonic.query
// skips Asleep NPCs, so Lua systems leave them be; AOI and replication treat both alike.
struct Asleep {};
struct Awake {};

// Names no Lua component may take: the Lua built-ins (read-only Character, Npc and Spawn, Transform, writable
// StateMachine)
// and every C++ component here, in lua/components.h and in lua/state_machines.h.
inline constexpr const char* kReservedComponentNames[] = {
    "Character",    "Transform",       "StateMachine", "Npc",                 "SessionLink",   "CharacterRef",
    "MoveState",    "PendingMoves",    "PendingSay",   "ChatLimit",           "PendingClientEvents", "ClientEventLimit", "PendingInteracts", "MoveLock",           "AoiCell",       "Visible",
    "Viewers",      "Moved",           "Dirty",        "SaveTimer",           "ReplicatedState", "ScriptsLeft",
    "PreservedComponents", "ComponentData", "StateMachineRuntime", "MapRef",
    "AreaRef",      "Spawn",           "SpawnRef",     "TileRef",             "Asleep",        "Awake",
    "ReplicationDirty"};

} // namespace chthonic::world
