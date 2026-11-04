// The world: one MapContext per loaded world map (maps.h), each with its own registry, AOI grid, Lua systems, NPC spawns
// and tile activation, created the first time a map is used (START_MAP's when the scripts load), one per live copy of
// an instance map (docs/world/instances.html: made on entry, by instance key, destroyed once empty for the
// map's emptyTimeoutSec), one per live layer of a layered world map (docs/world/maps.html#layers: the base
// one always, extra ones as copies, made when every live layer is full, destroyed like a copy), plus the realm context, whose Lua state runs only the game state machines. Each tick updates the map contexts in steps, with a serial point
// between the steps where the work the contexts queued for one another is done (docs/world/maps.html#map-contexts).
// With rules.mapThreads > 1 the contexts of a step update at once on that many workers, with a barrier after each step;
// they share nothing mutable meanwhile, so the result is the serial one (docs/world/maps.html#threading).
// Talks to the outside only through Send (to a session), SendMany (one message to several) and Save (to the DB
// worker), always on the calling thread. Each player and NPC is on one map (MapRef), in that map's context; only
// transfer() changes it. Entity ids are process-wide (Shared, IdPool), so the ids on the wire and in Lua stay unique
// and stable across contexts.
#pragma once
#include "../core/worker_pool.h"
#include "map_context.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>

namespace chthonic::world {

class World {
public:
    // A live context, for the admin port's instance list.
    struct ContextInfo {
        uint16_t map = 0;
        uint32_t instance = 0;          // 0: a world map's context
        std::optional<std::string> key; // a keyed copy's
        size_t players = 0, npcs = 0;
        SysTime createdAt{};
        std::optional<SysTime> emptySince; // a copy's empty timer, running since then (wall clock)
    };

    // maps must outlive the World and hold rules.startMap. defs: the Lua component types (lua/components.h); their
    // instances are restored, saved and replicated here.
    World(WorldRules rules, const Maps& maps, Send send, SendMany sendMany, Save save, lua::Defs defs = {});
    ~World();

    // Loads the state machine definitions under luaRoot/fsms/ and starts the game machines (by name) in the realm
    // context, then, in every map context (START_MAP's, created now, and any created later), fsms/, the NPC
    // definitions under luaRoot/npcs/ and the Lua systems under luaRoot/systems/, and runs their init; every error,
    // each once, empty if all loaded. A spawn whose "npc" has no definition is one too.
    std::vector<std::string> load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines = {});
    // Graceful shutdown, before the despawns: onLeave for every character, then every system's shutdown, then the
    // client events they queued, in every context. Undelivered bus events are dropped.
    void shutdown_scripts();
    // Per system: ok only if ok in every context; the first context's error.
    std::vector<lua::SystemStatus> script_status() const;
    // The Lua systems' validateCharacter verdict on C++-checked options (lua::Runtime::validate_character), asked in
    // START_MAP's context.
    bool validate_character(const std::string& name, const proto::CharOptions& options) {
        return context(rules_.startMap).validate_character(name, options);
    }
    // A map context's event bus, state machines (its characters' and NPCs'; game machines mirrored) and registry, for
    // server C++ and tests: START_MAP's, or a world map's (created on first use; an instance map has none: throws).
    EventBus& bus() { return bus(rules_.startMap); }
    EventBus& bus(uint16_t map) { return context(map).bus(); }
    lua::StateMachines& fsm() { return fsm(rules_.startMap); }
    lua::StateMachines& fsm(uint16_t map) { return context(map).fsm(); }
    entt::registry& registry() { return registry(rules_.startMap); }
    entt::registry& registry(uint16_t map) { return context(map).registry(); }
    // A live copy of an instance map, or an extra layer of a layered world map, or null (tests).
    MapContext* copy(uint16_t map, uint32_t instance) const;
    Shared& shared() { return shared_; } // tests: use up the entity index space
    // Every live context, in map then instanceId order.
    std::vector<ContextInfo> contexts() const;
    // The registry of the context e is in, or null if e is not in the world (tests).
    entt::registry* registry_of(entt::entity e) {
        MapContext* ctx = find(e);
        return ctx ? &ctx->registry() : nullptr;
    }
    // The realm context's bus (its fsm.changed events, posts to "realm") and state machines (the game machines).
    EventBus& realm_bus() { return realm_.bus(); }
    lua::StateMachines& realm_fsm() { return realm_.fsm(); }

