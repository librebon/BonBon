#include "Brand.h"
// Truevision TGA reader and writer (no external library).
//
// Import: uncompressed and RLE; colour-mapped (8/16-bit indices, 15/16/24/32-bit map entries),
// true-colour 15/16/24/32-bit, greyscale 8-bit and 16-bit (grey + alpha); any origin
// (bottom-left, top-left, and the rarer right-to-left variants); alpha from the descriptor's
// alpha bits, honouring the TGA 2.0 extension area's attributes type (no alpha / undefined /
// straight / premultiplied). A 32-bit image whose alpha is all zero is treated as opaque, as
// many old writers leave that channel empty.
// Export: 24-bit or 32-bit, raw or RLE (packets never cross scan lines), bottom-left or top-left
// origin, with a TGA 2.0 extension area and footer.
#include "Codecs.h"
#include <cstring>
#include <vector>

namespace {
inline int u16(const uchar *p) { return p[0] | (p[1] << 8); }
inline quint32 u32(const uchar *p) { return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24); }
inline int x5to8(int v) { return (v << 3) | (v >> 2); }
const char kSig[] = "TRUEVISION-XFILE.";

struct Hdr {
    int idLen, cmType, type, cmFirst, cmLen, cmBits, w, h, bpp, desc;
};
bool parse(const QByteArray &d, Hdr &h) {
    if (d.size() < 18) return false;
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    h = {p[0], p[1], p[2], u16(p + 3), u16(p + 5), p[7], u16(p + 12), u16(p + 14), p[16], p[17]};
    return true;
}
bool hasFooter(const QByteArray &d) {
    return d.size() >= 18 + 26 && std::memcmp(d.constData() + d.size() - 18, kSig, 18) == 0;
}
bool valid(const Hdr &h, qint64 size, bool lenient) {
    const int base = h.type & ~8;
    if ((h.type != 1 && h.type != 2 && h.type != 3 && h.type != 9 && h.type != 10 && h.type != 11) || h.w <= 0 || h.h <= 0) return false;
    if (h.cmType > 1 || (h.desc & 0xC0) || (h.desc & 15) > 8) return false;
    if (base == 1) {
        if (h.cmType != 1 || h.cmLen <= 0 || (h.bpp != 8 && h.bpp != 16)) return false;
        if (h.cmBits != 15 && h.cmBits != 16 && h.cmBits != 24 && h.cmBits != 32) return false;
    } else if (base == 2) {
        if (h.bpp != 15 && h.bpp != 16 && h.bpp != 24 && h.bpp != 32) return false;
    } else if (h.bpp != 8 && h.bpp != 16) return false;
    if (!lenient && h.cmType == 0 && h.cmLen != 0) return false;
    const qint64 cmBytes = h.cmType ? qint64(h.cmLen) * ((h.cmBits + 7) / 8) : 0;
    return 18 + h.idLen + cmBytes < size;
}
QRgb px16(int v, bool alphaBit) {
    return qRgba(x5to8((v >> 10) & 31), x5to8((v >> 5) & 31), x5to8(v & 31), alphaBit ? ((v & 0x8000) ? 255 : 0) : 255);
}
} // namespace

