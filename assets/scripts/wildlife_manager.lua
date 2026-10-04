-- =========================================================================
-- WILDLIFE & CAMPSITE MANAGER
-- =========================================================================
-- Spawns and manages a lively wilderness scene in glGen:
--   * An established exploration basecamp (campfire with flickering light,
--     canvas tent, log bench, storage crates, barrels, storm lantern, signpost)
--   * Herds of procedural deer grazing peacefully
--   * Fluffy rabbits hopping through the meadows
--   * A red fox patrolling the woodline
--   * Hawks soaring through the skies overhead
--
-- Can be executed standalone via GLGEN_SCRIPT or attached to a GameManager.
-- =========================================================================

local function get_ground_y(x, z)
    local ok, h = pcall(function() return terrain.height_at(x, z) end)
    if ok and h then return h end
    return 0.0
end

local function ensure_terrain()
    local ok, exists = pcall(function() return terrain.exists() end)
    if ok and not exists then
        pcall(function()
            terrain.regenerate {
                seed = 1337,
                heightScale = 1.0,
                viewDistanceChunks = 8,
                vegetation = true,
                grass = true,
            }
        end)
    end
end

local camp = {
    x = 4.0,
    z = -10.0,
    spawned = false,
    campfire_pos = nil,
    flicker_time = 0.0,
}

local wildlife_entities = {}

local function spawn_camp(cx, cz)
    local cy = get_ground_y(cx, cz)
    camp.campfire_pos = { x = cx, y = cy, z = cz }

    -- 1. Campfire
    local fire = world.spawn("gen://kitbash.v1/prop/campfire", {
        pos = { cx, cy, cz },
        rot = { 0, 15, 0 },
        scale = 1.0,
        name = "Campfire",
        collider = "box",
        onGround = true,
    })

    -- 2. Expedition Tent
    local tent_x = cx - 3.8
    local tent_z = cz - 2.5
    local tent_y = get_ground_y(tent_x, tent_z)
    world.spawn("gen://kitbash.v1/prop/tent", {
        pos = { tent_x, tent_y, tent_z },
        rot = { 0, 48, 0 },
        scale = 1.0,
        name = "CampTent",
        collider = "box",
        onGround = true,
    })

    -- 3. Log Bench
    local bench_x = cx + 2.4
    local bench_z = cz - 0.6
    local bench_y = get_ground_y(bench_x, bench_z)
    world.spawn("gen://kitbash.v1/prop/log_bench", {
        pos = { bench_x, bench_y, bench_z },
        rot = { 0, -65, 0 },
        scale = 1.0,
        name = "CampBench",
        collider = "box",
        onGround = true,
    })

    -- 4. Supply Crates & Barrels
    local crate_x = cx - 2.2
    local crate_z = cz + 2.6
    local crate_y = get_ground_y(crate_x, crate_z)
    world.spawn("gen://kitbash.v1/prop/crate", {
        pos = { crate_x, crate_y, crate_z },
        rot = { 0, 22, 0 },
        scale = 0.9,
        name = "SupplyCrate1",
        collider = "box",
        onGround = true,
    })

    local barrel_x = cx - 3.2
    local barrel_z = cz + 2.2
    local barrel_y = get_ground_y(barrel_x, barrel_z)
    world.spawn("gen://kitbash.v1/prop/barrel", {
        pos = { barrel_x, barrel_y, barrel_z },
        rot = { 0, -10, 0 },
        scale = 0.95,
        name = "WaterBarrel",
        collider = "cylinder",
        onGround = true,
    })

    -- 5. Storm Lantern on crate
    world.spawn("gen://kitbash.v1/prop/lantern", {
        pos = { crate_x, crate_y + 0.85, crate_z },
        rot = { 0, 35, 0 },
        scale = 1.0,
        name = "CampLantern",
        collider = "none",
    })

    -- 6. Trail Signpost
    local sign_x = cx + 5.5
    local sign_z = cz + 4.5
    local sign_y = get_ground_y(sign_x, sign_z)
    world.spawn("gen://kitbash.v1/prop/signpost", {
        pos = { sign_x, sign_y, sign_z },
        rot = { 0, -35, 0 },
        scale = 1.0,
        name = "TrailSignpost",
        collider = "box",
        onGround = true,
    })

    -- 7. Trail Marker Cairn
    local cairn_x = cx + 8.0
    local cairn_z = cz - 6.0
    local cairn_y = get_ground_y(cairn_x, cairn_z)
    world.spawn("gen://kitbash.v1/prop/cairn", {
        pos = { cairn_x, cairn_y, cairn_z },
        rot = { 0, 18, 0 },
        scale = 1.0,
        name = "TrailCairn",
        collider = "box",
        onGround = true,
    })

    -- 8. Forest Mushrooms near tree base
    local mush_x = cx - 5.0
    local mush_z = cz + 5.0
    local mush_y = get_ground_y(mush_x, mush_z)
    world.spawn("gen://kitbash.v1/prop/mushrooms", {
        pos = { mush_x, mush_y, mush_z },
        rot = { 0, 55, 0 },
        scale = 1.1,
        name = "MushroomPatch",
        collider = "none",
        onGround = true,
    })

    log.info("Wilderness Camp created at (" .. string.format("%.1f, %.1f", cx, cz) .. ")")
end

