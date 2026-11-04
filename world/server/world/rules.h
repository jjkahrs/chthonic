// Pure validation rules applied to untrusted client input: character names and chat text (core/chat_rules.h), client
// events, movement.
#pragma once
#include "../core/chat_rules.h"
#include "../core/clock.h"

#include <messages.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace chthonic {

// valid_character_name and sanitize_chat: core/chat_rules.h, shared with the chat service.

// A ClientEvent's payload as a JSON object, or nullopt if the event is invalid: name not 1-32 chars of [a-z0-9._],
// JSON over kClientEventJsonMax bytes, not JSON, or not an object.
std::optional<nlohmann::json> client_event_data(const proto::ClientEvent& ev);

struct MoveRules {
    float maxSpeed = 10;       // m/s
    float tolerance = 1;       // m/s on top of maxSpeed
    float burstSeconds = 1;    // distance budget cap, in seconds of travel; absorbs network jitter
    float minY = -100, maxY = 500;
    float groundTolerance = 0.5f; // m below the ground accepted as-is; deeper is lifted onto it
    float maxAir = 6;             // m above the ground past which a move may not rise (no flying)
    bool navCheck = true;         // moves must end on the map's navmesh, if it has one (movement_system)
};

// Distance budget: refills at (maxSpeed + tolerance) per second of server time, capped at burstSeconds of travel.
// Sustained speed can never exceed maxSpeed + tolerance, however the client times its packets.
struct MoveBudget {
    float meters = 0;
    TimePoint lastRefill{};
};

enum class MoveVerdict { accepted, too_fast, in_air, off_mesh };

// `ground` is the ground's height under `to`. A move ending more than maxAir above it must descend (falling): level or
// rising is in_air. On accepted, `to.y` is lifted to the ground if more than groundTolerance below it, then clamped to
// [minY, maxY], and the budget is spent. Speed is measured on x/z only. `onMesh` false (movement_system: `to` is off the
// map's navmesh) is off_mesh. Map bounds (x/z on a declared tile) and the navmesh are checked by movement_system, which
// knows the maps.
// ponytail: no jump in the client yet, so maxAir is headroom; a jump arc or flying mounts need a per-entity allowance.
MoveVerdict validate_move(const MoveRules& rules, MoveBudget& budget, const proto::Vec3& from, proto::Vec3& to,
                          float ground, TimePoint now, bool onMesh = true);

} // namespace chthonic
