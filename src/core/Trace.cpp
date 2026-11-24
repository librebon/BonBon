#include "core/Trace.h"
#include "io/Quantize.h"
#include "core/Document.h"
#include "core/Paths.h"
#include <QHash>
#include <QLineF>
#include <QSet>
#include <algorithm>
#include <functional>
#include <numeric>
#include <QtConcurrent/QtConcurrentMap>
#include <cmath>

namespace trace {
namespace {

inline bool cancelled(const std::atomic_bool *c) { return c && c->load(std::memory_order_relaxed); }
inline double cross(const QPointF &a, const QPointF &b) { return a.x() * b.y() - a.y() * b.x(); }
inline double dot(const QPointF &a, const QPointF &b) { return a.x() * b.x() + a.y() * b.y(); }
inline double len(const QPointF &a) { return std::sqrt(dot(a, a)); }
double distToLine(const QPointF &p, const QPointF &a, const QPointF &b) {
    const QPointF d = b - a;
    const double l = len(d);
    if (l < 1e-12) return len(p - a);
    return std::abs(cross(d, p - a)) / l;
}
double distToSeg(const QPointF &p, const QPointF &a, const QPointF &b) {
    const QPointF d = b - a;
    const double l2 = dot(d, d);
    if (l2 < 1e-12) return len(p - a);
    const double t = qBound(0.0, dot(p - a, d) / l2, 1.0);
    return len(p - (a + d * t));
}
QPointF bez(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p3, double t) {
    const double u = 1 - t;
    return p0 * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + p3 * (t * t * t);
}

// Douglas-Peucker on an open run pts[a..b] (indices modulo n); keeps flags in `keep`.
void dpRun(const QPolygonF &pts, int a, int b, double tol, QVector<bool> &keep) {
    const int n = pts.size();
    struct R { int a, b; };
    QVector<R> st{{a, b}};
    while (!st.isEmpty()) {
        const R r = st.takeLast();
        if (r.b - r.a < 2) continue;
        const QPointF &A = pts[r.a % n], &B = pts[r.b % n];
        double best = -1;
        int bi = -1;
        for (int i = r.a + 1; i < r.b; ++i) {
            const double d = distToSeg(pts[i % n], A, B);
            if (d > best) { best = d; bi = i; }
        }
        if (best > tol) {
            keep[bi % n] = true;
            st << R{r.a, bi} << R{bi, r.b};
        }
    }
}

QVector<int> dpClosedIdx(const QPolygonF &pts, double tol) {
    const int n = pts.size();
    QVector<int> idx;
    if (n <= 3) { for (int i = 0; i < n; ++i) idx << i; return idx; }
    int far = 0;
    double fd = -1;
    for (int i = 1; i < n; ++i) { const double d = len(pts[i] - pts[0]); if (d > fd) { fd = d; far = i; } }
    QVector<bool> keep(n, false);
    keep[0] = keep[far] = true;
    dpRun(pts, 0, far, tol, keep);
    dpRun(pts, far, n, tol, keep);
    for (int i = 0; i < n; ++i) if (keep[i]) idx << i;
    return idx;
}

// Potrace-style vertex adjustment: every polygon side becomes the least-squares line through the
// boundary points it replaces, and each vertex moves to where its two sides meet (within 1 px).
QPolygonF adjustVertices(const QPolygonF &dense, const QVector<int> &idx) {
    const int n = dense.size(), m = idx.size();
    QPolygonF P;
    for (int i : idx) P << dense[i];
    if (m < 3) return P;
    struct Line { QPointF c, d; bool ok; };
    QVector<Line> L(m);
    for (int k = 0; k < m; ++k) {
        const int a = idx[k], b = k + 1 < m ? idx[k + 1] : idx[0] + n;
        QVector<QPointF> q;
        for (int i = a + 1; i < b; ++i) q << dense[i % n];
        Line l{(P[k] + P[(k + 1) % m]) / 2, P[(k + 1) % m] - P[k], true};
        if (q.size() >= 2) {
            QPointF c(0, 0);
            for (const QPointF &p : q) c += p;
            c /= q.size();
            double sxx = 0, sxy = 0, syy = 0;
            for (const QPointF &p : q) { const QPointF d = p - c; sxx += d.x() * d.x(); sxy += d.x() * d.y(); syy += d.y() * d.y(); }
            const double th = 0.5 * std::atan2(2 * sxy, sxx - syy);
            QPointF d(std::cos(th), std::sin(th));
            if (dot(d, l.d) < 0) d = -d;
            l.c = c; l.d = d;
        }
        if (len(l.d) < 1e-9) l.ok = false; else l.d /= len(l.d);
        L[k] = l;
    }
    QPolygonF out = P;
    for (int j = 0; j < m; ++j) {
        const Line &a = L[(j + m - 1) % m], &b = L[j];
        if (!a.ok || !b.ok) continue;
        const double den = cross(a.d, b.d);
        if (std::abs(den) < 0.05) continue;                      // nearly parallel sides
        const double t = cross(b.c - a.c, b.d) / den;
        const QPointF x = a.c + a.d * t;
        if (len(x - P[j]) <= 1.0) out[j] = x;
    }
    return out;
}

QPolygonF dpOpen(const QPolygonF &pts, double tol) {
    const int n = pts.size();
    if (n <= 2) return pts;
    QVector<bool> keep(n, false);
    keep[0] = keep[n - 1] = true;
    dpRun(pts, 0, n - 1, tol, keep);
    QPolygonF out;
    for (int i = 0; i < n; ++i) if (keep[i]) out << pts[i];
    return out;
}

// Square corners: a short chamfer between two long, sharply turning sides becomes the corner.
QPolygonF fixCorners(QPolygonF P, QVector<int> &id) {
    for (int pass = 0; pass < 2; ++pass) {
        const int m = P.size();
        if (m < 5) return P;
        QVector<bool> drop(m, false);
        QPolygonF out = P;
        for (int j = 0; j < m; ++j) {
            const int j1 = (j + 1) % m, jp = (j + m - 1) % m, jn = (j + 2) % m;
            if (drop[j] || drop[j1] || drop[jp] || drop[jn]) continue;
            const QPointF a = P[jp], b = P[j], c = P[j1], d = P[jn];
            if (len(c - b) > 1.6 || len(b - a) < 2.5 || len(d - c) < 2.5) continue;
            const QPointF u = (b - a) / len(b - a), v = (d - c) / len(d - c);
            if (dot(u, v) > 0.5) continue;              // turns by less than 60 degrees
            QPointF x;
            if (QLineF(a, b).intersects(QLineF(c, d), &x) == QLineF::NoIntersection) continue;
            if (len(x - b) > 1.6 || len(x - c) > 1.6) continue;
            out[j] = x;
            drop[j1] = true;
        }
        QPolygonF n2;
        QVector<int> i2;
        for (int j = 0; j < m; ++j) if (!drop[j]) { n2 << out[j]; i2 << id[j]; }
        if (n2.size() == m) return P;
        P = n2;
        id = i2;
    }
    return P;
}

struct Seg { bool corner; QPointF a, v, c1, c2, e; };

double alphaAt(const QPointF &prev, const QPointF &cur, const QPointF &next) {
    const QPointF d = next - prev;
    const double denom = std::abs(d.x()) + std::abs(d.y());      // Potrace: L-infinity orthogonal
    if (denom < 1e-12) return 4.0 / 3.0;
    const double dd = std::abs(cross(cur - prev, next - prev)) / denom;
    const double a = dd > 1 ? 1 - 1.0 / dd : 0;
    return a / 0.75;
}

Seg makeSeg(const QPointF &prev, const QPointF &cur, const QPointF &next, double alphaMax, const QPointF &a, const QPointF &e) {
    Seg s;
    s.a = a; s.e = e; s.v = cur;
    double al = alphaAt(prev, cur, next);
    if (al >= alphaMax) { s.corner = true; s.c1 = a; s.c2 = e; return s; }
    s.corner = false;
    al = qBound(0.55, al, 1.0);
    s.c1 = a + (cur - a) * al;
    s.c2 = e + (cur - e) * al;
    return s;
}

// Schneider-style least-squares cubic with fixed end tangents through the samples.
bool fitCubic(const QPointF &S, const QPointF &t0, const QPointF &E, const QPointF &t1, const QVector<QPointF> &pts,
              const QVector<double> &u, double tol, QPointF *c1, QPointF *c2) {
    double C00 = 0, C01 = 0, C11 = 0, X0 = 0, X1 = 0;
    for (int i = 0; i < pts.size(); ++i) {
        const double t = u[i], m = 1 - t;
        const double b0 = m * m * m, b1 = 3 * m * m * t, b2 = 3 * m * t * t, b3 = t * t * t;
        const QPointF A1 = t0 * b1, A2 = t1 * b2;
        C00 += dot(A1, A1); C01 += dot(A1, A2); C11 += dot(A2, A2);
        const QPointF tmp = pts[i] - (S * (b0 + b1) + E * (b2 + b3));
        X0 += dot(A1, tmp); X1 += dot(A2, tmp);
    }
    const double det = C00 * C11 - C01 * C01;
    if (std::abs(det) < 1e-12) return false;
    const double a1 = (X0 * C11 - X1 * C01) / det, a2 = (C00 * X1 - C01 * X0) / det;
    const double chord = len(E - S);
    if (!(a1 > 1e-3 * chord) || !(a2 > 1e-3 * chord) || a1 > 2 * chord + 2 || a2 > 2 * chord + 2) return false;
    *c1 = S + t0 * a1;
    *c2 = E + t1 * a2;
    QPolygonF curve;
    for (int k = 0; k <= 48; ++k) curve << bez(S, *c1, *c2, E, k / 48.0);
    for (const QPointF &p : pts) {
        double best = 1e18;
        for (int k = 0; k < 48; ++k) best = std::min(best, distToSeg(p, curve[k], curve[k + 1]));
        if (best > tol) return false;
    }
    return true;
}

QVector<Seg> optimizeSegs(const QVector<Seg> &in, double tol, bool closed) {
    const int n = in.size();
    if (n < 3) return in;
    // closed paths: start the scan at a corner if there is one, so runs do not wrap around
    int start = 0;
    if (closed) for (int i = 0; i < n; ++i) if (in[i].corner) { start = i; break; }
    QVector<Seg> s;
    for (int i = 0; i < n; ++i) s << in[(start + i) % n];
    auto sign = [](const Seg &g) {
        const double c = cross(g.v - g.a, g.e - g.v);
        if (std::abs(c) < 0.08 * len(g.v - g.a) * len(g.e - g.v)) return 0;  // (nearly) straight: fits either way
        return c > 0 ? 1 : -1;
    };
    QVector<Seg> out;
    int i = 0;
    while (i < n) {
        if (s[i].corner) { out << s[i]; ++i; continue; }
        int best = i;
        QPointF bc1 = s[i].c1, bc2 = s[i].c2;
        int sg = sign(s[i]);
        double turn = 0;
        for (int k = i + 1; k < n && !s[k].corner; ++k) {
            const int sk = sign(s[k]);
            if (sk && sg && sk != sg) break;
            if (!sg) sg = sk;
            const QPointF d0 = s[k - 1].e - s[k - 1].c2, d1 = s[k].c2 - s[k].c1;
            turn += std::abs(std::atan2(cross(s[k - 1].c1 - s[k - 1].a, s[k].e - s[k].c2), dot(s[k - 1].c1 - s[k - 1].a, s[k].e - s[k].c2)));
            Q_UNUSED(d0); Q_UNUSED(d1);
            if (turn > 2.9) break;  // ~ 166 degrees
            const QPointF S = s[i].a, E = s[k].e;
            QPointF t0 = s[i].c1 - S, t1 = s[k].c2 - E;
            if (len(t0) < 1e-9 || len(t1) < 1e-9) break;
            t0 /= len(t0); t1 /= len(t1);
            QVector<QPointF> pts;
            QVector<double> cum;
            double total = 0;
            QPointF last = S;
            for (int q = i; q <= k; ++q)
                for (int st = 1; st <= 4; ++st) {
                    const QPointF p = bez(s[q].a, s[q].c1, s[q].c2, s[q].e, st / 4.0);
                    total += len(p - last);
                    last = p;
                    pts << p; cum << total;
                }
            if (total < 1e-9) break;
            QVector<double> u;
            for (double c : cum) u << c / total;
            QPointF c1, c2;
            if (!fitCubic(S, t0, E, t1, pts, u, tol, &c1, &c2)) break;
            best = k; bc1 = c1; bc2 = c2;
        }
        Seg m = s[i];
        m.e = s[best].e; m.c1 = bc1; m.c2 = bc2;
        if (best > i) m.v = (bc1 + bc2) / 2;
        out << m;
        i = best + 1;
    }
    return out;
}

void emitSegs(QPainterPath &path, QVector<Seg> segs, bool closed) {
    const int n = segs.size();
    if (!n) return;
    if (closed) {
        int st = -1;
        for (int i = 0; i < n; ++i) if (!segs[i].corner) { st = i; break; }
        if (st < 0) {  // a polygon
            path.moveTo(segs[0].v);
            for (int i = 1; i < n; ++i) path.lineTo(segs[i].v);
            path.closeSubpath();
            return;
        }
        std::rotate(segs.begin(), segs.begin() + st, segs.end());
    }
    path.moveTo(segs[0].a);
    for (int i = 0; i < n; ++i) {
        const Seg &s = segs[i];
        if (s.corner) {
            path.lineTo(s.v);
            const bool nextCorner = closed ? segs[(i + 1) % n].corner : (i + 1 < n && segs[i + 1].corner);
            if (!nextCorner) path.lineTo(s.e);
        } else path.cubicTo(s.c1, s.c2, s.e);
    }
    if (closed) path.closeSubpath();
}

// dense closed outline (points on the boundary) -> smooth closed subpath
void closedCurve(QPainterPath &path, const QPolygonF &dense, double dpTol, double alphaMax, bool opt, double tol, bool cornerFix,
                 double slack = 0) {
    const int n = dense.size();
    QVector<int> id = dpClosedIdx(dense, dpTol);
    QPolygonF P = adjustVertices(dense, id);
    if (cornerFix) P = fixCorners(P, id);
    // drop vertices that are (nearly) on the chord of their neighbours
    for (int guard = 0; guard < 2 && P.size() > 3; ++guard) {
        QPolygonF q;
        QVector<int> qi;
        const int m = P.size();
        for (int j = 0; j < m; ++j) {
            const QPointF &a = P[(j + m - 1) % m], &b = P[j], &c = P[(j + 1) % m];
            if (distToLine(b, a, c) < 0.15 && dot(b - a, c - b) > 0 && q.size() + (m - j) > 3) continue;
            q << b;
            qi << id[j];
        }
        if (q.size() == P.size()) break;
        P = q;
        id = qi;
    }
    const int m = P.size();
    if (m < 3) { if (m) { path.moveTo(P[0]); for (int j = 1; j < m; ++j) path.lineTo(P[j]); path.closeSubpath(); } return; }
    auto segAt = [&](int j) {
        const QPointF &prev = P[(j + m - 1) % m], &cur = P[j], &next = P[(j + 1) % m];
        return makeSeg(prev, cur, next, alphaMax, (prev + cur) / 2, (cur + next) / 2);
    };
    // Fit: a curve through the side midpoints lies inside a convex outline (and outside a concave
    // one) by part of the sagitta. Move each curved vertex along its normal by the mean offset of
    // the boundary points it covers; a few damped passes.
    auto midIdx = [n](int a, int b) { if (b < a) b += n; return (a + b) / 2; };
    for (int it = 0; it < 3 && n >= 8; ++it) {
        QVector<QPointF> move(m, QPointF(0, 0));
        for (int j = 0; j < m; ++j) {
            const Seg sg = segAt(j);
            if (sg.corner) continue;
            const QPointF mid = (sg.a + sg.e) / 2;
            QPointF nv = sg.v - mid;
            if (len(nv) < 0.05) continue;
            nv /= len(nv);
            QPolygonF curve;
            for (int k = 0; k <= 16; ++k) curve << bez(sg.a, sg.c1, sg.c2, sg.e, k / 16.0);
            const int s0 = midIdx(id[(j + m - 1) % m], id[j]);
            int s1 = midIdx(id[j], id[(j + 1) % m]);
            if (s1 < s0) s1 += n;
            double acc = 0;
            int cnt = 0;
            for (int k = s0; k <= s1 && k < s0 + n; ++k) {
                const QPointF p = dense[k % n];
                double best = 1e18;
                QPointF bq;
                for (int q = 0; q < 16; ++q) {
                    const QPointF A = curve[q], B = curve[q + 1], d = B - A;
                    const double l2 = dot(d, d);
                    const double t = l2 > 1e-12 ? qBound(0.0, dot(p - A, d) / l2, 1.0) : 0;
                    const QPointF x = A + d * t;
                    const double dd = len(p - x);
                    if (dd < best) { best = dd; bq = x; }
                }
                acc += dot(p - bq, nv);
                ++cnt;
            }
            if (cnt) move[j] = nv * qBound(-1.0, 0.7 * acc / cnt, 1.0);
        }
        for (int j = 0; j < m; ++j) P[j] += move[j];
    }
    QVector<Seg> segs;
    for (int j = 0; j < m; ++j) segs << segAt(j);
    if (opt) segs = optimizeSegs(segs, tol + slack, true);
    emitSegs(path, segs, true);
}

void openCurve(QPainterPath &path, const QPolygonF &dense, double dpTol, double alphaMax, bool opt, double tol) {
    const QPolygonF P = dpOpen(dense, dpTol);
    const int m = P.size();
    if (m < 2) return;
    if (m == 2) { path.moveTo(P[0]); path.lineTo(P[1]); return; }
    QVector<Seg> segs;
    // first and last halves are straight
    Seg first; first.corner = true; first.a = P[0]; first.v = P[0]; first.e = (P[0] + P[1]) / 2; first.c1 = first.a; first.c2 = first.e;
    segs << first;
    for (int j = 1; j < m - 1; ++j) segs << makeSeg(P[j - 1], P[j], P[j + 1], alphaMax, (P[j - 1] + P[j]) / 2, (P[j] + P[j + 1]) / 2);
    Seg last; last.corner = true; last.a = (P[m - 2] + P[m - 1]) / 2; last.v = last.a; last.e = P[m - 1]; last.c1 = last.a; last.c2 = last.e;
    segs << last;
    if (opt) segs = optimizeSegs(segs, tol, false);
    emitSegs(path, segs, false);
}

QVector<QPolygon> filtered(const QVector<QPolygon> &loops, int turd) {
    QVector<QPolygon> r;
    for (const QPolygon &l : loops) if (std::abs(loopArea(l)) > turd) r << l;
    return r;
}

inline int lum(QRgb c) { return (qRed(c) * 299 + qGreen(c) * 587 + qBlue(c) * 114) / 1000; }

QImage prepared(const QImage &src, bool ignoreTransparent) {
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    if (!ignoreTransparent) {
        QImage w(img.size(), QImage::Format_ARGB32);
        w.fill(Qt::white);
        for (int y = 0; y < img.height(); ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            QRgb *d = reinterpret_cast<QRgb *>(w.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const int a = qAlpha(s[x]);
                d[x] = qRgb((qRed(s[x]) * a + 255 * (255 - a)) / 255, (qGreen(s[x]) * a + 255 * (255 - a)) / 255, (qBlue(s[x]) * a + 255 * (255 - a)) / 255);
            }
        }
        img = w;
    }
    return img;
}

// boundaries of the pixels with label in `want` inside rect r (labels: -1 = nothing)
QVector<QPolygon> regionLoops(const QVector<int> &lab, int w, const QRect &r, const std::function<bool(int)> &want) {
    QVector<uchar> m(r.width() * r.height(), 0);
    for (int y = 0; y < r.height(); ++y) {
        const int *row = lab.constData() + (r.top() + y) * w + r.left();
        uchar *d = m.data() + y * r.width();
        for (int x = 0; x < r.width(); ++x) d[x] = want(row[x]) ? 1 : 0;
    }
    QVector<QPolygon> loops = boundaries(m, r.width(), r.height());
    for (QPolygon &l : loops) l.translate(r.topLeft());
    return loops;
}

QVector<int> majority(const QVector<int> &lab, int w, int h) {
    QVector<int> out = lab;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int c = lab[y * w + x];
            if (c < 0) continue;
            int cand[9], cnt[9], k = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    const int v = lab[yy * w + xx];
                    if (v < 0) continue;
                    int i = 0;
                    while (i < k && cand[i] != v) ++i;
                    if (i == k) { cand[k] = v; cnt[k] = 0; ++k; }
                    ++cnt[i];
                }
            int best = c, bc = 0;
            for (int i = 0; i < k; ++i) if (cnt[i] > bc || (cnt[i] == bc && cand[i] == c)) { bc = cnt[i]; best = cand[i]; }
            if (bc >= 5) out[y * w + x] = best;   // a clear majority of the 3x3 window
        }
    return out;
}

