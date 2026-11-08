local duck = asset_model("assets/duck.glb")
local parks = {}
local count = 0
local timer = 0

local function remove(index)
    local park = parks[index]
    for i = 1, park.count do scene_remove(park.objects[i]) end
    for i = index, count - 1 do parks[i] = parks[i + 1] end
    parks[count] = nil
    count = count - 1
end

local function clear()
    while count > 0 do remove(count) end
end

local function create(x, y, z, pitch)
    local park = {x=x, y=y, z=z, pitch=pitch, objects={}, count=0}
    local function box(px, py, pz, w, h, d, r, g, b)
        park.count = park.count + 1
        park.objects[park.count] = scene_box(px, py, pz, w, h, d, r, g, b)
    end
    local function plant(px, pz)
        box(px, y + 1, pz, 0.4, 2, 0.4, 0.2, 0.4, 0.2)
        box(px, y + 2.8, pz, 2.5, 2, 2.5, 0.2, 0.85, 0.4)
    end
    local side = math.min(18, pitch * 0.7)
    box(x, y + 0.03, z, side, 0.06, side, 0.2, 0.7, 0.3)
    box(x, y + 0.07, z, side * 0.35, 0.06, side * 0.35, 0.2, 0.6, 1)
    plant(x - side * 0.35, z - side * 0.35)
    plant(x + side * 0.35, z + side * 0.35)
    park.count = park.count + 1
    park.objects[park.count] = scene_model(duck, 0, x, y + 0.5, z, 1, 1, 1, 1, 1, 1)
    count = count + 1
    parks[count] = park
end

local function contains(x, z)
    for i = 1, count do
        if math.abs(parks[i].x - x) < 0.01 and math.abs(parks[i].z - z) < 0.01 then return true end
    end
    return false
end

return {
    on_start = function() hud_set("WAITING FOR CITY") end,
    on_tick = function(dt)
        if not world_available() then
            clear()
            hud_set("WAITING FOR CITY")
            return
        end
        timer = timer - dt
        if timer > 0 then return end
        timer = 1
        local vx, vy, vz = view_pose()
        if not vx then clear() hud_set("WAITING FOR VIEW") return end
        local index = count
        while index > 0 do
            local park = parks[index]
            local x, y, z, pitch, occupied = city_cell(park.x, park.z)
            local dx, dz = park.x - vx, park.z - vz
            if not x or occupied or math.abs(x - park.x) > 0.01 or math.abs(y - park.y) > 0.01 or
                math.abs(z - park.z) > 0.01 or math.abs(pitch - park.pitch) > 0.01 or dx*dx + dz*dz > 84*84 then
                remove(index)
            end
            index = index - 1
        end
        for iz = -2, 2 do
            for ix = -2, 2 do
                if count < 3 then
                    local x, y, z, pitch, occupied = city_cell(vx + ix * 28, vz + iz * 28)
                    if x and not occupied and not contains(x, z) then create(x, y, z, pitch) end
                end
            end
        end
        if count == 0 then hud_set("NO VACANT CELL NEARBY")
        else hud_set("PARK CELLS: " .. count) end
    end
}
