#include "Brand.h"
#include <cmath>
#include "Quantize.h"
#include <algorithm>
#include <vector>
#include <cstring>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QTextStream>
#include <QRegularExpression>

namespace quant {

namespace {
struct Box { int lo[3], hi[3]; std::vector<int> bins; long long count = 0; };

void shrink(Box &b, const std::vector<int> &hist) {
    for (int c = 0; c < 3; ++c) { b.lo[c] = 31; b.hi[c] = 0; }
    b.count = 0;
    for (int bin : b.bins) {
        int v[3] = {(bin >> 10) & 31, (bin >> 5) & 31, bin & 31};
        for (int c = 0; c < 3; ++c) { b.lo[c] = std::min(b.lo[c], v[c]); b.hi[c] = std::max(b.hi[c], v[c]); }
        b.count += hist[bin];
    }
}
}

QStringList paletteNames() {
    return {QStringLiteral("Adaptive"), QStringLiteral("Web 216"), QStringLiteral("Web Snap Adaptive"), QStringLiteral("Exact"),
            QStringLiteral("Grayscale"), QStringLiteral("Black & White"), QStringLiteral("Custom")};
}
QVector<int> paletteOrder() { return {Adaptive, Web216, WebSnap, Exact, Grayscale, BlackWhite, Custom}; }

namespace {
// every distinct opaque colour (alpha >= 128), or empty when there are more than `limit`
QVector<QRgb> exactColors(const QVector<QImage> &imgs, int limit) {
    QHash<QRgb, int> seen;
    QVector<QRgb> pal;
    for (const QImage &im0 : imgs) {
        const QImage im = im0.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < im.height(); ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(im.constScanLine(y));
            for (int x = 0; x < im.width(); ++x) {
                if (qAlpha(s[x]) < 128) continue;
                const QRgb c = s[x] | 0xFF000000u;
                if (seen.contains(c)) continue;
                if (pal.size() >= limit) return {};
                seen.insert(c, pal.size());
                pal << c;
            }
        }
    }
    return pal;
}
}

QVector<QRgb> buildPalette(const QVector<QImage> &imgs, int maxColors, int type, const QVector<QRgb> &custom) {
    maxColors = std::clamp(maxColors, 2, 256);
    QVector<QRgb> pal;
    if (type == Custom) {
        for (QRgb c : custom) { if (pal.size() >= 256) break; pal << (c | 0xFF000000u); }
        if (pal.size() >= 1) return pal;
        type = Adaptive;
    }
    if (type == Exact) {
        pal = exactColors(imgs, maxColors);
        if (!pal.isEmpty()) return pal;
        type = Adaptive;   // too many colours: Fireworks falls back to an adaptive table
    }
    if (type == WebSnap) {
        // adaptive, then colours within 7 levels of a web-safe value snap to it (Fireworks "Web Snap Adaptive")
        QVector<QRgb> a = buildPalette(imgs, maxColors, Adaptive);
        QVector<QRgb> outp;
        for (QRgb c : a) {
            int v[3] = {qRed(c), qGreen(c), qBlue(c)};
            bool near = true;
            for (int &x : v) { const int w = (x + 25) / 51 * 51; if (std::abs(w - x) > 7) near = false; }
            if (near) for (int &x : v) x = (x + 25) / 51 * 51;
            const QRgb n = qRgb(v[0], v[1], v[2]);
            if (!outp.contains(n)) outp << n;
        }
        return outp;
    }
    if (type == Web216) {
        for (int r = 0; r < 6; ++r) for (int g = 0; g < 6; ++g) for (int b = 0; b < 6; ++b)
            pal << qRgb(r * 51, g * 51, b * 51);
        return pal;
    }
    if (type == Grayscale) {
        for (int i = 0; i < maxColors; ++i) { int v = maxColors == 1 ? 0 : i * 255 / (maxColors - 1); pal << qRgb(v, v, v); }
        return pal;
    }
    if (type == BlackWhite) return {qRgb(0, 0, 0), qRgb(255, 255, 255)};

    std::vector<int> hist(32768, 0);
    std::vector<long long> sum(32768 * 3, 0);
    for (const QImage &im0 : imgs) {
        QImage im = im0.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < im.height(); ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(im.constScanLine(y));
            for (int x = 0; x < im.width(); ++x) {
                QRgb p = s[x];
                if (qAlpha(p) < 128) continue;
                int bin = ((qRed(p) >> 3) << 10) | ((qGreen(p) >> 3) << 5) | (qBlue(p) >> 3);
                hist[bin]++;
                sum[bin * 3] += qRed(p); sum[bin * 3 + 1] += qGreen(p); sum[bin * 3 + 2] += qBlue(p);
            }
        }
    }
    Box first;
    for (int i = 0; i < 32768; ++i) if (hist[i]) first.bins.push_back(i);
    if (first.bins.empty()) return {qRgb(0, 0, 0), qRgb(255, 255, 255)};
    shrink(first, hist);
    std::vector<Box> boxes{first};
    while (int(boxes.size()) < maxColors) {
        // split the box with the largest (range * population)
        int best = -1; long long bestScore = 0;
        for (int i = 0; i < int(boxes.size()); ++i) {
            if (boxes[i].bins.size() < 2) continue;
            int range = 0;
            for (int c = 0; c < 3; ++c) range = std::max(range, boxes[i].hi[c] - boxes[i].lo[c]);
            long long score = (long long)(range + 1) * boxes[i].count;
            if (score > bestScore) { bestScore = score; best = i; }
        }
        if (best < 0) break;
        Box b = std::move(boxes[best]);
        int axis = 0, range = -1;
        for (int c = 0; c < 3; ++c) if (b.hi[c] - b.lo[c] > range) { range = b.hi[c] - b.lo[c]; axis = c; }
        int shift = axis == 0 ? 10 : axis == 1 ? 5 : 0;
        std::sort(b.bins.begin(), b.bins.end(), [&](int a, int c) { return ((a >> shift) & 31) < ((c >> shift) & 31); });
        long long half = b.count / 2, acc = 0;
        size_t cut = 0;
        for (; cut < b.bins.size() - 1; ++cut) { acc += hist[b.bins[cut]]; if (acc >= half) break; }
        Box a, c;
        a.bins.assign(b.bins.begin(), b.bins.begin() + cut + 1);
        c.bins.assign(b.bins.begin() + cut + 1, b.bins.end());
        if (c.bins.empty()) { c.bins.push_back(a.bins.back()); a.bins.pop_back(); }
        shrink(a, hist); shrink(c, hist);
        boxes[best] = std::move(a);
        boxes.push_back(std::move(c));
    }
    for (auto &b : boxes) {
        long long r = 0, g = 0, bl = 0, n = 0;
        for (int bin : b.bins) { r += sum[bin * 3]; g += sum[bin * 3 + 1]; bl += sum[bin * 3 + 2]; n += hist[bin]; }
        if (n) pal << qRgb(int(r / n), int(g / n), int(bl / n));
    }
    return pal;
}