int borderMajority(const QVector<int> &lab, int w, int h) {
    QHash<int, int> n;
    auto add = [&](int x, int y) { const int v = lab[y * w + x]; if (v >= 0) ++n[v]; };
    for (int x = 0; x < w; ++x) { add(x, 0); add(x, h - 1); }
    for (int y = 0; y < h; ++y) { add(0, y); add(w - 1, y); }
    int best = -1, bc = 0;
    for (auto it = n.cbegin(); it != n.cend(); ++it) if (it.value() > bc || (it.value() == bc && it.key() < best)) { bc = it.value(); best = it.key(); }
    return best;
}

Result traceLabels(const QVector<int> &lab, int w, int h, const QVector<QColor> &colors, const Options &o, bool exact,
                   const std::atomic_bool *cancel) {
    Result res;
    const int nc = colors.size();
    QVector<int> count(nc, 0);
    QVector<QRect> box(nc);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int v = lab[y * w + x];
            if (v < 0 || v >= nc) continue;
            ++count[v];
            box[v] |= QRect(x, y, 1, 1);
        }
    const int bg = o.removeBackground ? borderMajority(lab, w, h) : -1;
    QVector<int> order;
    for (int i = 0; i < nc; ++i) if (count[i] && i != bg) order << i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return count[a] > count[b]; });
    const bool stacked = o.stacked && !exact;
    QVector<int> rank(nc, -1);
    for (int k = 0; k < order.size(); ++k) rank[order[k]] = k;
    // every colour layer is independent (P11): trace them on the thread pool, keep the order
    struct Layer { Shape s; int nodes = 0; bool ok = false; };
    QVector<Layer> layers(order.size());
    QVector<int> ks(order.size());
    std::iota(ks.begin(), ks.end(), 0);
    QtConcurrent::blockingMap(ks, [&](int k) {
        if (cancelled(cancel)) return;
        const int c = order[k];
        QVector<QPolygon> loops;
        if (stacked) {
            QRect r;
            for (int q = k; q < order.size(); ++q) r |= box[order[q]];
            loops = regionLoops(lab, w, r, [&](int v) { return v >= 0 && v < nc && rank[v] >= k; });
        } else loops = regionLoops(lab, w, box[c], [c](int v) { return v == c; });
        Layer &L = layers[k];
        L.s.color = colors[c];
        if (exact) L.s.path = polygonLoops(loops);
        else L.s.path = smoothLoops(filtered(loops, o.turdSize), o.alphaMax, o.optimize, o.optTolerance);
        if (L.s.path.isEmpty()) return;
        L.nodes = countNodes(L.s.path);
        L.ok = true;
    });
    if (cancelled(cancel)) { res.cancelled = true; return res; }
    for (const Layer &L : layers) {
        if (!L.ok) continue;
        res.nodes += L.nodes;
        res.shapes << L.s;
    }
    return res;
}

