-- physics_test.lua — Example script for glGen Engine Jolt Physics
-- Attach this script + a Rigidbody + a Collider to a bouncing cube!

local time_elapsed = 0.0

function on_spawn(entity)
    log.info("Physics Cube Spawned!")
    
    -- We can set initial velocities using our new bindings!
    entity:set_velocity(0.0, -10.0, 0.0)
end

function on_update(entity, dt)
    time_elapsed = time_elapsed + dt

    -- React to keyboard input: jump!
    if input.key_down("SPACE") then
        -- Add upward impulse
        entity:apply_impulse(0.0, 50.0, 0.0)
    end
    
    -- React to keyboard input: move forward/back
    if input.key_down("W") then
        entity:apply_impulse(0.0, 0.0, -10.0)
    elseif input.key_down("S") then
        entity:apply_impulse(0.0, 0.0, 10.0)
    end

    -- Move left/right
    if input.key_down("A") then
        entity:apply_impulse(-10.0, 0.0, 0.0)
    elseif input.key_down("D") then
        entity:apply_impulse(10.0, 0.0, 0.0)
    end
end
