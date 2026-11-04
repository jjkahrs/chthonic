-- Sample NPC definition: the Bat in the cave (maps/cave/.server/spawns/1_1.json). No path and no machine: it stays put.
return {
    name = "Bat",
    template = "Guard",
    components = {
        Health = { current = 10, max = 10, regen = false, title = "Bat", faction = "enemy", drops = {} },
    },
}
