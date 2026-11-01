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
