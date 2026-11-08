# Plugins

## Bundled gameplay plugins

Settings > Plugins lists the built-in **Car and Track** and **Parks and Toys** plugins. Both are enabled by default; select either to disable or re-enable it immediately. Disabling driving exits the car and removes the track; disabling parks removes the parks, ducks, stands and active snacks. Preferences are stored separately from other settings in `%LOCALAPPDATA%\SciFiShell\plugins.ini` on Windows (`~/.config/scifishell/plugins.ini` elsewhere). If the preference file cannot be read or written, the page reports an error and does not overwrite the file.

The gameplay plugins use two trusted native modules built alongside the application. The driving module contains track generation, sampling and rendering, driving simulation and car rendering; the parks module contains scenery, ducks and toy simulation and rendering. Host-side integration and some interaction and scoring logic remain in the executable. The modules must be present next to the executable, are loaded only when their features are enabled, and are unloaded on disable. They run with the application's full privileges, **not in a sandbox**. They are bundled modules, not independently installable third-party packages; Settings does not install native code. Settings > Plugins also installs **data-only theme packages**: choose a `.sfplugin` file, then enable it on the Plugins page and cycle to it using Settings > Shell > Theme. Theme packages have no executable code, external files, network or filesystem permissions. Installed packages are discovered from the per-user `SciFiShell/plugins` directory at startup and start disabled unless previously enabled.

A `.sfplugin` is one UTF-8/ASCII JSON file of at most 16 KiB, not a ZIP. All fields below are required; unknown fields, duplicate keys, permissions other than an empty array, and types other than `theme` are rejected. Strings use printable ASCII; `\"`, `\\` and `\/` are the supported escapes. IDs are lowercase reverse-domain-style letters, digits, dots and hyphens; `org.scifishell.*` is reserved. Colours are `#RRGGBB`.

An installable example is in `../examples/plugins/twilight.sfplugin`.

```json
{"id":"com.example.twilight","name":"TWILIGHT","version":"1.0.0","author":"Example","description":"A violet theme","type":"theme","platforms":["any"],"permissions":[],"accent":"#aa22ff","alt":"#12ccdd"}
```

**Restricted script plugins.** Settings > Plugins > Install Package also accepts a `.zip` containing exactly `plugin.json` and `scripts/main.lua` (an optional `scripts/` directory entry is allowed). An example is in `../examples/plugins/clock`; ZIP the *contents* of that folder, not the folder itself. The manifest requires the fields shown in that example: `type` is `script`, `entry` is `scripts/main.lua`, `platforms` is `["any"]`, `permissions` is `[]`, and `min_shell_version` is currently exactly `0.1.0`. Unknown fields and other capabilities are rejected. The archive is limited to 256 KiB and each file to 16 KiB; duplicate, unexpected, absolute, traversal and link entries are rejected. Files are read from the archive into memory, never extracted to archive-specified paths.

Scripts contribute a **Run** card to Settings > Plugins. Enable the plugin, then select Run to display its returned message in Settings. The Lua source must return 1–160 printable ASCII characters; long messages can be truncated to fit the panel. Each execution uses a fresh Lua 5.4 state with no standard libraries, module loader, filesystem, network, OS, input, or native-code API. Its only host function is `unix_time()`. Execution is limited to 100,000 Lua instructions and 1 MiB of Lua allocations; errors are displayed, and a failed enable does not persist an enabled state. This is a restricted in-process runtime, not OS-level isolation or a guarantee against interpreter vulnerabilities. Installing native executable extensions is not supported.

Installed packages start disabled unless a retained preference says otherwise. Installing never overwrites an existing package. To uninstall an installed package, select its **Remove** card twice within 15 seconds; this disables it first and removes the package file, leaving other user data untouched. Built-in plugins cannot be uninstalled. ZIP themes, arbitrary ZIP layouts, plugin data storage, updates and an app store are not implemented. Do not treat native code as sandboxed.

## Lua plugin authoring specification

