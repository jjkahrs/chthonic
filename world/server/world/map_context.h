// One loaded map's runtime (docs/world/maps.html#map-contexts): its own registry, AOI grid, Lua state (npcs/
// and systems/), spawner, tile activation and event bus, updated by World. A world map has one (a layered one, one per
// live layer: docs/world/maps.html#layers); an instance map one per live copy
// (docs/world/instances.html), each with its instanceId and key. A context reads only the immutable Maps
// and the process-wide Shared; whatever reaches past its map (a transfer, chthonic.create or a respawn on another map,
// chthonic.world.post, a game machine change, a broadcast) goes into its Outbox, which World drains at the serial points.
#pragma once
#include "maps.h"
#include "systems.h"

#include <db/char_repo.h>
#include <lua/runtime.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>

#include <entt/entity/registry.hpp>

namespace chthonic::world {

struct WorldRules {
    MoveRules move;
    ChatRules chat;
    float cellSize = 64, viewRadius = 128;
    uint16_t startMap = 1; // START_MAP: new characters, NPCs without a map, and the fallback for a bad saved map
    size_t chatRateCount = 5;
    Millis chatRateWindow{10000};
    size_t clientEventRateCount = 10; // ClientEvents per character per window; the rest are dropped
    Millis clientEventRateWindow{1000};
    Millis saveInterval{60000};
    float tickSeconds = 0.05f; // dt passed to Lua update
    size_t maxNpcs = 10000;    // chthonic.create raises past this
    int activationTiles = 1;   // ACTIVATION_TILES: tiles around a player's that load and stay awake (Activation)
    int mapThreads = 1;        // MAP_THREADS: workers updating the map contexts; 1: all on the calling thread
    // The first instance copy's id, then one up per copy; past UINT32_MAX no copy is made. chthonic_world: one above the
    // highest id saved in characters, so a saved id never names a new copy.
    uint64_t firstInstanceId = 1;
};

// Where a transfer puts an entity. Onto an instance map, key picks the copy: the live one with that key, or a new one
// registered under it; nullopt: a new keyless copy. Onto a layered world map, layer picks the layer (refused if it is
// gone or full; with layerHint, a layerFor pick, it falls back to World's placement instead); nullopt: World's
// placement for a player, the base layer for an NPC. Within the same copy (same map, same key, same or no layer) it is
// a teleport.
struct Destination {
    uint16_t map = 0;
    proto::Vec3 pos;
    float heading = 0;
    std::optional<std::string> key = std::nullopt; // instance maps only
    std::optional<uint32_t> layer = std::nullopt;   // layered world maps only
    bool layerHint = false;
};

// What every context shares, owned by World: the process-wide entity index space, handed out in blocks (IdPool), and
// the live instance copies by key.
struct Shared {
    static constexpr uint32_t kIdBlock = 4096;  // a world map's block
    static constexpr uint32_t kCopyBlock = 256; // an instance copy's, so thousands of copies fit the index space
    // Unused indices kept for world maps and live copies: no new copy is made below this.
    // ponytail: a fixed reserve; live copies can still eat it (then generate throws). Size it by use if that ever bites.
    static constexpr uint32_t kReserve = 16 * kIdBlock;
    // The next unused block of size entity indices; empty if none is left. A copy's (size < kIdBlock) is a range a
    // destroyed copy gave back first; a world map's is always new, so its spare holds a whole kIdBlock. Locked: a
    // context past its spare block takes one mid-update.
    // ponytail: one mutex, taken once per block; never contended in practice.
    std::pair<uint32_t, uint32_t> block(uint32_t size);
    // Room for a new instance copy: two ranges given back, or kReserve unused indices.
    bool room_for_copy();
    // A destroyed context's ids, for those made after it (IdPool::retire): the ones it freed, and its blocks' rest.
    void give_back(std::vector<entt::entity> freed, std::pair<uint32_t, uint32_t> a, std::pair<uint32_t, uint32_t> b);
    // Up to kIdBlock of the freed ids given back, for a new context (IdPool::adopt).
    std::vector<entt::entity> take_freed();

