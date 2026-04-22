-- fps_controller.lua
--
-- Gameplay input, player movement, aiming and Mouse1/Mouse2 interactions are
-- now handled by the C++ PlayerControllerSystem and PlayerInteractionSystem.
-- Keep this script as a harmless compatibility stub for older scenes that
-- still reference scripts/fps_controller.lua.

function on_spawn(entity)
    log.info("FPS Controller compatibility stub attached to " .. entity:get_name())
end

function on_update(entity, dt)
end
