-- =========================================================================
-- VIRTUAL CITY SCENE DISPLAY
-- =========================================================================
local city = world.spawn("gen://import.gltf/scene/virtual_city", {
    pos = { 0.0, 0.0, 0.0 },
    rot = { 0.0, 0.0, 0.0 },
    scale = 1.0,
    name = "VirtualCityBlock",
})

-- Place camera overlooking the city avenues and buildings
pcall(function()
    render.params {
        camPos = { 35.0, 22.0, 35.0 },
        camYaw = 225.0,
        camPitch = -24.0,
        sunIntensity = 1.8,
        sunPitch = 40.0,
        sunYaw = 140.0,
        shadowStrength = 1.0,
        ambientIntensity = 0.8,
    }
end)

log.info("VirtualCity scene spawned successfully")
scene.save("assets/scenes/virtual_city.json")
