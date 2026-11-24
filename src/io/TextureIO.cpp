#include "Brand.h"
// GPU texture containers.
// DDS: legacy DX9 headers (bit masks: RGB/RGBA/luminance/alpha, 8..32 bits), FourCC DXT1-5, ATI1/ATI2,
// BC4U/S, BC5U/S, D3DFMT float/16-bit codes, and the DX10 extension (DXGI formats incl. BC6H/BC7,
// sRGB variants, float, 10:10:10:2, 5:6:5, 5:5:5:1, 4:4:4:4). Cube faces, array layers and volume
// slices become frames (top mip level only). Writes BGRA8, BC1 (DXT1) or BC3 (DXT5), optional mips.
// KTX 2: vkFormats R8/RG8/RGB8/BGR8/RGBA8/BGRA8 (UNORM + SRGB), RGBA16F/32F, BC1-BC7; supercompression
// none / Zstandard / zlib. BasisLZ and UASTC (Basis Universal) are not supported (no transcoder).
// Writes RGBA8 / BC1 / BC3 (sRGB) with a Khronos Data Format Descriptor, optional mips and zstd.
// Block decoding: bcdec (third_party, MIT / Unlicense). Block encoding: own range-fit BC1/BC3 encoder.
#include "Codecs.h"
#include <QPainter>
#include <cmath>
#include <cstring>
#include <zlib.h>
#ifdef LW_HAVE_ZSTD
#include <zstd.h>
#endif
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "bcdec/bcdec.h"

