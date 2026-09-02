-- =========================================================================
-- EMBER RUN
-- =========================================================================
-- A timed first-person collection game.
--
-- Embers are scattered across the procedural terrain. Run over one to
-- collect it: each pickup adds time. Collect them all before the clock
-- runs out. Miss, and the run ends.
--
-- Attach this to any entity (it does not need a mesh -- it is a manager,
-- not a prop):
--
--     local mgr = world.spawn_primitive("cube", 0, -500, 0, 0.01)
--     mgr:set_name("GameManager")
--     mgr:attach_script("assets/scripts/ember_run.lua")
--
-- The player is the entity named "Player" (Create > Player in the editor,
-- which also gives it the capsule + camera the C++ controller drives).
--
-- Scripts only run in play mode, so nothing here executes while you are
-- editing the world.
-- =========================================================================

local cfg = {
    ember_count   = 14,
    spawn_radius  = 55.0,   -- metres from origin to scatter within
    min_spacing   = 7.0,    -- keep embers from clumping
    pickup_range  = 3.0,    -- how close counts as collected
    start_time    = 45.0,
    time_bonus    = 4.0,    -- seconds granted per ember
    hover_height  = 1.5,    -- above the terrain surface
    bob_amplitude = 0.35,
    bob_speed     = 2.2,
    spin_speed    = 90.0,   -- degrees/sec
    ember_scale   = 0.9,
    -- The renderer has exactly 4 point lights (VulkanRenderer::Params), so
    -- the nearest few embers get lit. That is a budget, but it also happens
    -- to be good design: the glow strengthens as you close on one.
    glow_lights   = 4,
    glow_radius   = 11.0,
    glow_strength = 70.0,
    glow_color    = { 1.0, 0.52, 0.16 },
}

-- Runtime state. Rebuilt wholesale by reset().
local embers = {}
local state = "playing"     -- playing | won | lost
-- Seeded with the full duration rather than 0: if on_spawn ever fails, the
-- first on_update must not see a zero clock and immediately declare a loss.
local time_left = cfg.start_time
local collected = 0
local elapsed = 0.0
local banner = nil
local banner_until = 0.0
local player = nil
local run_index = 0

-- Deterministic-ish scatter that still differs run to run.
local function scatter_point(i)
    -- Golden-angle spiral: even coverage without the clumping a pure random
    -- scatter gives at this density, and no rejection-sampling loop.
    local golden = 2.399963
    local a = i * golden + math.random() * 0.6
    local r = cfg.spawn_radius * math.sqrt((i + 0.5) / cfg.ember_count)
    r = r + math.random() * cfg.min_spacing * 0.4
    return math.cos(a) * r, math.sin(a) * r
end

local function ground_at(x, z)
    -- terrain.height_at returns 0 when no terrain exists, which is a fine
    -- flat-ground fallback.
    local ok, h = pcall(function() return terrain.height_at(x, z) end)
    if ok and h then return h end
    return 0.0
end

local function clear_embers()
    for _, e in ipairs(embers) do
        if e.ent and e.ent:is_valid() then e.ent:destroy() end
    end
    embers = {}
end

