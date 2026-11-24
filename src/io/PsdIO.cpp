// Adobe Photoshop PSD / PSB.
// Read: version 1 (PSD) and 2 (PSB); depth 1/8/16/32; colour modes bitmap, greyscale, indexed, RGB,
// CMYK, multichannel, duotone (as greyscale) and Lab; channel compression raw / RLE (PackBits) / ZIP /
// ZIP with prediction; layer names (Pascal + 'luni' Unicode), opacity, visibility, blend modes,
// user masks; 16/32-bit layer data in 'Lr16' / 'Lr32' blocks; group folders (nesting, name, opacity,
// blend, visibility; pass-through reads as Normal); adjustment / fill / text layers keep only their pixel data.
// Write: PSD v1, 8-bit RGB, one layer per document layer (RLE, Unicode names, blend keys, opacity,
// hidden flag, transparency) plus the flattened composite.
#include "Codecs.h"
#include "core/Object.h"
#include <functional>
#include <QPainter>
#include <cmath>
#include <cstring>
#include <zlib.h>

namespace codec {

int groupChildOf(const QVector<LayerGroup> &groups, int grp, int g) {
    int c = grp;
    for (int guard = 0; c >= 0 && c < groups.size() && groups[c].parent != g && guard < 256; ++guard) c = groups[c].parent;
    return c == g ? -1 : c;
}
namespace {
QVector<LayerNode> treeOf(const LayeredImage &li, const QVector<int> &idx, int g, int depth) {
    QVector<LayerNode> out;
    for (int k = 0; k < idx.size();) {
        const RasterLayer &l = li.layers[idx[k]];
        const int sub = l.group == g ? -1 : groupChildOf(li.groups, l.group, g);
        if (sub < 0 || depth > 32) { LayerNode n; n.layer = idx[k++]; out << n; continue; }
        QVector<int> run;
        while (k < idx.size() && li.layers[idx[k]].group != g && groupChildOf(li.groups, li.layers[idx[k]].group, g) == sub) run << idx[k++];
        LayerNode n;
        n.group = sub;
        n.children = treeOf(li, run, sub, depth + 1);
        out << n;
    }
    return out;
}
}
QVector<LayerNode> layerTree(const LayeredImage &li) {
    QVector<int> all;
    for (int i = 0; i < li.layers.size(); ++i) all << i;
    return treeOf(li, all, -1, 0);
}
namespace {
// composite the layers `idx` (bottom first) that sit inside group g; nested groups are composited
// on their own and drawn with the group's opacity / blend
QImage compositeRange(const LayeredImage &li, const QVector<int> &idx, int g, int depth) {
    QImage out(li.width, li.height, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    for (int k = 0; k < idx.size();) {
        const RasterLayer &l = li.layers[idx[k]];
        const int sub = l.group == g ? -1 : groupChildOf(li.groups, l.group, g);
        if (sub < 0 || depth > 32) {
            ++k;
            if (!l.visible || l.image.isNull() || l.opacity <= 0) continue;
            drawBlended(p, l.image, QRect(l.offset, l.image.size()),
                        BlendMode(qBound(0, l.blend, int(BlendMode::Erase))), l.opacity / 255.0);
            continue;
        }
        QVector<int> run;
        while (k < idx.size()) {
            const RasterLayer &m = li.layers[idx[k]];
            if (m.group == g || groupChildOf(li.groups, m.group, g) != sub) break;
            run << idx[k++];
        }
        const LayerGroup &G = li.groups[sub];
        if (!G.visible || G.opacity <= 0) continue;
        const QImage img = compositeRange(li, run, sub, depth + 1);
        drawBlended(p, img, img.rect(), BlendMode(qBound(0, G.blend, int(BlendMode::Erase))), G.opacity / 255.0);
    }
    p.end();
    return out;
}
}
QImage compositeLayers(const LayeredImage &li) {
    QVector<int> all;
    for (int i = 0; i < li.layers.size(); ++i) all << i;
    return compositeRange(li, all, -1, 0).convertToFormat(QImage::Format_ARGB32);
}

namespace {
struct Blend { const char *key; BlendMode m; };
const Blend kPsdBlend[] = {
    {"norm", BlendMode::Normal}, {"mul ", BlendMode::Multiply}, {"scrn", BlendMode::Screen}, {"over", BlendMode::Overlay},
    {"dark", BlendMode::Darken}, {"lite", BlendMode::Lighten}, {"diff", BlendMode::Difference}, {"div ", BlendMode::ColorDodge},
    {"idiv", BlendMode::ColorBurn}, {"hLit", BlendMode::HardLight}, {"sLit", BlendMode::SoftLight}, {"smud", BlendMode::Exclusion},
    {"hue ", BlendMode::Hue}, {"sat ", BlendMode::Saturation}, {"colr", BlendMode::Color}, {"lum ", BlendMode::Luminosity},
    // Invert and Tint have no Photoshop equivalent; Erase is Fireworks-only.
    // approximations of modes Libreworks lacks
    {"lddg", BlendMode::ColorDodge}, {"lbrn", BlendMode::ColorBurn}, {"dkCl", BlendMode::Darken}, {"lgCl", BlendMode::Lighten},
    {"diss", BlendMode::Normal}, {"pass", BlendMode::Normal},
};

struct Reader {
    const uchar *p; qint64 n; qint64 pos = 0; bool ok = true;
    bool need(qint64 k) { if (pos + k > n || k < 0) { ok = false; return false; } return true; }
    quint8 u8() { if (!need(1)) return 0; return p[pos++]; }
    quint16 u16() { if (!need(2)) return 0; const quint16 v = quint16(p[pos] << 8 | p[pos + 1]); pos += 2; return v; }
    quint32 u32() { if (!need(4)) return 0; const quint32 v = quint32(p[pos]) << 24 | quint32(p[pos + 1]) << 16 | quint32(p[pos + 2]) << 8 | p[pos + 3]; pos += 4; return v; }
    quint64 u64() { const quint64 hi = u32(); return hi << 32 | u32(); }
    qint32 i32() { return qint32(u32()); }
    QByteArray bytes(qint64 k) { if (!need(k)) return {}; QByteArray b(reinterpret_cast<const char *>(p + pos), qsizetype(k)); pos += k; return b; }
};

void unpackBitsRow(const uchar *s, qint64 len, uchar *d, qint64 want) {
    qint64 i = 0, o = 0;
    while (i < len && o < want) {
        const int c = static_cast<signed char>(s[i++]);
        if (c >= 0) { for (int k = 0; k <= c && i < len && o < want; ++k) d[o++] = s[i++]; }
        else if (c != -128) { if (i >= len) break; const uchar v = s[i++]; for (int k = 0; k < 1 - c && o < want; ++k) d[o++] = v; }
    }
}

constexpr qint64 kMaxPixels = qint64(1) << 28;   // per plane or layer: 1 GB as ARGB32

// Decode one channel plane to bytes per sample (depth/8, or 1 bit packed for depth 1).
bool decodePlane(Reader &r, int comp, int w, int h, int depth, bool psb, qint64 dataLen, QByteArray &plane, bool rleCountsShared = false,
                 const QVector<quint32> *sharedCounts = nullptr, int sharedIndex = 0) {
    const qint64 rowBytes = depth == 1 ? (w + 7) / 8 : qint64(w) * (depth / 8);
    if (w < 0 || h < 0 || qint64(w) * h > kMaxPixels) return false;
    if (w == 0 || h == 0) { plane = QByteArray(); r.pos += dataLen; return r.ok; }
    // a crafted header can claim a huge plane: check that the data is there before allocating
    if (comp == 0 && !r.need(rowBytes * h)) return false;
    if (comp == 1 && !(rleCountsShared && sharedCounts) && !r.need(qint64(h) * (psb ? 4 : 2))) return false;
    if (comp == 1 && rowBytes * h > (r.n - r.pos) * 128 + qint64(h) * 2) return false;   // PackBits expands at most 128x
    plane = QByteArray(rowBytes * h, 0);
    if (plane.size() != rowBytes * h) return false;
    const qint64 start = r.pos;
    if (comp == 0) {
        std::memcpy(plane.data(), r.p + r.pos, size_t(rowBytes * h));
        r.pos += rowBytes * h;
    } else if (comp == 1) {
        QVector<quint32> counts;
        if (rleCountsShared && sharedCounts) counts = sharedCounts->mid(sharedIndex * h, h);
        else for (int y = 0; y < h; ++y) counts << (psb ? r.u32() : r.u16());
        for (int y = 0; y < h; ++y) {
            if (!r.need(counts[y])) return false;
            unpackBitsRow(r.p + r.pos, counts[y], reinterpret_cast<uchar *>(plane.data()) + y * rowBytes, rowBytes);
            r.pos += counts[y];
        }
    } else if (comp == 2 || comp == 3) {
        const qint64 len = dataLen >= 0 ? dataLen : r.n - r.pos;
        if (!r.need(len)) return false;
        uLongf dl = uLongf(plane.size());
        if (uncompress(reinterpret_cast<Bytef *>(plane.data()), &dl, r.p + r.pos, uLong(len)) != Z_OK) return false;
        r.pos += len;
        if (comp == 3) {
            uchar *d = reinterpret_cast<uchar *>(plane.data());
            for (int y = 0; y < h; ++y) {
                uchar *row = d + y * rowBytes;
                if (depth == 8) for (int x = 1; x < w; ++x) row[x] = uchar(row[x] + row[x - 1]);
                else if (depth == 16) {
                    for (int x = 1; x < w; ++x) {
                        const quint16 prev = quint16(row[(x - 1) * 2] << 8 | row[(x - 1) * 2 + 1]);
                        const quint16 cur = quint16((row[x * 2] << 8 | row[x * 2 + 1]) + prev);
                        row[x * 2] = uchar(cur >> 8); row[x * 2 + 1] = uchar(cur);
                    }
                } else if (depth == 32) {
                    // byte-wise delta over the row, then the bytes are stored as 4 planes (all MSBs first)
                    for (qint64 x = 1; x < rowBytes; ++x) row[x] = uchar(row[x] + row[x - 1]);
                    QByteArray tmp(reinterpret_cast<const char *>(row), qsizetype(rowBytes));
                    for (int x = 0; x < w; ++x) for (int b = 0; b < 4; ++b) row[x * 4 + b] = uchar(tmp[b * w + x]);
                }
            }
        }
    } else return false;
    if (dataLen >= 0 && comp != 2 && comp != 3) r.pos = qMax(r.pos, start);   // RLE/raw consumed exactly
    return r.ok;
}

// sample -> 0..255
inline int sample8(const QByteArray &pl, qint64 i, int depth, int w, int x, int y) {
    const uchar *d = reinterpret_cast<const uchar *>(pl.constData());
    switch (depth) {
    case 1: { const qint64 rb = (w + 7) / 8; return (d[y * rb + x / 8] & (0x80 >> (x % 8))) ? 0 : 255; }   // 1 = black
    case 16: return d[i * 2];
    case 32: {
        quint32 u = quint32(d[i * 4]) << 24 | quint32(d[i * 4 + 1]) << 16 | quint32(d[i * 4 + 2]) << 8 | d[i * 4 + 3];
        float f; std::memcpy(&f, &u, 4);
        if (!std::isfinite(f)) f = 0;
        return int(std::lround(linearToSrgb(f) * 255));
    }
    default: return d[i];
    }
}
inline int sampleAlpha8(const QByteArray &pl, qint64 i, int depth) {
    const uchar *d = reinterpret_cast<const uchar *>(pl.constData());
    if (depth == 16) return d[i * 2];
    if (depth == 32) { quint32 u = quint32(d[i * 4]) << 24 | quint32(d[i * 4 + 1]) << 16 | quint32(d[i * 4 + 2]) << 8 | d[i * 4 + 3]; float f; std::memcpy(&f, &u, 4); return qBound(0, int(std::lround(f * 255)), 255); }
    return d[i];
}

QRgb labToRgb(int L8, int a8, int b8) {
    const double L = L8 * 100.0 / 255, a = a8 - 128.0, b = b8 - 128.0;
    double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    auto finv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    const double X = 0.96422 * finv(fx), Y = finv(fy), Z = 0.82521 * finv(fz);   // D50
    // Bradford-adapted D50 XYZ -> linear sRGB
    double r = 3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z;
    double g = -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z;
    double bb = 0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z;
    auto enc = [](double v) { return qBound(0, int(std::lround(linearToSrgb(float(v)) * 255)), 255); };
    return qRgb(enc(r), enc(g), enc(bb));
}

struct ColorCtx { int mode; int depth; QVector<QRgb> pal; };
// planes: colour planes in channel order (+ optional alpha)
QRgb pixelFrom(const ColorCtx &c, const QVector<const QByteArray *> &pl, qint64 i, int w, int x, int y) {
    auto s = [&](int k) { return pl[k] ? sample8(*pl[k], i, c.depth, w, x, y) : 0; };
    switch (c.mode) {
    case 0: { const int v = s(0); return qRgb(v, v, v); }
    case 2: { const int idx = pl[0] ? reinterpret_cast<const uchar *>(pl[0]->constData())[i] : 0; return idx < c.pal.size() ? c.pal[idx] : qRgb(0, 0, 0); }
    case 3: return qRgb(s(0), s(1), s(2));
    case 4: {   // stored inverted: 255 = no ink
        const int C = s(0), M = s(1), Y = s(2), K = s(3);
        return qRgb(C * K / 255, M * K / 255, Y * K / 255);
    }
    case 9: return labToRgb(s(0), s(1), s(2));
    default: { const int v = s(0); return qRgb(v, v, v); }   // greyscale, duotone, multichannel
    }
}
int colorChannels(int mode) { return mode == 3 || mode == 9 ? 3 : mode == 4 ? 4 : 1; }

QByteArray packBits(const uchar *s, int len) {
    QByteArray o;
    int i = 0;
    while (i < len) {
        int run = 1;
        while (i + run < len && run < 128 && s[i + run] == s[i]) ++run;
        if (run >= 2) { o.append(char(1 - run)); o.append(char(s[i])); i += run; continue; }
        int lit = 1;
        while (i + lit < len && lit < 128 && !(i + lit + 1 < len && s[i + lit] == s[i + lit + 1])) ++lit;
        o.append(char(lit - 1));
        o.append(reinterpret_cast<const char *>(s + i), lit);
        i += lit;
    }
    return o;
}
void be16(QByteArray &b, quint16 v) { b.append(char(v >> 8)); b.append(char(v)); }
void be32(QByteArray &b, quint32 v) { b.append(char(v >> 24)); b.append(char(v >> 16)); b.append(char(v >> 8)); b.append(char(v)); }
}

bool readPsd(const QByteArray &d, LayeredImage &out, QString *err) {
    Reader r{reinterpret_cast<const uchar *>(d.constData()), d.size()};
    if (d.size() < 26 || !d.startsWith("8BPS")) { setErr(err, QStringLiteral("PSD: bad signature")); return false; }
    r.pos = 4;
    const int version = r.u16();
    if (version != 1 && version != 2) { setErr(err, QStringLiteral("PSD: unsupported version %1").arg(version)); return false; }
    const bool psb = version == 2;
    r.pos += 6;
    const int channels = r.u16();
    const int h = int(r.u32()), w = int(r.u32());
    const int depth = r.u16(), mode = r.u16();
    if (w <= 0 || h <= 0 || qint64(w) * h > (1 << 28) || channels < 1 || channels > 56) { setErr(err, QStringLiteral("PSD: bad header")); return false; }
    if (depth != 1 && depth != 8 && depth != 16 && depth != 32) { setErr(err, QStringLiteral("PSD: unsupported depth %1").arg(depth)); return false; }
    if (mode != 0 && mode != 1 && mode != 2 && mode != 3 && mode != 4 && mode != 7 && mode != 8 && mode != 9) { setErr(err, QStringLiteral("PSD: unsupported colour mode %1").arg(mode)); return false; }
    ColorCtx cc{mode, depth, {}};
    const quint32 cmLen = r.u32();
    if (mode == 2 && cmLen >= 768) {
        const qint64 b = r.pos;
        for (int i = 0; i < 256; ++i) cc.pal << qRgb(r.p[b + i], r.p[b + 256 + i], r.p[b + 512 + i]);
    }
    if (mode == 8) out.notes << QStringLiteral("duotone shown as greyscale");
    r.pos += cmLen;
    const quint32 resLen = r.u32();
    r.pos += resLen;
    if (!r.ok) { setErr(err, QStringLiteral("PSD: truncated header sections")); return false; }
    out.width = w; out.height = h;
    out.layers.clear();
    // ---- layer and mask information
    const quint64 lmLen = psb ? r.u64() : r.u32();
    const qint64 lmEnd = r.pos + qint64(lmLen);
    auto parseLayerInfo = [&](Reader &lr, qint64 end, int ldepth) -> bool {
        int count = qint16(lr.u16());
        const bool firstAlphaIsMerged = count < 0;
        Q_UNUSED(firstAlphaIsMerged);
        count = qAbs(count);
        struct Ch { int id; quint64 len; };
        struct Rec { int top, left, bottom, right; QVector<Ch> ch; QByteArray key; int opacity; int flags; QString name; int maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0, maskDefault = 0; bool maskDisabled = false; int section = 0; };
        QVector<Rec> recs;
        out.groups.clear();
        for (int i = 0; i < count && lr.ok; ++i) {
            Rec rc;
            rc.top = lr.i32(); rc.left = lr.i32(); rc.bottom = lr.i32(); rc.right = lr.i32();
            const int nch = lr.u16();
            for (int c = 0; c < nch; ++c) { Ch ch; ch.id = qint16(lr.u16()); ch.len = psb ? lr.u64() : lr.u32(); rc.ch << ch; }
            lr.pos += 4;   // '8BIM'
            rc.key = lr.bytes(4);
            rc.opacity = lr.u8();
            lr.u8();       // clipping
            rc.flags = lr.u8();
            lr.u8();
            const quint32 extra = lr.u32();
            const qint64 exEnd = lr.pos + extra;
            const quint32 maskLen = lr.u32();
            if (maskLen >= 20) {
                const qint64 mEnd = lr.pos + maskLen;
                rc.maskTop = lr.i32(); rc.maskLeft = lr.i32(); rc.maskBottom = lr.i32(); rc.maskRight = lr.i32();
                rc.maskDefault = lr.u8();
                const int mflags = lr.u8();
                rc.maskDisabled = mflags & 2;
                lr.pos = mEnd;
            } else lr.pos += maskLen;
            const quint32 brLen = lr.u32();
            lr.pos += brLen;
            const int nameLen = lr.u8();
            rc.name = QString::fromLatin1(lr.bytes(nameLen));
            lr.pos += (4 - ((nameLen + 1) % 4)) % 4;
            while (lr.pos + 12 <= exEnd && lr.ok) {   // additional info
                const QByteArray sig = lr.bytes(4), key = lr.bytes(4);
                if (sig != "8BIM" && sig != "8B64") break;
                const bool longLen = psb && (key == "LMsk" || key == "Lr16" || key == "Lr32" || key == "Layr" || key == "Mt16" || key == "Mt32" || key == "Mtrn" || key == "Alph" || key == "FMsk" || key == "lnk2" || key == "FEid" || key == "FXid" || key == "PxSD");
                const quint64 len = longLen ? lr.u64() : lr.u32();
                const qint64 vEnd = lr.pos + qint64(len);
                if (key == "luni" && len >= 4) {
                    const quint32 nchar = lr.u32();
                    QString s;
                    for (quint32 k = 0; k < nchar && lr.pos + 2 <= vEnd; ++k) s += QChar(lr.u16());
                    while (s.endsWith(QChar(0))) s.chop(1);
                    if (!s.isEmpty()) rc.name = s;
                } else if ((key == "lsct" || key == "lsdk") && len >= 4) rc.section = int(lr.u32());
                lr.pos = vEnd;
            }
            lr.pos = exEnd;
            recs << rc;
        }
        if (!lr.ok) return false;
        // channel image data (records order: bottom layer first)
        QVector<int> gstack;
        for (const Rec &rc : recs) {
            // file values: subtract in 64 bits (no signed overflow), anything out of range is skipped
            const qint64 lw64 = qint64(rc.right) - rc.left, lh64 = qint64(rc.bottom) - rc.top;
            const bool sane = lw64 > 0 && lh64 > 0 && lw64 < 300000 && lh64 < 300000 && lw64 * lh64 <= kMaxPixels;
            const int lw = sane ? int(lw64) : 0, lh = sane ? int(lh64) : 0;
            QVector<QByteArray> planes;
            QVector<int> ids;
            for (const Ch &ch : rc.ch) {
                const qint64 st = lr.pos;
                qint64 pw = lw, ph = lh;
                if (ch.id == -2 || ch.id == -3) { pw = qint64(rc.maskRight) - rc.maskLeft; ph = qint64(rc.maskBottom) - rc.maskTop; }
                if (ch.len < 2) { lr.pos = st + qint64(ch.len); planes << QByteArray(); ids << ch.id; continue; }
                const int comp = lr.u16();
                QByteArray pl;
                const bool okp = pw > 0 && ph > 0 && pw < 300000 && ph < 300000 && pw * ph <= kMaxPixels && decodePlane(lr, comp, int(pw), int(ph), ldepth, psb, qint64(ch.len) - 2, pl);
                lr.pos = st + qint64(ch.len);
                planes << (okp ? pl : QByteArray());
                ids << ch.id;
            }
            // group folders: bottom-first, the hidden divider (3) opens a group and the folder record
            // (1 open / 2 closed) above its members closes it and carries its name, opacity, blend, visibility
            if (rc.section == 3) {
                LayerGroup G;
                G.parent = gstack.isEmpty() ? -1 : gstack.last();
                gstack << int(out.groups.size());
                out.groups << G;
                continue;
            }
            if (rc.section == 1 || rc.section == 2) {
                if (gstack.isEmpty()) { out.partial = true; continue; }
                LayerGroup &G = out.groups[gstack.takeLast()];
                G.name = rc.name;
                G.opacity = rc.opacity;
                G.visible = !(rc.flags & 2);
                for (const Blend &b : kPsdBlend) if (rc.key == QByteArray(b.key, 4)) { G.blend = int(b.m); break; }
                continue;
            }
            if (!sane) {
                if (lw64 != 0 || lh64 != 0) out.partial = true;
                RasterLayer L; L.name = rc.name; L.visible = !(rc.flags & 2); L.opacity = rc.opacity; L.group = gstack.isEmpty() ? -1 : gstack.last();
                L.image = QImage(1, 1, QImage::Format_ARGB32); L.image.fill(0);
                out.layers << L;
                continue;
            }
            QVector<const QByteArray *> cp(colorChannels(mode), nullptr);
            const QByteArray *alpha = nullptr, *mask = nullptr;
            for (int k = 0; k < ids.size(); ++k) {
                if (planes[k].isEmpty()) continue;
                if (ids[k] >= 0 && ids[k] < cp.size()) cp[ids[k]] = &planes[k];
                else if (ids[k] == -1) alpha = &planes[k];
                else if (ids[k] == -2) mask = &planes[k];
            }
            ColorCtx lc = cc; lc.depth = ldepth;
            QImage img(lw, lh, QImage::Format_ARGB32);
            if (img.isNull()) { out.partial = true; out.notes << QStringLiteral("layer '%1' too large").arg(rc.name); continue; }
            for (int y = 0; y < lh; ++y) {
                QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
                for (int x = 0; x < lw; ++x) {
                    const qint64 i = qint64(y) * lw + x;
                    QRgb c = pixelFrom(lc, cp, i, lw, x, y);
                    int a = alpha ? sampleAlpha8(*alpha, i, ldepth) : 255;
                    if (mask && !rc.maskDisabled) {
                        const qint64 mx = qint64(rc.left) + x - rc.maskLeft, my = qint64(rc.top) + y - rc.maskTop;
                        const qint64 mw = qint64(rc.maskRight) - rc.maskLeft, mh = qint64(rc.maskBottom) - rc.maskTop;
                        const int mv = (mx >= 0 && my >= 0 && mx < mw && my < mh) ? sampleAlpha8(*mask, qint64(my) * mw + mx, ldepth) : rc.maskDefault;
                        a = a * mv / 255;
                    }
                    row[x] = (c & 0xFFFFFF) | quint32(a) << 24;
                }
            }
            RasterLayer L;
            L.group = gstack.isEmpty() ? -1 : gstack.last();
            L.name = rc.name;
            L.image = img;
            L.offset = QPoint(rc.left, rc.top);
            L.opacity = rc.opacity;
            L.visible = !(rc.flags & 2);
            L.blend = 0;
            bool known = false;
            for (const Blend &b : kPsdBlend) if (rc.key == QByteArray(b.key, 4)) { L.blend = int(b.m); known = true; break; }
            if (!known) { out.partial = true; out.notes << QStringLiteral("blend mode '%1' shown as Normal").arg(QString::fromLatin1(rc.key)); }
            if (rc.flags & 0x10 && rc.flags & 8) Q_UNUSED(0);
            out.layers << L;
        }
        Q_UNUSED(end);
        return true;
    };
    if (lmLen > 0 && lmEnd <= r.n) {
        const quint64 liLen = psb ? r.u64() : r.u32();
        const qint64 liEnd = r.pos + qint64(liLen);
        if (liLen > 0) {
            Reader lr = r;
            lr.n = qMin<qint64>(liEnd, r.n);
            if (!parseLayerInfo(lr, liEnd, depth)) { out.layers.clear(); out.partial = true; out.notes << QStringLiteral("layer data unreadable"); }
        }
        r.pos = liEnd;
        // global layer mask info
        if (r.pos + 4 <= lmEnd) { const quint32 gm = r.u32(); r.pos += gm; }
        // tagged blocks: Lr16 / Lr32 hold the layers of 16/32-bit documents
        while (out.layers.isEmpty() && r.pos + 12 <= lmEnd && r.ok) {
            const QByteArray sig = r.bytes(4), key = r.bytes(4);
            if (sig != "8BIM" && sig != "8B64") break;
            const bool longLen = psb && (key == "Lr16" || key == "Lr32" || key == "Layr" || key == "LMsk" || key == "Mt16" || key == "Mt32" || key == "Mtrn" || key == "Alph" || key == "FMsk" || key == "FEid" || key == "FXid" || key == "PxSD" || key == "lnk2");
            const quint64 len = longLen ? r.u64() : r.u32();
            const qint64 vEnd = r.pos + qint64(len);
            if (key == "Lr16" || key == "Lr32" || key == "Layr") {
                Reader lr = r; lr.n = qMin<qint64>(vEnd, r.n);
                if (!parseLayerInfo(lr, vEnd, depth)) { out.layers.clear(); out.partial = true; }
            }
            r.pos = vEnd + ((len & 3) ? 4 - (len & 3) : 0) * 0;
        }
    }
    r.pos = lmEnd;
    // ---- composite image data
    if (r.ok && r.pos + 2 <= r.n) {
        const int comp = r.u16();
        const int ncol = colorChannels(mode);
        const bool hasAlpha = channels > ncol && mode != 7;
        const int planesToRead = qMin(channels, ncol + (hasAlpha ? 1 : 0));
        QVector<quint32> counts;
        if (comp == 1) for (int i = 0; i < channels * h; ++i) counts << (psb ? r.u32() : r.u16());
        QVector<QByteArray> planes;
        bool okc = r.ok;
        for (int c = 0; c < planesToRead && okc; ++c) {
            QByteArray pl;
            okc = decodePlane(r, comp, w, h, depth, psb, comp >= 2 ? -1 : 0, pl, comp == 1, &counts, c);
            planes << pl;
        }
        if (okc && planes.size() >= ncol) {
            QVector<const QByteArray *> cp;
            for (int c = 0; c < ncol; ++c) cp << &planes[c];
            QImage img(w, h, QImage::Format_ARGB32);
            if (img.isNull()) { setErr(err, QStringLiteral("PSD: not enough memory for the image")); return false; }
            // Photoshop stores the merged alpha only when the document has transparency; it is
            // premultiplied against white in the composite
            for (int y = 0; y < h; ++y) {
                QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    const qint64 i = qint64(y) * w + x;
                    QRgb c = pixelFrom(cc, cp, i, w, x, y);
                    int a = 255;
                    if (hasAlpha && planes.size() > ncol) {
                        a = sampleAlpha8(planes[ncol], i, depth);
                        if (a > 0 && a < 255) {   // un-matte from white
                            auto um = [&](int v) { return qBound(0, (v - (255 - a)) * 255 / a, 255); };
                            c = qRgb(um(qRed(c)), um(qGreen(c)), um(qBlue(c)));
                        }
                    }
                    row[x] = (c & 0xFFFFFF) | quint32(a) << 24;
                }
            }
            out.composite = img;
        }
    }
    if (out.layers.isEmpty() && out.composite.isNull()) { setErr(err, QStringLiteral("PSD: no readable image data")); return false; }
    if (out.layers.isEmpty()) {
        RasterLayer L; L.name = QStringLiteral("Background"); L.image = out.composite;
        out.layers << L;
    }
    return true;
}

bool writePsd(QByteArray &out, const LayeredImage &li, QString *err) {
    const int w = li.width, h = li.height;
    if (w <= 0 || h <= 0 || w > 30000 || h > 30000) { setErr(err, QStringLiteral("PSD: size must be 1..30000 px")); return false; }
    out = QByteArray("8BPS", 4);
    be16(out, 1); out.append(QByteArray(6, 0));
    be16(out, 4); be32(out, quint32(h)); be32(out, quint32(w)); be16(out, 8); be16(out, 3);
    be32(out, 0);   // colour mode data
    be32(out, 0);   // image resources
    // ---- layer info
    QByteArray lrec, ldata;
    int recCount = 0;
    // section: 0 layer, 1 open folder (the group record, above its members), 3 bounding divider (below)
    auto writeRec = [&](const RasterLayer &L, int section) {
        ++recCount;
        const QImage img = section ? QImage() : L.image.convertToFormat(QImage::Format_ARGB32);
        const int lw = img.width(), lh = img.height();
        // channel data: -1 alpha, 0 R, 1 G, 2 B
        QVector<QByteArray> chData;
        const int ids[4] = {-1, 0, 1, 2};
        for (int id : ids) {
            QByteArray counts, data;
            std::vector<uchar> row(size_t(qMax(1, lw)));
            for (int y = 0; y < lh; ++y) {
                const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
                for (int x = 0; x < lw; ++x) row[x] = uchar(id == -1 ? qAlpha(s[x]) : id == 0 ? qRed(s[x]) : id == 1 ? qGreen(s[x]) : qBlue(s[x]));
                const QByteArray pk = packBits(row.data(), lw);
                be16(counts, quint16(pk.size()));
                data += pk;
            }
            QByteArray c;
            be16(c, 1);
            c += counts + data;
            chData << c;
        }
        be32(lrec, quint32(L.offset.y())); be32(lrec, quint32(L.offset.x()));
        be32(lrec, quint32(L.offset.y() + lh)); be32(lrec, quint32(L.offset.x() + lw));
        be16(lrec, 4);
        for (int k = 0; k < 4; ++k) { be16(lrec, quint16(qint16(ids[k]))); be32(lrec, quint32(chData[k].size())); }
        lrec.append("8BIM", 4);
        QByteArray key("norm", 4);
        for (const Blend &b : kPsdBlend) if (int(b.m) == L.blend) { key = QByteArray(b.key, 4); break; }
        lrec.append(key);
        lrec.append(char(qBound(0, L.opacity, 255)));
        lrec.append(char(0));
        lrec.append(char(L.visible ? 0x08 : 0x0A));   // bit 3: bit 4 meaningful (PS 5+); bit 1: hidden
        lrec.append(char(0));
        QByteArray extra;
        be32(extra, 0);   // mask
        be32(extra, 0);   // blending ranges
        QByteArray nm = L.name.toLatin1().left(255);
        QByteArray pas; pas.append(char(nm.size())); pas += nm;
        while (pas.size() % 4) pas.append(char(0));
        extra += pas;
        // luni: Unicode name
        QByteArray uni;
        be32(uni, quint32(L.name.size()));
        for (QChar c : L.name) be16(uni, c.unicode());
        while (uni.size() % 4) uni.append(char(0));
        extra.append("8BIMluni", 8); be32(extra, quint32(uni.size())); extra += uni;
        if (section) {
            QByteArray sc;
            be32(sc, quint32(section));
            if (section == 1) { sc.append("8BIM", 4); sc.append(key); }
            extra.append("8BIMlsct", 8); be32(extra, quint32(sc.size())); extra += sc;
        }
        be32(lrec, quint32(extra.size()));
        lrec += extra;
        for (const QByteArray &c : chData) ldata += c;
    };
    std::function<void(const QVector<LayerNode> &)> emitNodes = [&](const QVector<LayerNode> &nodes) {
        for (const LayerNode &n : nodes) {
            if (n.layer >= 0) { writeRec(li.layers[n.layer], 0); continue; }
            const LayerGroup &G = li.groups[n.group];
            RasterLayer div;
            div.name = QStringLiteral("</Layer group>");
            writeRec(div, 3);
            emitNodes(n.children);
            RasterLayer fold;
            fold.name = G.name;
            fold.opacity = G.opacity;
            fold.blend = G.blend;
            fold.visible = G.visible;
            writeRec(fold, 1);
        }
    };
    emitNodes(layerTree(li));
    QByteArray layerInfo;
    be16(layerInfo, quint16(qint16(-recCount)));   // negative: first alpha channel = merged transparency
    layerInfo += lrec + ldata;
    if (layerInfo.size() % 2) layerInfo.append(char(0));
    QByteArray lm;
    be32(lm, quint32(layerInfo.size()));
    lm += layerInfo;
    be32(lm, 0);   // global mask info
    be32(out, quint32(lm.size()));
    out += lm;
    // ---- composite (RLE, R G B A); colour un-premultiplied (Photoshop mattes it against white)
    QImage comp = li.composite.isNull() ? compositeLayers(li) : li.composite.convertToFormat(QImage::Format_ARGB32);
    be16(out, 1);
    QByteArray counts, data;
    std::vector<uchar> row(static_cast<size_t>(w));
    for (int c = 0; c < 4; ++c)
        for (int y = 0; y < h; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(comp.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                const int a = qAlpha(s[x]);
                auto mat = [&](int v) { return (v * a + 255 * (255 - a)) / 255; };
                row[x] = uchar(c == 0 ? mat(qRed(s[x])) : c == 1 ? mat(qGreen(s[x])) : c == 2 ? mat(qBlue(s[x])) : a);
            }
            const QByteArray pk = packBits(row.data(), w);
            be16(counts, quint16(pk.size()));
            data += pk;
        }
    out += counts + data;
    return true;
}

} // namespace codec
