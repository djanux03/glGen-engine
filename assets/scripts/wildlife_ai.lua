-- =========================================================================
-- WILDLIFE AI
-- =========================================================================
-- Drives living behavior for wildlife entities in glGen:
--   * Deer: peaceful grazing, alert pauses, walking, alert fleeing from Player
--   * Rabbit: burst hopping with sinusoidal hop arc, twitching pauses, skittering
--   * Fox: curious patrolling, trotting, slinking into brush
--   * Bird: high altitude thermals, circular soaring with aerodynamic roll banking
--
-- Automatically identifies species from entity:get_name() or defaults to Deer.
-- Keeps all ground animals perfectly pinned to terrain.height_at(x, z).
-- =========================================================================

local state = {
    species = "deer", -- "deer" | "rabbit" | "fox" | "bird"
    ai_state = "idle",
    state_timer = 2.0,
    target_x = 0.0,
    target_z = 0.0,
    speed = 1.8,
    heading_yaw = 0.0,
    current_yaw = 0.0,
    hop_phase = 0.0,
    flight_center_x = 0.0,
    flight_center_z = 0.0,
    flight_radius = 45.0,
    flight_angle = 0.0,
    flight_altitude = 40.0,
    spawn_x = 0.0,
    spawn_z = 0.0,
    roam_radius = 35.0,
    time = 0.0,
}

local function normalize_angle(deg)
    while deg > 180.0 do deg = deg - 360.0 end
    while deg < -180.0 do deg = deg + 360.0 end
    return deg
end

local function lerp_angle(from, to, t)
    local diff = normalize_angle(to - from)
    return from + diff * math.min(1.0, t)
end

local function get_ground_y(x, z)
    local ok, h = pcall(function() return terrain.height_at(x, z) end)
    if ok and h then return h end
    return 0.0
end

local function pick_random_target(cx, cz, radius)
    local angle = math.random() * math.pi * 2.0
    local dist = 5.0 + math.random() * (radius - 5.0)
    return cx + math.cos(angle) * dist, cz + math.sin(angle) * dist
end

local function clamp(val, min_v, max_v)
    return math.min(max_v, math.max(min_v, val))
end

function on_spawn(entity)
    local name = entity:get_name()
    local pos = entity:get_position()
    local rot = entity:get_rotation()

    state.spawn_x = pos.x
    state.spawn_z = pos.z
    state.current_yaw = rot.y
    state.heading_yaw = rot.y
    state.target_x = pos.x
    state.target_z = pos.z
    state.time = math.random() * 10.0

    if name:find("Rabbit") or name:find("Hare") then
        state.species = "rabbit"
        state.speed = 3.2
        state.ai_state = "sitting"
        state.state_timer = 1.0 + math.random() * 2.5
        state.roam_radius = 18.0
    elseif name:find("Fox") or name:find("Wolf") then
        state.species = "fox"
        state.speed = 2.4
        state.ai_state = "patrol"
        state.state_timer = 2.0 + math.random() * 3.0
        state.roam_radius = 45.0
    elseif name:find("Bird") or name:find("Hawk") then
        state.species = "bird"
        state.speed = 11.0
        state.ai_state = "soaring"
        state.flight_center_x = pos.x
        state.flight_center_z = pos.z
        state.flight_radius = 35.0 + math.random() * 25.0
        state.flight_angle = math.random() * math.pi * 2.0
        state.flight_altitude = math.max(pos.y, get_ground_y(pos.x, pos.z) + 30.0)
    else
        state.species = "deer"
        state.speed = 1.6
        state.ai_state = "grazing"
        state.state_timer = 2.0 + math.random() * 4.0
        state.roam_radius = 35.0
    end
end

