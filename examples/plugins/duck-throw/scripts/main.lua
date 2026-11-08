local model = asset_model("assets/duck.glb")
local ducks = {}
local count = 0
local launcher

return {
    on_start = function()
        hud_set("DUCKS ACTIVE: 0")
    end,
    on_input = function(control, value)
        if control ~= "click" or value ~= 1 or count >= 16 or not world_available() then return end
        local x, y, z, dx, dy, dz = view_pose()
        if not x then return end
        local id = scene_model(model, 0, x + dx, y + dy, z + dz, 0.5, 0.5, 0.5, 1, 1, 1)
        count = count + 1
        hud_set("DUCKS ACTIVE: " .. count)
        ducks[count] = {id=id, x=x + dx, y=y + dy, z=z + dz,
                        vx=dx * 12, vy=dy * 12 + 2, vz=dz * 12, age=0}
        sound_play("throw", 0.6)
    end,
    on_tick = function(dt)
        if not world_available() then
            if launcher then scene_remove(launcher) launcher = nil end
        elseif not launcher then
            local x, y, z, dx, dy, dz = view_pose()
            if x then
                launcher = scene_box(x + dx * 5, y + dy * 5, z + dz * 5, 1, 1, 1, 1, 0.7, 0.1)
                scene_interactive(launcher, true)
            end
        end
        local i = 1
        local expired = false
        while i <= count do
            local duck = ducks[i]
            duck.age = duck.age + dt
            if duck.age >= 5 or not world_available() then
                if duck.age >= 5 then expired = true end
                scene_remove(duck.id)
                ducks[i] = ducks[count]
                ducks[count] = nil
                count = count - 1
            else
                duck.vy = duck.vy - 9.8 * dt
                local x = duck.x + duck.vx * dt
                local y = duck.y + duck.vy * dt
                local z = duck.z + duck.vz * dt
                local rx, ry, rz = world_resolve(x, y, z, 0.2, 0)
                if math.abs(rx - x) > 0.0001 then duck.vx = -duck.vx * 0.35 end
                if math.abs(rz - z) > 0.0001 then duck.vz = -duck.vz * 0.35 end
                x, z = rx, rz
                local floor = world_floor(x, y, z, 0.2, 0) + 0.15
                if y < floor then
                    y = floor
                    duck.vy = math.abs(duck.vy) * 0.4
                    duck.vx = duck.vx * 0.8
                    duck.vz = duck.vz * 0.8
                end
                duck.x, duck.y, duck.z = x, y, z
                scene_move(duck.id, x, y, z)
                scene_rotate(duck.id, 0, duck.age * 3, 0)
                i = i + 1
            end
        end
        if expired then sound_play("boom", 0.4) end
        hud_set("DUCKS ACTIVE: " .. count)
    end
}
