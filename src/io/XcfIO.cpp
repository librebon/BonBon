// GIMP XCF. Read: versions 0 ("file") .. 11+ (64-bit offsets from v11), RGB / greyscale / indexed,
// all precisions (8/16/32-bit integer, half, float, double; linear or perceptual), uncompressed / RLE /
// zlib tiles, layer opacity (integer and float), visibility, offsets, legacy and GIMP 2.10 layer modes,
// layer masks, layer groups (nesting from the item path, name, opacity, mode, visibility; pass-through
// reads as Normal).
// Write: version 10, 8-bit perceptual RGBA layers, RLE tiles.
#include "Codecs.h"
#include "core/Object.h"
#include <QMap>
#include <functional>
#include <cmath>
#include <cstring>
#include <zlib.h>

namespace codec {
namespace {
struct XR {
    const uchar *p; qint64 n; qint64 pos = 0; bool ok = true; bool ptr64 = false;
    bool need(qint64 k) { if (k < 0 || pos + k > n) { ok = false; return false; } return true; }
    quint8 u8() { if (!need(1)) return 0; return p[pos++]; }
    quint32 u32() { if (!need(4)) return 0; const quint32 v = quint32(p[pos]) << 24 | quint32(p[pos + 1]) << 16 | quint32(p[pos + 2]) << 8 | p[pos + 3]; pos += 4; return v; }
    qint32 i32() { return qint32(u32()); }
    quint64 ptr() { if (!ptr64) return u32(); const quint64 hi = u32(); return hi << 32 | u32(); }
    float f32() { quint32 u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    QString str() {
        const quint32 len = u32();
        if (!len || !need(len)) return {};
        QString s = QString::fromUtf8(reinterpret_cast<const char *>(p + pos), int(len) - 1);
        pos += len;
        return s;
    }
};

float halfToFloat(quint16 h) {
    const quint32 s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float f;
    if (e == 0) f = std::ldexp(float(m), -24);
    else if (e == 31) f = m ? 0.0f : INFINITY;
    else f = std::ldexp(float(m | 0x400), int(e) - 25);
    return s ? -f : f;
}

struct Prec { int bytes; int kind; bool linear; };   // kind: 0 int, 1 half, 2 float, 3 double
Prec precisionOf(int version, quint32 p) {
    if (version < 4) return {1, 0, false};
    if (version < 7) {
        switch (p) {
        case 0: return {1, 0, false}; case 1: return {2, 0, true}; case 2: return {4, 0, true};
        case 3: return {2, 1, true}; case 4: return {4, 2, true}; default: return {0, 0, false};
        }
    }
    const bool lin = (p % 100) == 0;
    switch (p / 100) {
    case 1: return {1, 0, lin}; case 2: return {2, 0, lin}; case 3: return {4, 0, lin};
    case 5: return {2, 1, lin}; case 6: return {4, 2, lin}; case 7: return {8, 3, lin};
    default: return {0, 0, false};
    }
}
// big-endian component -> 0..1
double comp(const uchar *s, const Prec &pr) {
    switch (pr.kind) {
    case 0:
        if (pr.bytes == 1) return s[0] / 255.0;
        if (pr.bytes == 2) return (s[0] << 8 | s[1]) / 65535.0;
        return (quint32(s[0]) << 24 | quint32(s[1]) << 16 | quint32(s[2]) << 8 | s[3]) / 4294967295.0;
    case 1: return halfToFloat(quint16(s[0] << 8 | s[1]));
    case 2: { quint32 u = quint32(s[0]) << 24 | quint32(s[1]) << 16 | quint32(s[2]) << 8 | s[3]; float f; std::memcpy(&f, &u, 4); return f; }
    default: { quint64 u = 0; for (int i = 0; i < 8; ++i) u = u << 8 | s[i]; double d; std::memcpy(&d, &u, 8); return d; }
    }
}
int to8(double v, bool linear, bool isAlpha) {
    if (!std::isfinite(v)) v = 0;
    v = qBound(0.0, v, 1.0);
    if (linear && !isAlpha) v = linearToSrgb(float(v));
    return int(std::lround(v * 255));
}

bool decodeRleTile(XR &r, qint64 end, int pixels, int bpp, QByteArray &tile) {
    tile = QByteArray(pixels * bpp, 0);
    uchar *d = reinterpret_cast<uchar *>(tile.data());
    for (int b = 0; b < bpp; ++b) {
        int i = 0;
        while (i < pixels) {
            if (r.pos >= end || !r.ok) return false;
            int n = r.u8();
            if (n >= 128) {
                int count;
                if (n == 128) { count = r.u8() << 8; count |= r.u8(); } else count = 256 - n;
                if (i + count > pixels || !r.need(count) || r.pos + count > end) return false;
                for (int k = 0; k < count; ++k) d[(i + k) * bpp + b] = r.p[r.pos + k];
                r.pos += count; i += count;
            } else {
                int count;
                if (n == 127) { count = r.u8() << 8; count |= r.u8(); } else count = n + 1;
                const uchar v = r.u8();
                if (i + count > pixels) return false;
                for (int k = 0; k < count; ++k) d[(i + k) * bpp + b] = v;
                i += count;
            }
        }
    }
    return r.ok;
}

// Read a hierarchy into a raw interleaved buffer (w*h*bpp).
bool readHierarchy(XR &r, quint64 hptr, int compression, QByteArray &pix, int &w, int &h, int &bpp) {
    r.pos = qint64(hptr);
    w = int(r.u32()); h = int(r.u32()); bpp = int(r.u32());
    const quint64 lptr = r.ptr();
    if (!r.ok || w <= 0 || h <= 0 || bpp <= 0 || bpp > 40 || qint64(w) * h > (1 << 27) || qint64(w) * h * bpp > (qint64(1) << 30)) return false;
    r.pos = qint64(lptr);
    const int lw = int(r.u32()), lh = int(r.u32());
    if (lw != w || lh != h) return false;
    const int tw = (w + 63) / 64, th = (h + 63) / 64;
    QVector<quint64> tiles;
    for (int i = 0; i < tw * th; ++i) tiles << r.ptr();
    const quint64 after = r.ptr();   // 0 terminator (or next pointer in some files)
    Q_UNUSED(after);
    if (!r.ok) return false;
    if (tiles.size() != tw * th) return false;
    pix = QByteArray(qsizetype(w) * h * bpp, 0);
    if (pix.size() != qsizetype(w) * h * bpp) return false;
    for (int t = 0; t < tiles.size(); ++t) {
        const int tx = t % tw, ty = t / tw;
        const int cw = qMin(64, w - tx * 64), ch = qMin(64, h - ty * 64);
        const int pixels = cw * ch;
        const qint64 start = qint64(tiles[t]);
        qint64 end = t + 1 < tiles.size() ? qint64(tiles[t + 1]) : r.n;
        if (end <= start || end > r.n) end = qMin<qint64>(r.n, start + qint64(pixels) * bpp * 3 / 2 + 1024);
        r.pos = start;
        QByteArray tile;
        if (compression == 0) {
            if (!r.need(qint64(pixels) * bpp)) return false;
            tile = QByteArray(reinterpret_cast<const char *>(r.p + r.pos), pixels * bpp);
        } else if (compression == 1) {
            if (!decodeRleTile(r, end, pixels, bpp, tile)) return false;   // a tile's data ends where the next one starts
        } else if (compression == 2) {
            tile = QByteArray(pixels * bpp, 0);
            z_stream zs{};
            if (inflateInit(&zs) != Z_OK) return false;
            zs.next_in = const_cast<Bytef *>(r.p + start);
            zs.avail_in = uInt(r.n - start);
            zs.next_out = reinterpret_cast<Bytef *>(tile.data());
            zs.avail_out = uInt(tile.size());
            const int rc = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (rc != Z_STREAM_END && zs.avail_out != 0) return false;
        } else return false;
        for (int y = 0; y < ch; ++y)
            std::memcpy(pix.data() + (qint64(ty * 64 + y) * w + tx * 64) * bpp, tile.constData() + qint64(y) * cw * bpp, size_t(cw) * bpp);
    }
    return true;
}

BlendMode modeFromXcf(quint32 m, bool *known) {
    *known = true;
    switch (m) {
    case 0: case 28: return BlendMode::Normal;
    case 3: case 30: return BlendMode::Multiply;
    case 4: case 31: return BlendMode::Screen;
    case 5: case 23: return BlendMode::Overlay;
    case 6: case 32: return BlendMode::Difference;
    case 9: case 35: return BlendMode::Darken;
    case 10: case 36: return BlendMode::Lighten;
    case 16: case 42: return BlendMode::ColorDodge;
    case 17: case 43: return BlendMode::ColorBurn;
    case 18: case 44: return BlendMode::HardLight;
    case 19: case 45: return BlendMode::SoftLight;
    case 52: return BlendMode::Exclusion;
    default: *known = false; return BlendMode::Normal;
    }
}
quint32 modeToXcf(int b) {
    switch (BlendMode(b)) {
    case BlendMode::Multiply: return 30; case BlendMode::Screen: return 31; case BlendMode::Overlay: return 23;
    case BlendMode::Darken: return 35; case BlendMode::Lighten: return 36; case BlendMode::Difference: return 32;
    case BlendMode::ColorDodge: return 42; case BlendMode::ColorBurn: return 43; case BlendMode::HardLight: return 44;
    case BlendMode::SoftLight: return 45; case BlendMode::Exclusion: return 52; default: return 28;
    }
}

void be32(QByteArray &b, quint32 v) { b.append(char(v >> 24)); b.append(char(v >> 16)); b.append(char(v >> 8)); b.append(char(v)); }
void putBe32(QByteArray &b, qsizetype at, quint32 v) { b[at] = char(v >> 24); b[at + 1] = char(v >> 16); b[at + 2] = char(v >> 8); b[at + 3] = char(v); }
void xstr(QByteArray &b, const QString &s) { const QByteArray u = s.toUtf8(); be32(b, quint32(u.size() + 1)); b += u; b.append(char(0)); }
void prop(QByteArray &b, quint32 type, const QByteArray &payload) { be32(b, type); be32(b, quint32(payload.size())); b += payload; }
QByteArray u32p(quint32 v) { QByteArray b; be32(b, v); return b; }

QByteArray rleEncodePlane(const uchar *s, int n, int stride) {
    QByteArray o;
    int i = 0;
    auto at = [&](int k) { return s[qsizetype(k) * stride]; };
    while (i < n) {
        int run = 1;
        while (i + run < n && at(i + run) == at(i) && run < 32768) ++run;
        if (run >= 3) {
            if (run <= 127) o.append(char(run - 1));
            else { o.append(char(127)); o.append(char(run >> 8)); o.append(char(run & 0xFF)); }
            o.append(char(at(i)));
            i += run;
            continue;
        }
        int lit = 0;
        while (i + lit < n && lit < 32768) {
            if (i + lit + 2 < n && at(i + lit) == at(i + lit + 1) && at(i + lit) == at(i + lit + 2)) break;
            ++lit;
        }
        if (lit <= 127) o.append(char(256 - lit));
        else { o.append(char(128)); o.append(char(lit >> 8)); o.append(char(lit & 0xFF)); }
        for (int k = 0; k < lit; ++k) o.append(char(at(i + k)));
        i += lit;
    }
    return o;
}
}

bool readXcf(const QByteArray &d, LayeredImage &out, QString *err) {
    if (!d.startsWith("gimp xcf ")) { setErr(err, QStringLiteral("XCF: bad signature")); return false; }
    XR r{reinterpret_cast<const uchar *>(d.constData()), d.size()};
    const QByteArray ver = d.mid(9, 4);
    int version = 0;
    if (ver == "file") version = 0;
    else if (ver.startsWith('v')) version = ver.mid(1).toInt();
    else { setErr(err, QStringLiteral("XCF: unknown version")); return false; }
    r.pos = 14;
    r.ptr64 = version >= 11;
    const int w = int(r.u32()), h = int(r.u32());
    r.u32();   // base type: the layers carry their own
    const quint32 precision = version >= 4 ? r.u32() : 150;
    const Prec pr = precisionOf(version, precision);
    if (!r.ok || w <= 0 || h <= 0 || qint64(w) * h > (1 << 28)) { setErr(err, QStringLiteral("XCF: bad header")); return false; }
    if (pr.bytes == 0) { setErr(err, QStringLiteral("XCF: unsupported precision %1").arg(precision)); return false; }
    int compression = 0;
    QVector<QRgb> cmap;
    for (;;) {
        const quint32 type = r.u32(), len = r.u32();
        if (!r.ok) { setErr(err, QStringLiteral("XCF: truncated properties")); return false; }
        if (type == 0) break;
        const qint64 end = r.pos + len;
        if (type == 17) compression = r.u8();
        else if (type == 1) {
            const quint32 n = r.u32();
            for (quint32 i = 0; i < n && i < 256; ++i) { const int R = r.u8(), G = r.u8(), B = r.u8(); cmap << qRgb(R, G, B); }
        }
        r.pos = end;
    }
    if (compression > 2) { setErr(err, QStringLiteral("XCF: unsupported tile compression %1").arg(compression)); return false; }
    QVector<quint64> layerPtrs;
    for (;;) { const quint64 p = r.ptr(); if (!r.ok) break; if (!p) break; layerPtrs << p; }
    out = LayeredImage();
    out.width = w; out.height = h;
    QVector<RasterLayer> topFirst;
    QMap<QVector<quint32>, int> groupOfPath;
    for (quint64 lp : layerPtrs) {
        r.pos = qint64(lp);
        const int lw = int(r.u32()), lh = int(r.u32());
        const quint32 ltype = r.u32();
        RasterLayer L;
        L.name = r.str();
        bool group = false, applyMask = false;
        QVector<quint32> itemPath;
        int opacity = 255; float fop = -1;
        quint32 mode = version >= 10 ? 28 : 0;
        for (;;) {
            const quint32 type = r.u32(), len = r.u32();
            if (!r.ok || type == 0) break;
            const qint64 end = r.pos + len;
            switch (type) {
            case 6: opacity = int(r.u32()); break;
            case 33: fop = r.f32(); break;
            case 7: mode = r.u32(); break;
            case 8: L.visible = r.u32() != 0; break;
            case 11: applyMask = r.u32() != 0; break;
            case 15: { const int x = r.i32(), y = r.i32(); L.offset = QPoint(x, y); break; }
            case 29: group = true; break;
            case 30: for (quint32 k = 0; k + 4 <= len && k < 4 * 64; k += 4) itemPath << r.u32(); break;
            default: break;
            }
            r.pos = end;
        }
        const quint64 hptr = r.ptr(), mptr = r.ptr();
        if (!r.ok) { out.partial = true; break; }
        L.opacity = fop >= 0 ? qBound(0, int(std::lround(fop * 255)), 255) : qBound(0, opacity, 255);
        bool known;
        L.blend = int(modeFromXcf(mode, &known));
        const QVector<quint32> parentPath = itemPath.isEmpty() ? itemPath : itemPath.mid(0, itemPath.size() - 1);
        L.group = groupOfPath.value(parentPath, -1);
        if (group) {
            if (itemPath.isEmpty()) { out.partial = true; continue; }   // no nesting information
            LayerGroup G;
            G.name = L.name;
            G.opacity = L.opacity;
            G.blend = known ? L.blend : 0;   // pass-through
            G.visible = L.visible;
            G.parent = L.group;
            groupOfPath[itemPath] = int(out.groups.size());
            out.groups << G;
            continue;
        }
        if (!known) { out.partial = true; out.notes << QStringLiteral("layer mode %1 shown as Normal").arg(mode); }
        QByteArray pix; int hw, hh, bpp;
        if (!readHierarchy(r, hptr, compression, pix, hw, hh, bpp) || hw != lw || hh != lh) { out.partial = true; out.notes << QStringLiteral("layer '%1' unreadable").arg(L.name); continue; }
        const bool hasA = ltype == 1 || ltype == 3 || ltype == 5;
        const bool indexed = ltype == 4 || ltype == 5;
        const int nc = (ltype <= 1 ? 3 : 1) + (hasA ? 1 : 0);
        const int cb = indexed ? 1 : pr.bytes;
        if (bpp != nc * cb) { out.partial = true; continue; }
        QImage img(lw, lh, QImage::Format_ARGB32);
        if (img.isNull()) { out.partial = true; out.notes << QStringLiteral("layer '%1' too large").arg(L.name); continue; }
        const uchar *s = reinterpret_cast<const uchar *>(pix.constData());
        for (int y = 0; y < lh; ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < lw; ++x) {
                const uchar *px = s + (qint64(y) * lw + x) * bpp;
                int R, G, B, A = 255;
                if (indexed) {
                    const QRgb c = px[0] < cmap.size() ? cmap[px[0]] : qRgb(0, 0, 0);
                    R = qRed(c); G = qGreen(c); B = qBlue(c);
                    if (hasA) A = px[1];
                } else if (ltype <= 1) {
                    R = to8(comp(px, pr), pr.linear, false); G = to8(comp(px + cb, pr), pr.linear, false); B = to8(comp(px + 2 * cb, pr), pr.linear, false);
                    if (hasA) A = to8(comp(px + 3 * cb, pr), false, true);
                } else {
                    R = G = B = to8(comp(px, pr), pr.linear, false);
                    if (hasA) A = to8(comp(px + cb, pr), false, true);
                }
                row[x] = qRgba(R, G, B, A);
            }
        }
        if (mptr && applyMask) {
            r.pos = qint64(mptr);
            const int mw = int(r.u32()), mh = int(r.u32());
            r.str();
            for (;;) { const quint32 type = r.u32(), len = r.u32(); if (!r.ok || type == 0) break; r.pos += len; }
            const quint64 mh_ptr = r.ptr();
            QByteArray mpix; int a, b, mbpp;
            if (r.ok && mw == lw && mh == lh && readHierarchy(r, mh_ptr, compression, mpix, a, b, mbpp) && mbpp == pr.bytes) {
                const uchar *ms = reinterpret_cast<const uchar *>(mpix.constData());
                for (int y = 0; y < lh; ++y) {
                    QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
                    for (int x = 0; x < lw; ++x) {
                        const int m = to8(comp(ms + (qint64(y) * lw + x) * mbpp, pr), false, true);
                        row[x] = (row[x] & 0xFFFFFF) | quint32(qAlpha(row[x]) * m / 255) << 24;
                    }
                }
            } else out.partial = true;
        }
        L.image = img;
        topFirst << L;
    }
    for (int i = topFirst.size() - 1; i >= 0; --i) out.layers << topFirst[i];
    if (out.layers.isEmpty()) { setErr(err, QStringLiteral("XCF: no readable layers")); return false; }
    return true;
}

bool writeXcf(QByteArray &out, const LayeredImage &li, QString *err) {
    if (li.width <= 0 || li.height <= 0 || li.width > 524288 || li.height > 524288) { setErr(err, QStringLiteral("XCF: bad size")); return false; }
    out = QByteArray("gimp xcf v010", 13);
    out.append(char(0));
    be32(out, quint32(li.width)); be32(out, quint32(li.height));
    be32(out, 0);     // RGB
    be32(out, 150);   // 8-bit perceptual (gamma) integer
    prop(out, 17, QByteArray(1, char(1)));   // RLE
    prop(out, 0, {});
    // XCF lists the top layer first; groups come before their members (pre-order) with an item path
    struct Item { const RasterLayer *L; bool group; QVector<quint32> path; RasterLayer gl; };
    QVector<Item> items;
    std::function<void(const QVector<LayerNode> &, const QVector<quint32> &)> collect = [&](const QVector<LayerNode> &nodes, const QVector<quint32> &path) {
        for (int i = nodes.size() - 1, pos = 0; i >= 0; --i, ++pos) {
            const LayerNode &n = nodes[i];
            QVector<quint32> p = path;
            p << quint32(pos);
            if (n.layer >= 0) { items << Item{&li.layers[n.layer], false, li.groups.isEmpty() ? QVector<quint32>() : p, {}}; continue; }
            const LayerGroup &G = li.groups[n.group];
            Item it{nullptr, true, p, {}};
            it.gl.name = G.name.isEmpty() ? QStringLiteral("Group") : G.name;
            it.gl.opacity = G.opacity; it.gl.blend = G.blend; it.gl.visible = G.visible;
            it.gl.image = QImage(1, 1, QImage::Format_ARGB32);
            it.gl.image.fill(0);
            items << it;
            collect(n.children, p);
        }
    };
    collect(layerTree(li), {});
    for (Item &it : items) if (it.group) it.L = &it.gl;
    const qsizetype layerTable = out.size();
    for (int i = 0; i <= items.size(); ++i) be32(out, 0);   // layer pointers + terminator
    be32(out, 0);   // no channels
    for (int k = 0; k < items.size(); ++k) {
        const RasterLayer &L = *items[k].L;
        const QImage img = L.image.convertToFormat(QImage::Format_ARGB32);
        const int w = img.width(), h = img.height();
        putBe32(out, layerTable + 4 * k, quint32(out.size()));
        be32(out, quint32(w)); be32(out, quint32(h)); be32(out, 1);   // RGBA
        xstr(out, L.name.isEmpty() ? QStringLiteral("Layer") : L.name);
        prop(out, 6, u32p(quint32(qBound(0, L.opacity, 255))));
        { float f = L.opacity / 255.0f; quint32 u; std::memcpy(&u, &f, 4); prop(out, 33, u32p(u)); }
        prop(out, 8, u32p(L.visible ? 1 : 0));
        prop(out, 7, u32p(modeToXcf(L.blend)));
        { QByteArray o; be32(o, quint32(L.offset.x())); be32(o, quint32(L.offset.y())); prop(out, 15, o); }
        if (items[k].group) prop(out, 29, {});
        if (!items[k].path.isEmpty()) { QByteArray p; for (quint32 v : items[k].path) be32(p, v); prop(out, 30, p); }
        prop(out, 0, {});
        const qsizetype hptrAt = out.size();
        be32(out, 0); be32(out, 0);   // hierarchy, mask
        putBe32(out, hptrAt, quint32(out.size()));
        be32(out, quint32(w)); be32(out, quint32(h)); be32(out, 4);
        const qsizetype lptrAt = out.size();
        be32(out, 0); be32(out, 0);
        putBe32(out, lptrAt, quint32(out.size()));
        be32(out, quint32(w)); be32(out, quint32(h));
        const int tw = (w + 63) / 64, th = (h + 63) / 64;
        const qsizetype tptrAt = out.size();
        for (int t = 0; t <= tw * th; ++t) be32(out, 0);
        std::vector<uchar> tile(64 * 64 * 4);
        for (int t = 0; t < tw * th; ++t) {
            const int tx = t % tw, ty = t / tw;
            const int cw = qMin(64, w - tx * 64), ch = qMin(64, h - ty * 64);
            for (int y = 0; y < ch; ++y) {
                const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(ty * 64 + y)) + tx * 64;
                for (int x = 0; x < cw; ++x) {
                    uchar *dp = tile.data() + (y * cw + x) * 4;
                    dp[0] = uchar(qRed(s[x])); dp[1] = uchar(qGreen(s[x])); dp[2] = uchar(qBlue(s[x])); dp[3] = uchar(qAlpha(s[x]));
                }
            }
            putBe32(out, tptrAt + 4 * t, quint32(out.size()));
            for (int b = 0; b < 4; ++b) out += rleEncodePlane(tile.data() + b, cw * ch, 4);
        }
        // XCF v010 uses 32-bit offsets: refuse files that would overflow them
        if (out.size() > 0x7FFFFFFF) { setErr(err, QStringLiteral("XCF: file larger than 2 GB")); return false; }
    }
    return true;
}

} // namespace codec