Result centerline(const QImage &img, const QVector<uchar> &fg, int w, int h, const Options &o, const std::atomic_bool *cancel) {
    Result res;
    const QVector<uchar> sk = thin(fg, w, h);
    if (cancelled(cancel)) { res.cancelled = true; return res; }
    long fgN = 0, skN = 0;
    double rs = 0, gs = 0, bs = 0;
    for (int i = 0; i < w * h; ++i) {
        if (fg[i]) {
            ++fgN;
            const QRgb c = img.pixel(i % w, i / w);
            rs += qRed(c); gs += qGreen(c); bs += qBlue(c);
        }
        skN += sk[i] != 0;
    }
    if (!skN) return res;
    const double width = qBound(1.0, double(fgN) / double(skN), 200.0);
    auto S = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && sk[y * w + x]; };
    auto deg = [&](int x, int y) {
        int d = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) if ((dx || dy) && S(x + dx, y + dy)) ++d;
        return d;
    };
    QVector<uchar> visited(w * h, 0);
    QSet<quint64> walked;
    auto ekey = [&](int a, int b) { return (quint64(qMin(a, b)) << 32) | quint64(qMax(a, b)); };
    struct Chain { QPolygonF pts; bool closed; int endDeg0, endDeg1; };
    QVector<Chain> chains;
    auto neighbours = [&](int p) {
        QVector<int> r;
        const int x = p % w, y = p / w;
        static const int ord[8][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
        for (const auto &d : ord) if (S(x + d[0], y + d[1])) r << (y + d[1]) * w + x + d[0];
        return r;
    };
    auto isNode = [&](int p) { return deg(p % w, p / w) != 2; };
    auto centre = [&](int p) { return QPointF(p % w + 0.5, p / w + 0.5); };
    for (int p = 0; p < w * h; ++p) {
        if (!sk[p] || !isNode(p)) continue;
        for (int q : neighbours(p)) {
            if (walked.contains(ekey(p, q))) continue;
            walked.insert(ekey(p, q));
            Chain ch;
            ch.closed = false;
            ch.pts << centre(p) << centre(q);
            int prev = p, cur = q;
            while (!isNode(cur) && !visited[cur]) {
                visited[cur] = 1;
                int next = -1;
                for (int n : neighbours(cur)) if (n != prev && !(isNode(n) && walked.contains(ekey(cur, n))) && !visited[n]) { next = n; break; }
                if (next < 0) for (int n : neighbours(cur)) if (n != prev && isNode(n)) { next = n; break; }
                if (next < 0) break;
                walked.insert(ekey(cur, next));
                prev = cur; cur = next;
                ch.pts << centre(cur);
            }
            ch.endDeg0 = deg(p % w, p / w);
            ch.endDeg1 = deg(cur % w, cur / w);
            chains << ch;
        }
    }
    for (int p = 0; p < w * h; ++p) {
        if (!sk[p] || visited[p] || isNode(p)) continue;
        Chain ch;
        ch.closed = true;
        int prev = -1, cur = p;
        while (cur >= 0 && !visited[cur]) {
            visited[cur] = 1;
            ch.pts << centre(cur);
            int next = -1;
            for (int n : neighbours(cur)) if (n != prev && !visited[n]) { next = n; break; }
            prev = cur; cur = next;
        }
        ch.endDeg0 = ch.endDeg1 = 2;
        chains << ch;
    }
    const double spur = std::max(double(o.turdSize), width * 1.5);
    QPainterPath path;
    QSet<QPair<QPair<double, double>, QPair<double, double>>> seenShort;
    for (const Chain &c : chains) {
        if (!c.closed && c.pts.size() <= 3) {  // junction-cluster bits can be found from both ends
            QPair<double, double> a{c.pts.first().x(), c.pts.first().y()}, b{c.pts.last().x(), c.pts.last().y()};
            if (b < a) std::swap(a, b);
            if (seenShort.contains({a, b})) continue;
            seenShort.insert({a, b});
        }
        double L = 0;
        for (int i = 1; i < c.pts.size(); ++i) L += len(c.pts[i] - c.pts[i - 1]);
        const bool isSpur = !c.closed && ((c.endDeg0 == 1) != (c.endDeg1 == 1)) && L < spur;
        if (isSpur) continue;
        if (!c.closed && c.endDeg0 == 1 && c.endDeg1 == 1 && L < std::max(1.0, double(o.turdSize))) continue;
        if (c.closed) { if (c.pts.size() >= 4) closedCurve(path, c.pts, 0.8, o.alphaMax, o.optimize, o.optTolerance, false); }
        else openCurve(path, c.pts, 0.8, o.alphaMax, o.optimize, o.optTolerance);
    }
    if (path.isEmpty()) return res;
    Shape s;
    s.path = path;
    s.stroke = true;
    s.strokeWidth = width;
    s.color = fgN ? QColor(int(rs / fgN), int(gs / fgN), int(bs / fgN)) : QColor(Qt::black);
    res.nodes = countNodes(path);
    res.shapes << s;
    return res;
}
} // namespace