[Download the example clock plugin ZIP](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.19/scifishell-clock-example.zip); its [source files](../examples/plugins/clock) are included in this repository. It contributes a **Run CLOCK** card which displays the current Unix time. This section describes the message-script type; the separate, experimental gameplay type is described below.

A script ZIP must have these exact, case-sensitive root paths:

```text
plugin.json
scripts/main.lua
```

An empty `scripts/` directory entry is optional; no other entries are allowed. ZIP the **contents** of the example directory, not the containing directory. The archive is limited to 256 KiB; each uncompressed file is limited to 16 KiB, and the script must not be empty. Duplicate paths, absolute paths, traversal, backslashes, symlinks, encryption, unsupported ZIP entries and corrupt CRCs are rejected. Archive paths are read into memory, never extracted to the filesystem.

The manifest must contain exactly the following fields; extra and duplicate keys are rejected:

```json
{
  "id": "com.example.clock",
  "name": "CLOCK",
  "version": "1.0.0",
  "author": "Example",
  "description": "Show the current Unix time",
  "entry": "scripts/main.lua",
  "type": "script",
  "platforms": ["any"],
  "permissions": [],
  "min_shell_version": "0.1.0"
}
```

| Field | Requirement |
|---|---|
| `id` | Unique, 3–80 ASCII characters, using lowercase letters, digits, `.` and `-`; at least one dot, no leading, trailing or consecutive dots. `org.scifishell.` is reserved. |
| `name` | Nonempty, at most 40 printable ASCII characters; labels the Settings cards. |
| `version` | Three dot-separated numeric components, at most 24 characters total, e.g. `1.0.0`. |
| `author` | Nonempty, at most 80 printable ASCII characters. |
| `description` | Nonempty, at most 160 printable ASCII characters. |
| `entry` | Exactly `scripts/main.lua`. |
| `type` | Exactly `script`. |
| `platforms` | Exactly `["any"]`. |
| `permissions` | Exactly `[]`; message scripts cannot request permissions. |
| `min_shell_version` | Exactly `0.1.0`; this is currently a fixed value, not a general version comparison. |

JSON strings accept printable ASCII only; `\"`, `\\` and `\/` are the only supported escapes. Fields are strings except for the two specified string arrays. Numbers, booleans, null and nested objects are not accepted. The Lua file is text-mode Lua 5.4 source. For example:

```lua
return "UNIX TIME: " .. unix_time()
```

The chunk must return a **string of 1–160 printable ASCII characters**; it may be truncated visually to fit the panel. `unix_time()` is the only supplied host function and returns the current Unix timestamp as an integer. Lua language operators, local variables, conditionals, loops and functions work subject to the budget, but **no standard libraries or base functions** are opened: for example `print`, `tostring`, `string`, `math`, `io`, `os`, `require`, `load` and `debug` are unavailable. Each execution starts a fresh state, with no persistence between runs. It is limited to 100,000 Lua VM instructions and 1 MiB of Lua allocations.

To package the example on Windows with Python, run this from the repository root:

```powershell
python -c "import zipfile; z=zipfile.ZipFile('clock.zip','w',zipfile.ZIP_DEFLATED); z.write('examples/plugins/clock/plugin.json','plugin.json'); z.write('examples/plugins/clock/scripts/main.lua','scripts/main.lua'); z.close()"
```

To install, open **Settings > Plugins > Install Package** and select the ZIP. Installation validates and copies it without overwriting an existing package. The input filename may be arbitrary; the installed copy is named `<id>.zip`. At most 64 theme and script package files are supported. Select the plugin name to enable it: enabling executes the script once as validation, and failure does not persist the enabled state. Select **Run** to execute it again and display its message; no script runs continuously. Select the name again to disable. Select **Remove** twice within 15 seconds to uninstall; the shell disables the plugin and removes its package. A retained disabled preference means reinstalling the same ID stays disabled.

