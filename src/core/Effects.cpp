#include "Effects.h"
#include "FxExtra.h"
#include <QtMath>
#include <cmath>
#include <vector>
#include <cstring>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentMap>

namespace fx {

// Box blur, three passes of radius r (approximates a gaussian with sigma ~ radius). P6 (0.3.4):
// rows run all channels in one sweep; columns run as row-major running sums over strips of
// columns (cache friendly); both are spread over a private thread pool; the rounding divide is a
// 64-bit reciprocal multiply, exact for every window up to kMaxRecipWin (checked exhaustively:
// floor((s + win/2) / win) == ((s + win/2) * ceil(2^40 / win)) >> 40 for s <= 255 * win).
// The result is bit-identical to the previous scalar version.
namespace {
constexpr int kMaxRecipWin = 40001;

struct Divider {
    int win, half;
    quint64 mul;
    explicit Divider(int r) : win(2 * r + 1), half(r), mul(((quint64(1) << 40) + quint64(win) - 1) / quint64(win)) {}
    uchar operator()(int sum) const {
        return win <= kMaxRecipWin ? uchar((quint64(sum + half) * mul) >> 40) : uchar((sum + half) / win);
    }
};

template <int CH> void boxRow(const uchar *s, uchar *d, int n, int r, const Divider &div) {
    int sum[CH];
    for (int c = 0; c < CH; ++c) sum[c] = 0;
    for (int i = -r; i <= r; ++i) { const int k = qBound(0, i, n - 1); for (int c = 0; c < CH; ++c) sum[c] += s[k * CH + c]; }
    for (int i = 0; i < n; ++i) {
        const int add = qMin(i + r + 1, n - 1), sub = qMax(i - r, 0);
        for (int c = 0; c < CH; ++c) { d[i * CH + c] = div(sum[c]); sum[c] += s[add * CH + c] - s[sub * CH + c]; }
    }
}

QThreadPool *blurPool() {
    // a private pool: effects also render on the global pool (live previews, batch), and nesting
    // blocking maps on one pool can starve it
    static QThreadPool *pool = [] { auto *tp = new QThreadPool; tp->setMaxThreadCount(qMax(2, QThread::idealThreadCount())); return tp; }();
    return pool;
}

// fn(begin, end) over [0, n) in chunks of at most `chunk`, in parallel when the image is big enough to pay for it
template <typename Fn> void parallelChunks(int n, int chunk, bool parallel, Fn fn) {
    if (!parallel || n <= chunk) { for (int b = 0; b < n; b += chunk) fn(b, qMin(n, b + chunk)); return; }
    QVector<int> starts;
    for (int i = 0; i < n; i += chunk) starts << i;
    QtConcurrent::blockingMap(blurPool(), starts, [&](int b) { fn(b, qMin(n, b + chunk)); });
}
} // namespace

void blurBuffer(uchar *data, int w, int h, int bpl, int ch, double radius) {
    if (radius <= 0.01 || w <= 0 || h <= 0) return;
    const int r = qMax(1, qRound(radius * 0.8));
    const Divider div(r);
    const bool parallel = qint64(w) * h >= 256 * 256;
    const size_t rowBytes = size_t(w) * ch;
    std::vector<uchar> tmp(rowBytes * size_t(h));
    for (int pass = 0; pass < 3; ++pass) {
        // horizontal: each row from a copy of itself
        parallelChunks(h, 16, parallel, [&](int y0, int y1) {
            std::vector<uchar> line(rowBytes);
            for (int y = y0; y < y1; ++y) {
                uchar *row = data + size_t(y) * bpl;
                std::memcpy(line.data(), row, rowBytes);
                if (ch == 4) boxRow<4>(line.data(), row, w, r, div);
                else boxRow<1>(line.data(), row, w, r, div);
            }
        });
        // vertical: running sums down strips of byte columns, reading a packed copy
        for (int y = 0; y < h; ++y) std::memcpy(tmp.data() + size_t(y) * rowBytes, data + size_t(y) * bpl, rowBytes);
        constexpr int kStrip = 256;
        parallelChunks(int(rowBytes), kStrip, parallel, [&](int x0, int x1) {
            int sum[kStrip];
            const int n = x1 - x0;
            for (int x = 0; x < n; ++x) sum[x] = 0;
            for (int i = -r; i <= r; ++i) {
                const uchar *row = tmp.data() + size_t(qBound(0, i, h - 1)) * rowBytes + x0;
                for (int x = 0; x < n; ++x) sum[x] += row[x];
            }
            for (int y = 0; y < h; ++y) {
                uchar *dst = data + size_t(y) * bpl + x0;
                const uchar *ad = tmp.data() + size_t(qMin(y + r + 1, h - 1)) * rowBytes + x0;
                const uchar *sb = tmp.data() + size_t(qMax(y - r, 0)) * rowBytes + x0;
                for (int x = 0; x < n; ++x) { dst[x] = div(sum[x]); sum[x] += ad[x] - sb[x]; }
            }
        });
    }
}

void blur(QImage &img, double radius) {
    if (img.isNull()) return;
    if (img.format() == QImage::Format_Alpha8 || img.format() == QImage::Format_Grayscale8) {
        blurBuffer(img.bits(), img.width(), img.height(), img.bytesPerLine(), 1, radius);
    } else {
        if (img.format() != QImage::Format_ARGB32_Premultiplied) img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        blurBuffer(img.bits(), img.width(), img.height(), img.bytesPerLine(), 4, radius);
    }
}

QImage alphaMask(const QImage &src) {
    QImage a = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage m(a.size(), QImage::Format_Alpha8);
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        uchar *d = m.scanLine(y);
        for (int x = 0; x < a.width(); ++x) d[x] = uchar(qAlpha(s[x]));
    }
    return m;
}