namespace codec {

namespace {
inline quint32 le32(const uchar *p) { return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24; }
inline quint64 le64(const uchar *p) { return quint64(le32(p)) | quint64(le32(p + 4)) << 32; }
void put16(QByteArray &b, quint16 v) { b.append(char(v)); b.append(char(v >> 8)); }
void put32(QByteArray &b, quint32 v) { b.append(char(v)); b.append(char(v >> 8)); b.append(char(v >> 16)); b.append(char(v >> 24)); }
void put64(QByteArray &b, quint64 v) { put32(b, quint32(v)); put32(b, quint32(v >> 32)); }
constexpr quint32 fourcc(const char *s) { return quint32(uchar(s[0])) | quint32(uchar(s[1])) << 8 | quint32(uchar(s[2])) << 16 | quint32(uchar(s[3])) << 24; }

float halfToFloat(quint16 h) {
    const quint32 s = quint32(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    quint32 f;
    if (e == 0) {
        if (m == 0) f = s;
        else { int ee = -1; quint32 mm = m; do { ++ee; mm <<= 1; } while (!(mm & 0x400)); f = s | quint32(127 - 15 - ee) << 23 | (mm & 0x3FF) << 13; }
    } else if (e == 31) f = s | 0x7F800000 | m << 13;
    else f = s | (e + 127 - 15) << 23 | m << 13;
    float r; std::memcpy(&r, &f, 4); return r;
}

enum class TF { Unknown, RGBA8, BGRA8, BGRX8, RGB8, BGR8, R8, RG8, A8, L8, LA8, BC1, BC2, BC3, BC4U, BC4S, BC5U, BC5S, BC6HU, BC6HS, BC7,
                RGBA16F, RGBA32F, RGBA16, R16F, R32F, RG16F, RG32F, RGB32F, RGB10A2, B5G6R5, B5G5R5A1, B4G4R4A4, Masked };
struct Masks { int bits = 32; quint32 r = 0, g = 0, b = 0, a = 0; bool lum = false; };

bool isBlock(TF f) { return f >= TF::BC1 && f <= TF::BC7; }
bool isFloat(TF f) { return f == TF::BC6HU || f == TF::BC6HS || f == TF::RGBA16F || f == TF::RGBA32F || f == TF::R16F || f == TF::R32F || f == TF::RG16F || f == TF::RG32F || f == TF::RGB32F; }
int blockBytes(TF f) { return (f == TF::BC1 || f == TF::BC4U || f == TF::BC4S) ? 8 : 16; }
int pixelBytes(TF f, const Masks &m) {
    switch (f) {
    case TF::R8: case TF::A8: case TF::L8: return 1;
    case TF::RG8: case TF::LA8: case TF::R16F: case TF::B5G6R5: case TF::B5G5R5A1: case TF::B4G4R4A4: return 2;
    case TF::RGB8: case TF::BGR8: return 3;
    case TF::RGBA16F: case TF::RGBA16: case TF::RG32F: return 8;
    case TF::RGBA32F: return 16;
    case TF::RGB32F: return 12;
    case TF::Masked: return (m.bits + 7) / 8;
    default: return 4;
    }
}
qint64 surfaceBytes(TF f, const Masks &m, int w, int h) {
    if (isBlock(f)) return qint64(qMax(1, (w + 3) / 4)) * qMax(1, (h + 3) / 4) * blockBytes(f);
    return qint64(w) * h * pixelBytes(f, m);
}
int maskGet(quint32 v, quint32 mask) {
    if (!mask) return -1;
    int shift = 0, bits = 0;
    while (!((mask >> shift) & 1)) ++shift;
    while (shift + bits < 32 && ((mask >> (shift + bits)) & 1)) ++bits;
    const quint32 x = (v & mask) >> shift;
    if (bits >= 8) return int(x >> (bits - 8));
    const quint32 mx = (1u << bits) - 1;
    return int((x * 255 + mx / 2) / mx);
}

// decode one surface: LDR into img (ARGB32) or HDR into hdr (linear float RGBA)
bool decodeSurface(TF f, const Masks &m, const uchar *p, int w, int h, QImage &img, HdrImage &hdr) {
    if (isFloat(f)) {
        hdr.width = w; hdr.height = h;
        hdr.rgba.assign(qsizetype(w) * h * 4, 1.0f);
        if (f == TF::BC6HU || f == TF::BC6HS) {
            const int bw = (w + 3) / 4, bh = (h + 3) / 4;
            float blk[16 * 3];
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx) {
                    bcdec_bc6h_float(p + (qint64(by) * bw + bx) * 16, blk, 4 * 3, f == TF::BC6HS);
                    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                        const int px = bx * 4 + x, py = by * 4 + y;
                        if (px >= w || py >= h) continue;
                        float *o = hdr.rgba.data() + (qsizetype(py) * w + px) * 4;
                        o[0] = blk[(y * 4 + x) * 3]; o[1] = blk[(y * 4 + x) * 3 + 1]; o[2] = blk[(y * 4 + x) * 3 + 2];
                    }
                }
            return true;
        }
        const int nch = f == TF::R16F || f == TF::R32F ? 1 : (f == TF::RG16F || f == TF::RG32F) ? 2 : f == TF::RGB32F ? 3 : 4;
        const bool half = f == TF::RGBA16F || f == TF::R16F || f == TF::RG16F;
        for (qint64 i = 0; i < qint64(w) * h; ++i) {
            float v[4] = {0, 0, 0, 1};
            for (int c = 0; c < nch; ++c) {
                if (half) v[c] = halfToFloat(quint16(p[(i * nch + c) * 2] | p[(i * nch + c) * 2 + 1] << 8));
                else { const quint32 u = le32(p + (i * nch + c) * 4); std::memcpy(&v[c], &u, 4); }
                if (!std::isfinite(v[c])) v[c] = 0;
            }
            if (nch == 1) v[1] = v[2] = v[0];
            float *o = hdr.rgba.data() + i * 4;
            o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; o[3] = qBound(0.0f, v[3], 1.0f);
        }
        return true;
    }
    img = QImage(w, h, QImage::Format_ARGB32);
    if (isBlock(f)) {
        const int bw = (w + 3) / 4, bh = (h + 3) / 4;
        uchar blk[16 * 4];
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const uchar *s = p + (qint64(by) * bw + bx) * blockBytes(f);
                std::memset(blk, 255, sizeof blk);
                switch (f) {
                case TF::BC1: bcdec_bc1(s, blk, 16); break;
                case TF::BC2: bcdec_bc2(s, blk, 16); break;
                case TF::BC3: bcdec_bc3(s, blk, 16); break;
                case TF::BC7: bcdec_bc7(s, blk, 16); break;
                case TF::BC4U: case TF::BC4S: {
                    uchar r[16]; bcdec_bc4(s, r, 4, f == TF::BC4S);
                    for (int i = 0; i < 16; ++i) { int v = f == TF::BC4S ? (int(static_cast<signed char>(r[i])) + 127) * 255 / 254 : r[i]; blk[i * 4] = blk[i * 4 + 1] = blk[i * 4 + 2] = uchar(v); }
                    break;
                }
                default: {   // BC5: RG normal map, reconstruct B = z
                    uchar rg[32]; bcdec_bc5(s, rg, 8, f == TF::BC5S);
                    for (int i = 0; i < 16; ++i) {
                        int r = rg[i * 2], g = rg[i * 2 + 1];
                        if (f == TF::BC5S) { r = (int(static_cast<signed char>(r)) + 127) * 255 / 254; g = (int(static_cast<signed char>(g)) + 127) * 255 / 254; }
                        const double nx = r / 127.5 - 1, ny = g / 127.5 - 1;
                        const double nz = std::sqrt(qMax(0.0, 1 - nx * nx - ny * ny));
                        blk[i * 4] = uchar(r); blk[i * 4 + 1] = uchar(g); blk[i * 4 + 2] = uchar(qBound(0.0, (nz + 1) * 127.5, 255.0));
                    }
                }
                }
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                    const int px = bx * 4 + x, py = by * 4 + y;
                    if (px >= w || py >= h) continue;
                    const uchar *c = blk + (y * 4 + x) * 4;
                    reinterpret_cast<QRgb *>(img.scanLine(py))[px] = qRgba(c[0], c[1], c[2], c[3]);
                }
            }
        return true;
    }
    const int pb = pixelBytes(f, m);
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const uchar *s = p + (qint64(y) * w + x) * pb;
            switch (f) {
            case TF::RGBA8: d[x] = qRgba(s[0], s[1], s[2], s[3]); break;
            case TF::BGRA8: d[x] = qRgba(s[2], s[1], s[0], s[3]); break;
            case TF::BGRX8: d[x] = qRgb(s[2], s[1], s[0]); break;
            case TF::RGB8: d[x] = qRgb(s[0], s[1], s[2]); break;
            case TF::BGR8: d[x] = qRgb(s[2], s[1], s[0]); break;
            case TF::R8: d[x] = qRgb(s[0], 0, 0); break;
            case TF::L8: d[x] = qRgb(s[0], s[0], s[0]); break;
            case TF::A8: d[x] = qRgba(0, 0, 0, s[0]); break;
            case TF::LA8: d[x] = qRgba(s[0], s[0], s[0], s[1]); break;
            case TF::RG8: d[x] = qRgb(s[0], s[1], 0); break;
            case TF::RGBA16: d[x] = qRgba(s[1], s[3], s[5], s[7]); break;
            case TF::RGB10A2: { const quint32 v = le32(s); d[x] = qRgba(int((v & 1023) >> 2), int(((v >> 10) & 1023) >> 2), int(((v >> 20) & 1023) >> 2), int((v >> 30) * 85)); break; }
            case TF::B5G6R5: { const quint32 v = quint32(s[0] | s[1] << 8); d[x] = qRgb(maskGet(v, 0xF800), maskGet(v, 0x07E0), maskGet(v, 0x001F)); break; }
            case TF::B5G5R5A1: { const quint32 v = quint32(s[0] | s[1] << 8); d[x] = qRgba(maskGet(v, 0x7C00), maskGet(v, 0x03E0), maskGet(v, 0x001F), v & 0x8000 ? 255 : 0); break; }
            case TF::B4G4R4A4: { const quint32 v = quint32(s[0] | s[1] << 8); d[x] = qRgba(maskGet(v, 0x0F00), maskGet(v, 0x00F0), maskGet(v, 0x000F), maskGet(v, 0xF000)); break; }
            default: {   // Masked
                quint32 v = 0;
                for (int i = 0; i < pb; ++i) v |= quint32(s[i]) << (8 * i);
                const int r = maskGet(v, m.r), g = maskGet(v, m.g), b = maskGet(v, m.b), a = maskGet(v, m.a);
                if (m.lum) d[x] = qRgba(r, r, r, a < 0 ? 255 : a);
                else d[x] = qRgba(r < 0 ? 0 : r, g < 0 ? 0 : g, b < 0 ? 0 : b, a < 0 ? 255 : a);
            }
            }
        }
    }
    return true;
}