On startup the shell discovers packages in `%LOCALAPPDATA%\SciFiShell\plugins` on Windows (`~/.config/scifishell/plugins` elsewhere); saved enablement is in the adjacent `plugins.ini`. New IDs default to disabled. An enabled script is validated again on startup; errors leave it marked **ERROR** rather than running. The **PLUGIN STORAGE** and **PACKAGES** cards show preference and discovery errors, and Settings notices show install, enable, run and removal errors. Check manifest fields, paths and budgets first if a package fails. A malformed preferences file is not overwritten.

Lua runs in-process, not in an OS sandbox, and interpreter vulnerabilities remain possible. No filesystem, network, OS, input, native code, module loader or persistent data API is exposed to message scripts.

## Experimental gameplay scripts

Gameplay scripts are a separate package type and are **not yet equivalent to Car and Track or Parks and Toys**. Those features remain trusted bundled native modules. Gameplay scripts can create boxes and simple textured models, update them over time, register click targets, and optionally receive movement input, query city collision and cell occupancy, play built-in sounds, display a short HUD message, apply a bounded player push, and offer explicitly activated, bounded player/camera control. They cannot register shared scores, create arbitrary meshes, or reproduce the bundled gameplay. Their API and limits are preliminary. The v0.9.19 clock download does not contain this functionality.

Download [scripted-car-track.zip](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.20/scripted-car-track.zip) and [scripted-parks-toys.zip](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.20/scripted-parks-toys.zip), or build them from this checkout with `python examples/plugins/build-gameplay-examples.py <empty-output-directory>`. Install them through Settings, then explicitly enable and consent to each. The second ZIP combines the nearby park and snack examples below; it uses the snack score as its HUD message. These examples are simplified, not replacements for the bundled native plugins. Disable the native plugins separately if you want to avoid overlapping features. The builder refuses to overwrite an existing example ZIP.

Scripted click targets consume their clicks before native shell actions, so a toy click cannot also open an object behind it. Picking uses bounding boxes and does not check city occlusion: an interactive object behind a wall can still receive a click.

An example is in [`examples/plugins/gameplay-duck`](../examples/plugins/gameplay-duck). To create its ZIP from the repository root, including the existing bundled Duck model:

```powershell
python -c "import zipfile; z=zipfile.ZipFile('gameplay-duck.zip','w',zipfile.ZIP_DEFLATED); z.write('examples/plugins/gameplay-duck/plugin.json','plugin.json'); z.write('examples/plugins/gameplay-duck/scripts/main.lua','scripts/main.lua'); z.write('assets/models/Duck.glb','assets/duck.glb'); z.close()"
```

[`examples/plugins/duck-throw`](../examples/plugins/duck-throw) demonstrates read-only view pose, a clickable launcher, city floor and collision queries, built-in sounds, a HUD counter and bounded, short-lived scripted projectiles. Create its ZIP with the same command, replacing `gameplay-duck` with `duck-throw` in the output filename and both source paths. It is an illustration, not a replacement for the bundled toys: it does not support pickup, explosions or scoring.

[`examples/plugins/park-cell`](../examples/plugins/park-cell) demonstrates finding up to three distinct vacant cells near the view, placing a small park and duck in each, and removing or relocating them as the city or view changes. Package it with the same command, replacing `gameplay-duck` with `park-cell` in the output filename and both source paths. It is not a replacement for the native city-wide parks and toys: its bounded nearby sample does not enumerate every city cell.

[`examples/plugins/snack-stand`](../examples/plugins/snack-stand) demonstrates two plugin-owned stands, ray picking, hotdog and ice cream pickup, bouncing projectiles, a temporary explosion and nearby player push, a wandering Duck model, plugin-local scoring and respawn. Create its ZIP with the gameplay-duck command, replacing `gameplay-duck` with `snack-stand` in the output filename and both source paths. It is not a replacement for the bundled toys: it spawns a single set of simplified objects near the view and does not interact with native ducks.

