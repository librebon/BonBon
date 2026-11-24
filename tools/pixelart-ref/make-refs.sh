#!/bin/sh
# Regenerates tests/data/pixelart from independent reference implementations. Not part of the build.
#   REF=<dir> must hold: sm_intern.h sm_sai.cpp sm_scale2x.{cpp,h} sm_scale3x.{cpp,h}   (ScummVM, GPL-3+:
#   graphics/scaler/{intern.h,sai.cpp,scale2x.*,scale3x.*}) and mmpx/code/js-demo.html (McGuire & Gagiu
#   MMPX supplement, MIT). Also needs python3 + Pillow + numpy, node, a C++ compiler and ffmpeg (hqx, xbr filters).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../../tests/data/pixelart}
REF=${REF:?set REF to the reference source directory}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
python3 "$HERE/sprites.py" "$OUT"
sed -n '1,/^void SAIScaler::scaleIntern/p' "$REF/sm_sai.cpp" | sed '$d' > "$TMP/sai_templates.inc"
c++ -O1 -std=c++17 -w -I"$HERE/shim" \
  -DSM_INTERN="\"$REF/sm_intern.h\"" -DSM_SCALE2X_H="\"$REF/sm_scale2x.h\"" -DSM_SCALE3X_H="\"$REF/sm_scale3x.h\"" \
  -DSM_SAI_TEMPLATES="\"$TMP/sai_templates.inc\"" \
  "$HERE/scummvm_ref.cpp" "$REF/sm_scale2x.cpp" "$REF/sm_scale3x.cpp" -o "$TMP/scummvm_ref"
for s in hero lines noise ramp alpha; do
  python3 "$HERE/rawio.py" png2raw "$OUT/$s.png" "$TMP/$s.raw"
  for m in scale2x scale3x scale4x; do
    "$TMP/scummvm_ref" $m "$TMP/$s.raw" "$TMP/o.raw"; python3 "$HERE/rawio.py" raw2png "$TMP/o.raw" "$OUT/${s}_$m.png"
  done
  if [ $s != alpha ]; then   # ScummVM's blends are only checked on opaque art
    for m in 2xsai super2xsai supereagle; do
      "$TMP/scummvm_ref" $m "$TMP/$s.raw" "$TMP/o.raw"; python3 "$HERE/rawio.py" raw2png "$TMP/o.raw" "$OUT/${s}_$m.png"
    done
    for n in 2 3 4; do
      ffmpeg -loglevel error -y -i "$OUT/$s.png" -vf "format=bgra,hqx=n=$n" "$TMP/h.png"
      python3 -c "from PIL import Image;Image.open('$TMP/h.png').convert('RGBA').save('$OUT/${s}_hq${n}x.png')"
      ffmpeg -loglevel error -y -i "$OUT/$s.png" -vf "format=rgb32,xbr=n=$n" "$TMP/x.png"
      python3 -c "from PIL import Image;Image.open('$TMP/x.png').convert('RGBA').save('$OUT/${s}_xbr${n}x.png')"
    done
  fi
  python3 "$HERE/eagle_ref.py" "$OUT/$s.png" "$OUT/${s}_eagle.png"
  node "$HERE/mmpx_ref.js" "$REF/mmpx/code/js-demo.html" "$TMP/$s.raw" "$TMP/o.raw"
  python3 "$HERE/rawio.py" raw2png "$TMP/o.raw" "$OUT/${s}_mmpx.png"
done
echo "references written to $OUT"
