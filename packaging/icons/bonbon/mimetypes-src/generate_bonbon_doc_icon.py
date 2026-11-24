#!/usr/bin/env python3
"""Procedural thin-border pink Bonbon .bon document (MIME) icon.

Rebuilds each size from the original 885d770 silhouette and fold geometry.
Does not recolour the original raster (that left highlight leftovers and
inner rings). Page interior is a clean pink gradient + sheen, running
uninterrupted up to a single 1px #C46476 border. The fold flap is #FFD6DE
with its own 1px border. Bn is composited last.

Run from this directory (or anywhere). Writes preview/QA images here and
installs the size PNGs into ../mimetypes/<size>/.
"""
from __future__ import annotations

import io
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
# packaging/icons/bonbon/mimetypes-src/ → repo root
REPO = ROOT.parents[3]
DEST = ROOT.parent / "mimetypes"
BN_GLYPH = ROOT / "bn-glyph.png"
BN_GLYPH_FALLBACK = REPO / "packaging" / "icons" / "bonbon" / "mimetypes-src" / "bn-glyph.png"
ORIG_COMMIT = "885d770"
SIZES = (16, 22, 24, 32, 48, 64, 128, 256)

PINK_TOP = np.array([0xFB, 0xBC, 0xC7], dtype=np.float32)  # #FBBCC7
PINK_BOT = np.array([0xF2, 0x94, 0xA6], dtype=np.float32)  # #F294A6
PINK_BORDER = np.array([0xC4, 0x64, 0x76], dtype=np.uint8)  # #C46476
PINK_FOLD = np.array([0xFF, 0xD6, 0xDE], dtype=np.uint8)  # #FFD6DE
GLOSS = np.array([0xFF, 0xFA, 0xFC], dtype=np.float32)

BN_FRAC = {
    16: 0.72, 22: 0.72, 24: 0.72,
    32: 0.62, 48: 0.62,
    64: 0.58, 128: 0.58, 256: 0.58,
}

# Page-interior pixels must match gradient+sheen within this many levels / channel
FILL_TOL = 2


def load_original(size: int) -> Image.Image:
    path = f"packaging/icons/bonbon/mimetypes/{size}x{size}/application-x-bonbon.png"
    data = subprocess.check_output(
        ["git", "-C", str(REPO), "show", f"{ORIG_COMMIT}:{path}"]
    )
    return Image.open(io.BytesIO(data)).convert("RGBA")


def page_colour_lut(h: int) -> np.ndarray:
    """Return (h, 3) float array: gradient + sheen for each y."""
    y = np.arange(h, dtype=np.float32)
    t = y / max(1, h - 1)
    t = t * t * (3.0 - 2.0 * t)  # smoothstep
    fill = PINK_TOP[None, :] * (1.0 - t)[:, None] + PINK_BOT[None, :] * t[:, None]
    sheen = np.clip(1.0 - (y / max(1, h)) / 0.40, 0.0, 1.0) ** 1.4 * 0.22
    out = fill * (1.0 - sheen)[:, None] + GLOSS[None, :] * sheen[:, None]
    return out


def silhouette_and_fold(orig: Image.Image):
    """Binary silhouette and fold geometry from the original icon."""
    a = np.array(orig)
    mask = a[:, :, 3] > 128
    ys, xs = np.where(mask)
    if not len(xs):
        raise RuntimeError("empty silhouette")
    x0, y0 = int(xs.min()), int(ys.min())
    x1, y1 = int(xs.max()), int(ys.max())
    top = np.where(mask[y0])[0]
    crease_x = int(top[-1])  # last opaque pixel on the top edge
    fold_bottom_y = y1
    for y in range(y0, y1 + 1):
        row = np.where(mask[y])[0]
        if len(row) and int(row[-1]) >= x1:
            fold_bottom_y = y
            break
    return mask, (x0, y0, x1, y1, crease_x, fold_bottom_y)


def edge4(mask: np.ndarray) -> np.ndarray:
    """1px 4-connected outline: opaque pixels with a transparent 4-neighbour."""
    p = np.pad(mask, 1, constant_values=False)
    interior = p[0:-2, 1:-1] & p[2:, 1:-1] & p[1:-1, 0:-2] & p[1:-1, 2:]
    return mask & ~interior


