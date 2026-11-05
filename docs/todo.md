# SciFiShell TODO

Local planning list. Nothing here is built yet unless marked.

## Revisit

- [ ] **Fog, take two.** The shader fog plus the FOG settings card still doesn't give the look wanted (atmosphere without
  washing out the floors from above). Try a completely different method, for example a screen-space depth fog pass in the
  post-process, volumetric height-fog slabs, or layered fog cards. Then set the tuned values as the new-install defaults.

## 1. VR video auto-detection and dual-mode playback

Unified media-player subsystem that detects VR video layouts and plays them well in both Desktop and VR mode.

### 1.1 Detection pipeline (deterministic, confidence-scored)
- [ ] Formats: SBS stereo, OU/TB stereo, 180 fisheye, 360 equirectangular, mono vs stereo, custom fisheye (best effort)
- [ ] Resolution analysis: SBS width about 2x height; OU height about 2x width; 360 aspect about 2:1; fisheye square or near-square
- [ ] Frame similarity between left/right or top/bottom halves to find stereo pairs
- [ ] Distortion sampling (fisheye curvature, equirectangular stretching)
- [ ] Metadata parsing (MP4/MKV projection and stereo tags)
- [ ] Filename heuristics: SBS, OU, TB, LR, 180, 360, VR, 3D, fisheye
- [ ] Output a confidence score and a badge such as "Detected: SBS Stereo (VR)"; fall back to manual override below 0.6

### 1.2 Desktop playback (2D)
- [ ] Auto-detect, crop to left eye (top eye for OU), show as normal 2D
- [ ] Mouse panning for 180/360 content
- [ ] "Playback Mode" dropdown: Auto (recommended), Flat 2D, SBS to 2D (left eye), OU to 2D (top eye), Raw Frame, 180 preview (pan), 360 preview (pan)

### 1.3 VR playback (immersive)
- [ ] Correct projection: SBS/OU stereoscopic sphere, 180 dome, 360 full sphere
- [ ] Head tracking (smooth), zoom, controller input, UI slider
- [ ] Mode override: Auto, SBS, OU, 180, 360 (custom lens profiles later)
- [ ] Reset view / re-centre, adjust FOV, toggle stereo, comfort mode (rotation dampening)

### 1.4 Manual override
- [ ] Auto, Flat, SBS, OU, 180, 360, Raw Frame, Custom Lens Profile (future)
- [ ] Persist overrides per file (can reuse the media DB)

### 1.5 Later
- [ ] Smart eye selection (better contrast/brightness)
- [ ] De-warp shader for fisheye previews on desktop
- [ ] Cache detected format per file
- [ ] VR theatre mode (play inside a virtual room)

## 2. H.A.R.V.I - integrated digital assistant and help system

Acronym chosen: **Holographic Autonomous Response & Virtual Intelligence** (option A).

Holographic blue fish-like avatar (angular, distinct from Magikarp), summoned with **F1**. Personality: helpful, quirky,
dry, friendly but not infantilised, clearly non-human (Bob, Clippy, JARVIS, HAL 9000, Marvin).

### 2.1 Summon and dismiss
- [ ] F1 summons: floats near the focus point (desktop) or in front of the user (VR)
- [ ] Dismiss with F1, Esc or a Dismiss button; holographic dissolve animation

### 2.2 Desktop behaviour
- [ ] Small avatar bottom-left or centre, with a dialogue panel (help text, buttons, quick actions, tutorials, hints, module shortcuts)
- [ ] Contextual help for the active window, links to settings modules, onboarding tips, "Did you know?" hints, troubleshooting
- [ ] Scripted dialogues and UI walkthroughs that highlight elements

### 2.3 VR behaviour
- [ ] Floating hologram with volumetric glow, idle animation, head/eye alignment
- [ ] Spatial help (pointing at UI), floating 3D buttons and dialogue bubbles
- [ ] VR tutorials: movement, grabbing, window manipulation
- [ ] Text responses now, audio later

### 2.4 Interaction engine
- [ ] JSON dialogue trees, branching flows, contextual triggers (e.g. first time opening Settings)
- [ ] Buttons, lists, toggles, quick actions, module shortcuts
- [ ] Modular "brain": local LLM, remote LLM, scripted fallback when none is available

### 2.5 Avatar and rendering
- [ ] Blue holographic fish: angular geometry, cyan accents, transparent fins, scanlines, particle trails
- [ ] Custom hologram shader, idle loop, summon/dismiss animations, emotion states (happy, confused, thinking)

### 2.6 UI components (follow the existing design system)
- [ ] Dialogue panel, button strip, tooltip bubbles, quick-action menu, tutorial highlighter, desktop context panel, VR floating 3D buttons

### 2.7 Later
- [ ] Voice synthesis and recognition, personality tuning, memory, skins, mini-games, mood from system telemetry, world-space companion

## 3. Plugin system, in-game app store and server infrastructure

### 3.1 Plugin architecture
- [ ] Single `.zip` package: `plugin.json`, compiled modules, assets (UI, textures, models, audio), scripts, metadata, optional settings schema
- [ ] Manifest: name, version, author, description, required SciFiShell version, supported modes (Desktop, VR, Worldspace), dependencies, permissions
- [ ] Extension points: UI panels, system modules, window manager, VR interactions, world-space objects, 3D scenes, HARVI dialogue packs, media player extensions, shaders, input mappings, persistent spaces, gameplay systems
- [ ] Load/unload at runtime, sandboxing where possible, safe fallback mode, plugin registry in the user profile
- [ ] "Plugins" settings module

### 3.2 Install workflow
- [ ] Desktop: drag-and-drop a zip, or an "Install Plugin" button; validate manifest, extract to the user plugin dir, register, load immediately
- [ ] VR: floating install panel or a "Plugin Terminal" object; HARVI gives feedback
- [ ] Uninstall: unload, delete the directory, update the registry

### 3.3 In-game app store (future)
- [ ] Browse, screenshots, descriptions, install/update/remove, ratings and reviews, settings, featured picks
- [ ] 3D storefront in VR, holographic panels, categories, search, detail pages, HARVI recommendations

### 3.4 Server (future)
- [ ] Plugin hosting and CDN, metadata API (REST or GraphQL), versioning, downloads, update notifications, developer submission portal
- [ ] Optional accounts, cloud sync, shared persistent spaces, vehicle ownership, presence, messaging, district matchmaking
- [ ] Secure upload pipeline, scalable multiplayer-ready architecture

### 3.5 Security
- [ ] Permission model with user confirmation (network, filesystem, VR input, world-space manipulation, HARVI, shader injection)
- [ ] Manifest validation now, signature verification later, safe fallback if a plugin fails to load

### 3.6 Later
- [ ] Dependency resolution, update notifications, ratings, HARVI plugin tutorials, vehicle plugins, custom districts, multiplayer packs, developer SDK and docs

## Carried over

- [ ] Visualizer: projectM hardware-accelerated option (see `visualizer-plan.md`)
- [ ] `mediaplay` script bug
- [ ] Re-record the demo video
- [ ] `docs/windows-input-plan.md`
- [ ] Bug-fix evening before 1.0
- [ ] Listening check of the car hum and SFX slider
- [ ] Ship v0.9.16 (media overhaul, fog sliders) once approved