QVector<QPolygon> boundaries(const QVector<uchar> &m, int w, int h) {
    QVector<QPolygon> loops;
    if (w <= 0 || h <= 0 || m.size() < qint64(w) * h) return loops;
    const int W = w + 1;
    QVector<uchar> orig(W * (h + 1), 0);
    auto at = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && m[y * w + x]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!m[y * w + x]) continue;
            if (!at(x, y - 1)) orig[y * W + x] |= 1;            // right along the top
            if (!at(x + 1, y)) orig[y * W + x + 1] |= 2;        // down the right side
            if (!at(x, y + 1)) orig[(y + 1) * W + x + 1] |= 4;  // left along the bottom
            if (!at(x - 1, y)) orig[(y + 1) * W + x] |= 8;      // up the left side
        }
    QVector<uchar> left = orig;
    static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    for (int v = 0; v < left.size(); ++v) {
        while (left[v]) {
            int d = 0;
            while (!(left[v] & (1 << d))) ++d;
            QPolygon poly;
            int cur = v;
            for (;;) {
                if (!(left[cur] & (1 << d))) break;  // back on a used edge: the loop is closed
                left[cur] &= uchar(~(1 << d));
                poly << QPoint(cur % W, cur / W);
                cur += dx[d] + dy[d] * W;
                const uchar o = orig[cur];
                const int r = (d + 1) & 3, l = (d + 3) & 3;
                if ((o & (1 << r)) && (o & (1 << l))) d = r;   // saddle: keep the pixel on the right (4-connected)
                else if (o & (1 << d)) {}
                else if (o & (1 << r)) d = r;
                else if (o & (1 << l)) d = l;
                else break;
            }
            if (poly.size() >= 4) loops << poly;
        }
    }
    return loops;
}

