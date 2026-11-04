#pragma once
// User preferences edited from the settings panel; persisted in %LOCALAPPDATA%\SciFiShell\settings.ini.

namespace sf {

struct Settings {
    int lookIdx = 2;      // index into kLookScale
    int speedIdx = 2;     // index into kSpeedScale
    bool invertY = false;
    bool crosshair = true;
    bool showFps = false;
    int webQuality = 2;   // browser resolution preset (WebBrowser::kQualityCount presets)
    int aaIdx = 2;        // anti-aliasing: 0 off, 1 2x, 2 4x (applies on restart)
    int distIdx = 2;      // draw distance index into kDistScale
    bool reflections = true;
    bool postFx = true;   // bloom, light streaks, grade
    bool vsync = true;
    bool carryWins = true;  // windows follow you between the city, lobbies and rooms
    int arrangeKey = 0;     // SDL keycode that arranges windows; 0 = the default, F8
    bool snapWins = true;   // dragged windows snap to the edges and height of their neighbours
    float sfxVol = 1.0f;  // sound effects volume, 0..1
    bool showCity = true;  // the city is the home view; off saves the generation and drawing cost
    float lightSpot = 1.07f, lightCap = 0.02f, lightBase = 0.01f, lightAmbient = 0.0f, lightFalloff = 0.6f;  // interior lighting sliders
    static constexpr float kDistScale[4] = {0.4f, 0.65f, 1.0f, 1.3f};
    float distScale() const { return kDistScale[distIdx]; }

    static constexpr int kSteps = 5;
    static constexpr float kLookScale[kSteps] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    static constexpr float kSpeedScale[kSteps] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    float lookScale() const { return kLookScale[lookIdx]; }
    float speedScale() const { return kSpeedScale[speedIdx]; }

    void load();
    void save() const;
};

}  // namespace sf