QImage colorize(const QImage &mask, const QColor &c, double opacity) {
    QImage out(mask.size(), QImage::Format_ARGB32_Premultiplied);
    const double k = qBound(0.0, opacity, 1.0) * c.alphaF();
    const int cr = c.red(), cg = c.green(), cb = c.blue();
    const int w = mask.width(), h = mask.height();
    for (int y = 0; y < h; ++y) {
        const uchar *s = mask.constScanLine(y);
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = qRound(s[x] * k);
            d[x] = qRgba(cr * a / 255, cg * a / 255, cb * a / 255, a);
        }
    }
    return out;
}

void boostMask(QImage &mask, double f) {
    for (int y = 0; y < mask.height(); ++y) {
        uchar *s = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) s[x] = uchar(qMin(255.0, s[x] * f));
    }
}

void invertMask(QImage &mask) {
    for (int y = 0; y < mask.height(); ++y) {
        uchar *s = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) s[x] = uchar(255 - s[x]);
    }
}

void maskBy(QImage &argb, const QImage &mask) {
    for (int y = 0; y < argb.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(argb.scanLine(y));
        const uchar *m = mask.constScanLine(y);
        for (int x = 0; x < argb.width(); ++x) {
            int a = m[x];
            QRgb p = d[x];
            d[x] = qRgba(qRed(p) * a / 255, qGreen(p) * a / 255, qBlue(p) * a / 255, qAlpha(p) * a / 255);
        }
    }
}

static QPointF offsetFor(const LiveEffect &e) {
    double a = qDegreesToRadians(e.angle);
    return QPointF(std::cos(a) * e.distance, -std::sin(a) * e.distance);
}


// Shade the edges of `mask` (Alpha8) lit from e.angle. `flip` swaps lit/shadowed sides.
// Returns an ARGB32_Premultiplied overlay limited to `limit` (or mask when null).
static QImage shadeImage(const QImage &mask, double width, double opacity, double angle, bool flip,
                         const QColor &hi, const QColor &lo, const QImage &limit = QImage()) {
    QImage m = mask.copy();
    blur(m, qMax(1.0, width));
    const QImage &lim = limit.isNull() ? mask : limit;
    QImage out(mask.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(0);
    const int w = m.width(), h = m.height();
    const double k = opacity / 100.0;
    const double scale = 6.0 / qMax(1.0, width / 2.0);
    const double a = qDegreesToRadians(angle);
    const double lx = std::cos(a), ly = -std::sin(a); // light direction (image coords, y down)
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        const uchar *orig = lim.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (!orig[x]) continue;
            int xl = qMax(0, x - 1), xr = qMin(w - 1, x + 1), yu = qMax(0, y - 1), yd = qMin(h - 1, y + 1);
            double dx = (m.constScanLine(y)[xr] - m.constScanLine(y)[xl]) / 2.0;
            double dy = (m.constScanLine(yd)[x] - m.constScanLine(yu)[x]) / 2.0;
            // the mask gradient points inwards; the outward surface normal is its negation
            double s = -(dx * lx + dy * ly) * M_SQRT2 * scale / 255.0 * 2.0; // positive = facing the light
            if (flip) s = -s;
            s = qBound(-1.0, s, 1.0);
            const QColor &c = s > 0 ? hi : lo;
            int al = qRound(std::abs(s) * 255 * k * orig[x] / 255.0);
            d[x] = qRgba(c.red() * al / 255, c.green() * al / 255, c.blue() * al / 255, al);
        }
    }
    return out;
}

static QImage bevelImage(const QImage &mask, const LiveEffect &e) {
    const bool flip = e.preset == BevelPreset::Inset || e.preset == BevelPreset::Inverted;
    QImage out = shadeImage(mask, e.width, e.opacity, e.angle, flip, e.color, Qt::black);
    if (e.preset == BevelPreset::Highlighted || e.preset == BevelPreset::Inverted) {
        QImage tint = colorize(mask, e.preset == BevelPreset::Highlighted ? QColor(Qt::white) : QColor(Qt::black), 0.25);
        QPainter p(&tint);
        p.drawImage(0, 0, out);
        p.end();
        return tint;
    }
    return out;
}

// Outer bevel: a shaded frame of e.color around the outside of the object.
static QImage outerBevelImage(const QImage &mask, const LiveEffect &e) {
    QImage dil = mask.copy();
    blur(dil, qMax(0.5, e.width / 1.5));
    boostMask(dil, 4.0);
    if (e.softness > 0) blur(dil, e.softness / 2.0);
    QImage ring = colorize(dil, e.color, 1.0);
    const bool flip = e.preset == BevelPreset::Inset || e.preset == BevelPreset::Inverted;
    QImage shade = shadeImage(dil, e.width / 2.0, e.opacity, e.angle, flip, Qt::white, Qt::black, dil);
    QPainter p(&ring);
    p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
    if (e.preset == BevelPreset::Highlighted) p.fillRect(ring.rect(), QColor(255, 255, 255, 64));
    if (e.preset == BevelPreset::Inverted) p.fillRect(ring.rect(), QColor(0, 0, 0, 64));
    p.drawImage(0, 0, shade);
    p.end();
    return ring;
}