namespace codec {

bool looksLikeTga(const QByteArray &d) {
    Hdr h;
    return parse(d, h) && valid(h, d.size(), hasFooter(d));
}

bool readTga(const QByteArray &d, ImageSeq &out, QString *err) {
    Hdr h;
    if (!parse(d, h) || !valid(h, d.size(), true)) { setErr(err, QStringLiteral("Not a valid TGA file")); return false; }
    if (qint64(h.w) * h.h > (qint64(1) << 28)) { setErr(err, QStringLiteral("TGA is too large")); return false; }
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    const qint64 size = d.size();
    qint64 pos = 18 + h.idLen;
    const int base = h.type & ~8;
    const bool rle = h.type & 8;
    const int alphaBits = h.desc & 15;

    // TGA 2.0 extension area: attributes type tells whether the alpha is meaningful.
    int attr = -1;
    if (hasFooter(d)) {
        quint32 ext = u32(p + size - 26);
        if (ext >= 18 && qint64(ext) + 495 <= size - 26 && u16(p + ext) >= 495) attr = p[ext + 494];
    }

    QVector<QRgb> cmap;
    if (h.cmType == 1) {
        const int eb = (h.cmBits + 7) / 8;
        if (pos + qint64(h.cmLen) * eb > size) { setErr(err, QStringLiteral("Truncated TGA colour map")); return false; }
        cmap.resize(h.cmLen);
        for (int i = 0; i < h.cmLen; ++i, pos += eb) {
            const uchar *e = p + pos;
            switch (h.cmBits) {
            case 15: cmap[i] = px16(u16(e), false); break;
            case 16: cmap[i] = px16(u16(e), alphaBits > 0); break;
            case 24: cmap[i] = qRgb(e[2], e[1], e[0]); break;
            default: cmap[i] = qRgba(e[2], e[1], e[0], e[3]); break;
            }
        }
    }

    const int bpp = (h.bpp + 7) / 8;
    const qint64 n = qint64(h.w) * h.h;
    std::vector<uchar> raw(size_t(n * bpp));
    if (!rle) {
        if (pos + n * bpp > size) { setErr(err, QStringLiteral("Truncated TGA image data")); return false; }
        std::memcpy(raw.data(), p + pos, size_t(n * bpp));
    } else {
        qint64 i = 0;
        while (i < n) {
            if (pos >= size) { setErr(err, QStringLiteral("Truncated TGA RLE data")); return false; }
            const int hd = p[pos++];
            const qint64 cnt = qMin<qint64>((hd & 0x7F) + 1, n - i);
            if (hd & 0x80) {
                if (pos + bpp > size) { setErr(err, QStringLiteral("Truncated TGA RLE data")); return false; }
                for (qint64 k = 0; k < cnt; ++k) std::memcpy(raw.data() + (i + k) * bpp, p + pos, size_t(bpp));
                pos += bpp;
            } else {
                const qint64 want = ((hd & 0x7F) + 1) * qint64(bpp);
                if (pos + want > size) { setErr(err, QStringLiteral("Truncated TGA RLE data")); return false; }
                std::memcpy(raw.data() + i * bpp, p + pos, size_t(cnt * bpp));
                pos += want;
            }
            i += cnt;
        }
    }

    // Does the file carry an alpha channel at all?
    bool alphaInData = (base == 2 && (h.bpp == 32 || (h.bpp == 16 && alphaBits > 0))) || (base == 3 && h.bpp == 16) ||
                       (base == 1 && (h.cmBits == 32 || (h.cmBits == 16 && alphaBits > 0)));
    if (attr >= 0 && attr <= 2) alphaInData = false;   // "no alpha" or "undefined, ignore"
    const bool premul = attr == 4;

    QImage img(h.w, h.h, QImage::Format_ARGB32);
    if (img.isNull()) { setErr(err, QStringLiteral("Out of memory decoding TGA")); return false; }
    const bool top = h.desc & 0x20, rtl = h.desc & 0x10;
    bool anyAlpha = false;
    for (int r = 0; r < h.h; ++r) {
        QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(top ? r : h.h - 1 - r));
        const uchar *s = raw.data() + size_t(r) * h.w * bpp;
        for (int c = 0; c < h.w; ++c, s += bpp) {
            QRgb v;
            if (base == 1) {
                int idx = (bpp == 1 ? s[0] : u16(s)) - h.cmFirst;
                v = idx >= 0 && idx < cmap.size() ? cmap[idx] : qRgba(0, 0, 0, 255);
            } else if (base == 3) {
                v = qRgba(s[0], s[0], s[0], bpp == 2 ? s[1] : 255);
            } else if (h.bpp == 24) {
                v = qRgb(s[2], s[1], s[0]);
            } else if (h.bpp == 32) {
                v = qRgba(s[2], s[1], s[0], s[3]);
            } else {
                v = px16(u16(s), alphaBits > 0);
            }
            if (!alphaInData) v |= 0xFF000000u;
            else if (qAlpha(v)) anyAlpha = true;
            dst[rtl ? h.w - 1 - c : c] = v;
        }
    }
    if (alphaInData && !anyAlpha) {
        // all-zero alpha: almost always a writer that left the channel empty
        for (int y = 0; y < h.h; ++y) {
            QRgb *l = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < h.w; ++x) l[x] |= 0xFF000000u;
        }
    } else if (alphaInData && premul) {
        img.reinterpretAsFormat(QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h.h; ++y) {   // clamp colour <= alpha so the data is valid premultiplied
            QRgb *l = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < h.w; ++x) { int a = qAlpha(l[x]); l[x] = qRgba(qMin(qRed(l[x]), a), qMin(qGreen(l[x]), a), qMin(qBlue(l[x]), a), a); }
        }
        img = img.convertToFormat(QImage::Format_ARGB32);
    }
    out.frames = {{img, 100}};
    out.width = h.w;
    out.height = h.h;
    return true;
}

