local box = scene_box(0, 1.7, 100, 2, 2, 1, 1, 1, 1)
scene_interactive(box, true)
return {
    on_start = function() hud_set("FAR READY") end,
    on_input = function(key, value)
        if key == "click" and value == 1 then hud_set("FAR PRESS") end
    end
}