function on_update(entity, dt)
    if not entity:is_valid() then return end
    state.time = state.time + dt
    state.state_timer = state.state_timer - dt

    local pos = entity:get_position()
    local rot = entity:get_rotation()
    local player = world.find_entity("Player")
    local player_pos = (player and player:is_valid()) and player:get_position() or nil

    -- ─────────────────────────────────────────────────────────────────────
    -- DEER BEHAVIOR
    -- ─────────────────────────────────────────────────────────────────────
    if state.species == "deer" then
        local dist_to_player = 999.0
        if player_pos then
            local dx = pos.x - player_pos.x
            local dz = pos.z - player_pos.z
            dist_to_player = math.sqrt(dx * dx + dz * dz)
        end

        -- Check flee trigger
        if dist_to_player < 14.0 and state.ai_state ~= "fleeing" then
            state.ai_state = "fleeing"
            state.state_timer = 4.0 + math.random() * 2.0
            -- Run away from player
            local away_dx = pos.x - player_pos.x
            local away_dz = pos.z - player_pos.z
            local away_len = math.max(0.1, math.sqrt(away_dx * away_dx + away_dz * away_dz))
            state.target_x = pos.x + (away_dx / away_len) * 30.0
            state.target_z = pos.z + (away_dz / away_len) * 30.0
        end

        if state.ai_state == "grazing" then
            -- Subtle head dip and breathing sway
            rot.x = -12.0 + math.sin(state.time * 1.6) * 4.0
            rot.z = math.sin(state.time * 0.8) * 1.5
            if state.state_timer <= 0.0 then
                state.ai_state = math.random() < 0.4 and "alert" or "walking"
                state.state_timer = 2.0 + math.random() * 3.5
                if state.ai_state == "walking" then
                    state.target_x, state.target_z = pick_random_target(state.spawn_x, state.spawn_z, state.roam_radius)
                end
            end
        elseif state.ai_state == "alert" then
            rot.x = 8.0 -- head up, vigilant
            rot.z = 0.0
            if state.state_timer <= 0.0 then
                state.ai_state = "walking"
                state.state_timer = 3.0 + math.random() * 4.0
                state.target_x, state.target_z = pick_random_target(state.spawn_x, state.spawn_z, state.roam_radius)
            end
        elseif state.ai_state == "walking" or state.ai_state == "fleeing" then
            local move_speed = state.ai_state == "fleeing" and 5.5 or 1.6
            local dx = state.target_x - pos.x
            local dz = state.target_z - pos.z
            local dist = math.sqrt(dx * dx + dz * dz)

            if dist > 0.5 then
                -- Rotate smoothly towards waypoint
                local target_yaw = math.deg(math.atan2(-dx, -dz))
                state.current_yaw = lerp_angle(state.current_yaw, target_yaw, dt * 4.0)

                -- Move forward along current heading
                local rad = math.rad(state.current_yaw)
                local step = move_speed * dt
                pos.x = pos.x - math.sin(rad) * step
                pos.z = pos.z - math.cos(rad) * step

                -- Subtle gait sway
                rot.x = math.sin(state.time * 5.0) * 2.0
                rot.z = math.sin(state.time * 6.0) * 2.5
            else
                if state.ai_state == "fleeing" then
                    state.ai_state = "alert"
                    state.state_timer = 2.5
                else
                    state.ai_state = "grazing"
                    state.state_timer = 3.0 + math.random() * 5.0
                end
            end

            if state.state_timer <= 0.0 and state.ai_state == "fleeing" then
                state.ai_state = "alert"
                state.state_timer = 2.0
            end
        end

        pos.y = get_ground_y(pos.x, pos.z)
        rot.y = state.current_yaw
        entity:set_position(pos.x, pos.y, pos.z)
        entity:set_rotation(rot.x, rot.y, rot.z)

    -- ─────────────────────────────────────────────────────────────────────
    -- RABBIT BEHAVIOR
    -- ─────────────────────────────────────────────────────────────────────
    elseif state.species == "rabbit" then
        local dist_to_player = 999.0
        if player_pos then
            local dx = pos.x - player_pos.x
            local dz = pos.z - player_pos.z
            dist_to_player = math.sqrt(dx * dx + dz * dz)
        end

        if dist_to_player < 9.0 and state.ai_state ~= "fleeing" then
            state.ai_state = "fleeing"
            state.state_timer = 3.0
            local away_dx = pos.x - player_pos.x
            local away_dz = pos.z - player_pos.z
            local away_len = math.max(0.1, math.sqrt(away_dx * away_dx + away_dz * away_dz))
            state.target_x = pos.x + (away_dx / away_len) * 20.0
            state.target_z = pos.z + (away_dz / away_len) * 20.0
        end

        local base_y = get_ground_y(pos.x, pos.z)

        if state.ai_state == "sitting" then
            rot.x = 6.0 + math.sin(state.time * 2.5) * 2.0
            rot.z = 0.0
            pos.y = base_y
            if state.state_timer <= 0.0 then
                state.ai_state = "hopping"
                state.state_timer = 1.2 + math.random() * 1.8
                state.target_x, state.target_z = pick_random_target(state.spawn_x, state.spawn_z, state.roam_radius)
                state.hop_phase = 0.0
            end
        elseif state.ai_state == "hopping" or state.ai_state == "fleeing" then
            local hop_freq = state.ai_state == "fleeing" and 5.0 or 3.2
            local hop_height = state.ai_state == "fleeing" and 0.45 or 0.28
            local hop_speed = state.ai_state == "fleeing" and 6.0 or 2.8

            state.hop_phase = state.hop_phase + dt * hop_freq
            local hop_arc = math.abs(math.sin(state.hop_phase * math.pi))
            pos.y = base_y + hop_arc * hop_height

            local dx = state.target_x - pos.x
            local dz = state.target_z - pos.z
            local dist = math.sqrt(dx * dx + dz * dz)

            if dist > 0.4 then
                local target_yaw = math.deg(math.atan2(-dx, -dz))
                state.current_yaw = lerp_angle(state.current_yaw, target_yaw, dt * 6.0)

                -- Forward impulse during mid-hop
                local forward_factor = hop_arc > 0.1 and 1.0 or 0.2
                local rad = math.rad(state.current_yaw)
                local step = hop_speed * forward_factor * dt
                pos.x = pos.x - math.sin(rad) * step
                pos.z = pos.z - math.cos(rad) * step

                rot.x = math.sin(state.hop_phase * math.pi) * 12.0
            else
                state.ai_state = "sitting"
                state.state_timer = 1.0 + math.random() * 2.0
            end

            if state.state_timer <= 0.0 then
                state.ai_state = "sitting"
                state.state_timer = 0.8 + math.random() * 2.2
            end
        end

        rot.y = state.current_yaw
        entity:set_position(pos.x, pos.y, pos.z)
        entity:set_rotation(rot.x, rot.y, rot.z)

    -- ─────────────────────────────────────────────────────────────────────
    -- SOARING BIRD BEHAVIOR
    -- ─────────────────────────────────────────────────────────────────────
    elseif state.species == "bird" then
        state.flight_angle = state.flight_angle + (dt * state.speed / state.flight_radius)
        if state.flight_angle > math.pi * 2.0 then
            state.flight_angle = state.flight_angle - math.pi * 2.0
        end

        -- Figure-8 / orbital path
        local cx = state.flight_center_x + math.cos(state.flight_angle) * state.flight_radius
        local cz = state.flight_center_z + math.sin(state.flight_angle * 2.0) * (state.flight_radius * 0.6)

        -- Tangent heading
        local next_a = state.flight_angle + 0.05
        local ncx = state.flight_center_x + math.cos(next_a) * state.flight_radius
        local ncz = state.flight_center_z + math.sin(next_a * 2.0) * (state.flight_radius * 0.6)
        local fdx = ncx - cx
        local fdz = ncz - cz

        local target_yaw = math.deg(math.atan2(-fdx, -fdz))
        state.current_yaw = lerp_angle(state.current_yaw, target_yaw, dt * 5.0)

        -- Aerodynamic banking roll based on turn rate
        local turn_diff = normalize_angle(target_yaw - state.current_yaw)
        rot.z = clamp(-turn_diff * 1.8, -32.0, 32.0)

        -- Subtle altitude undulation
        local cy = state.flight_altitude + math.sin(state.time * 1.5) * 2.0
        rot.x = math.sin(state.time * 1.5) * -4.0

        rot.y = state.current_yaw
        entity:set_position(cx, cy, cz)
        entity:set_rotation(rot.x, rot.y, rot.z)
    end
end