static inline int c255(double v) { return qBound(0, int(std::lround(v)), 255); }

// Builds a 256-entry tone curve through `pts` (monotone cubic / Fritsch-Carlson).
QVector<uchar> curveLut(const QPolygonF &ptsIn) {
    QVector<QPointF> pts(ptsIn.begin(), ptsIn.end());
    std::sort(pts.begin(), pts.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    QVector<uchar> lut(256);
    if (pts.size() < 2) { for (int i = 0; i < 256; ++i) lut[i] = uchar(i); return lut; }
    const int n = pts.size();
    QVector<double> dl(n - 1), m(n);
    for (int i = 0; i < n - 1; ++i) {
        double dx = qMax(1e-6, pts[i + 1].x() - pts[i].x());
        dl[i] = (pts[i + 1].y() - pts[i].y()) / dx;
    }
    m[0] = dl[0]; m[n - 1] = dl[n - 2];
    for (int i = 1; i < n - 1; ++i) m[i] = (dl[i - 1] * dl[i] <= 0) ? 0 : (dl[i - 1] + dl[i]) / 2;
    for (int i = 0; i < n - 1; ++i) {
        if (dl[i] == 0) { m[i] = m[i + 1] = 0; continue; }
        double a = m[i] / dl[i], b = m[i + 1] / dl[i], t = a * a + b * b;
        if (t > 9) { double tau = 3 / std::sqrt(t); m[i] = tau * a * dl[i]; m[i + 1] = tau * b * dl[i]; }
    }
    for (int x = 0; x < 256; ++x) {
        double y;
        if (x <= pts.first().x()) y = pts.first().y();
        else if (x >= pts.last().x()) y = pts.last().y();
        else {
            int i = 0;
            while (i < n - 2 && x > pts[i + 1].x()) ++i;
            double hgt = pts[i + 1].x() - pts[i].x(), t = (x - pts[i].x()) / hgt;
            double t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * pts[i].y() + (t3 - 2 * t2 + t) * hgt * m[i] +
                (-2 * t3 + 3 * t2) * pts[i + 1].y() + (t3 - t2) * hgt * m[i + 1];
        }
        lut[x] = uchar(c255(y));
    }
    return lut;
}

QVector<uchar> levelsLut(const LevelsChannel &c) {
    QVector<uchar> lut(256);
    const double lo = c.inBlack, hi = qMax(c.inBlack + 1, c.inWhite), g = qMax(0.01, c.gamma);
    for (int i = 0; i < 256; ++i) {
        double v = qBound(0.0, (i - lo) / (hi - lo), 1.0);
        v = std::pow(v, 1.0 / g);
        lut[i] = uchar(c255(c.outBlack + v * (c.outWhite - c.outBlack)));
    }
    return lut;
}

// Per-channel tables: channel table first, then the RGB master.
static void channelLuts(const LiveEffect &e, bool curves, QVector<uchar> out[3]) {
    QVector<uchar> master = curves ? curveLut(e.curves[ChRGB]) : levelsLut(e.levels[ChRGB]);
    for (int c = 0; c < 3; ++c) {
        QVector<uchar> ch = curves ? curveLut(e.curves[c + 1]) : levelsLut(e.levels[c + 1]);
        out[c].resize(256);
        for (int i = 0; i < 256; ++i) out[c][i] = master[ch[i]];
    }
}

LevelsChannel autoLevelsChannel(const QImage &imgIn, int channel) {
    QImage img = imgIn.convertToFormat(QImage::Format_ARGB32);
    QVector<int> hist = histogram(img, channel);
    int n = 0;
    for (int v : hist) n += v;
    LevelsChannel lc;
    if (!n) return lc;
    int lo = 0, hi = 255, acc = 0;
    while (lo < 254 && (acc += hist[lo]) <= n / 200) ++lo;
    acc = 0;
    while (hi > lo + 1 && (acc += hist[hi]) <= n / 200) --hi;
    lc.inBlack = lo; lc.inWhite = hi;
    return lc;
}

QVector<int> histogram(const QImage &imgIn, int channel) {
    QImage img = imgIn.convertToFormat(QImage::Format_ARGB32);
    QVector<int> hist(256, 0);
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (!qAlpha(s[x]) && channel != ChAlpha) continue;
            int v = channel == ChRed ? qRed(s[x]) : channel == ChGreen ? qGreen(s[x]) : channel == ChBlue ? qBlue(s[x])
                  : channel == ChAlpha ? qAlpha(s[x]) : qGray(s[x]);
            ++hist[v];
        }
    }
    return hist;
}

static inline quint32 hash32(quint32 x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

// Per-pixel colour operation on straight (non-premultiplied) RGBA.
template <typename F> static void perPixel(QImage &img, F f) {
    QImage s = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < s.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
        for (int x = 0; x < s.width(); ++x) {
            if (!qAlpha(d[x])) continue;
            int r = qRed(d[x]), g = qGreen(d[x]), b = qBlue(d[x]), a = qAlpha(d[x]);
            f(x, y, r, g, b, a);
            d[x] = qRgba(c255(r), c255(g), c255(b), c255(a));
        }
    }
    img = s.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

static void sharpenImg(QImage &src, double radius, double amt, int threshold) {
    QImage bl = src.copy();
    blur(bl, radius);
    const int w = src.width(), h = src.height();
    const bool parallel = qint64(w) * h >= 256 * 256;
    parallelChunks(h, 16, parallel, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(src.scanLine(y));
            const QRgb *b = reinterpret_cast<const QRgb *>(bl.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                const int a = qAlpha(d[x]);
                auto f = [&](int o, int bb) {
                    if (std::abs(o - bb) <= threshold) return o;
                    return qBound(0, int(std::lround(o + (o - bb) * amt)), a);
                };
                d[x] = qRgba(f(qRed(d[x]), qRed(b[x])), f(qGreen(d[x]), qGreen(b[x])), f(qBlue(d[x]), qBlue(b[x])), a);
            }
        }
    });
}

