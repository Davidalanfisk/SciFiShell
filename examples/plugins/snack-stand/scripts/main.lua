local duck_model = asset_model("assets/duck.glb")
local hotdog, icecream, snack, duck, shot, explosion
local held = nil
local center_x, center_y, center_z
local duck_x, duck_z
local duck_time, respawn, score, retry = 0, 0, 0, 0

local function status()
    hud_set("SCORE " .. score)
end

local function clear()
    input_capture_clicks(false)
    if hotdog then scene_remove(hotdog) end
    if icecream then scene_remove(icecream) end
    if snack then scene_remove(snack) end
    if duck then scene_remove(duck) end
    if explosion then
        for i = 1, 3 do scene_remove(explosion.objects[i]) end
    end
    hotdog, icecream, snack, duck, shot, explosion, held = nil, nil, nil, nil, nil, nil, nil
    respawn = 0
end

local function spawn_duck()
    duck_x, duck_z = center_x + 2, center_z
    duck = scene_model(duck_model, 0, duck_x, center_y + 0.5, duck_z, 0.8, 0.8, 0.8, 1, 1, 1)
end

local function make_stands()
    local x, y, z, dx, dy, dz = view_pose()
    if not x then return end
    local sx, sz = x + dx * 5, z + dz * 5
    local floor = world_floor(sx, y, sz, 0.2, 0)
    local right_x, right_z = -dz, dx
    local length = math.sqrt(right_x * right_x + right_z * right_z)
    if length < 0.001 then right_x, right_z, length = 1, 0, 1 end
    right_x, right_z = right_x / length, right_z / length
    local ix, iz = sx + right_x * 4, sz + right_z * 4
    local ice_floor = world_floor(ix, y, iz, 0.2, 0)
    hotdog = scene_box(sx, floor + 1, sz, 2, 2, 1, 1, 0.55, 0.2)
    icecream = scene_box(ix, ice_floor + 1, iz, 2, 2, 1, 1, 0.45, 0.8)
    scene_interactive(hotdog, true)
    scene_interactive(icecream, true)
    center_x, center_y, center_z = sx + right_x * 2, floor, sz + right_z * 2
    spawn_duck()
    status()
end

local function explode()
    local x, y, z = shot.x, shot.y, shot.z
    scene_remove(snack)
    snack, shot = nil, nil
    explosion = {age=0, objects={}}
    for i = 1, 3 do
        explosion.objects[i] = scene_box(x, y, z, 0.3, 0.3, 0.3, 1, i == 2 and 0.9 or 0.5, 0.2)
    end
    sound_play("boom", 0.6)
    local px, py, pz = view_pose()
    if px then
        local dx, dy, dz = px-x, py-y, pz-z
        local distance = math.sqrt(dx*dx + dy*dy + dz*dz)
        if distance < 18 then
            local strength = 1 - distance / 18
            local horizontal = math.sqrt(dx*dx + dz*dz)
            if horizontal < 0.01 then dx, dz, horizontal = 0, 1, 1 end
            player_impulse(dx / horizontal * 12 * strength, 8 * strength, dz / horizontal * 12 * strength)
        end
    end
    if duck then
        local dx, dy, dz = duck_x-x, center_y+0.5-y, duck_z-z
        if dx*dx + dy*dy + dz*dz < 49 then
            scene_remove(duck)
            duck = nil
            respawn = 90
            score = math.min(score + 100, 9999999)
            sound_play("duck_pop", 0.6)
            status()
        end
    end
end

return {
    on_start = function() hud_set("WAITING FOR CITY") end,
    on_input = function(control, value)
        if control ~= "click" or value ~= 1 or not hotdog then return end
        local x, y, z, dx, dy, dz = view_pose()
        if not x then return end
        if held then
            held = nil
            input_capture_clicks(false)
            shot = {x=x+dx, y=y+dy, z=z+dz, vx=dx*15, vy=dy*15+3.5, vz=dz*15, age=0}
            sound_play("throw", 0.6)
            status()
        elseif not shot and not explosion then
            local a = scene_hit(hotdog, x, y, z, dx, dy, dz, 14)
            local b = scene_hit(icecream, x, y, z, dx, dy, dz, 14)
            if a or b then
                held = b and (not a or b < a) and "icecream" or "hotdog"
                input_capture_clicks(true)
                if held == "hotdog" then
                    snack = scene_box(x+dx, y+dy, z+dz, 0.5, 0.15, 0.2, 1, 0.62, 0.2)
                else
                    snack = scene_box(x+dx, y+dy, z+dz, 0.26, 0.4, 0.26, 1, 0.45, 0.8)
                end
                sound_play("pickup", 0.6)
                hud_set("CLICK TO THROW " .. held)
            end
        end
    end,
    on_tick = function(dt)
        if not world_available() then
            clear()
            hud_set("WAITING FOR CITY")
            return
        end
        if not hotdog then
            retry = retry - dt
            if retry <= 0 then retry = 1 make_stands() end
            return
        end
        duck_time = (duck_time + dt) % 20.943951023931955
        if duck then
            duck_x = center_x + math.sin(duck_time * 0.3) * 0.6
            duck_z = center_z + math.cos(duck_time * 0.3) * 0.6
            scene_move(duck, duck_x, center_y + 0.5, duck_z)
            scene_rotate(duck, 0, duck_time * 0.3, 0)
        elseif respawn > 0 then
            respawn = respawn - dt
            if respawn <= 0 then spawn_duck() end
        end
        if explosion then
            explosion.age = explosion.age + dt
            if explosion.age >= 1.3 then
                for i = 1, 3 do scene_remove(explosion.objects[i]) end
                explosion = nil
            else
                local progress = math.min(explosion.age / 1.2, 1)
                local fade = math.max(0, 1 - progress)
                for i = 1, 3 do
                    local size = 0.3 + (4 + 4*i) * progress
                    scene_resize(explosion.objects[i], size, size, size)
                    scene_rotate(explosion.objects[i], 0, progress * i, 0)
                    scene_colour(explosion.objects[i], fade, fade * (i == 2 and 0.9 or 0.5), fade * 0.2)
                end
            end
        end
        if held then
            local x, y, z, dx, dy, dz = view_pose()
            if x then scene_move(snack, x+dx, y+dy-0.25, z+dz) end
        elseif shot then
            shot.age = shot.age + dt
            shot.vy = shot.vy - 20 * dt
            local x, y, z = shot.x + shot.vx * dt, shot.y + shot.vy * dt, shot.z + shot.vz * dt
            local rx, ry, rz = world_resolve(x, y, z, 0.2, 0)
            if math.abs(rx-x) > 0.0001 then shot.vx = -shot.vx * 0.35 end
            if math.abs(rz-z) > 0.0001 then shot.vz = -shot.vz * 0.35 end
            x, z = rx, rz
            local floor = world_floor(x, y, z, 0.2, 0) + 0.15
            if y < floor then
                y = floor
                if shot.vy < 0 then shot.vy = -shot.vy * 0.35 end
                shot.vx, shot.vz = shot.vx * 0.8, shot.vz * 0.8
            end
            shot.x, shot.y, shot.z = x, y, z
            scene_move(snack, x, y, z)
            scene_rotate(snack, shot.age*8, shot.age*6, 0)
            if shot.age >= 5 then explode() end
        end
    end
}
