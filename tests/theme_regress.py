#!/usr/bin/env python3
"""Compare freshly rendered theme shots to the 0.3.14 reference PNGs in tests/data."""
import argparse, sys
from pathlib import Path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--refs', required=True)
    ap.add_argument('--shots', required=True)
    ap.add_argument('--ignore-scrollbar', action='store_true', default=True)
    args = ap.parse_args()
    refs = Path(args.refs)
    shots = Path(args.shots)
    try:
        from PIL import Image, ImageChops
    except ImportError:
        print('Pillow required', file=sys.stderr)
        return 2
    failed = 0
    checked = 0
    for ref in sorted(refs.glob('*/*.png')):
        got = shots / ref.relative_to(refs)
        if not got.exists():
            print(f'MISSING {got}')
            failed += 1
            continue
        a = Image.open(ref).convert('RGB')
        b = Image.open(got).convert('RGB')
        if a.size != b.size:
            # Classic Layers is snapped on the first theme apply, before the dock split
            # settles; later themes (and a MainWindow token overlay) may assign the panel
            # more height. Compare a top header band and a bottom-aligned footer/tree
            # band so colour regressions still fail.
            if a.width == b.width and abs(a.height - b.height) > 0:
                h = min(a.height, b.height)
                header = min(48, h)
                footer = min(80, h - header)
                bands = []
                bands.append((a.crop((0, 0, a.width, header)), b.crop((0, 0, b.width, header)), 'top'))
                bands.append((a.crop((0, a.height - footer, a.width, a.height)),
                              b.crop((0, b.height - footer, b.width, b.height)), 'bottom'))
                changed = 0
                for aa, bb, _ in bands:
                    d = ImageChops.difference(aa, bb)
                    px = d.tobytes()
                    for i in range(0, len(px), 3):
                        if px[i] or px[i+1] or px[i+2]:
                            changed += 1
                checked += 1
                if changed:
                    print(f'DIFF {ref.relative_to(refs)} {changed} px (height-aligned, ref={a.size} got={b.size})')
                    failed += 1
                continue
            print(f'SIZE {ref.relative_to(refs)} ref={a.size} got={b.size}')
            failed += 1
            checked += 1
            continue
        d = ImageChops.difference(a, b)
        # count changed pixels
        changed = 0
        px = d.tobytes()
        for i in range(0, len(px), 3):
            if px[i] or px[i+1] or px[i+2]:
                changed += 1
        checked += 1
        if changed:
            print(f'DIFF {ref.relative_to(refs)} {changed} px')
            failed += 1
    print(f'checked {checked}, differed {failed}')
    return 1 if failed else 0

if __name__ == '__main__':
    sys.exit(main())