QVector<uchar> mapImage(const QImage &img0, const QVector<QRgb> &pal, int ditherAmount, int loss, int transparentIndex, int indexOffset) {
    const bool dither = ditherAmount > 0;
    const int dk = std::clamp(ditherAmount, 0, 100);
    const int lossT = loss > 0 ? int(std::pow(std::clamp(loss, 0, 100) / 100.0 * 40.0, 2) * 9) : 0; // weighted squared distance
    int prev = -1;
    QImage img = img0.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width(), h = img.height();
    QVector<uchar> out(w * h);
    std::vector<short> cache(32768, -1);
    auto nearest = [&](int r, int g, int b) -> int {
        int bin = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        if (cache[bin] >= 0) return cache[bin];
        int rr = (r & ~7) | 4, gg = (g & ~7) | 4, bb = (b & ~7) | 4;
        int best = 0, bestD = INT32_MAX;
        for (int i = 0; i < pal.size(); ++i) {
            int dr = qRed(pal[i]) - rr, dg = qGreen(pal[i]) - gg, db = qBlue(pal[i]) - bb;
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bestD) { bestD = d; best = i; }
        }
        cache[bin] = short(best);
        return best;
    };
    QHash<QRgb, int> exact;   // colours that are in the palette map to themselves exactly
    for (int i = pal.size() - 1; i >= 0; --i) exact.insert(pal[i] | 0xFF000000u, i);
    std::vector<int> err0((w + 2) * 3, 0), err1((w + 2) * 3, 0);
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        std::fill(err1.begin(), err1.end(), 0);
        prev = -1;
        for (int x = 0; x < w; ++x) {
            QRgb p = s[x];
            if (qAlpha(p) < 128 && transparentIndex >= 0) { out[y * w + x] = uchar(transparentIndex); prev = -1; continue; }
            int r = qRed(p), g = qGreen(p), b = qBlue(p);
            if (dither) {
                r = std::clamp(r + err0[(x + 1) * 3] * dk / 1600, 0, 255);
                g = std::clamp(g + err0[(x + 1) * 3 + 1] * dk / 1600, 0, 255);
                b = std::clamp(b + err0[(x + 1) * 3 + 2] * dk / 1600, 0, 255);
            }
            int idx = -1;
            if (lossT > 0 && prev >= 0) {
                const int dr = qRed(pal[prev]) - r, dg = qGreen(pal[prev]) - g, db = qBlue(pal[prev]) - b;
                if (dr * dr * 3 + dg * dg * 4 + db * db * 2 <= lossT) idx = prev;
            }
            if (idx < 0) { auto it = exact.constFind(qRgb(r, g, b)); idx = it != exact.constEnd() ? it.value() : nearest(r, g, b); }
            prev = idx;
            out[y * w + x] = uchar(idx + indexOffset);
            if (dither) {
                int er[3] = {r - qRed(pal[idx]), g - qGreen(pal[idx]), b - qBlue(pal[idx])};
                for (int c = 0; c < 3; ++c) {
                    err0[(x + 2) * 3 + c] += er[c] * 7;
                    err1[(x) * 3 + c] += er[c] * 3;
                    err1[(x + 1) * 3 + c] += er[c] * 5;
                    err1[(x + 2) * 3 + c] += er[c] * 1;
                }
            }
        }
        std::swap(err0, err1);
    }
    return out;
}