def render_page(size: int) -> tuple[Image.Image, dict]:
    orig = load_original(size)
    mask, geom = silhouette_and_fold(orig)
    x0, y0, x1, y1, crease_x, fold_bottom_y = geom
    h, w = mask.shape
    yy, xx = np.indices((h, w))

    sil_border = edge4(mask)

    # Fold flap sits to the right of the vertical crease, above the
    # horizontal fold-bottom, inside the silhouette.
    fold_zone = mask & (xx > crease_x) & (yy < fold_bottom_y)
    crease_col = mask & (xx == crease_x) & (yy >= y0) & (yy <= fold_bottom_y)
    fold_bottom = mask & (yy == fold_bottom_y) & (xx >= crease_x) & (xx <= x1)
    fold_border = crease_col | fold_bottom
    all_border = sil_border | fold_border
    fold_fill = fold_zone & ~all_border
    page_fill = mask & ~all_border & ~fold_zone

    lut = page_colour_lut(h)  # (h, 3)
    lut_u8 = np.clip(np.rint(lut), 0, 255).astype(np.uint8)

    out = np.zeros((h, w, 4), dtype=np.uint8)

    # Page interior: exact gradient+sheen, full alpha
    out[page_fill, :3] = lut_u8[yy[page_fill]]
    out[page_fill, 3] = 255

    # Fold flap: flat lighter pink
    out[fold_fill, :3] = PINK_FOLD
    out[fold_fill, 3] = 255

    # Single 1px rose border (silhouette + fold's own crease/bottom)
    out[all_border, :3] = PINK_BORDER
    out[all_border, 3] = 255

    # Antialias only the diagonal fold outer edge, and only at larger sizes
    # so 16–32 stay crisp 1px stairsteps.
    if size >= 48:
        _aa_diagonal(out, mask, sil_border, geom)

    pre_bn = Image.fromarray(out, "RGBA")
    img = place_bn(pre_bn, size, geom)
    meta = {
        "geom": geom,
        "page_fill": page_fill,
        "fold_fill": fold_fill,
        "all_border": all_border,
        "lut_u8": lut_u8,
        "mask": mask,
        "pre_bn": pre_bn,
    }
    return img, meta


def _aa_diagonal(out: np.ndarray, mask: np.ndarray, sil_border: np.ndarray, geom):
    """Soften only silhouette-edge pixels on the 45° fold cut.

    Does not add pixels outside the original mask or punch holes in the
    interior. Coverage is based on distance to the ideal diagonal line.
    """
    x0, y0, x1, y1, crease_x, fold_bottom_y = geom
    h, w = mask.shape
    # The outer diagonal runs from (crease_x, y0) to (x1, fold_bottom_y).
    # On these icons it is 45°: x - y = crease_x - y0.
    # Inside (page/fold) is x - y <= crease_x - y0 (more left / down).
    c = crease_x - y0
    # Restrict to the fold's outer edge (not the bottom or left of the page)
    yy, xx = np.indices((h, w))
    diag = sil_border & (yy > y0) & (yy < fold_bottom_y) & (xx >= crease_x)
    if not diag.any():
        return
    # Signed distance to the line x - y - c = 0, in pixel units
    # (normalised by sqrt(2) so it's Euclidean).
    dist = (xx.astype(np.float32) - yy.astype(np.float32) - c) / np.sqrt(2.0)
    # Coverage of the document in this pixel: 1 inside, 0 outside.
    # A 1px-wide linear filter: coverage = 0.5 - dist.
    coverage = np.clip(0.5 - dist, 0.0, 1.0)
    # Only touch diagonal edge pixels; keep them rose, vary alpha.
    ys, xs = np.where(diag)
    for y, x in zip(ys, xs):
        cov = float(coverage[y, x])
        if cov >= 0.999:
            continue
        if cov <= 0.001:
            # would be outside — keep a faint rose so the stairstep
            # doesn't leave a hole; actually leave as solid 1px.
            continue
        # Partial rose on transparency: no premultiplication issues if
        # RGB stays pure border.
        out[y, x, :3] = PINK_BORDER
        out[y, x, 3] = int(round(255 * cov))


def load_bn() -> Image.Image:
    p = BN_GLYPH if BN_GLYPH.exists() else BN_GLYPH_FALLBACK
    return Image.open(p).convert("RGBA")


