-- Original procedural geometry; no downloads, models, textures or external services.
local bugs, sparks, beams = {}, {}, {}
local bodyMesh, wireMesh, status, sight, weapon
local keys, active, aiming, zoom = {}, false, false, 1
local clock, spawnAt, drawAt, shotAt, flashUntil, hitUntil = 0, 0, 0, 0, 0, 0, 0
local score, kills, kick, mouseX, mouseY = 0, 0, 0, 0, 0
local levels, level = {2, 4, 8}, 1
local fx = nil
local PI = math.pi
local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end
local function length(x, y, z) return math.sqrt(x*x + y*y + z*z) end
local function shade(id, alpha, emission) scene_material(id, alpha, emission) end
local function hide(id) shade(id, 0, 0) end

local function geometry()
    local v, idx, wv, wi = {}, {}, {}, {}
    local function vertex(list, x, y, z)
        local n = #list
        list[n+1],list[n+2],list[n+3],list[n+4] = x,y,z,0
        list[n+5],list[n+6],list[n+7],list[n+8] = 1,0,0,0
        return n // 8 + 1
    end
    local function tri(list, a, b, c)
        list[#list+1], list[#list+2], list[#list+3] = a, b, c
    end
    local function rod(a, b, radius)
        local dx, dy, dz = b[1]-a[1], b[2]-a[2], b[3]-a[3]
        local d = length(dx, dy, dz)
        dx, dy, dz = dx/d, dy/d, dz/d
        local ux, uy, uz = -dz, 0, dx
        local n = length(ux, uy, uz)
        if n < .01 then ux, uy, uz, n = 1, 0, 0, 1 end
        ux, uy, uz = ux/n*radius, uy/n*radius, uz/n*radius
        local vx, vy, vz = dy*uz-dz*uy, dz*ux-dx*uz, dx*uy-dy*ux
        local base = #wv // 8
        for _, p in ipairs({a, b}) do
            for _, corner in ipairs({{-1,-1},{1,-1},{1,1},{-1,1}}) do
                vertex(wv, p[1]+corner[1]*ux+corner[2]*vx,
                           p[2]+corner[1]*uy+corner[2]*vy, p[3]+corner[1]*uz+corner[2]*vz)
            end
        end
        for j = 0, 3 do
            local k = (j+1)%4
            tri(wi, base+j+1, base+k+1, base+k+5)
            tri(wi, base+j+1, base+k+5, base+j+5)
        end
    end
    -- Flattened abdomen: distinct wire cage and translucent shell share the same coordinates.
    local rings = {}
    for r = 0, 4 do
        rings[r+1] = {}
        local t = -PI/2 + r*PI/4
        for j = 0, 7 do
            local a = j*PI/4
            local p = {math.cos(t)*math.cos(a)*.95, .18+math.cos(t)*math.sin(a)*.32, math.sin(t)*1.3+.25}
            rings[r+1][j+1] = {point=p, id=vertex(v, p[1], p[2], p[3])}
        end
    end
    for r = 1, 4 do
        for j = 1, 8 do
            local k = j%8+1
            local a, b, c, d = rings[r][j], rings[r][k], rings[r+1][k], rings[r+1][j]
            tri(idx, a.id, b.id, c.id); tri(idx, a.id, c.id, d.id)
            rod(a.point, d.point, .012)
            if r > 1 then rod(a.point, b.point, .012) end
        end
    end
    -- Broad pronotum/head, six bent legs and long antennae make the silhouette roach-like.
    local head = {}
    for _, p in ipairs({{-.5,0,-1.05},{.5,0,-1.05},{.5,.4,-1.05},{-.5,.4,-1.05},
                        {-.35,0,-1.7},{.35,0,-1.7},{.35,.3,-1.7},{-.35,.3,-1.7}}) do
        head[#head+1] = {point=p, id=vertex(v, p[1],p[2],p[3])}
    end
    for _, face in ipairs({{1,2,3,4},{5,8,7,6},{1,5,6,2},{4,3,7,8},{1,4,8,5},{2,6,7,3}}) do
        tri(idx, head[face[1]].id, head[face[2]].id, head[face[3]].id)
        tri(idx, head[face[1]].id, head[face[3]].id, head[face[4]].id)
    end
    for _, edge in ipairs({{1,2},{2,3},{3,4},{4,1},{5,6},{6,7},{7,8},{8,5},{1,5},{2,6},{3,7},{4,8}}) do
        rod(head[edge[1]].point, head[edge[2]].point, .017)
    end
    for _, side in ipairs({-1,1}) do
        for j = 0, 2 do
            local z = -.7 + j*.75
            local a, b, c = {side*.6,.1,z}, {side*1.35,.25,z-.35}, {side*1.85,-.13,z+.22}
            rod(a,b,.032); rod(b,c,.023)
        end
        rod({side*.25,.22,-1.65},{side*.55,.6,-2.1},.018)
        rod({side*.55,.6,-2.1},{side*1.05,.75,-2.55},.012)
    end
    bodyMesh, wireMesh = mesh_create(v,idx), mesh_create(wv,wi)
end

local function hologram(mesh, alpha, emission)
    local id = scene_mesh(mesh,0,0,-100,0,1,1,1,0,.92,1)
    shade(id,alpha,emission)
    return id
end
local function effectBox(r,g,b)
    local id = scene_box(0,-100,0,.1,.1,.1,r,g,b)
    hide(id)
    return id
end
local function clearEffects()
    for _, id in ipairs(beams) do hide(id) end
    for _, id in ipairs(sparks) do hide(id) end
    fx = nil
end
local function moveBug(b)
    for _, id in ipairs({b.body,b.wire}) do
        scene_move(id,b.x,b.y,b.z)
        scene_rotate(id,0,b.heading,math.sin(clock*20+b.phase)*.025)
        scene_resize(id,b.scale,b.scale,b.scale)
    end
    scene_move(b.hit,b.x,b.y+.15*b.scale,b.z)
    scene_rotate(b.hit,0,b.heading,0)
    scene_resize(b.hit,1.9*b.scale,.8*b.scale,3.3*b.scale)
end
local function despawn(b)
    b.alive = false; hide(b.body); hide(b.wire)
end

local function spawn(px,pz,extent)
    local target
    for _, b in ipairs(bugs) do if not b.alive then target=b; break end end
    if not target then return end
    -- Try only three candidates per spawn, never an unbounded placement search.
    for attempt = 1, 3 do
        local angle, distance = random_range(-PI,PI), random_range(14,48)
        local x, z = px+math.sin(angle)*distance, pz+math.cos(angle)*distance
        if length(x,0,z) < extent*.95 then
            local floor = world_floor(x,1.7,z,1.9,1.7)
            local rx, ry, rz = world_resolve(x,floor+1.7,z,1.9,1.7)
            if length(rx-x,ry-floor-1.7,rz-z) < .15 then
                target.x,target.y,target.z = x,floor+.24,z
                target.heading,target.scale,target.phase = random_range(-PI,PI),random_range(1,1.4),random_range(0,6)
                target.alive,target.expires = true,clock+random_range(28,48)
                shade(target.body,.13,.5); shade(target.wire,.95,3)
                moveBug(target)
                return
            end
        end
    end
end

local function fire(x,y,z,dx,dy,dz)
    if clock < shotAt then return end
    shotAt,flashUntil = clock+.85,clock+.1
    local distance, collided = world_raycast(x,y,z,dx,dy,dz,100), false
    if distance then collided=true else distance=100 end
    local victim
    for _, b in ipairs(bugs) do
        if b.alive then
            -- The wire antennae/legs are decoration; the shell/head box is the scoring volume.
            local t = scene_hit(b.hit,x,y,z,dx,dy,dz,distance)
            if t and (not collided or t < distance-.02) then distance,collided,victim=t,true,b end
        end
    end
    if victim then
        despawn(victim)
        kills = math.min(kills+1,1000000000)
        score = math.min(score+100+math.floor(distance)*2,1000000000)
        storage_set("score",score); storage_set("kills",kills)
        hitUntil=clock+.3
    end
    local yaw, pitch = math.atan(-dx,-dz), math.asin(clamp(dy,-1,1))
    for i, id in ipairs(beams) do
        scene_move(id,x+dx*distance/2,y+dy*distance/2,z+dz*distance/2)
        scene_resize(id,i==1 and .18 or .035,i==1 and .18 or .035,math.max(.02,distance))
        scene_rotate(id,pitch,yaw,0)
        shade(id,i==1 and .35 or 1,4)
    end
    local ex,ey,ez=x+dx*distance,y+dy*distance,z+dz*distance
    if collided then
        fx={x=ex,y=ey,z=ez,at=clock}
        for i, id in ipairs(sparks) do
            scene_move(id,ex,ey,ez); shade(id,1,4)
        end
    end
    kick = math.min(.26,kick+.18)
    sound_play("boom",1)
end

local function updateEffects()
    if clock > flashUntil then for _, id in ipairs(beams) do hide(id) end end
    if not fx then return end
    local age=clock-fx.at
    if age>.48 then for _, id in ipairs(sparks) do hide(id) end; fx=nil; return end
    for i, id in ipairs(sparks) do
        local angle=i*2.39996
        local radius=age*(i%3+2)
        scene_move(id,fx.x+math.cos(angle)*radius,fx.y+age*(2+i%4)-age*age*7,fx.z+math.sin(angle)*radius)
        local size=math.max(.015,.16*(1-age/.48))
        scene_resize(id,size,size*(1+i%3),size)
        scene_rotate(id,age*8,angle,age*5)
        shade(id,1-age/.48,4)
    end
end

local function paint()
    hud_clear(status,0,.02,.035,.68)
    hud_text(status,14,10,"BUG HUNTER",19,0,.95,1)
    hud_text(status,14,36,"SCORE "..score.."   BUGS "..kills,16,1,1,1)
    hud_text(status,14,61,active and "INFINITE AMMO  |  ESC / Y: EXIT" or "G / START: EQUIP LASER",12,.65,.85,.9)
    hud_clear(sight)
    hud_clear(weapon)
    if not active then return end
    local w,h=hud_view()
    w,h=w or 1280,h or 720
    local aspect=w/h
    local cy, cx=128,128
    if aiming then
        -- Normalized panels preserve the same central aim point at every aspect ratio.
        local rx=105/aspect
        hud_frame(sight,cx-rx,23,rx*2,210,2,0,.95,1,.65)
        hud_line(sight,cx-18/aspect,cy,cx-4/aspect,cy,1,.7,1,1)
        hud_line(sight,cx+4/aspect,cy,cx+18/aspect,cy,1,.7,1,1)
        hud_line(sight,cx,cy-18,cx,cy-4,1,.7,1,1)
        hud_line(sight,cx,cy+4,cx,cy+18,1,.7,1,1)
        hud_text(status,310,11,levels[level].."X",19,0,.95,1,1,"right")
    else
        hud_line(sight,cx-5/aspect,cy,cx+5/aspect,cy,1,.8,1,1)
        hud_line(sight,cx,cy-5,cx,cy+5,1,.8,1,1)
        -- A crisp view-space rifle silhouette avoids clipping the physical camera through a model.
        local recoil=math.max(0,shotAt-clock-.6)*90
        hud_rect(weapon,124,80+recoil,150,30,.04,.09,.13,1,5)
        hud_rect(weapon,45,63+recoil,185,12,.06,.15,.19,1,3)
        hud_rect(weapon,38,64+recoil,185,3,0,.9,1,1)
        hud_rect(weapon,155,44+recoil,56,22,.03,.06,.09,1,4)
        hud_frame(weapon,155,44+recoil,56,22,2,0,.8,1)
        hud_rect(weapon,182,104+recoil,32,55,.04,.08,.11,1,3)
        hud_rect(weapon,253,90+recoil,62,20,.06,.12,.17,1,4)
    end
    if clock<hitUntil then
        for _, sign in ipairs({-1,1}) do
            hud_line(sight,cx+sign*7/aspect,cy+7,cx+sign*13/aspect,cy+13,2,1,.6,.1)
            hud_line(sight,cx+sign*7/aspect,cy-7,cx+sign*13/aspect,cy-13,2,1,.6,.1)
        end
    end
    if clock<flashUntil then hud_frame(sight,1,1,254,254,2,.4,1,1,.55) end
end

return {
    on_start=function()
        assert(capabilities().api>=4,"Bug Hunter requires the new API 4 host, not v0.9.28")
        local function saved(key)
            local value=storage_get(key)
            if value==nil then return 0 end
            assert(type(value)=="number" and value>=0 and value<=1000000000 and value%1==0,"Invalid saved "..key)
            return value
        end
        score,kills=saved("score"),saved("kills")
        geometry()
        for i=1,6 do
            bugs[i]={body=hologram(bodyMesh,0,0),wire=hologram(wireMesh,0,0),
                     hit=effectBox(0,0,0),alive=false}
        end
        beams={effectBox(0,.8,1),effectBox(.8,1,1)}
        for i=1,10 do sparks[i]=effectBox(1,.35+i*.04,.08) end
        status=hud_panel("top_left",.015,.025,.27,.12,330,86)
        sight=hud_panel("center",0,0,1,1,256,256)
        weapon=hud_panel("bottom_right",-.06,0,.32,.28,330,180)
        control_exit_button("y")
        hud_set("BUG HUNTER: G / START TO EQUIP")
        paint()
    end,
    on_input=function(name,value)
        if name=="control" then
            active=value==1; keys={}; mouseX,mouseY,kick=0,0,0
            aiming=false; clearEffects(); paint()
        elseif name=="mouse_yaw" then mouseX=mouseX+value
        elseif name=="mouse_pitch" then mouseY=mouseY+value
        elseif name=="mouse_wheel" then level=clamp(level+(value>0 and 1 or -1),1,#levels)
        else
            local old=keys[name] or 0
            keys[name]=value
            if name=="dpad_up" and value==1 and old==0 then level=math.min(#levels,level+1) end
            if name=="dpad_down" and value==1 and old==0 then level=math.max(1,level-1) end
        end
    end,
    on_tick=function(dt)
        clock=clock+dt
        if not world_available() then
            for _, b in ipairs(bugs) do if b.alive then despawn(b) end end
            control_offer(nil); return
        end
        local map=map_view()
        local x,y,z,dx,dy,dz=view_pose()
        if not x or not map then control_offer(nil); return end
        control_offer(x,y,z)
        if clock>=spawnAt then spawn(x,z,map.max_x); spawnAt=clock+random_range(1.5,3.8) end
        for _, b in ipairs(bugs) do
            if b.alive and (clock>b.expires or length(b.x-x,0,b.z-z)>85) then despawn(b) end
            if b.alive then
                local nx,nz=b.x-math.sin(b.heading)*dt*1.6,b.z-math.cos(b.heading)*dt*1.6
                local rx,ry,rz=world_resolve(nx,b.y+1.46,nz,1.8,1.7)
                if length(rx-nx,ry-b.y-1.46,rz-nz)>.1 or length(nx,0,nz)>map.max_x*.97 then
                    b.heading=b.heading+1.4
                else
                    local floor=world_floor(nx,b.y+1.46,nz,1.8,1.7)
                    b.x,b.y,b.z=nx,floor+.24,nz
                end
                if random()<dt*.4 then b.heading=b.heading+random_range(-1,1) end
                b.heading=(b.heading+PI)%(2*PI)-PI
                moveBug(b)
            end
        end
        if active and dt>0 then
            aiming=(keys.aim or 0)>0 or (keys.left_trigger or 0)>.3
            zoom=aiming and levels[level] or 1
            control_zoom(zoom)
            if (keys.click or 0)>0 or (keys.right_trigger or 0)>.55 then fire(x,y,z,dx,dy,dz) end
            local yaw,pitch=math.atan(-dx,-dz),math.asin(clamp(dy,-1,1))
            local recoil=math.min(kick,2.4*dt); kick=kick-recoil
            local turn=-(mouseX+(keys.gamepad_right_x or 0)*dt*2.4)/zoom
            local tilt=(-mouseY+(keys.gamepad_right_y or 0)*dt*2.1)/zoom+recoil
            yaw=yaw+clamp(turn,-3.9*dt,3.9*dt)
            pitch=clamp(pitch+clamp(tilt,-3.9*dt,3.9*dt),-1.35,1.35)
            mouseX,mouseY=0,0
            local side=clamp((keys.move_x or 0)+(keys.gamepad_left_x or 0),-1,1)
            local forward=clamp((keys.move_y or 0)+(keys.gamepad_left_y or 0),-1,1)
            local speed=((keys.boost or 0)>0 or (keys.pad_ls or 0)>0) and 7 or 4
            if aiming then speed=2 end
            local norm=math.max(1,math.sqrt(side*side+forward*forward))
            local nx=x+(math.cos(yaw)*side-math.sin(yaw)*forward)*speed*dt/norm
            local nz=z+(-math.sin(yaw)*side-math.cos(yaw)*forward)*speed*dt/norm
            if length(nx,0,nz)>map.max_x*1.27 then nx,nz=x,z end
            control_pose(nx,math.max(.35,y-dt*6),nz,yaw,pitch)
        end
        updateEffects()
        if clock>=drawAt then paint(); drawAt=clock+.1 end
    end,
}