QVector<QRgb> loadPalette(const QString &path, QString *err) {
    QFile f(path);
    QVector<QRgb> pal;
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return pal; }
    const QByteArray d = f.readAll();
    const QString ext = QFileInfo(path).suffix().toLower();
    if (d.startsWith("GIMP Palette")) {
        QTextStream ts(d);
        while (!ts.atEnd()) {
            const QString line = ts.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#') || line.startsWith(QLatin1String("GIMP")) || line.contains(':')) continue;
            const QStringList p = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            bool a, b, c;
            if (p.size() >= 3) { int r = p[0].toInt(&a), g = p[1].toInt(&b), bl = p[2].toInt(&c); if (a && b && c) pal << qRgb(r, g, bl); }
        }
    } else if (d.startsWith("JASC-PAL")) {
        const QList<QByteArray> lines = d.split('\n');
        for (int i = 3; i < lines.size(); ++i) {
            const QList<QByteArray> p = lines[i].simplified().split(' ');
            if (p.size() >= 3) pal << qRgb(p[0].toInt(), p[1].toInt(), p[2].toInt());
        }
    } else if (d.startsWith("RIFF") && d.mid(8, 4) == "PAL ") {
        const int n = d.size() >= 24 ? (uchar(d[22]) | uchar(d[23]) << 8) : 0;
        for (int i = 0; i < n && 24 + i * 4 + 3 <= d.size(); ++i) pal << qRgb(uchar(d[24 + i * 4]), uchar(d[25 + i * 4]), uchar(d[26 + i * 4]));
    } else if (ext == QLatin1String("act") && (d.size() == 768 || d.size() == 772)) {
        int n = 256;
        if (d.size() == 772) n = qBound(1, (uchar(d[768]) << 8) | uchar(d[769]), 256);
        for (int i = 0; i < n; ++i) pal << qRgb(uchar(d[i * 3]), uchar(d[i * 3 + 1]), uchar(d[i * 3 + 2]));
    } else {
        QImageReader r(path);
        const QImage img = r.read();
        if (img.isNull()) { if (err) *err = QStringLiteral("Unrecognised palette file"); return pal; }
        if (img.colorCount() > 0) for (QRgb c : img.colorTable()) pal << (c | 0xFF000000u);
        else pal = exactColors({img}, 256);
        if (pal.isEmpty()) { if (err) *err = QStringLiteral("The image has more than 256 colours"); return pal; }
    }
    if (pal.size() > 256) pal.resize(256);
    if (pal.isEmpty() && err) *err = QStringLiteral("The palette is empty");
    return pal;
}

bool savePalette(const QString &path, const QVector<QRgb> &pal, QString *err) {
    QByteArray d;
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("gpl")) {
        d = QByteArray("GIMP Palette\nName: ") + LW_PRODUCT_NAME + "\nColumns: 16\n#\n";
        for (QRgb c : pal) d += QByteArray::number(qRed(c)) + ' ' + QByteArray::number(qGreen(c)) + ' ' + QByteArray::number(qBlue(c)) + "\tUntitled\n";
    } else if (ext == QLatin1String("pal")) {
        // JASC-PAL (Paint Shop Pro / Aseprite / GraphicsGale text palette)
        d = "JASC-PAL\r\n0100\r\n" + QByteArray::number(pal.size()) + "\r\n";
        for (QRgb c : pal) d += QByteArray::number(qRed(c)) + ' ' + QByteArray::number(qGreen(c)) + ' ' + QByteArray::number(qBlue(c)) + "\r\n";
    } else {
        d.fill(0, 772);
        for (int i = 0; i < pal.size() && i < 256; ++i) { d[i * 3] = char(qRed(pal[i])); d[i * 3 + 1] = char(qGreen(pal[i])); d[i * 3 + 2] = char(qBlue(pal[i])); }
        d[768] = char(pal.size() >> 8); d[769] = char(pal.size()); d[770] = char(0xFF); d[771] = char(0xFF);
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(d) != d.size()) { if (err) *err = f.errorString(); return false; }
    return true;
}

} // namespace quant
