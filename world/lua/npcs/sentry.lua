-- Sample NPC definition: the Sentry on the hills (maps/hills/.server/spawns/0_1.json), patrolling round the mesa.
return {
    name = "Sentry",
    template = "Guard",
    components = {
        Health = { current = 80, max = 80, title = "Sentry", faction = "neutral" },
        StateMachine = { machine = "patrol" },
    },
}