def place_bn(page: Image.Image, size: int, geom) -> Image.Image:
    x0, y0, x1, y1, crease_x, fold_bottom_y = geom
    page_w = x1 - x0 + 1
    page_h = y1 - y0 + 1
    fold = x1 - crease_x
    bn = load_bn()
    tw = max(2, int(round(page_w * BN_FRAC[size])))
    th = max(2, int(round(bn.height * (tw / bn.width))))
    bn = bn.resize((tw, th), Image.Resampling.LANCZOS)
    cx = x0 + page_w // 2 - fold // 12
    cy = y0 + int(page_h * 0.52)
    out = page.copy()
    out.alpha_composite(bn, (cx - bn.width // 2, cy - bn.height // 2))
    return out


def check_fill(meta: dict, size: int) -> list[str]:
    """On the pre-Bn canvas, every page-interior pixel must match the
    gradient+sheen, including the first pixel inside the 1px border.
    """
    a = np.array(meta["pre_bn"])
    page_fill = meta["page_fill"]
    lut = meta["lut_u8"]
    h, w = page_fill.shape
    yy = np.indices((h, w))[0]
    expected = lut[yy]
    got = a[:, :, :3]
    diff = np.max(np.abs(got.astype(np.int16) - expected.astype(np.int16)), axis=2)
    issues = []
    mismatch = page_fill & (diff > FILL_TOL)
    n = int(mismatch.sum())
    if n:
        ys, xs = np.where(mismatch)
        samples = ", ".join(
            f"({int(x)},{int(y)}) got={tuple(int(v) for v in a[y, x])} exp={tuple(int(v) for v in expected[y, x])}"
            for x, y in list(zip(xs, ys))[:8]
        )
        issues.append(f"size {size}: {n} page pixels off the fill: {samples}")
    return issues


def make_preview_256(icon: Image.Image):
    icon.save(ROOT / "preview-256.png")


def zoom(im: Image.Image, box, scale: int, bg=(36, 36, 40, 255)) -> Image.Image:
    crop = im.crop(box)
    mag = crop.resize(
        ((box[2] - box[0]) * scale, (box[3] - box[1]) * scale),
        Image.Resampling.NEAREST,
    )
    out = Image.new("RGBA", mag.size, bg)
    out.alpha_composite(mag)
    return out.convert("RGB")


def make_edge_zoom(icons: dict[int, Image.Image]):
    """After row only: 128 corners at 4x, 32 at 8x, 256 TR quadrant at 3x."""
    im128 = icons[128]
    im32 = icons[32]
    im256 = icons[256]
    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16
        )
        font_sm = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 13
        )
    except Exception:
        font = font_sm = ImageFont.load_default()

    parts = [
        ("128 TL 4×", zoom(im128, (0, 0, 40, 40), 4)),
        ("128 TR fold 4×", zoom(im128, (88, 0, 128, 40), 4)),
        ("128 BL 4×", zoom(im128, (0, 88, 40, 128), 4)),
        ("32 8×", zoom(im32, (0, 0, 32, 32), 8)),
        ("32 8× on green", zoom(im32, (0, 0, 32, 32), 8, bg=(0, 160, 80, 255))),
        # 256 top-right quadrant (fold + former leftover line y≈70–75, x≈145+)
        ("256 TR quadrant 3×", zoom(im256, (128, 0, 256, 128), 3)),
    ]
    gap = 16
    label_h = 22
    title_h = 40
    W = sum(p.width for _, p in parts) + gap * (len(parts) + 1) + 8
    H = title_h + label_h + max(p.height for _, p in parts) + 24
    sheet = Image.new("RGB", (W, H), (20, 20, 24))
    d = ImageDraw.Draw(sheet)
    d.text(
        (16, 10),
        "Bonbon .bon thin icon  —  edges (procedural fill, single 1px rose border)",
        fill=(220, 220, 224),
        font=font,
    )
    x = gap
    y = title_h
    for lab, p in parts:
        d.text((x, y), lab, fill=(190, 190, 194), font=font_sm)
        sheet.paste(p, (x, y + label_h))
        x += p.width + gap
    path = ROOT / "edge-zoom.png"
    sheet.save(path)
    print("wrote", path, sheet.size)


def make_qa_tiny(icons: dict[int, Image.Image]):
    sheet = Image.new("RGB", (900, 280), (40, 40, 44))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 13
        )
    except Exception:
        font = ImageFont.load_default()
    x = 10
    for s in (16, 22, 24, 32):
        im = icons[s]
        mag = im.resize((s * 8, s * 8), Image.Resampling.NEAREST)
        bg = Image.new("RGBA", mag.size, (0, 160, 80, 255))
        bg.alpha_composite(mag)
        sheet.paste(bg.convert("RGB"), (x, 28))
        d.text((x, 8), f"{s}px ×8", fill=(200, 200, 200), font=font)
        x += mag.size[0] + 16
    sheet.save(ROOT / "tiny-magnify.png")


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    icons = {}
    all_issues = []
    for s in SIZES:
        img, meta = render_page(s)
        d = ROOT / f"{s}x{s}"
        d.mkdir(exist_ok=True)
        path = d / "application-x-bonbon.png"
        img.save(path, optimize=True)
        dest_dir = DEST / f"{s}x{s}"
        dest_dir.mkdir(parents=True, exist_ok=True)
        img.save(dest_dir / "application-x-bonbon.png", optimize=True)
        icons[s] = img

        a = np.array(img)
        op = a[:, :, 3] > 128
        orig = np.array(load_original(s))
        oop = orig[:, :, 3] > 128
        ys, xs = np.where(op)
        oys, oxs = np.where(oop)
        issues = check_fill(meta, s)
        all_issues.extend(issues)
        x0, y0, x1, y1, crease_x, fold_bottom_y = meta["geom"]
        print(
            f"{s:3d}: opaque={int(op.sum()):5d} (orig {int(oop.sum()):5d})  "
            f"bbox {int(xs.min())},{int(ys.min())}-{int(xs.max())},{int(ys.max())}  "
            f"crease_x={crease_x} fold_bottom_y={fold_bottom_y}  "
            f"page={int(meta['page_fill'].sum())} fold={int(meta['fold_fill'].sum())} "
            f"border={int(meta['all_border'].sum())}  "
            f"{'OK' if not issues else 'ISSUES: ' + '; '.join(issues)}"
        )

    make_preview_256(icons[256])
    make_edge_zoom(icons)
    make_qa_tiny(icons)
    print("wrote", ROOT / "preview-256.png")
    if all_issues:
        print("FILL CHECK FAILED:")
        for i in all_issues:
            print(" ", i)
        raise SystemExit(1)
    print("all fill checks passed")


if __name__ == "__main__":
    main()