// High Pass: original minus its Gaussian blur, re-centred on 50% grey (alpha kept).
QImage highPass(const QImage &srcIn, double radius, bool keepColor) {
    QImage src = srcIn.convertToFormat(QImage::Format_ARGB32);
    QImage bl = srcIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    blur(bl, qMax(0.1, radius));
    bl = bl.convertToFormat(QImage::Format_ARGB32);
    QImage out(src.size(), QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(bl.constScanLine(y));
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            if (keepColor) {
                d[x] = qRgba(c255(qRed(s[x]) - qRed(b[x]) + 128.0), c255(qGreen(s[x]) - qGreen(b[x]) + 128.0),
                             c255(qBlue(s[x]) - qBlue(b[x]) + 128.0), qAlpha(s[x]));
            } else {
                const int v = c255(qGray(s[x]) - qGray(b[x]) + 128.0);
                d[x] = qRgba(v, v, v, qAlpha(s[x]));
            }
        }
    }
    return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Offset: shift by (dx, dy) whole pixels; uncovered areas wrap, repeat the edge pixels or stay transparent.
QImage offset(const QImage &srcIn, int dx, int dy, int undefinedAreas) {
    QImage src = srcIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = src.width(), h = src.height();
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    if (w <= 0 || h <= 0) return out;
    auto wrap = [](int v, int n) { v %= n; return v < 0 ? v + n : v; };
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        int sy = y - dy;
        bool outY = sy < 0 || sy >= h;
        if (undefinedAreas == LiveEffect::WrapAround) sy = wrap(sy, h);
        else sy = qBound(0, sy, h - 1);
        const QRgb *s = reinterpret_cast<const QRgb *>(src.constScanLine(sy));
        for (int x = 0; x < w; ++x) {
            int sx = x - dx;
            const bool outX = sx < 0 || sx >= w;
            if (undefinedAreas == LiveEffect::Transparent && (outX || outY)) { d[x] = 0; continue; }
            sx = undefinedAreas == LiveEffect::WrapAround ? wrap(sx, w) : qBound(0, sx, w - 1);
            d[x] = s[sx];
        }
    }
    return out;
}

// Nearest-site propagation (two-pass 8-neighbour sweep, as in 8SSEDT): for every pixel the offset
// to the nearest pixel with site[] set. Accurate to a fraction of a pixel, which is plenty for fades.
void nearestSites(const QVector<char> &site, int w, int h, QVector<QPoint> &vec) {
    const int INF = 1 << 14;
    vec.fill(QPoint(INF, INF), w * h);
    for (int i = 0; i < w * h; ++i) if (site[i]) vec[i] = QPoint(0, 0);
    auto d2 = [](const QPoint &p) { return qint64(p.x()) * p.x() + qint64(p.y()) * p.y(); };
    auto cmp = [&](int x, int y, int ox, int oy) {
        const int nx = x + ox, ny = y + oy;
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;
        const QPoint c = vec[ny * w + nx] + QPoint(ox, oy);
        if (d2(c) < d2(vec[y * w + x])) vec[y * w + x] = c;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) { cmp(x, y, -1, 0); cmp(x, y, 0, -1); cmp(x, y, -1, -1); cmp(x, y, 1, -1); }
        for (int x = w - 1; x >= 0; --x) cmp(x, y, 1, 0);
    }
    for (int y = h - 1; y >= 0; --y) {
        for (int x = w - 1; x >= 0; --x) { cmp(x, y, 1, 0); cmp(x, y, 0, 1); cmp(x, y, 1, 1); cmp(x, y, -1, 1); }
        for (int x = 0; x < w; ++x) cmp(x, y, -1, 0);
    }
}

