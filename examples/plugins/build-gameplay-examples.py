"""Build the two experimental gameplay ZIPs into a new output directory."""

import argparse
from pathlib import Path
import zipfile


ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "examples" / "plugins"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()
    output = args.output_directory
    output.mkdir(parents=True, exist_ok=True)
    car_zip = output / "scripted-car-track.zip"
    parks_zip = output / "scripted-parks-toys.zip"
    if car_zip.exists() or parks_zip.exists():
        parser.error("output ZIP already exists; choose an empty output directory")

    car = EXAMPLES / "car-track"
    park = (EXAMPLES / "park-cell" / "scripts" / "main.lua").read_text(encoding="utf-8")
    toys = (EXAMPLES / "snack-stand" / "scripts" / "main.lua").read_text(encoding="utf-8")
    script = (
        "local park = (function()\nlocal hud_set = function(_) end\n"
        + park
        + "\nend)()\nlocal toys = (function()\n"
        + toys
        + "\nend)()\nreturn {\n"
        + "on_start=function() park.on_start() toys.on_start() end,\n"
        + "on_tick=function(dt) park.on_tick(dt) toys.on_tick(dt) end,\n"
        + "on_input=function(key,value) toys.on_input(key,value) end\n}\n"
    )
    if len(script.encode("utf-8")) > 16384:
        parser.error("combined script exceeds the gameplay script limit")
    with zipfile.ZipFile(car_zip, "x", zipfile.ZIP_DEFLATED) as archive:
        for name in ("plugin.json", "scripts/main.lua", "assets/car.glb", "assets/paint.png"):
            archive.write(car / name, name)
    with zipfile.ZipFile(parks_zip, "x", zipfile.ZIP_DEFLATED) as archive:
        archive.write(EXAMPLES / "parks-toys" / "plugin.json", "plugin.json")
        archive.writestr("scripts/main.lua", script)
        archive.write(ROOT / "assets" / "models" / "Duck.glb", "assets/duck.glb")
    print(car_zip)
    print(parks_zip)


if __name__ == "__main__":
    main()
