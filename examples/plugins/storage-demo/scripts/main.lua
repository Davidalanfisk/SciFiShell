return {
    on_start = function()
        local count = storage_get("starts") or 0
        count = math.floor(count) + 1
        storage_set("starts", count)
        hud_set("STARTS: " .. count)
    end
}
