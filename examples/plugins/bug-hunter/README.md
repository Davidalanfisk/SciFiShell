# Bug Hunter

Original, small single-player laser-hunting prototype. Requires **SciFiShell v0.9.29 (API 4)**;
v0.9.28 is not sufficient. No network access, third-party assets or additional dependencies.

Included installed **disabled** with v0.9.29; enable BUG HUNTER in **Settings > Plugins** and approve its permissions.
For manual installation/reinstallation, import `BugHunter-0.1.1.zip` there.
Enter the city, close Settings and press **G / Xbox Start** to equip. Bugs appear near the player on clear city
ground, up to six at once, with random sizes, crawling and respawn delays. They are cyan wire cages with translucent
shells. The rifle is a view-space HUD weapon with a bright world-space laser, strong camera kick and impact sparks.

| Action | Mouse / keyboard | Xbox |
|---|---|---|
| Walk / look | WASD / mouse | Left / right sticks |
| Sprint | Hold Left Shift | Hold left-stick click |
| Aim | Hold right mouse | Hold LT |
| Magnification | Wheel: 2x / 4x / 8x | D-pad up / down |
| Fire | Left mouse (hold to repeat) | RT |
| Equip | G | Start |
| Exit weapon mode | Escape or G | Y, Start or View |

Infinite ammo, with a heavy 0.85-second firing interval. A kill scores **100 + 2 per metre of hit distance**.
Score and total kills save immediately to plugin-scoped storage and survive restarting/disabling the plugin.
Shots stop at city collision hulls, so targets behind a building do not score; misses into empty sky do not create
fictional impact explosions. Effects are visual/audio only: no city files, geometry or other plugins are damaged.

City ray queries use the host's simplified collision hulls, not visual mesh triangles. Custom buildings use their
existing box hulls; decorative gaps, sloped pad rims and native toys/track scenery need not match exact pixels.
Translucency is depth-tested, with back-to-front sorting per plugin, not order-independent transparency.
FPS play/zoom is desktop-only; UI/focus loss or controller disconnection releases the weapon and resets zoom.
The host reserves safe exit keys. Bugs may continue appearing while unequipped; disable the plugin to remove them.
The sound permission requires a working effects audio device. Invalid saved data is reported rather than overwritten.

Source is `plugin.json` and `scripts/main.lua`. Package only these two files at those root-relative paths, with
forward slashes in ZIP entry names. No containing directory, generator or downloaded asset is required.

No play-test, test-suite execution or screenshots were performed for this prototype: the user requested to handle
testing and visual confirmation. The user reported version 0.1.1 working after the `max_x` city-bounds fix.
Focused host regression cases are included for the user to run; they were not run for this release.

Updating from 0.1.0: in **Settings > Plugins**, select **REMOVE BUG HUNTER** twice within 15 seconds, then
install 0.1.1 and re-enable it with fresh consent. Plugin score and kill storage is retained on uninstall.
This does not require restarting or changing the host. Existing editable workspace files are removed by uninstall;
back up any edits before removing the old package.
