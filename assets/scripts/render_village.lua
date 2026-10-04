-- =========================================================================
-- COZY VILLAGE SCENE
-- =========================================================================
local village = world.spawn("gen://import.gltf/scene/village", {
    pos = { 0.0, 0.0, 0.0 },
    rot = { 0.0, 0.0, 0.0 },
    scale = 1.0,
    name = "CozyVillage",
})

pcall(function()
    render.params {
        camPos = { 25.0, 18.0, 30.0 },
        camYaw = -135.0,
        camPitch = -25.0,
        ambientIntensity = 0.8,
        exposure = 1.0,
    }
end)

log.info("Cozy village scene spawned")
scene.save("assets/scenes/cozy_village.json")
