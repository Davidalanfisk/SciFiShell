local elapsed = 0

return {
    on_start = function()
        sound_loop(sound_play_asset(asset_sound("tone.wav")))
        sound_loop(sound_play_asset(asset_sound("tone.ogg")))
    end,
    on_tick = function(dt)
        elapsed = elapsed + dt
        if elapsed >= 1 then
            elapsed = 0
            send_event("race_complete", 123.5)
        end
    end
}
