-- Weather card, news ticker and mini-map for the plugin HUD. Everything shown comes from the host or a declared
-- network host: the weather city and news source are explicit choices made in this plugin's room (no user
-- location is ever sent), and the map is the host's read-only city snapshot. Failures are shown, never faked.
local CITIES = {
    london = {"LONDON", 51.51, -0.13}, new_york = {"NEW YORK", 40.71, -74.01}, tokyo = {"TOKYO", 35.68, 139.69},
    sydney = {"SYDNEY", -33.87, 151.21}, reykjavik = {"REYKJAVIK", 64.15, -21.94},
    cape_town = {"CAPE TOWN", -33.92, 18.42},
}
local SOURCES = {
    bbc = {"BBC", "https://feeds.bbci.co.uk/news/rss.xml"},
    npr = {"NPR", "https://feeds.npr.org/1001/rss.xml"},
    nasa = {"NASA", "https://www.nasa.gov/news-release/feed/"},
}
local REFRESH, STALE_AFTER, MAP_RANGE = 600, 1200, 140  -- seconds, seconds, metres from the player to the map edge

local clock, refreshAt = 0, 0
local scale, viewW, viewH = 1, 0, 0
local W, L, T, M  -- weather, ticker label, ticker strip, map panels
local tickerUnits = 1000

local weather = {state = "loading"}       -- state: loading | ok | error
local news = {state = "loading", items = {}, fetched = nil}
local weatherId, newsId
local shown, storyUntil, tickerNote, noteUntil = 1, 0, nil, 0

local map = {view = nil, reason = nil, regions = {}, pois = {}, placesAt = -10, drawnAt = -10, key = ""}

-- Cut a string to at most n bytes without splitting a UTF-8 sequence.
local function clip(s, n)
    if #s <= n then return s end
    local i = n
    while i > 0 and s:byte(i + 1) and s:byte(i + 1) >= 0x80 and s:byte(i + 1) < 0xC0 do i = i - 1 end
    return s:sub(1, i)
end

