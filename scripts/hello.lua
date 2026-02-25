-- hello.lua — Example script for glGen engine
-- Attach this to an entity via ScriptComponent to see it in action.

local speed = 2.0
local time_elapsed = 0.0

function on_spawn(entity)
    log.info("Hello from Lua! Entity " .. entity:id() .. " (" .. entity:get_name() .. ") spawned!")
    local pos = entity:get_position()
    log.info("  Starting position: " .. pos.x .. ", " .. pos.y .. ", " .. pos.z)
end

function on_update(entity, dt)
    time_elapsed = time_elapsed + dt

    -- Gentle bobbing motion
    local pos = entity:get_position()
    local bob = math.sin(time_elapsed * 2.0) * 0.01
    entity:set_position(pos.x, pos.y + bob, pos.z)

    -- Slow rotation
    local rot = entity:get_rotation()
    entity:set_rotation(rot.x, rot.y + 30.0 * dt, rot.z)

    -- React to keyboard input
    if input.key_down("T") then
        log.info("T pressed! Entity " .. entity:id() .. " says hi!")
    end
end