TF dxgiFormat(quint32 f) {
    switch (f) {
    case 2: return TF::RGBA32F;
    case 6: return TF::RGB32F;
    case 10: return TF::RGBA16F;
    case 11: return TF::RGBA16;
    case 16: return TF::RG32F;
    case 24: return TF::RGB10A2;
    case 28: case 29: return TF::RGBA8;
    case 34: return TF::RG16F;
    case 41: return TF::R32F;
    case 49: return TF::RG8;
    case 54: return TF::R16F;
    case 61: return TF::R8;
    case 65: return TF::A8;
    case 70: case 71: case 72: return TF::BC1;
    case 73: case 74: case 75: return TF::BC2;
    case 76: case 77: case 78: return TF::BC3;
    case 79: case 80: return TF::BC4U;
    case 81: return TF::BC4S;
    case 82: case 83: return TF::BC5U;
    case 84: return TF::BC5S;
    case 85: return TF::B5G6R5;
    case 86: return TF::B5G5R5A1;
    case 87: case 90: case 91: return TF::BGRA8;
    case 88: case 92: case 93: return TF::BGRX8;
    case 94: case 95: return TF::BC6HU;
    case 96: return TF::BC6HS;
    case 97: case 98: case 99: return TF::BC7;
    case 115: return TF::B4G4R4A4;
    default: return TF::Unknown;
    }
}

