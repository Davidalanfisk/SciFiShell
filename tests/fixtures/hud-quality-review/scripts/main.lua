return {
    on_start = function()
        local weather = hud_panel("top_left", 0.02, 0.03, 0.28, 0.18, 360, 130)
        hud_clear(weather, 0.02, 0.05, 0.12, 0.95)
        hud_frame(weather, 2, 2, 356, 126, 3, 1, 0.6, 0.1)
        hud_text(weather, 12, 8, "WEATHER  LONDON", 20, 1, 0.7, 0.2)
        hud_text(weather, 12, 46, "8.9 C  wind 9 km/h", 28, 0.85, 0.95, 1)
        hud_text(weather, 12, 100, "DISPLAY-DENSITY TEXT  0123456789", 14, 0.5, 0.7, 0.9)

        local overlap = hud_panel("center", 0, 0, 0.8, 0.24, 1024, 180)
        hud_clear(overlap, 0.01, 0.02, 0.05, 0.9)
        hud_frame(overlap, 2, 2, 1020, 176, 3, 0.3, 1, 0.5)
        hud_text(overlap, 512, 16, "HUD ABOVE APP WINDOWS", 32, 1, 1, 1, 1, "center")
        hud_text(overlap, 512, 74, "Logical coordinates unchanged; text rasterized at display density.", 22, 0.7, 1, 0.8, 1, "center")
        hud_text(overlap, 512, 126, "Display-only HUD: clicks still pass through to the app window.", 20, 0.5, 0.8, 1, 1, "center")

        local ticker = hud_panel("bottom", 0, -0.03, 0.9, 0.09, 1024, 64)
        hud_clear(ticker, 0.02, 0.04, 0.1, 0.95)
        hud_text(ticker, 16, 8, "NEWS  Deterministic review text: same view, same content, before and after.", 20, 1, 0.85, 0.6)
        hud_text(ticker, 16, 40, "No live requests, credentials or user location in this comparison.", 14, 0.5, 0.7, 0.9)
    end,
}