double loopArea(const QPolygon &l) {
    qint64 a = 0;
    const int n = l.size();
    for (int i = 0; i < n; ++i) {
        const QPoint &p = l[i], &q = l[(i + 1) % n];
        a += qint64(p.x()) * q.y() - qint64(q.x()) * p.y();
    }
    return a / 2.0;
}

QPolygon cornersOnly(const QPolygon &l) {
    const int n = l.size();
    if (n < 4) return l;
    QPolygon r;
    for (int i = 0; i < n; ++i) {
        const QPoint a = l[(i + n - 1) % n], b = l[i], c = l[(i + 1) % n];
        if ((b - a) == (c - b) || (b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x()) == 0) {
            // straight on (same unit direction)
            if (QPoint::dotProduct(b - a, c - b) > 0) continue;
        }
        r << b;
    }
    return r;
}

QPainterPath polygonLoops(const QVector<QPolygon> &loops) {
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    for (const QPolygon &l : loops) {
        const QPolygon c = cornersOnly(l);
        if (c.size() < 3) continue;
        p.moveTo(c[0]);
        for (int i = 1; i < c.size(); ++i) p.lineTo(c[i]);
        p.closeSubpath();
    }
    return p;
}

// Polygon tolerance on the boundary midpoints (px) and the extra merge tolerance that absorbs the
// staircase wiggle of the unmerged segments; tuned on the IoU tests in v031_test.
static constexpr double kDpTol = 0.8;
static constexpr double kSlack = 0.15;
QPainterPath smoothLoops(const QVector<QPolygon> &loops, double alphaMax, bool optimize, double tol) {
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    for (const QPolygon &l : loops) {
        const int n = l.size();
        if (n < 4) continue;
        QPolygonF mid;
        mid.reserve(n);
        for (int i = 0; i < n; ++i) mid << (QPointF(l[i]) + QPointF(l[(i + 1) % n])) / 2;
        closedCurve(p, mid, kDpTol, alphaMax, optimize, std::max(0.05, tol), true, kSlack);
    }
    return p;
}

