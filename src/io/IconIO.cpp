// Windows ICO / CUR (PNG or DIB entries, any bit depth through the BMP decoder) and Apple ICNS
// (PNG / ARGB entries, legacy is32/il32/ih32/it32 PackBits RGB with s8mk/l8mk/h8mk/t8mk masks).
// JPEG 2000 ICNS entries are skipped (no decoder).
#include "Codecs.h"
#include <QBuffer>
#include <QMap>
#include <QSet>
#include <cmath>
#include <QPainter>
#include <algorithm>

namespace codec {

namespace {
inline quint32 le32(const uchar *p) { return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24; }
inline quint16 le16(const uchar *p) { return quint16(p[0] | p[1] << 8); }
inline quint32 be32(const uchar *p) { return quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3]; }
void put16(QByteArray &b, quint16 v) { b.append(char(v)); b.append(char(v >> 8)); }
void put32(QByteArray &b, quint32 v) { b.append(char(v)); b.append(char(v >> 8)); b.append(char(v >> 16)); b.append(char(v >> 24)); }
void putBe32(QByteArray &b, quint32 v) { b.append(char(v >> 24)); b.append(char(v >> 16)); b.append(char(v >> 8)); b.append(char(v)); }
QByteArray encodePng(const QImage &img) {
    QByteArray b;
    ExportOptions o;
    o.pngCompression = 9;
    if (!writePng(b, img, o, nullptr)) { QBuffer buf(&b); buf.open(QIODevice::WriteOnly); img.save(&buf, "PNG"); }
    return b;
}
bool isPng(const QByteArray &d) { return d.startsWith("\x89PNG\r\n\x1a\n"); }
void finishSeq(ImageSeq &out, QVector<QImage> imgs) {
    std::stable_sort(imgs.begin(), imgs.end(), [](const QImage &a, const QImage &b) { return a.width() * a.height() > b.width() * b.height(); });
    out.frames.clear();
    for (const QImage &i : imgs) out.frames.push_back({i.convertToFormat(QImage::Format_ARGB32), 0});
    out.width = imgs.first().width(); out.height = imgs.first().height();
    out.loops = 0;
}
}

QImage iconCanvas(const QImage &img, int size) {
    QImage c(size, size, QImage::Format_ARGB32_Premultiplied);
    c.fill(Qt::transparent);
    if (img.isNull()) return c.convertToFormat(QImage::Format_ARGB32);
    if (img.width() == size && img.height() == size) return img.convertToFormat(QImage::Format_ARGB32);   // exact, no premultiply round trip
    const QImage s = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter p(&c);
    p.drawImage((size - s.width()) / 2, (size - s.height()) / 2, s);
    p.end();
    return c.convertToFormat(QImage::Format_ARGB32);
}

bool readIco(const QByteArray &d, ImageSeq &out, QString *err) {
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    if (d.size() < 6 || le16(p) != 0 || (le16(p + 2) != 1 && le16(p + 2) != 2)) { setErr(err, QStringLiteral("ICO: bad header")); return false; }
    const int count = le16(p + 4);
    if (count == 0 || 6 + 16 * count > d.size()) { setErr(err, QStringLiteral("ICO: bad directory")); return false; }
    QVector<QImage> imgs;
    QString lastErr;
    for (int i = 0; i < count; ++i) {
        const uchar *e = p + 6 + 16 * i;
        const quint32 size = le32(e + 8), off = le32(e + 12);
        if (off >= quint32(d.size()) || size == 0 || qint64(off) + size > d.size()) continue;
        const QByteArray blob = d.mid(off, size);
        QImage img;
        if (isPng(blob)) img.loadFromData(blob, "PNG");
        else if (!readDib(blob, img, true, &lastErr)) img = QImage();
        if (!img.isNull()) imgs << img;
    }
    if (imgs.isEmpty()) { setErr(err, lastErr.isEmpty() ? QStringLiteral("ICO: no readable images") : lastErr); return false; }
    finishSeq(out, imgs);
    return true;
}

