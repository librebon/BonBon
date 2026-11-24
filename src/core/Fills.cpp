#include "Fills.h"
#include "Stroke.h"
#include "Procedural.h"
#include "Effects.h"
#include "Paths.h"
#include <QCache>
#include <QMutex>
#include <QPainterPathStroker>
#include <QtMath>
#include <random>

namespace fills {

namespace {
const struct { FillKind k; const char *key; const char *label; } kKinds[] = {
    {FillKind::Solid, "solid", "Solid"},
    {FillKind::WebDither, "webDither", "Web Dither"},
    {FillKind::Pattern, "pattern", "Pattern"},
    {FillKind::Linear, "linear", "Linear"},
    {FillKind::Radial, "radial", "Radial"},
    {FillKind::Cone, "cone", "Cone"},
    {FillKind::Contour, "contour", "Contour"},
    {FillKind::Satin, "satin", "Satin"},
    {FillKind::Starburst, "starburst", "Starburst"},
    {FillKind::Bars, "bars", "Bars"},
    {FillKind::Ripples, "ripples", "Ripples"},
    {FillKind::Waves, "waves", "Waves"},
    {FillKind::Ellipse, "ellipse", "Ellipse"},
    {FillKind::Rectangle, "rectangle", "Rectangle"},
};

double tri(double x) { // 0 -> 1 -> 0 over 2 units
    double f = x / 2 - std::floor(x / 2);
    return 1 - std::abs(1 - 2 * f);
}
double deviceScale(const QTransform &t) { return std::sqrt(std::abs(t.determinant())); }

// periodic value noise for seamless tiles
double hash2(int x, int y, int seed) {
    quint32 h = quint32(x) * 374761393u + quint32(y) * 668265263u + quint32(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return double((h ^ (h >> 16)) & 0xFFFFFF) / double(0xFFFFFF);
}
double vnoise(double x, double y, int period, int seed) {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    double fx = x - xi, fy = y - yi;
    auto w = [period](int v) { return ((v % period) + period) % period; };
    double a = hash2(w(xi), w(yi), seed), b = hash2(w(xi + 1), w(yi), seed);
    double c = hash2(w(xi), w(yi + 1), seed), d = hash2(w(xi + 1), w(yi + 1), seed);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
// fractal noise over a tile of `size` px, `cells` lattice cells across
double fbm(double x, double y, int size, int cells, int seed, int oct = 4) {
    double v = 0, amp = 0.5, tot = 0;
    for (int o = 0; o < oct; ++o) {
        int c = cells << o;
        v += amp * vnoise(x * c / size, y * c / size, c, seed + o);
        tot += amp;
        amp *= 0.5;
    }
    return v / tot;
}
QRgb mix(QRgb a, QRgb b, double t) {
    t = qBound(0.0, t, 1.0);
    return qRgb(int(qRed(a) + (qRed(b) - qRed(a)) * t), int(qGreen(a) + (qGreen(b) - qGreen(a)) * t), int(qBlue(a) + (qBlue(b) - qBlue(a)) * t));
}
} // namespace

QStringList fillKindLabels() {
    QStringList l;
    for (auto &k : kKinds) l << QString::fromLatin1(k.label);
    return l;
}
QString fillKindKey(FillKind k) {
    for (auto &e : kKinds) if (e.k == k) return QString::fromLatin1(e.key);
    return QStringLiteral("solid");
}
FillKind fillKindFromKey(const QString &key) {
    for (auto &e : kKinds) if (key == QLatin1String(e.key)) return e.k;
    return FillKind::Solid;
}
bool isGradient(FillKind k) { return int(k) >= int(FillKind::Linear); }

QVector<GradStop> effectiveGradient(const VObject &o) {
    if (o.gradient.size() >= 2) return o.gradient;
    if (o.gradient.size() == 1) return {o.gradient[0], {1, o.gradient[0].color}};
    return {{0, o.fill.isValid() ? o.fill : QColor(Qt::black)}, {1, Qt::white}};
}

QStringList gradientPresetNames() {
    return {QStringLiteral("Black, White"), QStringLiteral("Blue, Yellow, Blue"), QStringLiteral("Cobalt Blue"),
            QStringLiteral("Copper"), QStringLiteral("Emerald Green"), QStringLiteral("Pastels"),
            QStringLiteral("Red, Blue"), QStringLiteral("Red, Green, Blue"), QStringLiteral("Silver"),
            QStringLiteral("Spectrum"), QStringLiteral("Violet, Orange"), QStringLiteral("White, Transparent")};
}
QVector<GradStop> gradientPreset(const QString &n) {
    auto c = [](const char *s) { return QColor(QString::fromLatin1(s)); };
    if (n == QLatin1String("Blue, Yellow, Blue")) return {{0, c("#0000ff")}, {0.5, c("#ffff00")}, {1, c("#0000ff")}};
    if (n == QLatin1String("Cobalt Blue")) return {{0, c("#000066")}, {0.5, c("#3366ff")}, {1, c("#ccddff")}};
    if (n == QLatin1String("Copper")) return {{0, c("#6b2e0e")}, {0.35, c("#e09a5c")}, {0.6, c("#fbd6b0")}, {1, c("#7a3a14")}};
    if (n == QLatin1String("Emerald Green")) return {{0, c("#003311")}, {0.5, c("#22aa55")}, {1, c("#ccffdd")}};
    if (n == QLatin1String("Pastels")) return {{0, c("#ffcccc")}, {0.33, c("#ffffcc")}, {0.66, c("#ccffff")}, {1, c("#ccccff")}};
    if (n == QLatin1String("Red, Blue")) return {{0, c("#ff0000")}, {1, c("#0000ff")}};
    if (n == QLatin1String("Red, Green, Blue")) return {{0, c("#ff0000")}, {0.5, c("#00ff00")}, {1, c("#0000ff")}};
    if (n == QLatin1String("Silver")) return {{0, c("#666666")}, {0.3, c("#f4f4f4")}, {0.55, c("#999999")}, {0.8, c("#e8e8e8")}, {1, c("#777777")}};
    if (n == QLatin1String("Spectrum"))
        return {{0, c("#ff0000")}, {0.17, c("#ffff00")}, {0.33, c("#00ff00")}, {0.5, c("#00ffff")}, {0.67, c("#0000ff")}, {0.83, c("#ff00ff")}, {1, c("#ff0000")}};
    if (n == QLatin1String("Violet, Orange")) return {{0, c("#6600cc")}, {1, c("#ff9900")}};
    if (n == QLatin1String("White, Transparent")) return {{0, QColor(255, 255, 255, 255)}, {1, QColor(255, 255, 255, 0)}};
    return {{0, Qt::black}, {1, Qt::white}};
}

QColor sampleGradient(const QVector<GradStop> &in, double t) {
    if (in.isEmpty()) return Qt::black;
    QVector<GradStop> s = in;
    std::stable_sort(s.begin(), s.end(), [](const GradStop &a, const GradStop &b) { return a.pos < b.pos; });
    if (t <= s.first().pos) return s.first().color;
    if (t >= s.last().pos) return s.last().color;
    for (int i = 0; i + 1 < s.size(); ++i) {
        if (t <= s[i + 1].pos) {
            double span = s[i + 1].pos - s[i].pos;
            double f = span > 1e-9 ? (t - s[i].pos) / span : 1;
            const QColor &a = s[i].color, &b = s[i + 1].color;
            return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * f), float(a.greenF() + (b.greenF() - a.greenF()) * f),
                                    float(a.blueF() + (b.blueF() - a.blueF()) * f), float(a.alphaF() + (b.alphaF() - a.alphaF()) * f));
        }
    }
    return s.last().color;
}

bool usesSecondAxis(FillKind k) { return isGradient(k) && k != FillKind::Linear && k != FillKind::Bars; }

void setFillKind(VObject &o, FillKind k) {
    const bool wasGrad = isGradient(o.fillKind);
    o.fillKind = k;
    o.fillOn = true;
    if (!isGradient(k)) return;
    if (o.gradient.size() < 2) o.gradient = gradientPreset(QStringLiteral("Black, White"));   // 0.3.6 default preset
    // keep the user's handles when switching between gradient types; Contour starts from its own
    // defaults so it keeps the plain distance-from-edge look until its handles are dragged
    if (wasGrad && k != FillKind::Contour) return;
    switch (k) {
    case FillKind::Linear: case FillKind::Bars: case FillKind::Waves:
        o.gradP0 = {0, 0.5}; o.gradP1 = {1, 0.5}; o.gradP2 = {0, 0}; break;
    case FillKind::Ellipse:
        o.gradP0 = {0.5, 0.5}; o.gradP1 = {1, 0.5}; o.gradP2 = {0.5, 0.25}; break;
    default:
        o.gradP0 = {0.5, 0.5}; o.gradP1 = {1, 0.5}; o.gradP2 = {0.5, 0}; break;
    }
}

namespace {
// the bars' length at identity: half the bounds (so they end on the right and bottom edges)
QPointF patternBarLengths(const QRectF &b) { return {qMax(8.0, b.width() / 2), qMax(8.0, b.height() / 2)}; }
}
QTransform patternBrushTransform(const VObject &o) {
    const QRectF b = o.bounds();
    if (o.patternXform.isIdentity()) return QTransform::fromTranslate(b.left(), b.top());   // exactly as before
    const QTransform &t = o.patternXform;
    const QTransform lin(t.m11(), t.m12(), t.m21(), t.m22(), 0, 0);
    // about the centre handle: the tile point under it stays put when the bars turn or stretch
    const QPointF c = b.center() + QPointF(t.dx(), t.dy());
    return QTransform::fromTranslate(-b.width() / 2, -b.height() / 2) * lin * QTransform::fromTranslate(c.x(), c.y());
}
QImage patternImage(const VObject &o) {
    return (o.pattern == QLatin1String("Custom") && !o.patternImage.isNull()) ? o.patternImage
         : (o.pattern == QLatin1String("Procedural") && !o.patternProc.isEmpty()) ? proc::patternTile(o.patternProc) : patternTile(o.pattern);
}
// handles live in the object's own (untransformed) frame, as the tile is painted, then follow
// its transform onto the canvas
void patternHandles(const VObject &o, QPointF h[3]) {
    if (o.hasXform()) {
        patternHandles(o.untransformed(), h);
        for (int i = 0; i < 3; ++i) h[i] = o.xform.map(h[i]);
        return;
    }
    const QRectF b = o.bounds();
    const QTransform &t = o.patternXform;
    const QPointF L = patternBarLengths(b);
    h[0] = b.center() + QPointF(t.dx(), t.dy());
    h[1] = h[0] + QPointF(t.m11(), t.m12()) * L.x();
    h[2] = h[0] + QPointF(t.m21(), t.m22()) * L.y();
}
bool setPatternHandles(VObject &o, const QPointF hd[3]) {
    QPointF h[3] = {hd[0], hd[1], hd[2]};
    QRectF b = o.bounds();
    if (o.hasXform()) {
        bool inv = false;
        const QTransform back = o.xform.inverted(&inv);
        if (!inv) return false;
        for (auto &x : h) x = back.map(x);
        b = o.untransformed().bounds();
    }
    const QPointF L = patternBarLengths(b);
    const QPointF u = (h[1] - h[0]) / L.x(), v = (h[2] - h[0]) / L.y();
    const double det = u.x() * v.y() - u.y() * v.x();
    if (std::abs(det) < 1e-4 || std::hypot(u.x(), u.y()) < 1e-3 || std::hypot(v.x(), v.y()) < 1e-3) return false;
    const QPointF d = h[0] - b.center();
    QTransform t(u.x(), u.y(), v.x(), v.y(), d.x(), d.y());
    // snap a hair from identity back to it (so the default look is exact again)
    auto near = [](double x, double y) { return std::abs(x - y) < 1e-9; };
    if (near(t.m11(), 1) && near(t.m12(), 0) && near(t.m21(), 0) && near(t.m22(), 1) && near(t.dx(), 0) && near(t.dy(), 0)) t = QTransform();
    o.patternXform = t;
    return true;
}
void fillHandles(const VObject &o, QPointF h[3]) {
    if (o.fillKind == FillKind::Pattern) { patternHandles(o, h); return; }
    h[0] = handleToDoc(o, o.gradP0); h[1] = handleToDoc(o, o.gradP1); h[2] = handleToDoc(o, o.gradP2);
}
void setFillHandles(VObject &o, const QPointF h[3]) {
    if (o.fillKind == FillKind::Pattern) { setPatternHandles(o, h); return; }
    o.gradP0 = docToHandle(o, h[0]); o.gradP1 = docToHandle(o, h[1]); o.gradP2 = docToHandle(o, h[2]);
}

void resetContourHandles(VObject &o) { o.gradP0 = {0.5, 0.5}; o.gradP1 = {1, 0.5}; o.gradP2 = {0.5, 0}; }
bool contourHandlesDefault(const VObject &o) {
    auto eq = [](const QPointF &a, const QPointF &b) { return std::abs(a.x() - b.x()) < 1e-9 && std::abs(a.y() - b.y()) < 1e-9; };
    return eq(o.gradP0, {0.5, 0.5}) && eq(o.gradP1, {1, 0.5}) && eq(o.gradP2, {0.5, 0});
}

QPointF handleToDoc(const VObject &o, const QPointF &n) {
    const QRectF b = o.bounds();
    return {b.left() + n.x() * b.width(), b.top() + n.y() * b.height()};
}
QPointF docToHandle(const VObject &o, const QPointF &d) {
    const QRectF b = o.bounds();
    return {(d.x() - b.left()) / qMax(1e-6, b.width()), (d.y() - b.top()) / qMax(1e-6, b.height())};
}

void setGradientVector(VObject &o, const QPointF &from, const QPointF &to) {
    QPointF u = to - from;
    if (std::hypot(u.x(), u.y()) < 0.5) u = QPointF(1, 0);
    QPointF v(u.y(), -u.x());
    if (o.fillKind == FillKind::Ellipse) v *= 0.5;
    o.gradP0 = docToHandle(o, from);
    o.gradP1 = docToHandle(o, from + u);
    o.gradP2 = docToHandle(o, from + v);
}

namespace {
struct Frame { QPointF o, u, v; double det; };
Frame frameOf(const VObject &o) {
    Frame f;
    f.o = handleToDoc(o, o.gradP0);
    f.u = handleToDoc(o, o.gradP1) - f.o;
    f.v = handleToDoc(o, o.gradP2) - f.o;
    if (std::hypot(f.u.x(), f.u.y()) < 1e-6) f.u = QPointF(1, 0);
    f.det = f.u.x() * f.v.y() - f.u.y() * f.v.x();
    if (std::abs(f.det) < 1e-6) { f.v = QPointF(f.u.y(), -f.u.x()); f.det = f.u.x() * f.v.y() - f.u.y() * f.v.x(); }
    return f;
}
double tFromUV(FillKind k, double u, double v) {
    const double r = std::hypot(u, v);
    switch (k) {
    case FillKind::Linear: return qBound(0.0, u, 1.0);
    case FillKind::Radial: case FillKind::Ellipse: case FillKind::Contour: return qMin(r, 1.0);
    case FillKind::Cone: { double a = std::atan2(-v, u) / (2 * M_PI); return a - std::floor(a); }
    case FillKind::Rectangle: return qMin(qMax(std::abs(u), std::abs(v)), 1.0);
    case FillKind::Satin: return 0.5 - 0.5 * std::cos(3 * M_PI * qMin(r, 1.0));
    case FillKind::Starburst: { double a = std::atan2(v, u) / (2 * M_PI); return tri((a - std::floor(a)) * 24); }
    case FillKind::Bars: return tri(u * 8);
    case FillKind::Ripples: return tri(r * 8);
    case FillKind::Waves: return tri(u * 8 + 0.6 * std::sin(v * 4 * M_PI));
    default: return 0;
    }
}
QVector<QRgb> lutOf(const QVector<GradStop> &stops) {
    QVector<QRgb> lut(256);
    for (int i = 0; i < 256; ++i) lut[i] = qPremultiply(sampleGradient(stops, i / 255.0).rgba());
    return lut;
}
QString stopsKey(const QVector<GradStop> &s) {
    QString k;
    for (const auto &g : s) k += QString::number(g.pos, 'g', 5) + QLatin1Char(':') + QString::number(g.color.rgba(), 16) + QLatin1Char(',');
    return k;
}

// Contour with moved handles (Fireworks MX 2004 style), all in grid-pixel space:
//  p* = the deepest point of the shape (centroid of the pixels at the maximum distance)
//  q  = the peak: p* moved by the start handle's offset from the centre of the bounds
//  K  = how far a pixel still is from the peak, measured "through" the shape: at the default
//       handles K = Dmax - D (so t = D / Dmax, the plain look), it is 0 at q, and it never
//       falls below a small share of min(Dmax - D, |x - q|) so no false plateaus appear
//  sigma = |M (x - q)| / |x - q|, M = default frame * current frame^-1: longer end handles
//       shrink sigma (a wider spread), unequal or non-perpendicular axes make it elliptical or
//       skewed, and turning the handles turns the ellipse
//  t  = D / (D + sigma * K)
void contourFromHandles(const VObject &o, const Frame &f, double sx, double sy, int wi, int hi,
                        const QVector<float> &d, float mx, const QVector<QRgb> &lut, QImage &img) {
    double px = 0, py = 0, n = 0;
    const float near = mx - qMax(0.75f, mx * 0.01f);
    for (int y = 0; y < hi; ++y) for (int x = 0; x < wi; ++x) if (d[y * wi + x] > 0 && d[y * wi + x] >= near) { px += x; py += y; n += 1; }
    if (n == 0) { img.fill(lut[0]); return; }
    px /= n; py /= n;
    const QPointF c0 = handleToDoc(o, {0.5, 0.5});
    const double qx = px + (f.o.x() - c0.x()) / sx, qy = py + (f.o.y() - c0.y()) / sy;
    // default frame A0 (columns u0, v0) and the current frame A (columns u, v), in doc units
    const QPointF u0 = handleToDoc(o, {1, 0.5}) - c0, v0 = handleToDoc(o, {0.5, 0}) - c0;
    // A^-1
    const double ia = f.v.y() / f.det, ib = -f.v.x() / f.det, ic = -f.u.y() / f.det, id = f.u.x() / f.det;
    // M = S * A0 * A^-1 * S^-1 with S = diag(1/sx, 1/sy)
    const double a00 = u0.x() * ia + v0.x() * ic, a01 = u0.x() * ib + v0.x() * id;
    const double a10 = u0.y() * ia + v0.y() * ic, a11 = u0.y() * ib + v0.y() * id;
    const double m00 = a00, m01 = a01 * sy / sx, m10 = a10 * sx / sy, m11 = a11;
    const double Dmax = qMax(1.f, mx);
    for (int y = 0; y < hi; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < wi; ++x) {
            const double D = d[y * wi + x];
            if (D <= 0) { line[x] = lut[0]; continue; }
            const double ex = x - qx, ey = y - qy;
            const double rq = std::hypot(ex, ey), rp = std::hypot(x - px, y - py);
            const double rest = qMax(0.0, Dmax - D);
            const double K = qMax(rest + rq - rp, 0.15 * qMin(rest, rq));
            const double sigma = rq < 1e-9 ? 1.0 : std::hypot(m00 * ex + m01 * ey, m10 * ex + m11 * ey) / rq;
            const double den = D + sigma * K;
            const double t = den <= 0 ? 1.0 : D / den;
            line[x] = lut[qBound(0, int(t * 255 + 0.5), 255)];
        }
    }
}

QImage gradientImage(const VObject &o, const QRectF &R, int wi, int hi, const QPainterPath &path) {
    static thread_local QCache<QString, QImage> cache(48 * 1024);
    const Frame f = frameOf(o);
    QString key = QStringLiteral("%1|%2|%3,%4,%5,%6,%7,%8|%9,%10,%11,%12|%13x%14")
                      .arg(int(o.fillKind)).arg(stopsKey(effectiveGradient(o)))
                      .arg(f.o.x()).arg(f.o.y()).arg(f.u.x()).arg(f.u.y()).arg(f.v.x()).arg(f.v.y())
                      .arg(R.x()).arg(R.y()).arg(R.width()).arg(R.height()).arg(wi).arg(hi);
    if (o.fillKind == FillKind::Contour) {
        double h = 0;
        for (int i = 0; i < path.elementCount(); ++i) h += path.elementAt(i).x * (i + 1) * 0.37 + path.elementAt(i).y * (i + 3) * 0.61;
        key += QStringLiteral("|c%1").arg(h, 0, 'g', 12);
    }
    if (QImage *c = cache.object(key)) return *c;
    const QVector<QRgb> lut = lutOf(effectiveGradient(o));
    QImage img(wi, hi, QImage::Format_ARGB32_Premultiplied);
    const double sx = R.width() / wi, sy = R.height() / hi;
    if (o.fillKind == FillKind::Contour) {
        // distance from the outline (two-pass chamfer), normalised to the deepest point
        QImage mask(wi, hi, QImage::Format_Grayscale8);
        mask.fill(0);
        { QPainter mp(&mask); mp.scale(1 / sx, 1 / sy); mp.translate(-R.topLeft()); mp.fillPath(path, Qt::white); }
        QVector<float> d(wi * hi);
        const float INF = 1e9f;
        for (int y = 0; y < hi; ++y) for (int x = 0; x < wi; ++x) d[y * wi + x] = mask.constScanLine(y)[x] > 127 ? INF : 0;
        auto at = [&](int x, int y) { return (x < 0 || y < 0 || x >= wi || y >= hi) ? 0.f : d[y * wi + x]; };
        for (int y = 0; y < hi; ++y) for (int x = 0; x < wi; ++x) {
            float &v = d[y * wi + x];
            if (v == 0) continue;
            v = qMin(v, qMin(qMin(at(x - 1, y) + 1, at(x, y - 1) + 1), qMin(at(x - 1, y - 1) + 1.4142f, at(x + 1, y - 1) + 1.4142f)));
        }
        float mx = 0;
        for (int y = hi - 1; y >= 0; --y) for (int x = wi - 1; x >= 0; --x) {
            float &v = d[y * wi + x];
            if (v == 0) continue;
            v = qMin(v, qMin(qMin(at(x + 1, y) + 1, at(x, y + 1) + 1), qMin(at(x + 1, y + 1) + 1.4142f, at(x - 1, y + 1) + 1.4142f)));
            mx = qMax(mx, v);
        }
        if (contourHandlesDefault(o)) {
            for (int y = 0; y < hi; ++y) {
                QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
                for (int x = 0; x < wi; ++x) line[x] = lut[qBound(0, int(d[y * wi + x] / qMax(1.f, mx) * 255 + 0.5f), 255)];
            }
        } else {
            contourFromHandles(o, f, sx, sy, wi, hi, d, mx, lut, img);
        }
    } else {
        for (int y = 0; y < hi; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
            const double dy = R.top() + (y + 0.5) * sy - f.o.y();
            for (int x = 0; x < wi; ++x) {
                const double dx = R.left() + (x + 0.5) * sx - f.o.x();
                const double u = (dx * f.v.y() - dy * f.v.x()) / f.det;
                const double v = (f.u.x() * dy - f.u.y() * dx) / f.det;
                line[x] = lut[qBound(0, int(tFromUV(o.fillKind, u, v) * 255 + 0.5), 255)];
            }
        }
    }
    cache.insert(key, new QImage(img), qMax(1, int(img.sizeInBytes() / 1024)));
    return img;
}

QBrush brushFor(const VObject &o, const QPainterPath &path, double ds) {
    const QRectF b = o.bounds();
    switch (o.fillKind) {
    case FillKind::Solid: return QBrush(o.fill);
    case FillKind::WebDither: {
        auto lo = [](int v) { return v / 51 * 51; };
        auto hi = [](int v) { return qMin(255, (v + 50) / 51 * 51); };
        const QColor c = o.fill;
        QImage t(2, 2, QImage::Format_ARGB32_Premultiplied);
        const QRgb a = qPremultiply(qRgba(lo(c.red()), lo(c.green()), lo(c.blue()), c.alpha()));
        // pick the high colour per channel only where the colour is past the halfway point
        auto pick = [&](int v) { return v - lo(v) > 25 ? hi(v) : lo(v); };
        const QRgb bb = qPremultiply(qRgba(pick(c.red()) == lo(c.red()) ? hi(c.red()) : lo(c.red()),
                                           pick(c.green()) == lo(c.green()) ? hi(c.green()) : lo(c.green()),
                                           pick(c.blue()) == lo(c.blue()) ? hi(c.blue()) : lo(c.blue()), c.alpha()));
        const QRgb p0 = qPremultiply(qRgba(pick(c.red()), pick(c.green()), pick(c.blue()), c.alpha()));
        Q_UNUSED(a);
        t.setPixel(0, 0, p0); t.setPixel(1, 1, p0); t.setPixel(1, 0, bb); t.setPixel(0, 1, bb);
        QBrush br(t);
        br.setTransform(QTransform::fromTranslate(b.left(), b.top()));
        return br;
    }
    case FillKind::Pattern: {
        QBrush br(patternImage(o));
        br.setTransform(patternBrushTransform(o));
        return br;
    }
    default: break;
    }
    QRectF R = path.boundingRect().adjusted(-1, -1, 1, 1);
    if (R.isEmpty()) return QBrush(o.fill);
    double s = qBound(0.05, ds, 4.0);
    const double longest = qMax(R.width(), R.height()) * s;
    if (longest > 1024) s *= 1024 / longest;
    const int wi = qMax(1, qCeil(R.width() * s)), hi = qMax(1, qCeil(R.height() * s));
    QImage img = gradientImage(o, R, wi, hi, path);
    QBrush br(img);
    br.setTransform(QTransform(R.width() / wi, 0, 0, R.height() / hi, R.left(), R.top()));
    return br;
}
} // namespace