TF vkFormat(quint32 f, bool &srgb) {
    srgb = false;
    switch (f) {
    case 9: return TF::R8;
    case 15: srgb = true; return TF::R8;
    case 16: return TF::RG8;
    case 22: srgb = true; return TF::RG8;
    case 23: return TF::RGB8;
    case 29: srgb = true; return TF::RGB8;
    case 30: return TF::BGR8;
    case 36: srgb = true; return TF::BGR8;
    case 37: return TF::RGBA8;
    case 43: srgb = true; return TF::RGBA8;
    case 44: return TF::BGRA8;
    case 50: srgb = true; return TF::BGRA8;
    case 91: return TF::RGBA16;
    case 97: return TF::RGBA16F;
    case 109: return TF::RGBA32F;
    case 131: case 133: return TF::BC1;
    case 132: case 134: srgb = true; return TF::BC1;
    case 135: return TF::BC2;
    case 136: srgb = true; return TF::BC2;
    case 137: return TF::BC3;
    case 138: srgb = true; return TF::BC3;
    case 139: return TF::BC4U;
    case 140: return TF::BC4S;
    case 141: return TF::BC5U;
    case 142: return TF::BC5S;
    case 143: return TF::BC6HU;
    case 144: return TF::BC6HS;
    case 145: return TF::BC7;
    case 146: srgb = true; return TF::BC7;
    default: return TF::Unknown;
    }
}

QImage toLdr(const HdrImage &h) { return ImageIO::applyToneMap(h, ImageIO::toneMap()); }

// ---------------------------------------------------------------- BC1 / BC3 encoder
inline quint16 to565(const float *c) {
    const int r = qBound(0, int(std::lround(c[0] * 31 / 255.0f)), 31), g = qBound(0, int(std::lround(c[1] * 63 / 255.0f)), 63), b = qBound(0, int(std::lround(c[2] * 31 / 255.0f)), 31);
    return quint16(r << 11 | g << 5 | b);
}
inline void from565(quint16 v, int *c) {
    c[0] = ((v >> 11) & 31) * 255 / 31; c[1] = ((v >> 5) & 63) * 255 / 63; c[2] = (v & 31) * 255 / 31;
}
void encodeColorBlock(const QRgb *px, uchar *out, bool allowPunch) {
    bool punch = false;
    float mean[3] = {0, 0, 0};
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        if (allowPunch && qAlpha(px[i]) < 128) { punch = true; continue; }
        mean[0] += qRed(px[i]); mean[1] += qGreen(px[i]); mean[2] += qBlue(px[i]); ++n;
    }
    quint16 c0 = 0, c1 = 0;
    quint32 idx = 0;
    if (n == 0) {   // fully transparent: 3-colour mode, everything index 3
        idx = 0xFFFFFFFFu;
        out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
        std::memcpy(out + 4, &idx, 4);
        return;
    }
    for (float &m : mean) m /= float(n);
    // principal axis by power iteration on the covariance
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        if (allowPunch && qAlpha(px[i]) < 128) continue;
        const float r = qRed(px[i]) - mean[0], g = qGreen(px[i]) - mean[1], b = qBlue(px[i]) - mean[2];
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
    }
    float ax[3] = {1, 1, 1};
    for (int it = 0; it < 8; ++it) {
        const float x = cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2];
        const float y = cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2];
        const float z = cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2];
        const float l = std::sqrt(x * x + y * y + z * z);
        if (l < 1e-6f) break;
        ax[0] = x / l; ax[1] = y / l; ax[2] = z / l;
    }
    float mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < 16; ++i) {
        if (allowPunch && qAlpha(px[i]) < 128) continue;
        const float t = (qRed(px[i]) - mean[0]) * ax[0] + (qGreen(px[i]) - mean[1]) * ax[1] + (qBlue(px[i]) - mean[2]) * ax[2];
        mn = qMin(mn, t); mx = qMax(mx, t);
    }
    const float inset = (mx - mn) / 16.0f;
    mn += inset; mx -= inset;
    float e0[3], e1[3];
    for (int c = 0; c < 3; ++c) { e0[c] = qBound(0.0f, mean[c] + ax[c] * mx, 255.0f); e1[c] = qBound(0.0f, mean[c] + ax[c] * mn, 255.0f); }
    c0 = to565(e0); c1 = to565(e1);
    if (punch) { if (c0 > c1) std::swap(c0, c1); }        // c0 <= c1: 3 colours + transparent
    else {
        if (c0 < c1) std::swap(c0, c1);
        if (c0 == c1) {   // flat block: 4-colour mode needs c0 > c1
            if (c0 < 0xFFFF) ++c0; else --c1;
        }
    }
    int a[3], b[3], pal[4][3];
    from565(c0, a); from565(c1, b);
    for (int c = 0; c < 3; ++c) {
        pal[0][c] = a[c]; pal[1][c] = b[c];
        if (c0 > c1) { pal[2][c] = (2 * a[c] + b[c]) / 3; pal[3][c] = (a[c] + 2 * b[c]) / 3; }
        else { pal[2][c] = (a[c] + b[c]) / 2; pal[3][c] = 0; }
    }
    const int ncol = c0 > c1 ? 4 : 3;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        if (punch && qAlpha(px[i]) < 128) best = 3;
        else {
            int bestD = INT_MAX;
            for (int k = 0; k < ncol; ++k) {
                const int dr = qRed(px[i]) - pal[k][0], dg = qGreen(px[i]) - pal[k][1], db = qBlue(px[i]) - pal[k][2];
                const int dd = dr * dr * 2 + dg * dg * 4 + db * db;
                if (dd < bestD) { bestD = dd; best = k; }
            }
        }
        idx |= quint32(best) << (2 * i);
    }
    out[0] = uchar(c0); out[1] = uchar(c0 >> 8); out[2] = uchar(c1); out[3] = uchar(c1 >> 8);
    out[4] = uchar(idx); out[5] = uchar(idx >> 8); out[6] = uchar(idx >> 16); out[7] = uchar(idx >> 24);
}
void encodeAlphaBlock(const QRgb *px, uchar *out) {
    int mn = 255, mx = 0;
    for (int i = 0; i < 16; ++i) { mn = qMin(mn, qAlpha(px[i])); mx = qMax(mx, qAlpha(px[i])); }
    out[0] = uchar(mx); out[1] = uchar(mn);
    quint64 bits = 0;
    if (mx != mn) {
        int pal[8];
        pal[0] = mx; pal[1] = mn;
        for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * mx + k * mn) / 7;
        for (int i = 0; i < 16; ++i) {
            int best = 0, bd = 1 << 30;
            for (int k = 0; k < 8; ++k) { const int d = qAbs(qAlpha(px[i]) - pal[k]); if (d < bd) { bd = d; best = k; } }
            bits |= quint64(best) << (3 * i);
        }
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = uchar(bits >> (8 * i));
}
QByteArray encodeSurface(const QImage &img, int fmt) {   // 0 BGRA8 (DDS) / RGBA8 (KTX: caller swizzles), 1 BC1, 2 BC3
    const int w = img.width(), h = img.height();
    QByteArray out;
    if (fmt == 0) {
        out.resize(qsizetype(w) * h * 4);
        uchar *d = reinterpret_cast<uchar *>(out.data());
        for (int y = 0; y < h; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < w; ++x, d += 4) { d[0] = uchar(qBlue(s[x])); d[1] = uchar(qGreen(s[x])); d[2] = uchar(qRed(s[x])); d[3] = uchar(qAlpha(s[x])); }
        }
        return out;
    }
    const int bw = (w + 3) / 4, bh = (h + 3) / 4, bb = fmt == 1 ? 8 : 16;
    out.resize(qsizetype(bw) * bh * bb);
    QRgb blk[16];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                const int px = qMin(bx * 4 + x, w - 1), py = qMin(by * 4 + y, h - 1);   // clamp-pad partial blocks
                blk[y * 4 + x] = reinterpret_cast<const QRgb *>(img.constScanLine(py))[px];
            }
            uchar *o = reinterpret_cast<uchar *>(out.data()) + (qsizetype(by) * bw + bx) * bb;
            if (fmt == 1) encodeColorBlock(blk, o, true);
            else { encodeAlphaBlock(blk, o); encodeColorBlock(blk, o + 8, false); }
        }
    return out;
}
QVector<QImage> mipChain(const QImage &src, bool mips) {
    QVector<QImage> v{src.convertToFormat(QImage::Format_ARGB32)};
    if (!mips) return v;
    QImage cur = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    while (cur.width() > 1 || cur.height() > 1) {
        cur = cur.scaled(qMax(1, cur.width() / 2), qMax(1, cur.height() / 2), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        v << cur.convertToFormat(QImage::Format_ARGB32);
    }
    return v;
}
}

