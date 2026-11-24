// QOI ("Quite OK Image" format, qoiformat.org): lossless RGB / RGBA, read and write.
#include "Codecs.h"

namespace codec {

namespace {
inline int qoiHash(uchar r, uchar g, uchar b, uchar a) { return (r * 3 + g * 5 + b * 7 + a * 11) % 64; }
struct Px { uchar r = 0, g = 0, b = 0, a = 255; bool operator==(const Px &o) const { return r == o.r && g == o.g && b == o.b && a == o.a; } };
}

bool readQoi(const QByteArray &d, ImageSeq &out, QString *err) {
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    const qint64 n = d.size();
    if (n < 14 + 8 || memcmp(p, "qoif", 4) != 0) { setErr(err, QStringLiteral("QOI: bad header")); return false; }
    auto be32 = [&](int o) { return quint32(p[o]) << 24 | quint32(p[o + 1]) << 16 | quint32(p[o + 2]) << 8 | p[o + 3]; };
    const quint32 w = be32(4), h = be32(8);
    const int channels = p[12];
    if (w == 0 || h == 0 || w > 65535 || h > 65535 || quint64(w) * h > (1u << 28) || (channels != 3 && channels != 4)) {
        setErr(err, QStringLiteral("QOI: bad dimensions")); return false;
    }
    QImage img(int(w), int(h), QImage::Format_ARGB32);
    Px index[64];
    Px px;
    qint64 pos = 14;
    const qint64 end = n - 8;
    int run = 0;
    for (quint32 y = 0; y < h; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(int(y)));
        for (quint32 x = 0; x < w; ++x) {
            if (run > 0) { --run; }
            else if (pos < end) {
                const uchar b1 = p[pos++];
                if (b1 == 0xFE) { if (pos + 3 > end) break; px.r = p[pos]; px.g = p[pos + 1]; px.b = p[pos + 2]; pos += 3; }
                else if (b1 == 0xFF) { if (pos + 4 > end) break; px.r = p[pos]; px.g = p[pos + 1]; px.b = p[pos + 2]; px.a = p[pos + 3]; pos += 4; }
                else if ((b1 & 0xC0) == 0x00) px = index[b1];
                else if ((b1 & 0xC0) == 0x40) {
                    px.r += ((b1 >> 4) & 3) - 2; px.g += ((b1 >> 2) & 3) - 2; px.b += (b1 & 3) - 2;
                } else if ((b1 & 0xC0) == 0x80) {
                    if (pos >= end) break;
                    const uchar b2 = p[pos++];
                    const int vg = (b1 & 0x3F) - 32;
                    px.r += vg - 8 + ((b2 >> 4) & 0x0F); px.g += vg; px.b += vg - 8 + (b2 & 0x0F);
                } else run = b1 & 0x3F;
                index[qoiHash(px.r, px.g, px.b, px.a)] = px;
            }
            row[x] = qRgba(px.r, px.g, px.b, channels == 4 ? px.a : 255);
        }
    }
    out.frames = {{img, 100}};
    out.width = int(w); out.height = int(h);
    return true;
}

bool writeQoi(QByteArray &out, const QImage &src, const ExportOptions &o, QString *) {
    const bool alpha = o.alpha;
    const QImage img = alpha ? src.convertToFormat(QImage::Format_ARGB32) : flatten(src, o.matte);
    const int w = img.width(), h = img.height();
    out.clear();
    out.reserve(14 + w * h * 2 + 8);
    auto put32 = [&](quint32 v) { out.append(char(v >> 24)); out.append(char(v >> 16)); out.append(char(v >> 8)); out.append(char(v)); };
    out.append("qoif", 4);
    put32(quint32(w)); put32(quint32(h));
    out.append(char(alpha ? 4 : 3));
    out.append(char(0));   // sRGB with linear alpha
    Px index[64];
    Px prev;
    int run = 0;
    const qint64 total = qint64(w) * h;
    qint64 i = 0;
    for (int y = 0; y < h; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x, ++i) {
            Px px{uchar(qRed(row[x])), uchar(qGreen(row[x])), uchar(qBlue(row[x])), uchar(alpha ? qAlpha(row[x]) : 255)};
            if (px == prev) {
                ++run;
                if (run == 62 || i == total - 1) { out.append(char(0xC0 | (run - 1))); run = 0; }
                continue;
            }
            if (run > 0) { out.append(char(0xC0 | (run - 1))); run = 0; }
            const int hidx = qoiHash(px.r, px.g, px.b, px.a);
            if (index[hidx] == px) out.append(char(hidx));
            else {
                index[hidx] = px;
                if (px.a == prev.a) {
                    const int vr = int(static_cast<signed char>(px.r - prev.r)), vg = int(static_cast<signed char>(px.g - prev.g)), vb = int(static_cast<signed char>(px.b - prev.b));
                    const int vgr = vr - vg, vgb = vb - vg;
                    if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2)
                        out.append(char(0x40 | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2)));
                    else if (vgr > -9 && vgr < 8 && vg > -33 && vg < 32 && vgb > -9 && vgb < 8) {
                        out.append(char(0x80 | (vg + 32)));
                        out.append(char((vgr + 8) << 4 | (vgb + 8)));
                    } else { out.append(char(0xFE)); out.append(char(px.r)); out.append(char(px.g)); out.append(char(px.b)); }
                } else {
                    out.append(char(0xFF)); out.append(char(px.r)); out.append(char(px.g)); out.append(char(px.b)); out.append(char(px.a));
                }
            }
            prev = px;
        }
    }
    out.append("\0\0\0\0\0\0\0\1", 8);
    return true;
}

} // namespace codec
