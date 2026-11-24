#include "FxExtra.h"
#include "Effects.h"
#include "io/Quantize.h"
#include <QHash>
#include <QPainter>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <random>

namespace fx2 {
namespace {
inline int c255(double v) { return v <= 0 ? 0 : v >= 255 ? 255 : int(std::lround(v)); }
inline int ci(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
QImage straight(const QImage &img) { return img.convertToFormat(QImage::Format_ARGB32); }
QImage premul(const QImage &img) { return img.convertToFormat(QImage::Format_ARGB32_Premultiplied); }
inline double lum(QRgb c) { return 0.299 * qRed(c) + 0.587 * qGreen(c) + 0.114 * qBlue(c); }
inline double smooth(double e0, double e1, double x) {
    if (e1 <= e0) return x < e0 ? 0 : 1;
    double t = qBound(0.0, (x - e0) / (e1 - e0), 1.0);
    return t * t * (3 - 2 * t);
}
// bilinear sample of a premultiplied image; outside = transparent (or clamped / wrapped)
enum Edge { Transparent, Clamp, Wrap };
inline void sample(const QImage &img, double x, double y, double out[4], Edge edge = Transparent) {
    const int w = img.width(), h = img.height();
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
        int xx = x0 + i, yy = y0 + j;
        const double wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
        if (wgt <= 0) continue;
        if (edge == Wrap) { xx = ((xx % w) + w) % w; yy = ((yy % h) + h) % h; }
        else if (edge == Clamp) { xx = qBound(0, xx, w - 1); yy = qBound(0, yy, h - 1); }
        else if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
        const QRgb c = reinterpret_cast<const QRgb *>(img.constScanLine(yy))[xx];
        out[0] += qRed(c) * wgt; out[1] += qGreen(c) * wgt; out[2] += qBlue(c) * wgt; out[3] += qAlpha(c) * wgt;
    }
}
inline QRgb packPremul(const double v[4]) {
    const int a = c255(v[3]);
    return qRgba(qMin(a, c255(v[0])), qMin(a, c255(v[1])), qMin(a, c255(v[2])), a);
}
}

// ---------------------------------------------------------------- colour
QColor gradientAt(const QVector<GradStop> &st, double t) {
    if (st.isEmpty()) return QColor(int(t * 255), int(t * 255), int(t * 255));
    if (t <= st.first().pos) return st.first().color;
    if (t >= st.last().pos) return st.last().color;
    for (int i = 1; i < st.size(); ++i) {
        if (t <= st[i].pos) {
            const double span = st[i].pos - st[i - 1].pos;
            const double f = span > 1e-9 ? (t - st[i - 1].pos) / span : 1;
            const QColor &a = st[i - 1].color, &b = st[i].color;
            return QColor(c255(a.red() + (b.red() - a.red()) * f), c255(a.green() + (b.green() - a.green()) * f),
                          c255(a.blue() + (b.blue() - a.blue()) * f), c255(a.alpha() + (b.alpha() - a.alpha()) * f));
        }
    }
    return st.last().color;
}

QImage gradientMap(const QImage &src, QVector<GradStop> stops, bool reverse, int opacity) {
    std::stable_sort(stops.begin(), stops.end(), [](const GradStop &a, const GradStop &b) { return a.pos < b.pos; });
    QRgb lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = gradientAt(stops, (reverse ? 255 - i : i) / 255.0).rgba();
    const double op = qBound(0, opacity, 100) / 100.0;
    QImage s = straight(src);
    for (int y = 0; y < s.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
        for (int x = 0; x < s.width(); ++x) {
            if (!qAlpha(d[x])) continue;
            const QRgb m = lut[qGray(d[x])];
            const double ma = qAlpha(m) / 255.0 * op;
            d[x] = qRgba(c255(qRed(d[x]) + (qRed(m) - qRed(d[x])) * ma), c255(qGreen(d[x]) + (qGreen(m) - qGreen(d[x])) * ma),
                         c255(qBlue(d[x]) + (qBlue(m) - qBlue(d[x])) * ma), qAlpha(d[x]));
        }
    }
    return premul(s);
}

QImage autoContrast(const QImage &src) {
    QImage s = straight(src);
    QVector<int> hist(256, 0);
    int n = 0;
    for (int y = 0; y < s.height(); ++y) {
        const QRgb *d = reinterpret_cast<const QRgb *>(s.constScanLine(y));
        for (int x = 0; x < s.width(); ++x) if (qAlpha(d[x])) { ++hist[qGray(d[x])]; ++n; }
    }
    if (!n) return premul(s);
    int lo = 0, hi = 255, acc = 0;
    while (lo < 254 && (acc += hist[lo]) <= n / 200) ++lo;
    acc = 0;
    while (hi > lo + 1 && (acc += hist[hi]) <= n / 200) --hi;
    const double k = 255.0 / qMax(1, hi - lo);
    for (int y = 0; y < s.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
        for (int x = 0; x < s.width(); ++x) {
            if (!qAlpha(d[x])) continue;
            d[x] = qRgba(c255((qRed(d[x]) - lo) * k), c255((qGreen(d[x]) - lo) * k), c255((qBlue(d[x]) - lo) * k), qAlpha(d[x]));
        }
    }
    return premul(s);
}

// ---------------------------------------------------------------- dithering
QStringList ditherMethodNames() {
    return {QStringLiteral("None (nearest colour)"), QStringLiteral("Floyd\u2013Steinberg"), QStringLiteral("Atkinson"),
            QStringLiteral("Jarvis, Judice & Ninke"), QStringLiteral("Stucki"), QStringLiteral("Bayer 2\u00d72 (ordered)"),
            QStringLiteral("Bayer 4\u00d74 (ordered)"), QStringLiteral("Bayer 8\u00d78 (ordered)")};
}
QStringList paletteTargetNames() {
    return {QStringLiteral("Custom palette"), QStringLiteral("Web 216"), QStringLiteral("1-bit black & white"),
            QStringLiteral("RGB565 (16-bit)"), QStringLiteral("RGB332 (8-bit)"), QStringLiteral("N colours (adaptive)"),
            QStringLiteral("N greys"), QStringLiteral("RGB555 (15-bit)"), QStringLiteral("RGB444 (12-bit)")};
}

QVector<QRgb> targetPalette(int target, int n, const QImage &img, const QVector<QRgb> &custom) {
    QVector<QRgb> pal;
    switch (target) {
    case PalCustom: pal = custom; if (pal.isEmpty()) pal = {qRgb(0, 0, 0), qRgb(255, 255, 255)}; break;
    case PalWeb: for (int r = 0; r < 6; ++r) for (int g = 0; g < 6; ++g) for (int b = 0; b < 6; ++b) pal << qRgb(r * 51, g * 51, b * 51); break;
    case PalMono: pal = {qRgb(0, 0, 0), qRgb(255, 255, 255)}; break;
    case PalAdaptive: pal = quant::buildPalette({straight(img)}, qBound(2, n, 256), quant::Adaptive); break;
    case PalGray: { const int k = qBound(2, n, 256); for (int i = 0; i < k; ++i) { int v = c255(i * 255.0 / (k - 1)); pal << qRgb(v, v, v); } break; }
    default: break;  // per-channel targets
    }
    return pal;
}

namespace {
struct Quantizer {
    // per-channel levels (when pal is empty)
    int levels[3] = {256, 256, 256};
    bool grey = false;
    QVector<QRgb> pal;
    mutable QHash<quint32, QRgb> cache;
    static int q(int v, int lv) { if (lv >= 256) return v; const double s = 255.0 / (lv - 1); return c255(std::lround(v / s) * s); }
    QRgb nearest(int r, int g, int b) const {
        r = ci(r); g = ci(g); b = ci(b);
        if (pal.isEmpty()) {
            if (grey) { int v = q(qGray(r, g, b), levels[0]); return qRgb(v, v, v); }
            return qRgb(q(r, levels[0]), q(g, levels[1]), q(b, levels[2]));
        }
        const quint32 key = quint32(r << 16 | g << 8 | b);
        auto it = cache.constFind(key);
        if (it != cache.constEnd()) return *it;
        QRgb best = pal.first();
        int bd = INT_MAX;
        for (QRgb c : pal) {
            const int dr = qRed(c) - r, dg = qGreen(c) - g, db = qBlue(c) - b;
            const int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
            if (d < bd) { bd = d; best = c; }
        }
        cache.insert(key, best);
        return best;
    }
    // typical distance between neighbouring output levels (ordered-dither spread)
    double spread() const {
        if (pal.isEmpty()) return 255.0 / (qMin(levels[0], qMin(levels[1], levels[2])) - 1);
        return pal.size() <= 2 ? 255.0 : qBound(16.0, 255.0 / std::cbrt(double(pal.size())), 128.0);
    }
};

struct Kernel { int dx, dy; double w; };
const QVector<Kernel> &kernel(int method, double *div) {
    static const QVector<Kernel> fs = {{1, 0, 7}, {-1, 1, 3}, {0, 1, 5}, {1, 1, 1}};
    static const QVector<Kernel> atk = {{1, 0, 1}, {2, 0, 1}, {-1, 1, 1}, {0, 1, 1}, {1, 1, 1}, {0, 2, 1}};
    static const QVector<Kernel> jjn = {{1, 0, 7}, {2, 0, 5}, {-2, 1, 3}, {-1, 1, 5}, {0, 1, 7}, {1, 1, 5}, {2, 1, 3},
                                        {-2, 2, 1}, {-1, 2, 3}, {0, 2, 5}, {1, 2, 3}, {2, 2, 1}};
    static const QVector<Kernel> stu = {{1, 0, 8}, {2, 0, 4}, {-2, 1, 2}, {-1, 1, 4}, {0, 1, 8}, {1, 1, 4}, {2, 1, 2},
                                        {-2, 2, 1}, {-1, 2, 2}, {0, 2, 4}, {1, 2, 2}, {2, 2, 1}};
    switch (method) {
    case Atkinson: *div = 8; return atk;     // diffuses only 6/8 of the error
    case Jarvis: *div = 48; return jjn;
    case Stucki: *div = 42; return stu;
    default: *div = 16; return fs;
    }
}
double bayer(int n, int x, int y) {
    // recursive Bayer index, normalised to (-0.5, 0.5)
    int v = 0;
    for (int bit = n >> 1; bit > 0; bit >>= 1) {
        const int xb = (x & bit) ? 1 : 0, yb = (y & bit) ? 1 : 0;
        v = (v << 2) | ((xb ^ yb) << 1 | yb);  // 0 2 / 3 1 pattern
    }
    return (v + 0.5) / double(n * n) - 0.5;
}

QImage runDither(const QImage &src, int method, const Quantizer &qz, double strength) {
    QImage s = straight(src);
    const int w = s.width(), h = s.height();
    const double k = qBound(0.0, strength, 100.0) / 100.0;
    if (method >= Bayer2 && method <= Bayer8) {
        const int n = method == Bayer2 ? 2 : method == Bayer4 ? 4 : 8;
        const double sp = qz.spread() * k;
        for (int y = 0; y < h; ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
            for (int x = 0; x < w; ++x) {
                if (!qAlpha(d[x])) continue;
                const int t = int(std::lround(bayer(n, x, y) * sp));
                const QRgb c = qz.nearest(qRed(d[x]) + t, qGreen(d[x]) + t, qBlue(d[x]) + t);
                d[x] = (c & 0xffffff) | (d[x] & 0xff000000);
            }
        }
        return premul(s);
    }
    if (method <= DitherNone || method >= kDitherMethods) {
        for (int y = 0; y < h; ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
            for (int x = 0; x < w; ++x) if (qAlpha(d[x])) d[x] = (qz.nearest(qRed(d[x]), qGreen(d[x]), qBlue(d[x])) & 0xffffff) | (d[x] & 0xff000000);
        }
        return premul(s);
    }
    double div;
    const QVector<Kernel> &ker = kernel(method, &div);
    // error buffer for 3 rows
    QVector<float> err(size_t(w) * 3 * 3, 0.f);
    auto row = [&](int r) { return err.data() + size_t(r % 3) * w * 3; };
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
        const bool ltr = !(y & 1);   // serpentine
        for (int i = 0; i < w; ++i) {
            const int x = ltr ? i : w - 1 - i;
            if (!qAlpha(d[x])) continue;
            float *e = row(y) + x * 3;
            const double r = qRed(d[x]) + e[0], g = qGreen(d[x]) + e[1], b = qBlue(d[x]) + e[2];
            const QRgb c = qz.nearest(int(std::lround(r)), int(std::lround(g)), int(std::lround(b)));
            const double er[3] = {(r - qRed(c)) * k, (g - qGreen(c)) * k, (b - qBlue(c)) * k};
            d[x] = (c & 0xffffff) | (d[x] & 0xff000000);
            for (const Kernel &kk : ker) {
                const int nx = x + (ltr ? kk.dx : -kk.dx), ny = y + kk.dy;
                if (nx < 0 || nx >= w || ny >= h) continue;
                float *t = row(ny) + nx * 3;
                for (int ch = 0; ch < 3; ++ch) t[ch] += float(er[ch] * kk.w / div);
            }
        }
        std::fill(row(y), row(y) + size_t(w) * 3, 0.f);
    }
    return premul(s);
}
}

