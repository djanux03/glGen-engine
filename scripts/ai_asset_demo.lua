-- ai_asset_demo.lua — the Phase 2 scripting surface, end to end.
--
-- Everything an AI needs to author content is reachable from here: discover
-- generators, read their schemas, define an asset from parameters, place it,
-- scatter it across the terrain, and frame a shot of the result.
--
-- Run headless:
--   GLGEN_SCRIPT=scripts/ai_asset_demo.lua GLGEN_SMOKE_FRAMES=12 \
--   GLGEN_SMOKE_CAPTURE=captures/lua_demo.png
-- or paste any single line of it into the editor's Console tab.

log.info("=== glGen scripting API demo ===")

-- 1. Discover what can be generated -----------------------------------------
local gens = assets.generators()
log.info("generators: " .. table.concat(gens, ", "))
log.info("textures:   " .. table.concat(assets.texture_generators(), ", "))

-- Schemas are the same document the engine validates against, so a caller can
-- never be told something the validator disagrees with.
local schema = assets.schema("tree.v1")
local height = schema.properties.height
log.info(string.format("tree.v1 height: %s (%.1f .. %.1f, default %.1f)",
                       height.description, height.minimum, height.maximum,
                       height.default))

-- 2. Define an asset from parameters ----------------------------------------
-- A named id makes this a stable asset: defining it again regenerates it in
-- place and anything already spawned changes shape.
local birchId, warnings = assets.define{
  id = "tree/birch_script",
  generator = "tree.v1",
  seed = 20260801,
  params = {
    height = 9.0,
    trunkRadius = 0.16,
    trunkTaper = 0.6,
    crook = 0.3,
    branchLevels = 3,
    branchesPerLevel = 3,
    branchAngleDeg = 38,
    branchStartHeight = 0.42,
    canopy = "broadleaf",
    canopyDensity = 0.8,
    leafSize = 0.7,
    barkColor = { 0.78, 0.76, 0.70 },
    foliageColor = { 0.34, 0.52, 0.18 },
  },
  material = {
    bark = {
      generator = "tex.bark",
      params = { resolution = 256, ridgeDepth = 0.25, ridgeScale = 22,
                 color = { 0.78, 0.76, 0.70 } },
    },
    foliage = {
      generator = "tex.leaf",
      params = { resolution = 128, color = { 0.34, 0.52, 0.18 },
                 veinStrength = 0.5 },
    },
  },
}

if not birchId then
  log.error("assets.define failed: " .. tostring(warnings))
  return
end
log.info("defined -> " .. birchId)
for _, w in ipairs(warnings or {}) do log.warn("  " .. w) end

-- Out-of-range values are CLAMPED with a warning rather than rejected, so a
-- caller still gets a usable asset plus text describing the adjustment.
local _, clampWarnings = assets.define{
  id = "rock/oversized",
  generator = "rock.v1",
  seed = 7,
  params = { radius = 999.0 },  -- schema maximum is 20
}
for _, w in ipairs(clampWarnings or {}) do log.info("clamp check: " .. w) end

-- 3. Place it ---------------------------------------------------------------
-- Clear the default scattered forest first, so what follows is actually
-- visible rather than buried in it. This also exercises terrain.regenerate.
terrain.regenerate{ vegetation = false, grass = false }

local ground = terrain.height_at(0, 0)
local birch = world.spawn(birchId, {
  pos = { 0, ground, -8 },
  rot = { 0, 35, 0 },
  scale = 1.0,
  name = "ScriptedBirch",
  collider = "capsule",
})
log.info("spawned entity " .. birch:id() .. " at y=" .. string.format("%.1f", ground))

-- Material override on top of whatever the generator produced.
birch:set_material{ roughness = 0.95 }

-- A small ring of rocks, each its own seed: one recipe, many variants, which
-- is how variety is bought without paying for a mesh per instance.
for i = 1, 6 do
  local angle = (i / 6) * math.pi * 2
  local x, z = math.cos(angle) * 6.0, math.sin(angle) * 6.0 - 8.0
  local rockId = assets.define{
    id = "rock/ring_" .. i,
    generator = "rock.v1",
    seed = 1000 + i * 37,
    params = { radius = 0.35 + (i % 3) * 0.18, detail = 2, roughness = 0.4,
               flatCuts = i % 3 },
    material = { rock = { generator = "tex.rock",
                          params = { resolution = 128 } } },
  }
  if rockId then
    world.spawn(rockId, { pos = { x, terrain.height_at(x, z), z },
                          rot = { 0, i * 57, 0 }, name = "ScriptedRock" .. i })
  end
end

-- 4. Frame the shot ---------------------------------------------------------
render.params{
  -- Far enough back and level enough that a 9 m tree fits the frame; a
  -- near, downward-pitched camera frames its trunk and crops the crown.
  camPos = { 0, ground + 4.0, 14 },
  camYaw = 180,
  camPitch = -2,
  sunPitch = 34,
  sunYaw = 215,
  autoExposure = false,
  exposure = 0.95,
  fogDensity = 0.0015,
}

local stats = scene.stats()
log.info(string.format("scene: %d entities, %d meshes", stats.entities,
                       stats.meshes))
log.info("=== demo complete ===")