    // Every live keyed copy by (map, key): its instanceId and players as of the last serial point. Written by World at
    // the serial points only, so a context reads it during an update without a lock (a transfer's full check).
    struct Copy {
        uint32_t instance = 0;
        size_t players = 0;
    };
    std::map<std::pair<uint16_t, std::string>, Copy> copies;
    // Every live layer of a layered world map by (map, instanceId; 0: the base): its players as of the last serial
    // point. Written and read as copies.
    std::map<std::pair<uint16_t, uint32_t>, size_t> layers;
    // Every character in the world (any context): its session, for chthonic.reply after a map change. Written by
    // World::spawn / despawn only, never during an update, so a context reads it without a lock.
    std::unordered_map<entt::entity, uint32_t> sessions;

private:
    std::mutex mu_;
    uint32_t next_ = 0;
    std::vector<std::pair<uint32_t, uint32_t>> ranges_; // given back: handed out before new blocks
    std::vector<entt::entity> freed_;                   // given back, versions bumped
};

// A context's entity ids. Each entity is created with one (registry::create(hint)), so an entt::entity, and the wire id
// it is, is unique across contexts, and an entity keeps it when it moves to another context. A context hands out the
// ids it freed first, the last freed first (version bumped, as entt does), then new indices from its block of Shared's.
// What a context gets depends only on what it did itself, never on the other contexts, so a parallel update hands out
// the same ids as a serial one. With one context the ids are those one registry would give.
class IdPool {
public:
    explicit IdPool(uint32_t blockSize = Shared::kIdBlock) : blockSize_(blockSize) {}
    // Throws std::length_error once the index space is used up (only past Shared::kReserve).
    entt::entity generate(Shared& shared);
    void release(entt::entity e) { freed_.push_back(entt::entt_traits<entt::entity>::next(e)); }
    // Takes a spare block if it has none, so an update can make a block's worth of new ids without asking Shared. World
    // calls it for every context, in map order, before each update step.
    void reserve(Shared& shared) {
        if (spare_.first == spare_.second) spare_ = shared.block(blockSize_);
    }
    // A destroyed instance copy's ids go back to Shared (World, at the serial point), so making and destroying copies
    // never runs out of entity indices; a new copy adopts freed ones first.
    void retire(Shared& shared) {
        shared.give_back(std::exchange(freed_, {}), std::exchange(block_, {}), std::exchange(spare_, {}));
    }
    void adopt(Shared& shared) { freed_ = shared.take_freed(); }

private:
    std::vector<entt::entity> freed_;
    std::pair<uint32_t, uint32_t> block_{}, spare_{}; // [first, second): unused indices
    uint32_t blockSize_;
};

class MapContext {
public:
    // Work for other contexts, queued during this one's update and done by World at the next serial point.
    struct Create { // chthonic.create onto another map; its id is taken, and counted here, already
        entt::entity id;
        std::string name, templateName;
        uint16_t map;
        std::optional<proto::Vec3> pos;
        float heading;
        std::string npc;
        lua::Json components;
    };
    struct Outbox {
        std::vector<std::pair<entt::entity, Destination>> transfers; // onto another map (a portal, chthonic.transfer)
        std::vector<Create> creates;
        std::vector<std::tuple<uint16_t, uint32_t, uint32_t>> respawns; // (map, instance, record): its NPC died here
        std::vector<std::pair<std::string, std::string>> gameChanges; // (machine, to): chthonic.fsm.change on a game machine
        std::vector<lua::ClientEvent> broadcasts; // chthonic.broadcast: sent here already, for the other contexts' players
        std::vector<lua::Post> posts;
        bool empty() const {
            return transfers.empty() && creates.empty() && respawns.empty() && gameChanges.empty() && broadcasts.empty() &&
                   posts.empty();
        }
    };

    // rules, maps (which holds map) and shared must outlive the context. now: the current tick's time. instance: the
    // copy's instanceId (0: a world map's context), key its instance key (nullopt: keyless or a world map).
    MapContext(uint16_t map, uint32_t instance, std::optional<std::string> key, const WorldRules& rules, const Maps& maps,
               Shared& shared, Send send, SendMany sendMany, Save save, lua::Defs defs, TimePoint now);

    uint16_t map() const { return map_; }
    uint32_t instance() const { return instance_; }
    const std::optional<std::string>& key() const { return key_; }
    // World's empty timer on a copy: since when it has had no player (nullopt while it has one, or not looked at yet).
    std::optional<TimePoint> empty_since() const { return emptySince_; }
    void set_empty_since(std::optional<TimePoint> t) { emptySince_ = t; }
    SysTime created_at() const { return createdAt_; } // wall clock (the admin list)
    size_t npcs_here() const; // the NPCs in this context's registry now
    // lua::Runtime::load, with the game machines' states (the realm's) mirrored first, so an init already reads them;
    // then every spawn file entry's "npc" must have a definition. Every error.
    std::vector<std::string> load_scripts(const std::filesystem::path& luaRoot, std::vector<std::string> gameMachines,
                                          const std::map<std::string, std::string>& gameStates);
    void shutdown_scripts(); // as World::shutdown_scripts, for this context
    std::vector<lua::SystemStatus> script_status() const { return lua_.status(); }
    bool validate_character(const std::string& name, const proto::CharOptions& options) {
        return lua_.validate_character(name, options);
    }
    EventBus& bus() { return lua_.bus(); }
    lua::StateMachines& fsm() { return lua_.fsm(); }
    entt::registry& registry() { return reg_; }
    size_t population() const { return population_; }
    IdPool& ids() { return ids_; }

