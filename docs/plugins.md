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

Installed packages start disabled unless a retained preference says otherwise. Installing never overwrites an existing package. To uninstall an installed theme or script, select its **Remove** card twice within 15 seconds; this disables it first and removes the package file, leaving other user data untouched. Built-in plugins cannot be uninstalled. ZIP themes, arbitrary ZIP layouts, permission grants, plugin data storage, updates and an app store are not implemented. Do not treat native code as sandboxed.

## Lua plugin authoring specification

[Download the example clock plugin ZIP](https://github.com/Davidalanfisk/SciFiShell/releases/download/v0.9.19/scifishell-clock-example.zip); its [source files](../examples/plugins/clock) are included in this repository. It contributes a **Run CLOCK** card which displays the current Unix time. This is the complete supported executable extension interface today: a script returns a short message on the Settings page. It cannot add gameplay, windows, background tasks or new host APIs.

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
| `permissions` | Exactly `[]`; no permission grants exist. |
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

Lua runs in-process, not in an OS sandbox, and interpreter vulnerabilities remain possible. No filesystem, network, OS, input, native code, module loader or persistent data API is exposed to scripts. Native third-party packages, permissions, background scripts, custom gameplay/scene APIs, arbitrary ZIP layouts, package updates and an app store are not implemented.
