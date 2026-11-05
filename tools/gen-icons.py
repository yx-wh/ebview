#!/usr/bin/env python3
"""Generate the EBView icon set from one vector description.

The original icon (pixmaps/ebview.xpm) was a hand-drawn 48x48 raster from
2001: a teal lens with a serif "E" and a magnifier handle.  This script
carries the same geometry in floating point so every size is rendered from
scratch instead of being scaled up, which is what kept the old set blurry on
HiDPI displays.

    pixmaps/ebview.svg                      vector master
    pixmaps/hicolor/<N>x<N>/apps/ebview.png 16 24 32 48 64 128 256
    pixmaps/ebview.ico                      multi-size Windows icon
    pixmaps/ebview.xpm                      48x48, compiled into the binary
                                            by src/pixmap.c

Usage:  python3 tools/gen-icons.py
Requires Pillow only.  Run it from anywhere; paths are relative to the repo.

The SVG is an output, not the source: edit the constants below and re-run.
"""

import io
import os
import re
import struct

from PIL import Image, ImageDraw

# ---------------------------------------------------------------- geometry --
# Everything is expressed in the 48x48 design space of the original icon.

CANVAS = 48.0

LENS_CX, LENS_CY = 20.7, 20.7
LENS_R = 20.7          # outer edge of the rim; the disc fills the whole circle
RIM_W = 1.0            # diameter of the dark outline
RIM_COLOR = (81, 81, 81)

# The lens gradient runs diagonally: bright at the upper left, deep teal at
# the lower right.  Endpoints are the two points where the 45 degree axis
# meets the rim, so the stops map onto the disc exactly.
GRAD_P0 = (6.06, 6.06)
GRAD_P1 = (35.34, 35.34)
GRAD_C0 = (1, 247, 229)
GRAD_C1 = (36, 105, 121)

HANDLE_P0 = (34.4, 35.4)
HANDLE_P1 = (48.4, 50.1)
HANDLE_W = 3.6

E_FILL = (237, 249, 248)
E_STROKE = 1.2         # fattens the outline so the stem matches the original

# Serif "E" from Liberation Serif Regular, fitted to the 23x26 box the
# original glyph occupies (x 8.8..31.8, y 7.8..33.8).  Only the outline's
# centre line is stored; E_STROKE is added on top at render time, which is
# why the box above is inset by half a stroke.  Baked in as a path so neither
# this script nor the SVG depends on a font being installed.
E_PATH = (
    "M9.60 32.42L13.04 31.92L13.04 10.06L9.60 9.58L9.60 8.60L29.72 8.60L2"
    "9.72 14.54L28.40 14.54L27.76 10.52Q25.52 10.26 21.28 10.26L16.90 10."
    "26L16.90 19.96L24.14 19.96L24.76 17.00L26.04 17.00L26.04 24.62L24.76"
    " 24.62L24.14 21.62L16.90 21.62L16.90 31.74L22.18 31.74Q27.34 31.74 2"
    "8.94 31.44L30.08 26.85L31.40 26.85L31.02 33.40L9.60 33.40Z"
)

