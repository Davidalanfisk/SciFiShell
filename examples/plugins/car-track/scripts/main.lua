local body_model = asset_model("assets/car.glb")
local body_texture = asset_texture("assets/paint.png")
local objects = {}
local count = 0
local car, wheels = nil, {}
local x, y, z, yaw, speed = 0, 0, 0, 0, 0
local steer, throttle, retry = 0, 0, 0
local spin = 0
local center_x, center_z, rail_angle, lane = 0, 0, nil, 0
local heights, segments, refresh_index, refresh_timer, sample_y = {}, {}, 0, 0, 0
local driving = false
local pi = math.pi

local function add(px, py, pz, w, h, d, r, g, b)
    count = count + 1
    objects[count] = scene_box(px, py, pz, w, h, d, r, g, b)
    return objects[count]
end

local function clear()
    if driving then sound_engine(false, 0, false) end
    for i = 1, count do scene_remove(objects[i]) objects[i] = nil end
    count, car, wheels = 0, nil, {}
    speed, steer, throttle, spin, retry = 0, 0, 0, 0, 0
    rail_angle, lane = nil, 0
    driving = false
    control_offer(nil)
    heights, segments, refresh_index, refresh_timer = {}, {}, 0, 0
end

local function place_segment(i)
    local a, b = 2*pi*i/32, 2*pi*(i+1)/32
    local ax, az = center_x + 18*math.cos(a), center_z + 12*math.sin(a)
    local bx, bz = center_x + 18*math.cos(b), center_z + 12*math.sin(b)
    local sx, sz = bx-ax, bz-az
    local length = math.sqrt(sx*sx + sz*sz)
    local ay, by = heights[i+1], heights[(i+1)%32+1]
    local rise = by-ay
    local segment = segments[i+1]
    scene_move(segment, (ax+bx)/2, (ay+by)/2+0.03, (az+bz)/2)
    scene_resize(segment, 3.5, 0.06, math.sqrt(length*length+rise*rise)+0.1)
    scene_rotate(segment, -math.atan(rise, length), math.atan(sx, sz), 0)
end

local function place_car()
    scene_move(car, x, y + 0.6, z)
    scene_rotate(car, 0, yaw, 0)
    local s, c = math.sin(yaw), math.cos(yaw)
    for i = 1, 4 do
        local side = i <= 2 and -1 or 1
        local along = i % 2 == 0 and -1 or 1
        local rx, rz = side * 0.65, along * 0.75
        scene_move(wheels[i], x + c*rx + s*rz, y + 0.3, z - s*rx + c*rz)
        scene_rotate(wheels[i], spin, yaw, 0)
    end
end

local function create()
    local vx, vy, vz, dx, dy, dz = view_pose()
    if not vx then return end
    local cx, cz = vx + dx * 25, vz + dz * 25
    center_x, center_z = cx, cz
    sample_y = vy
    heights = {}
    for i = 0, 31 do
        local a = 2*pi*i/32
        heights[i+1] = world_floor(cx + 18*math.cos(a), vy, cz + 12*math.sin(a), 0.6, 0)
    end
    for i = 0, 31 do
        segments[i+1] = add(0, 0, 0, 1, 1, 1, 0.22, 0.25, 0.3)
        place_segment(i)
    end
    x, y, z, yaw = cx+18, heights[1], cz, pi
    car = scene_model(body_model, body_texture, x, y+0.6, z, 1.4, 0.7, 2.4, 1, 1, 1)
    count = count + 1
    objects[count] = car
    for i = 1, 4 do wheels[i] = add(x, y+0.3, z, 0.3, 0.55, 0.55, 0.08, 0.08, 0.08) end
    place_car()
    control_offer(x, y + 1.7, z)
    hud_set("WASD: REMOTE CAR")
end