// Feather Edges: fade the alpha of the shape's edges over `radius` px. Inside: the fade runs from
// the edge inward (the outline stays put); centred: half inside, half outside (the edge colours
// are extended outward). `softness` 0 = hard (alpha drops close to the edge) .. 100 = soft
// (a smooth ramp over the whole radius).
QImage featherEdges(const QImage &srcIn, double radius, bool centered, double softness) {
    QImage src = srcIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (radius <= 0 || src.isNull()) return src;
    const int pad = centered ? int(std::ceil(radius / 2)) + 1 : 1;   // the area outside counts as edge
    const int W = src.width() + 2 * pad, H = src.height() + 2 * pad;
    QVector<char> in(W * H, 0), out(W * H, 1);
    bool any = false;
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x)
            if (qAlpha(s[x]) >= 128) { const int i = (y + pad) * W + x + pad; in[i] = 1; out[i] = 0; any = true; }
    }
    if (!any) return src;
    QVector<QPoint> toOut, toIn;
    nearestSites(out, W, H, toOut);
    if (centered) nearestSites(in, W, H, toIn);
    const double soft = qBound(0.0, softness / 100.0, 1.0);
    auto falloff = [soft](double t) {
        const double u = qBound(0.0, t, 1.0);
        return (1 - soft) * std::pow(u, 0.25) + soft * u * u * (3 - 2 * u);
    };
    QImage res = src.copy();
    for (int y = 0; y < src.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(res.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const int i = (y + pad) * W + x + pad;
            double sd;   // signed distance to the edge: + inside, - outside
            if (in[i]) { const QPoint v = toOut[i]; sd = std::sqrt(double(v.x()) * v.x() + double(v.y()) * v.y()) - 0.5; }
            else if (centered) { const QPoint v = toIn[i]; sd = -(std::sqrt(double(v.x()) * v.x() + double(v.y()) * v.y()) - 0.5); }
            else {   // inside mode: the anti-aliased rim just outside the edge fades with the edge
                const double f = falloff(0.25 / radius);
                const QRgb p = d[x];
                d[x] = qRgba(int(std::lround(qRed(p) * f)), int(std::lround(qGreen(p) * f)), int(std::lround(qBlue(p) * f)), int(std::lround(qAlpha(p) * f)));
                continue;
            }
            const double t = centered ? (sd + radius / 2.0) / radius : sd / radius;
            const double f = falloff(t);
            if (in[i]) {
                const QRgb p = d[x];
                d[x] = qRgba(int(std::lround(qRed(p) * f)), int(std::lround(qGreen(p) * f)), int(std::lround(qBlue(p) * f)), int(std::lround(qAlpha(p) * f)));
            } else {
                if (f <= 0) { d[x] = 0; continue; }
                // outside: the colour of the nearest inside pixel, faded
                const QPoint v = toIn[i];
                const int sx = qBound(0, x + v.x(), src.width() - 1), sy = qBound(0, y + v.y(), src.height() - 1);
                const QRgb c = qUnpremultiply(reinterpret_cast<const QRgb *>(src.constScanLine(sy))[sx]);
                const int a = qMax(qAlpha(d[x]), int(std::lround(255 * f)));
                d[x] = qPremultiply(qRgba(qRed(c), qGreen(c), qBlue(c), a));
            }
        }
    }
    return res;
}

// Make Seamless Tile: offset by half the size (wrapping) so the old edges meet in the middle, then
// cross-fade those seams with the unshifted image over `blend` pixels. The new outer edges are
// interior pixels of the original, so the result tiles without visible joins.
QImage makeSeamless(const QImage &srcIn, double blendWidth) {
    QImage src = srcIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = src.width(), h = src.height();
    const QImage O = offset(src, w / 2, h / 2, LiveEffect::WrapAround);
    if (blendWidth <= 0 || w < 2 || h < 2) return O;
    // Bilinear cross-fade between the four half-offset variants: near the vertical seam use a copy
    // that is not shifted in x (continuous there), near the horizontal seam one not shifted in y.
    // At the outer edges the fully offset image is used, so the tile wraps without joins.
    const QImage A = offset(src, 0, h / 2, LiveEffect::WrapAround);
    const QImage S = offset(src, w / 2, 0, LiveEffect::WrapAround);
    const double half = qMax(0.5, blendWidth / 2.0);
    const double sx = double(w / 2) - 0.5, sy = double(h / 2) - 0.5; // seams between pixel w/2-1 and w/2 (truncating mid)
    auto weight = [half](double d) { double t = qBound(0.0, 1.0 - std::abs(d) / half, 1.0); return t * t * (3 - 2 * t); };
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        const QRgb *o = reinterpret_cast<const QRgb *>(O.constScanLine(y));
        const QRgb *a = reinterpret_cast<const QRgb *>(A.constScanLine(y));
        const QRgb *s2 = reinterpret_cast<const QRgb *>(S.constScanLine(y));
        const QRgb *i = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        const double ty = weight(y - sy);
        for (int x = 0; x < w; ++x) {
            const double tx = weight(x - sx);
            const double wo = (1 - tx) * (1 - ty), wa = tx * (1 - ty), ws = (1 - tx) * ty, wi = tx * ty;
            auto ch = [&](int (*f)(QRgb)) { return int(std::lround(f(o[x]) * wo + f(a[x]) * wa + f(s2[x]) * ws + f(i[x]) * wi)); };
            d[x] = qRgba(ch(qRed), ch(qGreen), ch(qBlue), ch(qAlpha));
        }
    }
    return out;
}

// Motion / Radial (spin) / Zoom blur: average of transformed samples (premultiplied, transparent outside).
static QImage sampledBlur(const QImage &src, const LiveEffect &e) {
    const int w = src.width(), h = src.height();
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(0);
    if (w == 0 || h == 0) return out;
    const QPointF ctr(w / 2.0, h / 2.0);
    QVector<QTransform> xf;
    if (e.type == EffectType::MotionBlur) {
        const int n = qMax(1, int(std::ceil(e.distance)) + 1);
        const double a = qDegreesToRadians(e.angle);
        for (int i = 0; i < n; ++i) {
            double t = n == 1 ? 0 : (double(i) / (n - 1) - 0.5) * e.distance;
            xf << QTransform::fromTranslate(std::cos(a) * t, -std::sin(a) * t);
        }
    } else {
        const int n = qBound(2, e.quality, 100);
        for (int i = 0; i < n; ++i) {
            double t = double(i) / (n - 1) - 0.5; // -0.5..0.5
            QTransform m;
            m.translate(ctr.x(), ctr.y());
            if (e.type == EffectType::RadialBlur) m.rotate(t * e.amount);
            else { double sc = 1.0 + (t - 0.5) * e.amount / 100.0; m.scale(sc, sc); } // zoom towards the centre
            m.translate(-ctr.x(), -ctr.y());
            xf << m;
        }
    }
    const int n = xf.size();
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int sr = 0, sg = 0, sb = 0, sa = 0;
            for (const QTransform &m : xf) {
                QPointF p = m.map(QPointF(x + 0.5, y + 0.5));
                int sx = int(std::floor(p.x())), sy = int(std::floor(p.y()));
                if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                QRgb c = reinterpret_cast<const QRgb *>(src.constScanLine(sy))[sx];
                sr += qRed(c); sg += qGreen(c); sb += qBlue(c); sa += qAlpha(c);
            }
            d[x] = qRgba(sr / n, sg / n, sb / n, sa / n);
        }
    }
    return out;
}