QVector<uchar> thin(const QVector<uchar> &mask, int w, int h) {
    QVector<uchar> m(w * h, 0);
    if (w <= 0 || h <= 0 || mask.size() < w * h) return m;
    for (int i = 0; i < w * h; ++i) m[i] = mask[i] ? 1 : 0;
    auto P = [&](int x, int y) -> int { return x >= 0 && y >= 0 && x < w && y < h ? m[y * w + x] : 0; };
    bool changed = true;
    QVector<int> del;
    while (changed) {
        changed = false;
        for (int step = 0; step < 2; ++step) {
            del.clear();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!m[y * w + x]) continue;
                    const int p2 = P(x, y - 1), p3 = P(x + 1, y - 1), p4 = P(x + 1, y), p5 = P(x + 1, y + 1), p6 = P(x, y + 1),
                              p7 = P(x - 1, y + 1), p8 = P(x - 1, y), p9 = P(x - 1, y - 1);
                    const int B = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
                    if (B < 2 || B > 6) continue;
                    const int A = (!p2 && p3) + (!p3 && p4) + (!p4 && p5) + (!p5 && p6) + (!p6 && p7) + (!p7 && p8) + (!p8 && p9) + (!p9 && p2);
                    if (A != 1) continue;
                    if (step == 0 ? ((p2 && p4 && p6) || (p4 && p6 && p8)) : ((p2 && p4 && p8) || (p2 && p6 && p8))) continue;
                    del << y * w + x;
                }
            for (int i : del) m[i] = 0;
            changed |= !del.isEmpty();
        }
    }
    return m;
}

