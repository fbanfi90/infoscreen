# PlatformIO pre-build script: reads .env and turns each setting into a
# -D compiler define, so personal settings (WiFi, station, city) never live in
# tracked files. A variable set in the shell overrides the same one in .env.
import os
import sys

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

# Settings that are text become C string literals; the rest are passed as-is.
STRINGS = {"WIFI_SSID", "WIFI_PASS", "STATION_ID", "VIA_FILTER", "BOARD_TITLE",
           "WEATHER_PLZ", "WEATHER_NAME", "TZ_POSIX"}
NUMBERS = {"DEPARTURES_FETCH", "SCREEN_ROTATION"}
# Optional font folder; handled below, not passed as a define.
FONTS = {"FONT"}
REQUIRED = {"WIFI_SSID", "WIFI_PASS", "STATION_ID", "WEATHER_PLZ"}

path = os.path.join(env.subst("$PROJECT_DIR"), ".env")  # noqa: F821
values = {}
if os.path.exists(path):
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                sys.exit(f".env line {n}: expected KEY=value, got: {line}")
            key, val = (s.strip() for s in line.split("=", 1))
            if len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'":
                val = val[1:-1]
            values[key] = val
else:
    print("load_env.py: no .env found (copy .env.example to .env)")

for key in STRINGS | NUMBERS | FONTS:
    if key in os.environ:
        values[key] = os.environ[key]

missing = [k for k in sorted(REQUIRED) if not values.get(k)]
if missing:
    sys.exit("load_env.py: missing in .env: " + ", ".join(missing))

unknown = sorted(set(values) - STRINGS - NUMBERS - FONTS)
if unknown:
    print("load_env.py: ignoring unknown .env keys: " + ", ".join(unknown))

defines = []
for key, val in values.items():
    if key in STRINGS:
        defines.append((key, env.StringifyMacro(val)))  # noqa: F821
    elif key in NUMBERS and val != "":
        if not val.lstrip("-").isdigit():
            sys.exit(f"load_env.py: {key} must be a number, got: {val}")
        defines.append((key, val))
env.Append(CPPDEFINES=defines)  # noqa: F821

# --- fonts ---
# FONT names a folder of BDF fonts: fonts/<FONT>/, or a path. For each text
# style the largest monospace font that fits is converted into the build
# directory and replaces the built-in font (see "text" in src/main.cpp). Styles with no
# fitting file, or all of them if FONT is empty, keep the built-in font.
project = env.subst("$PROJECT_DIR")  # noqa: F821
sys.path.insert(0, os.path.join(project, "fonts"))
import bdf2gfx  # noqa: E402

out_dir = os.path.join(env.subst("$BUILD_DIR"), "fonts")  # noqa: F821
os.makedirs(out_dir, exist_ok=True)
env.Append(CPPPATH=[out_dir])  # noqa: F821

font = values.get("FONT", "")
if font:
    font_dir = os.path.expanduser(font)
    if os.sep not in font_dir:
        font_dir = os.path.join("fonts", font_dir)
    font_dir = os.path.join(project, font_dir)
    if not os.path.isdir(font_dir):
        sys.exit(f"load_env.py: FONT={font}: folder {font_dir} not found. Leave FONT "
                 f"empty to use the built-in font.")
    bdfs = sorted(os.path.join(d, f) for d, _, fs in os.walk(font_dir)
                  for f in fs if f.lower().endswith(".bdf"))
    if not bdfs:
        sys.exit(f"load_env.py: FONT={font}: no .bdf files in {font_dir}")
    chosen, skipped = bdf2gfx.pick(bdfs)
    for reason in skipped:
        print(f"load_env.py: skipping {reason}")
    for role, pick in chosen.items():
        mw, mh, _ = bdf2gfx.ROLES[role]
        if pick is None:
            print(f"load_env.py: font {role:6} (max {mw}x{mh}): nothing fits, using built-in")
            continue
        path, w, h = pick
        code, _, _ = bdf2gfx.convert(path, role)
        out = os.path.join(out_dir, f"font_{role}.h")
        old = open(out).read() if os.path.exists(out) else None
        if code != old:  # rewrite only on change, so unchanged fonts don't trigger rebuilds
            with open(out, "w") as f:
                f.write(code)
        env.Append(CPPDEFINES=[f"FONT_{role.upper()}_CUSTOM"])  # noqa: F821
        shown = os.path.relpath(path, project)
        if shown.startswith(".."):
            shown = path
        print(f"load_env.py: font {role:6} (max {mw}x{mh}): {shown} ({w}x{h})")
