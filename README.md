# SciFiShell

Fullscreen LCARS-style 3D desktop shell prototype: Diligent Engine rendering, SDL2 windowing/input,
XInput (Xbox) controller, and OpenXR VR.

## Dependencies

| Dependency | Source |
|---|---|
| CMake >= 3.20, C++17 compiler, git, Python 3 (needed by Diligent's build) | system |
| Diligent Engine (DiligentCore) | fetched automatically by CMake (`FetchContent`, needs network) |
| OpenXR loader | fetched automatically (OpenXR-SDK) |
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

## Controls

| Input | Menu | File room |
|---|---|---|
| Keyboard | Up/Down select, Enter/Space activate, `V` toggle VR | WASD move, Enter/Space open the card under the crosshair, Esc/Backspace back to menu |
| Mouse | Hover to select, left click activates | Look around; left click opens the card under the crosshair |
| Xbox pad | D-pad up/down select, **A** activate | Left stick move, right stick look, **A** open, **B** back |
| VR (OpenXR) | Right controller ray points, trigger activates | Left thumbstick move, right ray + trigger open card, **B** (or menu button) back |

The app starts directly in the **This PC** room (all drives plus Home). The `..` card goes to the parent folder (or back
to This PC from a drive root); folder cards open that folder. Keyboard: WASD move, Space up, Left Ctrl down, Backspace
(pad **B**, VR **B**) go up a folder, `V` toggles VR, Esc quits.

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

## Tron/LCARS look, tool ring and themes

- **The Grid**: dark void with a glowing grid floor; file cards have accent edge lights (brighter on hover).
- **Tool ring**: open with **middle click**, **F2**, **pad Y** or **left Y/B/menu button in VR**. Look at a segment (or use the mouse wheel / d-pad / arrow keys) and click / **A** / trigger to launch: Files, Settings, Theme, Apps, Power. Apps opens a second ring outside the first with WEB, EDITOR (first text file in the room), MEDIA (first image/video/audio) and WINDOWS; WINDOWS swaps that ring for small transparent icons of your pinned taskbar apps. Esc / B steps back one level. There is no City segment: the city is always loaded. Esc / B / the open button closes it.
- **Themes**: Cyan, Amber, Magenta. Cycle with **F6** or the ring's Theme segment; the choice is saved in `%LOCALAPPDATA%\SciFiShell\theme.ini`.
- **Screenshots**: press **F12** to save the shell window as a PNG in `Pictures\SciFiShell` (you'll hear a beep; nothing is drawn on screen).
- **Settings**: the ring's Settings segment opens a card based hub (aim + click / A / trigger to open a card, arrow keys / d-pad / wheel to move, Esc / B goes back, then closes). The **SHELL** card holds the app options (VR mode, theme, look sensitivity, move speed, invert look Y, crosshair, FPS counter, browser resolution, exit), saved to `%LOCALAPPDATA%\SciFiShell\settings.ini`. On Windows the other cards are native replacements for the Control Panel (no hosted Windows pages): Display (resolution/scaling info), Sound (volume up/down, mute), Network (active adapters and IPs), Mouse (pointer speed, swap buttons), Personalisation (Windows dark mode, shell theme), Power (battery, lock, sleep) and System (PC name, Windows, CPU, memory, uptime, clock, drives). System and Display cards are read-only. Not covered yet: changing resolution, brightness, Bluetooth, Wi-Fi networks, night light, app management.
- **Window behaviour (desktop)**: apps, the browser, settings and the media/text viewer open filling about 85% of the screen as single glass cards with a title bar and small rounded corners. Every window has the same top-right buttons: fullscreen toggle then close (apps also have hide, to the left). Fullscreen (button, F11 for apps/viewer, Esc to leave) works for apps, the browser, settings and media. While the pointer is anywhere over a card (margins and gaps included) the view and walking are frozen and the mouse moves a cursor, so it behaves like a normal fixed window; move the cursor off it and the camera eases round and the keyboard (WASD/arrows) drives the camera again, regardless of which text field last had focus. Press and hold the mouse still on a window for about half a second to glide the view back so the window is centred (aimed straight at its middle) at 85% again. Mouse wheel over a picture zooms into the cursor. Windows can be repositioned by dragging: the title bar (apps), the empty parts of the toolbar (browser) or the title/details/margin of the viewer card. VR is unchanged. The browser's 3D depth options are disabled and hidden for now.
- **Applications (Windows)**: the ring's Apps segment shows an outer ring of icons (from the shell) for Notepad plus every program pinned to your Windows taskbar (`SCIFI_OPEN_APPS=1` opens it at start for debugging; the pin list is read from (read from `%APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar`). Several programs can be open as panels at once; the panel under the pointer takes the keyboard. Caveats: single-instance apps that hand off to an already-running process, apps with splash windows, and elevated apps (e.g. Task Manager) may fail to attach or pick the wrong window. The program runs normally but its window is parked off-screen and mirrored onto a floating panel. Aim + click to use it; once clicked, the keyboard (typing, Ctrl+C/X/V/A/Z) goes to it (hold Alt to walk). Esc / B hides the panel (program keeps running); the X on the panel's bar closes the program, – hides it. Input to the program's text area is injected as window messages; tabs, buttons and menus are clicked through UI Automation by the `SciFiUia.exe` helper (built next to `SciFiShell.exe`, must stay beside it). Menus/flyouts are mirrored into the panel. The hosted window is marked non-activating so clicking it never steals the foreground (and mouse capture) from the shell. There are no hover highlights. **Moving**: press and hold the panel's title bar (the dark strip, not the buttons) and aim around: the panel follows the pointer at the same distance and turns to face you; release to drop it (mouse only; pad/VR taps don't drag yet). **Fullscreen**: F11 or the square button on the panel's bar makes the program fill the screen (the window is resized to the screen's shape; the mouse / right stick move a pointer over it, the room is frozen); Esc / B / F11 returns. **Task switcher**: Alt+Tab (or Ctrl+Tab, which always works) opens a strip of cards (Room, each running program, Browser, Desktop); keep Alt/Ctrl held and press Tab (Shift+Tab goes back) or ←/→ to choose, release to switch; Esc cancels; Desktop minimises the shell. The shell grabs the keyboard so Alt+Tab (and the Win key) reach it instead of Windows; Ctrl+Tab therefore no longer reaches Notepad's tabs.
- **City** (the home view - the app starts here): a Tron-style city generated from this PC (fly with the usual controls at 8x speed; buildings and the CPU/RAM/GPU monuments are solid. Aim at a tower and click to enter it (opens that folder as a file room; Esc returns to the city). Windows open on top of the city. CPU (spire + per-core pillars), RAM (slab bank) and GPU (ziggurat + VRAM pylon) sit in the middle and show live load; each fixed/removable drive is a platform sized by its capacity, with a lit pad for used space and one tower per top-level folder (height ~ file count, width ~ bytes), joined to the core by roads. A background scan starts ~1.5 s after launch (so it has usually finished listing by the time you open the city) and the city rises out of the grid the first time you view it. A `ReadDirectoryChangesW` watcher per fixed drive flashes towers (green = added, red = removed) with a beam and resizes them. GPU load/VRAM come from Windows performance counters; if unavailable they fall back to a gentle animation. `SCIFI_OPEN_CITY=1` opens it at start (debug). The light cycle is not implemented yet.
- Limitations: fog, bloom and audio cues are shader approximations / not implemented; the viewer, web toolbar and editor panels use baked colours and don't follow the theme yet. `SCIFI_OPEN_RING=1` opens the ring at start (debug).

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

- **POWER** on the tool ring (or Settings > Power): exit shell, restart or shut down (restart/shutdown need a second click to confirm).
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
- Publish a release: `powershell -ExecutionPolicy Bypass -File .\installer\build-installer.ps1 -Version 0.2.0 -Publish` (uses `gh release create v0.2.0`, attaching the installer to the private repo `Davidalanfisk/SciFiShell`).
- Updates: an installed shell runs `update.ps1` hidden at launch. It asks GitHub for the latest release, and if the tag is newer than `version.txt` it asks "Download and install now?"; Yes downloads the installer and runs it silently, which closes and restarts the shell. The Start menu has "Check for SciFi Shell updates" (reports "up to date" too). `SCIFI_NO_UPDATE=1` skips the launch check.
- Because the repo is private the updater needs a GitHub token with read access (Contents) to it: put it in `%APPDATA%\SciFiShell\update_token.txt`, or set `GITHUB_TOKEN`, or have `gh` logged in.
- Build prerequisites for source builds: the CEF binary distribution must be extracted to `third_party\cef` (not committed - it is ~420 MB); Diligent and OpenXR are fetched by CMake.

City extras: stepped ramps at the end of every road and hub spoke let you walk on and off the slabs without jumping; the storage regions sit closer to the central components; the central hub streams data packets to the CPU / RAM / GPU monuments (faster when they are busier) and pulses a beacon.

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
| `ring [apps/windows/close]` / `tool files/settings/theme/apps` | command ring |
| `shot FILE` | save a screenshot to FILE |
| `where` / `log TEXT` / `quit` | diagnostics |

Set SCIFI_NO_SCRIPT=1 to disable. Mouse clicks cannot be injected yet.


## Bus lanes, loader and idle tour
- The tool ring can be opened at any time (F2 / middle mouse / Y), even with apps or the browser open. Several programs and the browser can be open together; the window you aim at has the pointer and keyboard.
- Bus lanes have proper corners. Click a lane in the city to be carried along it to the end of the route like a data packet; move, click or press Esc to step off.
- The floor grid glows near you and fades quickly with distance.
- A white command-ring loader shows at start-up, while the city builds, and when entering a building or room.
- After 10 s without input in the city the shell flies a tour of the hub, CPU/RAM/GPU and storage regions using the scripting system. Any input stops it. (Scripts: `cam X Y Z YAW PITCH SECONDS` and `ride ROUTE` were added.)