// ---------------------------------------------------------------- patterns & textures
QStringList patternNames() {
    return {QStringLiteral("Berber"), QStringLiteral("Bricks"), QStringLiteral("Checkerboard"), QStringLiteral("Denim"),
            QStringLiteral("Diamonds"), QStringLiteral("Dots"), QStringLiteral("Grid"), QStringLiteral("Leopard"),
            QStringLiteral("Marble"), QStringLiteral("Scales"), QStringLiteral("Stripes"), QStringLiteral("Weave"),
            QStringLiteral("Wood")};
}

QImage patternTile(const QString &name) {
    static QMutex mutex;   // also used from the Optimize estimate's worker thread
    static QHash<QString, QImage> cache;
    {
        QMutexLocker lock(&mutex);
        auto it = cache.find(name);
        if (it != cache.end()) return *it;
    }
    const int S = 64;
    QImage t(S, S, QImage::Format_ARGB32_Premultiplied);
    t.fill(Qt::white);
    QPainter p(&t);
    p.setRenderHint(QPainter::Antialiasing);
    if (name == QLatin1String("Bricks")) {
        t.fill(QColor(0xc8, 0xc0, 0xb4));
        for (int row = 0; row < 4; ++row)
            for (int col = -1; col < 2; ++col) {
                QRectF r(col * 32 + (row % 2) * 16 + 1, row * 16 + 1, 30, 14);
                p.fillRect(r, QColor::fromHsv(8 + (row * 7 + col * 3) % 10, 170 + (row * 13 + col * 5) % 40, 150 + (row * 11 + col * 17) % 40));
            }
    } else if (name == QLatin1String("Checkerboard")) {
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) p.fillRect(x * 16, y * 16, 16, 16, (x + y) % 2 ? QColor(0x33, 0x33, 0x33) : QColor(0xee, 0xee, 0xee));
    } else if (name == QLatin1String("Diamonds")) {
        t.fill(QColor(0x33, 0x55, 0x99));
        p.setBrush(QColor(0x99, 0xbb, 0xee)); p.setPen(Qt::NoPen);
        for (int y = -1; y < 3; ++y) for (int x = -1; x < 3; ++x) {
            QPointF c(x * 32 + 16 + ((y & 1) ? 16 : 0), y * 32 + 16);
            Q_UNUSED(c);
        }
        for (QPointF c : {QPointF(16, 16), QPointF(48, 48), QPointF(48, 16), QPointF(16, 48)}) {
            QPolygonF d({c + QPointF(0, -14), c + QPointF(14, 0), c + QPointF(0, 14), c + QPointF(-14, 0)});
            p.drawPolygon(d);
        }
    } else if (name == QLatin1String("Dots")) {
        t.fill(QColor(0xff, 0xee, 0xcc));
        p.setBrush(QColor(0xcc, 0x33, 0x33)); p.setPen(Qt::NoPen);
        for (QPointF c : {QPointF(16, 16), QPointF(48, 48), QPointF(48, 16), QPointF(16, 48)}) p.drawEllipse(c, 7, 7);
    } else if (name == QLatin1String("Grid")) {
        p.setPen(QPen(QColor(0x66, 0x88, 0xbb), 1));
        for (int i = 0; i < S; i += 8) { p.drawLine(QPointF(i + 0.5, 0), QPointF(i + 0.5, S)); p.drawLine(QPointF(0, i + 0.5), QPointF(S, i + 0.5)); }
    } else if (name == QLatin1String("Scales")) {
        t.fill(QColor(0x22, 0x66, 0x44));
        p.setPen(QPen(QColor(0x88, 0xdd, 0x99), 2));
        p.setBrush(QColor(0x33, 0x88, 0x55));
        for (int y = -1; y < 5; ++y) for (int x = -1; x < 5; ++x) p.drawEllipse(QPointF(x * 16 + ((y & 1) ? 8 : 0), y * 16), 10, 10);
    } else if (name == QLatin1String("Stripes")) {
        for (int i = 0; i < S; i += 16) p.fillRect(0, i, S, 8, QColor(0x33, 0x66, 0xcc));
    } else if (name == QLatin1String("Weave")) {
        t.fill(QColor(0x8b, 0x6b, 0x3d));
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
            QRectF r(x * 16 + 1, y * 16 + 1, 14, 14);
            QLinearGradient g(r.topLeft(), (x + y) % 2 ? r.topRight() : r.bottomLeft());   // alternate tiles run across / down
            g.setColorAt(0, QColor(0xe8, 0xd0, 0x9a)); g.setColorAt(0.5, QColor(0xc8, 0xa8, 0x6a)); g.setColorAt(1, QColor(0xa8, 0x88, 0x4a));
            p.fillRect(r, g);
        }
    } else {
        p.end();
        for (int y = 0; y < S; ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(t.scanLine(y));
            for (int x = 0; x < S; ++x) {
                QRgb c;
                if (name == QLatin1String("Wood")) {
                    double n = fbm(x, y, S, 2, 7, 3);
                    double ring = tri(x / 6.0 + n * 6);
                    c = mix(qRgb(0x8a, 0x55, 0x2a), qRgb(0xc8, 0x8a, 0x4c), ring);
                } else if (name == QLatin1String("Marble")) {
                    double n = fbm(x, y, S, 4, 3, 4);
                    double v = std::abs(std::sin((x + y) * M_PI / S * 2 + n * 8));
                    c = mix(qRgb(0x55, 0x55, 0x60), qRgb(0xf2, 0xf0, 0xec), std::pow(v, 0.35));
                } else if (name == QLatin1String("Denim")) {
                    double n = fbm(x, y, S, 8, 11, 2);
                    double tw = ((x + y) % 4 < 2) ? 0.15 : 0;
                    c = mix(qRgb(0x1c, 0x2e, 0x55), qRgb(0x6a, 0x84, 0xb4), n * 0.8 + tw);
                } else if (name == QLatin1String("Leopard")) {
                    double n = fbm(x, y, S, 4, 21, 2);
                    c = n > 0.62 ? qRgb(0x2a, 0x1a, 0x0a) : (n > 0.55 ? qRgb(0x8a, 0x55, 0x1a) : qRgb(0xe0, 0xb0, 0x60));
                } else { // Berber
                    double n = fbm(x, y, S, 16, 5, 2);
                    c = mix(qRgb(0x9a, 0x88, 0x70), qRgb(0xe8, 0xdc, 0xc4), n);
                }
                line[x] = c;
            }
        }
    }
    QMutexLocker lock(&mutex);
    cache.insert(name, t);
    return t;
}