QImage dither(const QImage &src, int method, int target, int nColors, const QVector<QRgb> &custom, double strength, bool serpentine) {
    Quantizer qz;
    switch (target) {
    case PalRGB565: qz.levels[0] = 32; qz.levels[1] = 64; qz.levels[2] = 32; break;
    case PalRGB555: qz.levels[0] = qz.levels[1] = qz.levels[2] = 32; break;
    case PalRGB444: qz.levels[0] = qz.levels[1] = qz.levels[2] = 16; break;
    case PalRGB332: qz.levels[0] = 8; qz.levels[1] = 8; qz.levels[2] = 4; break;
    default: qz.pal = targetPalette(target, nColors, src, custom); break;
    }
    if (!serpentine && method >= FloydSteinberg && method <= Stucki) {
        // plain left-to-right scanning: run row by row with the same kernels
        Quantizer q2 = qz;
        QImage s = straight(src);
        const int w = s.width(), h = s.height();
        double div;
        const QVector<Kernel> &ker = kernel(method, &div);
        const double k = qBound(0.0, strength, 100.0) / 100.0;
        QVector<float> err(size_t(w) * h * 3, 0.f);
        for (int y = 0; y < h; ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
            for (int x = 0; x < w; ++x) {
                if (!qAlpha(d[x])) continue;
                float *e = err.data() + (size_t(y) * w + x) * 3;
                const double r = qRed(d[x]) + e[0], g = qGreen(d[x]) + e[1], b = qBlue(d[x]) + e[2];
                const QRgb c = q2.nearest(int(std::lround(r)), int(std::lround(g)), int(std::lround(b)));
                const double er[3] = {(r - qRed(c)) * k, (g - qGreen(c)) * k, (b - qBlue(c)) * k};
                d[x] = (c & 0xffffff) | (d[x] & 0xff000000);
                for (const Kernel &kk : ker) {
                    const int nx = x + kk.dx, ny = y + kk.dy;
                    if (nx < 0 || nx >= w || ny >= h) continue;
                    float *t = err.data() + (size_t(ny) * w + nx) * 3;
                    for (int ch = 0; ch < 3; ++ch) t[ch] += float(er[ch] * kk.w / div);
                }
            }
        }
        return premul(s);
    }
    return runDither(src, method, qz, strength);
}

QImage ditherToBits(const QImage &src, int method, int rBits, int gBits, int bBits, bool grey, int aBits) {
    Quantizer qz;
    qz.levels[0] = 1 << qBound(1, rBits, 8); qz.levels[1] = 1 << qBound(1, gBits, 8); qz.levels[2] = 1 << qBound(1, bBits, 8);
    qz.grey = grey;
    QImage out = runDither(src, method, qz, 100);
    if (aBits < 8) {
        // alpha: quantize (ordered for 1-bit-ish alpha keeps edges stable)
        QImage s = straight(out);
        const int lv = 1 << qMax(1, aBits);
        for (int y = 0; y < s.height(); ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
            for (int x = 0; x < s.width(); ++x) d[x] = (d[x] & 0xffffff) | (uint(Quantizer::q(qAlpha(d[x]), lv)) << 24);
        }
        out = premul(s);
    }
    return out;
}

QVector<QRgb> uniqueColors(const QImage &img, int max) {
    QImage s = straight(img);
    QVector<QRgb> out;
    QHash<QRgb, char> seen;
    for (int y = 0; y < s.height(); ++y) {
        const QRgb *d = reinterpret_cast<const QRgb *>(s.constScanLine(y));
        for (int x = 0; x < s.width(); ++x) {
            if (qAlpha(d[x]) < 128) continue;
            const QRgb c = d[x] | 0xff000000;
            if (seen.contains(c)) continue;
            seen.insert(c, 1);
            out << c;
            if (max > 0 && out.size() >= max) return out;
        }
    }
    return out;
}

QImage replaceColor(const QImage &src, QRgb from, QRgb to) {
    ReplaceColorOpts o;
    o.from = from; o.to = to; o.tolerance = 0; o.softness = 0; o.range = RcSolid; o.strength = 1; o.hard = true;
    return replaceColorRange(src, o);
}

double colorDistance(QRgb a, QRgb b) {
    const int dr = qRed(a) - qRed(b), dg = qGreen(a) - qGreen(b), db = qBlue(a) - qBlue(b);
    return std::sqrt(double(dr * dr + dg * dg + db * db)) * (100.0 / (255.0 * 1.7320508075688772));
}

double colorMatch(QRgb c, const ReplaceColorOpts &o) {
    if (!qAlpha(c)) return 0;
    if ((c & 0xffffff) == (o.from & 0xffffff)) return 1;
    const double tol = qBound(0.0, o.tolerance, 100.0);
    if (tol <= 0) return 0;
    const double d = colorDistance(c, o.from);
    if (d > tol) return 0;
    const double soft = o.hard ? 0.0 : qBound(0.0, o.softness, 100.0) / 100.0;
    const double inner = tol * (1 - soft);
    if (d <= inner) return 1;
    return qBound(0.0, (tol - d) / qMax(1e-9, tol - inner), 1.0);
}

