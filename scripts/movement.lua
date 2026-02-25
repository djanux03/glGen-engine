-- movement.lua — WASD movement controller for glGen engine
-- Attach this to any entity to enable keyboard-driven movement.
-- Press LSHIFT to sprint.

local speed = 5.1       -- Base movement speed (units/sec)
local sprint_mult = 2.5  -- Sprint multiplier when holding Shift
local rot_speed = 90.0   -- Rotation speed (degrees/sec)

function on_spawn(entity)
    log.info("Movement script attached to Entity " .. entity:id() .. " (" .. entity:get_name() .. ")")
end

function on_update(entity, dt)
    local pos = entity:get_position()
    local rot = entity:get_rotation()

    -- Sprint modifier
    local current_speed = speed
    if input.key_down("LSHIFT") then
        current_speed = speed * sprint_mult
    end

    local move = current_speed * dt

    -- Forward / Backward (W/S) — move along Z axis
    if input.key_down("W") then
        pos.z = pos.z + move
    end
    if input.key_down("S") then
        pos.z = pos.z - move
    end

    -- Strafe Left / Right (A/D) — move along X axis
    if input.key_down("A") then
        pos.x = pos.x + move
    end
    if input.key_down("D") then
        pos.x = pos.x - move
    end

    -- Up / Down (SPACE / LCTRL)
    if input.key_down("SPACE") then
        pos.y = pos.y + move
    end
    if input.key_down("LCTRL") then
        pos.y = pos.y - move
    end

    -- Rotate with arrow keys
    if input.key_down("LEFT") then
        rot.y = rot.y + rot_speed * dt
    end
    if input.key_down("RIGHT") then
        rot.y = rot.y - rot_speed * dt
    end
    if input.key_down("UP") then
        rot.x = rot.x + rot_speed * dt
    end
    if input.key_down("DOWN") then
        rot.x = rot.x - rot_speed * dt
    end

    entity:set_position(pos.x, pos.y, pos.z)
    entity:set_rotation(rot.x, rot.y, rot.z)
end