QStringList textureNames() {
    return {QStringLiteral("Burlap"), QStringLiteral("Canvas"), QStringLiteral("Grain"), QStringLiteral("Grid"),
            QStringLiteral("Line-Diagonal"), QStringLiteral("Line-Horizontal"), QStringLiteral("Line-Vertical"),
            QStringLiteral("Mesh"), QStringLiteral("Noise"), QStringLiteral("Parchment"), QStringLiteral("Swirl"),
            QStringLiteral("Wood")};
}

QImage textureTile(const QString &name) {
    static QMutex mutex;   // also used from the Optimize estimate's worker thread
    static QHash<QString, QImage> cache;
    {
        QMutexLocker lock(&mutex);
        auto it = cache.find(name);
        if (it != cache.end()) return *it;
    }
    const int S = 64;
    QImage t(S, S, QImage::Format_Grayscale8);
    for (int y = 0; y < S; ++y) {
        uchar *line = t.scanLine(y);
        for (int x = 0; x < S; ++x) {
            double v; // 1 = white (no darkening)
            if (name == QLatin1String("Burlap")) v = 0.35 + 0.65 * (((x / 4 + y / 4) % 2) ? tri(y / 2.0) : tri(x / 2.0)) * (0.7 + 0.3 * fbm(x, y, S, 8, 2, 2));
            else if (name == QLatin1String("Canvas")) v = 0.55 + 0.45 * qMax(tri(x / 1.5), tri(y / 1.5)) * fbm(x, y, S, 16, 4, 1);
            else if (name == QLatin1String("Grain")) v = 0.3 + 0.7 * hash2(x, y, 9);
            else if (name == QLatin1String("Grid")) v = (x % 8 == 0 || y % 8 == 0) ? 0 : 1;
            else if (name == QLatin1String("Line-Diagonal")) v = ((x + y) % 6 < 2) ? 0 : 1;
            else if (name == QLatin1String("Line-Horizontal")) v = (y % 4 < 1) ? 0 : 1;
            else if (name == QLatin1String("Line-Vertical")) v = (x % 4 < 1) ? 0 : 1;
            else if (name == QLatin1String("Mesh")) v = (tri(x / 2.0) * tri(y / 2.0) > 0.5) ? 1 : 0.2;
            else if (name == QLatin1String("Parchment")) v = 0.45 + 0.55 * fbm(x, y, S, 4, 17, 5);
            else if (name == QLatin1String("Swirl")) { double a = std::atan2(y - 32.0, x - 32.0), r = std::hypot(x - 32.0, y - 32.0); v = tri(r / 3 + a * 2); }
            else if (name == QLatin1String("Wood")) v = 0.3 + 0.7 * tri(x / 4.0 + fbm(x, y, S, 2, 7, 3) * 5);
            else v = fbm(x, y, S, 16, 13, 3); // Noise
            line[x] = uchar(qBound(0, int(v * 255 + 0.5), 255));
        }
    }
    QMutexLocker lock(&mutex);
    cache.insert(name, t);
    return t;
}