namespace {
// W3C compositing "Color" blend: hue and saturation of `c`, luminosity of `l` (0..1 doubles)
inline double lum3(double r, double g, double b) { return 0.3 * r + 0.59 * g + 0.11 * b; }
void setLum(double &r, double &g, double &b, double l) {
    const double d = l - lum3(r, g, b);
    r += d; g += d; b += d;
    const double L = lum3(r, g, b), n = qMin(r, qMin(g, b)), x = qMax(r, qMax(g, b));
    if (n < 0) { const double k = L / qMax(1e-12, L - n); r = L + (r - L) * k; g = L + (g - L) * k; b = L + (b - L) * k; }
    if (x > 1) { const double k = (1 - L) / qMax(1e-12, x - L); r = L + (r - L) * k; g = L + (g - L) * k; b = L + (b - L) * k; }
}
inline int to8(double v) { return qBound(0, int(std::lround(v * 255.0)), 255); }
}

QRgb replacedColor(QRgb c, const ReplaceColorOpts &o) {
    const int a = qAlpha(c);
    switch (o.range) {
    case RcColor: {
        double r = qRed(o.to) / 255.0, g = qGreen(o.to) / 255.0, b = qBlue(o.to) / 255.0;
        setLum(r, g, b, lum3(qRed(c) / 255.0, qGreen(c) / 255.0, qBlue(c) / 255.0));
        return qRgba(to8(r), to8(g), to8(b), a);
    }
    case RcShift: {
        float h, s, l, al;
        QColor::fromRgb(c).getHslF(&h, &s, &l, &al);
        if (h < 0) h = 0;
        double nh = std::fmod(h * 360.0 + o.hue, 360.0);
        if (nh < 0) nh += 360;
        double ns = s, nl = l;
        const double ds = qBound(-100.0, o.saturation, 100.0) / 100.0, dl = qBound(-100.0, o.lightness, 100.0) / 100.0;
        ns = ds >= 0 ? ns + (1 - ns) * ds : ns * (1 + ds);
        nl = dl >= 0 ? nl + (1 - nl) * dl : nl * (1 + dl);
        const QColor q = QColor::fromHslF(float(nh / 360.0), float(qBound(0.0, ns, 1.0)), float(qBound(0.0, nl, 1.0)));
        return qRgba(q.red(), q.green(), q.blue(), a);
    }
    default: return (o.to & 0xffffff) | (QRgb(a) << 24);
    }
}

static inline QRgb blendRgb(QRgb c, QRgb r, double w) {
    if (w >= 1) return r;
    auto m = [w](int x, int y) { return qBound(0, int(std::lround(x + (y - x) * w)), 255); };
    return qRgba(m(qRed(c), qRed(r)), m(qGreen(c), qGreen(r)), m(qBlue(c), qBlue(r)), qAlpha(c));
}

// weight of the replacement for a straight pixel at a given coverage (0..1)
static inline double rcWeight(QRgb c, const ReplaceColorOpts &o, double cov) {
    const double w = colorMatch(c, o) * qBound(0.0, o.strength, 1.0) * cov;
    if (o.hard) return w >= 0.5 ? 1.0 : 0.0;
    return w;
}

QImage replaceColorRange(const QImage &src, const ReplaceColorOpts &o, const QImage &coverage) {
    QImage s = straight(src);
    const QImage cov = coverage.isNull() || coverage.size() != s.size() ? QImage() : coverage.convertToFormat(QImage::Format_Alpha8);
    QHash<QRgb, QRgb> cache;   // replacement per colour (Shift goes through QColor)
    for (int y = 0; y < s.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
        const uchar *cv = cov.isNull() ? nullptr : cov.constScanLine(y);
        for (int x = 0; x < s.width(); ++x) {
            const double w = rcWeight(d[x], o, cv ? cv[x] / 255.0 : 1.0);
            if (w <= 0) continue;
            auto it = cache.constFind(d[x]);
            QRgb r;
            if (it != cache.constEnd()) r = *it;
            else { r = replacedColor(d[x], o); if (cache.size() < 65536) cache.insert(d[x], r); }
            d[x] = blendRgb(d[x], r, w);
        }
    }
    return premul(s);
}

QImage replaceColorMask(const QImage &src, const ReplaceColorOpts &o) {
    const QImage s = straight(src);
    QImage m(s.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < s.height(); ++y) {
        const QRgb *d = reinterpret_cast<const QRgb *>(s.constScanLine(y));
        uchar *mm = m.scanLine(y);
        for (int x = 0; x < s.width(); ++x) mm[x] = uchar(qBound(0, int(std::lround(rcWeight(d[x], o, 1.0) * 255)), 255));
    }
    return m;
}

void ReplaceColorStroke::begin(const QImage &img, const ReplaceColorOpts &o, const QImage &clip) {
    m_o = o;
    m_orig = straight(img);
    m_cov = QImage(m_orig.size(), QImage::Format_Alpha8);
    m_cov.fill(0);
    m_clip = clip.isNull() || clip.size() != m_orig.size() ? QImage() : clip.convertToFormat(QImage::Format_Alpha8);
}

QRect ReplaceColorStroke::dab(QImage &img, QPointF c, double size, int edge, bool square, bool pixelPerfect) {
    if (m_orig.isNull() || img.size() != m_orig.size() || !std::isfinite(c.x()) || !std::isfinite(c.y()) || !std::isfinite(size)) return {};
    const double r = qBound(0.5, size / 2.0, 1e5);
    const QRect box = QRect(QPoint(int(std::floor(c.x() - r)), int(std::floor(c.y() - r))),
                            QPoint(int(std::ceil(c.x() + r)), int(std::ceil(c.y() + r)))) & m_orig.rect();
    if (box.isEmpty()) return {};
    if (img.format() != QImage::Format_ARGB32_Premultiplied) img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const double soft = pixelPerfect ? 0.0 : qBound(0, edge, 100) / 100.0;
    ReplaceColorOpts o = m_o;
    if (pixelPerfect) o.hard = true;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        uchar *cv = m_cov.scanLine(y);
        const QRgb *src = reinterpret_cast<const QRgb *>(m_orig.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
        const uchar *cl = m_clip.isNull() ? nullptr : m_clip.constScanLine(y);
        for (int x = box.left(); x <= box.right(); ++x) {
            const double dx = x + 0.5 - c.x(), dy = y + 0.5 - c.y();
            const double dist = square ? qMax(std::abs(dx), std::abs(dy)) : std::sqrt(dx * dx + dy * dy);
            double t;
            if (dist > r) t = 0;
            else if (soft <= 0) t = 1;
            else { const double in = r * (1 - soft); t = dist <= in ? 1.0 : qBound(0.0, (r - dist) / qMax(1e-9, r - in), 1.0); t = t * t * (3 - 2 * t); }
            const int tv = int(std::lround(t * 255));
            if (tv <= cv[x]) continue;
            cv[x] = uchar(tv);
            const double w = rcWeight(src[x], o, (tv / 255.0) * (cl ? cl[x] / 255.0 : 1.0));
            const QRgb out = w > 0 ? blendRgb(src[x], replacedColor(src[x], o), w) : src[x];
            dst[x] = qPremultiply(out);
        }
    }
    return box;
}

// ---------------------------------------------------------------- red eye
double redEyeMatch(QRgb p, const RedEyeOpts &o) {
    if (!qAlpha(p)) return 0;
    const int r = qRed(p), g = qGreen(p), b = qBlue(p);
    const int mx = qMax(g, b);
    if (r <= mx || r < 24) return 0;
    const double redness = double(r - mx) / r;                         // 0..1
    const double tol = qBound(0.0, std::isfinite(o.tolerance) ? o.tolerance : 50.0, 100.0) / 100.0;
    const double th = 0.62 - 0.5 * tol;                                // tolerance 0: 0.62, 50: 0.37, 100: 0.12
    const double band = 0.12;
    double m = qBound(0.0, (redness - th) / band + 0.5, 1.0);
    m = m * m * (3 - 2 * m);
    // the catchlight: bright and close to neutral (pinkish white) is never touched
    const int mn = qMin(r, qMin(g, b));
    if (mn > 150) m *= qBound(0.0, (210.0 - mn) / 60.0, 1.0);
    return m;
}

QRgb redEyeFixed(QRgb p, const RedEyeOpts &o) {
    const int g = qGreen(p), b = qBlue(p);
    const double k = 1.0 - 0.8 * qBound(0.0, std::isfinite(o.darken) ? o.darken : 50.0, 100.0) / 100.0;
    // desaturate to the pupil's own level (red pulled down to green / blue), keep a trace of the blue-green
    const double base = (g + b) / 2.0;
    const int nr = qBound(0, int(std::lround(base * k)), 255);
    const int ng = qBound(0, int(std::lround((base * 0.6 + g * 0.4) * k)), 255);
    const int nb = qBound(0, int(std::lround((base * 0.6 + b * 0.4) * k)), 255);
    return qRgba(nr, ng, nb, qAlpha(p));
}

void RedEyeStroke::begin(const QImage &img, const RedEyeOpts &o, const QImage &clip) {
    m_o = o;
    m_orig = straight(img);
    m_cov = QImage(m_orig.size(), QImage::Format_Alpha8);
    m_cov.fill(0);
    m_clip = clip.isNull() || clip.size() != m_orig.size() ? QImage() : clip.convertToFormat(QImage::Format_Alpha8);
}

