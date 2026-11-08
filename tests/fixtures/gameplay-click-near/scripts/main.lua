local box = scene_box(0, 1.7, 105, 2, 2, 1, 1, 1, 1)
scene_interactive(box, true)
local presses = 0
return {
    on_start = function() hud_set("NEAR READY") end,
    on_input = function(key, value)
        if key ~= "click" then return end
        if value == 1 then
            presses = presses + 1
            input_capture_clicks(presses == 1)
            hud_set("NEAR PRESS " .. presses)
        else
            hud_set("NEAR RELEASE " .. presses)
        end
    end
}
