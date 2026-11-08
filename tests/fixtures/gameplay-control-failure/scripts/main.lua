local active = false
local offered = false

return {
    on_tick = function()
        if not world_available() then return end
        local x, y, z = view_pose()
        if not x then return end
        if not offered then
            control_offer(x, y, z)
            offered = true
        end
        if active then missing_callback() end
    end,
    on_input = function(key, value)
        if key == "control" then active = value == 1 end
    end
}