QRect RedEyeStroke::dab(QImage &img, QPointF c, double size, int edge, bool square, double amount) {
    if (m_orig.isNull() || img.size() != m_orig.size() || !std::isfinite(c.x()) || !std::isfinite(c.y()) || !std::isfinite(size)
        || !std::isfinite(amount))
        return {};
    amount = qBound(0.0, amount, 1.0);
    const double r = qBound(0.5, size / 2.0, 1e5);
    const QRect box = QRect(QPoint(int(std::floor(c.x() - r)), int(std::floor(c.y() - r))),
                            QPoint(int(std::ceil(c.x() + r)), int(std::ceil(c.y() + r)))) & m_orig.rect();
    if (box.isEmpty()) return {};
    if (img.format() != QImage::Format_ARGB32_Premultiplied) img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const double soft = qBound(0, edge, 100) / 100.0;
    const double strength = qBound(0.0, std::isfinite(m_o.strength) ? m_o.strength : 1.0, 1.0);
    for (int y = box.top(); y <= box.bottom(); ++y) {
        uchar *cv = m_cov.scanLine(y);
        const QRgb *src = reinterpret_cast<const QRgb *>(m_orig.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(img.scanLine(y));
        const uchar *cl = m_clip.isNull() ? nullptr : m_clip.constScanLine(y);
        for (int x = box.left(); x <= box.right(); ++x) {
            const double dx = x + 0.5 - c.x(), dy = y + 0.5 - c.y();
            const double dist = square ? qMax(std::abs(dx), std::abs(dy)) : std::sqrt(dx * dx + dy * dy);
            double t;
            if (dist > r) t = 0;
            else if (soft <= 0) t = 1;
            else { const double in = r * (1 - soft); t = dist <= in ? 1.0 : qBound(0.0, (r - dist) / qMax(1e-9, r - in), 1.0); t = t * t * (3 - 2 * t); }
            const int tv = int(std::lround(t * amount * 255));
            if (tv <= cv[x]) continue;
            cv[x] = uchar(tv);
            const double m = redEyeMatch(src[x], m_o);
            const double w = m * strength * (tv / 255.0) * (cl ? cl[x] / 255.0 : 1.0);
            dst[x] = qPremultiply(w > 0 ? blendRgb(src[x], redEyeFixed(src[x], m_o), w) : src[x]);
        }
    }
    return box;
}

QImage paletteSwap(const QImage &src, const QVector<QRgb> &from, const QVector<QRgb> &to, bool nearest) {
    QImage s = straight(src);
    const int n = qMin(from.size(), to.size());
    if (!n) return premul(s);
    QHash<QRgb, int> idx;
    for (int i = 0; i < n; ++i) if (!idx.contains(from[i] & 0xffffff)) idx.insert(from[i] & 0xffffff, i);
    for (int y = 0; y < s.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
        for (int x = 0; x < s.width(); ++x) {
            if (!qAlpha(d[x])) continue;
            int i = idx.value(d[x] & 0xffffff, -1);
            if (i < 0 && nearest) {
                int bd = INT_MAX;
                for (int j = 0; j < n; ++j) {
                    const int dr = qRed(from[j]) - qRed(d[x]), dg = qGreen(from[j]) - qGreen(d[x]), db = qBlue(from[j]) - qBlue(d[x]);
                    const int dd = dr * dr + dg * dg + db * db;
                    if (dd < bd) { bd = dd; i = j; }
                }
            }
            if (i >= 0) d[x] = (to[i] & 0xffffff) | (d[x] & 0xff000000);
        }
    }
    return premul(s);
}

// ---------------------------------------------------------------- pixel art
QImage outline(const QImage &src, bool inner, bool eight, int width, const QColor &color, int thr, int opacity) {
    QImage s = straight(src);
    const int w = s.width(), h = s.height();
    width = qBound(1, width, 64);
    thr = qBound(1, thr, 255);
    QVector<int> dist(size_t(w) * h, INT_MAX);  // distance (in steps) to the other side
    QVector<int> cur, next;
    auto in = [&](int x, int y) { return qAlpha(reinterpret_cast<const QRgb *>(s.constScanLine(y))[x]) >= thr; };
    // seeds: pixels on the "from" side; BFS into the other side for `width` steps
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (in(x, y) != inner) { dist[size_t(y) * w + x] = 0; cur << y * w + x; }
    static const int n4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    static const int n8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
    const int nn = eight ? 8 : 4;
    const int (*nb)[2] = eight ? n8 : n4;
    if (inner) {
        // the image border counts as outside
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            if (!in(x, y)) continue;
            bool edge = false;
            for (int k = 0; k < nn && !edge; ++k) { const int nx = x + nb[k][0], ny = y + nb[k][1]; if (nx < 0 || ny < 0 || nx >= w || ny >= h) edge = true; }
            if (edge) { dist[size_t(y) * w + x] = 1; }
        }
    }
    for (int step = 1; step <= width && !cur.isEmpty(); ++step) {
        next.clear();
        for (int id : cur) {
            const int x = id % w, y = id / w;
            for (int k = 0; k < nn; ++k) {
                const int nx = x + nb[k][0], ny = y + nb[k][1];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                int &dd = dist[size_t(ny) * w + nx];
                if (dd <= step) continue;
                dd = step;
                next << ny * w + nx;
            }
        }
        std::swap(cur, next);
    }
    const double op = qBound(0, opacity, 100) / 100.0 * color.alphaF();
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int dd = dist[size_t(y) * w + x];
            if (dd < 1 || dd > width) continue;
            if (in(x, y) != inner) continue;
            if (inner) {
                d[x] = qRgba(c255(qRed(d[x]) + (color.red() - qRed(d[x])) * op), c255(qGreen(d[x]) + (color.green() - qGreen(d[x])) * op),
                             c255(qBlue(d[x]) + (color.blue() - qBlue(d[x])) * op), qAlpha(d[x]));
            } else {
                // under whatever partial alpha is there
                const double a0 = qAlpha(d[x]) / 255.0, a1 = op;
                const double a = a0 + a1 * (1 - a0);
                if (a <= 0) continue;
                auto mix = [&](int c0, int c1) { return c255((c0 * a0 + c1 * a1 * (1 - a0)) / a); };
                d[x] = qRgba(mix(qRed(d[x]), color.red()), mix(qGreen(d[x]), color.green()), mix(qBlue(d[x]), color.blue()), c255(a * 255));
            }
        }
    }
    return premul(s);
}

QVector<QPoint> linePixels(QPoint a, QPoint b) {
    QVector<QPoint> out;
    int x0 = a.x(), y0 = a.y();
    const int dx = std::abs(b.x() - x0), sx = x0 < b.x() ? 1 : -1, dy = -std::abs(b.y() - y0), sy = y0 < b.y() ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        out << QPoint(x0, y0);
        if (x0 == b.x() && y0 == b.y()) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return out;
}

QVector<QPoint> pixelPerfect(const QVector<QPoint> &in) {
    QVector<QPoint> pts;
    for (const QPoint &p : in) if (pts.isEmpty() || pts.last() != p) pts << p;
    if (pts.size() < 3) return pts;
    QVector<QPoint> out;
    out << pts[0];
    int i = 1;
    while (i < pts.size()) {
        if (i + 1 < pts.size()) {
            const QPoint a = out.last(), b = pts[i], c = pts[i + 1];
            // b is an L corner when a and c touch diagonally and b is orthogonally adjacent to both
            if ((a.x() == b.x() || a.y() == b.y()) && (c.x() == b.x() || c.y() == b.y()) && a.x() != c.x() && a.y() != c.y()
                && std::abs(a.x() - c.x()) == 1 && std::abs(a.y() - c.y()) == 1) {
                ++i;   // drop b
                continue;
            }
        }
        out << pts[i];
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------- game textures
namespace {
QVector<float> heightField(const QImage &s, int source) {
    QVector<float> hgt(size_t(s.width()) * s.height());
    for (int y = 0; y < s.height(); ++y) {
        const QRgb *d = reinterpret_cast<const QRgb *>(s.constScanLine(y));
        for (int x = 0; x < s.width(); ++x)
            hgt[size_t(y) * s.width() + x] = float((source == 1 ? qAlpha(d[x]) : lum(d[x]) * qAlpha(d[x]) / 255.0) / 255.0);
    }
    return hgt;
}
}

QImage normalMap(const QImage &src, int op, double strength, bool invertY, bool wrap, int source) {
    QImage s = straight(src);
    const int w = s.width(), h = s.height();
    if (!w || !h) return premul(s);
    const QVector<float> hg = heightField(s, source);
    auto H = [&](int x, int y) {
        if (wrap) { x = ((x % w) + w) % w; y = ((y % h) + h) % h; }
        else { x = qBound(0, x, w - 1); y = qBound(0, y, h - 1); }
        return double(hg[size_t(y) * w + x]);
    };
    const double a = op == 1 ? 3 : 1, b = op == 1 ? 10 : 2, norm = 2 * (2 * a + b);
    QImage out(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const QRgb *sd = reinterpret_cast<const QRgb *>(s.constScanLine(y));
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double gx = (a * (H(x + 1, y - 1) - H(x - 1, y - 1)) + b * (H(x + 1, y) - H(x - 1, y)) + a * (H(x + 1, y + 1) - H(x - 1, y + 1))) / norm * 2;
            const double gy = (a * (H(x - 1, y + 1) - H(x - 1, y - 1)) + b * (H(x, y + 1) - H(x, y - 1)) + a * (H(x + 1, y + 1) - H(x + 1, y - 1))) / norm * 2;
            double nx = -gx * strength, ny = (invertY ? -gy : gy) * strength, nz = 1;
            const double l = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= l; ny /= l; nz /= l;
            d[x] = qRgba(c255((nx * 0.5 + 0.5) * 255), c255((ny * 0.5 + 0.5) * 255), c255((nz * 0.5 + 0.5) * 255), source == 1 ? 255 : qAlpha(sd[x]));
        }
    }
    return premul(out);
}

QImage ambientOcclusion(const QImage &src, double radius, double strength, bool wrap, int source) {
    QImage s = straight(src);
    const int w = s.width(), h = s.height();
    if (!w || !h) return premul(s);
    const QVector<float> hg = heightField(s, source);
    auto H = [&](int x, int y) {
        if (wrap) { x = ((x % w) + w) % w; y = ((y % h) + h) % h; }
        else { x = qBound(0, x, w - 1); y = qBound(0, y, h - 1); }
        return double(hg[size_t(y) * w + x]);
    };
    radius = qBound(1.0, radius, 128.0);
    const int dirs = 16, steps = qBound(2, int(radius / 2), 8);
    double dx[dirs], dy[dirs];
    for (int i = 0; i < dirs; ++i) { dx[i] = std::cos(2 * M_PI * i / dirs); dy[i] = std::sin(2 * M_PI * i / dirs); }
    const double k = qBound(0.0, strength, 400.0) / 100.0;
    QImage out(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const QRgb *sd = reinterpret_cast<const QRgb *>(s.constScanLine(y));
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double h0 = H(x, y) * radius;
            double occ = 0;
            for (int i = 0; i < dirs; ++i) {
                double best = 0;
                for (int st = 1; st <= steps; ++st) {
                    const double dist = radius * st / steps;
                    const double hh = H(int(std::lround(x + dx[i] * dist)), int(std::lround(y + dy[i] * dist))) * radius;
                    best = qMax(best, (hh - h0) / dist);     // tangent of the horizon angle
                }
                occ += std::sin(std::atan(best));
            }
            const double ao = qBound(0.0, 1.0 - k * occ / dirs, 1.0);
            const int v = c255(ao * 255);
            d[x] = qRgba(v, v, v, source == 1 ? 255 : qAlpha(sd[x]));
        }
    }
    return premul(out);
}

