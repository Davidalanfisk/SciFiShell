local box = scene_box(0, 1.7, 105, 2, 2, 1, 1, 1, 1)
scene_interactive(box, true)
return {
    on_input = function(key, value)
        if key == "click" and value == 1 then
            input_capture_clicks(true)
            missing_callback()
        end
    end
}
