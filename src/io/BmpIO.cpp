// BMP / DIB: reads OS/2 1.x (BITMAPCOREHEADER), OS/2 2.x, Windows v3/v4/v5 headers; 1/2/4/8/16/24/32-bit,
// BI_BITFIELDS / BI_ALPHABITFIELDS, RLE4 / RLE8 (with delta / end-of-line escapes), top-down rows and
// embedded PNG / JPEG (BI_PNG / BI_JPEG). Writes 1/4/8-bit (palette, optional RLE4/RLE8), 16-bit (RGB565
// bitfields), 24-bit and 32-bit (BITMAPV5HEADER with alpha mask).
#include "Codecs.h"
#include "Quantize.h"
#include <QBuffer>
#include <QImageReader>

namespace codec {

namespace {
inline quint32 le32(const uchar *p) { return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24; }
inline quint16 le16(const uchar *p) { return quint16(p[0] | p[1] << 8); }
void put16(QByteArray &b, quint16 v) { b.append(char(v)); b.append(char(v >> 8)); }
void put32(QByteArray &b, quint32 v) { b.append(char(v)); b.append(char(v >> 8)); b.append(char(v >> 16)); b.append(char(v >> 24)); }

struct Mask {
    quint32 mask = 0; int shift = 0; int bits = 0;
    explicit Mask(quint32 m = 0) : mask(m) {
        if (!m) return;
        while (!((m >> shift) & 1)) ++shift;
        while (shift + bits < 32 && ((m >> (shift + bits)) & 1)) ++bits;
    }
    int get(quint32 v) const {
        if (!mask) return -1;
        const quint32 x = (v & mask) >> shift;
        if (bits >= 8) return int(x >> (bits - 8));
        const quint32 maxv = (1u << bits) - 1;
        return int((x * 255 + maxv / 2) / maxv);
    }
};
}

// d = DIB starting at the info header. fileOffset: pixel data offset relative to d (or -1 = right after the palette)
static bool decodeDib(const QByteArray &d, qint64 pixOffset, QImage &out, bool icon, QString *err) {
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    const qint64 n = d.size();
    if (n < 12) { setErr(err, QStringLiteral("BMP: truncated header")); return false; }
    const quint32 hsize = le32(p);
    if (hsize < 12 || hsize > n) { setErr(err, QStringLiteral("BMP: bad header size")); return false; }
    qint64 w, h; int bpp; quint32 comp = 0, clrUsed = 0;
    quint32 rm = 0, gm = 0, bm = 0, am = 0;
    const bool core = hsize == 12;
    if (core) { w = le16(p + 4); h = qint16(le16(p + 6)); bpp = le16(p + 10); }
    else {
        if (hsize < 16) { setErr(err, QStringLiteral("BMP: bad header")); return false; }
        w = qint32(le32(p + 4)); h = qint32(le32(p + 8)); bpp = le16(p + 14);
        if (hsize >= 20) comp = le32(p + 16);
        if (hsize >= 36) clrUsed = le32(p + 32);
        if (hsize >= 52) { rm = le32(p + 40); gm = le32(p + 44); bm = le32(p + 48); }
        if (hsize >= 56) am = le32(p + 52);
        if (hsize == 64 && comp == 3) comp = 99;   // OS/2 2.x: 3 = Huffman 1D (unsupported)
    }
    if (icon) h /= 2;
    const bool topDown = h < 0;
    h = qAbs(h);
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535 || w * h > (1 << 28)) { setErr(err, QStringLiteral("BMP: bad dimensions")); return false; }
    qint64 pos = hsize;
    // masks stored after a 40-byte header
    if (hsize == 40 && (comp == 3 || comp == 6)) {
        if (pos + (comp == 6 ? 16 : 12) > n) { setErr(err, QStringLiteral("BMP: truncated masks")); return false; }
        rm = le32(p + pos); gm = le32(p + pos + 4); bm = le32(p + pos + 8);
        if (comp == 6) am = le32(p + pos + 12);
        pos += comp == 6 ? 16 : 12;
    }
    if (comp == 4 || comp == 5) {   // BI_JPEG / BI_PNG
        const qint64 off = pixOffset >= 0 ? pixOffset : pos;
        QImage img;
        if (!img.loadFromData(d.mid(off))) { setErr(err, QStringLiteral("BMP: cannot decode embedded %1").arg(comp == 4 ? "JPEG" : "PNG")); return false; }
        out = img.convertToFormat(QImage::Format_ARGB32);
        return true;
    }
    QVector<QRgb> pal;
    if (bpp <= 8) {
        const int entries = clrUsed && clrUsed <= (1u << bpp) ? int(clrUsed) : (1 << bpp);
        const int es = core ? 3 : 4;
        for (int i = 0; i < entries && pos + es <= n; ++i, pos += es) pal << qRgb(p[pos + 2], p[pos + 1], p[pos]);
        while (pal.size() < (1 << bpp)) pal << qRgb(0, 0, 0);
    } else if (clrUsed && !core) pos += qint64(clrUsed) * 4;   // optional palette for >8-bit, skipped
    if (pixOffset >= 0 && pixOffset < n) pos = pixOffset;
    QImage img(int(w), int(h), QImage::Format_ARGB32);
    img.fill(icon ? 0u : 0xFF000000u);
    auto rowOf = [&](qint64 y) { return reinterpret_cast<QRgb *>(img.scanLine(int(topDown ? y : h - 1 - y))); };
    if (comp == 1 || comp == 2) {   // RLE8 / RLE4
        if (bpp != (comp == 1 ? 8 : 4)) { setErr(err, QStringLiteral("BMP: RLE with wrong bit depth")); return false; }
        img.fill(0);   // undefined (skipped) pixels are transparent, as in browsers
        qint64 x = 0, y = 0;
        while (pos + 1 < n && y < h) {
            const int c = p[pos], v = p[pos + 1];
            pos += 2;
            if (c > 0) {
                for (int i = 0; i < c && x < w; ++i, ++x) {
                    const int idx = comp == 1 ? v : (i & 1 ? v & 15 : v >> 4);
                    rowOf(y)[x] = pal[idx];
                }
            } else if (v == 0) { x = 0; ++y; }
            else if (v == 1) break;
            else if (v == 2) { if (pos + 1 >= n) break; x += p[pos]; y += p[pos + 1]; pos += 2; }
            else {
                const int cnt = v;
                const int bytes = comp == 1 ? cnt : (cnt + 1) / 2;
                for (int i = 0; i < cnt && pos + (comp == 1 ? i : i / 2) < n; ++i) {
                    const int b = p[pos + (comp == 1 ? i : i / 2)];
                    const int idx = comp == 1 ? b : (i & 1 ? b & 15 : b >> 4);
                    if (x < w && y < h) rowOf(y)[x] = pal[idx];
                    ++x;
                }
                pos += bytes + (bytes & 1);
            }
        }
        out = img;
        return true;
    }
    if (comp != 0 && comp != 3 && comp != 6) { setErr(err, QStringLiteral("BMP: unsupported compression %1").arg(comp)); return false; }
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) { setErr(err, QStringLiteral("BMP: unsupported bit depth %1").arg(bpp)); return false; }
    if (comp == 0) {
        if (bpp == 16) { rm = 0x7C00; gm = 0x03E0; bm = 0x001F; am = 0; }
        if (bpp == 32) { rm = 0xFF0000; gm = 0xFF00; bm = 0xFF; am = icon ? 0xFF000000 : 0; }
        if (bpp == 24) { rm = 0xFF0000; gm = 0xFF00; bm = 0xFF; }
    }
    const Mask R(rm), G(gm), B(bm), A(am);
    const qint64 stride = ((w * bpp + 31) / 32) * 4;
    bool anyAlpha = false;
    for (qint64 y = 0; y < h; ++y) {
        const qint64 rowPos = pos + y * stride;
        if (rowPos + stride > n) {
            if (y == 0) { setErr(err, QStringLiteral("BMP: truncated pixel data")); return false; }
            break;   // keep what was decoded
        }
        const uchar *s = p + rowPos;
        QRgb *dst = rowOf(y);
        for (qint64 x = 0; x < w; ++x) {
            if (bpp <= 8) {
                const qint64 bit = x * bpp;
                const int idx = (s[bit / 8] >> (8 - bpp - bit % 8)) & ((1 << bpp) - 1);
                dst[x] = pal[idx];
            } else {
                quint32 v = bpp == 16 ? le16(s + x * 2) : bpp == 24 ? quint32(s[x * 3] | s[x * 3 + 1] << 8 | s[x * 3 + 2] << 16) : le32(s + x * 4);
                const int a = A.get(v);
                if (a >= 0 && a != 0) anyAlpha = true;
                dst[x] = qRgba(R.get(v) < 0 ? 0 : R.get(v), G.get(v) < 0 ? 0 : G.get(v), B.get(v) < 0 ? 0 : B.get(v), a < 0 ? 255 : a);
            }
        }
    }
    // a 32-bit file whose alpha bytes are all zero is opaque (common writer behaviour)
    if (am && !anyAlpha && !icon)
        for (int y = 0; y < img.height(); ++y) { QRgb *r = reinterpret_cast<QRgb *>(img.scanLine(y)); for (int x = 0; x < img.width(); ++x) r[x] |= 0xFF000000u; }
    if (icon) {
        // AND mask: 1 = transparent; ignored for 32-bit entries that carry alpha
        const qint64 mpos = pos + h * stride, mstride = ((w + 31) / 32) * 4;
        const bool useMask = !(bpp == 32 && anyAlpha);
        if (bpp == 32 && !anyAlpha) for (int y = 0; y < img.height(); ++y) { QRgb *r = reinterpret_cast<QRgb *>(img.scanLine(y)); for (int x = 0; x < img.width(); ++x) r[x] |= 0xFF000000u; }
        if (bpp < 32) for (int y = 0; y < img.height(); ++y) { QRgb *r = reinterpret_cast<QRgb *>(img.scanLine(y)); for (int x = 0; x < img.width(); ++x) r[x] |= 0xFF000000u; }
        if (useMask && mpos + h * mstride <= n) {
            for (qint64 y = 0; y < h; ++y) {
                const uchar *m = p + mpos + y * mstride;
                QRgb *dst = rowOf(y);
                for (qint64 x = 0; x < w; ++x) if (m[x / 8] & (0x80 >> (x % 8))) dst[x] = 0;
            }
        }
    }
    out = img;
    return true;
}

