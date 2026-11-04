-- Sample character machine, attached by lua/systems/locomotion.lua: idle <-> moving. Each state may only be entered
-- from the other one, so idle -> idle (or moving -> moving) is denied. Every enter tells the owner's client
-- "fsm.state" {machine, from, to} (from is absent on the first entry), so clients and chthonic_bot can observe it.
--
-- Bandwidth: one message to the owner per transition, never per tick.
local function tell(ctx)
    chthonic.sendTo(ctx.entity, "fsm.state", {machine = ctx.machine, from = ctx.from, to = ctx.to})
end

return {
    name = "locomotion",
    initial = "idle",
    states = {
        idle   = { allowedFrom = {"moving"}, enter = tell },
        moving = { allowedFrom = {"idle"}, enter = tell },
    },
}