// ---------------------------------------------------------------- DDS
bool readDds(const QByteArray &d, ImageSeq &out, QString *err) {
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    const qint64 n = d.size();
    if (n < 128 || !d.startsWith("DDS ") || le32(p + 4) != 124) { setErr(err, QStringLiteral("DDS: bad header")); return false; }
    const int h = int(le32(p + 12)), w = int(le32(p + 16));
    const int depth = qMax(1, int(le32(p + 24)));
    const int mips = qMax(1, int(le32(p + 28)));
    const quint32 pfFlags = le32(p + 80), cc = le32(p + 84), bitCount = le32(p + 88);
    const quint32 caps2 = le32(p + 112);
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) { setErr(err, QStringLiteral("DDS: bad size")); return false; }
    TF f = TF::Unknown;
    Masks m;
    qint64 pos = 128;
    int layers = 1;
    const bool cube = caps2 & 0x200;
    int faces = 1;
    if (cube) { faces = 0; for (int b = 0; b < 6; ++b) if (caps2 & (0x400u << b)) ++faces; if (!faces) faces = 6; }
    if (pfFlags & 0x4) {
        if (cc == fourcc("DX10")) {
            if (n < 148) { setErr(err, QStringLiteral("DDS: truncated DX10 header")); return false; }
            f = dxgiFormat(le32(p + 128));
            const quint32 misc = le32(p + 136);
            layers = qMax(1, int(le32(p + 140)));
            if (misc & 0x4) faces = 6;   // DX10 TEXTURECUBE
            pos = 148;
            if (f == TF::Unknown) { setErr(err, QStringLiteral("DDS: unsupported DXGI format %1").arg(le32(p + 128))); return false; }
        } else if (cc == fourcc("DXT1")) f = TF::BC1;
        else if (cc == fourcc("DXT2") || cc == fourcc("DXT3")) f = TF::BC2;
        else if (cc == fourcc("DXT4") || cc == fourcc("DXT5")) f = TF::BC3;
        else if (cc == fourcc("ATI1") || cc == fourcc("BC4U")) f = TF::BC4U;
        else if (cc == fourcc("BC4S")) f = TF::BC4S;
        else if (cc == fourcc("ATI2") || cc == fourcc("BC5U")) f = TF::BC5U;
        else if (cc == fourcc("BC5S")) f = TF::BC5S;
        else if (cc == 36) f = TF::RGBA16;
        else if (cc == 111) f = TF::R16F;
        else if (cc == 112) f = TF::RG16F;
        else if (cc == 113) f = TF::RGBA16F;
        else if (cc == 114) f = TF::R32F;
        else if (cc == 115) f = TF::RG32F;
        else if (cc == 116) f = TF::RGBA32F;
        else { setErr(err, QStringLiteral("DDS: unsupported FourCC %1").arg(QString::fromLatin1(d.mid(84, 4)))); return false; }
    } else if (pfFlags & (0x40 | 0x20000 | 0x2 | 0x200)) {
        f = TF::Masked;
        m.bits = int(bitCount);
        if (m.bits != 8 && m.bits != 16 && m.bits != 24 && m.bits != 32) { setErr(err, QStringLiteral("DDS: unsupported bit count %1").arg(bitCount)); return false; }
        m.r = le32(p + 92); m.g = le32(p + 96); m.b = le32(p + 100);
        m.a = (pfFlags & (0x1 | 0x2)) ? le32(p + 104) : 0;
        m.lum = pfFlags & 0x20000;
        if (pfFlags & 0x2 && !(pfFlags & 0x40)) { m.r = m.g = m.b = 0; }   // alpha only
    } else { setErr(err, QStringLiteral("DDS: unknown pixel format")); return false; }

    // size of one full mip chain (one face / layer)
    auto chainBytes = [&](int dd) {
        qint64 t = 0; int mw = w, mh = h, md = dd;
        for (int i = 0; i < mips; ++i) { t += surfaceBytes(f, m, mw, mh) * md; mw = qMax(1, mw / 2); mh = qMax(1, mh / 2); md = qMax(1, md / 2); }
        return t;
    };
    const int images = layers * faces;
    const qint64 top = surfaceBytes(f, m, w, h);
    const qint64 chain = chainBytes(depth);
    out.frames.clear();
    for (int i = 0; i < images; ++i) {
        for (int z = 0; z < depth; ++z) {
            const qint64 off = pos + i * chain + z * top;
            if (off + top > n) {
                if (out.frames.isEmpty()) { setErr(err, QStringLiteral("DDS: truncated data")); return false; }
                break;
            }
            QImage img; HdrImage hdr;
            decodeSurface(f, m, p + off, w, h, img, hdr);
            if (isFloat(f)) img = toLdr(hdr);
            out.frames.push_back({img, 0});
        }
    }
    if (out.frames.isEmpty()) { setErr(err, QStringLiteral("DDS: no image data")); return false; }
    out.width = w; out.height = h; out.loops = 0;
    if (out.frames.size() > 1) for (auto &fr : out.frames) fr.delayMs = 500;
    return true;
}