local function spawn_wildlife(cx, cz)
    -- 1. Herd of Deer in the meadow clearing
    local deer_offsets = {
        { x = 12.0, z = -14.0, rot = 45.0, scale = 1.05 },
        { x = 16.0, z = -18.0, rot = 110.0, scale = 0.95 },
        { x = 10.0, z = -22.0, rot = -20.0, scale = 1.0 },
    }
    for i, d in ipairs(deer_offsets) do
        local wx = cx + d.x
        local wz = cz + d.z
        local wy = get_ground_y(wx, wz)
        local deer = world.spawn("gen://kitbash.v1/wildlife/deer", {
            pos = { wx, wy, wz },
            rot = { 0, d.rot, 0 },
            scale = d.scale,
            name = "Deer_" .. i,
            collider = "box",
            onGround = true,
        })
        if deer and deer:is_valid() then
            deer:attach_script("assets/scripts/wildlife_ai.lua")
            table.insert(wildlife_entities, deer)
        end
    end

    -- 2. Warren of Rabbits in the grass
    local rabbit_offsets = {
        { x = -6.0, z = -10.0, rot = 15.0 },
        { x = -8.5, z = -12.0, rot = -70.0 },
        { x = -5.0, z = -14.5, rot = 135.0 },
        { x = -11.0, z = -8.0, rot = 90.0 },
    }
    for i, r in ipairs(rabbit_offsets) do
        local wx = cx + r.x
        local wz = cz + r.z
        local wy = get_ground_y(wx, wz)
        local rabbit = world.spawn("gen://kitbash.v1/wildlife/rabbit", {
            pos = { wx, wy, wz },
            rot = { 0, r.rot, 0 },
            scale = 1.0,
            name = "Rabbit_" .. i,
            collider = "none",
            onGround = true,
        })
        if rabbit and rabbit:is_valid() then
            rabbit:attach_script("assets/scripts/wildlife_ai.lua")
            table.insert(wildlife_entities, rabbit)
        end
    end

    -- 3. Red Fox patrolling near the forest edge
    local fox_x = cx + 18.0
    local fox_z = cz + 10.0
    local fox_y = get_ground_y(fox_x, fox_z)
    local fox = world.spawn("gen://kitbash.v1/wildlife/fox", {
        pos = { fox_x, fox_y, fox_z },
        rot = { 0, -110.0, 0 },
        scale = 1.0,
        name = "Fox_1",
        collider = "box",
        onGround = true,
    })
    if fox and fox:is_valid() then
        fox:attach_script("assets/scripts/wildlife_ai.lua")
        table.insert(wildlife_entities, fox)
    end

    -- 4. Soaring Birds circling high above
    local bird_offsets = {
        { x = cx + 5.0, z = cz - 10.0, alt = 38.0, scale = 1.0 },
        { x = cx - 15.0, z = cz + 15.0, alt = 48.0, scale = 1.15 },
    }
    for i, b in ipairs(bird_offsets) do
        local by = get_ground_y(b.x, b.z) + b.alt
        local bird = world.spawn("gen://kitbash.v1/wildlife/bird", {
            pos = { b.x, by, b.z },
            rot = { 0, math.random() * 360.0, 0 },
            scale = b.scale,
            name = "Hawk_" .. i,
            collider = "none",
        })
        if bird and bird:is_valid() then
            bird:attach_script("assets/scripts/wildlife_ai.lua")
            table.insert(wildlife_entities, bird)
        end
    end

    log.info("Spawned wildlife: " .. #wildlife_entities .. " living creatures populated")
end

local function setup_camera(cx, cz)
    local cam_x = cx - 0.5
    local cam_z = cz + 6.8
    local cam_y = get_ground_y(cam_x, cam_z) + 2.2
    pcall(function()
        render.params {
            camPos = { cam_x, cam_y, cam_z },
            camYaw = 190.0,
            camPitch = -11.0,
        }
    end)
end

local function init_world()
    ensure_terrain()
    spawn_camp(camp.x, camp.z)
    spawn_wildlife(camp.x, camp.z)
    setup_camera(camp.x, camp.z)
    camp.spawned = true
end

-- If executed as a standalone script:
init_world()

-- Entity script hooks for manager lifecycle
function on_spawn(entity)
    entity:set_name("WildlifeManager")
    if not camp.spawned then
        init_world()
    end
end

function on_update(entity, dt)
    camp.flicker_time = camp.flicker_time + dt
    if camp.campfire_pos then
        -- Dynamic realistic campfire flicker
        local t = camp.flicker_time
        local flicker = math.sin(t * 11.0) * 8.0 + math.cos(t * 23.0) * 6.0 + (math.random() - 0.5) * 4.0
        local base_intensity = 55.0
        local intensity = math.max(25.0, base_intensity + flicker)

        render.params {
            pointLights = {
                {
                    position = { camp.campfire_pos.x, camp.campfire_pos.y + 0.45, camp.campfire_pos.z },
                    color = { 1.0, 0.52, 0.16 },
                    radius = 14.0,
                    intensity = intensity,
                },
                {
                    position = { camp.x - 2.2, get_ground_y(camp.x - 2.2, camp.z + 2.6) + 1.1, camp.z + 2.6 },
                    color = { 1.0, 0.85, 0.42 },
                    radius = 8.0,
                    intensity = 18.0,
                }
            }
        }
    end
end
