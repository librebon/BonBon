#include "Filters.h"
#include "Effects.h"
#include <QBitmap>
#include <QRegion>
#include <QtMath>
#include <cmath>
#include <vector>

namespace filters {

static inline int clamp255(double v) { return qBound(0, int(std::lround(v)), 255); }

QImage apply(const QImage &srcIn, Kind k, double p1, double p2) {
    QImage src = srcIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    switch (k) {
    case Kind::Invert:
    case Kind::Desaturate:
    case Kind::BrightnessContrast: {
        QImage img = src.convertToFormat(QImage::Format_ARGB32);
        double c = (100.0 + p2) / 100.0;
        for (int y = 0; y < img.height(); ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                QRgb px = d[x];
                int r = qRed(px), g = qGreen(px), b = qBlue(px), a = qAlpha(px);
                if (k == Kind::Invert) { r = 255 - r; g = 255 - g; b = 255 - b; }
                else if (k == Kind::Desaturate) { int l = qGray(px); r = g = b = l; }
                else {
                    auto f = [&](int v) { return clamp255((v - 128) * c + 128 + p1 * 2.55); };
                    r = f(r); g = f(g); b = f(b);
                }
                d[x] = qRgba(r, g, b, a);
            }
        }
        return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    case Kind::Blur: { fx::blur(src, 1); return src; }
    case Kind::BlurMore: { fx::blur(src, 2.5); return src; }
    case Kind::GaussianBlur: { fx::blur(src, qMax(0.1, p1)); return src; }
    case Kind::Sharpen:
    case Kind::SharpenMore: {
        QImage bl = src.copy();
        fx::blur(bl, 1);
        const double amt = k == Kind::Sharpen ? 0.6 : 1.4;
        const int w = src.width(), h = src.height();
        for (int y = 0; y < h; ++y) {
            QRgb *d = reinterpret_cast<QRgb *>(src.scanLine(y));
            const QRgb *b = reinterpret_cast<const QRgb *>(bl.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                const int a = qAlpha(d[x]);
                const int oR = qRed(d[x]), oG = qGreen(d[x]), oB = qBlue(d[x]);
                d[x] = qRgba(qBound(0, int(std::lround(oR + (oR - qRed(b[x])) * amt)), a),
                             qBound(0, int(std::lround(oG + (oG - qGreen(b[x])) * amt)), a),
                             qBound(0, int(std::lround(oB + (oB - qBlue(b[x])) * amt)), a), a);
            }
        }
        return src;
    }
    }
    return src;
}

static inline bool similar(QRgb a, QRgb b, int tol) {
    return qAbs(qRed(a) - qRed(b)) <= tol && qAbs(qGreen(a) - qGreen(b)) <= tol &&
           qAbs(qBlue(a) - qBlue(b)) <= tol && qAbs(qAlpha(a) - qAlpha(b)) <= tol;
}

QImage floodMask(const QImage &imgIn, QPoint seed, int tol, bool contiguous) {
    QImage img = imgIn.convertToFormat(QImage::Format_ARGB32);
    QImage mask(img.size(), QImage::Format_Alpha8);
    mask.fill(0);
    if (!img.rect().contains(seed)) return mask;
    const QRgb ref = reinterpret_cast<const QRgb *>(img.constScanLine(seed.y()))[seed.x()];
    const int w = img.width(), h = img.height();
    if (!contiguous) {
        for (int y = 0; y < h; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            uchar *m = mask.scanLine(y);
            for (int x = 0; x < w; ++x) if (similar(s[x], ref, tol)) m[x] = 255;
        }
        return mask;
    }
    std::vector<QPoint> stack{seed};
    while (!stack.empty()) {
        QPoint p = stack.back();
        stack.pop_back();
        int y = p.y();
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        uchar *m = mask.scanLine(y);
        int x = p.x();
        if (m[x] || !similar(s[x], ref, tol)) continue;
        int xl = x, xr = x;
        while (xl > 0 && !m[xl - 1] && similar(s[xl - 1], ref, tol)) --xl;
        while (xr < w - 1 && !m[xr + 1] && similar(s[xr + 1], ref, tol)) ++xr;
        for (int i = xl; i <= xr; ++i) m[i] = 255;
        for (int ny : {y - 1, y + 1}) {
            if (ny < 0 || ny >= h) continue;
            const QRgb *ns = reinterpret_cast<const QRgb *>(img.constScanLine(ny));
            const uchar *nm = mask.constScanLine(ny);
            bool run = false;
            for (int i = xl; i <= xr; ++i) {
                bool ok = !nm[i] && similar(ns[i], ref, tol);
                if (ok && !run) stack.push_back(QPoint(i, ny));
                run = ok;
            }
        }
    }
    return mask;
}

QPainterPath maskToPath(const QImage &mask, const QTransform &toCanvas) {
    QImage mono(mask.size(), QImage::Format_MonoLSB);
    mono.setColor(0, qRgb(255, 255, 255));
    mono.setColor(1, qRgb(0, 0, 0));
    mono.fill(0);
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *m = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) if (m[x] > 127) mono.setPixel(x, y, 1);
    }
    QRegion rgn(QBitmap::fromImage(mono));
    QPainterPath path;
    path.addRegion(rgn);
    return toCanvas.map(path.simplified());
}


QRect opaqueBounds(const QImage &imgIn) {
    QImage img = imgIn.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(s[x])) { x0 = qMin(x0, x); x1 = qMax(x1, x); y0 = qMin(y0, y); y1 = qMax(y1, y); }
    }
    if (x1 < 0) return {};
    return QRect(QPoint(x0, y0), QPoint(x1, y1));
}

} // namespace filters
