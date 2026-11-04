#include "rules.h"

#include <algorithm>
#include <cmath>

namespace chthonic {

std::optional<nlohmann::json> client_event_data(const proto::ClientEvent& ev) {
    if (!proto::valid_client_event_name(ev.name) || ev.json.size() > proto::kClientEventJsonMax) return std::nullopt;
    auto data = nlohmann::json::parse(ev.json, nullptr, false);
    if (!data.is_object()) return std::nullopt; // a parse error is "discarded", not an object
    return data;
}

MoveVerdict validate_move(const MoveRules& rules, MoveBudget& budget, const proto::Vec3& from, proto::Vec3& to,
                          float ground, TimePoint now, bool onMesh) {
    const float speed = rules.maxSpeed + rules.tolerance;
    const float elapsed = static_cast<float>(std::max(0.0, seconds_between(budget.lastRefill, now)));
    budget.meters = std::min(speed * rules.burstSeconds, budget.meters + speed * elapsed);
    budget.lastRefill = now;
    const float dist = std::hypot(to.x - from.x, to.z - from.z);
    if (dist > budget.meters) return MoveVerdict::too_fast;
    if (to.y > ground + rules.maxAir && to.y >= from.y) return MoveVerdict::in_air;
    if (!onMesh) return MoveVerdict::off_mesh;
    budget.meters -= dist;
    if (to.y < ground - rules.groundTolerance) to.y = ground;
    to.y = std::clamp(to.y, rules.minY, rules.maxY);
    return MoveVerdict::accepted;
}

} // namespace chthonic