bool writeIco(QByteArray &out, const QImage &src, const ExportOptions &o, bool cursor, QString *err) {
    const QVector<int> &all = ImageIO::iconSizes();
    QVector<int> sizes;
    for (int i = 0; i < all.size(); ++i) if ((o.icoSizes >> i) & 1 && all[i] <= 256) sizes << all[i];
    if (sizes.isEmpty()) sizes << qMin(256, qMax(src.width(), src.height()));
    std::sort(sizes.begin(), sizes.end(), std::greater<int>());
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    QVector<QByteArray> blobs;
    for (int s : sizes) {
        const QImage c = iconCanvas(img, s);
        blobs << (s >= 256 ? encodePng(c) : writeIconDib(c));   // Vista+ convention: PNG for 256 px
    }
    out.clear();
    put16(out, 0); put16(out, cursor ? 2 : 1); put16(out, quint16(sizes.size()));
    quint32 off = 6 + 16 * quint32(sizes.size());
    const double iw = qMax(1, img.width()), ih = qMax(1, img.height());
    for (int i = 0; i < sizes.size(); ++i) {
        const int s = sizes[i];
        out.append(char(s >= 256 ? 0 : s)); out.append(char(s >= 256 ? 0 : s));
        out.append(char(0)); out.append(char(0));
        if (cursor) {
            const double k = std::min(s / iw, s / ih);
            const double ox = (s - iw * k) / 2, oy = (s - ih * k) / 2;
            put16(out, quint16(qBound(0, int(std::lround(ox + o.curHotX * k)), s - 1)));
            put16(out, quint16(qBound(0, int(std::lround(oy + o.curHotY * k)), s - 1)));
        } else { put16(out, 1); put16(out, 32); }
        put32(out, quint32(blobs[i].size())); put32(out, off);
        off += quint32(blobs[i].size());
    }
    for (const QByteArray &b : blobs) out.append(b);
    if (out.isEmpty()) { setErr(err, QStringLiteral("ICO: nothing to write")); return false; }
    return true;
}

