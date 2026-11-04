-- Sample server system: every character gets Health and a Wallet. Health regenerates toward max while the character
-- stands still and drains slowly while it moves (Transform deltas). Each login pays one gold, so a relog shows the
-- Wallet persisted.
--
-- Bandwidth: every change of a replicated component is sent in full to the owner and everyone in view
-- (docs/world/protocol.html#lua-components). So this runs once a second per character, each on its own phase (no burst tick), and keeps
-- Health in whole points, written only when it changes: at most one update a second per character, not one a tick.
local PERIOD = 1             -- seconds between updates of one character
local REGEN_PER_SECOND = 2
local DRAIN_PER_METRE = 0.5

local state = {} -- entity id -> {x =, z = (position at the last update), wait = seconds to its next update}

local function position(e)
    local p = chthonic.get(e, "Transform").position
    return p.x, p.z
end

return {
    onEnter = function(e)
        if not chthonic.has(e, "Health") then chthonic.add(e, "Health") end
        if not chthonic.has(e, "Wallet") then chthonic.add(e, "Wallet") end
        chthonic.set(e, "Wallet", {gold = chthonic.get(e, "Wallet").gold + 1})
        local x, z = position(e)
        state[e] = {x = x, z = z, wait = (e % 20) / 20 * PERIOD} -- spread over the ticks of a period
    end,

    onLeave = function(e)
        state[e] = nil
    end,

    -- Each map has its own Lua state (docs/world/maps.html#map-contexts): the entry goes with the character.
    onMapLeave = function(e)
        local s = state[e]
        state[e] = nil
        return s
    end,
    onMapEnter = function(e, s) state[e] = s end,

    update = function(dt)
        for e in chthonic.query("Health") do
            local s = state[e]
            if s then
                s.wait = s.wait - dt
                if s.wait <= 0 then
                    s.wait = s.wait + PERIOD
                    local x, z = position(e)
                    local moved = math.sqrt((x - s.x) ^ 2 + (z - s.z) ^ 2)
                    s.x, s.z = x, z
                    local health = chthonic.get(e, "Health")
                    local current = health.current
                    if moved > 0 then
                        current = current - moved * DRAIN_PER_METRE
                    elseif health.regen then
                        current = current + REGEN_PER_SECOND * PERIOD
                    end
                    current = math.floor(math.max(0, math.min(health.max, current)) + 0.5)
                    if current ~= health.current then chthonic.set(e, "Health", {current = current}) end
                end
            end
        end
    end,
}
