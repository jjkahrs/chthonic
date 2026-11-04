-- Sample game machine: the zone's day/night phase, listed in GAME_STATE_MACHINES and flipped by
-- lua/systems/locomotion.lua. It only logs; nothing goes to clients.
local function announce(ctx)
    chthonic.log("zone phase: " .. ctx.to .. (ctx.from and (" (was " .. ctx.from .. ")") or ""))
end

return {
    name = "zone_cycle",
    initial = "day",
    states = {
        day   = { allowedFrom = {"night"}, enter = announce },
        night = { allowedFrom = {"day"}, enter = announce },
    },
}
