#include "AutoShape.h"
#include "Object.h"
#include <QtMath>
#include <cmath>

// qBound that tolerates hi < lo (tiny or degenerate rectangles, e.g. a callout narrower than its corner
// radii): the lower bound wins instead of tripping Qt's debug assertion / returning an out-of-range value.
template <class T> static inline T sb(T lo, T v, T hi) { if (hi < lo) hi = lo; return qBound(lo, v, hi); }

namespace autoshape {
namespace {
struct KindInfo { const char *key; const char *label; const char *tip; bool line; };
const KindInfo kKinds[] = {
    {"roundrect", "Rounded Rectangle", "Rectangle with a separate radius per corner (Alt+drag a diamond: one corner)", false},
    {"chamfer", "Chamfer Rectangle", "Rectangle with concave (scooped) corners", false},
    {"bevel", "Bevel Rectangle", "Rectangle with cut (bevelled) corners", false},
    {"polygon", "Polygon", "Regular polygon with N sides and optional rounded corners", false},
    {"star", "Star", "Star with N points, inner radius and roundness", false},
    {"smartpolygon", "Smart Polygon", "Polygon whose edges bend in or out; drag the second vertex to change the sides", false},
    {"arc", "Arc / Pie", "Ellipse arc, pie or donut segment", false},
    {"doughnut", "Doughnut", "Ring with an adjustable hole", false},
    {"spiral", "Spiral", "Archimedean spiral (open stroke)", true},
    {"arrow", "Arrow", "Block arrow with head and tail styles", false},
    {"arrowline", "Line with Arrowheads", "Line with a head style at each end", true},
    {"connector", "Connector", "Straight, elbow or curved connector line with arrowheads", true},
    {"gear", "Cog / Gear", "Gear with N teeth and a centre hole", false},
    {"frame", "Frame", "Rectangular frame with an adjustable border", false},
    {"lshape", "L-Shape", "L-shaped polygon with adjustable arm widths", false},
    {"speech", "Speech Bubble", "Rounded rectangle with a draggable tail", false},
    {"heart", "Heart", "Heart with adjustable cleft depth and point sharpness", false},
};

const QStringList kHeads = {QStringLiteral("None"), QStringLiteral("Open"), QStringLiteral("Filled"), QStringLiteral("Circle"),
                            QStringLiteral("Square"), QStringLiteral("Bar"), QStringLiteral("Diamond")};

Param P(const char *k, const char *l, double mn, double mx, double def, int dec = 0, const char *suffix = "") {
    Param p; p.key = QString::fromLatin1(k); p.label = QString::fromLatin1(l); p.min = mn; p.max = mx; p.def = def; p.decimals = dec;
    p.suffix = QString::fromUtf8(suffix);
    return p;
}
Param C(const char *k, const char *l, const QStringList &choices, int def) {
    Param p; p.key = QString::fromLatin1(k); p.label = QString::fromLatin1(l); p.min = 0; p.max = choices.size() - 1; p.def = def; p.choices = choices;
    return p;
}

const KindInfo *info(const QString &k) {
    for (const auto &i : kKinds) if (k == QLatin1String(i.key)) return &i;
    return nullptr;
}

QPointF fromUnit(const QRectF &r, double ux, double uy) { return QPointF(r.center().x() + ux * r.width() / 2, r.center().y() + uy * r.height() / 2); }
QPointF toUnit(const QRectF &r, const QPointF &p) {
    const double rx = r.width() / 2, ry = r.height() / 2;
    return QPointF(rx > 1e-9 ? (p.x() - r.center().x()) / rx : 0, ry > 1e-9 ? (p.y() - r.center().y()) / ry : 0);
}
QPointF polar(const QRectF &r, double deg, double radius) {
    const double a = qDegreesToRadians(deg);
    return fromUnit(r, radius * std::cos(a), radius * std::sin(a));
}
double angleOf(const QRectF &r, const QPointF &p) { const QPointF u = toUnit(r, p); return qRadiansToDegrees(std::atan2(u.y(), u.x())); }
double radiusOf(const QRectF &r, const QPointF &p) { const QPointF u = toUnit(r, p); return std::hypot(u.x(), u.y()); }
double norm360(double a) { a = std::fmod(a, 360.0); if (a < 0) a += 360; return a; }

// Classic heart as four cubics (two per side, mirrored). This follows the
// usual 2-cubic bezier heart — from the cleft at (0.5, 0.3):
//   C (0.5, 0.1) (0.1, 0.0) (0.05, 0.3), then (0.0, 0.55) (0.3, 0.75) (0.5, 1.0)
// — with the mid-join made G1 (Lout at (0, 0.60) instead of (0, 0.55)).
// Tangents are vertical at the cleft; the tip is a point (C0). There is no
// on-path knot at the lobe apex, so the tops stay round and sit above the
// cleft. `cleft` deepens and sharpens the dip; `point` tightens the tip.
struct HeartGeom {
    QPointF C, Cout, L, Lin, Lout, T, Tin;
};
HeartGeom heartGeom(double cleft, double point) {
    cleft = sb(0.0, cleft, 1.0);
    point = sb(0.0, point, 1.0);
    HeartGeom g;
    double cy, cLen;
    if (cleft <= 0.35) {
        const double u = cleft / 0.35;
        cy = 0.14 + (0.30 - 0.14) * u;     // default 0.35 -> 0.30
        cLen = 0.07 + (0.20 - 0.07) * u;   // default -> 0.20  (Cout at y=0.10)
    } else {
        const double u = (cleft - 0.35) / 0.65;
        cy = 0.30 + (0.52 - 0.30) * u;
        cLen = 0.20 + (0.50 - 0.20) * u;
    }
    g.L = QPointF(0.05, 0.30);
    g.Lin = QPointF(0.10, 0.00);
    double k;
    if (point <= 0.85) {
        const double u = point / 0.85;
        k = 1.15 + (1.00 - 1.15) * u;      // default 0.85 -> 1 (Lout at (0, 0.60))
        g.Tin = QPointF(0.10 + (0.30 - 0.10) * u, 0.68 + (0.75 - 0.68) * u);
    } else {
        const double u = (point - 0.85) / 0.15;
        k = 1.00 + (0.90 - 1.00) * u;
        g.Tin = QPointF(0.30 + (0.46 - 0.30) * u, 0.75 + (0.95 - 0.75) * u);
    }
    const QPointF d = g.L - g.Lin;
    g.Lout = g.L + d * k;
    g.C = QPointF(0.5, cy);
    g.Cout = QPointF(0.5, cy - cLen);
    g.T = QPointF(0.5, 1.0);
    return g;
}
QPointF heartMap(const QRectF &r, const QPointF &u) {
    return QPointF(r.left() + u.x() * r.width(), r.top() + u.y() * r.height());
}
QPointF heartMirror(const QPointF &u) { return QPointF(1.0 - u.x(), u.y()); }

QPainterPath heartPath(const QRectF &r, double cleft, double point) {
    const HeartGeom g = heartGeom(cleft, point);
    auto M = [&](const QPointF &u) { return heartMap(r, u); };
    QPainterPath path;
    path.moveTo(M(g.C));
    path.cubicTo(M(g.Cout), M(g.Lin), M(g.L));
    path.cubicTo(M(g.Lout), M(g.Tin), M(g.T));
    path.cubicTo(M(heartMirror(g.Tin)), M(heartMirror(g.Lout)), M(heartMirror(g.L)));
    path.cubicTo(M(heartMirror(g.Lin)), M(heartMirror(g.Cout)), M(g.C));
    path.closeSubpath();
    return path;
}

QPointF heartCleftPos(const QRectF &r, double cleft) {
    return heartMap(r, heartGeom(cleft, 0.85).C);
}
QPointF heartPointPos(const QRectF &r, double point) {
    // mid-point of the lower-right cubic (side join -> tip), so the handle sits on the outline
    const HeartGeom g = heartGeom(0.35, point);
    const QPointF a = heartMirror(g.L), c1 = heartMirror(g.Lout), c2 = heartMirror(g.Tin), b = g.T;
    const double t = 0.45;
    const double u = 1.0 - t;
    const QPointF mid = a * (u * u * u) + c1 * (3 * u * u * t) + c2 * (3 * u * t * t) + b * (t * t * t);
    return heartMap(r, mid);
}

// closed polygon with each corner rounded: `round` 0..1 cuts that fraction of half of each edge
QPainterPath roundedPolygon(const QPolygonF &pts, double round) {
    QPainterPath path;
    const int n = pts.size();
    if (n < 3) return path;
    if (round <= 1e-6) { path.addPolygon(pts); path.closeSubpath(); return path; }
    round = sb(0.0, round, 1.0) * 0.5;
    auto lerp = [](const QPointF &a, const QPointF &b, double t) { return a + (b - a) * t; };
    for (int i = 0; i < n; ++i) {
        const QPointF &prev = pts[(i + n - 1) % n], &cur = pts[i], &next = pts[(i + 1) % n];
        const QPointF a = lerp(cur, prev, round), b = lerp(cur, next, round);
        if (i == 0) path.moveTo(a); else path.lineTo(a);
        path.quadTo(cur, b);
    }
    path.closeSubpath();
    return path;
}

// arrow head at `tip` pointing along `dir` (unit). Solid kinds go to `solid`, line kinds to `open`.
// Returns how far the line should stop short of the tip.
double addHead(int style, const QPointF &tip, QPointF dir, double size, QPainterPath &solid, QPainterPath &open) {
    const double len = std::hypot(dir.x(), dir.y());
    if (len < 1e-9 || style <= 0) return 0;
    dir /= len;
    const QPointF nrm(-dir.y(), dir.x());
    const double s = qMax(1.0, size);
    switch (style) {
    case 1: { // open V
        open.moveTo(tip - dir * s + nrm * s * 0.5);
        open.lineTo(tip);
        open.lineTo(tip - dir * s - nrm * s * 0.5);
        return 0;
    }
    case 2: { // filled triangle
        QPolygonF t{tip, tip - dir * s + nrm * s * 0.5, tip - dir * s - nrm * s * 0.5};
        solid.addPolygon(t); solid.closeSubpath();
        return s * 0.8;
    }
    case 3: { // circle centred on the end
        solid.addEllipse(tip, s * 0.4, s * 0.4);
        return 0;
    }
    case 4: { // square centred on the end
        QPolygonF q{tip + (dir + nrm) * s * 0.35, tip + (-dir + nrm) * s * 0.35, tip + (-dir - nrm) * s * 0.35, tip + (dir - nrm) * s * 0.35};
        solid.addPolygon(q); solid.closeSubpath();
        return 0;
    }
    case 5: { // bar across the end
        open.moveTo(tip + nrm * s * 0.5);
        open.lineTo(tip - nrm * s * 0.5);
        return 0;
    }
    case 6: { // diamond
        QPolygonF q{tip, tip - dir * s * 0.5 + nrm * s * 0.35, tip - dir * s, tip - dir * s * 0.5 - nrm * s * 0.35};
        solid.addPolygon(q); solid.closeSubpath();
        return s;
    }
    }
    return 0;
}

QPointF ept(const QMap<QString, double> &pm, const QRectF &r, int which) {
    const double fx = pm.value(which ? QStringLiteral("x2") : QStringLiteral("x1")), fy = pm.value(which ? QStringLiteral("y2") : QStringLiteral("y1"));
    return QPointF(r.left() + fx * r.width(), r.top() + fy * r.height());
}
}

QStringList kinds() { QStringList l; for (const auto &k : kKinds) l << QString::fromLatin1(k.key); return l; }
QString label(const QString &kind) { const KindInfo *i = info(kind); return i ? QString::fromLatin1(i->label) : kind; }
QString tip(const QString &kind) { const KindInfo *i = info(kind); return i ? QString::fromLatin1(i->tip) : QString(); }
bool isKind(const QString &kind) { return info(kind) != nullptr; }

QVector<Param> params(const QString &k) {
    const QStringList heads = kHeads;
    if (k == QLatin1String("roundrect"))
        return {P("rTL", "Top left radius", 0, 10000, 12, 0, " px"), P("rTR", "Top right radius", 0, 10000, 12, 0, " px"),
                P("rBR", "Bottom right radius", 0, 10000, 12, 0, " px"), P("rBL", "Bottom left radius", 0, 10000, 12, 0, " px")};
    if (k == QLatin1String("chamfer") || k == QLatin1String("bevel")) return {P("size", "Corner size", 0, 10000, 12, 0, " px")};
    if (k == QLatin1String("polygon"))
        return {P("sides", "Sides", 3, 64, 6), P("roundness", "Roundness", 0, 1, 0, 2), P("rotation", "Rotation", -360, 360, 0, 1, "°")};
    if (k == QLatin1String("star"))
        return {P("points", "Points", 3, 64, 5), P("inner", "Inner radius", 0.02, 1, 0.45, 2), P("roundness", "Roundness", 0, 1, 0, 2),
                P("rotation", "Rotation", -360, 360, 0, 1, "°")};
    if (k == QLatin1String("smartpolygon"))
        return {P("sides", "Sides", 3, 32, 5), P("inner", "Edge bend", 0.1, 1.6, 1, 2), P("rotation", "Rotation", -360, 360, 0, 1, "°")};
    if (k == QLatin1String("arc"))
        return {C("style", "Style", {QStringLiteral("Pie"), QStringLiteral("Arc (open)"), QStringLiteral("Chord")}, 0),
                P("start", "Start angle", -360, 360, 0, 1, "°"), P("sweep", "Sweep", 1, 360, 270, 1, "°"), P("inner", "Inner radius", 0, 0.95, 0, 2)};
    if (k == QLatin1String("doughnut")) return {P("inner", "Hole radius", 0.02, 0.98, 0.5, 2)};
    if (k == QLatin1String("spiral"))
        return {P("turns", "Turns", 0.5, 30, 3, 1), P("inner", "Start radius", 0, 0.9, 0, 2),
                C("direction", "Direction", {QStringLiteral("Clockwise"), QStringLiteral("Counter-clockwise")}, 0)};
    if (k == QLatin1String("arrow"))
        return {P("headLen", "Head length", 0.05, 1, 0.4, 2), P("shaft", "Shaft thickness", 0.05, 1, 0.45, 2),
                C("headStyle", "Head", {QStringLiteral("Triangle"), QStringLiteral("Barbed"), QStringLiteral("Round")}, 0),
                C("tailStyle", "Tail", {QStringLiteral("Flat"), QStringLiteral("Notched"), QStringLiteral("Arrow (double)"), QStringLiteral("Round")}, 0)};
    if (k == QLatin1String("arrowline"))
        return {C("startHead", "Start", heads, 0), C("endHead", "End", heads, 2), P("headSize", "Head size", 2, 400, 12, 0, " px"),
                P("x1", "Start X", -10, 10, 0, 3), P("y1", "Start Y", -10, 10, 0.5, 3), P("x2", "End X", -10, 10, 1, 3), P("y2", "End Y", -10, 10, 0.5, 3)};
    if (k == QLatin1String("connector"))
        return {C("style", "Route", {QStringLiteral("Straight"), QStringLiteral("Elbow"), QStringLiteral("Curved")}, 1),
                P("bend", "Bend position", 0, 1, 0.5, 2), C("startHead", "Start", heads, 3), C("endHead", "End", heads, 2),
                P("headSize", "Head size", 2, 400, 10, 0, " px"),
                P("x1", "Start X", -10, 10, 0, 3), P("y1", "Start Y", -10, 10, 0, 3), P("x2", "End X", -10, 10, 1, 3), P("y2", "End Y", -10, 10, 1, 3)};
    if (k == QLatin1String("gear"))
        return {P("teeth", "Teeth", 4, 96, 12), P("root", "Tooth depth (root radius)", 0.3, 0.97, 0.78, 2), P("hole", "Hole radius", 0, 0.9, 0.3, 2),
                P("toothWidth", "Tooth width", 0.15, 0.85, 0.5, 2), P("rotation", "Rotation", -360, 360, 0, 1, "°")};
    if (k == QLatin1String("frame")) return {P("thickness", "Border", 1, 10000, 12, 0, " px")};
    if (k == QLatin1String("lshape")) return {P("thickX", "Vertical arm width", 0.02, 1, 0.35, 2), P("thickY", "Horizontal arm height", 0.02, 1, 0.35, 2)};
    if (k == QLatin1String("speech"))
        return {P("radius", "Corner radius", 0, 10000, 10, 0, " px"), P("tailX", "Tail tip X", -10, 10, 0.22, 3), P("tailY", "Tail tip Y", -10, 10, 1.4, 3),
                P("tailWidth", "Tail width", 0.02, 0.9, 0.18, 2)};
    if (k == QLatin1String("heart"))
        return {P("cleft", "Cleft depth", 0, 1, 0.35, 2), P("point", "Point sharpness", 0, 1, 0.85, 2)};
    return {};
}

QMap<QString, double> defaults(const QString &kind) {
    QMap<QString, double> m;
    for (const Param &p : params(kind)) m[p.key] = p.def;
    return m;
}

bool isLineKind(const QString &kind) {
    const KindInfo *i = info(kind);
    return i && i->line;
}

double param(const VObject &o, const QString &key) {
    auto it = o.shapeParams.constFind(key);
    if (it != o.shapeParams.constEnd()) return it.value();
    for (const Param &p : params(o.shapeKind)) if (p.key == key) return p.def;
    return 0;
}

void setParam(VObject &o, const QString &key, double v) {
    for (const Param &p : params(o.shapeKind))
        if (p.key == key) {
            v = sb(p.min, v, p.max);
            if (p.decimals == 0 || !p.choices.isEmpty()) v = std::round(v);
            o.shapeParams[key] = v;
            return;
        }
}

Geometry geometry(const VObject &o) {
    QMap<QString, double> pm = defaults(o.shapeKind);
    for (auto it = o.shapeParams.cbegin(); it != o.shapeParams.cend(); ++it) pm[it.key()] = it.value();
    return geometry(o.shapeKind, pm, o.rect.normalized());
}

Geometry geometry(const QString &k, const QMap<QString, double> &pmIn, const QRectF &r) {
    QMap<QString, double> pm = defaults(k);
    for (auto it = pmIn.cbegin(); it != pmIn.cend(); ++it) pm[it.key()] = it.value();
    auto v = [&](const char *key) { return pm.value(QString::fromLatin1(key)); };
    Geometry g;
    const double L = r.left(), T = r.top(), R = r.right(), B = r.bottom(), W = r.width(), H = r.height();
    const QPointF c = r.center();
    const double half = qMin(W, H) / 2;
    if (k == QLatin1String("roundrect")) {
        auto cl = [&](double x) { return sb(0.0, x, half); };
        const double a = cl(v("rTL")), b = cl(v("rTR")), cc = cl(v("rBR")), d = cl(v("rBL"));
        QPainterPath &p = g.fill;
        p.moveTo(L + a, T);
        p.lineTo(R - b, T);
        if (b > 0) p.arcTo(QRectF(R - 2 * b, T, 2 * b, 2 * b), 90, -90);
        p.lineTo(R, B - cc);
        if (cc > 0) p.arcTo(QRectF(R - 2 * cc, B - 2 * cc, 2 * cc, 2 * cc), 0, -90);
        p.lineTo(L + d, B);
        if (d > 0) p.arcTo(QRectF(L, B - 2 * d, 2 * d, 2 * d), 270, -90);
        p.lineTo(L, T + a);
        if (a > 0) p.arcTo(QRectF(L, T, 2 * a, 2 * a), 180, -90);
        p.closeSubpath();
    } else if (k == QLatin1String("chamfer")) {
        const double s = sb(0.0, v("size"), half);
        QPainterPath &p = g.fill;
        auto box = [s](double x, double y) { return QRectF(x - s, y - s, 2 * s, 2 * s); };
        p.moveTo(L + s, T);
        p.lineTo(R - s, T);
        if (s > 0) p.arcTo(box(R, T), 180, 90);
        p.lineTo(R, B - s);
        if (s > 0) p.arcTo(box(R, B), 90, 90);
        p.lineTo(L + s, B);
        if (s > 0) p.arcTo(box(L, B), 0, 90);
        p.lineTo(L, T + s);
        if (s > 0) p.arcTo(box(L, T), 270, 90);
        p.closeSubpath();
    } else if (k == QLatin1String("bevel")) {
        const double s = sb(0.0, v("size"), half);
        g.fill.addPolygon(QPolygonF{{L + s, T}, {R - s, T}, {R, T + s}, {R, B - s}, {R - s, B}, {L + s, B}, {L, B - s}, {L, T + s}});
        g.fill.closeSubpath();
    } else if (k == QLatin1String("polygon")) {
        const int n = sb(3, int(std::lround(v("sides"))), 64);
        QPolygonF pts;
        for (int i = 0; i < n; ++i) pts << polar(r, -90 + v("rotation") + i * 360.0 / n, 1);
        g.fill = roundedPolygon(pts, v("roundness"));
    } else if (k == QLatin1String("star")) {
        const int n = sb(3, int(std::lround(v("points"))), 64);
        QPolygonF pts;
        for (int i = 0; i < 2 * n; ++i) pts << polar(r, -90 + v("rotation") + i * 180.0 / n, i % 2 ? sb(0.02, v("inner"), 1.0) : 1.0);
        g.fill = roundedPolygon(pts, v("roundness"));
    } else if (k == QLatin1String("smartpolygon")) {
        const int n = sb(3, int(std::lround(v("sides"))), 32);
        const double mid = std::cos(M_PI / n) * v("inner");
        QPolygonF pts;
        for (int i = 0; i < 2 * n; ++i) pts << polar(r, -90 + v("rotation") + i * 180.0 / n, i % 2 ? mid : 1.0);
        g.fill.addPolygon(pts);
        g.fill.closeSubpath();
    } else if (k == QLatin1String("arc")) {
        const int style = int(v("style"));
        const double st = v("start"), sw = sb(1.0, v("sweep"), 360.0), in = sb(0.0, v("inner"), 0.95);
        if (style == 1) { // open arc
            g.open.arcMoveTo(r, -st);
            g.open.arcTo(r, -st, -sw);
        } else {
            QPainterPath &p = g.fill;
            if (sw >= 360 && in > 0) {
                p.addEllipse(r);
                const QRectF ir(c.x() - W / 2 * in, c.y() - H / 2 * in, W * in, H * in);
                p.addEllipse(ir);
                p.setFillRule(Qt::OddEvenFill);
            } else if (style == 0 && in > 0) { // donut segment
                const QRectF ir(c.x() - W / 2 * in, c.y() - H / 2 * in, W * in, H * in);
                p.arcMoveTo(r, -st);
                p.arcTo(r, -st, -sw);
                QPainterPath inner;
                inner.arcMoveTo(ir, -(st + sw));
                p.lineTo(inner.currentPosition());
                p.arcTo(ir, -(st + sw), sw);
                p.closeSubpath();
            } else if (style == 0) { // pie
                if (sw >= 360) p.addEllipse(r);
                else { p.moveTo(c); p.arcTo(r, -st, -sw); p.closeSubpath(); }
            } else { // chord
                p.arcMoveTo(r, -st);
                p.arcTo(r, -st, -sw);
                p.closeSubpath();
            }
        }
    } else if (k == QLatin1String("doughnut")) {
        const double in = sb(0.02, v("inner"), 0.98);
        g.fill.addEllipse(r);
        g.fill.addEllipse(QRectF(c.x() - W / 2 * in, c.y() - H / 2 * in, W * in, H * in));
        g.fill.setFillRule(Qt::OddEvenFill);
    } else if (k == QLatin1String("spiral")) {
        const double turns = sb(0.5, v("turns"), 30.0), in = sb(0.0, v("inner"), 0.9);
        const double dir = v("direction") >= 0.5 ? -1 : 1;
        const int steps = qMax(16, int(turns * 72));
        for (int i = 0; i <= steps; ++i) {
            const double t = double(i) / steps;
            const QPointF pt = polar(r, dir * t * turns * 360.0, in + (1 - in) * t);
            if (i == 0) g.open.moveTo(pt); else g.open.lineTo(pt);
        }
    } else if (k == QLatin1String("arrow")) {
        const double hl = sb(0.05, v("headLen"), 1.0) * W, sh = sb(0.05, v("shaft"), 1.0) * H / 2;
        const int hs = int(v("headStyle")), ts = int(v("tailStyle"));
        QPainterPath &p = g.fill;
        const double shaftEnd = hs == 1 ? R - hl * 0.65 : R - hl;
        // tail top-left start
        double tailX = L;
        if (ts == 2) tailX = hs == 1 ? L + hl * 0.65 : L + hl;
        if (ts == 3) tailX = L + sh;
        p.moveTo(tailX, c.y() - sh);
        p.lineTo(shaftEnd, c.y() - sh);
        if (hs == 2) { // round head: semicircle
            p.lineTo(R - hl, c.y() - sh);
            p.lineTo(R - hl, T);
            p.arcTo(QRectF(R - 2 * hl, T, 2 * hl, H), 90, -180);
            p.lineTo(R - hl, c.y() + sh);
        } else {
            p.lineTo(R - hl, T);
            p.lineTo(R, c.y());
            p.lineTo(R - hl, B);
            p.lineTo(shaftEnd, c.y() + sh);
        }
        p.lineTo(tailX, c.y() + sh);
        if (ts == 1) { p.lineTo(L, c.y() + sh); p.lineTo(L + sh, c.y()); p.lineTo(L, c.y() - sh); }
        else if (ts == 2) { p.lineTo(L + hl, B); p.lineTo(L, c.y()); p.lineTo(L + hl, T); p.lineTo(tailX, c.y() - sh); }
        else if (ts == 3) { p.arcTo(QRectF(L, c.y() - sh, 2 * sh, 2 * sh), 270, -180); }
        else p.lineTo(L, c.y() - sh);
        p.closeSubpath();
    } else if (k == QLatin1String("arrowline") || k == QLatin1String("connector")) {
        const QPointF a = ept(pm, r, 0), b = ept(pm, r, 1);
        const double hs = v("headSize");
        QPolygonF route{a, b};
        int curve = 0;
        if (k == QLatin1String("connector")) {
            const int style = int(v("style"));
            const double mx = a.x() + sb(0.0, v("bend"), 1.0) * (b.x() - a.x());
            if (style == 1) route = QPolygonF{a, {mx, a.y()}, {mx, b.y()}, b};
            else if (style == 2) { route = QPolygonF{a, {mx, a.y()}, {mx, b.y()}, b}; curve = 1; }
        }
        // direction at each end
        auto dirAt = [&](int end) {
            const int n = route.size();
            if (end == 0) { for (int i = 1; i < n; ++i) if (route[i] != route[0]) return route[0] - route[i]; }
            else { for (int i = n - 2; i >= 0; --i) if (route[i] != route[n - 1]) return route[n - 1] - route[i]; }
            return end ? b - a : a - b;
        };
        const QPointF d0 = dirAt(0), d1 = dirAt(1);
        const double cut0 = addHead(int(v("startHead")), a, d0, hs, g.heads, g.open);
        const double cut1 = addHead(int(v("endHead")), b, d1, hs, g.heads, g.open);
        auto shorten = [](const QPointF &p, QPointF d, double by) { const double l = std::hypot(d.x(), d.y()); return l > 1e-9 ? p - d / l * by : p; };
        QPolygonF rt = route;
        rt.first() = shorten(rt.first(), d0, cut0);
        rt.last() = shorten(rt.last(), d1, cut1);
        QPainterPath line;
        line.moveTo(rt.first());
        if (curve) line.cubicTo(rt[1], rt[2], rt[3]);
        else for (int i = 1; i < rt.size(); ++i) line.lineTo(rt[i]);
        line.addPath(g.open);
        g.open = line;
    } else if (k == QLatin1String("gear")) {
        const int n = sb(4, int(std::lround(v("teeth"))), 96);
        const double root = sb(0.3, v("root"), 0.97), tw = sb(0.15, v("toothWidth"), 0.85), hole = sb(0.0, v("hole"), 0.9);
        const double pitch = 360.0 / n, rot = v("rotation") - 90;
        QPolygonF pts;
        for (int i = 0; i < n; ++i) {
            const double a = rot + i * pitch;
            const double base = tw * pitch / 2, top = base * 0.7;
            pts << polar(r, a - base, root) << polar(r, a - top, 1) << polar(r, a + top, 1) << polar(r, a + base, root);
            // root arc between this tooth and the next
            const double a0 = a + base, a1 = a + pitch - base;
            for (int s = 1; s < 4; ++s) pts << polar(r, a0 + (a1 - a0) * s / 4.0, root);
        }
        g.fill.addPolygon(pts);
        g.fill.closeSubpath();
        if (hole > 0) g.fill.addEllipse(QRectF(c.x() - W / 2 * hole, c.y() - H / 2 * hole, W * hole, H * hole));
        g.fill.setFillRule(Qt::OddEvenFill);
    } else if (k == QLatin1String("frame")) {
        const double t = sb(0.0, v("thickness"), half);
        g.fill.addRect(r);
        if (t < half) g.fill.addRect(r.adjusted(t, t, -t, -t));
        g.fill.setFillRule(Qt::OddEvenFill);
    } else if (k == QLatin1String("lshape")) {
        const double tx = sb(0.02, v("thickX"), 1.0) * W, ty = sb(0.02, v("thickY"), 1.0) * H;
        g.fill.addPolygon(QPolygonF{{L, T}, {L + tx, T}, {L + tx, B - ty}, {R, B - ty}, {R, B}, {L, B}});
        g.fill.closeSubpath();
    } else if (k == QLatin1String("speech")) {
        const double rad = sb(0.0, v("radius"), half);
        QPainterPath body;
        body.addRoundedRect(r, rad, rad);
        const QPointF tipPt(L + v("tailX") * W, T + v("tailY") * H);
        if (r.contains(tipPt) || W < 2 || H < 2) { g.fill = body; return g; }
        const double tw = sb(0.02, v("tailWidth"), 0.9);
        const QPointF u = toUnit(r, tipPt);
        QPointF b0, b1;
        if (std::abs(u.y()) >= std::abs(u.x())) { // top / bottom edge
            const double w = tw * W;
            const double bx = sb(L + rad + w / 2, c.x() + (tipPt.x() - c.x()) * 0.5, R - rad - w / 2);
            const double inset = qMin(4.0, H / 4), ey = u.y() > 0 ? B - inset : T + inset;
            b0 = QPointF(bx - w / 2, ey); b1 = QPointF(bx + w / 2, ey);
        } else {
            const double w = tw * H;
            const double by = sb(T + rad + w / 2, c.y() + (tipPt.y() - c.y()) * 0.5, B - rad - w / 2);
            const double inset = qMin(4.0, W / 4), ex = u.x() > 0 ? R - inset : L + inset;
            b0 = QPointF(ex, by - w / 2); b1 = QPointF(ex, by + w / 2);
        }
        QPainterPath tail;
        tail.addPolygon(QPolygonF{b0, tipPt, b1});
        tail.closeSubpath();
        g.fill = body.united(tail).simplified();
    } else if (k == QLatin1String("heart")) {
        g.fill = heartPath(r, v("cleft"), v("point"));
    } else {
        g.fill.addRect(r);
    }
    return g;
}

QPainterPath outline(const VObject &o) {
    const Geometry g = geometry(o);
    QPainterPath p = g.fill;
    p.addPath(g.open);
    p.addPath(g.heads);
    return p;
}

QPainterPath previewPath(const QString &kind, const QRectF &r) {
    QMap<QString, double> pm = defaults(kind);
    // radii in px: scale them to the preview
    const double s = qMin(r.width(), r.height()) / 100.0;
    for (const char *k : {"rTL", "rTR", "rBR", "rBL", "size", "thickness", "radius", "headSize"})
        if (pm.contains(QString::fromLatin1(k))) pm[QString::fromLatin1(k)] *= qMax(0.15, s) * 2.0;
    if (kind == QLatin1String("speech")) { pm[QStringLiteral("tailY")] = 1.25; }
    QRectF rr = r;
    if (kind == QLatin1String("speech")) rr.setBottom(r.top() + r.height() * 0.78);
    const Geometry g = geometry(kind, pm, rr);
    QPainterPath p = g.fill;
    p.addPath(g.open);
    p.addPath(g.heads);
    return p;
}

QVector<Handle> handles(const VObject &o) {
    QVector<Handle> h;
    const QRectF r = o.rect.normalized();
    const QString &k = o.shapeKind;
    const double L = r.left(), T = r.top(), R = r.right(), B = r.bottom(), W = r.width(), H = r.height();
    const double half = qMin(W, H) / 2;
    auto add = [&](int id, const QPointF &p, const QString &tip) { h.append(Handle{id, p, tip}); };
    auto pv = [&](const char *key) { return param(o, QString::fromLatin1(key)); };
    if (k == QLatin1String("roundrect")) {
        add(0, {L + qMin(pv("rTL"), half), T}, QStringLiteral("Corner radius (Alt: this corner only)"));
        add(1, {R - qMin(pv("rTR"), half), T}, QStringLiteral("Corner radius (Alt: this corner only)"));
        add(2, {R - qMin(pv("rBR"), half), B}, QStringLiteral("Corner radius (Alt: this corner only)"));
        add(3, {L + qMin(pv("rBL"), half), B}, QStringLiteral("Corner radius (Alt: this corner only)"));
    } else if (k == QLatin1String("chamfer") || k == QLatin1String("bevel")) {
        add(0, {L + qMin(pv("size"), half), T}, QStringLiteral("Corner size"));
    } else if (k == QLatin1String("polygon")) {
        const int n = sb(3, int(pv("sides")), 64);
        add(0, polar(r, -90 + pv("rotation"), 1), QStringLiteral("Rotation"));
        add(1, polar(r, -90 + pv("rotation") + 360.0 / n, 1), QStringLiteral("Drag around to change the number of sides"));
        const QPointF v0 = polar(r, -90 + pv("rotation"), 1), v1 = polar(r, -90 + pv("rotation") + 360.0 / n, 1);
        add(2, v0 + (v1 - v0) * (0.08 + pv("roundness") * 0.42), QStringLiteral("Roundness"));
    } else if (k == QLatin1String("star")) {
        const int n = sb(3, int(pv("points")), 64);
        add(0, polar(r, -90 + pv("rotation"), 1), QStringLiteral("Rotation"));
        add(1, polar(r, -90 + pv("rotation") + 180.0 / n, pv("inner")), QStringLiteral("Inner radius"));
        add(2, polar(r, -90 + pv("rotation") + 360.0 / n, 1), QStringLiteral("Drag around to change the number of points"));
        const QPointF v0 = polar(r, -90 + pv("rotation"), 1), v1 = polar(r, -90 + pv("rotation") + 180.0 / n, pv("inner"));
        add(3, v0 + (v1 - v0) * (0.08 + pv("roundness") * 0.42), QStringLiteral("Roundness"));
    } else if (k == QLatin1String("smartpolygon")) {
        const int n = sb(3, int(pv("sides")), 32);
        add(0, polar(r, -90 + pv("rotation"), 1), QStringLiteral("Rotation"));
        add(1, polar(r, -90 + pv("rotation") + 180.0 / n, std::cos(M_PI / n) * pv("inner")), QStringLiteral("Bend the edges in or out"));
        add(2, polar(r, -90 + pv("rotation") + 360.0 / n, 1), QStringLiteral("Drag around to change the number of sides"));
    } else if (k == QLatin1String("arc")) {
        add(0, polar(r, pv("start"), 1), QStringLiteral("Start angle"));
        add(1, polar(r, pv("start") + pv("sweep"), 1), QStringLiteral("End angle"));
        if (int(pv("style")) == 0) add(2, polar(r, pv("start") + pv("sweep") / 2, qMax(0.0, pv("inner"))), QStringLiteral("Inner radius (donut)"));
    } else if (k == QLatin1String("doughnut")) {
        add(0, polar(r, 0, pv("inner")), QStringLiteral("Hole radius"));
    } else if (k == QLatin1String("spiral")) {
        add(0, polar(r, 0, pv("inner")), QStringLiteral("Start radius"));
        add(1, {L + W * (pv("turns") - 0.5) / 29.5, B}, QStringLiteral("Turns"));
    } else if (k == QLatin1String("arrow")) {
        add(0, {R - pv("headLen") * W, r.center().y() - pv("shaft") * H / 2}, QStringLiteral("Head length / shaft thickness"));
    } else if (k == QLatin1String("arrowline") || k == QLatin1String("connector")) {
        add(0, endpoint(o, 0), QStringLiteral("Start point"));
        add(1, endpoint(o, 1), QStringLiteral("End point"));
        if (k == QLatin1String("connector") && int(pv("style")) != 0) {
            const QPointF a = endpoint(o, 0), b = endpoint(o, 1);
            add(2, {a.x() + pv("bend") * (b.x() - a.x()), (a.y() + b.y()) / 2}, QStringLiteral("Bend position"));
        }
    } else if (k == QLatin1String("gear")) {
        const int n = sb(4, int(pv("teeth")), 96);
        const double pitch = 360.0 / n, rot = pv("rotation") - 90;
        add(0, polar(r, rot + pitch / 2, pv("root")), QStringLiteral("Tooth depth"));
        add(1, polar(r, 0, pv("hole")), QStringLiteral("Hole radius"));
        add(2, polar(r, rot + pitch, 1), QStringLiteral("Drag around to change the number of teeth"));
        add(3, polar(r, rot + pv("toothWidth") * pitch / 2 * 0.7, 1), QStringLiteral("Tooth width"));
    } else if (k == QLatin1String("frame")) {
        const double t = qMin(pv("thickness"), half);
        add(0, {L + t, T + t}, QStringLiteral("Border width"));
    } else if (k == QLatin1String("lshape")) {
        add(0, {L + pv("thickX") * W, B - pv("thickY") * H}, QStringLiteral("Arm widths"));
    } else if (k == QLatin1String("speech")) {
        add(0, {L + pv("tailX") * W, T + pv("tailY") * H}, QStringLiteral("Tail tip"));
        add(1, {L + qMin(pv("radius"), half), T}, QStringLiteral("Corner radius"));
    } else if (k == QLatin1String("heart")) {
        add(0, heartCleftPos(r, pv("cleft")), QStringLiteral("Cleft depth"));
        add(1, heartPointPos(r, pv("point")), QStringLiteral("Point sharpness"));
    }
    return h;
}

QPointF endpoint(const VObject &o, int which) {
    QMap<QString, double> pm = defaults(o.shapeKind);
    for (auto it = o.shapeParams.cbegin(); it != o.shapeParams.cend(); ++it) pm[it.key()] = it.value();
    return ept(pm, o.rect.normalized(), which);
}

void setEndpoints(VObject &o, const QPointF &a, const QPointF &b) {
    QRectF r = QRectF(a, b).normalized();
    o.rect = r;
    auto fr = [](double v, double lo, double size) { return size > 1e-9 ? (v - lo) / size : 0.5; };
    o.shapeParams[QStringLiteral("x1")] = fr(a.x(), r.left(), r.width());
    o.shapeParams[QStringLiteral("y1")] = fr(a.y(), r.top(), r.height());
    o.shapeParams[QStringLiteral("x2")] = fr(b.x(), r.left(), r.width());
    o.shapeParams[QStringLiteral("y2")] = fr(b.y(), r.top(), r.height());
}

void dragHandle(VObject &o, int id, const QPointF &p, bool alt) {
    const QRectF r = o.rect.normalized();
    const QString &k = o.shapeKind;
    const double L = r.left(), T = r.top(), R = r.right(), B = r.bottom(), W = r.width(), H = r.height();
    const double half = qMin(W, H) / 2;
    auto set = [&](const char *key, double v) { setParam(o, QString::fromLatin1(key), v); };
    auto pv = [&](const char *key) { return param(o, QString::fromLatin1(key)); };
    auto countFrom = [&](double baseAngle, double perStep, int lo, int hi) {
        // the handle sits one step around from the first vertex: its angle gives the count
        double d = norm360(angleOf(r, p) - baseAngle);
        if (d < 1) d = 1;
        return sb(lo, int(std::lround(360.0 / d / perStep)), hi);
    };
    if (k == QLatin1String("roundrect")) {
        double rad = 0;
        switch (id) {
        case 0: rad = p.x() - L; break;
        case 1: rad = R - p.x(); break;
        case 2: rad = R - p.x(); break;
        case 3: rad = p.x() - L; break;
        }
        rad = sb(0.0, rad, half);
        static const char *keys[] = {"rTL", "rTR", "rBR", "rBL"};
        if (alt) set(keys[sb(0, id, 3)], rad);
        else for (const char *kk : keys) set(kk, rad);
    } else if (k == QLatin1String("chamfer") || k == QLatin1String("bevel")) {
        set("size", sb(0.0, p.x() - L, half));
    } else if (k == QLatin1String("polygon")) {
        if (id == 0) set("rotation", norm360(angleOf(r, p) + 90 + 180) - 180);
        else if (id == 1) set("sides", countFrom(-90 + pv("rotation"), 1, 3, 64));
        else {
            const int n = sb(3, int(pv("sides")), 64);
            const QPointF v0 = polar(r, -90 + pv("rotation"), 1), v1 = polar(r, -90 + pv("rotation") + 360.0 / n, 1);
            const QPointF e = v1 - v0;
            const double t = QPointF::dotProduct(p - v0, e) / qMax(1e-9, QPointF::dotProduct(e, e));
            set("roundness", (t - 0.08) / 0.42);
        }
    } else if (k == QLatin1String("star")) {
        const int n = sb(3, int(pv("points")), 64);
        if (id == 0) set("rotation", norm360(angleOf(r, p) + 90 + 180) - 180);
        else if (id == 1) set("inner", radiusOf(r, p));
        else if (id == 2) set("points", countFrom(-90 + pv("rotation"), 1, 3, 64));
        else {
            const QPointF v0 = polar(r, -90 + pv("rotation"), 1), v1 = polar(r, -90 + pv("rotation") + 180.0 / n, pv("inner"));
            const QPointF e = v1 - v0;
            const double t = QPointF::dotProduct(p - v0, e) / qMax(1e-9, QPointF::dotProduct(e, e));
            set("roundness", (t - 0.08) / 0.42);
        }
    } else if (k == QLatin1String("smartpolygon")) {
        const int n = sb(3, int(pv("sides")), 32);
        if (id == 0) set("rotation", norm360(angleOf(r, p) + 90 + 180) - 180);
        else if (id == 1) set("inner", radiusOf(r, p) / std::cos(M_PI / n));
        else set("sides", countFrom(-90 + pv("rotation"), 1, 3, 32));
    } else if (k == QLatin1String("arc")) {
        const double a = angleOf(r, p);
        if (id == 0) {
            const double end = pv("start") + pv("sweep");
            double sw = norm360(end - a);
            if (sw < 1) sw = 360;
            set("start", a);
            set("sweep", sw);
        } else if (id == 1) {
            double sw = norm360(a - pv("start"));
            if (sw < 1) sw = 360;
            set("sweep", sw);
        } else set("inner", radiusOf(r, p));
    } else if (k == QLatin1String("doughnut")) {
        set("inner", radiusOf(r, p));
    } else if (k == QLatin1String("spiral")) {
        if (id == 0) set("inner", radiusOf(r, p));
        else set("turns", 0.5 + (W > 1e-9 ? (p.x() - L) / W : 0) * 29.5);
    } else if (k == QLatin1String("arrow")) {
        set("headLen", W > 1e-9 ? (R - p.x()) / W : 0.4);
        set("shaft", H > 1e-9 ? (r.center().y() - p.y()) / (H / 2) : 0.45);
    } else if (k == QLatin1String("arrowline") || k == QLatin1String("connector")) {
        if (id == 0 || id == 1) {
            const QPointF a = id == 0 ? p : endpoint(o, 0), b = id == 1 ? p : endpoint(o, 1);
            setEndpoints(o, a, b);
        } else {
            const QPointF a = endpoint(o, 0), b = endpoint(o, 1);
            set("bend", std::abs(b.x() - a.x()) > 1e-9 ? (p.x() - a.x()) / (b.x() - a.x()) : 0.5);
        }
    } else if (k == QLatin1String("gear")) {
        const int n = sb(4, int(pv("teeth")), 96);
        const double pitch = 360.0 / n, rot = pv("rotation") - 90;
        if (id == 0) set("root", radiusOf(r, p));
        else if (id == 1) set("hole", radiusOf(r, p));
        else if (id == 2) set("teeth", countFrom(rot, 1, 4, 96));
        else set("toothWidth", norm360(angleOf(r, p) - rot + 180) - 180 > 0 ? (norm360(angleOf(r, p) - rot + 180) - 180) / (pitch / 2 * 0.7) : 0.15);
    } else if (k == QLatin1String("frame")) {
        set("thickness", sb(1.0, qMin(p.x() - L, p.y() - T), half));
    } else if (k == QLatin1String("lshape")) {
        set("thickX", W > 1e-9 ? (p.x() - L) / W : 0.35);
        set("thickY", H > 1e-9 ? (B - p.y()) / H : 0.35);
    } else if (k == QLatin1String("speech")) {
        if (id == 0) {
            set("tailX", W > 1e-9 ? (p.x() - L) / W : 0.5);
            set("tailY", H > 1e-9 ? (p.y() - T) / H : 1.4);
        } else set("radius", sb(0.0, p.x() - L, half));
    } else if (k == QLatin1String("heart")) {
        if (id == 0) {
            // invert cy (piecewise through the default 0.35 -> 0.30)
            if (H <= 1e-9) {
                set("cleft", 0.35);
            } else {
                const double uy = (p.y() - T) / H;
                const double cleft = (uy <= 0.30)
                    ? (uy - 0.14) / (0.30 - 0.14) * 0.35
                    : 0.35 + (uy - 0.30) / (0.52 - 0.30) * 0.65;
                set("cleft", cleft);
            }
        } else {
            // sharper = handle closer to the centre line (see heartPointPos)
            const double target = W > 1e-9 ? sb(0.0, (p.x() - L) / W, 1.0) : 0.85;
            const double ux = target >= 0.5 ? target : 1.0 - target;
            double lo = 0.0, hi = 1.0;
            for (int i = 0; i < 24; ++i) {
                const double mid = 0.5 * (lo + hi);
                const double hx = heartPointPos(QRectF(0, 0, 1, 1), mid).x();
                if (hx > ux) lo = mid;  // blunter sits further from the centre
                else hi = mid;
            }
            set("point", 0.5 * (lo + hi));
        }
    }
}

void init(VObject &o, const QString &kind) {
    o.type = ObjType::AutoShape;
    o.shapeKind = kind;
    o.shapeParams = defaults(kind);
    if (isLineKind(kind)) o.fillOn = false;
}
}
