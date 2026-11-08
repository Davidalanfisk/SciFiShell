on_event("race_complete", function(time)
    if time ~= 123.5 then error("unexpected race time") end
    hud_set("MESSAGE RECEIVED")
end)

return {
    on_start = function()
        hud_set("WAITING")
    end
}
