#!/usr/bin/env python3
"""Regenerate docs/themes.png: every built-in theme plus a few themes generated from a seed colour.

usage: tools/make-themes-png.py [build-dir] [out.png]
Needs Pillow and an offscreen-capable Qt build of Libreworks.
"""
import os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

build = sys.argv[1] if len(sys.argv) > 1 else "build"
out = sys.argv[2] if len(sys.argv) > 2 else "docs/themes.png"
themes = ["classic", "light", "dark", "midnight", "graphite", "studio8", "solarized-dark", "solarized-light",
          "nord", "synthwave", "pink", "pink-night", "high-contrast",
          "seed:#2e7d32:light:60", "seed:#1565c0:dark:70", "seed:#ff6f00:dark:100"]
label = {t: (f"generated from {t.split(':')[1].upper()} ({t.split(':')[2]}, vibrancy {t.split(':')[3]})" if t.startswith("seed:") else t) for t in themes}
COLS, TW, TH, PAD, LABEL = 4, 512, 320, 24, 30
env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
shots = []
with tempfile.TemporaryDirectory() as tmp:
    for i, t in enumerate(themes):
        p = os.path.join(tmp, f"{i}.png")
        subprocess.run([os.path.join(build, "libreworks"), "--sample", "--size", "1280x800", "--theme", t, "--screenshot", p],
                       check=True, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shots.append(Image.open(p).convert("RGB").resize((TW, TH), Image.LANCZOS))
rows = (len(themes) + COLS - 1) // COLS
sheet = Image.new("RGB", (COLS * TW + (COLS + 1) * PAD // 2, rows * (TH + LABEL) + (rows + 1) * PAD // 2), (24, 24, 26))
draw = ImageDraw.Draw(sheet)
try:
    font = ImageFont.truetype("DejaVuSans.ttf", 15)
except OSError:
    font = ImageFont.load_default()
for i, (t, im) in enumerate(zip(themes, shots)):
    x = PAD // 2 + (i % COLS) * (TW + PAD // 2)
    y = PAD // 2 + (i // COLS) * (TH + LABEL + PAD // 2)
    sheet.paste(im, (x, y))
    w = draw.textlength(label[t], font=font)
    draw.text((x + (TW - w) / 2, y + TH + 7), label[t], fill=(235, 235, 235), font=font)
sheet.save(out, optimize=True)
print(out, sheet.size)
