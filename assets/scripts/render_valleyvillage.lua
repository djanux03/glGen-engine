-- =========================================================================
-- VALLEY VILLAGE SCENE
-- =========================================================================
local valley = world.spawn("gen://import.gltf/scene/valleyvillage", {
    pos = { 0.0, 0.0, 0.0 },
    rot = { 0.0, 0.0, 0.0 },
    scale = 1.0,
    name = "ValleyVillage",
})

pcall(function()
    render.params {
        camPos = { 50.0, 35.0, 60.0 },
        camYaw = -135.0,
        camPitch = -25.0,
        ambientIntensity = 0.8,
        exposure = 1.0,
    }
end)

log.info("Valley village scene spawned")
scene.save("assets/scenes/valley_village.json")