local function spawn_embers()
    clear_embers()
    for i = 1, cfg.ember_count do
        local x, z = scatter_point(i)
        local base = ground_at(x, z) + cfg.hover_height
        -- collider="none": these are pickups, not physics objects. Giving
        -- them rigidbodies would make them roll down the hills before the
        -- player ever reached them.
        local ent = world.spawn("__primitive_sphere", {
            pos = { x, base, z },
            scale = cfg.ember_scale,
            name = "Ember_" .. i,
            collider = "none",
        })
        if ent and ent:is_valid() then
            embers[#embers + 1] = {
                ent = ent, x = x, z = z, base = base,
                phase = math.random() * 6.283,
                alive = true,
            }
        end
    end
    log.info("Ember Run: scattered " .. #embers .. " embers")
end

local function reset()
    -- The script sandbox opens base/math/string/table only -- there is no os
    -- library, so os.time() is not available as a seed (calling it aborts
    -- on_spawn entirely). Vary the layout by run instead.
    run_index = run_index + 1
    math.randomseed(run_index * 104729 + cfg.ember_count * 7919)
    state = "playing"
    time_left = cfg.start_time
    collected = 0
    elapsed = 0.0
    banner = "GO"
    banner_until = 1.2
    spawn_embers()
end

local function find_player()
    local p = world.find_entity("Player")
    if p and p:is_valid() then return p end
    return nil
end

-- Lights the embers nearest the player. With only four hardware point
-- lights, handing them to the closest embers makes the glow read as
-- "something is near you" rather than as static decoration.
local function update_glow(px, pz)
    local near = {}
    for _, e in ipairs(embers) do
        if e.alive then
            local dx, dz = px - e.x, pz - e.z
            near[#near + 1] = { d2 = dx * dx + dz * dz, e = e }
        end
    end
    table.sort(near, function(a, b) return a.d2 < b.d2 end)

    local lights = {}
    for i = 1, math.min(cfg.glow_lights, #near) do
        local e = near[i].e
        local pos = e.ent:is_valid() and e.ent:get_position() or nil
        lights[#lights + 1] = {
            position = { e.x, pos and pos.y or e.base, e.z },
            color = cfg.glow_color,
            radius = cfg.glow_radius,
            intensity = cfg.glow_strength,
        }
    end
    render.params { pointLights = lights }
end

function on_spawn(entity)
    log.info("=== EMBER RUN ===")
    entity:set_name("GameManager")
    reset()
end

function on_update(entity, dt)
    player = player or find_player()

    -- Restart is always available, including from the end screens.
    if input.key_pressed("R") then
        reset()
    end

    if state == "playing" then
        elapsed = elapsed + dt
        time_left = time_left - dt

        -- Animate the remaining embers: bob + spin so they read as pickups
        -- rather than scenery.
        for _, e in ipairs(embers) do
            if e.alive and e.ent:is_valid() then
                local y = e.base + math.sin(elapsed * cfg.bob_speed + e.phase)
                                   * cfg.bob_amplitude
                e.ent:set_position(e.x, y, e.z)
                local r = e.ent:get_rotation()
                e.ent:set_rotation(r.x, (r.y + cfg.spin_speed * dt) % 360, r.z)
            end
        end

        -- Collection: flat (XZ) distance, so standing under a hovering ember
        -- on a slope still counts.
        if player then
            local p = player:get_position()
            for _, e in ipairs(embers) do
                if e.alive then
                    local dx, dz = p.x - e.x, p.z - e.z
                    local dy = p.y - e.base
                    if dx * dx + dz * dz < cfg.pickup_range * cfg.pickup_range
                       and math.abs(dy) < 4.0 then
                        e.alive = false
                        if e.ent:is_valid() then e.ent:destroy() end
                        collected = collected + 1
                        time_left = time_left + cfg.time_bonus
                        banner = "+" .. math.floor(cfg.time_bonus) .. "s"
                        banner_until = 0.9
                        log.info("Ember " .. collected .. "/" .. cfg.ember_count)
                    end
                end
            end
        end

        if player then
            local p = player:get_position()
            update_glow(p.x, p.z)
        end

        if collected >= cfg.ember_count then
            state = "won"
            banner, banner_until = "ALL EMBERS COLLECTED", 999.0
            render.params { pointLights = {} }
        elseif time_left <= 0.0 then
            time_left = 0.0
            state = "lost"
            banner, banner_until = "OUT OF TIME", 999.0
            clear_embers()
            render.params { pointLights = {} }
        end
    end

    if banner_until > 0.0 then
        banner_until = banner_until - dt
        if banner_until <= 0.0 then banner = nil end
    end

    -- Declare the HUD. C++ decides how it looks (see GameHud.h).
    local hint
    if state == "playing" then
        if player then
            hint = "WASD to run  ~  Mouse to look  ~  R to restart"
        else
            hint = "No Player in the scene -- Create > Player, then press Play"
        end
    elseif state == "won" then
        hint = string.format("Cleared with %.1fs to spare  ~  R to run again",
                             time_left)
    else
        hint = string.format("You collected %d of %d  ~  R to try again",
                             collected, cfg.ember_count)
    end

    game.hud {
        title = "EMBER RUN",
        score = collected,
        total = cfg.ember_count,
        time = time_left,
        timeMax = cfg.start_time,
        banner = banner,
        bannerTone = (state == "won" and "good")
                     or (state == "lost" and "bad")
                     or "neutral",
        hint = hint,
    }
end