    // MAX_NPCS across contexts that may update at once: each counts its own NPCs (net: created here, counting
    // chthonic.create onto another map, less destroyed here; may be negative), and World splits what is left under
    // MAX_NPCS among them (set_npc_share) before each update step. A context then counts the NPCs everywhere at that
    // point, plus its own since, plus the other contexts' shares as if used: it never takes another's share, so
    // what it may create depends on nothing they do meanwhile. With one context: exactly the NPCs everywhere.
    int64_t npcs() const { return npcs_; }
    void set_npc_share(int64_t total, size_t left, size_t share) { npcBase_ = total - npcs_ + int64_t(left - share); }
    size_t npc_count() const { return size_t(std::max<int64_t>(0, npcBase_ + npcs_)); }

    // From hold() to release_held(), sends and saves wait here (in call order) instead of going out: World holds them
    // during a parallel update, then passes them on, context by context in map order, after the barrier. Each session
    // is in one context, so it gets its messages in the order a serial update sends them. hold() runs on the worker
    // and frees what the last release passed on there, off the tick thread.
    void hold() {
        held_.sessions.clear();
        held_.msgs.clear();
        held_.saves.clear();
        holding_ = true;
    }
    void release_held();
    // ms spent in tick_move + tick_rest since the last take (World times them for the tick stats).
    void add_update_ms(double ms) { updateMs_ += ms; }
    double take_update_ms() { return std::exchange(updateMs_, 0); }

    // As World's, for an entity of (or a position on) this map. spawn: pos already checked (on the map, on its ground).
    entt::entity spawn(uint32_t sessionId, const db::CharRow& row, proto::Vec3 pos, float heading, TimePoint now,
                       const std::optional<proto::ChatServer>& chat = std::nullopt);
    void despawn(entt::entity e);
    // id: one Shared already counts (a queued Create), else a new one.
    entt::entity create_npc(std::string name, std::string templateName, std::optional<proto::Vec3> pos, float heading,
                            std::string npc, const lua::Json& components, entt::entity id = entt::null);
    void destroy_npc(entt::entity e);
    void remove_npcs();
    // A transfer within this map, as today's World::transfer (the destination is owned).
    void transfer(entt::entity e, const Destination& to);
    // The NPC of this map's spawn record died in another context: it respawns as if destroyed here.
    void spawn_destroyed(uint32_t record);
    // A player's transfer into the full copy of map with key is refused: logged, and instance.full {entity, map, key}
    // published on this context's bus (by World too, when the copy filled up after the request).
    void refuse_full(entt::entity e, uint16_t map, const std::string& key);
    // As refuse_full, for a layer of a layered world map that is full or gone: instance.full {entity, map, layer}.
    void refuse_layer(entt::entity e, uint16_t map, uint32_t layer);
    // The layer this context's systems' layerFor(e, map) pick (lua::Runtime::layer_for; e null at a login).
    std::optional<uint32_t> layer_for(entt::entity e, uint16_t map) { return lua_.layer_for(e, map); }

    // Moving e to another context, in this order (World::transfer): leave_map runs onMapLeave (and sends the client
    // events queued so far, while e can still be seen); depart takes e out of this map (MapChange, Despawns,
    // area.leave, its grid cell and tiles) but leaves its components; World moves them over; the destination's
    // arrive puts it on that map (grid, tiles, area.enter) and enter_map runs onMapEnter.
    lua::Carried leave_map(entt::entity e);
    void depart(entt::entity e, const Destination& to);
    void arrive(entt::entity e, const Destination& to);
    void enter_map(entt::entity e, const lua::Carried& carried) { lua_.enter_map(e, carried); }

    void push_move(entt::entity e, const proto::Move& m);
    void push_say(entt::entity e, proto::Say say);
    void push_client_event(entt::entity e, std::string name, nlohmann::json data, uint32_t requestId);
    void push_interact(entt::entity e, uint32_t objectId);
    // Client events from another context (the realm's, another map's broadcast) for this one's players.
    void send_client_events(std::vector<lua::ClientEvent> events);

