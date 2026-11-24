#include "Retouch.h"
#include "Effects.h"
#include <QVector>
#include <QtMath>

namespace retouch {

// Fast ARGB32(_Premultiplied) pixel access — avoids QImage::pixel/setPixel deep copies.
static inline QRgb atPixel(const QImage &img, int x, int y) {
    return reinterpret_cast<const QRgb *>(img.constScanLine(y))[x];
}
static inline QRgb atPixel(const QImage &img, QPoint pt) { return atPixel(img, pt.x(), pt.y()); }
static inline void putPixel(QImage &img, int x, int y, QRgb v) {
    reinterpret_cast<QRgb *>(img.scanLine(y))[x] = v;
}



static Tip g_tip;
TipScope::TipScope(Tip t) : m_prev(g_tip) { g_tip = t; }
TipScope::~TipScope() { g_tip = m_prev; }

double tipWeightAt(double dx, double dy, double r, const Tip &t) {
    const double d = t.square ? qMax(qAbs(dx), qAbs(dy)) : std::sqrt(dx * dx + dy * dy);
    return edgeWeight(d, r, t.edge);
}

static void ensure(QImage &img) {
    if (img.format() != QImage::Format_ARGB32_Premultiplied) img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

static QRect dabRect(const QImage &img, QPointF c, double r) {
    return QRect(QPoint(qFloor(c.x() - r), qFloor(c.y() - r)), QPoint(qCeil(c.x() + r), qCeil(c.y() + r))).intersected(img.rect());
}

static inline double clipAt(const QImage *clip, int x, int y) {
    if (!clip) return 1;
    if (x < 0 || y < 0 || x >= clip->width() || y >= clip->height()) return 0;
    return clip->constScanLine(y)[x] / 255.0;
}

static inline QRgb mix(QRgb a, QRgb b, double t) { // premultiplied lerp
    auto l = [t](int x, int y) { return qBound(0, int(x + (y - x) * t + 0.5), 255); };
    return qRgba(l(qRed(a), qRed(b)), l(qGreen(a), qGreen(b)), l(qBlue(a), qBlue(b)), l(qAlpha(a), qAlpha(b)));
}

// apply `f(x, y, pixel, weight)` to each pixel of the tip
template <typename F> static void forTip(QImage &img, QPointF c, double size, const QImage *clip, F f) {
    const double r = size / 2.0;
    const QRect R = dabRect(img, c, r);
    const Tip tip = g_tip;
    for (int y = R.top(); y <= R.bottom(); ++y) {
        QRgb *d = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = R.left(); x <= R.right(); ++x) {
            double w = tipWeightAt(x + 0.5 - c.x(), y + 0.5 - c.y(), r, tip) * clipAt(clip, x, y);
            if (w > 0) d[x] = f(x, y, d[x], w);
        }
    }
}

static QImage blurredAround(const QImage &img, QRect R, int pad, double radius, QPoint *origin) {
    QRect B = R.adjusted(-pad, -pad, pad, pad).intersected(img.rect());
    QImage b = img.copy(B);
    fx::blur(b, radius);
    *origin = B.topLeft();
    return b;
}

void blurDab(QImage &img, QPointF c, double size, int strength, const QImage *clip) {
    ensure(img);
    const QRect R = dabRect(img, c, size / 2.0);
    if (R.isEmpty()) return;
    QPoint o;
    const QImage b = blurredAround(img, R, 4, qMax(1.0, size / 10.0), &o);
    const double k = qBound(0, strength, 100) / 100.0;
    forTip(img, c, size, clip, [&](int x, int y, QRgb p, double w) { return mix(p, atPixel(b, x - o.x(), y - o.y()), w * k); });
}

void sharpenDab(QImage &img, QPointF c, double size, int strength, const QImage *clip) {
    ensure(img);
    const QRect R = dabRect(img, c, size / 2.0);
    if (R.isEmpty()) return;
    QPoint o;
    const QImage b = blurredAround(img, R, 4, 1.0, &o);
    const double k = qBound(0, strength, 100) / 100.0 * 0.5;
    forTip(img, c, size, clip, [&](int x, int y, QRgb p, double w) {
        QRgb bl = atPixel(b, x - o.x(), y - o.y());
        const int a = qAlpha(p);
        auto s = [&](int v, int bv) { return qBound(0, int(v + (v - bv) * w * k + 0.5), a); };
        return qRgba(s(qRed(p), qRed(bl)), s(qGreen(p), qGreen(bl)), s(qBlue(p), qBlue(bl)), a);
    });
}

void dodgeBurnDab(QImage &img, QPointF c, double size, int exposure, Range range, bool burn, const QImage *clip) {
    ensure(img);
    const double k = qBound(0, exposure, 100) / 100.0 * 0.35;
    forTip(img, c, size, clip, [&](int, int, QRgb pp, double w) {
        const int a = qAlpha(pp);
        if (!a) return pp;
        QRgb p = qUnpremultiply(pp);
        const double l = qGray(p) / 255.0;
        double t = range == Range::Shadows ? 1 - l : range == Range::Highlights ? l : 1 - qAbs(2 * l - 1);
        t = 0.1 + 0.9 * t;
        const double amt = w * k * t;
        auto f = [&](int v) { return qBound(0, int(burn ? v - v * amt : v + (255 - v) * amt + 0.5), 255); };
        return qPremultiply(qRgba(f(qRed(p)), f(qGreen(p)), f(qBlue(p)), a));
    });
}

void stampDab(QImage &img, const QImage &src, QPointF c, QPointF offset, double size, int opacity, const QImage *clip) {
    ensure(img);
    const double k = qBound(0, opacity, 100) / 100.0;
    const int ox = qRound(offset.x()), oy = qRound(offset.y());
    forTip(img, c, size, clip, [&](int x, int y, QRgb p, double w) {
        const int sx = x + ox, sy = y + oy;
        if (sx < 0 || sy < 0 || sx >= src.width() || sy >= src.height()) return p;
        return mix(p, reinterpret_cast<const QRgb *>(src.constScanLine(sy))[sx], w * k);
    });
}

void stampDabMode(QImage &img, const QImage &src, QPointF c, QPointF offset, double size, int opacity,
                  QPainter::CompositionMode mode, const QImage *clip) {
    ensure(img);
    const double r = size / 2.0, k = qBound(0, opacity, 100) / 100.0;
    const QRect R = dabRect(img, c, r);
    if (R.isEmpty()) return;
    const int ox = qRound(offset.x()), oy = qRound(offset.y());
    QImage layer(R.size(), QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    const Tip tip = g_tip;
    for (int y = R.top(); y <= R.bottom(); ++y) {
        QRgb *l = reinterpret_cast<QRgb *>(layer.scanLine(y - R.top()));
        const int sy = y + oy;
        if (sy < 0 || sy >= src.height()) continue;
        const QRgb *s = reinterpret_cast<const QRgb *>(src.constScanLine(sy));
        for (int x = R.left(); x <= R.right(); ++x) {
            const int sx = x + ox;
            if (sx < 0 || sx >= src.width()) continue;
            const double w = tipWeightAt(x + 0.5 - c.x(), y + 0.5 - c.y(), r, tip) * clipAt(clip, x, y) * k;
            if (w <= 0) continue;
            const QRgb p = s[sx];
            l[x - R.left()] = qRgba(int(qRed(p) * w + 0.5), int(qGreen(p) * w + 0.5), int(qBlue(p) * w + 0.5), int(qAlpha(p) * w + 0.5));
        }
    }
    QPainter p(&img);
    p.setCompositionMode(mode);
    p.drawImage(R.topLeft(), layer);
}

double edgeWeight(double d, double r, int edge) {
    if (r <= 0 || d >= r) return 0;
    const double inner = r * (1.0 - qBound(0, edge, 100) / 100.0 * 0.95);
    if (d <= inner) return 1;
    const double t = (d - inner) / qMax(1e-6, r - inner);
    return 1 - t * t * (3 - 2 * t);
}

namespace {
// separable box blur (3 passes ~ Gaussian) of a float plane
void boxBlur(QVector<float> &a, int w, int h, int r) {
    if (r < 1) return;
    QVector<float> t(qMax(w, h));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            float *row = a.data() + y * w;
            double acc = 0;
            for (int i = -r; i <= r; ++i) acc += row[qBound(0, i, w - 1)];
            for (int x = 0; x < w; ++x) {
                t[x] = float(acc / (2 * r + 1));
                acc += row[qMin(w - 1, x + r + 1)] - row[qMax(0, x - r)];
            }
            std::copy(t.begin(), t.begin() + w, row);
        }
        for (int x = 0; x < w; ++x) {
            double acc = 0;
            for (int i = -r; i <= r; ++i) acc += a[qBound(0, i, h - 1) * w + x];
            for (int y = 0; y < h; ++y) {
                t[y] = float(acc / (2 * r + 1));
                acc += a[qMin(h - 1, y + r + 1) * w + x] - a[qMax(0, y - r) * w + x];
            }
            for (int y = 0; y < h; ++y) a[y * w + x] = t[y];
        }
    }
}

struct Patch {
    QRect R;                 // region in image coordinates
    QVector<float> ch[4];    // premultiplied r, g, b, a
};

Patch readPatch(const QImage &img, const QRect &R, QPoint shift, const QImage &fallback) {
    Patch p;
    p.R = R;
    const int w = R.width(), h = R.height();
    for (auto &c : p.ch) c.resize(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int ix = R.left() + x, iy = R.top() + y;
            const int sx = ix + shift.x(), sy = iy + shift.y();
            QRgb v;
            if (sx >= 0 && sy >= 0 && sx < img.width() && sy < img.height()) v = reinterpret_cast<const QRgb *>(img.constScanLine(sy))[sx];
            else v = reinterpret_cast<const QRgb *>(fallback.constScanLine(iy))[ix];
            p.ch[0][y * w + x] = qRed(v); p.ch[1][y * w + x] = qGreen(v); p.ch[2][y * w + x] = qBlue(v); p.ch[3][y * w + x] = qAlpha(v);
        }
    return p;
}

// membrane: blur(v * m) / blur(m) with m = 1 outside the tip, 0 inside
void membrane(Patch &p, const QVector<float> &m, const QVector<float> &mb, int r) {
    const int n = p.R.width() * p.R.height();
    for (auto &c : p.ch) {
        QVector<float> wv(n);
        for (int i = 0; i < n; ++i) wv[i] = c[i] * m[i];
        boxBlur(wv, p.R.width(), p.R.height(), r);
        for (int i = 0; i < n; ++i) c[i] = mb[i] > 1e-4f ? wv[i] / mb[i] : c[i];
    }
}
}

