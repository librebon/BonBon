#!/usr/bin/env python3
"""Independent numpy Eagle 2x (edge-clamped) reference: each output corner is the diagonal neighbour
when it equals both adjacent edge neighbours, else the centre pixel."""
import sys, numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGBA')).astype(np.uint32)
p = (a[..., 3] << 24) | (a[..., 0] << 16) | (a[..., 1] << 8) | a[..., 2]
q = np.pad(p, 1, mode='edge'); h, w = p.shape
s = lambda dy, dx: q[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
A, B, C, D, E, F, G, H, I = (s(-1, -1), s(-1, 0), s(-1, 1), s(0, -1), s(0, 0), s(0, 1), s(1, -1), s(1, 0), s(1, 1))
o = np.zeros((2 * h, 2 * w), np.uint32)
o[0::2, 0::2] = np.where((A == B) & (A == D), A, E)
o[0::2, 1::2] = np.where((C == B) & (C == F), C, E)
o[1::2, 0::2] = np.where((G == D) & (G == H), G, E)
o[1::2, 1::2] = np.where((I == F) & (I == H), I, E)
rgba = np.stack([(o >> 16) & 255, (o >> 8) & 255, o & 255, o >> 24], -1).astype(np.uint8)
Image.fromarray(rgba, 'RGBA').save(sys.argv[2])
