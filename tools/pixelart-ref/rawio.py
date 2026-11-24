"""PNG <-> raw (u32 w, u32 h, then w*h little-endian 0xAARRGGBB words) for the C++ and JS references."""
import struct, sys
from PIL import Image
def png2raw(png, raw):
    im = Image.open(png).convert('RGBA'); w, h = im.size
    with open(raw, 'wb') as f:
        f.write(struct.pack('<II', w, h))
        for r, g, b, a in (im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata()): f.write(struct.pack('<I', (a << 24) | (r << 16) | (g << 8) | b))
def raw2png(raw, png):
    with open(raw, 'rb') as f:
        w, h = struct.unpack('<II', f.read(8)); px = struct.unpack('<%dI' % (w * h), f.read(4 * w * h))
    im = Image.new('RGBA', (w, h))
    im.putdata([((p >> 16) & 255, (p >> 8) & 255, p & 255, p >> 24) for p in px]); im.save(png)
if __name__ == '__main__':
    {'png2raw': png2raw, 'raw2png': raw2png}[sys.argv[1]](sys.argv[2], sys.argv[3])
