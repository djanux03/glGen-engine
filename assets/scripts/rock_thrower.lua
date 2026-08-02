-- ============================================================================
-- rock_thrower.lua
-- glGen Engine Script: Throws physical rocks on Mouse1 click!
-- ============================================================================

function on_spawn(self)
    log.info("Rock Thrower script attached to entity: " .. self:get_name())
end

function on_update(self, dt)
    -- Check for left mouse click (Mouse 1)
    if input.mouse_pressed(0) then
        local pos = self:get_position()
        local forward = self:get_forward()

        -- Calculate spawn position 1.5 meters ahead of player view
        local spawnX = pos.x + forward.x * 1.5
        local spawnY = pos.y + 1.2 + forward.y * 1.5
        local spawnZ = pos.z + forward.z * 1.5

        -- Spawn a physical rock (Sphere primitive with dynamic mass & collider)
        local rock = world.spawn_rock(spawnX, spawnY, spawnZ, 0.45)

        if rock:is_valid() then
            -- Apply forward throwing force
            local force = 35.0
            local impulseX = forward.x * force
            local impulseY = (forward.y + 0.25) * force
            local impulseZ = forward.z * force

            rock:apply_impulse(impulseX, impulseY, impulseZ)
            log.info("Threw rock with impulse (" .. string.format("%.1f, %.1f, %.1f", impulseX, impulseY, impulseZ) .. ")")
        end
    end
end
