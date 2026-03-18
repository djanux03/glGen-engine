-- fps_controller.lua — First-Person Shooter controller
-- Attach to the Camera Entity. Handle WASD, Mouselook, and Raycast shooting.

local speed = 10.0
local sprint_mult = 2.0
local sensitivity = 0.12 -- mouselook sensitivity (tuned for pixel deltas)

local pitch = 0.0
local yaw = 0.0
local last_yaw = 0.0
local last_pitch = 0.0
local vel_y = 0.0

-- Character tuning
local gravity = -9.8
local max_fall_speed = -25.0
local ground_clearance = 0.9
local ground_snap = 1.2
local jump_speed = 4.5

-- Used for simple shooting cooldown so you don't fire 60 bullets a second
local last_shot_time = 0.0
local fire_rate = 0.2 -- seconds between shots

function on_spawn(entity)
    log.info("FPS Controller attached to " .. entity:get_name())
    local r = entity:get_rotation()
    pitch = r.x
    yaw = r.y
    last_yaw = yaw
    last_pitch = pitch
end

function on_update(entity, dt)
    -- 1. Mouselook handled in C++ (CoreAppLayer) for stability.
    
    -- 2. WASD Movement (relative to camera yaw direction)
    local current_speed = speed
    if input.key_down("LSHIFT") then current_speed = speed * sprint_mult end
    local move = current_speed * dt
    
    -- Use engine-consistent forward/right vectors
    local f = entity:get_forward_flat()
    local r = entity:get_right_flat()
    local forward_x = f.x
    local forward_z = f.z
    local right_x = r.x
    local right_z = r.z
    
    local vx = 0.0
    local vz = 0.0
    if input.key_down("W") then
        vx = vx + forward_x * move
        vz = vz + forward_z * move
    end
    if input.key_down("S") then
        vx = vx - forward_x * move
        vz = vz - forward_z * move
    end
    if input.key_down("A") then
        vx = vx - right_x * move
        vz = vz - right_z * move
    end
    if input.key_down("D") then
        vx = vx + right_x * move
        vz = vz + right_z * move
    end

    -- Manual gravity + ground snap
    local pos = entity:get_position()
    local hit = physics.raycast(pos.x, pos.y, pos.z, 0.0, -1.0, 0.0, 5.0, entity:id())
    local grounded = false
    if hit.hit and hit.distance <= ground_snap then
        grounded = true
        if vel_y < 0.0 then vel_y = 0.0 end
        pos.y = hit.position.y + ground_clearance
        if input.key_down("SPACE") then
            vel_y = jump_speed
            grounded = false
            pos.y = pos.y + vel_y * dt
        end
    else
        vel_y = vel_y + gravity * dt
        if vel_y < max_fall_speed then vel_y = max_fall_speed end
        pos.y = pos.y + vel_y * dt
    end

    pos.x = pos.x + vx
    pos.z = pos.z + vz
    entity:set_position(pos.x, pos.y, pos.z)
    
    -- 3. Shooting (Left click = GLFW button 0)
    last_shot_time = last_shot_time + dt
    
    if input.mouse_down(0) and last_shot_time >= fire_rate then
        last_shot_time = 0.0
        
        -- Calculate accurate forward vector from pitch and yaw
        local f3 = entity:get_forward()
        local dir_x = f3.x
        local dir_y = f3.y
        local dir_z = f3.z
        
        -- Raycast into the physics engine up to 1000 units away
        local hit = physics.raycast(pos.x, pos.y, pos.z, dir_x, dir_y, dir_z, 1000.0, entity:id())
        
        if hit.hit then
            log.info("BANG! Hit entity ID: " .. tostring(hit.entityId) .. " at distance: " .. tostring(hit.distance))
        else 
            log.info("BANG! (Missed)")
        end
    end
end
