# For developers

## Contents

- [Tech stack](#tech-stack)
- [Dependencies](#dependencies)
- [Build](#build)
- [Web browser](#web-browser--3d-web-depth-compositor-windows)
- [Interface and themes](#neonlcars-look-start-menu-and-themes)
- [Notes and limitations](#notes--limitations)
- [Installer and updates](#installer-and-updates-windows)
- [Scripting](#scripting)
- [Credits](#credits)

## Tech stack

| Area | What it uses |
|---|---|
| Language / build | C++20, CMake 3.20+ (dependencies fetched with `FetchContent`), Visual Studio 2022 and vcpkg on Windows |
| Rendering | [Diligent Engine](https://github.com/DiligentGraphics/DiligentEngine) (DiligentCore): Direct3D 12 on Windows, Vulkan on Linux, Metal on macOS. HLSL shaders in `shaders/` (LCARS panels, the 3D world, post-processing, web layers) |
| The city | Procedural. The folder tree is scanned (and cached) and turned into districts, buildings, bus lanes, parks, ponds, the race track and more. Everything is drawn with a few custom pipelines: glass, neon wireframe and reflective grid floors |
| Post-processing | Bloom, light streaks and a dark colour grade, plus MSAA (off, 2x or 4x) |
| Windowing and input | SDL2 (window, keyboard, mouse, game controllers). XInput for Xbox pads |
| VR | OpenXR (SteamVR compatible): head pose, controller rays, swapchains, gaze reticle |
| 2D and in-world UI | A custom UI kit (`UiKit`, `LCARSUI`) on top of `stb_truetype` text, with Inter, Space Grotesk and JetBrains Mono fonts |
| Web browser | Chromium Embedded Framework (CEF) rendering into a texture, with a depth compositor that lifts images and video off the page |
| Media | Windows Media Foundation for video and audio. Metadata is read in-house, plus a background library scan. A SQLite database (`media.db`) stores scraped info and watch progress; WinINet fetches posters, plots, episode lists, artist and album info and lyrics from TVmaze, iTunes, Deezer, Wikipedia and lrclib on a background thread |
| Images and models | `stb_image` and `stb_image_write` for images, `cgltf` for glTF/GLB models such as the car |
| Audio | SDL audio. Built-in sound effects and the electric car hum are synthesised in code (`Sfx`); gameplay plugins can also package bounded WAV and OGG Vorbis samples |
| Hosted Windows apps | Window capture plus a UI Automation helper (`SciFiUia.exe`) that mirrors real programs onto panels |
| Tools | Built-in system monitor, terminal, file browser, image editor, FTP client, calculator and code editor |
| Packaging | Inno Setup installer, an updater script that checks GitHub Releases, and releases published with the `gh` CLI |

## Dependencies

| Dependency | Source |
|---|---|
| CMake >= 3.20, C++20 compiler, git, Python 3 (needed by Diligent's build) | system |
| Diligent Engine (DiligentCore) | fetched automatically by CMake (`FetchContent`, needs network) |
| OpenXR loader | fetched automatically (OpenXR-SDK) |
| Lua 5.4.8 and miniz 3.1.2 | fetched automatically at pinned revisions for restricted script plugins |
| SDL2 (dev package) | Windows: `vcpkg install sdl2`; Linux: `libsdl2-dev`; macOS: `brew install sdl2` |

Backends: **Direct3D 12** (Windows), **Vulkan** (Linux, X11 session or XWayland; needs the Vulkan SDK/drivers),
**Metal** (macOS).

## Build

Windows (Visual Studio 2022, vcpkg):
```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
build\Release\SciFiShell.exe
```

For a release, ensure CEF 156 is present at `third_party\cef` or pass `-DSCIFI_CEF_ROOT=<extracted-CEF-directory>` when configuring; otherwise the browser is omitted. Use a Windows x64 generator and the `Release` configuration. Run `ctest --test-dir build -C Release --output-on-failure` before packaging; the explicit `GameplayArchiveLargeTest --expanded-limit` target also checks the 1 GiB archive and 100 MiB asset boundaries. The [v0.9.22 plugin changelog](plugins.md#v0922-changelog) describes authoring limits and their separate runtime budgets. `build-installer.ps1` writes the supplied version into the staged `version.txt` and excludes test binaries/fixtures from the installer.

Linux:
```
sudo apt install build-essential cmake libsdl2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libvulkan-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/SciFiShell
```

macOS:
```
brew install cmake sdl2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/SciFiShell
```

Pin a Diligent release with `-DSCIFI_DILIGENT_TAG=<tag>` (default `master`). Shaders are copied next to the
executable (`shaders/`) and compiled at start-up.

Inside a file room the `..` card goes to the parent folder (or back to This PC from a drive root); folder cards open
that folder.

## Web browser + 3D Web Depth Compositor (Windows)

Press **B** in the File Room to open a Chromium (CEF, off-screen) browser panel in front of you. Pages are rendered to a
texture; the pointer ray / mouse maps 3D hit -> page pixel -> CEF events. Setup: CEF 156 (Windows 64 "minimal") is
extracted to `third_party/cef`; CMake builds the wrapper automatically and copies the runtime next to the exe. If the
folder is missing the shell builds without a browser. The project now builds as C++20 (required by CEF).

- Toolbar: back / forward / reload / address (click or `Ctrl+L`, Enter to go; plain text searches DuckDuckGo) /
  depth chip / site chip / close. `Alt+Left/Right` back/forward, `F5` reload, `Esc` leaves the address bar,
  pad **B** / VR back closes the panel (page stays alive). Pad/VR stick scrolls, **A**/trigger clicks, wheel scrolls.
- Keyboard: WASD/Space/Ctrl keep flying the camera while the browser is open. Keys go to the page only while a text
  field (or the address bar) has focus; arrows/PageUp/PageDown/Home/End always scroll the page.
- **Depth effects** (`F3` cycles Off / Subtle / Cinematic, `F4` toggles the current site): a read-only injected script
  finds images, video/iframes and stacking contexts and sorts them into 4 depth bands. The compositor draws extra
  quads sampling the same page texture (raised images/video with rim light, soft card shadows, per-band parallax from
  mouse/head). The DOM is never modified and input always hits the base page plane.
- Settings persist in `%LOCALAPPDATA%\SciFiShell\webdepth.ini` (`mode=`, `disable=<host>`).
- Safeguards: analysis runs only after load/input (debounced, <=40 ms budget), caps on cards/media, pages that are too
  complex fall back to a single plane, layers flatten while the layout is stale, and in VR depth is halved and cards
  dropped when frame time exceeds ~14 ms.
- Limitations: no dropdown popups or right-click menu, no video curvature, only Ctrl+V clipboard paste.

## Neon/LCARS look, Start menu and themes

- **The Grid**: dark void with a glowing grid floor; file cards have accent edge lights (brighter on hover).
- **Start menu**: a normal glass window (so it follows you between city, lobby and rooms, and can be moved, resized, arranged and minimised like any other). Open or close it with **middle click**, **F2**, **pad Y** or **left Y/B/menu button in VR**. The centre holds the built-in tools and Settings; the left rail has Power (Exit shell, Restart, Shut down, Lock, Sleep - restart and shut down ask for a second click); the **Now** column lists every open or minimised window (click to bring it to you) with **Arc**, **Tile** and **Minimise all** buttons. Search filters tools as you type; Enter launches the first match and Esc clears the search. It closes itself when you launch something.
- **Themes**: Cyan, Amber, Magenta. Cycle with **F6** or in Settings; the choice is saved in `%LOCALAPPDATA%\SciFiShell\theme.ini`.
- **Screenshots**: press **F12** to save the visible screen area occupied by the shell as a PNG in `Pictures\SciFiShell` (you'll hear a beep; nothing is drawn on screen). Other windows overlapping the shell can appear in the capture; inspect it before sharing.
- **Settings**: the Start menu's Settings tile opens a card based hub (aim + click / A / trigger to open a card, arrow keys / d-pad / wheel to move and scroll longer pages, Esc / B goes back). The **SHELL** card holds app options, saved to `%LOCALAPPDATA%\SciFiShell\settings.ini`. **GRAPHICS** combines performance, lighting and fog controls, including reset buttons. On Windows the other cards are native replacements for the Control Panel (no hosted Windows pages): Display, Sound, Network, Mouse, Personalisation, Power and System. System and Display cards are read-only.
- **Windows (desktop)**: the browser, apps, built-in tools (monitor, terminal, files, editor), settings and the media viewer are all glass windows that behave the same way and stay open until you close them, so you can arrange as many as you like. New windows fan out in front of you. Click a window to raise it and give it the keyboard (the frontmost one under the aim takes the mouse; the world, doors and bus lanes still work around windows). **Drag** any window by its header (or the empty parts of the browser toolbar) to reposition it. Header buttons: minimise (hides it, restore from Alt+Tab or the Start menu), fullscreen, close. **Right click** a window to pin the view to it (the camera glides to centre it and the mouse moves a cursor); right click again, press Esc or click empty space to look around freely. Esc / pad B steps back inside the focused window (leaves the address bar or fullscreen, goes up a page) and never closes it. The middle mouse button / F2 only toggles the Start menu. Alt+Tab lists every window and glides you to a point in front of the one you choose (switching also turns flying on). VR keeps pointing with the controller.
- **Applications (Windows)**: Windows application hosting is experimental and is not offered in the Start menu while compatibility is being improved.
- **City** (the home view - the app starts here): a neon-grid city generated from this PC (fly with the usual controls at 8x speed; buildings and the CPU/RAM/GPU monuments are solid. Aim at a tower and click to enter it (opens that folder as a file room; Esc returns to the city). Windows open on top of the city. CPU (spire + per-core pillars), RAM (slab bank) and GPU (ziggurat + VRAM pylon) sit in the middle and show live load; each fixed/removable drive is a platform sized by its capacity, with a lit pad for used space and one tower per top-level folder (height ~ file count, width ~ bytes), joined to the core by roads. A background scan starts ~1.5 s after launch (so it has usually finished listing by the time you open the city) and the city rises out of the grid the first time you view it. A `ReadDirectoryChangesW` watcher per fixed drive flashes towers (green = added, red = removed) with a beam and resizes them. GPU load/VRAM come from Windows performance counters; if unavailable they fall back to a gentle animation. `SCIFI_OPEN_CITY=1` opens it at start (debug). Click a bus lane (or run the `ride N` script command) to ride along it with a chase camera and a glowing trail; any movement input, click or Esc gets you off.
- Limitations: fog, bloom and audio cues are shader approximations / not implemented; the viewer, web toolbar and editor panels use baked colours and don't follow the theme yet. `SCIFI_OPEN_START=1` opens the Start menu at start (debug).

## Notes / limitations

- VR rendering is implemented for the **Direct3D 12 + OpenXR (SteamVR)** path, i.e. Windows. On Linux/macOS the
  "VR MODE" entry reports that VR needs the Windows D3D12 build and the shell continues in desktop mode.
- Hold Left Shift to move 30% faster.
- Media viewer: click an image/video/audio card (or Enter) and it flies forward, expands with details, and starts playing.
  Click the bar controls (play, seek, fullscreen, close), or use Space pause, Left/Right seek 5 s, Up/Down volume,
  F fullscreen, Esc/Backspace/pad B to exit fullscreen then close.
- Text editor: click a text/code card to open it in the viewer with syntax highlighting (C/C++, JS/TS, Python, JSON,
  HTML/XML, CSS, SQL, Markdown, shell, and more). Mouse wheel / Up / Down / PgUp / PgDn / stick scroll. Press **E**,
  click the pencil or click the text to edit (arrows, Home/End, Ctrl+Home/End, Tab, Ctrl+V paste, click to place the
  caret); **Ctrl+S** or SAVE writes the file; **Esc** leaves edit mode. Closing with unsaved changes asks you to close
  again to discard. Files over 2 MB or binary files open read only.
- The viewer panel is drawn over the room, so it no longer clips into nearby cards.
- Text uses Space Grotesk / Inter / JetBrains Mono from `assets/fonts` (copied next to the exe).
- File Room cards show thumbnails: images (JPG/PNG/etc. via stb_image), video first frames (Windows Media
  Foundation; MKV/WebM may lack codecs), audio cards, and text previews. They load on a background thread.
- Thumbnails are cached as PNGs in `%LOCALAPPDATA%\SciFiShell\thumbs` (`~/.cache/scifishell/thumbs` elsewhere), keyed
  by path, size and modified time; delete the folder to rebuild them.
- Debug env vars: `SCIFI_START_PATH=<dir>`, `SCIFI_CAM_YAW=<degrees>`, `SCIFI_START_MENU=1` (old start menu).
- The project was written without access to a build environment; Diligent's API evolves, so if a call no longer
  matches, pin `SCIFI_DILIGENT_TAG` to a release from the same period as the code.

## Latest additions
- Animated cloud sky in the theme's base colour; the floor grid fades into a horizon mist at any distance.
- Desktop fullscreen (F / bar button): the media fills the whole screen, controls fade after 3 s and return on any input. Images: mouse wheel or Xbox triggers zoom, drag to pan; next/previous stays fullscreen. Mouse / right stick move an on-screen cursor. Esc / B leaves fullscreen. D-pad left/right seeks video/audio.
- Taller file ring (up to 1000 files), bigger flyable room.
- With a file open: click another card to switch, click empty space to close; prev/next arrows on the control bar (Left/Right keys for images).



## Power, blocks and file rooms

- **Power** in the Start menu (or Settings > Power): exit shell, restart or shut down (restart/shutdown need a second click to confirm).
- City: a drive's top-level folders are city blocks; their subfolders are buildings (loose files form a (FILES) building). Buildings have wide bases (12-19 m) on raised sidewalks, with lane-marked streets between rows and columns of buildings.
- Clicking a non-media file opens its file room: details plus OPEN, COPY FILE, COPY PATH, SEND TO DESKTOP, DUPLICATE, RENAME (via Explorer), REVEAL IN EXPLORER and DELETE (Recycle Bin, confirm). Media and text files still open in the fullscreen viewer.


## Building interiors

Entering a building puts you in an atrium (with a fountain in the middle when it has 3+ floors). Each subfolder (plus a "(FILES)" room for loose files) is a door set into the walls. Each floor holds at most 40 doors; bigger buildings stack as many floors as needed, each with a glass-railed walkway and a glass lift on the far wall (walk up to the shaft and the car comes to your level; step in and press a level button inside; there is one CALL button on the wall beside each shaft door). The entrance wall carries doors too (filled last, after the other walls), around a grand glowing EXIT door whose whole doorway is the exit trigger. A room holding just one subfolder is entered automatically. Ceilings carry a single skylight, no strip lights. Doors share a thin white neon outline (round top, square bottom), with labels only on the front face. The lift is recessed so its doors sit flush with the far wall. Movement is Minecraft-creative style, in buildings and the city: Space jumps, double-tap Space toggles flying, then Space rises and Left Ctrl descends; touching a floor ends the flight. Leaving a building puts you on the street outside its door; every city building has a door on its +z face labelled with its folder name. The view only locks onto an image card or window after a long click. The atrium holds no file objects; each door shows its room title, a preview thumbnail and a summary (file counts by type, sample names). Click a door to enter: the gallery room is built on entry, sized by its file count and hung with framed thumbnails that load as you approach. Click a frame to open/play a file (or open a subfolder as a new building). Esc steps back room -> lobby -> city. Walls are solid.

## City cache

The scanned city layout is cached in `%LOCALAPPDATA%\SciFiShell\city.cache` so the city appears immediately on launch; a background rescan then updates it. Folders created after the cache was saved appear on the next run.

## File panel

Clicking a file frame opens a panel with details and actions (open, copy, send, duplicate, reveal, delete). RENAME is done in-app: choose it, type the new name, Enter to apply, Esc to cancel.

## Installer and updates (Windows)

- Build the installer (needs Inno Setup: `winget install -e --id JRSoftware.InnoSetup`): build Release first, then
  `powershell -ExecutionPolicy Bypass -File .\installer\build-installer.ps1 -Version 0.1.0` -> `installer\Output\SciFiShell-Setup-0.1.0.exe`. It installs per-user to `%LOCALAPPDATA%\Programs\SciFiShell` (or Program Files if you choose "all users"), with Start menu / optional desktop shortcuts.
- Publish a release: `powershell -ExecutionPolicy Bypass -File .\installer\build-installer.ps1 -Version 0.2.0 -Publish` (uses `gh release create v0.2.0`, attaching the installer to the public repo `Davidalanfisk/SciFiShell`).
- Updates: an installed shell runs `update.ps1` from its executable directory hidden at launch. It asks GitHub for the latest release, and if the tag is newer than `version.txt` it asks "Download and install now?"; Yes downloads the installer and runs it silently, which closes and restarts the shell. Automatic check errors are silent; the Windows Start menu entry "Check for SciFi Shell updates" reports errors and "up to date" too. Setting `SCIFI_NO_UPDATE` skips the launch check. Source builds do not include the updater or automatically install releases. A taskbar pin to a source build must be replaced manually if you want to run the installed release.
- The repo is public, so the updater needs no token or GitHub account.
- Build prerequisites for source builds: the CEF binary distribution must be extracted to `third_party\cef` (not committed - it is ~420 MB); Diligent and OpenXR are fetched by CMake.

City extras: a ring of shallow steps round every region lets you walk on and off the slabs without jumping; the storage regions sit closer to the central components; the central hub streams data packets to the CPU / RAM / GPU monuments (faster when they are busier) and pulses a beacon.

## Scripting

The app runs commands from a script file: set SCIFI_SCRIPT=<file> at launch, or drop a file at %LOCALAPPDATA%\SciFiShell\control.txt while it runs (it is picked up within 0.5 s and deleted). One command per line, # for comments. Output (`where`, `log`, errors) goes to control.log in the same folder.

| Command | Effect |
|---|---|
| `wait S` | pause |
| `goto X Y Z [S]` | move to a position (optional duration) |
| `walk M [S]` / `strafe M [S]` | move relative to facing |
| `turn DEG [S]` / `look YAW PITCH [S]` | rotate |
| `fly on/off` | creative flight |
| `city` / `enter PATH` / `room N` / `leave` | navigate city, buildings, rooms |
| `tool start` / `tool startpower` | open the Start menu (optionally with Power expanded) |
| `tool settings/theme/monitor/terminal/files2d/editor/media` | launch a tool |
| `sp X Y Z YAW PITCH SECONDS` / `go` | spline camera path: add points (each SECONDS after the previous), then `go` flies them as one smooth move |
| `shot FILE` | save a screenshot to FILE |
| `where` / `log TEXT` / `quit` | diagnostics |

Set SCIFI_NO_SCRIPT=1 to disable. Mouse clicks cannot be injected yet.


## Bus lanes, loader and idle tour
- The Start menu can be opened at any time (F2 / middle mouse / Y), even with apps or the browser open. Several programs and the browser can be open together; the window you aim at has the pointer and keyboard.
- Bus lanes have proper corners. Click a lane in the city to be carried along it to the end of the route like a data packet; move, click or press Esc to step off.
- The floor grid glows near you and fades quickly with distance.
- A white ring loader shows at start-up, while the city builds, and when entering a building or room.
- After 10 s without input in the city the shell flies a tour of the hub, CPU/RAM/GPU and storage regions using the scripting system. Any input stops it. (Scripts: `cam X Y Z YAW PITCH SECONDS` and `ride ROUTE` were added.)

### Bus network routing
Storage routes are planned so they never run under a platform or monument. Lanes are raised on single-leg gantries at staggered heights, so crossings need no bridges. The idle tour is now one continuous, faster flight that swings between low runs through the city blocks and high sweeps.

### Routing, tour and media player (v0.3.0)

- **Bus routing**: routes are planned on a grid (Dijkstra) around platforms, plinths and the hub; crossings are bridged with straight sloped ramps, so lanes never run under a region or through the CPU.
- **Idle tour**: a continuous Catmull-Rom spline flight that dives down between buildings and climbs out; it avoids rooftops by looking ahead and lifting. Any input stops it.
- **Media player**: Start > Media opens the player pages. Choose folders on MEDIA FOLDERS (presets, or the folder you are standing in); a background scan stores a database in `%LOCALAPPDATA%\SciFiShell\media\`. MEDIA TRACKS browses/filters and queues; MEDIA QUEUE shows, saves and loads the playlist (`playlist.m3u8`). Audio keeps playing with all panels closed and auto-advances (shuffle/repeat available); video opens in the media card.

## Credits

- This work is based on "Tron Light Cycle" (https://sketchfab.com/3d-models/tron-light-cycle-083076c8a3644b088ce7f1e107a12ca6) by firestar (https://sketchfab.com/rustyfirestar18), licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/). The glTF was repacked into a single GLB with the 80 m trail geometry removed.