// ------------------------------------------------------------------ ICNS
namespace {
QByteArray unpackBits(const uchar *p, qint64 n, qint64 &pos, qint64 want) {
    QByteArray o;
    while (o.size() < want && pos < n) {
        const int c = p[pos++];
        if (c < 128) { const qint64 len = c + 1; for (qint64 i = 0; i < len && pos < n; ++i) o.append(char(p[pos++])); }
        else { const int len = c - 125; if (pos >= n) break; const char v = char(p[pos++]); o.append(QByteArray(len, v)); }
    }
    o.resize(want);
    return o;
}
// legacy 24-bit RGB (each channel PackBits-compressed separately; it32 has 4 leading zero bytes)
QImage legacyRgb(const QByteArray &data, int size, bool it32) {
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    const qint64 n = data.size(), px = qint64(size) * size;
    QImage img(size, size, QImage::Format_ARGB32);
    if (img.isNull()) return img;
    if (n == px * 4) {   // uncompressed ARGB (some writers)
        for (int i = 0; i < px; ++i) reinterpret_cast<QRgb *>(img.bits())[i] = qRgb(p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
        return img;
    }
    qint64 pos = it32 ? 4 : 0;
    const QByteArray r = unpackBits(p, n, pos, px), g = unpackBits(p, n, pos, px), b = unpackBits(p, n, pos, px);
    QRgb *d = reinterpret_cast<QRgb *>(img.bits());
    for (qint64 i = 0; i < px; ++i) d[i] = qRgb(uchar(r[i]), uchar(g[i]), uchar(b[i]));
    return img;
}
// ic04 / ic05 "ARGB" + PackBits
QImage argbPacked(const QByteArray &data, int size) {
    if (!data.startsWith("ARGB")) return {};
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    qint64 pos = 4;
    const qint64 px = qint64(size) * size;
    const QByteArray a = unpackBits(p, data.size(), pos, px), r = unpackBits(p, data.size(), pos, px), g = unpackBits(p, data.size(), pos, px), b = unpackBits(p, data.size(), pos, px);
    QImage img(size, size, QImage::Format_ARGB32);
    if (img.isNull()) return img;
    QRgb *d = reinterpret_cast<QRgb *>(img.bits());
    for (qint64 i = 0; i < px; ++i) d[i] = qRgba(uchar(r[i]), uchar(g[i]), uchar(b[i]), uchar(a[i]));
    return img;
}
struct IcnsType { const char *type; int size; };
// PNG-based entries we write: icp4 16, icp5 32, icp6 64, ic07 128, ic08 256, ic09 512, ic10 1024 (512@2x)
const IcnsType kIcnsWrite[] = {{"icp4", 16}, {"icp5", 32}, {"icp6", 64}, {"ic07", 128},
                               {"ic08", 256}, {"ic09", 512}, {"ic10", 1024}};
}

bool readIcns(const QByteArray &d, ImageSeq &out, QString *err) {
    const uchar *p = reinterpret_cast<const uchar *>(d.constData());
    if (d.size() < 8 || !d.startsWith("icns")) { setErr(err, QStringLiteral("ICNS: bad signature")); return false; }
    const qint64 total = qMin<qint64>(be32(p + 4), d.size());
    QMap<QByteArray, QByteArray> chunks;
    QVector<QByteArray> orderTypes;
    for (qint64 pos = 8; pos + 8 <= total;) {
        const QByteArray type = d.mid(pos, 4);
        const qint64 len = be32(p + pos + 4);
        if (len < 8 || pos + len > total) break;
        if (!chunks.contains(type)) orderTypes << type;
        chunks.insert(type, d.mid(pos + 8, len - 8));
        pos += len;
    }
    QVector<QImage> imgs;
    QSet<int> haveSize;
    bool sawJp2 = false;
    for (const QByteArray &t : orderTypes) {
        const QByteArray &c = chunks[t];
        if (t == "TOC " || t == "icnV" || t == "name" || t == "info" || t.endsWith("mk") || t == "sbtp" || t == "slct") continue;
        QImage img;
        if (isPng(c)) img.loadFromData(c, "PNG");
        else if (c.startsWith("\0\0\0\x0cjP  ") || c.startsWith(QByteArray("\xff\x4f\xff\x51", 4))) { sawJp2 = true; continue; }
        else if (t == "ic04" || t == "icp4") img = argbPacked(c, 16);
        else if (t == "ic05" || t == "icp5") img = argbPacked(c, 32);
        else if (t == "is32" || t == "il32" || t == "ih32" || t == "it32") {
            const int size = t == "is32" ? 16 : t == "il32" ? 32 : t == "ih32" ? 48 : 128;
            img = legacyRgb(c, size, t == "it32");
            const QByteArray mk = chunks.value(t == "is32" ? "s8mk" : t == "il32" ? "l8mk" : t == "ih32" ? "h8mk" : "t8mk");
            if (mk.size() >= size * size) {
                QRgb *dd = reinterpret_cast<QRgb *>(img.bits());
                for (int i = 0; i < size * size; ++i) dd[i] = (dd[i] & 0xFFFFFF) | quint32(uchar(mk[i])) << 24;
            }
        }
        if (img.isNull()) continue;
        // keep one image per size (prefer the first: modern PNG entries are listed first by iconutil)
        if (haveSize.contains(img.width())) continue;
        haveSize.insert(img.width());
        imgs << img;
    }
    if (imgs.isEmpty()) {
        setErr(err, sawJp2 ? QStringLiteral("ICNS: only JPEG 2000 entries (not supported)") : QStringLiteral("ICNS: no readable images"));
        return false;
    }
    finishSeq(out, imgs);
    return true;
}

bool writeIcns(QByteArray &out, const QImage &src, const ExportOptions &o, QString *err) {
    const QVector<int> &all = ImageIO::iconSizes();
    QByteArray body, toc;
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    for (const IcnsType &t : kIcnsWrite) {
        const int bit = all.indexOf(t.size);
        if (bit < 0 || !((o.icnsSizes >> bit) & 1)) continue;
        const QByteArray png = encodePng(iconCanvas(img, t.size));
        body.append(t.type, 4); putBe32(body, quint32(png.size() + 8)); body.append(png);
        toc.append(t.type, 4); putBe32(toc, quint32(png.size() + 8));
    }
    if (body.isEmpty()) { setErr(err, QStringLiteral("ICNS: no sizes selected")); return false; }
    QByteArray tocChunk("TOC ", 4);
    putBe32(tocChunk, quint32(toc.size() + 8));
    tocChunk.append(toc);
    out = QByteArray("icns", 4);
    putBe32(out, quint32(8 + tocChunk.size() + body.size()));
    out.append(tocChunk);
    out.append(body);
    return true;
}

} // namespace codec