QImage makeSeamless(const QImage &src, int mode, double width) {
    if (mode == 0) return premul(fx::makeSeamless(premul(src), width));
    QImage s = premul(src);
    const int w = s.width(), h = s.height();
    QImage o = s.copy();
    // mirror blend: pixel at distance d from the seam mixes with its mirror image across the seam
    auto blendAxis = [&](bool horiz) {
        const int n = horiz ? w : h, m = horiz ? h : w;
        const int bw = qBound(1, int(std::lround(width)), n / 2);
        QImage in = o.copy();
        for (int d = 0; d < bw; ++d) {
            const double t = 1.0 - double(d) / bw;
            const double f = 0.5 * t * t * (3 - 2 * t);   // 0.5 at the seam (both sides equal), 0 at `width`
            for (int k = 0; k < m; ++k) {
                const int a = d, b = n - 1 - d;
                auto px = [&](QImage &img, int i) -> QRgb & { return horiz ? reinterpret_cast<QRgb *>(img.scanLine(k))[i] : reinterpret_cast<QRgb *>(img.scanLine(i))[k]; };
                const QRgb pa = px(in, a), pb = px(in, b);
                auto mix = [f](QRgb p, QRgb q) {
                    return qRgba(c255(qRed(p) + (qRed(q) - qRed(p)) * f), c255(qGreen(p) + (qGreen(q) - qGreen(p)) * f),
                                 c255(qBlue(p) + (qBlue(q) - qBlue(p)) * f), c255(qAlpha(p) + (qAlpha(q) - qAlpha(p)) * f));
                };
                px(o, a) = mix(pa, pb);
                px(o, b) = mix(pb, pa);
            }
        }
    };
    if (w >= 2) blendAxis(true);
    if (h >= 2) blendAxis(false);
    return o;
}

// ---------------------------------------------------------------- photo
QImage lensCorrection(const QImage &srcIn, const double p[8]) {
    const QImage src = premul(srcIn);
    const int w = src.width(), h = src.height();
    bool identity = true;
    for (int i = 0; i < 8; ++i) if (i != 4 && std::abs(p[i]) > 1e-9) identity = false;
    if (identity || !w || !h) return src;
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0, rn = std::sqrt(cx * cx + cy * cy) + 1e-9;
    const double k = p[0] / 100.0 * 0.5;
    const double caR = 1 + p[1] / 100.0 * 0.01, caB = 1 + p[2] / 100.0 * 0.01;
    const double vig = p[3] / 100.0, mid = qBound(0.0, p[4], 100.0) / 100.0;
    const double pv = p[5] / 100.0 * 0.5, ph = p[6] / 100.0 * 0.5;
    const double ang = qDegreesToRadians(p[7]), ca = std::cos(ang), sa = std::sin(ang);
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            double u = (x - cx) / rn, v = (y - cy) / rn;
            // straighten (rotate the sampling grid)
            const double ru = ca * u - sa * v, rv = sa * u + ca * v;
            u = ru; v = rv;
            // perspective: keystone scales x with y (vertical) and y with x (horizontal)
            const double vy = (y - cy) / qMax(1.0, cy), ux = (x - cx) / qMax(1.0, cx);
            u *= 1 + pv * vy;
            v *= 1 + ph * ux;
            // radial distortion (+ = remove barrel)
            const double r2 = u * u + v * v;
            const double s = 1 - k * r2;
            double c[4], cR[4], cB[4];
            auto at = [&](double scale, double out4[4]) { sample(src, cx + u * s * scale * rn, cy + v * s * scale * rn, out4); };
            at(1, c);
            if (std::abs(p[1]) > 1e-9) { at(caR, cR); c[0] = cR[0]; }
            if (std::abs(p[2]) > 1e-9) { at(caB, cB); c[2] = cB[2]; }
            if (std::abs(vig) > 1e-9) {
                const double r = std::sqrt(((x - cx) / rn) * ((x - cx) / rn) + ((y - cy) / rn) * ((y - cy) / rn));
                const double f = smooth(mid * 0.7, 1.0 + (1 - mid) * 0.3, r);
                const double m = qMax(0.0, 1 + vig * f);
                for (int ch = 0; ch < 3; ++ch) c[ch] = qMin(c[3], c[ch] * m);
            }
            for (int ch = 0; ch < 3; ++ch) c[ch] = qMin(c[ch], c[3]);
            d[x] = packPremul(c);
        }
    }
    return out;
}

namespace {
QImage diskBlur(const QImage &s, double radius) {
    const int w = s.width(), h = s.height(), r = qMax(1, int(std::ceil(radius)));
    QVector<QPoint> offs;
    for (int dy = -r; dy <= r; ++dy) for (int dx = -r; dx <= r; ++dx) if (dx * dx + dy * dy <= radius * radius + 0.5) offs << QPoint(dx, dy);
    QImage out(w, h, s.format());
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (const QPoint &o : offs) {
                const QRgb c = reinterpret_cast<const QRgb *>(s.constScanLine(qBound(0, y + o.y(), h - 1)))[qBound(0, x + o.x(), w - 1)];
                acc[0] += qRed(c); acc[1] += qGreen(c); acc[2] += qBlue(c); acc[3] += qAlpha(c);
            }
            for (double &a : acc) a /= offs.size();
            d[x] = packPremul(acc);
        }
    }
    return out;
}
QImage motionBlur(const QImage &s, double radius, double angleDeg) {
    const int w = s.width(), h = s.height();
    const int n = qMax(1, int(std::ceil(radius)));
    const double dx = std::cos(qDegreesToRadians(angleDeg)), dy = -std::sin(qDegreesToRadians(angleDeg));
    QImage out(w, h, s.format());
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            double acc[4] = {0, 0, 0, 0}, c[4];
            for (int i = -n; i <= n; ++i) {
                sample(s, x + dx * i, y + dy * i, c, Clamp);
                for (int ch = 0; ch < 4; ++ch) acc[ch] += c[ch];
            }
            for (double &a : acc) a /= 2 * n + 1;
            d[x] = packPremul(acc);
        }
    }
    return out;
}
}

QImage smartSharpen(const QImage &srcIn, double amount, double radius, int kind, double angle, double noise) {
    QImage src = premul(srcIn);
    radius = qBound(0.1, radius, 64.0);
    QImage bl;
    if (kind == 1) bl = diskBlur(src, radius);
    else if (kind == 2) bl = motionBlur(src, radius, angle);
    else { bl = src.copy(); fx::blur(bl, radius); }
    const double k = amount / 100.0;
    const double nthr = qBound(0.0, noise, 100.0) / 100.0 * 24.0;   // detail below this (levels) is damped
    QImage s = straight(src), b = straight(bl);
    for (int y = 0; y < s.height(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(s.scanLine(y));
        const QRgb *e = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < s.width(); ++x) {
            if (!qAlpha(d[x])) continue;
            const double det[3] = {double(qRed(d[x]) - qRed(e[x])), double(qGreen(d[x]) - qGreen(e[x])), double(qBlue(d[x]) - qBlue(e[x]))};
            const double mag = (std::abs(det[0]) + std::abs(det[1]) + std::abs(det[2])) / 3;
            const double damp = nthr > 0 ? smooth(nthr * 0.5, nthr, mag) : 1.0;
            d[x] = qRgba(c255(qRed(d[x]) + det[0] * k * damp), c255(qGreen(d[x]) + det[1] * k * damp), c255(qBlue(d[x]) + det[2] * k * damp), qAlpha(d[x]));
        }
    }
    return premul(s);
}