void healDab(QImage &img, const QImage &srcIn, QPointF c, QPointF offset, double size, int edge, HealMode mode, const QImage *clip, QPoint srcOrigin) {
    ensure(img);
    const QImage src = srcIn.format() == QImage::Format_ARGB32_Premultiplied ? srcIn : srcIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const double r = qMax(0.5, size / 2.0);
    const int br = qMax(1, qCeil(r * 0.75));        // membrane blur radius (3 box passes)
    const int pad = 3 * br + 2;
    const QRect tip = dabRect(img, c, r);
    if (tip.isEmpty()) return;
    const QRect R = tip.adjusted(-pad, -pad, pad, pad).intersected(img.rect());
    const int w = R.width(), h = R.height(), n = w * h;
    const QPoint shift(qRound(offset.x()), qRound(offset.y()));
    Patch dst = readPatch(img, R, QPoint(0, 0), img);
    Patch sp = readPatch(src, R, shift - srcOrigin, img);
    Patch md = dst, ms = sp;
    // the tip's own coverage (core = 0 in the membrane weights, ring = 1)
    QVector<float> tw(n), m(n);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double dd = QLineF(c, QPointF(R.left() + x + 0.5, R.top() + y + 0.5)).length();
            tw[y * w + x] = float(mode == HealMode::Replace ? (dd < r ? 1.0 : 0.0) : edgeWeight(dd, r, edge));
            m[y * w + x] = dd < r ? 0.f : 1.f;
        }
    QVector<float> mb = m;
    boxBlur(mb, w, h, br);
    membrane(md, m, mb, br);
    membrane(ms, m, mb, br);
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(R.top() + y));
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            const double wgt = tw[i] * clipAt(clip, R.left() + x, R.top() + y);
            if (wgt <= 0) continue;
            // Alpha still follows the Poisson membrane so coverage heals with the source.
            // Colour uses the source's *luminance* high-pass on the destination's colour
            // membrane. A per-channel RGB Poisson (sp + Md - Ms) is mathematically fine
            // but shifts hue: dark dots on a red field become green when transferred onto
            // brown skin, because most of the residual sits in the red channel. Photoshop
            // keeps the destination's colour and only transfers texture; luma texture
            // does the same (dark dots stay dark brown).
            double o[4];
            o[3] = qBound(0.0, double(sp.ch[3][i] + (md.ch[3][i] - ms.ch[3][i])), 255.0);
            auto unpre = [](const Patch &p, int i, double out[3]) {
                const double a = p.ch[3][i];
                if (a > 1.0) {
                    out[0] = p.ch[0][i] * 255.0 / a;
                    out[1] = p.ch[1][i] * 255.0 / a;
                    out[2] = p.ch[2][i] * 255.0 / a;
                } else {
                    out[0] = p.ch[0][i];
                    out[1] = p.ch[1][i];
                    out[2] = p.ch[2][i];
                }
            };
            auto luma = [](const double c[3]) { return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]; };
            double sRGB[3], msRGB[3], mdRGB[3];
            unpre(sp, i, sRGB);
            unpre(ms, i, msRGB);
            unpre(md, i, mdRGB);
            const double tex = luma(sRGB) - luma(msRGB);
            const double ymd = luma(mdRGB);
            const double yout = ymd + tex;
            if (ymd > 1.0) {
                const double scale = yout / ymd;
                for (int k = 0; k < 3; ++k) o[k] = mdRGB[k] * scale;
            } else {
                for (int k = 0; k < 3; ++k) o[k] = mdRGB[k] + tex;
            }
            for (int k = 0; k < 3; ++k) {
                o[k] = qBound(0.0, o[k], 255.0) * (o[3] / 255.0);
                o[k] = qBound(0.0, o[k], o[3]);
            }
            const QRgb healed = qRgba(int(o[0] + 0.5), int(o[1] + 0.5), int(o[2] + 0.5), int(o[3] + 0.5));
            const QRgb cur = line[R.left() + x];
            if (mode == HealMode::Lighten && qGray(healed) <= qGray(cur)) continue;
            if (mode == HealMode::Darken && qGray(healed) >= qGray(cur)) continue;
            line[R.left() + x] = mix(cur, healed, wgt);
        }
    }
}

