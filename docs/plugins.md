# Plugins

This guide targets the **SciFi Shell v0.9.27 Windows x64 installer**. Other released or unreleased builds may behave differently. The experimental gameplay API can change; there is no stability or compatibility guarantee.

## v0.9.27 changelog

Scripted player/camera control now permits **1000 units/second**, up from 25, while retaining
0.25-unit collision substeps, a 0.1-second host tick cap, 4-radian/second angular limits and
the existing finite-pose, world, permission, focus and explicit-activation checks.
This is not a native vehicle-speed change. Scripts hardcoding the old speed must update their
own motion/chase-camera pacing; upgrading the host alone does not speed them up.

The [input contract](#controller-input) now includes Left Shift `boost`, Xbox Start acquisition
and Start/Back/B release with exclusive controller routing, held-state cleanup/resynchronization,
and active-owner-only `mouse_yaw` / `mouse_pitch` relative angular impulses. Mouse impulses are
consumed once, not multiplied by dt or treated as held axes. No new manifest permission or
API-version value is required; the older v0.9.26 host does not supply these new controls.

### TrackFly 2.2.1 example (v0.9.27)

Download [TrackFly2-2.2.1.zip](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.27/TrackFly2-2.2.1.zip)
from the release, separately from the installer. This is the exact reviewed package by
**Davidalanfisk**, supplied by its author and published with their approval; its asset bytes
and authorship are preserved. No additional asset licence or public-domain status is asserted.
It is optional, not bundled/enabled by the installer, and does not replace existing examples.
The manifest's review-host wording refers to the controls now released in **v0.9.27**.

Use **Settings > Plugins > Install Package**, select the ZIP without extracting it, then select
**TRACKFLY 2** twice within 30 seconds to review and grant the requested permissions.
If the same ID is already installed, back up your profile first and use the normal disable/remove
flow before installing the new package; import does not overwrite existing packages, and removal
can remove saved data. The ID remains `com.davidalanfisk.trackfly2`; score-key naming is unchanged.
Wait for its city survey, approach the launch pad within four units, and press **G / Xbox Start**.
Keep the world focused with panels closed. **Escape/G/Start/B/View-Back** releases flight.

| Action | Keyboard / mouse | Xbox |
|---|---|---|
| Yaw | Mouse left/right; A/D adds yaw | Left stick X |
| Pitch | Mouse up/down, following shell sensitivity/invert-Y | Right stick Y |
| Throttle / brake-reverse | W/S | Left stick Y or RT/LT |
| Strafe | No separate key | Right stick X or D-pad left/right |
| Climb / descend | Space / Left Ctrl | Y/X or D-pad up/down |
| Boost | Hold Left Shift or left mouse | Hold A or RB |
| Full brake (overrides boost/throttle) | S brakes/reverses | Hold LB |

Boost sources are independent; releasing one does not cancel another held source.
Boost can launch from rest, with about 3.6 seconds of energy and 7.1 seconds to recharge.
The cruise/boost targets are 300/800 units/second, with smoothed acceleration and a combined
800-unit velocity ceiling. The package paces camera corrections below 900 units/second and
combined rotation below 3.8 radians/second; excess mouse motion is discarded, not backlogged.
Only plugin-generated fake ground shadows were removed, not global renderer shadows.

The in-flight HUD starts with **actual camera/player speed** in shell units/second (not km/h),
measured from the last accepted view-position displacement and callback dt, then compact course/time
guidance. It updates about four times/second, resets on acquisition and reads zero for a blocked
camera; craft speed can differ because of camera lag and collision.
The plugin caps physics steps at 66 ms, so very low frame rates slow flight rather than enlarging
steps. Host collision is still player-sized city collision, not full swept vehicle-body physics;
the plugin also performs its own bounded craft sweep. City bounds and geometry can constrain travel.
This is not an unlimited-world, FPS, physical-controller or VR-flight certification.

## v0.9.25 changelog

Default Downtown buildings use the standard city door face/trim and a north-facing door at
`(plot.x,0,plot.z-6.1)` (2.72 units wide, 3.4 high). The default-building exit remains
`(plot.x,1.7,plot.z-12)`. Optional custom exteriors retain the separate host frame at `plot.z-9`;
their model schema and validation limits are unchanged.

A separate Settings quarter north of Computer is host-owned, not a plugin privilege. Its buildings
are generated from the existing Settings taxonomy and use the exact menu callbacks, values,
slider limits and confirmations. No package can replace these rooms, disable a settings group or
acquire new filesystem/rendering/OS permissions through the district. Existing city collision and
track-clearance queries account for both districts.

Settings buildings and galleries use the enlarged scale (64-unit plot pitch), while Downtown
keeps its existing scale and saved placements. Settings rooms use dark, color-correct live consoles,
eye-height controls and the existing permission/destructive confirmation surfaces.
All package manifest fields, `downtown_model` limits and gameplay API permissions remain unchanged.

## Neon Circuit Arcade (v0.9.26)

**NEON CIRCUIT ARCADE** (`org.scifishell.arcade`) is a bundled
precompiled C++ native plugin, not a Lua package.
Visit through **Settings > Downtown > Visit Neon Circuit Arcade**, or its cyan/magenta-trimmed
building south of Computer. Twelve cabinets occupy three rows. The back half is covered by a
furnished office/bedroom mezzanine; the front has a double-height ceiling. The stairs start at
the right front corner, turn left at the landing, and open into the upstairs railing.
Walk into stairs normally; jumping/flying is unnecessary. The entry wall has Exit, On / Off
and Rearrange controls. Escape/Back exits the room when no cabinet is selected.
The upper stair ends at a level landing with a guarded opening onto the full-width loft.
Disabled/failed plugins retain the ordinary host management room and cannot be uninstalled.

Aim at a cabinet screen and click, press Enter / gamepad A, or use the VR controller ray and
trigger. Selection opens that cabinet's menu and seats the desktop view at the screen.
Choose Play, Game, Difficulty (1-3), Invert Y or Browse for NES ROM. Use arrows, mouse wheel,
D-pad, left stick or VR left stick to select a row; left/right changes its value, and
Enter/click/A/VR trigger activates it. Configuration is independent for every cabinet.
Escape/gamepad B/Back/VR Back stops the game and returns to the cabinet menu; a second
press releases the cabinet and restores the previous viewpoint. The shell's other hotkeys,
camera movement and controller shortcuts are suppressed while a cabinet owns input.
F12 and the app-close action remain available.

| Original game | Play |
|---|---|
| Pulse Rally | Move paddle with W/S, arrows, mouse up/down, gamepad left stick or VR left stick. Outlast the opposite paddle. |
| Orbit Breaker | A/D or left/right stick rotates; W or stick up thrusts; Space/mouse button/gamepad A or right trigger/VR trigger fires. Clear the drifting vector rocks. |
| Sky Rescue | WASD/arrows or left stick moves. Fire as above; Shift/gamepad X/VR left grip rescues when close to the ground. |
| River Runner | Tap/release a direction (arrows/WASD/D-pad/stick) to hop. Avoid traffic, stay on river platforms and reach the far bank. |

These are independently authored mechanics/graphics, not ports or replicas of commercial games.
Inactive cabinets run only a procedural attract painter at 8 Hz: there are no background game
sessions, emulator pages, ROM loads or audio voices. One cabinet may run at a time; original
games use a fixed 60 Hz simulation, and NES uses one isolated CEF surface. Leaving a cabinet,
focus loss, leaving/rebuilding the district, disabling the plugin or shutting down stops its
session and owned sounds. Focus loss ends rather than suspends a run. Native effects and NES
audio follow the shell SFX volume.

Preferences and original-game best scores reuse the bounded atomic storage format
(64 entries / 64 KiB maximum) in `%LOCALAPPDATA%\SciFiShell\arcade\builtin.arcade.store`
on Windows. Scores are saved at game over and when a run ends. Imported ROMs are copied to fixed
`cabinet-0.nes` through `cabinet-11.nes` names in that same private profile directory, never
into the application or installation assets. At most twelve 1 MiB imports can be retained.
Corrupt preferences are preserved and reported as a failed plugin start; failed writes are
shown on the cabinet, not reported as saved. Back up or repair a corrupt file before retrying.
Native code is trusted, not an OS sandbox, and multiple writers to one profile are unsupported.

### NES import and limits

The Windows CEF build bundles **JSNES 2.1.0**, Apache-2.0, with its licence in
`assets\arcade\JSNES-LICENSE.txt`. No commercial ROMs, BIOS files, artwork or music are included
or fetched. Select an idle cabinet, choose **Browse for NES ROM**, select a local `.nes` file
in the Windows file picker, and then choose **Play**. You can also drop exactly one local
`.nes` file onto the shell while the cabinet menu is open. Cancelling leaves the cabinet unchanged.
The picker starts at the Windows system drive root.
Only use material you are entitled to run. The file is bounded to 1 MiB, must have an exact
iNES or supported NES 2.0 header and payload, mapper 0, 16/32 KiB PRG and 0/8 KiB CHR.
NES 2.0 is limited to NTSC/multiregion NROM, no PRG RAM, standard controllers, no extra ROMs
or submapper, and 8 KiB volatile CHR RAM when no CHR ROM is present. Its compatible header is
adapted only in memory for JSNES; the original and imported file remain unchanged. Trainers,
battery saves, PAL/Dendy-only NES 2.0, other mappers, archives, links and mismatched/trailing data are rejected
with a visible error. This deliberately does **not** claim all NES games work. Non-CEF builds
report that NES requires CEF while retaining the four native games.

NES directions use WASD/arrows, D-pad or left sticks. A is Space/mouse/gamepad A/right
trigger/VR trigger; B is Shift/gamepad X/VR left grip. Start is Enter/gamepad Start/VR menu,
and Select is Tab/gamepad LB/VR right grip. Back always releases emulator input instead of
being sent to the game. There is no save-state, battery-save, ROM browser, arbitrary URL,
download button or JavaScript filesystem bridge. Importing requires an explicit selection or drop,
not a directory scan. The native picker is available on Windows; other builds use drag and drop.
The Windows picker appears on the desktop monitor, including when using VR.

The host verifies the pinned engine SHA-256 before execution and passes only validated bytes
as base64 to an ephemeral `about:blank` surface. Its separate request context has no disk cache;
navigation, subresources, network requests and popups are denied. Frame/startup errors are
reported on the cabinet. Closing mutes/hides and closes that page; the next cabinet never
inherits its input or audio. These restrictions are in-process safeguards, not an OS sandbox.

**Not implemented:** MAME-compatible emulation/engine fetching, other console engines,
save states, two-player games, arbitrary control remapping, and pinball. MAME as a whole
is GPL-2.0-or-later with mixed component licences; a pinned compatible browser distribution,
its source/compliance materials, driver/ROM requirements and resource limits still need
validation before integration. No placeholder fetch button pretends that this work is done.
Pinball remains on the [wishlist](todo.md#arcade-wishlist).
Physical headset/controller comfort, audio listening and broad ROM compatibility need
manual review; simulated input checks are not hardware certification.

## v0.9.24 changelog

Downtown now fits its occupied plots instead of reserving all 128 possible plots. The floor, outline,
label, storage packing, bus avoidance and track clearance use the same dynamic bounds.
The connector road to Computer has been removed. The bundled Car and Track and Parks and Toys
now have default buildings, host management rooms and persistent rearrangement alongside packages.
Their existing on/off controls work even when disabled; built-in plugins cannot be uninstalled.
Existing package placements and optional `downtown_model` schema remain compatible.

## v0.9.23 changelog

Downtown gives every installed package a stable physical building south of Computer, surrounded by
a theme-colored glass-region outline matching the district's full reserved bounds. Host-owned entrances,
management rooms, safe evacuation, persisted plot moves/swaps, and the optional gameplay manifest
field `downtown_model` are new in this release. Existing packages need no changes and receive a default
building; disabled and failed plugins keep their entrances. Existing source/archive/runtime limits and
digest-bound consent and removal-confirmation checks are retained. The model field is optional but requires v0.9.23.

## Downtown district (v0.9.24)

Downtown is a host-owned district **south of Computer** (+Z in city coordinates). Every installed
theme, message-script or gameplay package gets a building, including disabled or failed packages.
The bundled native **Car and Track** (`org.scifishell.driving`) and **Parks and Toys**
(`org.scifishell.parks`) also receive stable plots and default buildings. They use the same Visit,
Arrange and On / Off controls; their removal card is disabled and labeled **BUILT-IN - CANNOT DELETE**.
Buildings represent
installed packages, not running Lua scene objects; disabling a plugin does not remove its management
entrance. Uninstalling removes it.

Walk south from Computer or use **Settings > Downtown > Visit [name]**. Aim at the standard door
(or the separate illuminated host frame for a custom exterior) from within 12 units and click/select.
Every entrance leads to a host-managed, empty
room with package ID, name, version, description and status, an **On / Off** control, **Delete Plugin**,
**Rearrange**, and an exit. Escape/Back also exits. No package filesystem is exposed in this room.
The host retains these controls even if plugin code fails.

Toggling or deleting from inside first moves you safely outside and opens Plugins. Enabling gameplay
uses the same digest-bound permission notice as Settings: select the plugin again within 30 seconds
to consent. Dismiss the panel to cancel without enabling. Delete arms the ordinary Remove confirmation:
select **Remove [name]** again within 15 seconds to confirm, or let that deadline expire to cancel. A failed operation
reports its error and leaves the remaining package/building accessible; removal may already have disabled
the package or revoked consent. Re-enable through the normal permission flow if removal failed.

To rearrange, select **Arrange [name]** in Downtown (or the room's Rearrange card), choose
**Previous Plot / Next Plot**, then **Move / Swap**. The target card names the current occupant;
occupied destinations swap both buildings, empty destinations move only the selected building.
Rearrangement returns a city visitor to the district gateway before changing geometry.
Numbered plots use a four-column, 32-unit-pitch layout starting at `(x=-48,z=128)`.
There are 128 selectable plots for the existing 64-package installation limit plus the two bundled
plugins. The district is the bounding rectangle of occupied plot centers with a 20-unit margin on
each side; a completely empty district uses a minimal 32x40 footprint south of Computer.
The colored floor/outline and label, drive packing, bus avoidance and native/scripted track clearance
all use these same bounds. Installing or removing a plugin grows/shrinks the region without renumbering
existing buildings. Intentionally selecting a distant plot includes the gaps in the outline.
Resizing repacks storage regions and regenerates bus/track geometry, first releasing player control,
leaving the car/data-lane ride and returning city visitors to the safe gateway. Nearby storage regions
may therefore move; occupied plugin plots do not. The track may pass overhead with clearance.
There is currently no connector road between Computer and Downtown.

Arrangements are saved atomically to `%LOCALAPPDATA%\SciFiShell\downtown.ini` on Windows
(alongside `plugins.ini` on other platforms), keyed by package ID. New packages take the lowest free
plot; uninstall releases only its plot. Disable/re-enable, failures, city rebuilds and restarts retain
placements. Corrupt preferences are preserved and reported; buildings use a deterministic read-only
recovery layout until the preferences are repaired. Write failures are reported, not presented as saved
moves. Lifecycle changes still reconcile the live city. As with other plugin preferences, concurrent
writers from multiple shell instances are not supported.

### Optional exterior model

Gameplay ZIP manifests may add one optional string, without changing `api_version`:

```json
"downtown_model": "assets/garage.glb"
```

Include that exact file in the existing package asset table. It must pass the normal restricted GLB
decoder (including rejection of external resources) and package/archive/decoded limits, **plus**
8,192 vertices, 8,192 triangles and an embedded texture no larger than 256x256.
The model's baked scene bounds are uniformly scaled to fit a 12x12x12 box, horizontally centered on
the plot and grounded at y=0. Existing scene-node transforms and embedded material/texture are retained.
The host uses a conservative solid box for collision, not triangle or doorway collision.
The entrance is always a separate host frame at `(plot.x,0,plot.z-9)`; exit is at
`(plot.x,1.7,plot.z-12)`. Do not depend on a model doorway, interior geometry, Lua startup, or enabled
state for entry. Custom exteriors currently use the same host room as default buildings.

The exterior is uploaded through bounded, host-owned GPU resources during registration, independently
of script consent or execution. It stays visible when the plugin is off/failed. Renderer budget/allocation
failure is logged and shown on the package's Arrange card and uses the default building rather than
losing management access. Uninstall releases its exterior GPU resources. Theme and message-script
packages have no asset table and always use the procedural host-default building. Missing or invalid
declared models reject installation; omitting the field preserves all existing package behavior.
No new Lua functions, arbitrary filesystem loading, remote downloads, custom interiors or marketplace
are introduced. This manifest field is not accepted by v0.9.22 and earlier installers.

## v0.9.22 changelog

Plugin Lua source, gameplay/script `plugin.json`, and theme `.sfplugin` files now accept **10 MiB (10,485,760 bytes) each**, inclusively, instead of 16 KiB. Message-script ZIPs now allow **20 MiB + 64 KiB (21,037,056 bytes)** on disk so both files fit even without compression. The shell's read-only code viewer also reads up to 10 MiB instead of 2 MiB; its rendered-line cap remains 4,000 characters. Gameplay ZIP disk/expanded totals remain 1 GiB and encoded binary assets remain 100 MiB.

Lua compilation now has a **128 MiB allocation ceiling** per state. After compilation, compiler scratch is collected and the loaded code/constants establish a fixed baseline; the chunk and callbacks share at most **1 MiB of additional Lua allocations above that baseline**, never exceeding 128 MiB total. The 100,000-instruction budget for the chunk and each callback is unchanged. A source within the file cap can still fail explicitly if its compiled representation exceeds the compilation ceiling, Lua syntax limits, or execution budgets. Compilation and package validation are synchronous and can pause the shell.

Manifest schemas and field-length limits are unchanged: the larger file allowance permits formatting, not arbitrary extra metadata or 10 MiB field values. Names remain at most 40 printable ASCII characters, authors 80, descriptions 160, IDs 80, and versions 24. Decoded/GPU memory, archive-entry/metadata, storage, messaging, voice and other behavior/resource limits are unchanged. Installer staging excludes test executables and test-only native fixtures; Windows shared-SDL builds copy the required SDL2 runtime alongside the shell.

## Alpha gameplay API additions

The following APIs are available in v0.9.21, **not in the v0.9.20 installer**. They remain experimental; share the required shell version with plugin users.

### Gameplay package limits

Gameplay ZIP files may be at most **1 GiB (1,073,741,824 bytes)** on disk and their entries may total at most **1 GiB uncompressed**. Each uncompressed binary asset may be at most **100 MiB (104,857,600 bytes)**; `plugin.json` and `scripts/main.lua` may each be at most **10 MiB (10,485,760 bytes)**. The 34-entry limit remains. Installed ZIP files share a 4 GiB disk quota when installing either a gameplay or script package; do not install concurrently from multiple shell processes. ZIP metadata is also bounded. Assets are read one at a time rather than unpacked to disk, but loading a large asset still temporarily uses memory proportional to its encoded size. Installation, approval and startup read and validate packages synchronously, so large files can pause the shell. Decoded assets share a separate 16 MiB per-package budget, and each format has additional geometry, dimension, or duration limits. GLB parsing has an 8 MiB allocator budget, separate from the encoded-file cap. The internal in-memory archive interface remains capped at 8 MiB on disk and expanded, so it cannot load a 10 MiB source; Settings installation, consent and startup use the file-backed path and accept the full text limit.

The allowed asset extensions now include `.wav` and `.ogg` as well as `.png` and `.glb`. Nested paths such as `assets/ships/fighter.glb` are supported: the portion after `assets/` is at most 80 characters, lowercase ASCII letters, digits, `_`, `-`, `.`, and `/`. Components cannot be empty or begin with `.`, and `..` is forbidden. Include files only: directory entries, links, duplicate paths, backslashes, and traversal are rejected. Existing consent records remain valid for unchanged packages; newer consent records store a SHA-256 digest and size instead of another copy of the ZIP. Packages changed after approval require new consent. These checks do not protect against a hostile process running as the same user.

These additions are a framework, not the HARVI, FlyTrack, arcade, apartments, garages, ownership or other game content. Networking, multiplayer, shared worlds, voice chat, server authority and remote asset downloads are not provided. The existing text HUD remains available; this build does not add a widget toolkit, a dedicated plugin debug-log API, or a Lua metadata-query API.

### Persistent storage

Add `"storage"` to a gameplay manifest's `permissions`; users must explicitly grant it. It exposes `storage_get(key)` (value or `nil` if absent), `storage_exists(key)` (boolean), `storage_set(key,value)`, and `storage_delete(key)` (deleting a missing key succeeds). Values are strings, finite numbers or booleans. Keys are 1-64 printable ASCII characters; strings may contain arbitrary bytes and are limited to 1,024 bytes. There are at most 64 keys and 65,536 bytes of serialized data per plugin. Errors, invalid arguments and quota violations stop the callback/plugin rather than silently discarding writes.

Writes are synchronous and persisted immediately: at most 8 may burst, replenishing 4 per second, with at most 64 storage calls per callback. Avoid writes every frame. Storage survives disable, restart, package replacement, and uninstall; it is not automatically deleted. On Windows files live in `%LOCALAPPDATA%\SciFiShell\plugin-storage`, named by plugin ID. This is not secret storage, encrypted storage, an OS sandbox, or a coordination mechanism for simultaneous shell processes; avoid sensitive data and concurrent writers. Corrupt or unreadable files stop startup and are not overwritten. Other plugins cannot address the store using this API, but native code or other processes running as the same user have normal filesystem access.

The [storage demo ZIP](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.21/storage-demo.zip) increments a stored count on each start and shows it on the HUD. Its source is in [`examples/plugins/storage-demo`](../examples/plugins/storage-demo). From the repository root, package it without directory entries:

```powershell
python -c "import zipfile,pathlib; p=pathlib.Path('examples/plugins/storage-demo'); z=zipfile.ZipFile('storage-demo.zip','w',zipfile.ZIP_DEFLATED); [z.write(p/n,n) for n in ('plugin.json','scripts/main.lua')]; z.close()"
```

### Controller input

Gameplay manifests may request `"controller"` together with `"input"`. Consent lists controller input separately. `on_input(control,value)` receives `gamepad_left_x`, `gamepad_left_y`, `gamepad_right_x`, `gamepad_right_y`, `left_trigger`, `right_trigger`, `button_a`, `button_b`, `button_x`, `button_y`, `button_start`, `button_back`, `dpad_up`, `dpad_down`, `dpad_left`, and `dpad_right`. For compatibility each change is also delivered first under its legacy `pad_lx`, `pad_ly`, `pad_rx`, `pad_ry`, `pad_lt`, `pad_rt`, `pad_a`, `pad_b`, `pad_x`, `pad_y`, `pad_start`, `pad_back`, `pad_up`, `pad_down`, `pad_left`, or `pad_right` name respectively. Handle only one naming family to avoid acting twice. `pad_connected`, `pad_lb`, `pad_rb`, `pad_ls`, and `pad_rs` have no new alias. Sticks are dead-zoned in `[-1,1]`, Y is positive up, triggers are in `[0,1]`, and buttons and availability use 0/1. Only changes are delivered; leaving the available interaction context or disconnecting resets held values to zero. On Windows this uses the first XInput controller; elsewhere it uses the first SDL game controller. VR controller events are not included.

Controller input is shared with the shell unless the plugin holds the existing `control` lease. In shared mode shell shortcuts may act on a button first or open a panel, preventing that press from reaching the plugin; for example X opens the keyboard and Y opens Start. A nearby offer from a controller-enabled plugin displays **G / Start: Enter**. A fresh Xbox **Start** press activates it using the same focus, consent, proximity and world-context gates as G; connecting a pad or regaining focus with Start held does not activate it. While it owns control, **Start**, **Back/View** or **B** releases it, as do Escape/G and controller disconnection. These three buttons are reserved by the host while owned, not delivered as script actions. Other pad axes/buttons go to the plugin, without shell keyboard/ring/window-switcher shortcuts. After exit or context loss, held pad controls must return to neutral before controlling the shell again. Input remains unavailable when interacting with panels, typing, driving natively, or in VR.

With `input`, Left Shift also produces `on_input("boost",1)` when held and
`on_input("boost",0)` on release or unavailable interaction context. It is independent of
controller buttons; combine it with your chosen pad boost action in the script.
On activation the host resets the keyboard/controller change snapshots so currently held values are
resent after `on_input("control",1)`. Before `control=0`, previously delivered axes, buttons, click
and boost receive zero/release values. Scripts should still clear their local input state on release
or disconnect and must not propose poses before activation. In shared mode the existing input
permission still allows remote controls when no lease is held.

With `input` and an **active control lease**, captured flat-screen mouse movement produces
`on_input("mouse_yaw",delta)` and `on_input("mouse_pitch",delta)` before `on_tick`.
These are relative **radian impulses**, not held axes: positive yaw turns right (subtract from
camera yaw), positive pitch looks down (subtract from pitch). Shell look sensitivity and invert-Y
are already applied at 0.0025 radians/pixel before sensitivity scaling. Accumulate and consume
them once per tick; **do not multiply by dt**. Identical nonzero deltas repeat on consecutive
frames; zero deltas generate no callback. Each axis is clamped to `4*dt` radians with no backlog
and at most two mouse callbacks per frame; scripts must still clamp their combined
mouse/keyboard/controller rotation to the pose limits. Nonfinite deltas are discarded.
No mouse input is delivered to unowned plugins or while UI, focus, capture or VR makes world
input unavailable. Acquisition and capture/context recovery frames discard relative motion;
it is never resent with held-state snapshots. Clear pending impulses on `control=0`.
The native camera does not also rotate while a plugin owns control.

### Packaged samples

With the required `"scene"` and existing `"sound"` permissions, package `.wav` or `.ogg` files under `assets/`. `asset_sound("sounds/tone.ogg")` returns a package-local sound asset handle; `sound_play_asset(sound)` starts it at full volume and returns a distinct voice handle. Use `sound_loop(voice)` to enable looping and `sound_stop(voice)` to stop it. A voice that has already finished cannot be made to loop. Alternatively, `sound_sample("assets/sounds/tone.ogg", 0.5, true)` starts the named sample with a 0-1 volume and a required loop boolean, returning a voice handle. Voices stop automatically when the plugin stops or fails. Calls enqueue actions for the next host frame; the existing sound event rate limit also applies to sample actions. Calling `sound_loop` on a newly queued voice combines it with the start action.

Samples must be mono or stereo, 8-48 kHz, and at most 30 seconds; WAV must be uncompressed 16-bit PCM, and OGG must contain Vorbis audio. Audio is mixed to mono and resampled for the effects device. There are at most four sample voices per plugin and 16 globally, with at most eight queued sample actions per frame. Decoded samples share the 16 MiB per-package decoded-asset budget with textures and models. Invalid samples and exceeded limits fail explicitly.

The [audio demo ZIP](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.21/audio-demo.zip) starts an original looping tone and relies on automatic cleanup. Its source is in [`examples/plugins/audio-demo`](../examples/plugins/audio-demo). From the repository root:

```powershell
python -c "import zipfile,pathlib; p=pathlib.Path('examples/plugins/audio-demo'); z=zipfile.ZipFile('audio-demo.zip','w',zipfile.ZIP_DEFLATED); [z.write(p/n,n) for n in ('plugin.json','scripts/main.lua','assets/tone.ogg')]; z.close()"
```

### Time and randomness

`time_now()` returns monotonic seconds from an unspecified origin; `time_since_start()` and its alias `time_elapsed()` return monotonic seconds since this plugin session started. `time_frame()` is the current host frame number, shared by plugins; `time_delta()` is the latest clamped tick delta in seconds, initially zero. `time_unix()` returns whole seconds since the Unix epoch; it uses the system clock and can jump if the clock changes. No additional permission is needed.

`random()` returns a number in `[0,1)` and `random_range(min,max)` returns a finite real between finite ordered bounds. `random_seed(number)` resets this plugin session's generator to a deterministic sequence; the seed must be finite. The Lua `math.random` and `math.randomseed` generator remains available separately. Neither generator is suitable for security or secrets.

### Inter-plugin messages

Request `"messaging"` to send local events with `send_event(eventName, data)` and register a handler with `on_event(eventName, callback)`, whose callback receives `data`. Payloads are finite numbers, booleans, or strings of at most 256 bytes. Event names are 1-32 lowercase ASCII letters, digits, `.`, `_` or `-`; up to 32 names can be registered, and registering a name again replaces its callback. The existing `message_emit(topic, payload)` remains string-only, and `on_message(sender, topic, payload)` receives all events, including typed ones. If both handlers are present, `on_message` runs first, followed by the matching `on_event` callback, each with a separate instruction budget. Messages are broadcast to other currently running plugins that also have this permission, not to the sender. They are delivered on a later frame, with the sender's plugin ID supplied to `on_message` by the host. A message from a plugin that stops before delivery is discarded. Delivery is not durable, private, or networked; do not send secrets or rely on messages for authority.

```lua
-- Sender (messaging permission): send_event("race_complete", elapsed_seconds)
-- Receiver also requests hud.
on_event("race_complete", function(seconds)
    hud_set("RACE COMPLETE")
end)
return {}
```

Each plugin can queue at most eight outgoing messages before the host drains them, with a burst of four and four messages per second thereafter. The shared pending queue is capped at 64; exceeding a limit stops the sending plugin. Callback errors stop the receiving plugin.

### Asset discovery

With the required `"scene"` permission, `asset_exists(path)` checks a full packaged path, and `asset_list()` returns an alphabetically sorted array of all packaged asset paths. `asset_find(pattern)` returns sorted full paths; patterns may start with `assets/` or be relative to it. `*` matches zero or more characters within one path component and `?` matches one; neither crosses `/`. For example, `asset_find("ships/*.glb")` finds `assets/ships/fighter.glb` but not `assets/ships/sub/fighter.glb`. Patterns without wildcards retain the earlier prefix-matching behavior; an omitted or empty pattern lists all assets. These functions do not access arbitrary files or directories. Use `asset_model`, `asset_texture`, `asset_sound`, or `sound_sample` to use the returned assets.

### Procedural meshes

`mesh_create()` creates a mutable builder. `mesh_vertex(mesh,x,y,z)` appends a vertex and returns its one-based index. `mesh_normal(mesh,x,y,z)` and `mesh_uv(mesh,u,v)` set attributes on the latest vertex; defaults are `(0,0,1)` and `(0,0)`. `mesh_triangle(mesh,a,b,c)` appends a triangle of existing one-based indices. `mesh_upload(mesh)` finalizes the nonempty builder and returns the mesh handle; after upload it cannot be edited or uploaded again. `scene_mesh(mesh)` creates a white, untextured unit-sized instance at the origin. Actual GPU upload occurs when the renderer needs it.

Alternatively, `mesh_create(vertices, indices)` creates an immutable mesh directly. `vertices` is a flat array of eight numbers per vertex: position x/y/z, normal x/y/z, and UV u/v. `indices` is a flat array of one-based vertex indices, three per triangle. All values must be finite and within -10,000 to 10,000. Each plugin may create at most eight builders or meshes in total, each with at most 1,024 vertices and 6,144 indices.

Use `scene_mesh(handle, texture, x, y, z, width, height, depth, red, green, blue)` to instantiate it; use texture `0` for the default or a handle from `asset_texture`. It returns a scene object handle usable with the existing scene transform and removal functions. The mesh is uploaded when needed and remains immutable until the plugin stops; existing per-owner GPU limits apply. GPU and mesh resources are released on stop or failure.

## Start here

You do not need to build the shell to make a plugin: install [v0.9.27](https://github.com/Davidalanfisk/SciFiShell/releases/tag/v0.9.27), a text editor, and optionally Python 3 for the ZIP commands below. The installer is unsigned. Launch the **installed** shortcut rather than an older source build or taskbar pin.

| What you want | Package | Start with |
|---|---|---|
| Change two shell theme colours | One JSON `.sfplugin` file, no code | [Theme packages](#theme-packages) |
| Return a short message on demand | ZIP with a Lua message script; no permissions | [Clock example](#lua-plugin-authoring-specification) |
| Add experimental in-world objects and interactions | ZIP with Lua and optional GLB/PNG/WAV/OGG assets; explicit permissions and consent | [First gameplay plugin](#first-gameplay-plugin), then the [API reference](#experimental-gameplay-scripts) |

1. Open the Start menu with **F2** or middle-click, then **Settings > Plugins > Install Package**. Select the downloaded or locally built `.sfplugin` or `.zip`.
2. Select the installed plugin's name to enable it. Gameplay plugins show their ID and requested permissions: inspect these and select again within 30 seconds to consent. New packages are disabled until enabled. A retained preference for the same ID can change the initial state.
3. For the [clock ZIP](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.19/scifishell-clock-example.zip), select **Run CLOCK** after enabling it. For the [scripted car and track](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.20/scripted-car-track.zip) or [scripted parks and toys](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.20/scripted-parks-toys.zip), return to the city to try them. Disable the corresponding native features separately if they overlap.
4. To iterate, change your source, rebuild the ZIP, select **Remove** twice within 15 seconds to uninstall the old package, then install and enable the new ZIP. Installation does not overwrite existing packages; changed gameplay ZIP contents need fresh consent. Keep your source outside the installed plugin directory.

**Trust and privacy:** permissions restrict which host functions a gameplay script receives; they are not an OS security boundary. Lua and asset decoding execute in the shell process, and bugs in those components can compromise it. Only install and approve packages from people you trust, and review requested permissions and source. Gameplay plugins may request the bounded persistent storage API described above; it is not secret storage. There is no third-party native-code installation, general filesystem, network, OS or module-loader API. Bundled native modules have the shell's full privileges.

## First gameplay plugin

This minimal example creates one box four units in front of the current view. Make a folder named `hello-plugin` with `plugin.json` and `scripts\main.lua` inside it. Save both as plain ASCII or UTF-8 **without a BOM**. Use a unique ID you control when sharing; do not use the reserved `org.scifishell.` prefix.

`plugin.json`:

```json
{
  "id": "com.example.hello",
  "name": "HELLO BOX",
  "version": "1.0.0",
  "author": "Example",
  "description": "Place a box in front of the view",
  "entry": "scripts/main.lua",
  "type": "gameplay",
  "platforms": ["any"],
  "permissions": ["scene", "view"],
  "api_version": "1"
}
```

`scripts\main.lua`:

```lua
local box

return {
    on_tick = function(dt)
        if not box then
            local x, y, z, dx, dy, dz = view_pose()
            if x then
                box = scene_box(x + dx * 4, y + dy * 4, z + dz * 4,
                                1, 1, 1, 0.2, 0.8, 1)
            end
        end
    end
}
```

From the directory **containing** `hello-plugin`, create a ZIP with Python 3 on Windows:

```powershell
python -c "import zipfile,pathlib; p=pathlib.Path('hello-plugin'); z=zipfile.ZipFile('hello-plugin.zip','w',zipfile.ZIP_DEFLATED); [z.write(p / n,n) for n in ('plugin.json','scripts/main.lua')]; z.close()"
```

Install `hello-plugin.zip`, consent to `scene` and `view`, and close Settings to look for the box. It is placed once, where the view was when the plugin started; it does not follow you and can be obscured by city geometry. Disable and re-enable to place it again. The script waits if the view is not yet available. Try changing the size or RGB values, then use the remove/reinstall workflow above. Use a new ID if the example ID is already installed.

The [gameplay duck source](../examples/plugins/gameplay-duck) adds animation and a packaged model; the [other examples](#experimental-gameplay-scripts) demonstrate interaction and world queries. For debugging, inspect Settings notices and the **PLUGIN STORAGE** / **PACKAGES** cards; a callback error stops the plugin. Ensure the ZIP has the exact root paths shown, the manifest has no extra fields, and requested functions have their corresponding permission.

## Bundled gameplay plugins

Settings > Plugins lists the built-in **Car and Track** and **Parks and Toys** plugins. Both are enabled by default; select either to disable or re-enable it immediately. Disabling driving exits the car and removes the track; disabling parks removes the parks, ducks, stands and active snacks. Preferences are stored separately from other settings in `%LOCALAPPDATA%\SciFiShell\plugins.ini` on Windows (`~/.config/scifishell/plugins.ini` elsewhere). If the preference file cannot be read or written, the page reports an error and does not overwrite the file.

The gameplay plugins use two trusted native modules built alongside the application. The driving module contains track generation, sampling and rendering, driving simulation and car rendering; the parks module contains scenery, ducks and toy simulation and rendering. Host-side integration and some interaction and scoring logic remain in the executable. The modules must be present next to the executable, are loaded only when their features are enabled, and are unloaded on disable. They run with the application's full privileges, **not in a sandbox**. They are bundled modules, not independently installable third-party packages; Settings does not install native code. Settings > Plugins also installs **data-only theme packages**: choose a `.sfplugin` file, then enable it on the Plugins page and cycle to it using Settings > Shell > Theme. Theme packages have no executable code, external files, network or filesystem permissions. Installed packages are discovered from the per-user `SciFiShell/plugins` directory at startup and start disabled unless previously enabled.

## Theme packages

A `.sfplugin` is one UTF-8/ASCII JSON file of at most 10 MiB, not a ZIP. All fields below are required; unknown fields, duplicate keys, permissions other than an empty array, and types other than `theme` are rejected. Strings use printable ASCII; `\"`, `\\` and `\/` are the supported escapes. IDs are lowercase reverse-domain-style letters, digits, dots and hyphens; `org.scifishell.*` is reserved. Colours are `#RRGGBB`.

An installable example is [`examples/plugins/twilight.sfplugin`](../examples/plugins/twilight.sfplugin).

```json
{"id":"com.example.twilight","name":"TWILIGHT","version":"1.0.0","author":"Example","description":"A violet theme","type":"theme","platforms":["any"],"permissions":[],"accent":"#aa22ff","alt":"#12ccdd"}
```

The shared `id`, `name`, `version`, `author` and `description` rules are in the [message-script manifest table](#lua-plugin-authoring-specification). `platforms` must be exactly `["any"]`; `permissions` must be `[]`; `accent` and `alt` are the two RGB colours. Install the file, enable it, then cycle to the theme in **Settings > Shell > Theme**. Not every built-in panel uses theme colours.

Installed packages start disabled unless a retained preference says otherwise. Installation never overwrites an existing package. Built-in plugins cannot be uninstalled. ZIP themes, arbitrary ZIP layouts, automatic package updates and an app store are not implemented. Gameplay plugins can request the bounded persistent storage described above; message scripts cannot.

## Lua plugin authoring specification

[Download the example clock plugin ZIP](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.19/scifishell-clock-example.zip); its [source files](../examples/plugins/clock) are included in this repository. It contributes a **Run CLOCK** card which displays the current Unix time. This section describes the message-script type; the separate, experimental gameplay type is described below.

A script ZIP must have these exact, case-sensitive root paths:

```text
plugin.json
scripts/main.lua
```

An empty `scripts/` directory entry is optional; no other entries are allowed. ZIP the **contents** of the example directory, not the containing directory. The archive is limited to 20 MiB + 64 KiB (21,037,056 bytes); each uncompressed file is limited to 10 MiB, and the script must not be empty. Duplicate paths, absolute paths, traversal, backslashes, symlinks, encryption, unsupported ZIP entries and corrupt CRCs are rejected. Archive paths are read into memory, never extracted to the filesystem.

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

The chunk must return a **string of 1–160 printable ASCII characters**; it may be truncated visually to fit the panel. `unix_time()` is the only supplied host function and returns the current Unix timestamp as an integer. Lua language operators, local variables, conditionals, loops and functions work subject to the budget, but **no standard libraries or base functions** are opened: for example `print`, `tostring`, `string`, `math`, `io`, `os`, `require`, `load` and `debug` are unavailable. Each execution starts a fresh state, with no persistence between runs. It is limited to 100,000 Lua VM instructions and 1 MiB of additional Lua allocations above the compiled-code baseline, with the separate 128 MiB compilation/total ceiling described in the [changelog](#v0922-changelog).

To package the example on Windows with Python, run this from the repository root:

```powershell
python -c "import zipfile; z=zipfile.ZipFile('clock.zip','w',zipfile.ZIP_DEFLATED); z.write('examples/plugins/clock/plugin.json','plugin.json'); z.write('examples/plugins/clock/scripts/main.lua','scripts/main.lua'); z.close()"
```

To install, open **Settings > Plugins > Install Package** and select the ZIP. Installation validates and copies it without overwriting an existing package. The input filename may be arbitrary; the installed copy is named `<id>.zip`. At most 64 installed theme, message-script and gameplay package files are supported in total. Select the plugin name to enable it: enabling executes the script once as validation, and failure does not persist the enabled state. Select **Run** to execute it again and display its message; no script runs continuously. Select the name again to disable. Select **Remove** twice within 15 seconds to uninstall; the shell disables the plugin and removes its package. A retained disabled preference means reinstalling the same ID stays disabled.

On startup the shell discovers packages in `%LOCALAPPDATA%\SciFiShell\plugins` on Windows (`~/.config/scifishell/plugins` elsewhere); saved enablement is in the adjacent `plugins.ini`. New IDs default to disabled. An enabled script is validated again on startup; errors leave it marked **ERROR** rather than running. The **PLUGIN STORAGE** and **PACKAGES** cards show preference and discovery errors, and Settings notices show install, enable, run and removal errors. Check manifest fields, paths and budgets first if a package fails. A malformed preferences file is not overwritten.

Lua runs in-process, not in an OS sandbox, and interpreter vulnerabilities remain possible. No filesystem, network, OS, input, native code, module loader or persistent data API is exposed to message scripts.

## Experimental gameplay scripts

Gameplay scripts are a separate package type and are **not yet equivalent to Car and Track or Parks and Toys**. Those features remain trusted bundled native modules. Gameplay scripts can create boxes, models and bounded procedural meshes, update them over time, register click targets, and optionally receive input, query city collision and cell occupancy, play sounds, display a short HUD message, apply a bounded player push and offer explicitly activated player/camera control. Storage and local events are available with consent, but do not provide authoritative shared scores or reproduce bundled gameplay. The API and limits are preliminary. The v0.9.19 clock download does not contain this functionality.

Download [scripted-car-track.zip](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.20/scripted-car-track.zip) and [scripted-parks-toys.zip](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.20/scripted-parks-toys.zip), or build them from this checkout with `python examples/plugins/build-gameplay-examples.py <empty-output-directory>`. Install them through Settings, then explicitly enable and consent to each. The second ZIP combines the nearby park and snack examples below; it uses the snack score as its HUD message. These examples are simplified, not replacements for the bundled native plugins. Disable the native plugins separately if you want to avoid overlapping features. The builder refuses to overwrite an existing example ZIP.

Scripted click targets consume their clicks before native shell actions, including while a claimed click is held, so that click does not also open an object behind it. Picking uses bounding boxes and does not check city occlusion: an interactive object behind a wall can still receive a click. A miss remains available to the shell.

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

Gameplay ZIPs must contain `plugin.json`, `scripts/main.lua`, and optionally assets under `assets/`. Supported extensions, nested path rules and current size limits are described in [gameplay package limits](#gameplay-package-limits). Duplicate, unexpected, absolute, traversal, backslash, link, encrypted, unsupported and corrupt entries are rejected. Assets are read without extracting archive paths to disk. The older v0.9.20 release allows only flat GLB/PNG assets and caps gameplay ZIPs at 8 MiB and each asset at 2 MiB.

The manifest has exactly `id`, `name`, `version`, `author`, `description`, `entry`, `type`, `platforms`, `permissions` and `api_version`. The first five follow the message-script rules above. Set `entry` to `scripts/main.lua`, `type` to `gameplay`, `platforms` to `["any"]` and `api_version` to the **string** `"1"`. `permissions` must contain `scene`, and may additionally contain `world`, `layout`, `input`, `view`, `sound`, `hud`, `impulse`, `control`, `storage`, `controller` and `messaging`; duplicates and unknown permissions are rejected. `layout` and `impulse` also require `world`; `control` requires `world`, `input` and `view`; `controller` requires `input`. `scene` allows visible objects, `world` permits city floor and collision queries, `layout` reveals city cell positions and occupancy, `view` reads the user's in-world camera position and direction, `sound` plays built-in effects and packaged samples, `hud` displays text, `impulse` applies bounded player pushes, `control` offers player/camera control, `storage` persists bounded plugin data, `messaging` broadcasts local plugin events, and `input` provides movement axes and exclusive click routing while interacting with the world, not while typing or interacting with UI. `controller` permits the controller events described above. The host does not expose typed text.

Install through **Settings > Plugins > Install Package**. New packages start disabled. Selecting a gameplay plugin without a matching grant displays its ID and requested permissions; select it again within 30 seconds to consent and enable it. Consent is stored against the exact ZIP contents, so changed contents require consent again. Disabling releases its objects and GPU resources but retains consent for re-enabling; uninstall revokes consent. Use **Disable** to stop a failed plugin, or select its name to retry. An enabled plugin can restart automatically if its unchanged package still has consent. Callback errors stop the plugin and are reported in Settings and stderr. Do not approve packages you do not trust: Lua and image/model decoders run in the shell process, not an OS sandbox.

The script chunk returns a table with optional `on_start()`, `on_tick(dt)`, `on_input(control, value)` and `on_stop()` functions. The host calls `on_start` when enabling, `on_tick` each frame, `on_input` for permitted input changes and relative mouse impulses, and `on_stop` when stopping normally. State in local variables persists while the plugin runs, but a restart creates a fresh Lua state. The chunk and each callback have a 100,000-instruction budget; each plugin has 1 MiB of additional Lua allocations above its compiled-code baseline, with a 128 MiB compilation/total ceiling. `dt` is seconds, capped by the host at 0.1 per frame. Only Lua's `math` library and the functions below are opened: there is no base library, filesystem, network, module loader or operating-system API. Callbacks run on the application thread; exceeding the budget or raising an error stops the plugin and releases its scene.

* `scene_box(x,y,z,width,height,depth,r,g,b)` and `scene_model(model,texture,x,y,z,width,height,depth,r,g,b)` return owned integer handles. Texture `0` uses the GLB's embedded texture or white; otherwise use `asset_texture(path)`. `asset_model(path)` and `asset_texture(path)` return a package-local numeric asset ID or raise an error for an unknown path.
* `scene_move(handle,x,y,z)`, `scene_rotate(handle,x,y,z)` (radians), `scene_resize(handle,width,height,depth)`, `scene_colour(handle,r,g,b)` and `scene_remove(handle)` update owned objects. Coordinates are world-space, with Y vertical; box positions are their centres. Positions and rotations must be finite and at most 10,000 in magnitude, sizes must be in `(0,1000]`, and colour components in `[0,1]`. Up to 128 boxes and model instances combined are allowed.
* `scene_hit(handle,ox,oy,oz,dx,dy,dz,maxDistance)` tests a ray against the object's transformed unit box and returns the entry distance in world units, or nil on a miss. The origin must be finite within ±10,000, each direction component must be finite in `[-1.001,1.001]` and the nonzero direction is normalized; distance must be in `[0,100]`. A ray starting inside returns zero. This is a box approximation for models, not a triangle, city-occlusion or cross-plugin query. Unknown or removed handles fail the callback. It does not consume click input or affect shell interaction.
* With `world`, `world_available()` reports whether the city is active and built. `world_floor(x,y,z,radius,eyeHeight)` returns a city floor height; `world_resolve(x,y,z,radius,eyeHeight)` returns a collision-adjusted position. They fail when the city is unavailable; radii and eye heights must be between 0 and 100.
* With `world`, `world_track_clearance(x,z,radius)` returns the maximum height of native track-generation obstacle footprints intersecting the square from `(x-radius,z-radius)` to `(x+radius,z+radius)`, or zero if none intersects. It uses monuments, platforms and current/target tower dimensions, matching the native generator's obstacle inputs; it is **not** a complete city collision or bridge-clearance query. Coordinates must be finite within ±10,000 and radius between 0 and 100. It fails if the city is unavailable, the data is invalid, or the combined traversal exceeds 4,096 monuments, platforms, blocks and towers. It shares the 64-call callback budget. City geometry can change, so callers must resample.
* With `layout` and `world`, `city_cell(x,z)` returns `centerX, floorY, centerZ, pitch, occupied` for the grown city grid cell containing the point, or nil outside a cell. It reveals positions and occupancy, not names or paths. It fails when the city is unavailable or a query exceeds 4,096 examined platforms, 4,096 examined blocks or 4,096 towers in the selected block. City layout and occupancy may change; scripts must recheck and remove or relocate their objects. It does not enumerate every cell or reserve one, and multiple plugins may select the same cell.
* With `view`, `view_pose()` returns `x,y,z,dx,dy,dz` for the in-world camera or VR headset; it returns nil before entering the file room. Direction is the view direction, not a VR controller pointer. This is read-only and available while panels are open. At most 64 scene-hit, world, layout and view calls combined are allowed per callback.
* With `input`, `on_input` receives `move_x`, `move_y`, `move_z` axis changes in `[-1,1]`, Left Shift `boost` changes in `0/1`, and routed `click` press/release values `1`/`0`. An active lease owner also receives `mouse_yaw` / `mouse_pitch` radian impulses under the [relative mouse contract](#controller-input); these are not held axes. With `scene` and `input`, `scene_interactive(handle,true)` makes an owned object a click target; `false` removes it. The host chooses the nearest target within 14 units, breaking equal-distance ties by plugin ID, and consumes the click before native shell actions; misses go to the shell instead. Removing an object removes its target. `scene_hit` alone does not register one. With an active control lease, clicks go to its owner instead. A plugin can call `input_capture_clicks(true)` in a routed click press to capture later world clicks (for example while holding an item), then call `input_capture_clicks(false)` when finished. Capture cannot start without a routed press, never overrides UI or unavailable input, and is cleared on disable, failure or uninstall. The host resets held input when interaction becomes unavailable, including UI focus, native driving, VR and focus loss. Picking uses approximate object boxes, does not account for city occlusion, and targets behind walls can receive clicks. For controller events, also request `controller` as described above; VR controller input is not exposed.
* With `sound`, `sound_play(name, volume)` queues one built-in effect: `pickup`, `throw`, `boom`, `duck_pop`, `car_summon` or `rail_on`. Volume must be finite and in `[0,1]`, and the shell's sound volume applies. At most eight events can be queued before the host processes them; the host permits a burst of four and replenishes four per second per plugin. Exceeding either limit, lacking an audio device or exhausting the shared voice limit stops the plugin with an error. Events from a failed callback are discarded; disabling, failing or uninstalling a plugin stops its active voices. Already-buffered audio may finish playing. Bounded WAV/OGG package samples are described above; arbitrary filesystem audio and positional audio are not supported.
* With `sound`, `sound_engine(active, speed, boost)` controls one plugin-owned synthesized car hum. `active` and `boost` are booleans; `speed` is finite and between 0 and 110. Updates are coalesced until the host processes them; the latest value wins. At most four plugin hums run at once. Starting one without an audio device or when the hum limit is reached stops the plugin with an error. Turning it off, disabling, failing or uninstalling removes the owner's hum; already-buffered audio may finish playing. This is the built-in synthesized sound, not packaged or positional audio.
* With `hud`, `hud_set(text)` sets one plugin-owned, ID-labelled message of up to 32 printable ASCII characters; an empty string clears it. Messages appear in the file room and are coalesced at most four times per second per plugin. Up to four running plugins can display messages, stacked in ID order. Stopping, failing or uninstalling removes the message. This is display text only, not a shared scoring API.
* With `impulse` and `world`, `player_impulse(x,y,z)` queues a push velocity in world axes. Values must be finite, horizontal magnitude at most 12 and vertical between 0 and 8. Only one push may be pending; a plugin may apply one every two seconds, with excess requests stopping it. Pushes decay over time and the combined displacement from all plugins is capped at 12 units/second horizontally and 8 vertically before normal world collision and gravity. Pushes are discarded and residual motion cleared when world interaction is unavailable, including UI focus, driving, VR and loss of window focus; disabling or failing a plugin clears its motion. This does not reproduce the native explosion's exact physics.
* With `control`, `control_offer(x,y,z)` advertises a finite position within ±10,000; `control_offer(nil)` withdraws it. While the focused city world is available, the nearest offer within four units is shown with its plugin ID and G activates it. Only one owner can hold control. The host sends `on_input("control",1)` on activation and `on_input("control",0)` on release; the script must not propose movement until activated. While active, `control_pose(x,y,z,yaw,pitch)` queues at most one absolute player/camera pose per host tick, with finite coordinates and yaw within ±10,000 and pitch within ±1.4 radians. Input callbacks and `on_tick` share that queue, so normally propose the pose only from `on_tick`. The host limits movement to **1000 units/second** (previously 25) and yaw/pitch changes to 4 radians/second, enforces city bounds, and resolves collision in steps no larger than 0.25 units; invalid proposals stop the plugin. Host ticks are capped at 0.1 seconds, allowing up to 100 units of requested travel per tick, with about 400 collision substeps at the ceiling. Collision adjustments that cannot fit within the movement or world bounds block further motion instead of stopping the plugin. Camera motion can lag a vehicle or stop on collision. Escape or G releases control; leaving the city, overlays, focus loss, native driving, riding or VR also releases it. Other plugins do not receive movement/click input while control is held. Disable, failure and uninstall release it. This is not an OS security boundary or the native car physics.

  This ceiling applies to the scripted player/camera pose, not a vehicle's own speed setting.
  Controller-enabled offers also accept a fresh Xbox Start press; Start, Back/View or B releases
  their lease. These exit buttons are host-reserved, other pad inputs are isolated from native
  shortcuts while owned, and held-state snapshots are cleared/refreshed on ownership changes
  as described in the [controller contract](#controller-input).
  Existing scripts, including generated third-party scripts that pace motion below 25 units/second,
  do not speed up automatically: change their own speed/chase-camera pacing to use the new allowance.
  The bundled car example and native vehicle tuning are unchanged. Motion still stays inside the city's
  radial extent and eye-height range 0.35-450, as well as the absolute coordinate bounds above.
  Collision uses the city's existing player-shaped resolver (0.8-unit radius, 1.7-unit eye height),
  not swept vehicle-body physics; higher speeds mean more collision work, not a frame-rate guarantee.

PNG textures are limited to 1024×1024, must pass checksum and decoder checks, and must be opaque to render. GLB support is deliberately narrow: up to 32 meshes and 32 nodes are flattened into one static triangle model, with at most 64 total primitive instances sharing one material, embedded binary data, a simple base-colour material and at most one embedded PNG. Reused meshes count separately for every node. Geometry is capped at 65,536 vertices and 196,608 indices across all instances. Nonuniform node transforms and shear are supported with inverse-transpose normals, but reflected, singular or ill-conditioned transforms, external URIs, animations, skins, extensions and many other glTF features are rejected. Flattened nodes cannot be animated independently. The bundled Duck model is supported; the bundled CarConcept model is not. Decoded package assets are capped at 16 MiB. Up to four gameplay plugins may run; GPU model resources are limited to 16 MiB and 32 model/texture combinations per plugin, and 64 MiB and 128 combinations globally. Resource creation may fail on a constrained device.

At startup enabled plugins can start before the city is active. Check `world_available()` before querying the world, including in `on_start`; an unavailable query stops the plugin and must be retried from Settings. There is not yet a city-availability event.