QImage surfaceBlur(const QImage &srcIn, int radius, int threshold) {
    QImage s = premul(srcIn);
    const int w = s.width(), h = s.height();
    radius = qBound(1, radius, 40);
    const double t = qMax(1, threshold) * 2.5;
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        const QRgb *row0 = reinterpret_cast<const QRgb *>(s.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb c0 = row0[x];
            const int v0[4] = {qRed(c0), qGreen(c0), qBlue(c0), qAlpha(c0)};
            double acc[4] = {0, 0, 0, 0}, wsum[4] = {0, 0, 0, 0};
            for (int yy = qMax(0, y - radius); yy <= qMin(h - 1, y + radius); ++yy) {
                const QRgb *r = reinterpret_cast<const QRgb *>(s.constScanLine(yy));
                for (int xx = qMax(0, x - radius); xx <= qMin(w - 1, x + radius); ++xx) {
                    const int v[4] = {qRed(r[xx]), qGreen(r[xx]), qBlue(r[xx]), qAlpha(r[xx])};
                    for (int ch = 0; ch < 4; ++ch) {
                        const double wt = 1.0 - std::abs(v[ch] - v0[ch]) / t;
                        if (wt <= 0) continue;
                        acc[ch] += v[ch] * wt; wsum[ch] += wt;
                    }
                }
            }
            double o[4];
            for (int ch = 0; ch < 4; ++ch) o[ch] = wsum[ch] > 0 ? acc[ch] / wsum[ch] : v0[ch];
            d[x] = packPremul(o);
        }
    }
    return out;
}

QImage bilateralDenoise(const QImage &srcIn, double sigmaS, double sigmaR, int passes) {
    QImage s = premul(srcIn);
    const int w = s.width(), h = s.height();
    sigmaS = qBound(0.5, sigmaS, 20.0);
    sigmaR = qBound(1.0, sigmaR, 255.0);
    const int r = qMax(1, int(std::ceil(sigmaS * 2)));
    QVector<double> sw(size_t(2 * r + 1) * (2 * r + 1));
    for (int dy = -r; dy <= r; ++dy) for (int dx = -r; dx <= r; ++dx) sw[size_t(dy + r) * (2 * r + 1) + dx + r] = std::exp(-(dx * dx + dy * dy) / (2 * sigmaS * sigmaS));
    const double inv2r = 1.0 / (2 * sigmaR * sigmaR);
    for (int pass = 0; pass < qBound(1, passes, 5); ++pass) {
        QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
            const QRgb *row0 = reinterpret_cast<const QRgb *>(s.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb c0 = row0[x];
                double acc[4] = {0, 0, 0, 0}, ws = 0;
                for (int yy = qMax(0, y - r); yy <= qMin(h - 1, y + r); ++yy) {
                    const QRgb *rr = reinterpret_cast<const QRgb *>(s.constScanLine(yy));
                    for (int xx = qMax(0, x - r); xx <= qMin(w - 1, x + r); ++xx) {
                        const QRgb c = rr[xx];
                        const int dr = qRed(c) - qRed(c0), dg = qGreen(c) - qGreen(c0), db = qBlue(c) - qBlue(c0), da = qAlpha(c) - qAlpha(c0);
                        const double wt = sw[size_t(yy - y + r) * (2 * r + 1) + xx - x + r] * std::exp(-(dr * dr + dg * dg + db * db + da * da) / 3.0 * inv2r);
                        acc[0] += qRed(c) * wt; acc[1] += qGreen(c) * wt; acc[2] += qBlue(c) * wt; acc[3] += qAlpha(c) * wt; ws += wt;
                    }
                }
                for (double &a : acc) a /= ws;
                d[x] = packPremul(acc);
            }
        }
        s = out;
    }
    return s;
}

// ---------------------------------------------------------------- content-aware fill (PatchMatch)
namespace {
constexpr int kR = 3;   // 7x7 patches
constexpr qint64 kMaxFillPixels = 4000000;   // region of interest above this is filled at a reduced scale
struct Level {
    int w = 0, h = 0;
    QVector<float> px;      // 4 floats per pixel (straight RGBA)
    QVector<char> hole;
    float *at(int x, int y) { return px.data() + (size_t(y) * w + x) * 4; }
    const float *at(int x, int y) const { return px.data() + (size_t(y) * w + x) * 4; }
    bool isHole(int x, int y) const { return hole[size_t(y) * w + x]; }
};

Level downsample(const Level &f) {
    Level c;
    c.w = qMax(1, (f.w + 1) / 2); c.h = qMax(1, (f.h + 1) / 2);
    c.px.fill(0, size_t(c.w) * c.h * 4);
    c.hole.fill(0, size_t(c.w) * c.h);
    for (int y = 0; y < c.h; ++y) for (int x = 0; x < c.w; ++x) {
        float acc[4] = {0, 0, 0, 0};
        int n = 0;
        bool anyHole = false;
        for (int j = 0; j < 2; ++j) for (int i = 0; i < 2; ++i) {
            const int fx = 2 * x + i, fy = 2 * y + j;
            if (fx >= f.w || fy >= f.h) continue;
            if (f.isHole(fx, fy)) { anyHole = true; continue; }
            const float *p = f.at(fx, fy);
            for (int k = 0; k < 4; ++k) acc[k] += p[k];
            ++n;
        }
        c.hole[size_t(y) * c.w + x] = anyHole;
        float *o = c.at(x, y);
        for (int k = 0; k < 4; ++k) o[k] = n ? acc[k] / n : 0;
    }
    return c;
}

// onion-peel diffusion fill of the holes (initial guess): each ring of unknown pixels next to known ones
// takes the mean of its known 8-neighbours, ring by ring. Frontier based, so it is linear in the pixel
// count even when the hole covers most of the level (the old full-image sweep per ring was quadratic).
void diffuseFill(Level &l) {
    const int w = l.w, h = l.h;
    if (w <= 0 || h <= 0) return;
    QVector<char> known(l.hole.size());
    for (int i = 0; i < l.hole.size(); ++i) known[i] = !l.hole[i];
    QVector<char> queued(known.size(), 0);
    QVector<int> ring;
    auto pushNeighbours = [&](int x, int y, QVector<int> &out) {
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const int ni = ny * w + nx;
            if (known[ni] || queued[ni]) continue;
            queued[ni] = 1;
            out << ni;
        }
    };
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (known[y * w + x]) pushNeighbours(x, y, ring);
    while (!ring.isEmpty()) {
        QVector<int> filled;
        filled.reserve(ring.size());
        for (int i : ring) {
            const int x = i % w, y = i / w;
            float acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h || !known[ny * w + nx]) continue;
                const float *p = l.at(nx, ny);
                for (int k = 0; k < 4; ++k) acc[k] += p[k];
                ++n;
            }
            if (!n) continue;   // cannot happen (queued from a known neighbour); kept as a guard
            float *o = l.at(x, y);
            for (int k = 0; k < 4; ++k) o[k] = acc[k] / n;
            filled << i;
        }
        for (int i : filled) known[i] = 1;   // the whole ring becomes known together (order independent)
        QVector<int> next;
        for (int i : filled) pushNeighbours(i % w, i / w, next);
        ring.swap(next);
    }
}

struct PM {
    Level *L = nullptr;
    QVector<char> validSrc;     // patch centred here lies fully inside and outside the hole
    QVector<QPoint> validList;
    QVector<int> targets;       // centre indices of patches that touch the hole
    QVector<QPoint> nnf;        // per pixel (only meaningful at targets)
    QVector<float> cost;
    std::mt19937 *rng = nullptr;