return {
    on_start = function() hud_set("WAITING FOR CITY") end,
    on_input = function(control, value)
        if control == "move_x" then steer = value
        elseif control == "move_y" then throttle = value
        elseif control == "control" then
            driving = value == 1
            if not driving then sound_engine(false, 0, false) end
        end
    end,
    on_tick = function(dt)
        if not world_available() then
            clear()
            hud_set("WAITING FOR CITY")
            return
        end
        if not car then
            retry = retry - dt
            if retry <= 0 then retry = 1 create() end
            return
        end
        refresh_timer = refresh_timer - dt
        if refresh_timer <= 0 then
            refresh_timer = 0.25
            local a = 2*pi*refresh_index/32
            heights[refresh_index+1] = world_floor(center_x + 18*math.cos(a), sample_y,
                                                    center_z + 12*math.sin(a), 0.6, 0)
            place_segment((refresh_index+31)%32)
            place_segment(refresh_index)
            refresh_index = (refresh_index+1)%32
        end
        dt = math.min(dt, 0.05)
        speed = speed + throttle * 12 * dt
        speed = speed * math.exp(-0.7 * dt)
        speed = math.max(-6, math.min(20, speed))
        if rail_angle and throttle < 0 then
            rail_angle = nil
            hud_set("WASD: REMOTE CAR")
        end
        if not rail_angle and throttle > 0 and speed > 4 then
            local a = math.atan((z-center_z)/12, (x-center_x)/18)
            local tx, tz = -18*math.sin(a), 12*math.cos(a)
            local length = math.sqrt(tx*tx + tz*tz)
            local distance = math.sqrt((x-center_x-18*math.cos(a))^2 + (z-center_z-12*math.sin(a))^2)
            if distance < 1.4 and (-math.sin(yaw)*tx - math.cos(yaw)*tz)/length > 0.7 then
                rail_angle, lane = a, 0
                hud_set("ON RAIL: S TO EXIT")
            end
        end
        local nx, nz
        if rail_angle then
            local tx, tz = -18*math.sin(rail_angle), 12*math.cos(rail_angle)
            local length = math.sqrt(tx*tx + tz*tz)
            rail_angle = (rail_angle + speed * dt / length) % (2*pi)
            lane = math.max(-1.2, math.min(1.2, lane + steer*2*dt))
            local c, s = math.cos(rail_angle), math.sin(rail_angle)
            local ox, oz = c/18, s/12
            local olen = math.sqrt(ox*ox + oz*oz)
            nx, nz = center_x + 18*c + ox/olen*lane, center_z + 12*s + oz/olen*lane
            yaw = math.atan(18*s, -12*c)
        else
            local direction = speed < 0 and -1 or 1
            yaw = (yaw - steer * 1.8 * math.min(math.abs(speed) / 4, 1) * direction * dt + pi) % (2*pi) - pi
            nx, nz = x - math.sin(yaw)*speed*dt, z - math.cos(yaw)*speed*dt
        end
        local rx, ry, rz = world_resolve(nx, y+1, nz, 0.8, 1)
        if math.abs(rx-nx) + math.abs(rz-nz) > 0.01 then
            speed = speed * 0.5
            if rail_angle then rail_angle = nil hud_set("WASD: REMOTE CAR") end
        end
        x, z = rx, rz
        y = world_floor(x, ry, z, 0.6, 0)
        spin = (spin + speed * dt / 0.3) % (2*pi)
        place_car()
        control_offer(x, y + 1.7, z)
        if driving then
            sound_engine(true, math.abs(speed), false)
            local vx, vy, vz, dx, dy, dz = view_pose()
            if vx then
                local tx, ty, tz = x + math.sin(yaw)*5, y + 3, z + math.cos(yaw)*5
                local ax, ay, az = tx-vx, ty-vy, tz-vz
                local distance = math.sqrt(ax*ax + ay*ay + az*az)
                local factor = distance > 0 and math.min(1, 15*dt/distance) or 0
                local current_yaw = math.atan(-dx, -dz)
                local current_pitch = math.asin(math.max(-1, math.min(1, dy)))
                local turn = (yaw-current_yaw+pi) % (2*pi)-pi
                turn = math.max(-3*dt, math.min(3*dt, turn))
                local pitch = current_pitch + math.max(-3*dt, math.min(3*dt, -0.15-current_pitch))
                control_pose(vx + ax*factor, vy + ay*factor, vz + az*factor, current_yaw + turn, pitch)
            end
        end
    end
}
