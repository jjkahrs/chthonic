-- Sample server system: the event bus and client events. On login the player gets "welcome" (chthonic.sendTo) and
-- "player.entered" goes on the bus. Its subscriber runs on the next tick, after the AOI step has sent the newcomer's
-- Spawn, and tells the newcomer and everyone who sees it "player.arrived" (chthonic.sendNearby). On logout the whole zone
-- gets "player.left" (chthonic.broadcast), before the leaver's Despawn.
--
-- Bandwidth: each client event is a message per recipient, and broadcast reaches every player in the zone. So this
-- sends only on login and logout, never per tick: in a crowd of N that is about N messages per login or logout.
--
-- It also logs the server's area events (docs/world/areas.html): "area.leave" then "area.enter" when a player crosses into
-- another area of maps/<map>/areas.json, and "area.enter" alone on login. flags are the area's, with its parent's.
local function log_area(event, p)
    local flags = {}
    for k, v in pairs(p.flags) do flags[#flags + 1] = k .. "=" .. tostring(v) end
    table.sort(flags)
    chthonic.log(string.format("%s entity %d map %d area %s (%s) flags {%s}", event, p.entity, p.map, p.area, p.name,
                           table.concat(flags, ", ")))
end

return {
    init = function()
        chthonic.subscribe("player.entered", function(p)
            chthonic.sendNearby(p.entity, "player.arrived", {entity = p.entity, name = p.name})
        end)
        chthonic.subscribe("area.leave", function(p) log_area("area.leave", p) end)
        chthonic.subscribe("area.enter", function(p) log_area("area.enter", p) end)
    end,

    onEnter = function(e)
        local name = chthonic.get(e, "Character").name
        chthonic.sendTo(e, "welcome", {name = name})
        chthonic.publish("player.entered", {entity = e, name = name})
    end,

    onLeave = function(e)
        chthonic.broadcast("player.left", {entity = e, name = chthonic.get(e, "Character").name})
    end,

    update = function(dt) end,
}
