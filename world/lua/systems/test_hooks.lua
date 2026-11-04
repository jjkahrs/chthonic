-- Sample test hooks for the bot. Remove this file from a real game's lua/systems/.
-- Scenario 18 ("respawn"): a character whose name starts with "Respawntest" destroys the sample Guard (spawn
-- "guard_1", maps/start/.server/spawns/4_4.json) when it enters, so the bot can watch it respawn respawnSec later.
-- Scenarios 21 and 22 ("client-events", "client-event-rate"): every "client.test.echo" event (chthonic.world.send's
-- ClientEvent) goes back to its sender as "test.echo" {n = data.n, entity = sender}.
-- Scenario 25 ("objects"): the hills' Test Shrine (maps/hills/.server/interactables/0_1.json) publishes "test.shrine"
-- when a player activates it in range; it goes back to that player with what the server put in the event.
-- Scenario 41 ("instances") and load.sh's --instance-size: "client.test.instance_key" {key} sets the instance key the
-- crypt's copy is picked by (instanceKey below; docs/world/instances.html) for the sender's next transfer
-- into an instance map; instance.full goes back to the refused player as "test.instance_full" {map, key}.
local instanceKeys = {} -- by entity, until it leaves this map
return {
    init = function()
        chthonic.subscribe("client.test.instance_key", function(e)
            local key = e.data.key -- the client's: only a short string is used
            instanceKeys[e.entity] = type(key) == "string" and #key > 0 and #key <= 64 and key or nil
        end)
        chthonic.subscribe("instance.full", function(p)
            chthonic.sendTo(p.entity, "test.instance_full", {map = p.map, key = p.key})
        end)
        chthonic.subscribe("client.test.echo", function(e)
            local n = type(e.data.n) == "number" and e.data.n or -1 -- data is the client's: never trusted
            chthonic.sendTo(e.entity, "test.echo", {n = n, entity = e.entity})
        end)
        chthonic.subscribe("test.shrine", function(e)
            chthonic.sendTo(e.entity, "test.shrine", {entity = e.entity, map = e.map, object = e.object, msg = e.data and e.data.msg})
        end)
    end,
    onEnter = function(e)
        if chthonic.get(e, "Character").name:sub(1, 11) ~= "Respawntest" then return end
        for npc in chthonic.query("Spawn") do
            if chthonic.get(npc, "Spawn").id == "guard_1" then chthonic.destroy(npc) end
        end
    end,
    instanceKey = function(e, map) return instanceKeys[e] end,
    onMapLeave = function(e) instanceKeys[e] = nil end,
    onLeave = function(e) instanceKeys[e] = nil end,
    update = function() end,
}
