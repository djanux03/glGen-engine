-- =========================================================================
-- SCI-FI SPACESHIP HALLWAY SCENE
-- =========================================================================
local hallway = world.spawn("gen://import.gltf/scene/space_ship_hallway", {
    pos = { 0.0, 0.0, 0.0 },
    rot = { 0.0, 0.0, 0.0 },
    scale = 1.0,
    name = "SpaceShipCorridor",
})

-- Position camera at eye height inside the hallway looking down the corridor
pcall(function()
    render.params {
        camPos = { 0.0, 1.8, 8.5 },
        camYaw = 180.0,
        camPitch = -2.0,
        ambientIntensity = 0.65,
        bloomIntensity = 0.15,
        exposure = 1.1,
        pointLights = {
            {
                position = { 0.0, 3.2, 5.0 },
                color = { 0.35, 0.75, 1.0 },
                radius = 12.0,
                intensity = 35.0,
            },
            {
                position = { 0.0, 3.2, -4.0 },
                color = { 0.4, 0.8, 1.0 },
                radius = 12.0,
                intensity = 35.0,
            },
            {
                position = { 0.0, 1.0, 0.0 },
                color = { 1.0, 0.6, 0.2 },
                radius = 6.0,
                intensity = 18.0,
            }
        }
    }
end)

log.info("Sci-Fi Space Ship Hallway scene spawned and lit")
scene.save("assets/scenes/spaceship_hallway.json")