void adjust(QImage &img, const LiveEffect &e) {
    if (img.format() != QImage::Format_ARGB32_Premultiplied) img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (fx2::apply(img, e)) return;   // v0.3.0 effects
    switch (e.type) {
    case EffectType::Blur: blur(img, 1); break;
    case EffectType::BlurMore: blur(img, 2.5); break;
    case EffectType::GaussianBlur: blur(img, qMax(0.1, e.softness)); break;
    case EffectType::Sharpen: sharpenImg(img, 1, 0.6, 0); break;
    case EffectType::SharpenMore: sharpenImg(img, 1, 1.4, 0); break;
    case EffectType::UnsharpMask: sharpenImg(img, qMax(0.1, e.softness), e.amount / 100.0 * 2.0, e.threshold); break;
    case EffectType::Invert: perPixel(img, [](int, int, int &r, int &g, int &b, int &) { r = 255 - r; g = 255 - g; b = 255 - b; }); break;
    case EffectType::BrightnessContrast: {
        const double c = (100.0 + e.contrast) / 100.0, br = e.brightness * 2.55;
        perPixel(img, [&](int, int, int &r, int &g, int &b, int &) {
            auto f = [&](int v) { return (v - 128) * c + 128 + br; };
            r = int(std::lround(f(r))); g = int(std::lround(f(g))); b = int(std::lround(f(b)));
        });
        break;
    }
    case EffectType::Levels: case EffectType::Curves: case EffectType::AutoLevels: {
        LiveEffect le = e;
        if (e.type == EffectType::AutoLevels) {
            le.levels[ChRGB] = LevelsChannel();
            for (int c = ChRed; c <= ChBlue; ++c) le.levels[c] = autoLevelsChannel(img, c);
        }
        QVector<uchar> lut[3];
        channelLuts(le, e.type == EffectType::Curves, lut);
        const QVector<uchar> alut = e.type == EffectType::Curves ? curveLut(le.curves[ChAlpha]) : levelsLut(le.levels[ChAlpha]);
        bool alphaId = true;
        for (int i = 0; i < 256; ++i) if (alut[i] != i) { alphaId = false; break; }
        perPixel(img, [&](int, int, int &r, int &g, int &b, int &a) { r = lut[0][r]; g = lut[1][g]; b = lut[2][b]; if (!alphaId) a = alut[a]; });
        break;
    }
    case EffectType::Desaturate:
        perPixel(img, [](int, int, int &r, int &g, int &b, int &) { r = g = b = qGray(r, g, b); });
        break;
    case EffectType::Threshold:
        perPixel(img, [&](int, int, int &r, int &g, int &b, int &) { r = g = b = qGray(r, g, b) >= e.threshold ? 255 : 0; });
        break;
    case EffectType::Posterize: {
        const int n = qBound(2, e.posterize, 255);
        perPixel(img, [n](int, int, int &r, int &g, int &b, int &) {
            auto q = [n](int v) { return int(std::lround(std::lround(v * (n - 1) / 255.0) * 255.0 / (n - 1))); };
            r = q(r); g = q(g); b = q(b);
        });
        break;
    }
    case EffectType::ColorBalance: {
        perPixel(img, [&](int, int, int &r, int &g, int &b, int &) {
            const double l = qGray(r, g, b) / 255.0;
            const double w[3] = {(1 - l) * (1 - l), 1 - (1 - l) * (1 - l) - l * l, l * l};
            double d[3] = {0, 0, 0};
            for (int rg = 0; rg < 3; ++rg) for (int c = 0; c < 3; ++c) d[c] += w[rg] * e.balance[rg][c] * 1.28;
            double nr = r + d[0], ng = g + d[1], nb = b + d[2];
            if (e.preserveLuminosity) {
                double shift = (0.299 * r + 0.587 * g + 0.114 * b) - (0.299 * nr + 0.587 * ng + 0.114 * nb);
                nr += shift; ng += shift; nb += shift;
            }
            r = int(std::lround(nr)); g = int(std::lround(ng)); b = int(std::lround(nb));
        });
        break;
    }
    case EffectType::PhotoFilter: {
        const double dens = qBound(0.0, e.amount / 100.0, 1.0);
        const int fr = e.color.red(), fg = e.color.green(), fb = e.color.blue();
        perPixel(img, [&](int, int, int &r, int &g, int &b, int &) {
            double nr = r + (r * fr / 255.0 - r) * dens, ng = g + (g * fg / 255.0 - g) * dens, nb = b + (b * fb / 255.0 - b) * dens;
            if (e.preserveLuminosity) {
                double ol = 0.299 * r + 0.587 * g + 0.114 * b, nl = 0.299 * nr + 0.587 * ng + 0.114 * nb;
                if (nl > 0.5) { double k = ol / nl; nr *= k; ng *= k; nb *= k; }
                else { nr = ng = nb = ol; }
            }
            r = int(std::lround(nr)); g = int(std::lround(ng)); b = int(std::lround(nb));
        });
        break;
    }
    case EffectType::MotionBlur: case EffectType::RadialBlur: case EffectType::ZoomBlur:
        img = sampledBlur(img, e);
        break;
    case EffectType::HueSaturation: {
        const double lt = e.lightness / 100.0;
        perPixel(img, [&](int, int, int &r, int &g, int &b, int &) {
            float h, s, l;
            QColor::fromRgb(r, g, b).getHslF(&h, &s, &l);
            if (h < 0) h = 0;
            if (e.colorize) {
                h = float(std::fmod(std::fmod(e.hue, 360.0) + 360.0, 360.0) / 360.0);
                s = float(qBound(0.0, (e.saturation + 100.0) / 200.0, 1.0));
            } else {
                h = float(std::fmod(h + e.hue / 360.0 + 2.0, 1.0));
                s = float(qBound(0.0, s * (1.0 + e.saturation / 100.0), 1.0));
            }
            l = float(lt > 0 ? l + (1 - l) * lt : l + l * lt);
            QColor c = QColor::fromHslF(h, s, qBound(0.0f, l, 1.0f));
            r = c.red(); g = c.green(); b = c.blue();
        });
        break;
    }
    case EffectType::ColorFill: {
        QImage orig = img.convertToFormat(QImage::Format_ARGB32);
        QImage filled = img.copy();
        {
            QColor c = e.color;
            c.setAlphaF(qBound(0.0, e.opacity / 100.0, 1.0));
            QImage wash(filled.size(), QImage::Format_ARGB32_Premultiplied);
            wash.fill(c.rgba());
            blitBlend(filled, wash, QPoint(0, 0), e.blend, 1);
        }
        filled = filled.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < orig.height(); ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(orig.scanLine(y));
            const QRgb *f = reinterpret_cast<const QRgb *>(filled.constScanLine(y));
            for (int x = 0; x < orig.width(); ++x)
                if (qAlpha(d[x])) d[x] = qRgba(qRed(f[x]), qGreen(f[x]), qBlue(f[x]), qAlpha(d[x]));
        }
        img = orig.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        break;
    }
    case EffectType::Noise: {
        const double amp = e.amount * 2.55;
        perPixel(img, [&](int x, int y, int &r, int &g, int &b, int &) {
            quint32 base = hash32(quint32(x) * 73856093U ^ quint32(y) * 19349663U ^ 0x9e3779b9U);
            auto n = [&](quint32 h) { return ((h & 0xffff) / 65535.0 - 0.5) * 2.0 * amp; };
            if (e.colorNoise) {
                r += int(n(base)); g += int(n(hash32(base + 1))); b += int(n(hash32(base + 2)));
            } else {
                int v = int(n(base));
                r += v; g += v; b += v;
            }
        });
        break;
    }
    case EffectType::FindEdges: {
        QImage s = img.convertToFormat(QImage::Format_ARGB32);
        QImage out = s.copy();
        const int w = s.width(), h = s.height();
        auto px = [&](int x, int y) { return reinterpret_cast<const QRgb *>(s.constScanLine(qBound(0, y, h - 1)))[qBound(0, x, w - 1)]; };
        for (int y = 0; y < h; ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                if (!qAlpha(d[x])) continue;
                int ch[3];
                for (int c = 0; c < 3; ++c) {
                    auto v = [&](int xx, int yy) { QRgb p = px(xx, yy); return c == 0 ? qRed(p) : c == 1 ? qGreen(p) : qBlue(p); };
                    int gx = -v(x - 1, y - 1) - 2 * v(x - 1, y) - v(x - 1, y + 1) + v(x + 1, y - 1) + 2 * v(x + 1, y) + v(x + 1, y + 1);
                    int gy = -v(x - 1, y - 1) - 2 * v(x, y - 1) - v(x + 1, y - 1) + v(x - 1, y + 1) + 2 * v(x, y + 1) + v(x + 1, y + 1);
                    ch[c] = 255 - c255(std::sqrt(double(gx * gx + gy * gy)) / 4.0);
                }
                d[x] = qRgba(ch[0], ch[1], ch[2], qAlpha(d[x]));
            }
        }
        img = out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        break;
    }
    case EffectType::HighPass: img = highPass(img, e.softness, e.keepColor); break;
    case EffectType::Offset: img = offset(img, int(std::lround(e.offsetX)), int(std::lround(e.offsetY)), e.undefinedAreas); break;
    case EffectType::FeatherEdges: img = featherEdges(img, e.width, e.featherCentered, e.amount); break;
    case EffectType::ConvertToAlpha:
        perPixel(img, [](int, int, int &r, int &g, int &b, int &a) {
            int gray = qGray(r, g, b);
            a = (255 - gray) * a / 255;
            r = g = b = 0;
        });
        break;
    default: break;
    }
}