bool writeDds(QByteArray &out, const QImage &src, const ExportOptions &o, QString *) {
    const QVector<QImage> chain = mipChain(src, o.mipmaps);
    const int w = src.width(), h = src.height(), fmt = qBound(0, o.ddsFormat, 2);
    QByteArray data;
    for (const QImage &lv : chain) data += encodeSurface(lv, fmt);
    out = QByteArray("DDS ", 4);
    put32(out, 124);
    quint32 flags = 0x1 | 0x2 | 0x4 | 0x1000 | (fmt ? 0x80000 : 0x8) | (o.mipmaps ? 0x20000 : 0);
    put32(out, flags);
    put32(out, quint32(h)); put32(out, quint32(w));
    put32(out, fmt ? quint32(encodeSurface(chain[0], fmt).size()) : quint32(w * 4));
    put32(out, 0);
    put32(out, quint32(chain.size()));
    for (int i = 0; i < 11; ++i) put32(out, 0);
    // pixel format
    put32(out, 32);
    if (fmt) { put32(out, 0x4); out.append(fmt == 1 ? "DXT1" : "DXT5", 4); for (int i = 0; i < 5; ++i) put32(out, 0); }
    else { put32(out, 0x40 | 0x1); put32(out, 0); put32(out, 32); put32(out, 0x00FF0000); put32(out, 0x0000FF00); put32(out, 0x000000FF); put32(out, 0xFF000000); }
    put32(out, 0x1000 | (o.mipmaps ? 0x400008 : 0));   // TEXTURE | (COMPLEX | MIPMAP)
    put32(out, 0); put32(out, 0); put32(out, 0); put32(out, 0);
    out += data;
    return true;
}

