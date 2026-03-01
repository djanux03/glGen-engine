-- fps_controller.lua — First-Person Shooter controller
-- Attach to the Camera Entity. Handle WASD, Mouselook, and Raycast shooting.

local speed = 10.0
local sprint_mult = 2.0
local sensitivity = 15.0 -- mouselook sensitivity

local pitch = 0.0
local yaw = 0.0

-- Used for simple shooting cooldown so you don't fire 60 bullets a second
local last_shot_time = 0.0
local fire_rate = 0.2 -- seconds between shots

function on_spawn(entity)
    log.info("FPS Controller attached to " .. entity:get_name())
    local r = entity:get_rotation()
    pitch = r.x
    yaw = r.y
end

function on_update(entity, dt)
    local pos = entity:get_position()
    
    -- 1. Mouselook
    local dx = input.mouse_dx()
    local dy = input.mouse_dy()
    yaw = yaw + dx * sensitivity * dt
    pitch = pitch + dy * sensitivity * dt
    
    -- Clamp pitch to prevent flipping backwards
    if pitch > 89.0 then pitch = 89.0 end
    if pitch < -89.0 then pitch = -89.0 end
    
    entity:set_rotation(pitch, yaw, 0.0)
    
    -- 2. WASD Movement (relative to camera yaw direction)
    local current_speed = speed
    if input.key_down("LSHIFT") then current_speed = speed * sprint_mult end
    local move = current_speed * dt
    
    local yaw_rad = math.rad(yaw)
    local forward_x = math.sin(yaw_rad)
    local forward_z = math.cos(yaw_rad)
    
    local right_x = math.sin(yaw_rad - math.pi / 2)
    local right_z = math.cos(yaw_rad - math.pi / 2)
    
    if input.key_down("W") then
        pos.x = pos.x - forward_x * move
        pos.z = pos.z - forward_z * move
    end
    if input.key_down("S") then
        pos.x = pos.x + forward_x * move
        pos.z = pos.z + forward_z * move
    end
    if input.key_down("A") then
        pos.x = pos.x - right_x * move
        pos.z = pos.z - right_z * move
    end
    if input.key_down("D") then
        pos.x = pos.x + right_x * move
        pos.z = pos.z + right_z * move
    end
    
    -- Up / Down (For floating around or jumping)
    if input.key_down("SPACE") then pos.y = pos.y + move end
    if input.key_down("LCTRL") then pos.y = pos.y - move end
    
    entity:set_position(pos.x, pos.y, pos.z)
    
    -- 3. Shooting (Left click = GLFW button 0)
    last_shot_time = last_shot_time + dt
    
    if input.mouse_down(0) and last_shot_time >= fire_rate then
        last_shot_time = 0.0
        
        -- Calculate accurate forward vector from pitch and yaw
        local pitch_rad = math.rad(pitch)
        local dir_x = -math.sin(yaw_rad) * math.cos(pitch_rad)
        local dir_y = math.sin(pitch_rad)
        local dir_z = -math.cos(yaw_rad) * math.cos(pitch_rad)
        
        -- Raycast into the physics engine up to 1000 units away
        local hit = physics.raycast(pos.x, pos.y, pos.z, dir_x, dir_y, dir_z, 1000.0)
        
        if hit.hit then
            log.info("BANG! Hit entity ID: " .. tostring(hit.entityId) .. " at distance: " .. tostring(hit.distance))
        else 
            log.info("BANG! (Missed)")
        end
    end
end
