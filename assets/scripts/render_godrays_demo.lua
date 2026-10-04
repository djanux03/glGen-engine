-- =========================================================================
-- SUNKEN PILLARS SCENE
-- =========================================================================
local pillars = world.spawn("gen://import.gltf/scene/godrays_demo", {
    pos = { 0.0, 0.0, 0.0 },
    rot = { 0.0, 0.0, 0.0 },
    scale = 1.0,
    name = "SunkenPillars",
})

pcall(function()
    render.params {
        camPos = { 15.0, 10.0, 20.0 },
        camYaw = -135.0,
        camPitch = -20.0,
        ambientIntensity = 0.8,
        exposure = 1.0,
    }
end)

log.info("Sunken pillars scene spawned")
scene.save("assets/scenes/sunken_pillars.json")
