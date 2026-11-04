-- Sample server system: state machines. Every character gets the "locomotion" machine (fsms/locomotion.lua) in
-- onEnter, which enters "idle" at once. update moves it to "moving" when the character's Transform changes and back to
-- "idle" after it stood still for STILL_SECONDS (so a gap between two client moves is not a stop). The zone-wide game
-- machine "zone_cycle" (fsms/zone_cycle.lua) flips between day and night every PHASE_SECONDS, if GAME_STATE_MACHINES
-- lists it. It runs in the realm context: this asks for the flip (every map's copy of this system asks at the same
-- tick, and the realm applies the same change once, at the end of the tick).
--
-- Bandwidth: the machine's enter sends one message to the owner per transition (a start or a stop), never per tick.
local STILL_SECONDS = 1
local PHASE_SECONDS = 300

local chars = {} -- entity -> { pos = last position, still = seconds without moving }
local zoneCycle = false
local phaseTime = 0

local function moved(a, b) return a.x ~= b.x or a.y ~= b.y or a.z ~= b.z end

return {
    init = function()
        -- chthonic.fsm raises for a game machine GAME_STATE_MACHINES doesn't list. The realm entered "day" at startup.
        zoneCycle = pcall(chthonic.fsm.state, "zone_cycle")
        -- A flip (from any map's copy of this system) restarts every copy's phase, so maps started later keep time too.
        chthonic.subscribe("fsm.changed", function(p)
            if p.machine == "zone_cycle" then phaseTime = 0 end
        end)
    end,

    onEnter = function(e)
        chthonic.add(e, "StateMachine")
        chthonic.set(e, "StateMachine", {machine = "locomotion", initialState = "idle"}) -- enters "idle" now
        -- Denied: "idle" may only be entered from "moving". Returns false and logs a warning; no enter, no event.
        chthonic.fsm.change(e, "idle")
        chars[e] = {pos = chthonic.get(e, "Transform").position, still = 0}
    end,

    onLeave = function(e) chars[e] = nil end, -- the machine goes away with the character, without an exit

    -- Each map has its own Lua state (docs/world/maps.html#map-contexts): a character's entry goes with it to
    -- the next map's, and its StateMachine (a component) keeps its state.
    onMapLeave = function(e)
        local c = chars[e]
        chars[e] = nil
        return c
    end,
    onMapEnter = function(e, c) chars[e] = c end,

    update = function(dt)
        for e, c in pairs(chars) do
            local pos = chthonic.get(e, "Transform").position
            if moved(pos, c.pos) then
                c.still = 0
                if chthonic.fsm.state(e) == "idle" then chthonic.fsm.change(e, "moving") end
            else
                c.still = c.still + dt
                if c.still >= STILL_SECONDS and chthonic.fsm.state(e) == "moving" then chthonic.fsm.change(e, "idle") end
            end
            c.pos = pos
        end

        if zoneCycle then
            phaseTime = phaseTime + dt
            if phaseTime >= PHASE_SECONDS then
                phaseTime = 0
                chthonic.fsm.change("zone_cycle", chthonic.fsm.state("zone_cycle") == "day" and "night" or "day")
            end
        end
    end,
}