int countNodes(const QPainterPath &p) {
    int n = 0;
    for (int i = 0; i < p.elementCount(); ++i) {
        const auto e = p.elementAt(i);
        if (e.isMoveTo() || e.isLineTo()) ++n;
        else if (e.isCurveTo()) { ++n; i += 2; }
    }
    return n;
}

Result trace(const QImage &src, const Options &oIn, const std::atomic_bool *cancel) {
    Result res;
    if (src.isNull() || src.width() <= 0 || src.height() <= 0) return res;
    Options o = oIn;
    o.turdSize = qBound(0, o.turdSize, 100000);
    o.alphaMax = std::isfinite(o.alphaMax) ? qBound(0.0, o.alphaMax, 1.3334) : 1.0;
    o.optTolerance = std::isfinite(o.optTolerance) ? qBound(0.01, o.optTolerance, 5.0) : 0.2;
    o.threshold = qBound(0, o.threshold, 256);
    o.colors = qBound(2, o.colors, 256);
    o.smoothing = qBound(0, o.smoothing, 10);
    const QImage img = prepared(src, o.ignoreTransparent);
    const int w = img.width(), h = img.height();
    auto transparent = [&](QRgb c) { return o.ignoreTransparent && qAlpha(c) < 128; };

    if (o.mode == Mode::Bw || o.mode == Mode::Centerline) {
        QVector<uchar> fg(w * h, 0);
        for (int y = 0; y < h; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                if (transparent(s[x])) continue;
                const bool dark = lum(s[x]) < o.threshold;
                fg[y * w + x] = (dark != o.invert) ? 1 : 0;
            }
        }
        if (o.mode == Mode::Centerline) return centerline(img, fg, w, h, o, cancel);
        Shape s;
        s.color = Qt::black;
        s.path = smoothLoops(filtered(boundaries(fg, w, h), o.turdSize), o.alphaMax, o.optimize, o.optTolerance);
        if (cancelled(cancel)) { res.cancelled = true; return res; }
        if (!s.path.isEmpty()) { res.nodes = countNodes(s.path); res.shapes << s; }
        return res;
    }

    QVector<int> lab(w * h, -1);
    QVector<QColor> colors;
    if (o.mode == Mode::PixelArt) {
        QHash<QRgb, int> idx;
        for (int y = 0; y < h; ++y) {
            const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                if (qAlpha(s[x]) == 0 || transparent(s[x])) continue;
                auto it = idx.find(s[x]);
                if (it == idx.end()) { it = idx.insert(s[x], colors.size()); colors << QColor::fromRgba(s[x]); }
                lab[y * w + x] = it.value();
            }
            if (cancelled(cancel)) { res.cancelled = true; return res; }
        }
        return traceLabels(lab, w, h, colors, o, true, cancel);
    }
    // colours
    QVector<QRgb> pal = o.palette;
    for (QRgb &c : pal) c |= 0xff000000u;
    if (pal.isEmpty()) pal = quant::buildPalette({img}, o.colors, quant::Adaptive);
    if (pal.isEmpty()) return res;
    const QVector<uchar> map = quant::mapImage(img, pal, 0, 0, -1, 0);
    for (int y = 0; y < h; ++y) {
        const QRgb *s = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x)
            if (!transparent(s[x]) && y * w + x < map.size()) lab[y * w + x] = qMin(int(map[y * w + x]), int(pal.size()) - 1);
    }
    for (int i = 0; i < o.smoothing; ++i) {
        lab = majority(lab, w, h);
        if (cancelled(cancel)) { res.cancelled = true; return res; }
    }
    for (QRgb c : pal) colors << QColor::fromRgb(c);
    return traceLabels(lab, w, h, colors, o, false, cancel);
}

