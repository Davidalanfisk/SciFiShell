SCIFI SHELL 0.9.29 - INCLUDED GAMEPLAY PLUGINS

On the first launch with this bundle, packages not already installed in the
current profile are installed automatically, DISABLED. Nothing here grants
permissions or starts a script. Enable a plugin in Settings > Plugins and
review the consent notice. The network demo sends no requests until enabled.

Included:
  TRACKFLY 2 2.2.1 (com.davidalanfisk.trackfly2), by Davidalanfisk.
    Exact author-approved ZIP from the v0.9.27 public release; unchanged.
    SHA256 4e7b0ea92f4d0b7c9ad71b70d9c8133d645b53649d2ccdc1dd453f0d4a789f02
    Original controls/provenance notes: TrackFly2-2.2.1-controls.txt.
    G / Xbox Start near its launch pad enters flight; Escape/G/Start/B/View exits.
    Mouse look, WASD throttle/yaw, Shift boost; Xbox sticks, triggers, A/RB boost.
    Published with the author's approval; no new asset licence is asserted.

  BUG HUNTER 0.1.1 (com.scifishell.bug-hunter), original procedural Lua.
    G / Start equips. Mouse look, WASD move, left-click fire, right-click scope,
    wheel 2x/4x/8x zoom, Shift sprint; Escape exits.
    Xbox sticks move/look, RT fire, LT scope, D-pad zoom, left-stick click sprint,
    Y/Start/View exits. Infinite ammo. Score/kills save per profile.
    Requires the new API 4 host. Desktop FPS only, simplified city collision hulls.

  NETWORK HUD DEMO 1.1.2 (com.example.network-hud-demo).
    Original weather/news/map observatory, Lua and procedurally authored GLB.
    Choose one of six preset cities and BBC/NPR/NASA in the plugin's room.
    N opens the current story in the built-in browser while focused in the city.
    HTTPS/text access only to declared, approved hosts; Windows transport only.
    Mini-map unavailable inside rooms or in VR; no automatic geolocation.

UPGRADES AND REMOVAL
Existing same-ID packages, enabled choices, consents, scores, settings and
editable workspaces are not replaced or reset by this bundle. A one-time
per-profile receipt at SciFiShell/plugin-defaults/<plugin-id> records that each
default was offered. Removing it in Settings does not cause automatic reinstall
on the next launch or host update. Future bundle versions do not silently
overwrite an installed package or restore a removed one.

To reinstall or explicitly update a removed/older plugin, use Settings > Plugins
> Install Package with a ZIP in this directory (or a published release download).
If the same ID is already installed, back up your edits, disable/remove it, then
import. Scores persist, but uninstall removes its editable workspace and room
settings. Re-enable and approve changed package permissions normally.

These are editable data/Lua packages, not trusted native modules or an OS
sandbox. Only enable packages you trust. No ROMs, BIOS files, user profiles or
stopped local plugin-workshop experiment are included.