PNG_SIZES = (16, 24, 32, 48, 64, 128, 256)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
XPM_SIZE = 48

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ------------------------------------------------------------------ paths --
def flatten(path, steps=24):
    """Turn an SVG path (M/L/C/Q/Z, absolute, as emitted by fontTools) into
    closed polygons.  Enough for the one path above, not a general parser."""
    tokens = re.findall(r"[MLCQZ]|-?\d*\.?\d+", path)
    pos = 0
    cur = (0.0, 0.0)
    polygons, points = [], []

    def number():
        nonlocal pos
        value = float(tokens[pos])
        pos += 1
        return value

    while pos < len(tokens):
        cmd = tokens[pos]
        pos += 1
        if cmd == "M":
            cur = (number(), number())
            points = [cur]
        elif cmd == "L":
            cur = (number(), number())
            points.append(cur)
        elif cmd == "C":
            p1 = (number(), number())
            p2 = (number(), number())
            p3 = (number(), number())
            for i in range(1, steps + 1):
                t = i / steps
                u = 1 - t
                points.append((
                    u ** 3 * cur[0] + 3 * u * u * t * p1[0]
                    + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                    u ** 3 * cur[1] + 3 * u * u * t * p1[1]
                    + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
            cur = p3
        elif cmd == "Q":
            p1 = (number(), number())
            if pos < len(tokens) and tokens[pos] not in "MLCQZ":
                p2 = (number(), number())
            else:
                p2 = p1
            for i in range(1, steps + 1):
                t = i / steps
                u = 1 - t
                points.append((
                    u * u * cur[0] + 2 * u * t * p1[0] + t * t * p2[0],
                    u * u * cur[1] + 2 * u * t * p1[1] + t * t * p2[1]))
            cur = p2
        elif cmd == "Z":
            polygons.append(points)
            points = []
    if len(points) > 1:
        polygons.append(points)
    return polygons


# --------------------------------------------------------------- rendering --
def _gradient_color(t):
    t = 0.0 if t < 0 else (1.0 if t > 1 else t)
    return tuple(int(round(GRAD_C0[i] + (GRAD_C1[i] - GRAD_C0[i]) * t))
                 for i in range(3))


def _gradient_layer(width, scale):
    """A 45 degree linear gradient.  The colour only depends on x+y, so the
    layer is built from one 1-row ramp shifted once per row - exact, and far
    cheaper than evaluating the ramp per pixel."""
    ramp_w = 2 * width - 1
    ramp = Image.new("RGB", (ramp_w, 1))
    load = ramp.load()
    span = (GRAD_P0[0] + GRAD_P0[1], GRAD_P1[0] + GRAD_P1[1])
    for x in range(ramp_w):
        load[x, 0] = _gradient_color(
            (x / scale - span[0]) / (span[1] - span[0]))

    layer = Image.new("RGB", (width, width))
    for y in range(width):
        layer.paste(ramp.crop((y, 0, y + width, 1)), (0, y))
    return layer


def render(size, supersample=8):
    k = size * supersample / CANVAS
    w = int(round(CANVAS * k))

    image = Image.new("RGBA", (w, w), (0, 0, 0, 0))

    disc_mask = Image.new("L", (w, w), 0)
    ImageDraw.Draw(disc_mask).ellipse(
        [(LENS_CX - LENS_R) * k, (LENS_CY - LENS_R) * k,
         (LENS_CX + LENS_R) * k, (LENS_CY + LENS_R) * k], fill=255)
    image.paste(_gradient_layer(w, k), (0, 0), disc_mask)

    draw = ImageDraw.Draw(image)

    # The rim is drawn inside the disc's bounding box, so its outer edge lands
    # exactly on the lens radius.
    radius = LENS_R * k
    draw.ellipse([LENS_CX * k - radius, LENS_CY * k - radius,
                  LENS_CX * k + radius, LENS_CY * k + radius],
                 outline=RIM_COLOR + (255,),
                 width=max(1, int(round(RIM_W * k))))

    for polygon in flatten(E_PATH):
        points = [(x * k, y * k) for x, y in polygon]
        draw.polygon(points, fill=E_FILL + (255,))
        stroke = max(1, int(round(E_STROKE * k)))
        draw.line(points + [points[0]], fill=E_FILL + (255,),
                  width=stroke, joint="curve")

    draw.line([(HANDLE_P0[0] * k, HANDLE_P0[1] * k),
               (HANDLE_P1[0] * k, HANDLE_P1[1] * k)],
              fill=RIM_COLOR + (255,), width=max(1, int(round(HANDLE_W * k))))

    return image.resize((size, size), Image.BOX)


# ------------------------------------------------------------------ writers --
def write_svg(path):
    c0 = "#%02X%02X%02X" % GRAD_C0
    c1 = "#%02X%02X%02X" % GRAD_C1
    rim = "#%02X%02X%02X" % RIM_COLOR
    fill = "#%02X%02X%02X" % E_FILL
    # SVG strokes straddle the path while the renderer draws the rim inside
    # the lens outline, so the radius has to be pulled in by half a stroke for
    # the two to agree.
    rim_r = LENS_R - RIM_W / 2
    svg = f"""<?xml version="1.0" encoding="UTF-8"?>
<!-- EBView application icon. Generated by tools/gen-icons.py; edit that
     file (or the geometry it describes) rather than hand-tuning this one. -->
<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48"
     viewBox="0 0 48 48">
  <title>EBView</title>
  <defs>
    <linearGradient id="lens" gradientUnits="userSpaceOnUse"
                    x1="{GRAD_P0[0]}" y1="{GRAD_P0[1]}"
                    x2="{GRAD_P1[0]}" y2="{GRAD_P1[1]}">
      <stop offset="0" stop-color="{c0}"/>
      <stop offset="1" stop-color="{c1}"/>
    </linearGradient>
  </defs>
  <circle cx="{LENS_CX}" cy="{LENS_CY}" r="{rim_r}" fill="url(#lens)"
          stroke="{rim}" stroke-width="{RIM_W}"/>
  <path d="{E_PATH}" fill="{fill}" stroke="{fill}"
        stroke-width="{E_STROKE}" stroke-linejoin="round"/>
  <line x1="{HANDLE_P0[0]}" y1="{HANDLE_P0[1]}"
        x2="{HANDLE_P1[0]}" y2="{HANDLE_P1[1]}"
        stroke="{rim}" stroke-width="{HANDLE_W}"/>
</svg>
"""
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(svg)


def write_xpm(image, path):
    """48x48 XPM with two-character keys, which is what the compiled-in
    window icon uses (src/pixmap.c includes this file directly)."""
    flat = image.convert("RGBA")
    transparent = (0, 0, 0, 0)

    def collect(source):
        found = {}
        width, height = source.size
        pixels = source.load()
        for y in range(height):
            for x in range(width):
                pixel = pixels[x, y]
                key = (transparent if pixel[3] < 128
                       else (pixel[0], pixel[1], pixel[2], 255))
                found[key] = None
        return found

    palette = collect(flat)
    if len(palette) > 256:
        # Quantise first, then rebuild the palette from the reduced image.
        flat = flat.convert("P", palette=Image.ADAPTIVE, colors=255).convert("RGBA")
        palette = collect(flat)

    colors = list(palette)
    transparent_first = transparent in colors
    if transparent_first:
        colors.remove(transparent)
        colors.insert(0, transparent)

    # Exactly 64 symbols, two per key, so 256 colours fit into a 2-char XPM.
    # No space: "  " is reserved for the "None" (transparent) key.
    symbols = ("+abcdefghijklmnopqrstuvwxyz"
               "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.")
    keys = {}
    index = 0
    for color in colors:
        if color is transparent:
            keys[color] = "  "
            continue
        keys[color] = symbols[index // 64] + symbols[index % 64]
        index += 1

    width, height = flat.size
    lines = ["/* XPM */", "static char * ebview_xpm[] = {",
             '"%d %d %d 2",' % (width, height, len(colors))]
    for color in colors:
        if color is transparent:
            lines.append('"   c None",')
        else:
            lines.append('"%s c #%02X%02X%02X",' % (keys[color], color[0], color[1], color[2]))

    pixels = flat.load()
    for y in range(height):
        row = []
        for x in range(width):
            pixel = pixels[x, y]
            key = transparent if pixel[3] < 128 else (pixel[0], pixel[1], pixel[2], 255)
            row.append(keys[key])
        lines.append('"%s",' % "".join(row))
    lines[-1] = lines[-1].rstrip(",")
    lines.append("};")

    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def write_ico(images, path):
    """Multi-size .ico with 32bpp DIB payloads.  Written by hand because
    Pillow would rescale one bitmap instead of using our per-size renders."""
    header = struct.pack("<HHH", 0, 1, len(images))
    entries = b""
    payloads = b""
    offset = 6 + 16 * len(images)

    for size, image in images:
        width = height = size
        if size >= 128:
            # Stored as PNG (Vista and later).  An uncompressed 256x256
            # 32bpp DIB is 256 KB on its own, which would dwarf the binary.
            buffer = io.BytesIO()
            image.convert("RGBA").save(buffer, format="PNG", optimize=True)
            payload = buffer.getvalue()
        else:
            pixels = image.convert("RGBA").load()
            xor = bytearray()
            for y in range(height - 1, -1, -1):          # bottom-up
                for x in range(width):
                    r, g, b, a = pixels[x, y]
                    xor += bytes((b, g, r, a))
            and_mask = bytearray()
            stride = ((width + 31) // 32) * 4
            for y in range(height - 1, -1, -1):
                bits = bytearray(stride)
                for x in range(width):
                    if pixels[x, y][3] == 0:
                        bits[x // 8] |= 0x80 >> (x % 8)
                and_mask += bits

            dib = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0,
                              len(xor) + len(and_mask), 0, 0, 0, 0)
            payload = bytes(dib + xor + and_mask)
        entries += struct.pack("<BBBBHHII",
                              width & 0xFF, height & 0xFF, 0, 0, 1, 32,
                              len(payload), offset)
        payloads += payload
        offset += len(payload)

    with open(path, "wb") as handle:
        handle.write(header + entries + payloads)


# --------------------------------------------------------------------- main --
def main():
    pixmaps = os.path.join(ROOT, "pixmaps")

    write_svg(os.path.join(pixmaps, "ebview.svg"))

    for size in PNG_SIZES:
        target = os.path.join(pixmaps, "hicolor", "%dx%d" % (size, size),
                              "apps", "ebview.png")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        render(size).save(target)

    write_ico([(size, render(size)) for size in ICO_SIZES],
              os.path.join(pixmaps, "ebview.ico"))
    write_xpm(render(XPM_SIZE), os.path.join(pixmaps, "ebview.xpm"))

    print("wrote ebview.svg, %d PNG sizes, ebview.ico, ebview.xpm"
          % len(PNG_SIZES))


if __name__ == "__main__":
    main()
