-- Sample server system: NPC patrols. The NPCs themselves come from the maps' spawn files (docs/world/npcs.html#npc-spawns); this
-- walks every one whose Spawn has a path. Its NPC definition (npcs/guard.lua, npcs/sentry.lua) gives it the "patrol"
-- machine (fsms/patrol.lua), which starts in "walking". While "walking", update moves it at SPEED m/s toward its next
-- path point, facing where it goes; on arrival it switches to "waiting" for the path's wait seconds, then walks to the
-- next point: back to the first after the last (loop), or back along the path (loop = false). A respawned NPC is a new entity and starts over at the first point.
-- On a map with a navmesh (docs/world/maps.html#navmesh) it walks chthonic.map.path's corners from where it is to the next point, so it
-- goes around cliffs; without one (or no path), straight there. The server keeps it on the ground.
--
-- Bandwidth: every chthonic.set of an NPC's Transform is one 20-byte entry per tick (20 Hz) in the EntityMoves of each player
-- that sees it (one message a tick per player, all its moves), so a walking NPC costs as much as a walking player; a
-- waiting one costs nothing. With many NPCs in a crowd, move them less
-- often (every other tick, say) and let clients interpolate.
local SPEED = 3 -- m/s, a walk

local patrols = {} -- entity -> {target = point index, step = +1 or -1, waited = seconds, route = corners, leg = index}
local order, sweep = {}, 0 -- the entities in patrols, and where the gone-sweep is in that list

-- chthonic.query skips sleeping NPCs (far from every player: docs/world/npcs.html#sleep), so an NPC missing from
-- it keeps its patrol and goes on from there when it wakes. A few patrols a tick are checked instead, and dropped
-- once their NPC is gone.
-- ponytail: 8 a tick, so a destroyed patrol's entry lingers a while; a destroy event if that ever matters.
local function sweepGone()
    for _ = 1, 8 do
        if #order == 0 then return end
        sweep = sweep % #order + 1
        local e = order[sweep]
        if not pcall(chthonic.has, e, "Spawn") then -- raises for ids that are not in the world
            patrols[e] = nil
            order[sweep] = order[#order]
            order[#order] = nil
            sweep = sweep - 1
        end
    end
end

-- The point after target, and the direction to go on in.
local function advance(spawn, p)
    local n = #spawn.path
    if spawn.loop then return p.target % n + 1, 1 end
    local step = p.step
    if p.target + step < 1 or p.target + step > n then step = -step end
    return math.max(1, math.min(n, p.target + step)), step
end

local function walk(e, spawn, p, dt)
    if chthonic.fsm.state(e) == "waiting" then
        p.waited = p.waited + dt
        if p.waited >= spawn.wait then
            p.waited = 0
            p.target, p.step = advance(spawn, p)
            p.route = nil
            chthonic.fsm.change(e, "walking")
        end
        return
    end
    local tf = chthonic.get(e, "Transform")
    local pos = tf.position
    if not p.route then
        local point = spawn.path[p.target] -- spawn files may leave y 0: the path wants it on the ground
        local to = {x = point.x, y = chthonic.map.ground(tf.map, point.x, point.z) or point.y, z = point.z}
        p.route = chthonic.map.path(tf.map, pos, to) or {point}
        p.leg = 1
    end
    local goal = p.route[p.leg]
    local dx, dz = goal.x - pos.x, goal.z - pos.z
    local dist = math.sqrt(dx * dx + dz * dz)
    local step = SPEED * dt
    if dist <= step then
        chthonic.set(e, "Transform", {position = {x = goal.x, y = pos.y, z = goal.z}})
        p.leg = p.leg + 1
        if p.leg > #p.route then
            p.route = nil
            chthonic.fsm.change(e, "waiting")
        end
    else
        chthonic.set(e, "Transform", {
            position = {x = pos.x + dx / dist * step, y = pos.y, z = pos.z + dz / dist * step},
            heading = math.atan2(dx, dz), -- the protocol's heading: 0 faces +z
        })
    end
end

return {
    update = function(dt)
        for e in chthonic.query("Spawn") do
            local spawn = chthonic.get(e, "Spawn")
            if spawn.path then
                local p = patrols[e]
                if not p then
                    p = {target = 1, step = 1, waited = 0}
                    patrols[e], order[#order + 1] = p, e
                end
                walk(e, spawn, p, dt)
            end
        end
        sweepGone()
    end,
}