bool readDib(const QByteArray &d, QImage &out, bool icon, QString *err) { return decodeDib(d, -1, out, icon, err); }

bool readBmp(const QByteArray &d, ImageSeq &out, QString *err) {
    if (d.size() < 14 + 12 || d[0] != 'B' || d[1] != 'M') {
        // OS/2 bitmap arrays ("BA") hold several bitmaps: read the first one
        if (d.size() >= 14 + 14 && d[0] == 'B' && d[1] == 'A') return readBmp(d.mid(14), out, err);
        setErr(err, QStringLiteral("BMP: bad signature")); return false;
    }
    const quint32 off = le32(reinterpret_cast<const uchar *>(d.constData()) + 10);
    QImage img;
    if (!decodeDib(d.mid(14), off >= 14 ? qint64(off) - 14 : -1, img, false, err)) return false;
    out.frames = {{img, 100}};
    out.width = img.width(); out.height = img.height();
    return true;
}

namespace {
QByteArray rle(const QVector<uchar> &idx, int w, int h, int bits) {
    QByteArray o;
    for (int y = h - 1; y >= 0; --y) {   // bottom-up
        const uchar *r = idx.constData() + qsizetype(y) * w;
        int x = 0;
        while (x < w) {
            // run of identical pixels (RLE4: alternating pair counts as a run too)
            int run = 1;
            if (bits == 8) { while (x + run < w && run < 255 && r[x + run] == r[x]) ++run; }
            else { while (x + run < w && run < 255 && r[x + run] == r[x + (run & 1)]) ++run; }
            if (run >= 3 || x + run >= w) {
                o.append(char(run));
                o.append(char(bits == 8 ? r[x] : (r[x] << 4 | (run > 1 ? r[x + 1] : 0))));
                x += run;
                continue;
            }
            // literal stretch until the next run of 3+
            int lit = 0;
            while (x + lit < w && lit < 255) {
                int nr = 1;
                while (x + lit + nr < w && nr < 3 && r[x + lit + nr] == r[x + lit]) ++nr;
                if (nr >= 3) break;
                ++lit;
            }
            if (lit < 3) {   // absolute mode needs >= 3 pixels: emit short runs
                for (int i = 0; i < lit; ++i) { o.append(char(1)); o.append(char(bits == 8 ? r[x + i] : r[x + i] << 4)); }
                x += lit;
                continue;
            }
            o.append(char(0)); o.append(char(lit));
            QByteArray b;
            if (bits == 8) for (int i = 0; i < lit; ++i) b.append(char(r[x + i]));
            else for (int i = 0; i < lit; i += 2) b.append(char(r[x + i] << 4 | (i + 1 < lit ? r[x + i + 1] : 0)));
            if (b.size() & 1) b.append(char(0));
            o.append(b);
            x += lit;
        }
        o.append(char(0)); o.append(char(y == 0 ? 1 : 0));   // end of line / end of bitmap
    }
    return o;
}
}