bool writeTga(QByteArray &out, const QImage &src, const ExportOptions &o, QString *err) {
    if (src.width() > 65535 || src.height() > 65535) { setErr(err, QStringLiteral("TGA images are limited to 65535 x 65535 pixels")); return false; }
    const QImage img = o.alpha ? src.convertToFormat(QImage::Format_ARGB32) : flatten(src, o.matte);
    const int w = img.width(), h = img.height(), bpp = o.alpha ? 4 : 3;
    out.clear();
    out.reserve(18 + w * h * bpp + 600);
    uchar hdr[18] = {0};
    hdr[2] = o.tgaRle ? 10 : 2;
    hdr[12] = uchar(w); hdr[13] = uchar(w >> 8);
    hdr[14] = uchar(h); hdr[15] = uchar(h >> 8);
    hdr[16] = uchar(bpp * 8);
    hdr[17] = uchar((o.alpha ? 8 : 0) | (o.tgaTopLeft ? 0x20 : 0));
    out.append(reinterpret_cast<const char *>(hdr), 18);
    std::vector<uchar> row(size_t(w) * bpp);
    for (int r = 0; r < h; ++r) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(o.tgaTopLeft ? r : h - 1 - r));
        for (int x = 0; x < w; ++x) {
            uchar *d = row.data() + size_t(x) * bpp;
            d[0] = uchar(qBlue(s[x])); d[1] = uchar(qGreen(s[x])); d[2] = uchar(qRed(s[x]));
            if (bpp == 4) d[3] = uchar(qAlpha(s[x]));
        }
        if (!o.tgaRle) { out.append(reinterpret_cast<const char *>(row.data()), int(row.size())); continue; }
        auto same = [&](int a, int b) { return std::memcmp(row.data() + size_t(a) * bpp, row.data() + size_t(b) * bpp, size_t(bpp)) == 0; };
        int x = 0;
        while (x < w) {
            int run = 1;
            while (x + run < w && run < 128 && same(x, x + run)) ++run;
            if (run >= 2) {
                out.append(char(0x80 | (run - 1)));
                out.append(reinterpret_cast<const char *>(row.data() + size_t(x) * bpp), bpp);
                x += run;
                continue;
            }
            // raw packet: extend until two identical pixels start a run
            int len = 1;
            while (x + len < w && len < 128 && !(x + len + 1 < w && same(x + len, x + len + 1))) ++len;
            out.append(char(len - 1));
            out.append(reinterpret_cast<const char *>(row.data() + size_t(x) * bpp), len * bpp);
            x += len;
        }
    }
    // TGA 2.0 extension area (495 bytes) and footer.
    const quint32 extOff = quint32(out.size());
    QByteArray ext(495, '\0');
    ext[0] = char(495 & 0xFF); ext[1] = char(495 >> 8);
    const QByteArray sw = QByteArrayLiteral(LW_PRODUCT_NAME);
    std::memcpy(ext.data() + 426, sw.constData(), size_t(sw.size()));   // software ID
    ext[494] = char(o.alpha ? 3 : 0);                                     // attributes type
    out += ext;
    uchar foot[8] = {uchar(extOff), uchar(extOff >> 8), uchar(extOff >> 16), uchar(extOff >> 24), 0, 0, 0, 0};
    out.append(reinterpret_cast<const char *>(foot), 8);
    out.append(kSig, 18);
    return true;
}

} // namespace codec