    void prepare() {
        const int w = L->w, h = L->h;
        // integral of holes for quick "patch has a hole pixel" queries
        QVector<int> I(size_t(w + 1) * (h + 1), 0);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            I[size_t(y + 1) * (w + 1) + x + 1] = L->isHole(x, y) + I[size_t(y) * (w + 1) + x + 1] + I[size_t(y + 1) * (w + 1) + x] - I[size_t(y) * (w + 1) + x];
        auto holes = [&](int x0, int y0, int x1, int y1) {   // inclusive, clamped
            x0 = qMax(0, x0); y0 = qMax(0, y0); x1 = qMin(w - 1, x1); y1 = qMin(h - 1, y1);
            if (x1 < x0 || y1 < y0) return 0;
            return I[size_t(y1 + 1) * (w + 1) + x1 + 1] - I[size_t(y0) * (w + 1) + x1 + 1] - I[size_t(y1 + 1) * (w + 1) + x0] + I[size_t(y0) * (w + 1) + x0];
        };
        validSrc.fill(0, size_t(w) * h);
        validList.clear(); targets.clear();
        for (int y = kR; y < h - kR; ++y) for (int x = kR; x < w - kR; ++x) {
            const int n = holes(x - kR, y - kR, x + kR, y + kR);
            if (!n) { validSrc[size_t(y) * w + x] = 1; validList << QPoint(x, y); }
        }
        // target centres: any patch covering a hole pixel; centres clamped into the valid centre range
        QVector<char> isT(size_t(w) * h, 0);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            if (!L->isHole(x, y)) continue;
            for (int cy = qMax(kR, y - kR); cy <= qMin(h - 1 - kR, y + kR); ++cy)
                for (int cx = qMax(kR, x - kR); cx <= qMin(w - 1 - kR, x + kR); ++cx) isT[size_t(cy) * w + cx] = 1;
        }
        for (int i = 0; i < isT.size(); ++i) if (isT[i]) targets << i;
        nnf.fill(QPoint(-1, -1), size_t(w) * h);
        cost.fill(std::numeric_limits<float>::max(), size_t(w) * h);
    }
    float dist(int tx, int ty, int sx, int sy, float cap) const {
        float d = 0;
        for (int dy = -kR; dy <= kR; ++dy) {
            const float *a = L->at(tx - kR, ty + dy), *b = L->at(sx - kR, sy + dy);
            for (int i = 0; i < (2 * kR + 1) * 4; ++i) { const float e = a[i] - b[i]; d += e * e; }
            if (d >= cap) return d;
        }
        return d;
    }
    bool valid(int x, int y) const { return x >= 0 && y >= 0 && x < L->w && y < L->h && validSrc[size_t(y) * L->w + x]; }
    void tryCand(int t, int tx, int ty, int sx, int sy) {
        if (!valid(sx, sy)) return;
        if (sx == tx && sy == ty) return;
        const float d = dist(tx, ty, sx, sy, cost[t]);
        if (d < cost[t]) { cost[t] = d; nnf[t] = QPoint(sx, sy); }
    }
    void randomInit() {
        std::uniform_int_distribution<int> pick(0, validList.size() - 1);
        for (int t : targets) {
            const QPoint s = validList[pick(*rng)];
            nnf[t] = s;
            cost[t] = dist(t % L->w, t / L->w, s.x(), s.y(), std::numeric_limits<float>::max());
        }
    }
    void recost() {
        for (int t : targets) {
            const QPoint s = nnf[t];
            if (!valid(s.x(), s.y())) {
                std::uniform_int_distribution<int> pick(0, validList.size() - 1);
                nnf[t] = validList[pick(*rng)];
            }
            cost[t] = dist(t % L->w, t / L->w, nnf[t].x(), nnf[t].y(), std::numeric_limits<float>::max());
        }
    }
    void iterate(int iters) {
        const int w = L->w;
        std::uniform_real_distribution<double> u(-1, 1);
        QVector<char> isT(size_t(w) * L->h, 0);
        for (int t : targets) isT[t] = 1;
        for (int it = 0; it < iters; ++it) {
            const bool fwd = !(it & 1);
            const int dir = fwd ? -1 : 1;
            for (int k = 0; k < targets.size(); ++k) {
                const int t = targets[fwd ? k : targets.size() - 1 - k];
                const int tx = t % w, ty = t / w;
                // propagation from the already visited neighbours
                for (int a = 0; a < 2; ++a) {
                    const int nx = tx + (a == 0 ? dir : 0), ny = ty + (a == 1 ? dir : 0);
                    if (nx < 0 || ny < 0 || nx >= w || ny >= L->h || !isT[size_t(ny) * w + nx]) continue;
                    const QPoint s = nnf[size_t(ny) * w + nx];
                    tryCand(t, tx, ty, s.x() - (nx - tx), s.y() - (ny - ty));
                }
                // random search with halving radius
                for (double rad = qMax(w, L->h); rad >= 1; rad /= 2) {
                    const QPoint s = nnf[t];
                    tryCand(t, tx, ty, int(std::lround(s.x() + u(*rng) * rad)), int(std::lround(s.y() + u(*rng) * rad)));
                }
            }
        }
    }
    // EM voting step: hole pixels become the weighted mean of the source pixels that their patches predict
    void vote() {
        const int w = L->w, h = L->h;
        QVector<double> acc(size_t(w) * h * 4, 0.0), wsum(size_t(w) * h, 0.0);
        // similarity weights relative to the median-ish patch cost
        QVector<float> cs;
        cs.reserve(targets.size());
        for (int t : targets) cs << cost[t];
        std::nth_element(cs.begin(), cs.begin() + cs.size() * 3 / 4, cs.end());
        const double sigma2 = qMax(1.0, double(cs[cs.size() * 3 / 4]));
        for (int t : targets) {
            const int tx = t % w, ty = t / w;
            const QPoint s = nnf[t];
            const double wt = std::exp(-cost[t] / (2 * sigma2)) + 1e-6;
            for (int dy = -kR; dy <= kR; ++dy) for (int dx = -kR; dx <= kR; ++dx) {
                const int x = tx + dx, y = ty + dy;
                if (!L->isHole(x, y)) continue;
                const float *p = L->at(s.x() + dx, s.y() + dy);
                double *a = acc.data() + (size_t(y) * w + x) * 4;
                for (int k = 0; k < 4; ++k) a[k] += p[k] * wt;
                wsum[size_t(y) * w + x] += wt;
            }
        }
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const double ws = wsum[size_t(y) * w + x];
            if (!L->isHole(x, y) || ws <= 0) continue;
            float *o = L->at(x, y);
            const double *a = acc.data() + (size_t(y) * w + x) * 4;
            for (int k = 0; k < 4; ++k) o[k] = float(a[k] / ws);
        }
    }
};
}

QImage contentAwareFill(const QImage &srcIn, const QImage &maskIn, quint32 seed, const std::function<bool(int)> &progress) {
    QImage src = straight(srcIn);
    const int W = src.width(), H = src.height();
    if (!W || !H || maskIn.isNull()) return premul(src);
    QImage mask = maskIn.size() == src.size() ? maskIn : maskIn.scaled(src.size());
    // Alpha8 masks use their alpha; other images their grey level (times alpha)
    const bool alphaMask = mask.format() == QImage::Format_Alpha8;
    QImage m = mask.convertToFormat(QImage::Format_ARGB32);
    QRect hb;
    QVector<char> holeFull(size_t(W) * H, 0);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const QRgb mc = reinterpret_cast<const QRgb *>(m.constScanLine(y))[x];
        const int v = alphaMask ? qAlpha(mc) : qGray(mc) * qAlpha(mc) / 255;
        if (v > 127) { holeFull[size_t(y) * W + x] = 1; hb |= QRect(x, y, 1, 1); }
    }
    if (hb.isEmpty()) return premul(src);
    // region of interest: the hole's box grown by its size (at least 48 px), so the search stays local and fast
    const int grow = qMax(48, qMax(hb.width(), hb.height()));
    const QRect roi = hb.adjusted(-grow, -grow, grow, grow) & src.rect();
    // Very large regions are filled at a reduced scale (bounded memory and time), then the hole
    // pixels are scaled back; the pixels outside the hole are never touched.
    const qint64 roiPx = qint64(roi.width()) * roi.height();
    if (roiPx > kMaxFillPixels) {
        const double k = std::sqrt(double(kMaxFillPixels) / double(roiPx));
        const QSize small(qMax(1, int(roi.width() * k)), qMax(1, int(roi.height() * k)));
        QImage holeImg(roi.size(), QImage::Format_Alpha8);
        holeImg.fill(0);
        for (int y = 0; y < roi.height(); ++y) {
            uchar *hm = holeImg.scanLine(y);
            for (int x = 0; x < roi.width(); ++x) hm[x] = holeFull[size_t(y + roi.top()) * W + x + roi.left()] ? 255 : 0;
        }
        const QImage smallSrc = src.copy(roi).scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        // any hole coverage in the reduced image counts as hole (so no hole colour leaks into the fill)
        QImage smallMask = holeImg.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int y = 0; y < smallMask.height(); ++y) { uchar *mm = smallMask.scanLine(y); for (int x = 0; x < smallMask.width(); ++x) mm[x] = mm[x] ? 255 : 0; }
        const QImage filled = straight(contentAwareFill(smallSrc, smallMask, seed, progress));
        if (filled.isNull() || filled.size() != small) return premul(srcIn);
        const QImage up = filled.scaled(roi.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < roi.height(); ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(src.scanLine(y + roi.top()));
            const QRgb *u = reinterpret_cast<const QRgb *>(up.constScanLine(y));
            for (int x = 0; x < roi.width(); ++x)
                if (holeFull[size_t(y + roi.top()) * W + x + roi.left()]) d[x + roi.left()] = u[x];
        }
        return premul(src);
    }
    // NB: the pyramid vector grows below, so never keep a reference to one of its levels across an
    // append. (0.3.0 kept `Level &L0 = pyr[0]` and used it after the pyramid was built: a
    // use-after-free whenever the hole was more than 14 px across -- the Content-Aware Remove crash.)
    QVector<Level> pyr;
    pyr.reserve(16);
    {
    Level L0;
    L0.w = roi.width(); L0.h = roi.height();
    L0.px.resize(size_t(L0.w) * L0.h * 4);
    L0.hole.resize(size_t(L0.w) * L0.h);
    for (int y = 0; y < L0.h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(src.constScanLine(y + roi.top()));
        for (int x = 0; x < L0.w; ++x) {
            const QRgb c = s[x + roi.left()];
            float *p = L0.at(x, y);
            p[0] = qRed(c); p[1] = qGreen(c); p[2] = qBlue(c); p[3] = qAlpha(c);
            L0.hole[size_t(y) * L0.w + x] = holeFull[size_t(y + roi.top()) * W + x + roi.left()];
        }
    }
    pyr << std::move(L0);
    }
    // pyramid: until the hole is a few patches wide
    int holeDim = qMax(hb.width(), hb.height());
    while (holeDim > 2 * (2 * kR + 1) && pyr.last().w / 2 > 4 * kR && pyr.last().h / 2 > 4 * kR) {
        pyr << downsample(pyr.last());
        holeDim /= 2;
    }
    std::mt19937 rng(seed);
    const int levels = pyr.size();
    QVector<QPoint> prevNnf;
    int prevW = 0;
    for (int li = levels - 1; li >= 0; --li) {
        Level &L = pyr[li];
        PM pm;
        pm.L = &L; pm.rng = &rng;
        pm.prepare();
        if (pm.validList.isEmpty() || pm.targets.isEmpty()) { diffuseFill(L); prevNnf.clear(); continue; }
        if (li == levels - 1 || prevNnf.isEmpty()) {
            diffuseFill(L);
            pm.randomInit();
        } else {
            for (int t : pm.targets) {
                const int x = t % L.w, y = t / L.w;
                const int px = qMin(x / 2, prevW - 1), py = y / 2;
                const QPoint s = prevNnf.value(size_t(py) * prevW + px, QPoint(-1, -1));
                pm.nnf[t] = s.x() < 0 ? QPoint(-1, -1) : QPoint(2 * s.x() + (x & 1), 2 * s.y() + (y & 1));
            }
            // initial colours: diffuse, then vote with the upsampled field
            diffuseFill(L);
            pm.recost();
            pm.vote();
            pm.recost();
        }
        const int em = li == 0 ? 2 : 4;
        for (int e = 0; e < em; ++e) {
            pm.iterate(li == 0 ? 3 : 4);
            pm.vote();
            if (e + 1 < em) pm.recost();
            if (progress && !progress(int(100.0 * ((levels - 1 - li) * em + e + 1) / (levels * em)))) return premul(srcIn);
        }
        prevNnf = pm.nnf;
        prevW = L.w;
    }
    const Level &L0 = pyr[0];   // the pyramid is complete: no more appends from here on
    if (L0.w != roi.width() || L0.h != roi.height()) return premul(srcIn);
    for (int y = 0; y < L0.h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(src.scanLine(y + roi.top()));
        for (int x = 0; x < L0.w; ++x) {
            if (!L0.isHole(x, y)) continue;
            const float *p = L0.at(x, y);
            d[x + roi.left()] = qRgba(c255(p[0]), c255(p[1]), c255(p[2]), c255(p[3]));
        }
    }
    return premul(src);
}