bool writeBmp(QByteArray &out, const QImage &src, const ExportOptions &o, QString *) {
    int bits = o.bmpBits;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) bits = 24;
    const QImage img = bits == 32 ? src.convertToFormat(QImage::Format_ARGB32) : flatten(src, o.matte);
    const int w = img.width(), h = img.height();
    QVector<QRgb> pal;
    QVector<uchar> idx;
    if (bits <= 8) {
        const int maxc = 1 << bits;
        pal = quant::buildPalette({img}, qMin(maxc, qBound(2, o.colors, 256)), bits == 1 && o.palette == quant::Adaptive ? quant::BlackWhite : o.palette, o.customPalette);
        if (pal.size() > maxc) pal.resize(maxc);
        idx = quant::mapImage(img, pal, o.dither ? o.ditherAmount : 0, 0, -1, 0);
    }
    const bool useRle = o.bmpRle && (bits == 4 || bits == 8);
    const int hdr = bits == 32 ? 124 : 40;
    const int palBytes = pal.size() * 4;
    const int extraMasks = bits == 16 ? 12 : 0;
    const qint64 stride = ((qint64(w) * bits + 31) / 32) * 4;
    QByteArray pix;
    if (useRle) pix = rle(idx, w, h, bits);
    else {
        pix.fill(0, stride * h);
        for (int y = 0; y < h; ++y) {
            uchar *d = reinterpret_cast<uchar *>(pix.data()) + qsizetype(h - 1 - y) * stride;
            const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                switch (bits) {
                case 1: case 4: case 8: {
                    const int bit = x * bits;
                    d[bit / 8] |= uchar(idx[y * w + x] << (8 - bits - bit % 8));
                    break;
                }
                case 16: {
                    const quint16 v = quint16((qRed(s[x]) >> 3) << 11 | (qGreen(s[x]) >> 2) << 5 | (qBlue(s[x]) >> 3));
                    d[x * 2] = uchar(v); d[x * 2 + 1] = uchar(v >> 8);
                    break;
                }
                case 24: d[x * 3] = uchar(qBlue(s[x])); d[x * 3 + 1] = uchar(qGreen(s[x])); d[x * 3 + 2] = uchar(qRed(s[x])); break;
                default: d[x * 4] = uchar(qBlue(s[x])); d[x * 4 + 1] = uchar(qGreen(s[x])); d[x * 4 + 2] = uchar(qRed(s[x])); d[x * 4 + 3] = uchar(qAlpha(s[x]));
                }
            }
        }
    }
    const quint32 off = quint32(14 + hdr + extraMasks + palBytes);
    out.clear();
    out.append("BM", 2);
    put32(out, off + quint32(pix.size()));
    put32(out, 0);
    put32(out, off);
    put32(out, quint32(hdr));
    put32(out, quint32(w)); put32(out, quint32(h));
    put16(out, 1); put16(out, quint16(bits));
    put32(out, useRle ? (bits == 8 ? 1 : 2) : (bits == 16 || bits == 32) ? 3 : 0);
    put32(out, quint32(pix.size()));
    put32(out, 2835); put32(out, 2835);   // 72 dpi
    put32(out, quint32(pal.size())); put32(out, 0);
    if (bits == 16) { put32(out, 0xF800); put32(out, 0x07E0); put32(out, 0x001F); }
    if (bits == 32) {   // BITMAPV5HEADER tail
        put32(out, 0x00FF0000); put32(out, 0x0000FF00); put32(out, 0x000000FF); put32(out, 0xFF000000);
        out.append("BGRs", 4);                      // LCS_sRGB (stored little-endian as 'sRGB')
        for (int i = 0; i < 9 + 3; ++i) put32(out, 0);   // endpoints + gamma
        put32(out, 4);                              // LCS_GM_IMAGES
        put32(out, 0); put32(out, 0); put32(out, 0);
    }
    for (QRgb c : pal) { out.append(char(qBlue(c))); out.append(char(qGreen(c))); out.append(char(qRed(c))); out.append(char(0)); }
    out.append(pix);
    return true;
}

QByteArray writeIconDib(const QImage &src) {
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width(), h = img.height();
    QByteArray o;
    put32(o, 40); put32(o, quint32(w)); put32(o, quint32(h * 2)); put16(o, 1); put16(o, 32);
    put32(o, 0); put32(o, 0); put32(o, 0); put32(o, 0); put32(o, 0); put32(o, 0);
    for (int y = h - 1; y >= 0; --y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) { o.append(char(qBlue(s[x]))); o.append(char(qGreen(s[x]))); o.append(char(qRed(s[x]))); o.append(char(qAlpha(s[x]))); }
    }
    const int mstride = ((w + 31) / 32) * 4;
    for (int y = h - 1; y >= 0; --y) {
        QByteArray row(mstride, 0);
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) if (qAlpha(s[x]) == 0) row[x / 8] = char(uchar(row[x / 8]) | (0x80 >> (x % 8)));
        o.append(row);
    }
    return o;
}

} // namespace codec
