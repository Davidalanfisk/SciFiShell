local offered = false
local driving = false
local marker = nil

return {
    on_tick = function(dt)
        if not world_available() then return end
        local x, y, z, dx, dy, dz = view_pose()
        if not x then return end
        if not marker then
            local clearance = world_track_clearance(0, -55, 0)
            marker = scene_box(0, clearance, 0, 1, 1, 1, 1, 1, 1)
        end
        if not offered then
            control_offer(x, y, z)
            offered = true
        end
        if driving then
            local yaw = math.atan(-dx, -dz)
            local pitch = math.asin(math.max(-1, math.min(1, dy)))
            control_pose(x + dt * 2, y, z, yaw, pitch)
        end
    end,
    on_input = function(key, value)
        if key == "control" then driving = value == 1 end
    end
}