    // Places the character (at its saved map and position, lifted onto the ground if it is under it, or START_MAP's
    // spawn if that map is not loaded or the position is off it) in that map's context (on an instance map: the saved
    // copy if it is live and has room, else the map's exit, logged; on a layered map: login_layer), sends
    // EnterWorldOk, wakes the tiles around it (spawning the NPCs of those loading now, so onEnter sees them), restores
    // its persisted Lua components and runs onEnter. Other players get Spawn on the next tick.
    entt::entity spawn(uint32_t sessionId, const db::CharRow& row, TimePoint now);
    // Runs onLeave and sends the client events it queued, saves the character, sends Despawn to everyone who could
    // see it, removes it.
    void despawn(entt::entity e);
    // An NPC (chthonic.create, a spawn) on map (default START_MAP, must be loaded) at pos (clamp_to_map: on the ground), or
    // the map's spawn point (on the ground too), made from definition npc ("" for none) with components ({"<Type>":
    // valid value}; a StateMachine enters its initial state now). Players in range get its Spawn on the next tick. It
    // has no session and is never saved.
    entt::entity create_npc(std::string name, std::string templateName, std::optional<uint16_t> map,
                            std::optional<proto::Vec3> pos, float heading, std::string npc = "",
                            const lua::Json& components = lua::Json::object());
    // Sends Despawn to every player who sees the NPC, removes it. One from a spawn file (SpawnRef) comes back
    // respawnSec after this (never for -1), asleep or not, on the map of its spawn file.
    void destroy_npc(entt::entity e);
    // Graceful shutdown, after the players are gone: removes every NPC; nothing to send, nothing to save.
    void remove_npcs();
    // pos on a declared tile of map (which must be loaded), standing on its ground (within MIN_Y/MAX_Y).
    proto::Vec3 clamp_to_map(uint16_t map, proto::Vec3 pos) const { return world::clamp_to_map(maps_, rules_.move, map, pos); }
    // Every map change goes through here (player or NPC), and teleports within a map too; a context's own requests
    // (portals, chthonic.transfer onto another map) come here at the serial points. Refused (false, logged) if this
    // process does not own the destination tile, or if a player's destination is a full instance copy (instance.full
    // on the source context's bus; nothing moves), or a chthonic.transfer layer = that is full or gone (likewise). Onto an
    // instance map, to.key picks the copy (Destination); a new one is created here. Onto a layered map, layer_dest
    // picks the layer. Otherwise moves e at once onto the ground at the destination and
    // takes it out of every view: the player gets MapChange, then a Despawn per old neighbour, who get its Despawn,
    // and its area is updated (area events). Onto another map, in this order: onMapLeave in the old context, the
    // MapChange, Despawns and area.leave there, every component moved to the new context, area.enter and onMapEnter
    // there. The next aoi pass sends the Spawns for the new position: this tick's for a portal (World::tick moves
    // it before the aoi pass), the next tick's for chthonic.transfer.
    bool transfer(entt::entity e, const Destination& to);

    void push_move(entt::entity e, const proto::Move& m);
    void push_say(entt::entity e, proto::Say say);
    // The chat service's mutes (via the chat link): chat_system drops a muted character's says, yells and emotes.
    Mutes& mutes() { return mutes_; }
    // The map e is on (MapRef); e must be in the world.
    uint16_t map_of(entt::entity e) const;
    // A ClientEvent GameServer already checked (valid name, JSON object within kClientEventJsonMax).
    void push_client_event(entt::entity e, std::string name, nlohmann::json data, uint32_t requestId = 0);
    // An Interact; checked by interact_system on the next tick.
    void push_interact(entt::entity e, uint32_t objectId);