[`examples/plugins/car-track`](../examples/plugins/car-track) demonstrates a 32-segment terrain-sampled oval and a car using a packaged GLB body and PNG texture, with script-owned acceleration, steering, wheel motion, simple automatic oval-rail following, and city floor/collision queries. ZIP `plugin.json`, `scripts/main.lua`, `assets/car.glb`, and `assets/paint.png` at those exact paths; omit the generator script and directory entries. `generate-assets.py` recreates the two original example assets. Approach the car and press G to activate a bounded chase view; Escape or G releases it. Outside control mode WASD still moves the player and remotely drives the car. The fixed oval and simplified rail simulation do not replicate the native generated track or its physics; the car does not use CarConcept. Track heights are refreshed one sample every 0.25 seconds while the city is available; changes can take at least eight seconds to cover the oval. This is a driving API experiment, not a replacement for the native Car and Track.

To package it from the repository root:

```powershell
python -c "import zipfile,pathlib; p=pathlib.Path('examples/plugins/car-track'); z=zipfile.ZipFile('car-track.zip','w',zipfile.ZIP_DEFLATED); [z.write(p/n,n) for n in ('plugin.json','scripts/main.lua','assets/car.glb','assets/paint.png')]; z.close()"
```

Gameplay ZIPs must contain `plugin.json`, `scripts/main.lua`, and optionally flat `assets/<name>.glb` or `assets/<name>.png` files. Asset names use lowercase ASCII letters, digits, hyphens, underscores and dots; names beginning with a dot or containing `..` are rejected. Directory entries are not accepted. The archive is limited to 8 MiB compressed and expanded, each asset to 2 MiB, each text file to 16 KiB, and the archive to 34 entries. Duplicate, unexpected, absolute, traversal, backslash, link, encrypted, unsupported and corrupt entries are rejected. Files are read in memory and never extracted using archive paths.

The manifest has exactly `id`, `name`, `version`, `author`, `description`, `entry`, `type`, `platforms`, `permissions` and `api_version`. The first five follow the message-script rules above. Set `entry` to `scripts/main.lua`, `type` to `gameplay`, `platforms` to `["any"]` and `api_version` to the **string** `"1"`. `permissions` must contain `scene`, and may additionally contain `world`, `layout`, `input`, `view`, `sound`, `hud`, `impulse` and `control`; duplicates and unknown permissions are rejected. `layout` and `impulse` also require `world`; `control` requires `world`, `input` and `view`. `scene` allows visible objects, `world` permits city floor and collision queries, `layout` reveals city cell positions and occupancy, `view` reads the user's in-world camera position and direction, `sound` plays built-in effects, `hud` displays text, `impulse` applies bounded player pushes, `control` offers player/camera control, and `input` provides movement axes and exclusive click routing while interacting with the world, not while typing or interacting with UI. The host does not expose typed text.

Install through **Settings > Plugins > Install Package**. New packages start disabled. Selecting a gameplay plugin without a matching grant displays its ID and requested permissions; select it again within 30 seconds to consent and enable it. Consent is stored against the exact ZIP contents, so changed contents require consent again. Disabling releases its objects and GPU resources but retains consent for re-enabling; uninstall revokes consent. Use **Disable** to stop a failed plugin, or select its name to retry. An enabled plugin can restart automatically if its unchanged package still has consent. Callback errors stop the plugin and are reported in Settings and stderr. Do not approve packages you do not trust: Lua and image/model decoders run in the shell process, not an OS sandbox.

The script returns a table with optional `on_start()`, `on_tick(dt)`, `on_input(control, value)` and `on_stop()` functions. The chunk and each callback have a 100,000-instruction budget; each plugin has 1 MiB of Lua allocations. `dt` is seconds, capped by the host at 0.1 per frame. Only Lua's `math` library and the functions below are opened: there is no base library, filesystem, network, module loader or operating-system API. Callbacks run on the application thread; exceeding the budget or raising an error stops the plugin and releases its scene.