-- Replace control characters with spaces (the sandbox has no pattern functions such as gsub).
local function printable(s)
    local parts, from = {}, 1
    for i = 1, #s do
        local b = s:byte(i)
        if b < 32 or b == 127 then parts[#parts + 1] = s:sub(from, i - 1); from = i + 1 end
    end
    if from == 1 then return s end
    parts[#parts + 1] = s:sub(from)
    return table.concat(parts, " ")
end

-------------------------------------------------------------------------------------------------- weather icons
local function sun(p, x, y, r, a)
    hud_circle(p, x, y, r, 1.0, 0.8, 0.25, a or 1)
    for k = 0, 7 do
        local t = k * math.pi / 4
        hud_line(p, x + math.cos(t) * (r + 3), y + math.sin(t) * (r + 3), x + math.cos(t) * (r + 7),
                 y + math.sin(t) * (r + 7), 2, 1.0, 0.8, 0.25, a or 1)
    end
end

local function cloud(p, x, y, shade)
    local c = shade or 0.85
    hud_circle(p, x - 8, y + 3, 7, c, c, c + 0.05, 1)
    hud_circle(p, x + 1, y - 2, 9, c, c, c + 0.05, 1)
    hud_circle(p, x + 10, y + 3, 7, c, c, c + 0.05, 1)
    hud_rect(p, x - 8, y + 3, x + 10, y + 10, c, c, c + 0.05, 1)
end

local function drops(p, x, y, r, g, b, slant, count)
    for k = 0, count - 1 do
        local dx = x - 9 + k * 7
        hud_line(p, dx, y + 13, dx - slant, y + 20, 2, r, g, b, 1)
    end
end

local function icon(p, x, y, code, day, wind)
    if code == nil then
        hud_text(p, x, y - 9, "?", 18, 0.7, 0.75, 0.85, 1, "center")
    elseif code == 0 or code == 1 then
        if wind then
            for k = 0, 2 do hud_line(p, x - 14, y - 7 + k * 7, x + 10 - k * 5, y - 7 + k * 7, 2, 0.8, 0.9, 1, 1) end
            hud_circle(p, x + 12, y - 9, 3, 0.8, 0.9, 1, 1)
        elseif day then sun(p, x, y, 8)
        else
            hud_circle(p, x, y, 9, 0.9, 0.92, 0.75, 1)
            hud_circle(p, x + 5, y - 4, 8, 0.05, 0.08, 0.14, 1)
        end
    elseif code == 2 then
        sun(p, x - 6, y - 6, 6, 0.95); cloud(p, x + 3, y + 1)
    elseif code == 3 then
        cloud(p, x, y)
    elseif code == 45 or code == 48 then
        for k = 0, 3 do hud_line(p, x - 14 + (k % 2) * 4, y - 9 + k * 6, x + 14 - (k % 2) * 4, y - 9 + k * 6, 2, 0.7, 0.72, 0.78, 1) end
    elseif code >= 71 and code <= 77 or code == 85 or code == 86 then
        cloud(p, x, y - 4)
        for k = 0, 2 do hud_circle(p, x - 8 + k * 8, y + 13 + (k % 2) * 4, 2, 1, 1, 1, 1) end
    elseif code >= 95 then
        cloud(p, x, y - 4, 0.55)
        hud_line(p, x + 2, y + 8, x - 3, y + 15, 2, 1, 0.85, 0.2, 1)
        hud_line(p, x - 3, y + 15, x + 3, y + 15, 2, 1, 0.85, 0.2, 1)
        hud_line(p, x + 3, y + 15, x - 2, y + 22, 2, 1, 0.85, 0.2, 1)
    elseif code >= 51 and code <= 57 then
        cloud(p, x, y - 4); drops(p, x, y - 4, 0.45, 0.7, 1, 0, 3)
    else  -- 61-67 rain, 80-82 showers
        cloud(p, x, y - 4, 0.7); drops(p, x, y - 4, 0.35, 0.65, 1, 3, 3)
    end
end

local CONDITIONS = {[0] = "CLEAR", [1] = "MAINLY CLEAR", [2] = "PARTLY CLOUDY", [3] = "OVERCAST", [45] = "FOG",
    [48] = "FOG", [51] = "DRIZZLE", [53] = "DRIZZLE", [55] = "DRIZZLE", [56] = "FREEZING DRIZZLE",
    [57] = "FREEZING DRIZZLE", [61] = "LIGHT RAIN", [63] = "RAIN", [65] = "HEAVY RAIN", [66] = "FREEZING RAIN",
    [67] = "FREEZING RAIN", [71] = "LIGHT SNOW", [73] = "SNOW", [75] = "HEAVY SNOW", [77] = "SNOW GRAINS",
    [80] = "SHOWERS", [81] = "SHOWERS", [82] = "HEAVY SHOWERS", [85] = "SNOW SHOWERS", [86] = "SNOW SHOWERS",
    [95] = "THUNDERSTORM", [96] = "THUNDERSTORM", [99] = "THUNDERSTORM"}

local function drawWeather()
    if not W then return end
    local city = CITIES[setting("location")] or CITIES.london
    hud_clear(W, 0.02, 0.04, 0.08, 0.32)  -- no border; mostly see-through
    if weather.state == "ok" then
        icon(W, 26, 30, weather.code, weather.day, weather.windy)
        hud_text(W, 52, 9, string.format("%.0f C", weather.temp), 20, 0.92, 0.96, 1.0, 1)
        hud_text(W, 52, 34, city[1] .. "  " .. (CONDITIONS[weather.code] or "UNKNOWN"), 10, 0.7, 0.8, 0.9, 1)
    else
        icon(W, 26, 30, nil)
        hud_text(W, 52, 12, city[1], 12, 0.85, 0.9, 1.0, 1)
        hud_text(W, 52, 32, weather.state == "loading" and "LOADING..." or clip("UNAVAILABLE: " .. weather.err, 60),
                 10, weather.state == "loading" and 0.6 or 1.0, 0.7, 0.6, 1)
    end
end

-------------------------------------------------------------------------------------------------- news ticker
local function sourceInfo() return SOURCES[setting("news")] or SOURCES.bbc end

local function drawLabel()
    if not L then return end
    hud_clear(L, 0.55, 0.08, 0.12, 0.78)
    hud_text(L, 6, 5, sourceInfo()[1], 11, 1, 1, 1, 1)
    hud_text(L, 74, 6, "[N]", 9, 1, 0.85, 0.75, 1, "right")
end

local function currentText()
    if noteUntil > clock and tickerNote then return tickerNote, 1.0, 0.75, 0.4 end
    if news.state == "loading" then return "LOADING " .. sourceInfo()[1] .. " HEADLINES...", 0.6, 0.7, 0.8 end
    if news.state == "error" then return clip("NEWS UNAVAILABLE: " .. news.err, 200), 1.0, 0.6, 0.5 end
    if news.fetched and clock - news.fetched > STALE_AFTER then
        return "HEADLINES OUT OF DATE - WAITING FOR AN UPDATE", 1.0, 0.75, 0.4
    end
    local item = news.items[shown]
    if not item then return "NO HEADLINES IN THIS FEED", 0.7, 0.7, 0.75 end
    return shown .. "/" .. #news.items .. "   " .. item.title, 0.95, 0.92, 0.82
end

-- One line, clipped to the strip; scrolls (marquee) only when the measured text is wider than the strip.
local function drawTicker()
    if not T then return end
    local text, r, g, b = currentText()
    local size = 12
    local width = text_width(text, size)
    local loop = 0
    if width + 16 > tickerUnits then
        local canvas = math.min(4096, math.ceil(width + tickerUnits * 0.4))
        hud_panel_remove(T)
        T = hud_panel("bottom_right", 0, 0, 1 - 80 * scale / viewW, 22 * scale / viewH, canvas, 22)
        hud_clear(T, 0.02, 0.03, 0.06, 0.72)
        hud_text(T, 8, 4, text, size, r, g, b, 1)
        hud_scroll(T, 60, tickerUnits)
        loop = canvas / 60
    else
        hud_panel_remove(T)
        T = hud_panel("bottom_right", 0, 0, 1 - 80 * scale / viewW, 22 * scale / viewH, tickerUnits, 22)
        hud_clear(T, 0.02, 0.03, 0.06, 0.72)
        hud_text(T, 8, 4, text, size, r, g, b, 1)
    end
    storyUntil = clock + math.max(8, loop + 1)
end

-------------------------------------------------------------------------------------------------- mini-map
local KIND_COLOURS = {plugin = {0.3, 0.95, 0.9}, monument = {1.0, 0.8, 0.3}, settings = {0.8, 0.5, 1.0}}

local function drawMap()
    if not M then return end
    local S = 180
    hud_clear(M, 0.02, 0.04, 0.08, 0.5)
    local v = map.view
    if not v then
        hud_frame(M, 1, 1, S - 1, S - 1, 1, 0.4, 0.5, 0.6, 0.6)
        hud_text(M, S / 2, S / 2 - 16, "MAP UNAVAILABLE", 12, 1.0, 0.7, 0.5, 1, "center")
        hud_text(M, S / 2, S / 2 + 2, clip(string.upper(map.reason or "no city"), 40), 9, 0.7, 0.75, 0.85, 1, "center")
        return
    end
    local k = (S / 2) / MAP_RANGE
    local function px(x) return S / 2 + (x - v.x) * k end
    local function pz(z) return S / 2 + (z - v.z) * k end
    local function inside(x, z, pad) return math.abs(x - v.x) < MAP_RANGE + pad and math.abs(z - v.z) < MAP_RANGE + pad end
    local function clampc(c) return math.max(-40, math.min(S + 40, c)) end
    local budget = 470
    -- City boundary
    hud_frame(M, clampc(px(v.min_x)), clampc(pz(v.min_z)), clampc(px(v.max_x)), clampc(pz(v.max_z)), 1, 0.35, 0.45, 0.6, 0.7)
    for _, r in ipairs(map.regions) do
        if budget < 4 then break end
        if inside(r.x, r.z, (r.width + r.depth) / 2) then
            local x0, z0 = clampc(px(r.x - r.width / 2)), clampc(pz(r.z - r.depth / 2))
            local x1, z1 = clampc(px(r.x + r.width / 2)), clampc(pz(r.z + r.depth / 2))
            hud_rect(M, x0, z0, x1, z1, 0.2, 0.5, 0.8, 0.12)
            hud_frame(M, x0, z0, x1, z1, 1, 0.3, 0.65, 1.0, 0.7)
            local lx, lz = px(r.x), pz(r.z - r.depth / 2) + 2
            if lx > 10 and lx < S - 10 and lz > 0 and lz < S - 10 then
                hud_text(M, lx, lz, clip(r.name, 20), 8, 0.6, 0.8, 1.0, 0.9, "center")
            end
            budget = budget - 3
        end
    end
    for _, p in ipairs(map.pois) do
        if budget < 3 then break end
        if inside(p.x, p.z, 0) then
            local c = KIND_COLOURS[p.kind] or {0.9, 0.9, 0.9}
            hud_circle(M, px(p.x), pz(p.z), 2.5, c[1], c[2], c[3], 1)
            budget = budget - 1
        end
    end
    -- Player arrow, heading in degrees clockwise from north (up).
    local h = math.rad(v.heading)
    local fx, fy = math.sin(h), -math.cos(h)
    local cx, cy = S / 2, S / 2
    local tipx, tipy = cx + fx * 9, cy + fy * 9
    local lx, ly = cx - fx * 5 - fy * 5, cy - fy * 5 + fx * 5
    local rx, ry = cx - fx * 5 + fy * 5, cy - fy * 5 - fx * 5
    hud_line(M, tipx, tipy, lx, ly, 2, 1, 1, 1, 1)
    hud_line(M, tipx, tipy, rx, ry, 2, 1, 1, 1, 1)
    hud_line(M, lx, ly, cx - fx * 2, cy - fy * 2, 2, 1, 1, 1, 1)
    hud_line(M, rx, ry, cx - fx * 2, cy - fy * 2, 2, 1, 1, 1, 1)
    hud_frame(M, 1, 1, S - 1, S - 1, 1, 0.4, 0.5, 0.6, 0.6)
    hud_text(M, S / 2, 3, "N", 9, 1, 0.5, 0.45, 1, "center")
    local bar = 50 * k
    hud_line(M, 8, S - 8, 8 + bar, S - 8, 2, 0.8, 0.85, 0.9, 1)
    hud_text(M, 12 + bar, S - 15, "50 M", 8, 0.8, 0.85, 0.9, 1)
end

local function updateMap(force)
    if not M then return end
    if clock - map.placesAt > 2 or force then
        map.placesAt = clock
        map.regions = map_regions()
        map.pois = map_pois()
    end
    local v, reason = map_view()
    map.view, map.reason = v, reason
    local key = v and string.format("%.0f:%.0f:%.0f", v.x, v.z, v.heading / 4) or ("x" .. tostring(reason))
    if force or key ~= map.key or clock - map.drawnAt > 2 then
        map.key, map.drawnAt = key, clock
        drawMap()
    end
end

-------------------------------------------------------------------------------------------------- layout & fetch
local function layout()
    local w, h = hud_view()
    if not w then w, h = 1920, 1080 end  -- sizing only; panels are rebuilt when the real view size arrives
    viewW, viewH = w, h
    scale = math.max(0.75, math.min(2.5, h / 1080))
    for _, p in ipairs({W, L, T, M}) do hud_panel_remove(p) end
    W = hud_panel("top_left", 12 / w, 12 / h, 230 * scale / w, 56 * scale / h, 230, 56)
    L = hud_panel("bottom_left", 0, 0, 80 * scale / w, 22 * scale / h, 80, 22)
    tickerUnits = math.max(64, math.min(4096, math.floor((w - 80 * scale) / scale)))
    T = hud_panel("bottom_right", 0, 0, 1 - 80 * scale / w, 22 * scale / h, tickerUnits, 22)
    M = hud_panel("top_right", -12 / w, 12 / h, 180 * scale / w, 180 * scale / h, 180, 180)
    drawWeather(); drawLabel(); drawTicker(); updateMap(true)
end

local function fetchWeather()
    if weatherId then net_cancel(weatherId) end
    local c = CITIES[setting("location")] or CITIES.london
    weatherId = net_get(string.format(
        "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f&current=temperature_2m,weather_code,is_day,wind_speed_10m",
        c[2], c[3]))
end

local function fetchNews()
    if newsId then net_cancel(newsId) end
    newsId = net_get(sourceInfo()[2])
end

return {
    on_start = function()
        local caps = capabilities()
        assert(caps.network and caps.hud and caps.map, "needs a host with network, HUD panels and the city map")
        hud_set("NETWORK HUD DEMO: N OPENS THE CURRENT STORY")
        layout()
        fetchWeather(); fetchNews()
        refreshAt = REFRESH
    end,
    on_tick = function(dt)
        clock = clock + dt
        if math.floor(clock) ~= math.floor(clock - dt) then
            local w, h = hud_view()
            if w and (w ~= viewW or h ~= viewH) then layout() end
        end
        if clock >= refreshAt then
            refreshAt = clock + REFRESH
            if not weatherId then fetchWeather() end
            if not newsId then fetchNews() end
        end
        if clock >= storyUntil then
            if news.state == "ok" and #news.items > 0 and not (noteUntil > clock) then
                shown = shown % #news.items + 1
            end
            drawTicker()
        end
        updateMap(false)
    end,
    on_setting = function(key)
        if key == "location" then
            weather = {state = "loading"}; drawWeather(); fetchWeather()
        elseif key == "news" then
            -- Stories from the previous source are hidden immediately rather than shown under the new label.
            news = {state = "loading", items = {}}; shown = 1
            drawLabel(); drawTicker(); fetchNews()
        end
    end,
    on_action = function(name)
        if name ~= "open_story" then return end
        local item = news.state == "ok" and news.items[shown]
        if not item or (news.fetched and clock - news.fetched > STALE_AFTER) then
            tickerNote = "NO CURRENT STORY TO OPEN"
        elseif not item.link or item.link == "" then
            tickerNote = "THIS STORY HAS NO LINK"
        else
            local ok = pcall(open_link, item.link)
            tickerNote = ok and ("OPENING " .. sourceInfo()[1] .. " STORY IN THE BROWSER")
                         or "STORY LINK IS NOT ON THIS PLUGIN'S APPROVED LINK HOSTS"
        end
        noteUntil = clock + 3
        drawTicker()
        storyUntil = clock + 3
    end,
    on_network = function(id, ok, status, text)
        if id == weatherId then
            weatherId = nil
            if not ok then weather = {state = "error", err = clip(printable(text), 60)}
            else
                local data, err = json_decode(text)
                local cur = type(data) == "table" and data.current
                if type(cur) == "table" and type(cur.temperature_2m) == "number" and type(cur.weather_code) == "number" then
                    weather = {state = "ok", temp = cur.temperature_2m, code = math.floor(cur.weather_code),
                               day = cur.is_day ~= 0, windy = type(cur.wind_speed_10m) == "number" and cur.wind_speed_10m >= 35}
                else
                    weather = {state = "error", err = "bad data: " .. clip(tostring(err or "missing fields"), 40)}
                end
            end
            drawWeather()
        elseif id == newsId then
            newsId = nil
            if not ok then
                news = {state = "error", err = clip(printable(text), 120), items = {}}
            else
                local items, err = feed_items(text, 8)
                local list = {}
                for _, item in ipairs(items or {}) do
                    if item.title and item.title ~= "" then
                        list[#list + 1] = {title = clip(printable(item.title), 200), link = item.link}
                    end
                end
                if items then news = {state = "ok", items = list, fetched = clock}
                else news = {state = "error", err = clip(tostring(err), 120), items = {}} end
            end
            shown = 1
            drawTicker()
        end
        -- Responses for requests replaced by a settings change were cancelled; anything else is ignored.
    end,
}