QTransform imageToCanvas(const VObject &b) {
    const QRectF r = b.rect.normalized();
    const QSize is = b.image.size();
    if (is.isEmpty()) return b.xform;
    const QTransform t(r.width() / is.width(), 0, 0, r.height() / is.height(), r.x(), r.y());
    return t * b.xform;
}

QVector<VObject> toObjects(const VObject &bitmap, const Result &r, bool hardEdges) {
    QVector<VObject> out;
    const QTransform t = imageToCanvas(bitmap);
    // stroke widths scale with the mean linear scale of the mapping
    const double lin = std::sqrt(std::abs(t.determinant()));
    int n = 0;
    for (const Shape &sh : r.shapes) {
        const QPainterPath p = t.map(sh.path);
        if (p.isEmpty()) continue;
        VObject o;
        paths::fromPainterPath(o, p);
        if (o.points.size() < 2) continue;
        o.name = QStringLiteral("Trace %1 %2").arg(++n).arg(sh.color.name(sh.color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb));
        o.antialias = !hardEdges;
        if (sh.stroke) {
            o.closed = false;
            o.fillOn = false;
            o.strokeOn = true;
            o.stroke = sh.color;
            o.strokeWidth = qMax(0.1, sh.strokeWidth * lin);
        } else {
            o.fillOn = true;
            o.fill = sh.color;
            o.strokeOn = false;
            o.strokeWidth = 0;
        }
        out << o;
    }
    return out;
}

QVector<quint64> insert(Document &d, quint64 bitmapId, const Result &r, const Insert &how) {
    QVector<quint64> ids;
    int layer = -1, index = -1;
    VObject *b = d.find(bitmapId, &layer, &index);
    if (!b || b->type != ObjType::Bitmap || r.cancelled) return ids;
    QVector<VObject> objs = toObjects(*b, r, how.hardEdges);
    if (objs.isEmpty()) return ids;
    const QString baseName = b->name.isEmpty() ? QStringLiteral("Bitmap") : b->name;
    for (VObject &o : objs) { o.id = d.newId(); ids << o.id; }
    if (how.asGroup) {   // v0.3.2: the traced shapes as one group (on the new layer too, when both are set)
        VObject g;
        g.type = ObjType::Group;
        g.id = d.newId();
        g.name = QStringLiteral("Trace of %1").arg(baseName);
        g.children = objs;
        objs = {g};
        ids = {g.id};
    }
    if (how.group) {
        d.curLayer = layer;
        const int nl = d.addLayer(QStringLiteral("Trace of %1").arg(baseName));
        if (nl <= layer) ++layer;   // the bitmap's layer moved up
        for (const VObject &o : objs) d.objects(nl).append(o);
    } else {
        if (QVector<VObject> *v = d.containerOf(bitmapId, nullptr, &index))
            for (int i = 0; i < objs.size(); ++i) v->insert(index + 1 + i, objs[i]);
    }
    if (!how.keepOriginal) {
        int i2 = -1;
        if (QVector<VObject> *v = d.containerOf(bitmapId, nullptr, &i2)) v->removeAt(i2);
    }
    d.setSelection(QSet<quint64>(ids.begin(), ids.end()));
    d.notifyStructure();
    return ids;
}
}
