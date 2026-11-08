local duck
local elapsed = 0

return {
    on_start = function()
        scene_box(0, 0.1, -4, 3, 0.2, 3, 0.2, 0.6, 0.3)
        duck = scene_model(asset_model("assets/duck.glb"), 0, 0, 0.2, -4, 1, 1, 1, 1, 1, 1)
    end,
    on_tick = function(dt)
        elapsed = elapsed + dt
        scene_move(duck, 0, 0.2 + 0.1 * math.sin(elapsed * 2), -4)
        scene_rotate(duck, 0, math.sin(elapsed) * 0.5, 0)
    end
}