QPointF spotHealOffset(const QImage &imgIn, QPointF c, double size) {
    const QImage img = imgIn.format() == QImage::Format_ARGB32_Premultiplied ? imgIn : imgIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const double r = qMax(0.5, size / 2.0);
    QPointF best(size, 0);
    double bestCost = 1e300;
    // compare a ring just outside the tip: the candidate patch's ring should look like the spot's
    for (double dist : {size * 0.8, size * 1.1, size * 1.6, size * 2.2}) {
        for (int k = 0; k < 8; ++k) {
            const double a = k * M_PI / 4;
            const QPointF off(std::cos(a) * dist, std::sin(a) * dist);
            const QPointF cc = c + off;
            if (cc.x() - r < 0 || cc.y() - r < 0 || cc.x() + r > img.width() || cc.y() + r > img.height()) continue;
            double cost = 0; int cnt = 0;
            const int steps = qMax(8, int(r * 2));
            for (int s = 0; s < steps; ++s) {
                const double t = 2 * M_PI * s / steps;
                const QPointF ringP(std::cos(t) * r * 1.2, std::sin(t) * r * 1.2);
                const QPoint p1 = (c + ringP).toPoint(), p2 = (cc + ringP).toPoint();
                if (!img.rect().contains(p1) || !img.rect().contains(p2)) continue;
                const QRgb v1 = atPixel(img, p1), v2 = atPixel(img, p2);
                cost += qPow(qRed(v1) - qRed(v2), 2) + qPow(qGreen(v1) - qGreen(v2), 2) + qPow(qBlue(v1) - qBlue(v2), 2);
                ++cnt;
            }
            // prefer flat-ish neighbours: also penalise the candidate's inner variance relative to the ring
            if (cnt && cost / cnt < bestCost) { bestCost = cost / cnt; best = off; }
        }
        if (bestCost < 1e300) break;
    }
    if (bestCost >= 1e300) {
        // Near a corner every ring candidate can fall off the image; step toward the
        // centre so Spot Healing still has a source instead of an off-image offset.
        const QPointF mid(img.width() * 0.5, img.height() * 0.5);
        QPointF dir = mid - c;
        const double len = std::hypot(dir.x(), dir.y());
        if (len > 1e-3) {
            dir *= size / len;
            QPointF cc = c + dir;
            const double maxX = img.width() - r, maxY = img.height() - r;
            if (maxX > r && maxY > r) {
                cc.setX(qBound(r, cc.x(), maxX));
                cc.setY(qBound(r, cc.y(), maxY));
                best = cc - c;
            } else {
                best = QPointF(0, 0);
            }
        } else {
            best = QPointF(0, 0);
        }
    }
    return best;
}

