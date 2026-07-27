#!/usr/bin/env python3
"""Render src/resource/tesseraemu-icon.svg into a macOS .icns.

    tools/icon/build-icon.py [--out src/resource/tesseraemu.icns]

The SVG is the design source of truth and this script is the only thing that turns it
into a binary, so the shipped icon is reproducible rather than an opaque blob whose
provenance nobody can check a year later.

Why not a real SVG rasteriser: none is installed. rsvg-convert, Inkscape and cairosvg are
all absent, and `qlmanage -t` (the native QuickLook path) hung rather than producing
output. So this parses the subset of SVG the icon actually uses -- rounded rects, one
linear and one radial gradient, group and element opacity -- rather than reimplementing
the drawing by hand, which would silently drift from the SVG the moment either changed.

Fitting to Apple's grid: macOS icons since Big Sur sit in a squircle occupying 824 of
1024px with transparent margin, and a full-bleed square renders visibly larger than its
neighbours in the Dock. Conveniently the artwork's own corner radius (114/512 = 0.2227)
is within 1% of Apple's proportion (185.4/824 = 0.2250), so fitting is a pure scale and
centre -- the shape needs no reshaping.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image, ImageDraw

SVG_NS = "{http://www.w3.org/2000/svg}"
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

CANVAS = 1024          # final icns canvas
BODY = 824             # Apple's icon body within that canvas
SS = 4                 # supersample factor; downsampled at the end for antialiasing


def parse_color(text, default_alpha=1.0):
    text = text.strip()
    m = re.fullmatch(r"#([0-9a-fA-F]{6})", text)
    if not m:
        raise ValueError(f"unsupported colour: {text}")
    v = int(m.group(1), 16)
    return np.array([(v >> 16) & 255, (v >> 8) & 255, v & 255, default_alpha * 255.0])


def read_stops(node):
    stops = []
    for s in node.findall(f"{SVG_NS}stop"):
        offset = float(s.get("offset", "0"))
        alpha = float(s.get("stop-opacity", "1"))
        stops.append((offset, parse_color(s.get("stop-color", "#000000"), alpha)))
    return sorted(stops, key=lambda p: p[0])


def interp_stops(t, stops):
    """t: HxW array in [0,1] -> HxWx4 RGBA float array."""
    out = np.zeros(t.shape + (4,), dtype=np.float64)
    offs = [o for o, _ in stops]
    for i in range(len(stops) - 1):
        o0, c0 = stops[i]
        o1, c1 = stops[i + 1]
        span = max(o1 - o0, 1e-9)
        m = (t >= o0) & (t <= o1)
        local = np.clip((t[m] - o0) / span, 0.0, 1.0)[:, None]
        out[m] = c0[None, :] * (1 - local) + c1[None, :] * local
    out[t < offs[0]] = stops[0][1]
    out[t > offs[-1]] = stops[-1][1]
    return out


def build_gradients(root, size, scale):
    """id -> HxWx4 RGBA float array covering the whole canvas."""
    grads = {}
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    # sample pixel centres, mapped back into SVG user units
    ux = (xx + 0.5) / scale
    uy = (yy + 0.5) / scale

    for node in root.iter():
        gid = node.get("id")
        if not gid:
            continue
        if node.tag == f"{SVG_NS}linearGradient":
            x1, y1 = float(node.get("x1", 0)), float(node.get("y1", 0))
            x2, y2 = float(node.get("x2", 0)), float(node.get("y2", 0))
            dx, dy = x2 - x1, y2 - y1
            denom = dx * dx + dy * dy
            t = np.clip(((ux - x1) * dx + (uy - y1) * dy) / max(denom, 1e-9), 0.0, 1.0)
            grads[gid] = interp_stops(t, read_stops(node))
        elif node.tag == f"{SVG_NS}radialGradient":
            # objectBoundingBox units; the icon's box is square so the radius maps
            # directly onto the side length.
            box = 512.0
            cx = float(node.get("cx", 0.5)) * box
            cy = float(node.get("cy", 0.5)) * box
            r = float(node.get("r", 0.5)) * box
            d = np.sqrt((ux - cx) ** 2 + (uy - cy) ** 2)
            grads[gid] = interp_stops(np.clip(d / max(r, 1e-9), 0.0, 1.0), read_stops(node))
    return grads


def rect_mask(size, scale, x, y, w, h, rx):
    img = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(img)
    box = [x * scale, y * scale, (x + w) * scale - 1, (y + h) * scale - 1]
    if rx > 0:
        d.rounded_rectangle(box, radius=rx * scale, fill=255)
    else:
        d.rectangle(box, fill=255)
    return np.asarray(img, dtype=np.float64) / 255.0


def render(svg_path):
    root = ET.parse(svg_path).getroot()
    size = BODY * SS
    scale = size / 512.0
    grads = build_gradients(root, size, scale)

    canvas = np.zeros((size, size, 4), dtype=np.float64)

    def draw(node, inherited_opacity, inherited_fill):
        op = float(node.get("opacity", "1")) * inherited_opacity
        # fill is inherited: the tile rects carry no fill of their own and take it from
        # the enclosing <g fill="url(#accent)">.
        fill = node.get("fill", inherited_fill)
        m = re.fullmatch(r"url\(#(.+)\)", fill or "")
        if not m or m.group(1) not in grads:
            raise ValueError(f"unsupported fill: {fill!r}")
        src = grads[m.group(1)]
        mask = rect_mask(size, scale,
                         float(node.get("x", 0)), float(node.get("y", 0)),
                         float(node.get("width")), float(node.get("height")),
                         float(node.get("rx", 0)))
        a = (src[:, :, 3] / 255.0) * mask * op          # source alpha
        # standard source-over compositing, premultiplied internally
        dst_a = canvas[:, :, 3] / 255.0
        out_a = a + dst_a * (1 - a)
        safe = np.where(out_a > 0, out_a, 1.0)
        for c in range(3):
            canvas[:, :, c] = (src[:, :, c] * a + canvas[:, :, c] * dst_a * (1 - a)) / safe
        canvas[:, :, 3] = out_a * 255.0

    def walk(node, opacity, fill):
        for child in node:
            if child.tag == f"{SVG_NS}g":
                walk(child,
                     opacity * float(child.get("opacity", "1")),
                     child.get("fill", fill))
            elif child.tag == f"{SVG_NS}rect":
                draw(child, opacity, fill)

    walk(root, 1.0, None)

    art = Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8), "RGBA")
    art = art.resize((BODY, BODY), Image.LANCZOS)

    out = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    off = (CANVAS - BODY) // 2
    out.paste(art, (off, off), art)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--svg", default=os.path.join(REPO, "src/resource/tesseraemu-icon.svg"))
    ap.add_argument("--out", default=os.path.join(REPO, "src/resource/tesseraemu.icns"))
    ap.add_argument("--png", help="also write the flat 1024 png here")
    args = ap.parse_args()

    master = render(args.svg)
    if args.png:
        master.save(args.png)

    tmp = tempfile.mkdtemp()
    iconset = os.path.join(tmp, "icon.iconset")
    os.makedirs(iconset)
    for base in (16, 32, 128, 256, 512):
        for scale_factor, suffix in ((1, ""), (2, "@2x")):
            px = base * scale_factor
            master.resize((px, px), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{base}x{base}{suffix}.png"))

    subprocess.run(["iconutil", "-c", "icns", iconset, "-o", args.out], check=True)
    shutil.rmtree(tmp)
    print(f"wrote {args.out} ({os.path.getsize(args.out):,} bytes)")


if __name__ == "__main__":
    sys.exit(main())
