-- Sample NPC definition (docs/world/npcs.html#npc-definitions): the Guard at the start map's gate (maps/start/.server/spawns/4_4.json).
-- It patrols its spawn's path: systems/npcs.lua moves it, the patrol machine (fsms/patrol.lua) says whether it walks or
-- waits. Health replicates to the players who see it.
return {
    name = "Guard",
    template = "Guard", -- the scene entity clients clone
    components = {
        Health = { current = 50, max = 50, title = "Gate Guard", faction = "ally" },
        StateMachine = { machine = "patrol" },
    },
}