* `scene_box(x,y,z,width,height,depth,r,g,b)` and `scene_model(model,texture,x,y,z,width,height,depth,r,g,b)` return owned integer handles. Texture `0` uses the GLB's embedded texture or white; otherwise use `asset_texture(path)`. `asset_model(path)` and `asset_texture(path)` return a package-local numeric asset ID or raise an error for an unknown path.
* `scene_move(handle,x,y,z)`, `scene_rotate(handle,x,y,z)` (radians), `scene_resize(handle,width,height,depth)`, `scene_colour(handle,r,g,b)` and `scene_remove(handle)` update owned objects. Positions and rotations must be finite and at most 10,000 in magnitude, sizes must be in `(0,1000]`, and colour components in `[0,1]`. Up to 128 boxes and model instances combined are allowed.
* `scene_hit(handle,ox,oy,oz,dx,dy,dz,maxDistance)` tests a ray against the object's transformed unit box and returns the entry distance in world units, or nil on a miss. The origin must be finite within ±10,000, each direction component must be finite in `[-1.001,1.001]` and the nonzero direction is normalized; distance must be in `[0,100]`. A ray starting inside returns zero. This is a box approximation for models, not a triangle, city-occlusion or cross-plugin query. Unknown or removed handles fail the callback. It does not consume click input or affect shell interaction.
* With `world`, `world_available()` reports whether the city is active and built. `world_floor(x,y,z,radius,eyeHeight)` returns a city floor height; `world_resolve(x,y,z,radius,eyeHeight)` returns a collision-adjusted position. They fail when the city is unavailable; radii and eye heights must be between 0 and 100.
* With `world`, `world_track_clearance(x,z,radius)` returns the maximum height of native track-generation obstacle footprints intersecting the square from `(x-radius,z-radius)` to `(x+radius,z+radius)`, or zero if none intersects. It uses monuments, platforms and current/target tower dimensions, matching the native generator's obstacle inputs; it is **not** a complete city collision or bridge-clearance query. Coordinates must be finite within ±10,000 and radius between 0 and 100. It fails if the city is unavailable, the data is invalid, or the combined traversal exceeds 4,096 monuments, platforms, blocks and towers. It shares the 64-call callback budget. City geometry can change, so callers must resample.
* With `layout` and `world`, `city_cell(x,z)` returns `centerX, floorY, centerZ, pitch, occupied` for the grown city grid cell containing the point, or nil outside a cell. It reveals positions and occupancy, not names or paths. It fails when the city is unavailable or a query exceeds 4,096 examined platforms, 4,096 examined blocks or 4,096 towers in the selected block. City layout and occupancy may change; scripts must recheck and remove or relocate their objects. It does not enumerate every cell or reserve one, and multiple plugins may select the same cell.
* With `view`, `view_pose()` returns `x,y,z,dx,dy,dz` for the in-world camera or VR headset; it returns nil before entering the file room. Direction is the view direction, not a VR controller pointer. This is read-only and available while panels are open. At most 64 scene-hit, world, layout and view calls combined are allowed per callback.
* With `input`, `on_input` receives `move_x`, `move_y`, `move_z` axis changes in `[-1,1]` and routed `click` press/release values `1`/`0`. With `scene` and `input`, `scene_interactive(handle,true)` makes an owned object a click target; `false` removes it. The host chooses the nearest target within 14 units, breaking equal-distance ties by plugin ID, and consumes the click before native shell actions; misses go to the shell instead. Removing an object removes its target. `scene_hit` alone does not register one. With an active control lease, clicks go to its owner instead. A plugin can call `input_capture_clicks(true)` in a routed click press to capture later world clicks (for example while holding an item), then call `input_capture_clicks(false)` when finished. Capture cannot start without a routed press, never overrides UI or unavailable input, and is cleared on disable, failure or uninstall. The host resets held input when interaction becomes unavailable, including UI focus, native driving, VR and focus loss. Picking uses approximate object boxes, does not account for city occlusion, and targets behind walls can receive clicks. These are desktop movement and mouse events, not a controller or VR input interface.
* With `sound`, `sound_play(name, volume)` queues one built-in effect: `pickup`, `throw`, `boom`, `duck_pop`, `car_summon` or `rail_on`. Volume must be finite and in `[0,1]`, and the shell's sound volume applies. At most eight events can be queued before the host processes them; the host permits a burst of four and replenishes four per second per plugin. Exceeding either limit, lacking an audio device or exhausting the shared voice limit stops the plugin with an error. Events from a failed callback are discarded; disabling, failing or uninstalling a plugin stops its active voices. Already-buffered audio may finish playing. Arbitrary audio files and positional audio are not supported.
* With `sound`, `sound_engine(active, speed, boost)` controls one plugin-owned synthesized car hum. `active` and `boost` are booleans; `speed` is finite and between 0 and 110. Updates are coalesced until the host processes them; the latest value wins. At most four plugin hums run at once. Starting one without an audio device or when the hum limit is reached stops the plugin with an error. Turning it off, disabling, failing or uninstalling removes the owner's hum; already-buffered audio may finish playing. This is the built-in synthesized sound, not packaged or positional audio.
* With `hud`, `hud_set(text)` sets one plugin-owned, ID-labelled message of up to 32 printable ASCII characters; an empty string clears it. Messages appear in the file room and are coalesced at most four times per second per plugin. Up to four running plugins can display messages, stacked in ID order. Stopping, failing or uninstalling removes the message. This is display text only, not a shared scoring API.
* With `impulse` and `world`, `player_impulse(x,y,z)` queues a push velocity in world axes. Values must be finite, horizontal magnitude at most 12 and vertical between 0 and 8. Only one push may be pending; a plugin may apply one every two seconds, with excess requests stopping it. Pushes decay over time and the combined displacement from all plugins is capped at 12 units/second horizontally and 8 vertically before normal world collision and gravity. Pushes are discarded and residual motion cleared when world interaction is unavailable, including UI focus, driving, VR and loss of window focus; disabling or failing a plugin clears its motion. This does not reproduce the native explosion's exact physics.
* With `control`, `control_offer(x,y,z)` advertises a finite position within ±10,000; `control_offer(nil)` withdraws it. While the focused city world is available, the nearest offer within four units is shown with its plugin ID and G activates it. Only one owner can hold control. The host sends `on_input("control",1)` on activation and `on_input("control",0)` on release; the script must not propose movement until activated. While active, `control_pose(x,y,z,yaw,pitch)` queues at most one absolute player/camera pose per host tick, with finite coordinates and yaw within ±10,000 and pitch within ±1.4 radians. Input callbacks and `on_tick` share that queue, so normally propose the pose only from `on_tick`. The host limits movement to 25 units/second and yaw/pitch changes to 4 radians/second, enforces city bounds, and resolves collision in small steps; invalid proposals stop the plugin. Collision adjustments that cannot fit within the movement or world bounds block further motion instead of stopping the plugin. Camera motion can lag a vehicle or stop on collision. Escape or G releases control; leaving the city, overlays, focus loss, native driving, riding or VR also releases it. Other plugins do not receive movement/click input while control is held. Disable, failure and uninstall release it. This is not an OS security boundary or the native car physics.

PNG textures are limited to 1024×1024, must pass checksum and decoder checks, and must be opaque to render. GLB support is deliberately narrow: up to 32 meshes and 32 nodes are flattened into one static triangle model, with at most 64 total primitive instances sharing one material, embedded binary data, a simple base-colour material and at most one embedded PNG. Reused meshes count separately for every node. Geometry is capped at 65,536 vertices and 196,608 indices across all instances. Nonuniform node transforms and shear are supported with inverse-transpose normals, but reflected, singular or ill-conditioned transforms, external URIs, animations, skins, extensions and many other glTF features are rejected. Flattened nodes cannot be animated independently. The bundled Duck model is supported; the bundled CarConcept model is not. Decoded package assets are capped at 16 MiB. Up to four gameplay plugins may run; GPU model resources are limited to 16 MiB and 32 model/texture combinations per plugin, and 64 MiB and 128 combinations globally. Resource creation may fail on a constrained device.

At startup enabled plugins can start before the city is active. Check `world_available()` before querying the world, including in `on_start`; an unavailable query stops the plugin and must be retried from Settings. There is not yet a city-availability event.