    void tick(TimePoint now);
    size_t population() const;
    // Each map context's update time in the last tick (ms; tick_move + tick_rest), in map order: the tick stats.
    const std::vector<double>& context_update_ms() const { return contextMs_; }

private:
    using Key = std::pair<uint16_t, uint32_t>; // (map, instanceId); 0 for a world map
    // A world map's context, created (and its scripts loaded, once load_scripts has run) on first use. An instance map
    // has none: throws std::logic_error.
    MapContext& context(uint16_t map);
    MapContext& context(uint16_t map) const { return const_cast<World*>(this)->context(map); }
    // Creates a context (a world map's, or a new copy with a fresh instanceId), its scripts loaded as context's.
    MapContext& create(uint16_t map, uint32_t instance, std::optional<std::string> key);
    // An instance map's copy for key: the live one registered under it, else a new one (always for nullopt); null if
    // a new one is needed and the instance ids are exhausted.
    MapContext* copy_for(uint16_t map, const std::optional<std::string>& key);
    // A new copy (an instance copy, or a layer: key nullopt) with the next instanceId; null if the instance ids are
    // exhausted or the entity ids low (Shared::room_for_copy), no_copy_reason() says which.
    MapContext* new_copy(uint16_t map, const std::optional<std::string>& key);
    const char* no_copy_reason() const;
    // Layered world map's placement for an arriving player (docs/world/maps.html#layers): the hint if it is a
    // live layer with room, else the fullest live layer with room, else a new layer, else (no new copy can be made)
    // the fullest, over its cap, logged. Never refuses.
    MapContext& layer_for(uint16_t map, std::optional<uint32_t> hint);
    // The layer of layered map to.map that e (in from) moves to: an NPC the one named if live, else the base; a player
    // layer_for, unless to.layer is a chthonic.transfer layer = (not a hint): then that layer if live with room, else
    // null, refused (refuse_layer on from).
    MapContext* layer_dest(entt::entity e, MapContext& from, const Destination& to);
    // A login onto layered map `map`: the saved layer (row.instanceId, saved on this map) if live with room, else
    // layer_for with the base layer's layerFor(nil, map) as the hint.
    MapContext& login_layer(const db::CharRow& row, uint16_t map);
    // The context e is in, or null.
    // ponytail: a scan over the contexts, a valid() check each; an id -> context index if thousands of copies live.
    MapContext* find(entt::entity e) const;
    // The serial point's empty timers: every copy with no player starts (or keeps) its timer, one with a player stops
    // it; one whose timer ran past its map's emptyTimeoutSec runs its systems' shutdown, loses its NPCs and its key
    // and is returned, to be erased at the end of the serial point (after its outbox).
    std::vector<Key> expire_copies();
    void refresh_copies(); // Shared::copies' player counts
    bool move_to(entt::entity e, const Destination& to); // transfer, without the serial point after it
    // The serial point: what the contexts (and the realm) queued since the last one, in map order: transfers,
    // creates, respawns, game machine changes (applied in the realm, each distinct one once), the realm's client
    // events and the broadcasts for the other contexts' players; then the game machine mirrors are refreshed. Posts
    // are collected and, at the end of a tick only, published on their targets' buses, so they are delivered on the
    // next tick.
    void serial(bool endOfTick);
    void deliver(const nlohmann::json& from, const lua::Post& post);
    // One update step: step(ctx) for every context, on the workers if there are any (then the sends and saves they
    // held are passed on after the barrier, in map order), else one after another here. Each context first gets its
    // spare id block and its share of MAX_NPCS, in map order: the same either way.
    void update(const std::function<void(MapContext&)>& step);
    void share_npcs();    // MapContext::set_npc_share for every context
    int64_t npcs() const; // every context's NPCs (and those on their way to one)

    WorldRules rules_;
    const Maps& maps_;
    Send send_;
    SendMany sendMany_;
    Save save_;
    lua::Defs defs_;
    Shared shared_;
    std::map<Key, std::unique_ptr<MapContext>> contexts_; // after shared_ (they hold a reference)
    uint64_t nextInstance_; // > UINT32_MAX: ids exhausted
    int64_t retiredNpcs_ = 0; // destroyed copies' npcs(): their net share of the NPC count
    bool scriptsShutDown_ = false;
    entt::registry realmReg_; // empty: the realm has no entities
    lua::Runtime realm_;      // fsms/ and the game machines only
    EventBus::Subscription realmChanged_; // the game machines' fsm.changed, forwarded to every map context's bus
    std::vector<lua::Post> realmPosts_;   // chthonic.world.post from the realm's Lua, since the last serial point
    std::vector<std::pair<nlohmann::json, lua::Post>> posts_; // (from, post), until the end of the tick
    Mutes mutes_;
    std::optional<std::filesystem::path> luaRoot_; // set by load_scripts: contexts created later load from it
    std::vector<std::string> gameMachines_;
    TimePoint now_{}; // the current tick's time
    std::vector<double> contextMs_;
    std::vector<MapContext*> stepContexts_; // the contexts of the running update step
    std::unique_ptr<WorkerPool> pool_; // null with mapThreads 1; last: its workers stop before the contexts go
};

} // namespace chthonic::world