// Min of two Alpha8 masks of the same size.
static QImage minMask(const QImage &a, const QImage &b) {
    QImage out = a.copy();
    for (int y = 0; y < out.height(); ++y) {
        uchar *d = out.scanLine(y);
        const uchar *s = b.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) d[x] = qMin(d[x], s[x]);
    }
    return out;
}

// Live effects form an ordered pipeline, top of the list first: each effect works on the
// composite of the object and every effect above it (halos of shadows and glows included) and
// hands its result to the next one. So a Hue/Saturation below a Glow recolours the glow, one
// above it does not; a blur below a Drop Shadow softens the shadow too.
//  - adjustments (colour, blur, sharpen, noise, feather...) transform the whole composite
//  - outer effects (Drop Shadow, Glow, Outer Bevel) are built from the composite's silhouette
//    and drawn beneath it
//  - inner effects (Inner Shadow/Glow/Bevel, Emboss) shade the object's own shape (not halos)
QRect alphaBounds(const QImage &imgIn) {
    const QImage img = imgIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(s[x])) { x0 = qMin(x0, x); x1 = qMax(x1, x); y0 = qMin(y0, y); y1 = qMax(y1, y); }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

QImage applyEffects(const QImage &baseIn, const QVector<LiveEffect> &effects, const QRect &objRect) {
    QImage cur = baseIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage shape0 = alphaMask(cur);
    for (const auto &e : effects) {
        if (!e.enabled) continue;
        if (e.type == EffectType::Offset || e.type == EffectType::NormalMap || e.type == EffectType::AmbientOcclusion) {
            // wrap within the object's own box (plus anything earlier effects drew), never through
            // the transparent padding of the render buffer -- that would show as a 1 px frame and
            // a 2 px seam where the wrapped edges meet
            QRect r = alphaBounds(cur);
            if (!objRect.isEmpty()) r = r.isEmpty() ? objRect : r.united(objRect);
            r &= cur.rect();
            if (r.isEmpty()) continue;
            // Normal Map / AO sample neighbours: their wrap (or edge clamp) must meet the object's
            // own opposite edge, not the buffer's transparent padding (a 1 px seam column/row)
            QImage moved = cur.copy(r);
            if (e.type == EffectType::Offset) moved = offset(moved, int(std::lround(e.offsetX)), int(std::lround(e.offsetY)), e.undefinedAreas);
            else adjust(moved, e);
            QPainter p(&cur);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(r.topLeft(), moved);
            continue;
        }
        if (effectIsAdjustment(e.type)) { adjust(cur, e); continue; }
        const QImage mask = alphaMask(cur);
        switch (e.type) {
        case EffectType::DropShadow: case EffectType::OuterGlow: case EffectType::OuterBevel: {
            QImage halo;
            QPointF at;
            if (e.type == EffectType::DropShadow) {
                QImage m = mask.copy();
                blur(m, e.softness);
                halo = colorize(m, e.color, e.opacity / 100.0);
                at = offsetFor(e);
            } else if (e.type == EffectType::OuterGlow) {
                QImage m = mask.copy();
                if (e.width > 0) { blur(m, e.width / 1.5); boostMask(m, 4.0); }
                blur(m, e.softness);
                halo = colorize(m, e.color, e.opacity / 100.0);
            } else {
                halo = outerBevelImage(mask, e);
            }
            QImage out(cur.size(), QImage::Format_ARGB32_Premultiplied);
            out.fill(0);
            QPainter p(&out);
            p.drawImage(at, halo);
            p.drawImage(0, 0, cur);
            p.end();
            cur = out;
            break;
        }
        case EffectType::InnerShadow: case EffectType::InnerGlow: case EffectType::InnerBevel:
        case EffectType::InsetEmboss: case EffectType::RaisedEmboss: {
            const QImage shape = minMask(shape0, mask);
            QImage layer;
            if (e.type == EffectType::InnerShadow) {
                QImage m = shape.copy();
                invertMask(m);
                QImage sm(m.size(), QImage::Format_Alpha8);
                sm.fill(255);
                QPoint off = offsetFor(e).toPoint();
                for (int y = 0; y < m.height(); ++y) {
                    int sy = y - off.y();
                    if (sy < 0 || sy >= m.height()) continue;
                    uchar *d = sm.scanLine(y);
                    const uchar *s = m.constScanLine(sy);
                    for (int x = 0; x < m.width(); ++x) { int sx = x - off.x(); if (sx >= 0 && sx < m.width()) d[x] = s[sx]; }
                }
                blur(sm, e.softness);
                layer = colorize(sm, e.color, e.opacity / 100.0);
                maskBy(layer, shape);
            } else if (e.type == EffectType::InnerGlow) {
                QImage m = shape.copy();
                invertMask(m);
                if (e.width > 0) { blur(m, e.width / 1.5); boostMask(m, 4.0); }
                blur(m, e.softness);
                layer = colorize(m, e.color, e.opacity / 100.0);
                maskBy(layer, shape);
            } else if (e.type == EffectType::InnerBevel) {
                layer = bevelImage(shape, e);
            } else {
                layer = shadeImage(shape, e.width, e.opacity, e.angle, e.type == EffectType::InsetEmboss, Qt::white, Qt::black);
                if (!e.showObject) {   // the emboss replaces the object (earlier halos stay)
                    QPainter q(&cur);
                    q.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                    q.drawImage(0, 0, colorize(shape, Qt::black, 1.0));
                }
            }
            QPainter p(&cur);
            p.drawImage(0, 0, layer);
            break;
        }
        default: break;
        }
    }
    return cur;
}

} // namespace fx