// ---------------------------------------------------------------- KTX 2
static const uchar kKtx2Id[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

bool readKtx2(const QByteArray &d, ImageSeq &out, QString *err) {
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    const qint64 n = d.size();
    if (n < 80 || std::memcmp(p, kKtx2Id, 12) != 0) { setErr(err, QStringLiteral("KTX2: bad identifier")); return false; }
    const quint32 vk = le32(p + 12);
    const int w = int(le32(p + 20)), h = qMax(1, int(le32(p + 24))), depth = qMax(1, int(le32(p + 28)));
    const int layers = qMax(1, int(le32(p + 32))), faces = qMax(1, int(le32(p + 36))), levels = qMax(1, int(le32(p + 40)));
    const quint32 scheme = le32(p + 44);
    if (scheme == 1 || vk == 0) { setErr(err, QStringLiteral("KTX2: Basis Universal (BasisLZ / UASTC) textures are not supported")); return false; }
    bool srgb = false;
    const TF f = vkFormat(vk, srgb);
    if (f == TF::Unknown) { setErr(err, QStringLiteral("KTX2: unsupported vkFormat %1").arg(vk)); return false; }
    if (w <= 0 || w > 32768 || h > 32768 || 80 + 24 * qint64(levels) > n) { setErr(err, QStringLiteral("KTX2: bad header")); return false; }
    const quint64 off = le64(p + 80), len = le64(p + 88), ulen = le64(p + 96);   // level 0
    if (off + len > quint64(n)) { setErr(err, QStringLiteral("KTX2: truncated level data")); return false; }
    QByteArray level;
    const QByteArray comp = d.mid(qsizetype(off), qsizetype(len));
    if (scheme == 0) level = comp;
    else if (scheme == 2) {
#ifdef LW_HAVE_ZSTD
        level.resize(qsizetype(ulen));
        const size_t r = ZSTD_decompress(level.data(), size_t(level.size()), comp.constData(), size_t(comp.size()));
        if (ZSTD_isError(r)) { setErr(err, QStringLiteral("KTX2: zstd: %1").arg(QString::fromLatin1(ZSTD_getErrorName(r)))); return false; }
        level.resize(qsizetype(r));
#else
        setErr(err, QStringLiteral("KTX2: Zstandard support not built")); return false;
#endif
    } else if (scheme == 3) {
        level.resize(qsizetype(ulen));
        uLongf dl = uLongf(ulen);
        if (uncompress(reinterpret_cast<Bytef *>(level.data()), &dl, reinterpret_cast<const Bytef *>(comp.constData()), uLong(comp.size())) != Z_OK) { setErr(err, QStringLiteral("KTX2: zlib error")); return false; }
    } else { setErr(err, QStringLiteral("KTX2: unknown supercompression %1").arg(scheme)); return false; }
    Masks m;
    const qint64 surf = surfaceBytes(f, m, w, h);
    out.frames.clear();
    for (int l = 0; l < layers; ++l)
        for (int fc = 0; fc < faces; ++fc)
            for (int z = 0; z < depth; ++z) {
                const qint64 o2 = ((qint64(l) * faces + fc) * depth + z) * surf;
                if (o2 + surf > level.size()) break;
                QImage img; HdrImage hdr;
                decodeSurface(f, m, reinterpret_cast<const uchar *>(level.constData()) + o2, w, h, img, hdr);
                if (isFloat(f)) img = toLdr(hdr);
                out.frames.push_back({img, out.frames.isEmpty() ? 0 : 500});
            }
    if (out.frames.isEmpty()) { setErr(err, QStringLiteral("KTX2: no image data")); return false; }
    if (out.frames.size() > 1) for (auto &fr : out.frames) fr.delayMs = 500;
    out.width = w; out.height = h; out.loops = 0;
    Q_UNUSED(srgb);
    return true;
}

bool writeKtx2(QByteArray &out, const QImage &src, const ExportOptions &o, QString *err) {
    const int fmt = qBound(0, o.ddsFormat, 2);
    const QVector<QImage> chain = mipChain(src, o.mipmaps);
    const int w = src.width(), h = src.height(), levels = chain.size();
#ifndef LW_HAVE_ZSTD
    const bool zstd = false;
#else
    const bool zstd = o.ktxZstd;
#endif
    const quint32 vk = fmt == 0 ? 43 : fmt == 1 ? 134 : 138;   // RGBA8_SRGB, BC1_RGBA_SRGB, BC3_SRGB
    // level payloads (RGBA order for RGBA8)
    QVector<QByteArray> raw, stored;
    for (const QImage &lv : chain) {
        QByteArray b = encodeSurface(lv, fmt);
        if (fmt == 0) for (qsizetype i = 0; i + 3 < b.size(); i += 4) std::swap(b[i], b[i + 2]);   // BGRA -> RGBA
        raw << b;
#ifdef LW_HAVE_ZSTD
        if (zstd) {
            QByteArray c(qsizetype(ZSTD_compressBound(size_t(b.size()))), 0);
            const size_t r = ZSTD_compress(c.data(), size_t(c.size()), b.constData(), size_t(b.size()), qBound(1, o.ktxZstdLevel, 22));
            if (ZSTD_isError(r)) { setErr(err, QStringLiteral("KTX2: zstd failed")); return false; }
            c.resize(qsizetype(r));
            stored << c;
            continue;
        }
#endif
        stored << b;
    }
    // Data Format Descriptor
    QByteArray dfd;
    {
        QByteArray blk;
        const int samples = fmt == 0 ? 4 : fmt == 1 ? 1 : 2;
        put32(blk, 0);                                   // vendorId 0 (Khronos), descriptorType 0
        put16(blk, 2);                                   // versionNumber
        put16(blk, quint16(24 + 16 * samples));          // descriptorBlockSize
        blk.append(char(fmt == 0 ? 1 : fmt == 1 ? 128 : 130));   // colorModel RGBSDA / BC1A / BC3
        blk.append(char(1));                             // primaries BT709
        blk.append(char(2));                             // transfer sRGB
        blk.append(char(0));                             // flags: straight alpha
        if (fmt == 0) { blk.append(char(0)); blk.append(char(0)); }
        else { blk.append(char(3)); blk.append(char(3)); }
        blk.append(char(0)); blk.append(char(0));
        const int bytesPlane0 = zstd ? 0 : (fmt == 0 ? 4 : fmt == 1 ? 8 : 16);
        blk.append(char(bytesPlane0));
        for (int i = 0; i < 7; ++i) blk.append(char(0));
        auto sample = [&](int bitOffset, int bitLength, int channel, quint32 upper) {
            put16(blk, quint16(bitOffset)); blk.append(char(bitLength - 1)); blk.append(char(channel));
            put32(blk, 0); put32(blk, 0); put32(blk, upper);
        };
        if (fmt == 0) { sample(0, 8, 0, 255); sample(8, 8, 1, 255); sample(16, 8, 2, 255); sample(24, 8, 15 | 0x10, 255); }
        else if (fmt == 1) sample(0, 64, 1, 0xFFFFFFFFu);   // BC1A alpha-present
        else { sample(0, 64, 15 | 0x10, 0xFFFFFFFFu); sample(64, 64, 0, 0xFFFFFFFFu); }
        put32(dfd, quint32(4 + blk.size()));
        dfd += blk;
    }
    // key/value data
    QByteArray kvd;
    auto kv = [&](const QByteArray &k, const QByteArray &v) {
        const QByteArray e = k + '\0' + v + '\0';
        put32(kvd, quint32(e.size())); kvd += e;
        while (kvd.size() % 4) kvd.append(char(0));
    };
    kv("KTXorientation", "rd");
    kv("KTXwriter", QByteArray(LW_PRODUCT_NAME) + " " LW_VERSION);
    const quint32 dfdOff = 80 + 24 * quint32(levels);
    const quint32 kvdOff = dfdOff + quint32(dfd.size());
    qint64 dataStart = kvdOff + kvd.size();
    const int align = zstd ? 1 : (fmt == 0 ? 4 : fmt == 1 ? 8 : 16);
    // levels are stored smallest first
    QVector<quint64> offs(levels);
    QByteArray body;
    qint64 cur = dataStart;
    for (int l = levels - 1; l >= 0; --l) {
        while (cur % align) { body.append(char(0)); ++cur; }
        offs[l] = quint64(cur);
        body += stored[l];
        cur += stored[l].size();
    }
    out = QByteArray(reinterpret_cast<const char *>(kKtx2Id), 12);
    put32(out, vk); put32(out, 1);
    put32(out, quint32(w)); put32(out, quint32(h)); put32(out, 0);
    put32(out, 0); put32(out, 1); put32(out, quint32(levels));
    put32(out, zstd ? 2 : 0);
    put32(out, dfdOff); put32(out, quint32(dfd.size()));
    put32(out, kvdOff); put32(out, quint32(kvd.size()));
    put64(out, 0); put64(out, 0);
    for (int l = 0; l < levels; ++l) { put64(out, offs[l]); put64(out, quint64(stored[l].size())); put64(out, quint64(raw[l].size())); }
    out += dfd; out += kvd; out += body;
    return true;
}

} // namespace codec