namespace {
QImage texTileFor(const QString &name, const QString &procJson) {
    return (name == QLatin1String("Procedural") && !procJson.isEmpty()) ? proc::textureTile(procJson) : textureTile(name);
}
// multiply the (premultiplied) buffer `img`, covering `area` at sx/sy px per unit, by a tiling
// grayscale texture anchored at `org`
void multiplyTexture(QImage &img, const QRectF &area, double sx, double sy, QPointF org, const QImage &tt, int amount) {
    if (tt.isNull() || amount <= 0) return;
    const double amt = amount / 100.0;
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        const double dy = area.top() + (y + 0.5) / sy - org.y();
        const int ty = ((int(std::floor(dy)) % tt.height()) + tt.height()) % tt.height();
        const uchar *tl = tt.constScanLine(ty);
        for (int x = 0; x < img.width(); ++x) {
            if (!qAlpha(line[x])) continue;
            const double dx = area.left() + (x + 0.5) / sx - org.x();
            const int tx = ((int(std::floor(dx)) % tt.width()) + tt.width()) % tt.width();
            const double f = 1 - amt * (1 - tl[tx] / 255.0);
            const QRgb c = line[x];
            line[x] = qRgba(int(qRed(c) * f), int(qGreen(c) * f), int(qBlue(c) * f), qAlpha(c));
        }
    }
}
}