// ---------------------------------------------------------------- liquify
QStringList Liquify::modeNames() {
    return {QStringLiteral("Forward Warp"), QStringLiteral("Pinch"), QStringLiteral("Bloat"),
            QStringLiteral("Twirl Clockwise"), QStringLiteral("Twirl Counterclockwise"), QStringLiteral("Reconstruct")};
}
Liquify::Liquify(const QImage &src) { setSource(src); }
void Liquify::setSource(const QImage &src) {
    m_src = premul(src);
    m_w = m_src.width(); m_h = m_src.height();
    m_d.fill(QPointF(0, 0), size_t(m_w) * m_h);
}
void Liquify::reset() { m_d.fill(QPointF(0, 0)); }
bool Liquify::isIdentity() const {
    for (const QPointF &p : m_d) if (std::abs(p.x()) > 1e-6 || std::abs(p.y()) > 1e-6) return false;
    return true;
}
QPointF Liquify::sampleD(double x, double y) const {
    x = qBound(0.0, x, double(m_w - 1)); y = qBound(0.0, y, double(m_h - 1));
    const int x0 = int(x), y0 = int(y), x1 = qMin(x0 + 1, m_w - 1), y1 = qMin(y0 + 1, m_h - 1);
    const double fx = x - x0, fy = y - y0;
    auto D = [&](int xx, int yy) { return m_d[size_t(yy) * m_w + xx]; };
    return D(x0, y0) * (1 - fx) * (1 - fy) + D(x1, y0) * fx * (1 - fy) + D(x0, y1) * (1 - fx) * fy + D(x1, y1) * fx * fy;
}
void Liquify::stroke(Mode m, QPointF from, QPointF to, double radius, double pressure) {
    // non-finite input (NaN / inf from a degenerate view mapping) would turn into undefined
    // float->int conversions below; huge radii / points are clamped to a sane range around the image
    if (!m_w || !m_h || !(radius >= 0.5) || !std::isfinite(radius) || !std::isfinite(pressure)) return;
    if (!std::isfinite(from.x()) || !std::isfinite(from.y()) || !std::isfinite(to.x()) || !std::isfinite(to.y())) return;
    if (m < ForwardWarp || m >= kModes) return;
    const double lim = 4.0 * qMax(m_w, m_h) + 16;
    radius = qMin(radius, lim);
    from = QPointF(qBound(-lim, from.x(), lim), qBound(-lim, from.y(), lim));
    to = QPointF(qBound(-lim, to.x(), lim), qBound(-lim, to.y(), lim));
    pressure = qBound(0.0, pressure, 1.0);
    const QPointF c = to;
    const QPointF mv = to - from;
    const QRect box = QRectF(c.x() - radius - 1, c.y() - radius - 1, 2 * radius + 2, 2 * radius + 2).toAlignedRect() & QRect(0, 0, m_w, m_h);
    QVector<QPointF> nd = m_d;
    for (int y = box.top(); y <= box.bottom(); ++y) for (int x = box.left(); x <= box.right(); ++x) {
        const double dx = x - c.x(), dy = y - c.y();
        const double r = std::sqrt(dx * dx + dy * dy) / radius;
        if (r >= 1) continue;
        const double f = (1 - r * r) * (1 - r * r) * pressure;
        QPointF src;   // where output pixel x now samples from (in output space, before composition)
        switch (m) {
        case ForwardWarp: src = QPointF(x, y) - mv * f; break;
        case Pinch: src = QPointF(x + dx * f * 0.15, y + dy * f * 0.15); break;
        case Bloat: src = QPointF(x - dx * f * 0.15, y - dy * f * 0.15); break;
        case TwirlCW: case TwirlCCW: {
            const double a = (m == TwirlCW ? -1 : 1) * f * 0.25;
            const double ca = std::cos(a), sa = std::sin(a);
            src = QPointF(c.x() + dx * ca - dy * sa, c.y() + dx * sa + dy * ca);
            break;
        }
        case Reconstruct: nd[size_t(y) * m_w + x] = m_d[size_t(y) * m_w + x] * (1 - f * 0.35); continue;
        default: continue;
        }
        nd[size_t(y) * m_w + x] = src - QPointF(x, y) + sampleD(src.x(), src.y());
    }
    m_d = nd;
}
QImage Liquify::render() const {
    QImage out(m_w, m_h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < m_h; ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(out.scanLine(y));
        const QRgb *s = reinterpret_cast<const QRgb *>(m_src.constScanLine(y));
        for (int x = 0; x < m_w; ++x) {
            const QPointF dd = m_d[size_t(y) * m_w + x];
            if (dd.isNull()) { d[x] = s[x]; continue; }
            double c[4];
            sample(m_src, x + dd.x(), y + dd.y(), c, Clamp);
            d[x] = packPremul(c);
        }
    }
    return out;
}

// ---------------------------------------------------------------- effect dispatch
ReplaceColorOpts replaceOptsFrom(const LiveEffect &e) {
    ReplaceColorOpts o;
    o.from = e.color.rgba();
    o.to = e.color2.rgba();
    o.tolerance = e.amount;
    o.softness = e.p[0];
    o.range = qBound(0, e.mode, 2);
    o.hue = e.hue; o.saturation = e.saturation; o.lightness = e.lightness;
    o.strength = qBound(0, e.opacity, 100) / 100.0;
    o.hard = e.flag;
    return o;
}
bool apply(QImage &img, const LiveEffect &e) {
    switch (e.type) {
    case EffectType::GradientMap: img = gradientMap(img, e.stops, e.flag, e.opacity); return true;
    case EffectType::AutoContrast: img = autoContrast(img); return true;
    case EffectType::Dither: img = dither(img, e.mode, e.mode2, e.posterize, e.palette, e.amount, e.flag); return true;
    case EffectType::Outline: img = outline(img, e.mode == 1, e.mode2 == 1, int(std::lround(e.width)), e.color, e.threshold, e.opacity); return true;
    case EffectType::PaletteSwap: img = paletteSwap(img, e.palette, e.palette2, e.flag); return true;
    case EffectType::NormalMap: img = normalMap(img, e.mode, e.amount, e.flag, e.flag2, e.mode2); return true;
    case EffectType::AmbientOcclusion: img = ambientOcclusion(img, e.width, e.amount, e.flag2, e.mode2); return true;
    case EffectType::LensCorrection: img = lensCorrection(img, e.p); return true;
    case EffectType::SmartSharpen: img = smartSharpen(img, e.amount, e.softness, e.mode, e.angle, e.p[0]); return true;
    case EffectType::SurfaceBlur: img = surfaceBlur(img, int(std::lround(e.softness)), e.threshold); return true;
    case EffectType::BilateralDenoise: img = bilateralDenoise(img, e.softness, e.amount, e.quality); return true;
    case EffectType::MakeSeamless: img = makeSeamless(img, e.mode, e.width); return true;
    case EffectType::ReplaceColor: img = replaceColorRange(img, replaceOptsFrom(e)); return true;
    default: return false;
    }
}
}
