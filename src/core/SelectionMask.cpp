#include "SelectionMask.h"
#include "Effects.h"
#include "Filters.h"
#include <QPainter>
#include <QVector>
#include <cmath>

namespace selmask {

QImage blank(QSize size) {
    QImage m(size.expandedTo(QSize(0, 0)), QImage::Format_Alpha8);
    m.fill(0);
    return m;
}

QImage fromPath(const QPainterPath &p, QSize size, QPoint origin, bool antiAlias) {
    QImage m = blank(size);
    if (m.isNull() || p.isEmpty()) return m;
    QPainter q(&m);
    q.setRenderHint(QPainter::Antialiasing, antiAlias);
    q.setPen(Qt::NoPen);
    q.setBrush(Qt::black);
    q.translate(-origin);
    q.drawPath(p);
    return m;
}

QRect bounds(const QImage &mask) {
    int x0 = mask.width(), y0 = mask.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *s = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            if (s[x]) { x0 = qMin(x0, x); x1 = qMax(x1, x); y0 = qMin(y0, y); y1 = qMax(y1, y); }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

QPainterPath outline(const QImage &mask, QPoint origin) {
    bool half = false;
    for (int y = 0; y < mask.height() && !half; ++y) {
        const uchar *s = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) if (s[x] >= 128) { half = true; break; }
    }
    QImage m = mask;
    if (!half) {   // only faint pixels: show where they are
        for (int y = 0; y < m.height(); ++y) { uchar *s = m.scanLine(y); for (int x = 0; x < m.width(); ++x) if (s[x]) s[x] = 255; }
    }
    return filters::maskToPath(m, QTransform::fromTranslate(origin.x(), origin.y()));
}

template <typename F> static QImage combine(const QImage &a, const QImage &b, F f) {
    QImage r = a.copy();
    for (int y = 0; y < r.height() && y < b.height(); ++y) {
        uchar *d = r.scanLine(y);
        const uchar *s = b.constScanLine(y);
        for (int x = 0; x < r.width() && x < b.width(); ++x) d[x] = uchar(f(d[x], s[x]));
    }
    return r;
}
QImage unite(const QImage &a, const QImage &b) { return combine(a, b, [](int u, int v) { return qMax(u, v); }); }
QImage subtract(const QImage &a, const QImage &b) { return combine(a, b, [](int u, int v) { return (u * (255 - v) + 127) / 255; }); }
QImage intersect(const QImage &a, const QImage &b) { return combine(a, b, [](int u, int v) { return qMin(u, v); }); }
QImage invert(const QImage &a) {
    QImage r = a.copy();
    for (int y = 0; y < r.height(); ++y) { uchar *d = r.scanLine(y); for (int x = 0; x < r.width(); ++x) d[x] = 255 - d[x]; }
    return r;
}

QImage feather(const QImage &m, double radius) {
    QImage r = m.convertToFormat(QImage::Format_Alpha8);
    if (radius > 0) fx::blur(r, radius / 2.0);
    return r;
}

// distance (px, pixel centre to the nearest site centre) for every pixel
static QVector<float> distanceTo(const QImage &m, bool toSelected) {
    const int w = m.width(), h = m.height();
    QVector<char> site(w * h, 0);
    for (int y = 0; y < h; ++y) {
        const uchar *s = m.constScanLine(y);
        for (int x = 0; x < w; ++x) site[y * w + x] = (s[x] >= 128) == toSelected;
    }
    QVector<QPoint> v;
    fx::nearestSites(site, w, h, v);
    QVector<float> d(w * h);
    for (int i = 0; i < w * h; ++i) d[i] = std::sqrt(float(v[i].x()) * v[i].x() + float(v[i].y()) * v[i].y());
    return d;
}

QImage expand(const QImage &m, double px) {
    if (px <= 0) return m.copy();
    const QVector<float> d = distanceTo(m, true);
    QImage r = m.copy();
    for (int y = 0; y < r.height(); ++y) {
        uchar *s = r.scanLine(y);
        for (int x = 0; x < r.width(); ++x) {
            const double c = qBound(0.0, px + 0.5 - d[y * r.width() + x] + 0.5, 1.0);   // anti-aliased edge
            s[x] = uchar(qMax<int>(s[x], int(c * 255 + 0.5)));
        }
    }
    return r;
}

QImage contract(const QImage &m, double px) {
    if (px <= 0) return m.copy();
    // pixels beyond the mask count as not selected
    QImage padded = blank(m.size() + QSize(2, 2));
    { QPainter q(&padded); q.setCompositionMode(QPainter::CompositionMode_Source); q.drawImage(1, 1, m); }
    const QVector<float> d = distanceTo(padded, false);
    QImage r = m.copy();
    for (int y = 0; y < r.height(); ++y) {
        uchar *s = r.scanLine(y);
        for (int x = 0; x < r.width(); ++x) {
            const double c = qBound(0.0, d[(y + 1) * padded.width() + x + 1] - 0.5 - px + 0.5, 1.0);
            s[x] = uchar(qMin<int>(s[x], int(c * 255 + 0.5)));
        }
    }
    return r;
}

QImage border(const QImage &m, double width) {
    const double h = qMax(0.5, width / 2.0);
    return subtract(expand(m, h), contract(m, h));
}

QImage smooth(const QImage &m, double radius) {
    QImage r = feather(m, radius * 2);
    for (int y = 0; y < r.height(); ++y) { uchar *s = r.scanLine(y); for (int x = 0; x < r.width(); ++x) s[x] = s[x] >= 128 ? 255 : 0; }
    return r;
}

QImage similar(const QImage &imgIn, const QImage &m, int tolerance) {
    const QImage img = imgIn.convertToFormat(QImage::Format_ARGB32);
    // colours under the selection, in 32 levels per channel, then grown by the tolerance
    const int N = 32, step = 256 / N;
    QVector<char> bins(N * N * N, 0);
    auto bin = [&](QRgb c) { return (qRed(c) / step) * N * N + (qGreen(c) / step) * N + qBlue(c) / step; };
    bool any = false;
    for (int y = 0; y < img.height() && y < m.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        const uchar *k = m.constScanLine(y);
        for (int x = 0; x < img.width() && x < m.width(); ++x)
            if (k[x] >= 128 && qAlpha(s[x]) > 0) { bins[bin(s[x])] = 1; any = true; }
    }
    QImage r = blank(img.size());
    if (!any) return r;
    const int g = (qBound(0, tolerance, 255) + step / 2) / step;
    if (g > 0) {
        for (int axis = 0; axis < 3; ++axis) {   // separable chebyshev dilation of the colour set
            QVector<char> next = bins;
            for (int i = 0; i < N * N * N; ++i) {
                if (!bins[i]) continue;
                int c[3] = {i / (N * N), (i / N) % N, i % N};
                for (int k = -g; k <= g; ++k) {
                    int cc[3] = {c[0], c[1], c[2]};
                    cc[axis] += k;
                    if (cc[axis] < 0 || cc[axis] >= N) continue;
                    next[cc[0] * N * N + cc[1] * N + cc[2]] = 1;
                }
            }
            bins = next;
        }
    }
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        uchar *d = r.scanLine(y);
        for (int x = 0; x < img.width(); ++x) if (qAlpha(s[x]) > 0 && bins[bin(s[x])]) d[x] = 255;
    }
    return unite(r, m.size() == r.size() ? m : r);
}

QImage toGrey(const QImage &m) {
    QImage g(m.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < m.height(); ++y) {
        const uchar *s = m.constScanLine(y);
        QRgb *d = reinterpret_cast<QRgb *>(g.scanLine(y));
        for (int x = 0; x < m.width(); ++x) d[x] = qRgb(s[x], s[x], s[x]);
    }
    return g;
}

QImage fromGrey(const QImage &greyIn) {
    const QImage grey = greyIn.convertToFormat(QImage::Format_ARGB32);
    QImage m = blank(grey.size());
    for (int y = 0; y < grey.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(grey.constScanLine(y));
        uchar *d = m.scanLine(y);
        // luminance x alpha: transparent (erased) areas are not selected
        for (int x = 0; x < grey.width(); ++x) d[x] = uchar((qGray(s[x]) * qAlpha(s[x]) + 127) / 255);
    }
    return m;
}
}