void fillShape(QPainter &p, const VObject &o, const QPainterPath &path) {
    const double ds = deviceScale(p.transform());
    const bool tex = !o.texture.isEmpty() && o.textureAmount > 0;
    if (o.feather <= 0 && !tex) {
        p.fillPath(path, brushFor(o, path, ds));
        return;
    }
    // render into a private buffer so the texture multiplies only the fill and the feather blurs it
    const double m = o.feather * 2 + 2;
    QRectF area = path.boundingRect().adjusted(-m, -m, m, m);
    double s = qBound(0.05, ds, 4.0);
    const double longest = qMax(area.width(), area.height()) * s;
    if (longest > 4096) s *= 4096 / longest;
    const int wi = qMax(1, qCeil(area.width() * s)), hi = qMax(1, qCeil(area.height() * s));
    QImage img(wi, hi, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    const double sx = wi / area.width(), sy = hi / area.height();
    {
        QPainter ip(&img);
        ip.setRenderHint(QPainter::Antialiasing, o.antialias || o.feather > 0);
        ip.setRenderHint(QPainter::SmoothPixmapTransform);
        ip.scale(sx, sy);
        ip.translate(-area.topLeft());
        ip.fillPath(path, brushFor(o, path, s));
    }
    if (tex) multiplyTexture(img, area, sx, sy, o.bounds().topLeft(), texTileFor(o.texture, o.textureProc), o.textureAmount);
    if (o.feather > 0) fx::blur(img, o.feather * s / 2);
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(area, img);
    p.restore();
}

// ---------------------------------------------------------------- strokes

static stroke::PaintInput strokeInput(const VObject &o, bool closedShape)
{
    stroke::PaintInput in;
    in.params = o.strokeCustom ? o.strokeParams : stroke::paramsFor(o.strokeKind);
    in.width = o.strokeWidth;
    in.color = o.stroke;
    in.closedShape = closedShape;
    in.align = o.strokeAlign;
    in.sharpRect = o.type == ObjType::Rect && o.roundness <= 0;
    in.seed = o.strokeSeed ? o.strokeSeed
              : (quint32(o.id * 2654435761ull) ^ quint32(qHash(o.strokeKind)));
    const int n = o.points.size();
    const bool hasP = o.ptPressure.size() == n && n > 0;
    const bool hasS = o.ptSpeed.size() == n && n > 0;
    const bool hasT = o.ptTime.size() == n && n > 0;
    if (hasP || hasS || hasT) {
        in.sampleDist.resize(n);
        in.samplePressure.resize(n);
        in.sampleSpeed.resize(n);
        if (hasT) in.sampleTime.resize(n);
        double dist = 0;
        in.sampleDist[0] = 0;
        in.samplePressure[0] = hasP ? o.pointPressure(0) : 1.0;
        in.sampleSpeed[0] = hasS ? o.pointSpeed(0) : 1.0;
        if (hasT) in.sampleTime[0] = o.ptTime[0];
        for (int i = 1; i < n; ++i) {
            dist += QLineF(o.points[i - 1], o.points[i]).length();
            in.sampleDist[i] = dist;
            in.samplePressure[i] = hasP ? o.pointPressure(i) : 1.0;
            in.sampleSpeed[i] = hasS ? o.pointSpeed(i) : 1.0;
            if (hasT) in.sampleTime[i] = o.ptTime[i];
        }
    }
    return in;
}

const QVector<StrokePreset> &strokePresets() {
    static QVector<StrokePreset> v = [] {
        QVector<StrokePreset> r;
        for (const auto &p : stroke::presets()) r.append({p.key, p.category, p.name});
        return r;
    }();
    return v;
}
QStringList strokeCategories() { return stroke::categories(); }
QStringList strokeNamesInCategory(const QString &category) { return stroke::namesInCategory(category); }
QString strokeKey(const QString &category, const QString &name) { return stroke::keyFor(category, name); }
QString resolveStrokeKind(const QString &keyOrCategory) { return stroke::canonicalKey(keyOrCategory); }
double strokeExtent(const VObject &o) {
    const stroke::Params p = o.strokeCustom ? o.strokeParams : stroke::paramsFor(o.strokeKind);
    return stroke::extent(p, o.strokeWidth);
}

// Path Scrubber data (v0.3.1): the stroke is drawn at its widest (every stroke kind, textures and
// all) into a buffer, then masked by a sweep of discs whose radius and alpha follow the per-anchor
// size and opacity along the path, so the stroke tapers and fades smoothly.
static void strokeWithPressure(QPainter &p, const VObject &o, const QPainterPath &path0, bool closedShape) {
    const double maxS = o.maxPointSize();
    double any = 0;
    for (int i = 0; i < o.points.size(); ++i) any = qMax(any, o.pointSize(i) * o.pointOpacity(i));
    if (any <= 0) return;
    VObject wide = o;
    wide.clearPointPressure();
    wide.strokeWidth = o.strokeWidth * maxS;
    const bool aligned = closedShape && o.strokeAlign != 0;
    const double half = (aligned ? o.strokeWidth : o.strokeWidth / 2) + strokeExtent(o);
    const double m = wide.strokeWidth * 2 + strokeExtent(wide) * 2 + 4;
    const QRectF area = path0.boundingRect().adjusted(-m, -m, m, m);
    const QTransform dt = p.deviceTransform();
    double sc = qBound(0.05, std::sqrt(std::abs(dt.determinant())), 4.0);
    const double longest = qMax(area.width(), area.height()) * sc;
    if (longest > 4096) sc *= 4096 / longest;
    const int wi = qMax(1, qCeil(area.width() * sc)), hi = qMax(1, qCeil(area.height() * sc));
    QImage img(wi, hi, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    QImage mask(wi, hi, QImage::Format_ARGB32_Premultiplied);
    mask.fill(0);
    const double sx = wi / area.width(), sy = hi / area.height();
    {
        QPainter ip(&img);
        ip.setRenderHints(p.renderHints());
        ip.scale(sx, sy);
        ip.translate(-area.topLeft());
        strokeShape(ip, wide, path0, closedShape);
    }
    {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing);
        mp.scale(sx, sy);
        mp.translate(-area.topLeft());
        mp.setCompositionMode(QPainter::CompositionMode_Lighten);
        mp.setPen(Qt::NoPen);
        const double aa = 0.6 / qMax(0.05, sc);
        const auto all = paths::pressureSamples(o, qBound(0.25, half / 3, 4.0));
        for (const auto &cs : all) {
            auto rad = [&](const paths::PressureSample &s) { return s.size > 0 ? s.size * half + aa * qMin(1.0, s.size * 4) : 0.0; };
            auto col = [&](double a) { const int g = qRound(qBound(0.0, a, 1.0) * 255); return QColor(g, g, g); };
            for (int k = 0; k < cs.size(); ++k) {
                const double r = rad(cs[k]);
                if (r <= 0 || cs[k].opacity <= 0) continue;
                mp.setBrush(col(cs[k].opacity));
                mp.drawEllipse(cs[k].p, r, r);
            }
            for (int k = 0; k + 1 < cs.size(); ++k) {
                const QPointF d = cs[k + 1].p - cs[k].p;
                const double l = std::hypot(d.x(), d.y());
                if (l < 1e-9) continue;
                const QPointF n(-d.y() / l, d.x() / l);
                const double ra = rad(cs[k]), rb = rad(cs[k + 1]);
                const double op = qMin(cs[k].opacity, cs[k + 1].opacity);
                if ((ra <= 0 && rb <= 0) || op <= 0) continue;
                mp.setBrush(col(op));
                mp.drawPolygon(QPolygonF({cs[k].p + n * ra, cs[k + 1].p + n * rb, cs[k + 1].p - n * rb, cs[k].p - n * ra}));
            }
        }
    }
    for (int y = 0; y < hi; ++y) {
        auto *row = reinterpret_cast<QRgb *>(mask.scanLine(y));
        for (int x = 0; x < wi; ++x) { const int a = qRed(row[x]); row[x] = qRgba(a, a, a, a); }
    }
    {
        QPainter ip(&img);
        ip.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        ip.drawImage(0, 0, mask);
    }
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(area, img);
    p.restore();
}

void strokeShape(QPainter &p, const VObject &o, const QPainterPath &path0, bool closedShape) {
    if (!o.strokeOn || o.strokeWidth <= 0) return;
    if (o.hasPointPressure()) { strokeWithPressure(p, o, path0, closedShape); return; }
    const QString tex = !o.strokeTexture.isEmpty() ? o.strokeTexture
                      : (o.strokeCustom ? o.strokeParams.texture : stroke::paramsFor(o.strokeKind).texture);
    const int texAmt = o.strokeTextureAmount > 0 ? o.strokeTextureAmount
                      : (o.strokeCustom ? o.strokeParams.textureAmount : stroke::paramsFor(o.strokeKind).textureAmount);
    if (!tex.isEmpty() && texAmt > 0) {
        const QImage tt = texTileFor(tex, o.strokeTextureProc);
        VObject plain = o;
        plain.strokeTexture.clear();
        plain.strokeTextureAmount = 0;
        if (plain.strokeCustom) { plain.strokeParams.texture.clear(); plain.strokeParams.textureAmount = 0; }
        // Skip factory texture when we already multiply it here: temporarily paint without the
        // preset texture by using a custom copy with texture cleared.
        if (!plain.strokeCustom) {
            plain.strokeCustom = true;
            plain.strokeParams = stroke::paramsFor(o.strokeKind);
            plain.strokeParams.texture.clear();
            plain.strokeParams.textureAmount = 0;
        }
        const double m = o.strokeWidth * 2 + strokeExtent(o) * 2 + 4;
        const QRectF area = path0.boundingRect().adjusted(-m, -m, m, m);
        const QTransform dt = p.deviceTransform();
        double s = qBound(0.05, std::sqrt(std::abs(dt.determinant())), 4.0);
        const double longest = qMax(area.width(), area.height()) * s;
        if (longest > 4096) s *= 4096 / longest;
        const int wi = qMax(1, qCeil(area.width() * s)), hi = qMax(1, qCeil(area.height() * s));
        QImage img(wi, hi, QImage::Format_ARGB32_Premultiplied);
        img.fill(0);
        const double sx = wi / area.width(), sy = hi / area.height();
        {
            QPainter ip(&img);
            ip.setRenderHints(p.renderHints());
            ip.scale(sx, sy);
            ip.translate(-area.topLeft());
            strokeShape(ip, plain, path0, closedShape);
        }
        multiplyTexture(img, area, sx, sy, o.bounds().topLeft(), tt, texAmt);
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(area, img);
        p.restore();
        return;
    }
    stroke::paint(p, path0, strokeInput(o, closedShape));
}

} // namespace fills