    // The tick, split at World's serial point: movement and areas (portal transfers onto another map are queued), then
    // the rest (World::tick lists the order).
    void tick_move(TimePoint now);
    void tick_rest(TimePoint now, const Mutes& mutes, SysTime wallNow);
    Outbox take_outbox() { return std::exchange(outbox_, {}); }
    // The realm's game machine states, for chthonic.fsm here.
    void mirror_game_machines(const std::map<std::string, std::string>& states) {
        if (lua_.fsm().game_states() != states) lua_.fsm().mirror(states);
    }

private:
    // A transfer from Lua or a portal: refused (false, logged) if this process does not own the destination tile;
    // within this map (on a copy: without a key, or with this copy's) at once, onto another queued. Onto an instance map
    // the key is key (chthonic.transfer's instance =), else what the systems' instanceKey(e, map) returns; a player's into a
    // copy that was full at the last serial point (with the players this context queued for it since) is refused
    // (refuse_full). Onto a layered world map: layer (chthonic.transfer's layer =) must be live, with room for a player
    // (else refuse_layer); without it, a player's transfer onto another map asks layerFor(e, map) and keeps a pick
    // that is live with room as a hint; within the map it stays in this layer.
    bool request_transfer(entt::entity e, Destination to, std::optional<std::string> key = std::nullopt,
                          std::optional<uint32_t> layer = std::nullopt);
    bool full(uint16_t map, const std::string& key) const;
    // A live layer of a layered map with room for one more player (for an NPC: just live), counting the players this
    // context queued for it since the last serial point.
    bool layer_room(uint16_t map, uint32_t layer, bool player) const;
    entt::entity lua_create(std::string name, std::string templateName, std::optional<uint16_t> map,
                            std::optional<proto::Vec3> pos, float heading, std::string npc, const lua::Json& components);
    void leave_views(entt::entity e);
    void spawn_due(TimePoint now);
    proto::Vec3 clamp(proto::Vec3 pos) const;
    // Sends the client events Lua queued; a broadcast also goes to the outbox for the other contexts.
    void send_lua_client_events();
    entt::entity create(entt::entity id); // in the registry with id, one from ids_
    void destroy(entt::entity e);         // out of the registry, its id back to ids_

    // What hold() keeps: each message with its sessions (sessions[first, first + count)), and whether it was a SendMany.
    struct Held {
        struct Msg {
            uint32_t first, count;
            bool many;
            proto::Message msg;
        };
        std::vector<uint32_t> sessions;
        std::vector<Msg> msgs;
        std::vector<db::CharSave> saves;
    };

    uint16_t map_;
    uint32_t instance_;
    std::optional<std::string> key_;
    SysTime createdAt_ = std::chrono::system_clock::now();
    std::optional<TimePoint> emptySince_;
    const WorldRules& rules_;
    const Maps& maps_;
    Shared& shared_;
    Send out_;            // World's: the sessions
    SendMany outMany_;
    Save outSave_;
    bool holding_ = false;
    Held held_;
    Send send_;           // out_ or, while holding, held_; every system sends through these
    SendMany sendMany_;
    Save save_;
    lua::Defs defs_;
    entt::registry reg_;
    AoiGrid grid_;
    lua::Runtime lua_; // after reg_ and grid_: it holds a reference and creates NPCs through this context
    Spawner spawner_;  // every map's spawn file entries, in Maps order (SpawnRef indexes it); only this map's spawn here
    Activation activation_;
    Outbox outbox_;
    IdPool ids_{instance_ ? Shared::kCopyBlock : Shared::kIdBlock}; // after instance_
    int64_t npcs_ = 0, npcBase_ = 0; // npc_count = npcBase_ + npcs_
    double updateMs_ = 0;
    size_t population_ = 0;
    TimePoint now_{}; // the current tick's time (transfer resets move budgets to it)
};

// Moves every component of e (the C++ ones and every Lua component, persisted or not, StateMachine included) from one
// registry to another, where e is created with the same id; e is gone from `from` afterwards. defs: the Lua
// component types.
void move_entity(entt::registry& from, entt::registry& to, entt::entity e, const lua::Defs& defs);
// pos on a declared tile of map (which must be loaded), standing on its ground (within MIN_Y/MAX_Y).
proto::Vec3 clamp_to_map(const Maps& maps, const MoveRules& rules, uint16_t map, proto::Vec3 pos);

} // namespace chthonic::world
