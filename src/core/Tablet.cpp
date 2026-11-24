#include "Tablet.h"
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace tablet {

QVector<QPointF> defaultCurve() { return {QPointF(0, 0), QPointF(1, 1)}; }

QVector<QPointF> normalizedCurve(const QVector<QPointF> &pts) {
    QVector<QPointF> out;
    for (QPointF p : pts) {
        if (!std::isfinite(p.x()) || !std::isfinite(p.y())) continue;
        out << QPointF(std::clamp(p.x(), 0.0, 1.0), std::clamp(p.y(), 0.0, 1.0));
    }
    std::stable_sort(out.begin(), out.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    // drop points closer than 1/100 in x (keep the later one)
    QVector<QPointF> u;
    for (const QPointF &p : out) {
        if (!u.isEmpty() && p.x() - u.last().x() < 0.01) u.last() = p;
        else u << p;
    }
    if (u.isEmpty()) return defaultCurve();
    if (u.first().x() > 0) u.prepend(QPointF(0, u.first().x() < 0.01 ? u.first().y() : 0));
    if (u.last().x() < 1) u.append(QPointF(1, u.last().x() > 0.99 ? u.last().y() : 1));
    u.first().setX(0);
    u.last().setX(1);
    if (u.size() < 2) return defaultCurve();
    return u;
}

double mapCurve(const QVector<QPointF> &ptsIn, double p) {
    if (!std::isfinite(p)) return 1.0;
    p = std::clamp(p, 0.0, 1.0);
    const QVector<QPointF> &pts = ptsIn.size() >= 2 ? ptsIn : defaultCurve();
    const int n = int(pts.size());
    if (p <= pts.first().x()) return std::clamp(pts.first().y(), 0.0, 1.0);
    if (p >= pts.last().x()) return std::clamp(pts.last().y(), 0.0, 1.0);
    // secant slopes and Fritsch-Carlson tangents
    QVector<double> d(n - 1), m(n);
    for (int i = 0; i < n - 1; ++i) {
        const double h = pts[i + 1].x() - pts[i].x();
        d[i] = h > 1e-12 ? (pts[i + 1].y() - pts[i].y()) / h : 0;
    }
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (int i = 1; i < n - 1; ++i) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
    for (int i = 0; i < n - 1; ++i) {
        if (std::abs(d[i]) < 1e-12) { m[i] = m[i + 1] = 0; continue; }
        const double a = m[i] / d[i], b = m[i + 1] / d[i];
        const double s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    int k = 0;
    while (k < n - 2 && p > pts[k + 1].x()) ++k;
    const double h = pts[k + 1].x() - pts[k].x();
    if (h <= 1e-12) return std::clamp(pts[k + 1].y(), 0.0, 1.0);
    const double t = (p - pts[k].x()) / h, t2 = t * t, t3 = t2 * t;
    const double v = (2 * t3 - 3 * t2 + 1) * pts[k].y() + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * pts[k + 1].y() +
                     (t3 - t2) * h * m[k + 1];
    return std::clamp(v, 0.0, 1.0);
}

QString encodeCurve(const QVector<QPointF> &pts) {
    QStringList l;
    for (const QPointF &p : pts) l << QStringLiteral("%1,%2").arg(p.x(), 0, 'g', 6).arg(p.y(), 0, 'g', 6);
    return l.join(QLatin1Char(';'));
}

QVector<QPointF> decodeCurve(const QString &s) {
    QVector<QPointF> pts;
    for (const QString &part : s.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList xy = part.split(QLatin1Char(','));
        bool ok1 = false, ok2 = false;
        if (xy.size() == 2) {
            const double x = xy[0].toDouble(&ok1), y = xy[1].toDouble(&ok2);
            if (ok1 && ok2) pts << QPointF(x, y);
        }
    }
    return pts.size() >= 2 ? normalizedCurve(pts) : defaultCurve();
}

Sample lerp(const Sample &a, const Sample &b, double t) {
    Sample s;
    t = std::clamp(t, 0.0, 1.0);
    s.pressure = a.pressure + (b.pressure - a.pressure) * t;
    s.tiltX = a.tiltX + (b.tiltX - a.tiltX) * t;
    s.tiltY = a.tiltY + (b.tiltY - a.tiltY) * t;
    double dr = std::fmod(b.rotation - a.rotation, 360.0);
    if (dr > 180) dr -= 360;
    if (dr < -180) dr += 360;
    s.rotation = a.rotation + dr * t;
    s.hasTilt = a.hasTilt || b.hasTilt;
    return s;
}

double nibAngle(const Sample &s, double fallback) {
    if (!s.hasTilt) return fallback;
    if (std::abs(s.rotation) > 1e-6) return -s.rotation * M_PI / 180.0;
    if (std::hypot(s.tiltX, s.tiltY) >= 5) return std::atan2(s.tiltY, s.tiltX);
    return fallback;
}
}
