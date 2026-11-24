#!/usr/bin/env python3
"""Convert monospace BDF bitmap fonts to Adafruit GFXfont C code for
Arduino_GFX, covering Latin-1 (codes 32..255).

Every glyph is stored as a full character cell, positioned so it exactly
matches the background box Arduino_GFX paints (top at yAdvance*2/3 above the
cursor). That makes text drawn with a background color overwrite old text
cleanly, which is why the font must be monospace. Missing codes become blank.

The firmware uses five font roles. Each one is generated as `font_<role>`
plus FONT_<ROLE>_W / FONT_<ROLE>_H (cell size in pixels).

load_env.py runs this automatically for fonts set in .env. By hand:
  bdf2gfx.py ROLE font.bdf > font_ROLE.h      (ROLE: small, text, header, medium, big)
"""
import os
import sys

# role: (largest cell w, h the screen layout has room for, prefer bold).
# The sizes are the Terminus ones; smaller fonts fit too.
ROLES = {
    "small":  (6, 12, False),   # status line, forecast labels
    "text":   (8, 14, False),   # destinations, min/max
    "header": (8, 16, True),    # titles, clock
    "medium": (12, 24, True),   # line, departure time, minutes
    "big":    (16, 32, True),   # current temperature
}


def parse_bdf(path):
    glyphs, bbox, ascent, descent, bold = {}, None, None, None, False
    with open(path, encoding="latin-1") as f:
        lines = iter(f.read().splitlines())
    for line in lines:
        p = line.split()
        if not p:
            continue
        if p[0] == "FONTBOUNDINGBOX":
            bbox = tuple(int(v) for v in p[1:5])
        elif p[0] == "FONT_ASCENT":
            ascent = int(p[1])
        elif p[0] == "FONT_DESCENT":
            descent = int(p[1])
        elif p[0] == "WEIGHT_NAME":
            bold = "bold" in line.lower()
        elif p[0] == "STARTCHAR":
            enc, bbx, dwidth, rows = None, None, None, []
            for line in lines:
                q = line.split()
                if not q:
                    continue
                if q[0] == "ENCODING":
                    enc = int(q[1])
                elif q[0] == "DWIDTH":
                    dwidth = int(q[1])
                elif q[0] == "BBX":
                    bbx = tuple(int(v) for v in q[1:5])
                elif q[0] == "BITMAP":
                    for line in lines:
                        if line.startswith("ENDCHAR"):
                            break
                        rows.append(int(line, 16) << (32 - 4 * len(line)))  # left-align to 32 bits
                    break
            if enc is not None and 32 <= enc <= 255 and bbx:
                glyphs[enc] = (bbx, dwidth, rows)
    if bbox is None:
        raise ValueError(f"{path}: not a BDF font (no FONTBOUNDINGBOX)")
    if ascent is None:
        ascent = bbox[1] + bbox[3]
    if descent is None:
        descent = -bbox[3]
    return glyphs, bbox, ascent, descent, bold


def cell_size(glyphs, bbox, ascent, descent, path):
    widths = {dw if dw is not None else bbox[0] for _, dw, _ in glyphs.values()}
    if len(widths) != 1:
        raise ValueError(f"{path}: not monospace (glyph widths {sorted(widths)}); "
                         "only monospace bitmap fonts are supported")
    return widths.pop(), ascent + descent


def info(path):
    """Returns (cell width, cell height, bold). Raises ValueError if unusable."""
    glyphs, bbox, ascent, descent, bold = parse_bdf(path)
    return (*cell_size(glyphs, bbox, ascent, descent, path), bold)


def pick(paths):
    """Chooses a font file per role from `paths`: the largest one that fits the
    role's cell size, bold or not as the role prefers. Returns
    ({role: (path, w, h) or None}, [skipped files with reasons])."""
    fonts, skipped = [], []
    for p in paths:
        try:
            fonts.append((p, *info(p)))
        except (OSError, ValueError) as e:
            skipped.append(str(e))
    chosen = {}
    for role, (mw, mh, want_bold) in ROLES.items():
        fits = [f for f in fonts if f[1] <= mw and f[2] <= mh]
        # ties: first file name wins (max() keeps the first of equal keys)
        best = max(sorted(fits), key=lambda f: (f[2], f[1], f[3] == want_bold), default=None)
        chosen[role] = best[:3] if best else None
    return chosen, skipped


def convert(path, role):
    """Returns (C code, cell width, cell height) for one font role."""
    glyphs, bbox, ascent, descent, _ = parse_bdf(path)
    W, H = cell_size(glyphs, bbox, ascent, descent, path)
    if 0xFC not in glyphs:
        print(f"bdf2gfx: warning: {os.path.basename(path)} has no Latin-1 glyphs "
              "(ä, ö, ü, ...); they will show as blanks", file=sys.stderr)
    base = H * 2 // 3  # Arduino_GFX's assumed baseline
    name = f"font_{role}"

    bitmap, table = [], []
    for code in range(32, 256):
        cell = [[0] * W for _ in range(H)]
        if code in glyphs:
            (w, h, xo, yo), _, rows = glyphs[code]
            top = ascent - (yo + h)  # cell row of the glyph's first bitmap row
            for i, row in enumerate(rows):
                for j in range(w):
                    if row >> (31 - j) & 1:
                        r, c = top + i, xo + j
                        if 0 <= r < H and 0 <= c < W:
                            cell[r][c] = 1
        bits = [b for r in cell for b in r]
        offset = len(bitmap)
        for i in range(0, len(bits), 8):
            chunk = bits[i:i + 8] + [0] * (8 - len(bits[i:i + 8]))
            bitmap.append(int("".join(map(str, chunk)), 2))
        # never end a C++ comment with a backslash: it would swallow the next line
        ch = chr(code) if 32 < code < 127 and code != 0x5C else ""
        table.append(f"  {{{offset:5d}, {W}, {H}, {W}, 0, {-base}}},  // 0x{code:02X} {ch}".rstrip())

    if len(bitmap) >= 65536:
        raise ValueError(f"{path}: {W}x{H} is too large for GFXfont (max ~32x64)")
    R = role.upper()
    out = [f"// {os.path.basename(path)}: {W}x{H} cell, generated by fonts/bdf2gfx.py",
           f"#define FONT_{R}_W {W}",
           f"#define FONT_{R}_H {H}",
           f"const uint8_t {name}_bitmap[] PROGMEM = {{"]
    for i in range(0, len(bitmap), 16):
        out.append("  " + ", ".join(f"0x{b:02X}" for b in bitmap[i:i + 16]) + ",")
    out.append("};")
    out.append(f"const GFXglyph {name}_glyphs[] PROGMEM = {{")
    out += table
    out.append("};")
    out.append(f"const GFXfont {name} PROGMEM = {{(uint8_t *){name}_bitmap, "
               f"(GFXglyph *){name}_glyphs, 32, 255, {H}}};")
    return "\n".join(out) + "\n", W, H


def main():
    args = sys.argv[1:]
    try:
        if len(args) == 2 and args[0] in ROLES:
            sys.stdout.write(convert(args[1], args[0])[0])
        else:
            sys.exit(__doc__)
    except (OSError, ValueError) as e:
        sys.exit(f"bdf2gfx: {e}")


if __name__ == "__main__":
    main()