void spotHealDab(QImage &img, QPointF c, double size, int edge, HealMode mode, const QImage *clip) {
    ensure(img);
    const QPointF off = spotHealOffset(img, c, size);
    const int pad = qCeil(size) * 3 + 4;
    const QRect tip = dabRect(img, c + off, size / 2.0);
    const QRect S = tip.adjusted(-pad, -pad, pad, pad).intersected(img.rect());
    const QImage snap = img.copy(S);   // only the neighbourhood, not the whole image
    healDab(img, snap, c, off, size, edge, mode, clip, S.topLeft());
}

void Smudger::start(const QImage &imgIn, QPointF c, double size) {
    QImage img = imgIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_size = qMax(1.0, size);
    const int n = qCeil(m_size) + 2;
    m_carry = QImage(n, n, QImage::Format_ARGB32_Premultiplied);
    m_carry.fill(0);
    const QPoint tl(qFloor(c.x()) - n / 2, qFloor(c.y()) - n / 2);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            QPoint s = tl + QPoint(x, y);
            if (img.rect().contains(s)) putPixel(m_carry, x, y, atPixel(img, s));
        }
}

void Smudger::startWithColor(const QColor &c, double size) {
    m_size = qMax(1.0, size);
    const int n = qCeil(m_size) + 2;
    m_carry = QImage(n, n, QImage::Format_ARGB32_Premultiplied);
    m_carry.fill(QColor(c.red(), c.green(), c.blue()).rgba());
}

void Smudger::dab(QImage &img, QPointF c, int pressure, const QImage *clip) {
    ensure(img);
    if (m_carry.isNull()) start(img, c, m_size);
    const int n = m_carry.width();
    const QPoint tl(qFloor(c.x()) - n / 2, qFloor(c.y()) - n / 2);
    const double k = qBound(0, pressure, 100) / 100.0;
    forTip(img, c, m_size, clip, [&](int x, int y, QRgb p, double w) {
        const int cx = x - tl.x(), cy = y - tl.y();
        if (cx < 0 || cy < 0 || cx >= n || cy >= n) return p;
        QRgb carried = atPixel(m_carry, cx, cy);
        QRgb out = mix(p, carried, w * k);
        putPixel(m_carry, cx, cy, out);
        return out;
    });
}

QVector<QPointF> dabsAlong(QPointF a, QPointF b, double spacing) {
    QVector<QPointF> r;
    const double len = QLineF(a, b).length();
    const int n = qMax(1, qCeil(len / qMax(0.5, spacing)));
    for (int i = 1; i <= n; ++i) r.append(a + (b - a) * (double(i) / n));
    return r;
}

} // namespace retouch
