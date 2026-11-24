// ZSoft PCX: reads 1/2/4/8 bits x 1..4 planes (mono, CGA, EGA 16-colour, VGA 256 with the trailing
// palette, 24-bit RGB, 32-bit RGBA), RLE or raw. Writes version 5 RLE: 24-bit RGB (3 planes),
// 32-bit RGBA (4 planes, alpha on) or 8-bit indexed with the 256-colour VGA palette.
#include "Codecs.h"
#include "Quantize.h"

namespace codec {

namespace {
inline quint16 le16(const uchar *p) { return quint16(p[0] | p[1] << 8); }
const QRgb kCga[16] = {0xff000000, 0xff0000aa, 0xff00aa00, 0xff00aaaa, 0xffaa0000, 0xffaa00aa, 0xffaa5500, 0xffaaaaaa,
                       0xff555555, 0xff5555ff, 0xff55ff55, 0xff55ffff, 0xffff5555, 0xffff55ff, 0xffffff55, 0xffffffff};
}

bool readPcx(const QByteArray &d, ImageSeq &out, QString *err) {
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    const qint64 n = d.size();
    if (n < 128 || p[0] != 0x0A || p[1] > 5) { setErr(err, QStringLiteral("PCX: bad header")); return false; }
    const int enc = p[2], bpp = p[3];
    const int x0 = le16(p + 4), y0 = le16(p + 6), x1 = le16(p + 8), y1 = le16(p + 10);
    const int planes = p[65], bpl = le16(p + 66);
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w <= 0 || h <= 0 || w > 32767 || h > 32767 || planes < 1 || planes > 4 || bpl <= 0 || (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8)) {
        setErr(err, QStringLiteral("PCX: unsupported layout (%1 bits x %2 planes)").arg(bpp).arg(planes)); return false;
    }
    if (qint64(bpl) * 8 < qint64(w) * bpp) { setErr(err, QStringLiteral("PCX: bad bytes per line")); return false; }
    const qint64 lineBytes = qint64(bpl) * planes;
    QByteArray raw(lineBytes * h, 0);
    qint64 pos = 128, o = 0;
    const qint64 want = raw.size();
    while (o < want && pos < n) {
        int c = p[pos++];
        if (enc == 1 && (c & 0xC0) == 0xC0) {
            const int cnt = c & 0x3F;
            if (pos >= n) break;
            const char v = char(p[pos++]);
            for (int i = 0; i < cnt && o < want; ++i) raw[o++] = v;
        } else raw[o++] = char(c);
    }
    QVector<QRgb> pal;
    if (bpp == 8 && planes == 1) {
        if (n >= 769 && p[n - 769] == 0x0C) for (int i = 0; i < 256; ++i) pal << qRgb(p[n - 768 + i * 3], p[n - 767 + i * 3], p[n - 766 + i * 3]);
        else for (int i = 0; i < 256; ++i) pal << qRgb(i, i, i);   // no palette: greyscale
    } else if (bpp * planes <= 4) {
        const int nc = 1 << (bpp * planes);
        const bool hdrPal = p[1] != 3;   // version 3 = "no palette, use the default"
        bool allZero = true;
        for (int i = 0; i < 48; ++i) if (p[16 + i]) allZero = false;
        for (int i = 0; i < nc; ++i) {
            if (nc == 2 && (allZero || !hdrPal)) pal << (i ? 0xffffffffu : 0xff000000u);
            else if (hdrPal && !allZero) pal << qRgb(p[16 + i * 3], p[17 + i * 3], p[18 + i * 3]);
            else pal << kCga[i % 16];
        }
    }
    QImage img(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const uchar *line = reinterpret_cast<const uchar *>(raw.constData()) + y * lineBytes;
        QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            if (bpp == 8 && planes >= 3) {
                const int r = line[x], g = line[bpl + x], b = line[2 * bpl + x], a = planes == 4 ? line[3 * bpl + x] : 255;
                dst[x] = qRgba(r, g, b, a);
            } else if (bpp == 8) dst[x] = pal[line[x]];
            else {
                int idx = 0;
                for (int pl = 0; pl < planes; ++pl) {
                    const uchar *pp = line + pl * bpl;
                    const int bit = x * bpp;
                    const int v = (pp[bit / 8] >> (8 - bpp - bit % 8)) & ((1 << bpp) - 1);
                    idx |= v << (pl * bpp);
                }
                dst[x] = idx < pal.size() ? pal[idx] : 0xff000000u;
            }
        }
    }
    out.frames = {{img, 100}};
    out.width = w; out.height = h;
    return true;
}

namespace {
void rleLine(QByteArray &o, const uchar *s, int len) {
    int i = 0;
    while (i < len) {
        int run = 1;
        while (i + run < len && run < 63 && s[i + run] == s[i]) ++run;
        if (run > 1 || (s[i] & 0xC0) == 0xC0) { o.append(char(0xC0 | run)); o.append(char(s[i])); }
        else o.append(char(s[i]));
        i += run;
    }
}
}

bool writePcx(QByteArray &out, const QImage &src, const ExportOptions &o, QString *) {
    const bool indexed = o.pcx8;
    const bool alpha = !indexed && o.alpha && src.hasAlphaChannel();
    const QImage img = alpha ? src.convertToFormat(QImage::Format_ARGB32) : flatten(src, o.matte);
    const int w = img.width(), h = img.height();
    const int planes = indexed ? 1 : alpha ? 4 : 3;
    const int bpl = (w + 1) & ~1;   // even
    QVector<QRgb> pal;
    QVector<uchar> idx;
    if (indexed) {
        pal = quant::buildPalette({img}, qBound(2, o.colors, 256), o.palette, o.customPalette);
        idx = quant::mapImage(img, pal, o.dither ? o.ditherAmount : 0, 0, -1, 0);
    }
    out = QByteArray(128, 0);
    uchar *hd = reinterpret_cast<uchar *>(out.data());
    hd[0] = 0x0A; hd[1] = 5; hd[2] = 1; hd[3] = 8;
    auto set16 = [&](int off, int v) { hd[off] = uchar(v); hd[off + 1] = uchar(v >> 8); };
    set16(4, 0); set16(6, 0); set16(8, w - 1); set16(10, h - 1); set16(12, 72); set16(14, 72);
    hd[65] = uchar(planes); set16(66, bpl); set16(68, 1);   /* palette info: 1 = colour/BW */ set16(70, w); set16(72, h);
    QByteArray line(bpl * planes, 0);
    for (int y = 0; y < h; ++y) {
        line.fill(0);
        uchar *l = reinterpret_cast<uchar *>(line.data());
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            if (indexed) l[x] = idx[y * w + x];
            else {
                l[x] = uchar(qRed(s[x])); l[bpl + x] = uchar(qGreen(s[x])); l[2 * bpl + x] = uchar(qBlue(s[x]));
                if (alpha) l[3 * bpl + x] = uchar(qAlpha(s[x]));
            }
        }
        // each plane is encoded separately (runs must not cross planes)
        for (int pl = 0; pl < planes; ++pl) rleLine(out, l + pl * bpl, bpl);
    }
    if (indexed) {
        out.append(char(0x0C));
        for (int i = 0; i < 256; ++i) {
            const QRgb c = i < pal.size() ? pal[i] : 0;
            out.append(char(qRed(c))); out.append(char(qGreen(c))); out.append(char(qBlue(c)));
        }
    }
    return true;
}

} // namespace codec
