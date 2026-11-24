#!/usr/bin/env python3
"""Deterministic test sprites for the pixel-art scaler reference tests (tests/data/pixelart)."""
import random, struct, sys, os
from PIL import Image

def hero():
    m = [
        "................",
        ".....KKKKKK.....",
        "....KRRRRRRK....",
        "...KRRRRRRRRK...",
        "...KSSSKSKSK....",
        "..KSKSSSKSSSK...",
        "..KSKKSSSKSSSK..",
        "..KKSSSSKKKKK...",
        "....SSSSSSSK....",
        "...KKRKKKRK.....",
        "..KKKRKKRKKKK...",
        ".KKKKRRRRKKKKK..",
        ".SSKRYRRYRKSS...",
        ".SSSRRRRRRSSS...",
        "...RRR..RRR.....",
        "..KKK....KKK....",
    ]
    pal = {'.': (120, 170, 255), 'K': (40, 30, 20), 'R': (210, 40, 40), 'S': (250, 200, 160), 'Y': (250, 230, 40)}
    img = Image.new('RGBA', (16, 16))
    for y, row in enumerate(m):
        for x, c in enumerate(row):
            img.putpixel((x, y), pal[c] + (255,))
    return img

def lines():
    w = h = 24
    img = Image.new('RGBA', (w, h), (255, 255, 255, 255))
    ink, ink2 = (0, 0, 0, 255), (30, 90, 200, 255)
    for i in range(24): img.putpixel((i, i), ink)                         # 1:1
    for i in range(24): img.putpixel((i, min(23, 12 + i // 2)), ink2)      # 2:1
    for i in range(12): img.putpixel((min(23, 2 + i // 2), i), ink2)       # 1:2
    import math
    for a in range(360):
        x = int(round(17 + 5 * math.cos(math.radians(a)))); y = int(round(6 + 5 * math.sin(math.radians(a))))
        img.putpixel((x, y), (200, 0, 0, 255))
    for y in range(16, 22):
        for x in range(2, 8): img.putpixel((x, y), (0, 160, 60, 255))
    return img

def noise():
    r = random.Random(1234)
    cols = [(0, 0, 0, 255), (255, 255, 255, 255), (230, 120, 20, 255)]
    img = Image.new('RGBA', (16, 16))
    for y in range(16):
        for x in range(16): img.putpixel((x, y), cols[r.randrange(3)])
    return img

def ramp():
    r = random.Random(99)
    img = Image.new('RGBA', (12, 12))
    for y in range(12):
        for x in range(12):
            base = (x // 3 * 60, y // 3 * 60, 255 - (x + y) * 9)
            if r.random() < 0.25: base = (r.randrange(256), r.randrange(256), r.randrange(256))
            img.putpixel((x, y), base + (255,))
    return img

def alpha():
    img = Image.new('RGBA', (16, 16), (0, 0, 0, 0))
    for y in range(16):
        for x in range(16):
            d = (x - 7.5) ** 2 + (y - 7.5) ** 2
            if d < 36: img.putpixel((x, y), (250, 210, 30, 255))
            if 30 <= d < 42: img.putpixel((x, y), (60, 30, 0, 255))
    for p in [(5, 6), (10, 6), (6, 10), (7, 11), (8, 11), (9, 10)]: img.putpixel(p, (60, 30, 0, 255))
    return img

SPRITES = {'hero': hero, 'lines': lines, 'noise': noise, 'ramp': ramp, 'alpha': alpha}

if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for n, f in SPRITES.items():
        f().save(os.path.join(out, n + '.png'))
