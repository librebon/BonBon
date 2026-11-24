#include "Brand.h"
// SVG import into vector objects and SVG export of the document.
//
// Import maps rect, circle, ellipse, line, polyline, polygon, path (every command, arcs become cubic
// Beziers), text / tspan, image (data URIs and relative files), groups with transforms and opacity,
// fill / stroke colours and opacities, linear / radial gradients, presentation attributes, inline
// style="" and simple <style> rules (element, .class and #id selectors). Top-level groups become
// layers; nested groups become group objects (0.3.2). Files that use features the object model
// cannot express (use, clipPath, mask, filter, pattern, marker, foreignObject, textPath) are
// imported as one bitmap rendered by QtSvg.
//
// Export writes real SVG elements for rectangles, ellipses, lines, paths and text with solid, linear
// or radial fills and plain strokes; anything else (live effects, other fill categories, textures,
// feathered edges, brush strokes, symbol instances) is embedded as a PNG <image> rendered exactly as
// on the canvas, so the SVG always looks like the document.
#include "DocIO.h"
#include "core/AutoShape.h"
#include "core/Document.h"
#include "core/Fills.h"
#include "core/Paths.h"
#include "core/Render.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <zlib.h>

namespace docio {
using codec::setErr;

bool isGzip(const QByteArray &d) { return d.size() > 2 && uchar(d[0]) == 0x1F && uchar(d[1]) == 0x8B; }

QByteArray gzip(const QByteArray &d) {
    z_stream zs{};
    if (deflateInit2(&zs, 9, Z_DEFLATED, 15 + 16, 9, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    QByteArray out(int(deflateBound(&zs, uLong(d.size()))) + 32, 0);
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(d.constData()));
    zs.avail_in = uInt(d.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    out.resize(int(zs.total_out));
    deflateEnd(&zs);
    return rc == Z_STREAM_END ? out : QByteArray();
}

QByteArray gunzip(const QByteArray &d, bool *ok) {
    if (ok) *ok = false;
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return {};
    QByteArray out;
    char buf[65536];
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(d.constData()));
    zs.avail_in = uInt(d.size());
    int rc;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof buf;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        out.append(buf, int(sizeof buf - zs.avail_out));
        if (out.size() > (256 << 20)) { rc = Z_MEM_ERROR; break; }   // zip-bomb guard
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    if (ok) *ok = rc == Z_STREAM_END;
    return out;
}

namespace {
// Libreworks' private SVG namespace (live Auto Shape parameters on exported <path>s)
const QString kLwNs = QStringLiteral("https://libreworks.org/ns/svg/1");
// ======================================================================= import helpers
struct Grad {
    bool radial = false;
    bool userSpace = false;
    double x1 = 0, y1 = 0, x2 = 1, y2 = 0;
    double cx = 0.5, cy = 0.5, r = 0.5, fx = 0.5, fy = 0.5;
    bool hasX1 = false, hasY1 = false, hasX2 = false, hasY2 = false, hasCx = false, hasCy = false, hasR = false, hasUnits = false;
    QTransform xf;
    bool hasXf = false;
    QVector<GradStop> stops;
    QString href;
};

struct Style {
    QString fill = QStringLiteral("black"), stroke = QStringLiteral("none");
    double fillOpacity = 1, strokeOpacity = 1, opacity = 1, strokeWidth = 1;
    QString fontFamily = QStringLiteral("Sans Serif");
    double fontSize = 16;
    bool bold = false, italic = false, underline = false;
    QString anchor = QStringLiteral("start");
    QString color = QStringLiteral("black");
    bool display = true, visible = true;
    QString fillRule = QStringLiteral("nonzero");
    QTransform xf;   // accumulated, element -> canvas
    BlendMode blend = BlendMode::Normal; // not inherited; styleFor resets it
};

double parseLength(QString s, double ref = 0, double fontSize = 16) {
    s = s.trimmed();
    if (s.isEmpty()) return 0;
    static const QRegularExpression re(QStringLiteral("^([+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?)\\s*([a-z%]*)$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return 0;
    const double v = m.captured(1).toDouble();
    const QString u = m.captured(2);
    if (u.isEmpty() || u == QLatin1String("px")) return v;
    if (u == QLatin1String("pt")) return v * 96.0 / 72.0;
    if (u == QLatin1String("pc")) return v * 16;
    if (u == QLatin1String("mm")) return v * 96.0 / 25.4;
    if (u == QLatin1String("cm")) return v * 96.0 / 2.54;
    if (u == QLatin1String("in")) return v * 96;
    if (u == QLatin1String("em")) return v * fontSize;
    if (u == QLatin1String("ex")) return v * fontSize / 2;
    if (u == QLatin1String("%")) return v * ref / 100.0;
    return v;
}

QVector<double> numbers(const QString &s) {
    QVector<double> v;
    static const QRegularExpression re(QStringLiteral("[+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?"));
    auto it = re.globalMatch(s);
    while (it.hasNext()) v << it.next().captured(0).toDouble();
    return v;
}

QTransform parseTransform(const QString &s) {
    QTransform t;
    static const QRegularExpression re(QStringLiteral("(matrix|translate|scale|rotate|skewX|skewY)\\s*\\(([^)]*)\\)"));
    auto it = re.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString fn = m.captured(1);
        const QVector<double> a = numbers(m.captured(2));
        QTransform x;
        if (fn == QLatin1String("matrix") && a.size() >= 6) x = QTransform(a[0], a[1], a[2], a[3], a[4], a[5]);
        else if (fn == QLatin1String("translate") && !a.isEmpty()) x.translate(a[0], a.size() > 1 ? a[1] : 0);
        else if (fn == QLatin1String("scale") && !a.isEmpty()) x.scale(a[0], a.size() > 1 ? a[1] : a[0]);
        else if (fn == QLatin1String("rotate") && !a.isEmpty()) {
            if (a.size() >= 3) { x.translate(a[1], a[2]); x.rotate(a[0]); x.translate(-a[1], -a[2]); }
            else x.rotate(a[0]);
        } else if (fn == QLatin1String("skewX") && !a.isEmpty()) x = QTransform(1, 0, std::tan(a[0] * M_PI / 180), 1, 0, 0);
        else if (fn == QLatin1String("skewY") && !a.isEmpty()) x = QTransform(1, std::tan(a[0] * M_PI / 180), 0, 1, 0, 0);
        t = x * t;   // listed outermost first: the last one applies to the point first
    }
    return t;
}

QColor parseColor(QString s, const QString &current, bool *none = nullptr) {
    s = s.trimmed();
    if (none) *none = false;
    if (s.isEmpty() || s == QLatin1String("none") || s == QLatin1String("transparent")) { if (none) *none = true; return {}; }
    if (s == QLatin1String("currentColor")) return parseColor(current, QStringLiteral("black"), none);
    if (s.startsWith(QLatin1String("rgb"))) {
        const QString inner = s.mid(s.indexOf(QLatin1Char('(')) + 1).chopped(1);
        const QStringList parts = inner.split(QRegularExpression(QStringLiteral("[,\\s/]+")), Qt::SkipEmptyParts);
        auto comp = [&](int i) {
            if (i >= parts.size()) return 0;
            const QString p = parts[i];
            if (p.endsWith(QLatin1Char('%'))) return qBound(0, qRound(p.chopped(1).toDouble() * 2.55), 255);
            return qBound(0, qRound(p.toDouble()), 255);
        };
        QColor c(comp(0), comp(1), comp(2));
        if (parts.size() > 3) {
            const QString a = parts[3];
            c.setAlphaF(float(qBound(0.0, a.endsWith(QLatin1Char('%')) ? a.chopped(1).toDouble() / 100 : a.toDouble(), 1.0)));
        }
        return c;
    }
    if (s.startsWith(QLatin1Char('#')) && (s.size() == 5 || s.size() == 9)) {   // #rgba / #rrggbbaa
        QString h = s.mid(1);
        if (h.size() == 4) { QString e; for (QChar c : h) e += QString(2, c); h = e; }
        bool ok;
        const uint v = h.toUInt(&ok, 16);
        if (ok) return QColor(int(v >> 24), int((v >> 16) & 0xFF), int((v >> 8) & 0xFF), int(v & 0xFF));
    }
    QColor c = QColor::fromString(s);
    if (!c.isValid() && none) *none = true;
    return c;
}

void arcToCubic(QPainterPath &p, QPointF from, double rx, double ry, double phiDeg, bool large, bool sweep, QPointF to) {
    if (from == to) return;
    rx = std::abs(rx); ry = std::abs(ry);
    if (rx < 1e-9 || ry < 1e-9) { p.lineTo(to); return; }
    const double phi = phiDeg * M_PI / 180, cp = std::cos(phi), sp = std::sin(phi);
    const double dx = (from.x() - to.x()) / 2, dy = (from.y() - to.y()) / 2;
    const double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    const double lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lam > 1) { rx *= std::sqrt(lam); ry *= std::sqrt(lam); }
    const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double co = den > 0 ? std::sqrt(qMax(0.0, num / den)) : 0;
    if (large == sweep) co = -co;
    const double cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
    const double cx = cp * cxp - sp * cyp + (from.x() + to.x()) / 2, cy = sp * cxp + cp * cyp + (from.y() + to.y()) / 2;
    auto ang = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
    const double th1 = ang(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
    double dth = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
    if (!sweep && dth > 0) dth -= 2 * M_PI;
    else if (sweep && dth < 0) dth += 2 * M_PI;
    const int segs = qMax(1, int(std::ceil(std::abs(dth) / (M_PI / 2) - 1e-9)));
    const double d = dth / segs;
    const double k = 4.0 / 3.0 * std::tan(d / 4);
    double t = th1;
    auto pt = [&](double a) { const double ex = rx * std::cos(a), ey = ry * std::sin(a); return QPointF(cp * ex - sp * ey + cx, sp * ex + cp * ey + cy); };
    auto der = [&](double a) { const double ex = -rx * std::sin(a), ey = ry * std::cos(a); return QPointF(cp * ex - sp * ey, sp * ex + cp * ey); };
    for (int i = 0; i < segs; ++i) {
        const double t2 = t + d;
        const QPointF p1 = pt(t), p2 = i == segs - 1 ? to : pt(t2);
        p.cubicTo(p1 + der(t) * k, p2 - der(t2) * k, p2);
        t = t2;
    }
}

QPainterPath parsePathData(const QString &d) {
    QPainterPath p;
    int i = 0;
    const int n = d.size();
    QChar cmd, prevCmd;
    QPointF cur, start, lastCtrl;
    auto skipWs = [&]() { while (i < n && (d[i].isSpace() || d[i] == QLatin1Char(','))) ++i; };
    auto num = [&](bool *ok) -> double {
        skipWs();
        const int s = i;
        if (i < n && (d[i] == QLatin1Char('+') || d[i] == QLatin1Char('-'))) ++i;
        bool dot = false, digits = false;
        while (i < n && (d[i].isDigit() || (!dot && d[i] == QLatin1Char('.')))) { if (d[i] == QLatin1Char('.')) dot = true; else digits = true; ++i; }
        if (digits && i < n && (d[i] == QLatin1Char('e') || d[i] == QLatin1Char('E'))) {
            const int save = i; ++i;
            if (i < n && (d[i] == QLatin1Char('+') || d[i] == QLatin1Char('-'))) ++i;
            if (i < n && d[i].isDigit()) { while (i < n && d[i].isDigit()) ++i; } else i = save;
        }
        *ok = digits;
        return digits ? QStringView(d).mid(s, i - s).toDouble() : 0;
    };
    auto flag = [&](bool *ok) -> bool {
        skipWs();
        if (i < n && (d[i] == QLatin1Char('0') || d[i] == QLatin1Char('1'))) { *ok = true; return d[i++] == QLatin1Char('1'); }
        *ok = false; return false;
    };
    while (true) {
        skipWs();
        if (i >= n) break;
        if (d[i].isLetter() && d[i] != QLatin1Char('e') && d[i] != QLatin1Char('E')) cmd = d[i++];
        else if (cmd.isNull()) break;
        const bool rel = cmd.isLower();
        const QChar C = cmd.toUpper();
        bool ok = true;
        if (C == QLatin1Char('Z')) { p.closeSubpath(); cur = start; prevCmd = C; cmd = QChar(); continue; }
        auto P = [&](double x, double y) { return rel ? cur + QPointF(x, y) : QPointF(x, y); };
        if (C == QLatin1Char('M')) {
            const double x = num(&ok), y = ok ? num(&ok) : 0; if (!ok) break;
            cur = start = P(x, y); p.moveTo(cur);
            cmd = rel ? QLatin1Char('l') : QLatin1Char('L');
            lastCtrl = cur;
        } else if (C == QLatin1Char('L')) {
            const double x = num(&ok), y = ok ? num(&ok) : 0; if (!ok) break;
            cur = P(x, y); p.lineTo(cur); lastCtrl = cur;
        } else if (C == QLatin1Char('H')) {
            const double x = num(&ok); if (!ok) break;
            cur = QPointF(rel ? cur.x() + x : x, cur.y()); p.lineTo(cur); lastCtrl = cur;
        } else if (C == QLatin1Char('V')) {
            const double y = num(&ok); if (!ok) break;
            cur = QPointF(cur.x(), rel ? cur.y() + y : y); p.lineTo(cur); lastCtrl = cur;
        } else if (C == QLatin1Char('C')) {
            double a[6]; for (double &v : a) { v = num(&ok); if (!ok) break; } if (!ok) break;
            const QPointF c1 = P(a[0], a[1]), c2 = P(a[2], a[3]), e = P(a[4], a[5]);
            p.cubicTo(c1, c2, e); lastCtrl = c2; cur = e;
        } else if (C == QLatin1Char('S')) {
            double a[4]; for (double &v : a) { v = num(&ok); if (!ok) break; } if (!ok) break;
            const QPointF c1 = (prevCmd == QLatin1Char('C') || prevCmd == QLatin1Char('S')) ? cur * 2 - lastCtrl : cur;
            const QPointF c2 = P(a[0], a[1]), e = P(a[2], a[3]);
            p.cubicTo(c1, c2, e); lastCtrl = c2; cur = e;
        } else if (C == QLatin1Char('Q')) {
            double a[4]; for (double &v : a) { v = num(&ok); if (!ok) break; } if (!ok) break;
            const QPointF q = P(a[0], a[1]), e = P(a[2], a[3]);
            p.cubicTo(cur + (q - cur) * (2.0 / 3), e + (q - e) * (2.0 / 3), e); lastCtrl = q; cur = e;
        } else if (C == QLatin1Char('T')) {
            double a[2]; for (double &v : a) { v = num(&ok); if (!ok) break; } if (!ok) break;
            const QPointF q = (prevCmd == QLatin1Char('Q') || prevCmd == QLatin1Char('T')) ? cur * 2 - lastCtrl : cur;
            const QPointF e = P(a[0], a[1]);
            p.cubicTo(cur + (q - cur) * (2.0 / 3), e + (q - e) * (2.0 / 3), e); lastCtrl = q; cur = e;
        } else if (C == QLatin1Char('A')) {
            const double rx = num(&ok); if (!ok) break;
            const double ry = num(&ok); if (!ok) break;
            const double rot = num(&ok); if (!ok) break;
            const bool large = flag(&ok); if (!ok) break;
            const bool sweep = flag(&ok); if (!ok) break;
            const double x = num(&ok); if (!ok) break;
            const double y = num(&ok); if (!ok) break;
            const QPointF e = P(x, y);
            arcToCubic(p, cur, rx, ry, rot, large, sweep, e);
            cur = e; lastCtrl = cur;
        } else break;
        prevCmd = C;
    }
    return p;
}

struct Rule { QString sel; QMap<QString, QString> decl; };
QMap<QString, QString> parseDecls(const QString &s) {
    QMap<QString, QString> m;
    for (const QString &kv : s.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const int c = kv.indexOf(QLatin1Char(':'));
        if (c > 0) m.insert(kv.left(c).trimmed(), kv.mid(c + 1).trimmed().remove(QStringLiteral("!important")).trimmed());
    }
    return m;
}

// ======================================================================= importer
class Importer {
public:
    DocState &st;
    std::function<quint64()> newId;
    QString baseDir;
    QStringList notes;
    QHash<QString, Grad> grads;
    QVector<Rule> rules;
    bool unsupported = false;
    QString unsupportedWhat;
    QVector<QVector<VObject>> layerObjs;
    QColor canvas = Qt::transparent;   // <rect id="canvas"> written by Libreworks
    QStringList layerNames;
    QVector<bool> layerVisible;
    double docW = 0, docH = 0;
    QString pendingGrad;
    QTransform pendingGradXf;
    int lwOwnerDepth = -1;   // stack depth of the <path> parts belonging to the last rebuilt Auto Shape
    int lwPressureDepth = -1;   // ... and of the taper outlines of the last path with pressure data
    struct LegacyShape { QString kind, params, rect; int depth = -1; } legacy;   // 0.3.0 <g data-lw-shape>

    Importer(DocState &s, std::function<quint64()> id, QString base) : st(s), newId(std::move(id)), baseDir(std::move(base)) {}

    void flag(const QString &what) {
        if (!unsupported) unsupportedWhat = what;
        unsupported = true;
    }
    // first pass: gradients, CSS rules and features without an object equivalent
    void scan(const QByteArray &data) {
        QXmlStreamReader x(data);
        Grad *g = nullptr;
        bool inStyle = false;
        QString css;
        while (!x.atEnd()) {
            x.readNext();
            if (x.isStartElement()) {
                const QString nm = x.name().toString();
                const auto at = x.attributes();
                static const QStringList bad = {QStringLiteral("use"), QStringLiteral("mask"), QStringLiteral("filter"), QStringLiteral("pattern"),
                                                QStringLiteral("marker"), QStringLiteral("foreignObject"), QStringLiteral("textPath"), QStringLiteral("switch")};
                if (bad.contains(nm)) flag(nm);
                for (const char *a : {"clip-path", "mask", "filter", "marker-start", "marker-mid", "marker-end"}) {
                    const QString v = at.value(QLatin1String(a)).toString();
                    if (!v.isEmpty() && v != QLatin1String("none")) flag(QString::fromLatin1(a));
                }
                const QString style = at.value(QLatin1String("style")).toString();
                for (const char *a : {"clip-path:url", "mask:url", "filter:url", "marker"})
                    if (QString(style).remove(QLatin1Char(' ')).contains(QLatin1String(a))) flag(QString::fromLatin1(a).section(QLatin1Char(':'), 0, 0));
                if (nm == QLatin1String("linearGradient") || nm == QLatin1String("radialGradient")) {
                    const QString id = at.value(QLatin1String("id")).toString();
                    Grad gr;
                    gr.radial = nm == QLatin1String("radialGradient");
                    auto has = [&](const char *k) { return at.hasAttribute(QLatin1String(k)); };
                    auto frac = [&](const char *k, double def) {
                        if (!has(k)) return def;
                        const QString v = at.value(QLatin1String(k)).toString().trimmed();
                        if (v.endsWith(QLatin1Char('%'))) return v.chopped(1).toDouble() / 100.0;
                        return parseLength(v);
                    };
                    gr.hasUnits = has("gradientUnits");
                    gr.userSpace = at.value(QLatin1String("gradientUnits")) == QLatin1String("userSpaceOnUse");
                    gr.hasX1 = has("x1"); gr.hasY1 = has("y1"); gr.hasX2 = has("x2"); gr.hasY2 = has("y2");
                    gr.x1 = frac("x1", 0); gr.y1 = frac("y1", 0); gr.x2 = frac("x2", 1); gr.y2 = frac("y2", 0);
                    gr.hasCx = has("cx"); gr.hasCy = has("cy"); gr.hasR = has("r");
                    gr.cx = frac("cx", 0.5); gr.cy = frac("cy", 0.5); gr.r = frac("r", 0.5);
                    gr.fx = frac("fx", gr.cx); gr.fy = frac("fy", gr.cy);
                    if (has("gradientTransform")) { gr.xf = parseTransform(at.value(QLatin1String("gradientTransform")).toString()); gr.hasXf = true; }
                    gr.href = at.value(QLatin1String("xlink:href")).toString();
                    if (gr.href.isEmpty()) gr.href = at.value(QLatin1String("href")).toString();
                    if (gr.href.startsWith(QLatin1Char('#'))) gr.href = gr.href.mid(1);
                    grads.insert(id, gr);
                    g = &grads[id];
                } else if (nm == QLatin1String("stop") && g) {
                    const QMap<QString, QString> d = parseDecls(at.value(QLatin1String("style")).toString());
                    const QString off = at.value(QLatin1String("offset")).toString().trimmed();
                    double pos = off.endsWith(QLatin1Char('%')) ? off.chopped(1).toDouble() / 100 : off.toDouble();
                    QString col = d.value(QStringLiteral("stop-color"), at.value(QLatin1String("stop-color")).toString());
                    if (col.isEmpty()) col = QStringLiteral("black");
                    const QString op = d.value(QStringLiteral("stop-opacity"), at.value(QLatin1String("stop-opacity")).toString());
                    QColor c = parseColor(col, QStringLiteral("black"));
                    if (!c.isValid()) c = Qt::black;
                    if (!op.isEmpty()) c.setAlphaF(float(qBound(0.0, op.toDouble(), 1.0) * c.alphaF()));
                    pos = qBound(0.0, pos, 1.0);
                    if (!g->stops.isEmpty()) pos = qMax(pos, g->stops.last().pos);
                    g->stops << GradStop{pos, c};
                } else if (nm == QLatin1String("style")) { inStyle = true; css.clear(); }
            } else if ((x.isCharacters() || x.isCDATA()) && inStyle) css += x.text();
            else if (x.isEndElement()) {
                if (x.name() == QLatin1String("linearGradient") || x.name() == QLatin1String("radialGradient")) g = nullptr;
                if (x.name() == QLatin1String("style")) { inStyle = false; parseCss(css); }
            }
        }
        for (int pass = 0; pass < 4; ++pass)   // href chains: inherit stops and unset attributes
            for (auto it = grads.begin(); it != grads.end(); ++it) {
                Grad &gr = it.value();
                if (gr.href.isEmpty() || !grads.contains(gr.href) || gr.href == it.key()) continue;
                const Grad base = grads.value(gr.href);
                if (gr.stops.isEmpty()) gr.stops = base.stops;
                if (!gr.hasUnits && base.hasUnits) { gr.userSpace = base.userSpace; gr.hasUnits = true; }
                if (!gr.hasXf && base.hasXf) { gr.xf = base.xf; gr.hasXf = true; }
                if (!gr.hasX1 && base.hasX1) { gr.x1 = base.x1; gr.hasX1 = true; }
                if (!gr.hasY1 && base.hasY1) { gr.y1 = base.y1; gr.hasY1 = true; }
                if (!gr.hasX2 && base.hasX2) { gr.x2 = base.x2; gr.hasX2 = true; }
                if (!gr.hasY2 && base.hasY2) { gr.y2 = base.y2; gr.hasY2 = true; }
                if (!gr.hasCx && base.hasCx) { gr.cx = base.cx; gr.hasCx = true; }
                if (!gr.hasCy && base.hasCy) { gr.cy = base.cy; gr.hasCy = true; }
                if (!gr.hasR && base.hasR) { gr.r = base.r; gr.hasR = true; }
            }
    }
    void parseCss(QString css) {
        css.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"), QRegularExpression::DotMatchesEverythingOption));
        static const QRegularExpression re(QStringLiteral("([^{}]+)\\{([^}]*)\\}"));
        auto it = re.globalMatch(css);
        while (it.hasNext()) {
            const auto m = it.next();
            const auto decl = parseDecls(m.captured(2));
            for (const QString &sel : m.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString s = sel.trimmed();
                if (s.contains(QLatin1Char(' ')) || s.contains(QLatin1Char('>')) || s.contains(QLatin1Char(':')) || s.contains(QLatin1Char('[')) || s.startsWith(QLatin1Char('@'))) {
                    notes << QStringLiteral("CSS selector '%1' ignored").arg(s);
                    continue;
                }
                rules << Rule{s, decl};
            }
        }
    }
    void applyProps(Style &s, const QMap<QString, QString> &p) {
        for (auto it = p.begin(); it != p.end(); ++it) {
            const QString &k = it.key(), &v = it.value();
            if (v == QLatin1String("inherit")) continue;
            auto frac = [](const QString &str) { return qBound(0.0, str.endsWith(QLatin1Char('%')) ? str.chopped(1).toDouble() / 100 : str.toDouble(), 1.0); };
            if (k == QLatin1String("fill")) s.fill = v;
            else if (k == QLatin1String("stroke")) s.stroke = v;
            else if (k == QLatin1String("fill-opacity")) s.fillOpacity = frac(v);
            else if (k == QLatin1String("stroke-opacity")) s.strokeOpacity = frac(v);
            else if (k == QLatin1String("stroke-width")) s.strokeWidth = parseLength(v, std::hypot(docW, docH) / std::sqrt(2.0), s.fontSize);
            else if (k == QLatin1String("font-family")) {
                QString f = v.split(QLatin1Char(',')).first().trimmed();
                f.remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
                if (f == QLatin1String("sans-serif")) f = QStringLiteral("Sans Serif");
                s.fontFamily = f;
            } else if (k == QLatin1String("font-size")) { const double fs = parseLength(v, s.fontSize, s.fontSize); if (fs > 0) s.fontSize = fs; }
            else if (k == QLatin1String("font-weight")) s.bold = v == QLatin1String("bold") || v == QLatin1String("bolder") || v.toInt() >= 600;
            else if (k == QLatin1String("font-style")) s.italic = v == QLatin1String("italic") || v == QLatin1String("oblique");
            else if (k == QLatin1String("text-decoration")) s.underline = v.contains(QLatin1String("underline"));
            else if (k == QLatin1String("text-anchor")) s.anchor = v;
            else if (k == QLatin1String("color")) s.color = v;
            else if (k == QLatin1String("display")) s.display = v != QLatin1String("none");
            else if (k == QLatin1String("visibility")) s.visible = v != QLatin1String("hidden") && v != QLatin1String("collapse");
            else if (k == QLatin1String("fill-rule")) s.fillRule = v;
            else if (k == QLatin1String("mix-blend-mode")) {
                QString key;
                bool up = false;
                for (QChar ch : v.trimmed()) { if (ch == QLatin1Char('-')) { up = true; continue; } key += up ? ch.toUpper() : ch; up = false; }
                s.blend = blendModeFromKey(key);
            }
        }
    }
    Style styleFor(const QXmlStreamReader &x, const Style &parent) {
        Style s = parent;
        s.opacity = 1;
        s.display = true;
        s.blend = BlendMode::Normal;
        const auto at = x.attributes();
        QMap<QString, QString> pres;
        for (const auto &a : at) pres.insert(a.qualifiedName().toString(), a.value().toString());
        applyProps(s, pres);
        const QString tag = x.name().toString();
        const QStringList classes = at.value(QLatin1String("class")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString id = at.value(QLatin1String("id")).toString();
        for (int pass = 0; pass < 3; ++pass)   // element, then class, then id (specificity order)
            for (const Rule &r : rules) {
                bool match = false;
                if (pass == 0) match = r.sel == tag || r.sel == QLatin1String("*");
                else if (pass == 1 && r.sel.contains(QLatin1Char('.'))) {
                    const QString t = r.sel.section(QLatin1Char('.'), 0, 0), c = r.sel.section(QLatin1Char('.'), 1);
                    match = (t.isEmpty() || t == tag) && classes.contains(c);
                } else if (pass == 2) match = !id.isEmpty() && r.sel == QLatin1Char('#') + id;
                if (match) applyProps(s, r.decl);
            }
        const auto inl = parseDecls(at.value(QLatin1String("style")).toString());
        applyProps(s, inl);
        if (pres.contains(QStringLiteral("opacity"))) s.opacity = qBound(0.0, pres.value(QStringLiteral("opacity")).toDouble(), 1.0);
        if (inl.contains(QStringLiteral("opacity"))) s.opacity = qBound(0.0, inl.value(QStringLiteral("opacity")).toDouble(), 1.0);
        if (at.hasAttribute(QLatin1String("transform"))) s.xf = parseTransform(at.value(QLatin1String("transform")).toString()) * parent.xf;
        return s;
    }

    void paintFrom(VObject &o, const Style &s, const QTransform &geomXf, double strokeScale, double groupOpacity) {
        bool none = false;
        o.fillOn = false;
        o.strokeOn = false;
        const QString f = s.fill.trimmed();
        if (f.startsWith(QLatin1String("url("))) {
            QString id = f.mid(4, f.indexOf(QLatin1Char(')')) - 4).trimmed();
            id.remove(QLatin1Char('#')).remove(QLatin1Char('\'')).remove(QLatin1Char('"'));
            if (grads.contains(id) && !grads[id].stops.isEmpty()) {
                pendingGrad = id;
                pendingGradXf = geomXf;
                o.fillOn = true;
            } else {
                const QString rest = f.mid(f.indexOf(QLatin1Char(')')) + 1).trimmed();
                const QColor c = parseColor(rest.isEmpty() ? QStringLiteral("none") : rest, s.color, &none);
                if (!none) { o.fillOn = true; o.fill = c; }
                if (rest.isEmpty()) notes << QStringLiteral("paint server '%1' not supported: fill dropped").arg(id);
            }
        } else {
            QColor c = parseColor(f, s.color, &none);
            if (!none && c.isValid()) { o.fillOn = true; c.setAlphaF(float(c.alphaF() * s.fillOpacity)); o.fill = c; o.fillKind = FillKind::Solid; }
        }
        const QString sk = s.stroke.trimmed();
        if (s.strokeWidth > 0) {
            QColor c;
            if (sk.startsWith(QLatin1String("url("))) {
                QString id = sk.mid(4, sk.indexOf(QLatin1Char(')')) - 4).trimmed().remove(QLatin1Char('#'));
                if (grads.contains(id) && !grads[id].stops.isEmpty()) { c = grads[id].stops.first().color; notes << QStringLiteral("gradient stroke approximated by a solid colour"); }
            } else c = parseColor(sk, s.color, &none);
            if (c.isValid() && !(none && !sk.startsWith(QLatin1String("url(")))) {
                o.strokeOn = true;
                c.setAlphaF(float(c.alphaF() * s.strokeOpacity));
                o.stroke = c;
                o.strokeWidth = qMax(0.1, s.strokeWidth * strokeScale);
                o.strokeKind = QStringLiteral("basic.hard");
            }
        }
        o.opacity = qBound(0, qRound(s.opacity * groupOpacity * 100), 100);
        o.blend = s.blend;
    }
    // turn the pending SVG gradient into Libreworks handles (needs the object's final geometry)
    void resolveGradient(VObject &o, const Style &s) {
        if (pendingGrad.isEmpty()) return;
        const Grad gr = grads.value(pendingGrad);
        pendingGrad.clear();
        QVector<GradStop> stops = gr.stops;
        for (GradStop &g : stops) g.color.setAlphaF(float(g.color.alphaF() * s.fillOpacity));
        if (stops.size() == 1) stops << GradStop{1.0, stops[0].color};
        if (stops.first().pos > 0) stops.prepend(GradStop{0, stops.first().color});
        if (stops.last().pos < 1) stops.append(GradStop{1, stops.last().color});
        o.gradient = stops;
        o.fillKind = gr.radial ? FillKind::Radial : FillKind::Linear;
        const VObject base = o.untransformed();
        const QRectF bb = base.bounds();
        QTransform toObj;
        if (!gr.userSpace) toObj = QTransform(bb.width(), 0, 0, bb.height(), bb.left(), bb.top());
        else if (!o.hasXform()) toObj = pendingGradXf;   // geometry baked to the canvas
        const QTransform g = (gr.hasXf ? gr.xf : QTransform()) * toObj;
        QPointF P0, P1, P2;
        if (!gr.radial) {
            P0 = g.map(QPointF(gr.x1, gr.y1));
            P1 = g.map(QPointF(gr.x2, gr.y2));
            const QPointF u = P1 - P0;
            P2 = P0 + QPointF(u.y(), -u.x());
        } else {
            P0 = g.map(QPointF(gr.cx, gr.cy));
            P1 = g.map(QPointF(gr.cx + gr.r, gr.cy));
            P2 = g.map(QPointF(gr.cx, gr.cy - gr.r));
            if (std::abs(gr.fx - gr.cx) > 1e-6 || std::abs(gr.fy - gr.cy) > 1e-6) notes << QStringLiteral("radial gradient focal point ignored");
        }
        o.gradP0 = fills::docToHandle(base, P0);
        o.gradP1 = fills::docToHandle(base, P1);
        o.gradP2 = fills::docToHandle(base, P2);
    }
    static double scaleOf(const QTransform &t) { return std::sqrt(std::abs(t.determinant())); }
    // keep simple transforms out of xform (translate / scale are baked into the geometry)
    static void simplify(VObject &o) {
        if (!o.hasXform() || o.type == ObjType::Bitmap) return;
        if (o.xform.type() <= QTransform::TxScale && o.xform.m11() > 0 && o.xform.m22() > 0) {
            const double sc = scaleOf(o.xform);
            const bool uniform = std::abs(o.xform.m11() - o.xform.m22()) < 1e-9;
            if (o.type == ObjType::Text && !uniform) return;
            o.rect = o.xform.mapRect(o.rect);
            o.strokeWidth *= sc;
            if (o.type == ObjType::Text) o.fontSize *= o.xform.m11();
            o.xform = QTransform();
        }
    }
    // nested <g> elements become group objects (v0.3.2)
    struct OpenGroup { VObject g; int depth; bool explicitGroup; };
    QVector<OpenGroup> groups;
    void add(VObject o, int layer) {
        if (o.id == 0) o.id = newId();
        if (!groups.isEmpty()) groups.last().g.children << o;
        else layerObjs[layer] << o;
    }
    void closeGroup(int layer) {
        OpenGroup og = groups.takeLast();
        if (og.g.children.isEmpty()) return;
        if (og.g.children.size() == 1 && !og.explicitGroup && og.g.effects.isEmpty()) {
            // a wrapper <g> around one element (common in other programs' files): no group object
            VObject c = og.g.children[0];
            c.opacity = qRound(c.opacity * og.g.opacity / 100.0);
            if (c.blend == BlendMode::Normal) c.blend = og.g.blend;
            add(c, layer);
            return;
        }
        og.g.id = newId();
        add(og.g, layer);
    }

    bool run(const QByteArray &data, QString *err) {
        scan(data);
        if (unsupported) return true;
        QXmlStreamReader x(data);
        struct Entry { Style s; double op; };
        QVector<Entry> stack;
        int skipDepth = 0;
        int layerDepth = -1;
        int curLayer = 0;
        layerObjs.resize(1); layerNames << QStringLiteral("Layer 1"); layerVisible << true;
        bool defaultUsed = false;
        bool inText = false;
        QString textBuf;
        Style textStyle;
        double textOp = 1;
        QPointF textPos;
        bool textPosSet = false, textStyled = false;
        static const QStringList skipEls = {QStringLiteral("defs"), QStringLiteral("style"), QStringLiteral("metadata"), QStringLiteral("title"),
                                            QStringLiteral("desc"), QStringLiteral("linearGradient"), QStringLiteral("radialGradient"),
                                            QStringLiteral("clipPath"), QStringLiteral("symbol"), QStringLiteral("namedview"), QStringLiteral("script")};
        while (!x.atEnd()) {
            x.readNext();
            if (x.isStartElement()) {
                if (skipDepth) { ++skipDepth; continue; }
                const QString nm = x.name().toString();
                const auto at = x.attributes();
                if (skipEls.contains(nm)) { skipDepth = 1; continue; }
                if (stack.isEmpty()) {
                    if (nm != QLatin1String("svg")) { setErr(err, QStringLiteral("SVG: root element is not <svg>")); return false; }
                    Style root;
                    const QVector<double> vb = numbers(at.value(QLatin1String("viewBox")).toString());
                    const QString ws = at.value(QLatin1String("width")).toString(), hs = at.value(QLatin1String("height")).toString();
                    double w = ws.isEmpty() || ws.endsWith(QLatin1Char('%')) ? (vb.size() == 4 ? vb[2] : 300) : parseLength(ws);
                    double h = hs.isEmpty() || hs.endsWith(QLatin1Char('%')) ? (vb.size() == 4 ? vb[3] : 150) : parseLength(hs);
                    if (w <= 0) w = 300;
                    if (h <= 0) h = 150;
                    docW = w; docH = h;
                    if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) {
                        const QString par = at.value(QLatin1String("preserveAspectRatio")).toString().trimmed();
                        double sx = w / vb[2], sy = h / vb[3];
                        double tx = -vb[0] * sx, ty = -vb[1] * sy;
                        if (!par.startsWith(QLatin1String("none"))) {
                            const double s = par.contains(QLatin1String("slice")) ? qMax(sx, sy) : qMin(sx, sy);
                            const QString al = par.isEmpty() ? QStringLiteral("xMidYMid") : par.section(QLatin1Char(' '), 0, 0);
                            const double fx = al.contains(QLatin1String("xMin")) ? 0 : al.contains(QLatin1String("xMax")) ? 1 : 0.5;
                            const double fy = al.contains(QLatin1String("YMin")) ? 0 : al.contains(QLatin1String("YMax")) ? 1 : 0.5;
                            sx = sy = s;
                            tx = -vb[0] * s + (w - vb[2] * s) * fx;
                            ty = -vb[1] * s + (h - vb[3] * s) * fy;
                        }
                        root.xf = QTransform(sx, 0, 0, sy, tx, ty);
                    }
                    Style s = styleFor(x, root);
                    s.xf = root.xf;
                    stack << Entry{s, s.opacity};
                    continue;
                }
                Style s = styleFor(x, stack.last().s);
                const double parentOp = stack.last().op;
                const bool isGroup = nm == QLatin1String("g") || nm == QLatin1String("a") || nm == QLatin1String("svg");
                const bool topGroup = isGroup && stack.size() == 1 && layerDepth < 0;
                if (!s.display && !topGroup) { skipDepth = 1; continue; }
                if (isGroup) {
                    if (topGroup) {
                        QString name = at.value(QLatin1String("inkscape:label")).toString();
                        if (name.isEmpty()) name = at.value(QLatin1String("data-name")).toString();
                        if (name.isEmpty()) name = at.value(QLatin1String("id")).toString();
                        if (name.isEmpty()) name = QStringLiteral("Layer %1").arg(layerObjs.size() + (defaultUsed ? 1 : 0));
                        const bool vis = s.display && s.visible;
                        if (!defaultUsed && layerObjs.size() == 1 && layerObjs[0].isEmpty()) { layerNames[0] = name; layerVisible[0] = vis; curLayer = 0; defaultUsed = true; }
                        else { layerObjs.resize(layerObjs.size() + 1); layerNames << name; layerVisible << vis; curLayer = int(layerObjs.size()) - 1; }
                        s.display = true; s.visible = true;
                        layerDepth = int(stack.size()) + 1;
                    }
                    if (nm == QLatin1String("svg")) notes << QStringLiteral("nested <svg> viewport not clipped");
                    if (!topGroup && nm == QLatin1String("g") && !at.hasAttribute(QLatin1String("data-lw-shape")) && !at.hasAttribute(kLwNs, QStringLiteral("part"))) {
                        // a group object: its opacity and blend apply to the group as a unit
                        OpenGroup og;
                        og.g.type = ObjType::Group;
                        og.g.name = at.value(QLatin1String("inkscape:label")).toString();
                        if (og.g.name.isEmpty()) og.g.name = at.value(kLwNs, QStringLiteral("name")).toString();
                        og.g.opacity = qBound(0, qRound(s.opacity * 100), 100);
                        QString bm = parseDecls(at.value(QLatin1String("style")).toString()).value(QStringLiteral("mix-blend-mode"));
                        if (bm.isEmpty()) bm = at.value(QLatin1String("mix-blend-mode")).toString();
                        if (!bm.isEmpty()) {
                            QString key;
                            bool up = false;
                            for (QChar ch : bm.trimmed()) { if (ch == QLatin1Char('-')) { up = true; continue; } key += up ? ch.toUpper() : ch; up = false; }
                            og.g.blend = blendModeFromKey(key);
                        }
                        stack << Entry{s, parentOp};
                        og.depth = int(stack.size());
                        og.explicitGroup = at.hasAttribute(kLwNs, QStringLiteral("group"));
                        groups << og;
                        continue;
                    }
                    stack << Entry{s, parentOp * s.opacity};
                    if (nm == QLatin1String("g") && at.hasAttribute(QLatin1String("data-lw-shape")))
                        legacy = LegacyShape{at.value(QLatin1String("data-lw-shape")).toString(), at.value(QLatin1String("data-lw-params")).toString(),
                                             at.value(QLatin1String("data-lw-rect")).toString(), int(stack.size())};
                    continue;
                }
                stack << Entry{s, parentOp};
                if (layerDepth < 0 && curLayer == 0) defaultUsed = true;
                auto L = [&](const char *k, double ref = 0) { return parseLength(at.value(QLatin1String(k)).toString(), ref, s.fontSize); };
                VObject o;
                if (nm == QLatin1String("rect")) {
                    const double w = L("width", docW), h = L("height", docH);
                    if (w <= 0 || h <= 0) continue;
                    double rx = L("rx", docW), ry = L("ry", docH);
                    if (!at.hasAttribute(QLatin1String("rx"))) rx = ry;
                    if (!at.hasAttribute(QLatin1String("ry"))) ry = rx;
                    o.type = ObjType::Rect;
                    o.name = QStringLiteral("Rectangle");
                    o.rect = QRectF(L("x", docW), L("y", docH), w, h);
                    const double rr = qMin(qMax(rx, ry), qMin(w, h) / 2);
                    o.roundness = qBound(0.0, rr / (qMin(w, h) / 2) * 100, 100.0);
                    if (std::abs(rx - ry) > 0.5) notes << QStringLiteral("elliptical rectangle corners made circular");
                    o.xform = s.xf;
                    paintFrom(o, s, QTransform(), 1, parentOp);
                    // the Libreworks canvas rectangle becomes the canvas colour again
                    if (layerDepth < 0 && at.value(QLatin1String("id")) == QLatin1String("canvas") && o.rect == QRectF(0, 0, docW, docH) &&
                        o.xform.isIdentity() && o.fillOn && !o.strokeOn && o.fillKind == FillKind::Solid) {
                        canvas = o.fill;
                        canvas.setAlphaF(canvas.alphaF() * o.opacity / 100.0);
                        continue;
                    }
                } else if (nm == QLatin1String("circle") || nm == QLatin1String("ellipse")) {
                    const double cx = L("cx", docW), cy = L("cy", docH);
                    double rx, ry;
                    if (nm == QLatin1String("circle")) rx = ry = L("r", std::hypot(docW, docH) / std::sqrt(2.0));
                    else {
                        rx = L("rx", docW); ry = L("ry", docH);
                        if (!at.hasAttribute(QLatin1String("rx"))) rx = ry;
                        if (!at.hasAttribute(QLatin1String("ry"))) ry = rx;
                    }
                    if (rx <= 0 || ry <= 0) continue;
                    o.type = ObjType::Ellipse;
                    o.name = nm == QLatin1String("circle") ? QStringLiteral("Circle") : QStringLiteral("Ellipse");
                    o.rect = QRectF(cx - rx, cy - ry, rx * 2, ry * 2);
                    o.xform = s.xf;
                    paintFrom(o, s, QTransform(), 1, parentOp);
                } else if (nm == QLatin1String("line")) {
                    o.type = ObjType::Line;
                    o.name = QStringLiteral("Line");
                    o.p1 = s.xf.map(QPointF(L("x1", docW), L("y1", docH)));
                    o.p2 = s.xf.map(QPointF(L("x2", docW), L("y2", docH)));
                    paintFrom(o, s, s.xf, scaleOf(s.xf), parentOp);
                    o.fillOn = false;
                    pendingGrad.clear();
                } else if (nm == QLatin1String("polyline") || nm == QLatin1String("polygon") || nm == QLatin1String("path")) {
                    if (nm == QLatin1String("path")) {
                        // a Libreworks Auto Shape: rebuild the live shape from lw:shape / lw:rect / lw:<param>
                        const int depth = int(stack.size());
                        QString kind = at.value(kLwNs, QStringLiteral("shape")).toString();
                        QString rectS = at.value(kLwNs, QStringLiteral("rect")).toString();
                        QMap<QString, double> pv;
                        for (const auto &a : at)
                            if (a.namespaceUri() == kLwNs && a.name() != QLatin1String("shape") && a.name() != QLatin1String("rect") && a.name() != QLatin1String("part") &&
                                a.name() != QLatin1String("transform")) {
                                bool ok = false;
                                const double v = a.value().toDouble(&ok);
                                if (ok) pv.insert(a.name().toString(), v);
                            }
                        const bool isPart = at.hasAttribute(kLwNs, QStringLiteral("part"));
                        const bool legacyChild = legacy.depth >= 0 && depth == legacy.depth + 1;
                        if (kind.isEmpty() && legacyChild && lwOwnerDepth != depth) {
                            kind = legacy.kind;
                            rectS = legacy.rect;
                            for (const QString &kv : legacy.params.split(QLatin1Char(';'), Qt::SkipEmptyParts))
                                pv.insert(kv.section(QLatin1Char('='), 0, 0), kv.section(QLatin1Char('='), 1).toDouble());
                        }
                        const QVector<double> rv = numbers(rectS);
                        bool edited = false;
                        if (!kind.isEmpty() && autoshape::isKind(kind) && rv.size() == 4) {
                            // geometry edited in another program: keep what was drawn (plain paths)
                            const autoshape::Geometry g = autoshape::geometry(kind, [&] {
                                VObject t; autoshape::init(t, kind); t.rect = QRectF(rv[0], rv[1], rv[2], rv[3]);
                                for (auto it = pv.cbegin(); it != pv.cend(); ++it) if (t.shapeParams.contains(it.key())) autoshape::setParam(t, it.key(), it.value());
                                return t.shapeParams; }(), QRectF(rv[0], rv[1], rv[2], rv[3]));
                            const QString part = at.value(kLwNs, QStringLiteral("part")).toString();
                            const QPainterPath want = part == QLatin1String("open") ? g.open : part == QLatin1String("heads") ? g.heads : g.fill;
                            const QRectF a = parsePathData(at.value(QLatin1String("d")).toString()).boundingRect(), b = want.boundingRect();
                            edited = want.isEmpty() || std::abs(a.left() - b.left()) > 0.5 || std::abs(a.top() - b.top()) > 0.5 ||
                                     std::abs(a.right() - b.right()) > 0.5 || std::abs(a.bottom() - b.bottom()) > 0.5;
                            if (edited) { notes << QStringLiteral("Auto Shape '%1' was edited outside %2: imported as paths").arg(kind, QStringLiteral(LW_PRODUCT_NAME)); lwOwnerDepth = -1; }
                        }
                        if (!edited && !kind.isEmpty() && autoshape::isKind(kind) && rv.size() == 4) {
                            VObject ao;
                            autoshape::init(ao, kind);
                            ao.type = ObjType::AutoShape;
                            ao.shapeKind = kind;
                            ao.shapeParams = autoshape::defaults(kind);
                            ao.rect = QRectF(rv[0], rv[1], rv[2], rv[3]);
                            for (auto it = pv.cbegin(); it != pv.cend(); ++it)
                                if (ao.shapeParams.contains(it.key())) autoshape::setParam(ao, it.key(), it.value());
                            ao.name = autoshape::label(kind);
                            ao.xform = s.xf;
                            if (at.hasAttribute(kLwNs, QStringLiteral("transform")) && stack.size() >= 2) {
                                // the exact transform, if the rounded SVG one agrees with it
                                const QTransform exact = parseTransform(at.value(kLwNs, QStringLiteral("transform")).toString()) * stack[stack.size() - 2].s.xf;
                                const QTransform &r = s.xf;
                                if (std::abs(exact.m11() - r.m11()) < 1e-2 && std::abs(exact.m12() - r.m12()) < 1e-2 && std::abs(exact.m21() - r.m21()) < 1e-2 &&
                                    std::abs(exact.m22() - r.m22()) < 1e-2 && std::abs(exact.dx() - r.dx()) < 0.5 && std::abs(exact.dy() - r.dy()) < 0.5)
                                    ao.xform = exact;
                            }
                            paintFrom(ao, s, QTransform(), 1, parentOp);
                            ao.visible = s.visible;
                            resolveGradient(ao, s);
                            if (ao.xform.type() <= QTransform::TxTranslate) simplify(ao);
                            add(ao, curLayer);
                            lwOwnerDepth = depth;
                            pendingGrad.clear();
                            continue;
                        }
                        if ((isPart || legacyChild) && lwOwnerDepth == depth) continue;   // the shape's other parts
                        if (at.value(kLwNs, QStringLiteral("part")) == QLatin1String("taper") && lwPressureDepth == depth) continue;
                    }
                    QPainterPath pp;
                    if (nm == QLatin1String("path")) pp = parsePathData(at.value(QLatin1String("d")).toString());
                    else {
                        const QVector<double> v = numbers(at.value(QLatin1String("points")).toString());
                        for (int i = 0; i + 1 < v.size(); i += 2) { if (i == 0) pp.moveTo(v[0], v[1]); else pp.lineTo(v[i], v[i + 1]); }
                        if (nm == QLatin1String("polygon")) pp.closeSubpath();
                    }
                    if (pp.elementCount() < 2) continue;
                    pp = s.xf.map(pp);
                    // Libreworks' "closed" applies to every contour: split mixed open / closed paths
                    QVector<QPainterPath> subs;
                    QVector<bool> closed;
                    QPainterPath cur;
                    for (int i = 0; i < pp.elementCount(); ++i) {
                        const auto e = pp.elementAt(i);
                        if (e.isMoveTo()) { if (cur.elementCount()) subs << cur; cur = QPainterPath(); cur.moveTo(e); }
                        else if (e.isLineTo()) cur.lineTo(e);
                        else if (e.isCurveTo() && i + 2 < pp.elementCount()) { cur.cubicTo(e, pp.elementAt(i + 1), pp.elementAt(i + 2)); i += 2; }
                    }
                    if (cur.elementCount()) subs << cur;
                    for (const auto &sp : subs) {
                        const QPointF a = sp.elementAt(0), b = sp.elementAt(sp.elementCount() - 1);
                        closed << (std::hypot(a.x() - b.x(), a.y() - b.y()) < 1e-3);
                    }
                    const bool allSame = std::all_of(closed.begin(), closed.end(), [&](bool c) { return c == closed.first(); });
                    QVector<QPainterPath> parts;
                    if (allSame) parts << pp; else parts = subs;
                    if (!allSame) notes << QStringLiteral("path with open and closed parts split into separate objects");
                    for (const QPainterPath &part : parts) {
                        VObject po;
                        paths::fromPainterPath(po, part);
                        if (po.points.size() < 2) continue;
                        po.name = nm == QLatin1String("path") ? QStringLiteral("Path") : nm == QLatin1String("polygon") ? QStringLiteral("Polygon") : QStringLiteral("Polyline");
                        paintFrom(po, s, s.xf, scaleOf(s.xf), parentOp);
                        po.visible = s.visible;
                        resolveGradient(po, s);
                        if (nm == QLatin1String("path") && at.hasAttribute(kLwNs, QStringLiteral("size")) && parts.size() == 1) {
                            // Path Scrubber data: the stroke and the per-point size / opacity
                            const QVector<double> sz = numbers(at.value(kLwNs, QStringLiteral("size")).toString());
                            const QVector<double> op = numbers(at.value(kLwNs, QStringLiteral("opacity")).toString());
                            bool none = false;
                            QColor sc = parseColor(at.value(kLwNs, QStringLiteral("stroke")).toString(), QStringLiteral("black"), &none);
                            const double sw = at.value(kLwNs, QStringLiteral("stroke-width")).toDouble() * scaleOf(s.xf);
                            if (!none && sc.isValid() && sw > 0) {
                                bool ok = false;
                                const double sa = at.value(kLwNs, QStringLiteral("stroke-opacity")).toDouble(&ok);
                                if (ok) sc.setAlphaF(float(qBound(0.0, sa, 1.0)));
                                po.strokeOn = true;
                                po.stroke = sc;
                                po.strokeWidth = sw;
                                po.strokeKind = QStringLiteral("basic.hard");
                                po.strokeAlign = 0;
                            }
                            if (sz.size() == po.points.size() && op.size() == po.points.size()) {
                                po.ptSize.clear(); po.ptOpacity.clear();
                                for (double v : sz) po.ptSize << (std::isfinite(v) ? qBound(0.0, v, VObject::kMaxPointSize) : 1.0);
                                for (double v : op) po.ptOpacity << (std::isfinite(v) ? qBound(0.0, v, 1.0) : 1.0);
                            } else notes << QStringLiteral("path pressure data did not match the edited path: dropped");
                            lwPressureDepth = int(stack.size());
                        }
                        add(po, curLayer);
                    }
                    pendingGrad.clear();
                    continue;
                } else if (nm == QLatin1String("text")) {
                    inText = true; textBuf.clear(); textStyle = s; textOp = parentOp; textStyled = false;
                    const QVector<double> xs = numbers(at.value(QLatin1String("x")).toString()), ys = numbers(at.value(QLatin1String("y")).toString());
                    textPos = QPointF(xs.value(0), ys.value(0));
                    textPosSet = !xs.isEmpty() || !ys.isEmpty();
                    continue;
                } else if (nm == QLatin1String("tspan") && inText) {
                    const QVector<double> xs = numbers(at.value(QLatin1String("x")).toString()), ys = numbers(at.value(QLatin1String("y")).toString());
                    if (!textPosSet && (!xs.isEmpty() || !ys.isEmpty())) { textPos = QPointF(xs.value(0), ys.value(0)); textPosSet = true; }
                    else if (!textBuf.trimmed().isEmpty() && !ys.isEmpty()) textBuf += QLatin1Char('\n');
                    if (!textStyled && textBuf.trimmed().isEmpty()) { const QTransform keep = textStyle.xf; textStyle = s; textStyle.xf = keep; textStyled = true; }
                    continue;
                } else if (nm == QLatin1String("image")) {
                    QString href = at.value(QLatin1String("xlink:href")).toString();
                    if (href.isEmpty()) href = at.value(QLatin1String("href")).toString();
                    QImage img;
                    if (href.startsWith(QLatin1String("data:"))) {
                        const int c = href.indexOf(QLatin1Char(','));
                        const QByteArray payload = href.mid(c + 1).toLatin1();
                        img.loadFromData(href.left(c).contains(QLatin1String(";base64")) ? QByteArray::fromBase64(payload) : QByteArray::fromPercentEncoding(payload));
                    } else if (!href.isEmpty() && !href.contains(QLatin1String("://")) && !baseDir.isEmpty()) {
                        img.load(QDir(baseDir).filePath(href));
                    }
                    if (img.isNull()) { notes << QStringLiteral("image '%1' could not be loaded").arg(href.left(60)); continue; }
                    double w = L("width", docW), h = L("height", docH);
                    if (w <= 0) w = img.width();
                    if (h <= 0) h = img.height();
                    o.type = ObjType::Bitmap;
                    o.name = QStringLiteral("Bitmap");
                    o.image = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                    o.rect = QRectF(L("x", docW), L("y", docH), w, h);
                    o.xform = s.xf;
                    if (o.xform.type() <= QTransform::TxTranslate && std::abs(w - img.width()) < 1e-6 && std::abs(h - img.height()) < 1e-6) {
                        o.rect.translate(o.xform.dx(), o.xform.dy());
                        o.xform = QTransform();
                    }
                    o.fillOn = o.strokeOn = false;
                    o.stampInterpolation();
                    o.opacity = qBound(0, qRound(s.opacity * parentOp * 100), 100);
                } else {
                    continue;
                }
                o.visible = s.visible;
                resolveGradient(o, s);
                simplify(o);
                add(o, curLayer);
            } else if (x.isCharacters()) {
                if (inText && !skipDepth) textBuf += x.text().toString().replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
            } else if (x.isEndElement()) {
                if (skipDepth) { --skipDepth; continue; }
                if (x.name() == QLatin1String("text") && inText) {
                    inText = false;
                    QString text = textBuf;
                    text.replace(QRegularExpression(QStringLiteral(" *\\n *")), QStringLiteral("\n"));
                    text = text.trimmed();
                    if (!text.isEmpty()) {
                        const Style &s = textStyle;
                        VObject o;
                        o.type = ObjType::Text;
                        o.name = QStringLiteral("Text");
                        o.text = text;
                        o.fontFamily = s.fontFamily;
                        o.fontSize = s.fontSize;
                        o.bold = s.bold; o.italic = s.italic; o.underline = s.underline;
                        o.textAlign = s.anchor == QLatin1String("middle") ? 1 : s.anchor == QLatin1String("end") ? 2 : 0;
                        o.rect = QRectF(0, 0, 100000, 100000);
                        o.fitTextBox();
                        const QVector<VObject::TextLine> lines = o.textLines();
                        const double baseOff = lines.isEmpty() ? QFontMetricsF(o.font()).ascent() : lines.first().baseline.y() - o.rect.top();
                        double left = textPos.x();
                        if (o.textAlign == 1) left -= o.rect.width() / 2;
                        else if (o.textAlign == 2) left -= o.rect.width();
                        o.rect.moveTo(left, textPos.y() - baseOff);
                        o.xform = s.xf;
                        paintFrom(o, s, QTransform(), 1, textOp);
                        o.visible = s.visible;
                        resolveGradient(o, s);
                        simplify(o);
                        add(o, curLayer);
                    }
                }
                if (!stack.isEmpty()) {
                    stack.removeLast();
                    while (!groups.isEmpty() && int(stack.size()) < groups.last().depth) closeGroup(curLayer);
                    if (lwOwnerDepth >= 0 && int(stack.size()) < lwOwnerDepth - 1) lwOwnerDepth = -1;
                    if (lwPressureDepth >= 0 && int(stack.size()) < lwPressureDepth - 1) lwPressureDepth = -1;
                    if (legacy.depth >= 0 && int(stack.size()) < legacy.depth) legacy = LegacyShape{};
                    if (layerDepth >= 0 && int(stack.size()) < layerDepth) { layerDepth = -1; curLayer = 0; }
                }
            }
        }
        if (x.hasError()) { setErr(err, QStringLiteral("SVG: %1 (line %2)").arg(x.errorString()).arg(x.lineNumber())); return false; }
        return true;
    }
};

// ======================================================================= export
QString num(double v) {
    if (std::abs(v) < 5e-5) return QStringLiteral("0");
    QString s = QString::number(v, 'f', 3);
    while (s.contains(QLatin1Char('.')) && (s.endsWith(QLatin1Char('0')) || s.endsWith(QLatin1Char('.')))) s.chop(1);
    return s;
}
QString colorStr(const QColor &c) { return c.name(QColor::HexRgb); }
QString matrixStr(const QTransform &t) {
    return QStringLiteral("matrix(%1 %2 %3 %4 %5 %6)").arg(num(t.m11()), num(t.m12()), num(t.m21()), num(t.m22()), num(t.dx()), num(t.dy()));
}
bool plainStroke(const VObject &o) {
    return !o.strokeOn || (o.strokeKind == QLatin1String("basic.hard") && o.strokeAlign == 0 && (o.strokeTexture.isEmpty() || o.strokeTextureAmount == 0));
}
bool exportsAsVector(const VObject &o) {
    if (o.type == ObjType::Group) {
        // a <g>: every member a vector, the mask (if any) a vector outline (<clipPath>)
        for (const auto &e : o.effects) if (e.enabled) return false;
        for (const VObject &c : o.children) if (c.visible && !c.isWeb() && !exportsAsVector(c)) return false;
        if (o.maskActive() && !(o.maskMode == VObject::MaskOutline && o.maskObj[0].isVector() && o.maskObj[0].type != ObjType::Line)) return false;
        return true;
    }
    if (!o.isVector()) return false;
    for (const auto &e : o.effects) if (e.enabled) return false;
    if (o.fillOn && o.type != ObjType::Line && o.fillKind != FillKind::Solid && o.fillKind != FillKind::Linear && o.fillKind != FillKind::Radial && o.fillKind != FillKind::Pattern) return false;
    if (o.fillOn && o.type != ObjType::Line && o.fillKind == FillKind::Pattern && fills::patternImage(o).isNull()) return false;
    if (o.feather > 0 || (!o.texture.isEmpty() && o.textureAmount > 0)) return false;
    if (!plainStroke(o)) return false;
    if (o.type == ObjType::Text && (o.textOnPath || std::abs(o.kerning) > 1e-6 || std::abs(o.hScale - 100) > 1e-6 || std::abs(o.baselineShift) > 1e-6)) return false;
    return true;
}
QString blendCss(BlendMode m) {
    switch (m) {
    case BlendMode::Multiply: return QStringLiteral("multiply");
    case BlendMode::Screen: return QStringLiteral("screen");
    case BlendMode::Overlay: return QStringLiteral("overlay");
    case BlendMode::Darken: return QStringLiteral("darken");
    case BlendMode::Lighten: return QStringLiteral("lighten");
    case BlendMode::Difference: return QStringLiteral("difference");
    case BlendMode::ColorDodge: return QStringLiteral("color-dodge");
    case BlendMode::ColorBurn: return QStringLiteral("color-burn");
    case BlendMode::HardLight: return QStringLiteral("hard-light");
    case BlendMode::SoftLight: return QStringLiteral("soft-light");
    case BlendMode::Exclusion: return QStringLiteral("exclusion");
    case BlendMode::Hue: return QStringLiteral("hue");
    case BlendMode::Saturation: return QStringLiteral("saturation");
    case BlendMode::Color: return QStringLiteral("color");
    case BlendMode::Luminosity: return QStringLiteral("luminosity");
    case BlendMode::Invert: return QStringLiteral("invert");
    case BlendMode::Tint: return QStringLiteral("tint");
    case BlendMode::Erase: return QStringLiteral("erase");
    default: return {};
    }
}
QString pathData(const VObject &o) {
    QString d;
    const int n = o.points.size();
    const bool curves = o.hIn.size() == n && o.hOut.size() == n;
    QVector<int> starts{0};
    for (int c : o.contours) if (c > 0 && c < n) starts << c;
    starts << n;
    for (int c = 0; c + 1 < starts.size(); ++c) {
        const int a = starts[c], b = starts[c + 1];
        if (b - a < 1) continue;
        d += QStringLiteral("M%1 %2").arg(num(o.points[a].x()), num(o.points[a].y()));
        auto seg = [&](int i, int j) {
            const bool straight = !curves || (o.hOut[i] == o.points[i] && o.hIn[j] == o.points[j]);
            if (straight) d += QStringLiteral("L%1 %2").arg(num(o.points[j].x()), num(o.points[j].y()));
            else d += QStringLiteral("C%1 %2 %3 %4 %5 %6").arg(num(o.hOut[i].x()), num(o.hOut[i].y()), num(o.hIn[j].x()), num(o.hIn[j].y()), num(o.points[j].x()), num(o.points[j].y()));
        };
        for (int i = a; i + 1 < b; ++i) seg(i, i + 1);
        if (o.closed) { if (b - a > 1) seg(b - 1, a); d += QLatin1Char('Z'); }
    }
    return d;
}
QByteArray pngData(const QImage &img) {
    QByteArray b;
    QBuffer buf(&b);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return b;
}

class Exporter {
public:
    QXmlStreamWriter &w;
    int gradN = 0;
    bool pure = true;
    QRect canvas;
    Exporter(QXmlStreamWriter &wr, QRect c) : w(wr), canvas(c) {}

    QString gradient(const VObject &o) {
        const QString id = QStringLiteral("lwg%1").arg(++gradN);
        const VObject base = o.untransformed();
        const QPointF P0 = fills::handleToDoc(base, o.gradP0), P1 = fills::handleToDoc(base, o.gradP1), P2 = fills::handleToDoc(base, o.gradP2);
        QPointF u = P1 - P0, v = P2 - P0;
        if (std::hypot(u.x(), u.y()) < 1e-6) u = QPointF(1, 0);
        if (std::abs(u.x() * v.y() - u.y() * v.x()) < 1e-6) v = QPointF(u.y(), -u.x());
        const bool radial = o.fillKind == FillKind::Radial;
        w.writeStartElement(QStringLiteral("defs"));
        w.writeStartElement(radial ? QStringLiteral("radialGradient") : QStringLiteral("linearGradient"));
        w.writeAttribute(QStringLiteral("id"), id);
        w.writeAttribute(QStringLiteral("gradientUnits"), QStringLiteral("userSpaceOnUse"));
        if (radial) {
            // the unit circle in the handles' (u, v) frame is exactly Libreworks' radial gradient
            w.writeAttribute(QStringLiteral("cx"), QStringLiteral("0"));
            w.writeAttribute(QStringLiteral("cy"), QStringLiteral("0"));
            w.writeAttribute(QStringLiteral("r"), QStringLiteral("1"));
            w.writeAttribute(QStringLiteral("gradientTransform"), matrixStr(QTransform(u.x(), u.y(), v.x(), v.y(), P0.x(), P0.y())));
        } else {
            w.writeAttribute(QStringLiteral("x1"), num(P0.x())); w.writeAttribute(QStringLiteral("y1"), num(P0.y()));
            w.writeAttribute(QStringLiteral("x2"), num(P1.x())); w.writeAttribute(QStringLiteral("y2"), num(P1.y()));
        }
        for (const GradStop &s : fills::effectiveGradient(o)) {
            w.writeEmptyElement(QStringLiteral("stop"));
            w.writeAttribute(QStringLiteral("offset"), num(s.pos));
            w.writeAttribute(QStringLiteral("stop-color"), colorStr(s.color));
            if (s.color.alpha() < 255) w.writeAttribute(QStringLiteral("stop-opacity"), num(s.color.alphaF()));
        }
        w.writeEndElement();
        w.writeEndElement();
        return id;
    }
    // a Pattern fill: the tile as a userSpaceOnUse <pattern>; the pattern handles are its patternTransform
    QString pattern(const VObject &o) {
        const QString id = QStringLiteral("lwp%1").arg(++gradN);
        const QImage tile = fills::patternImage(o).convertToFormat(QImage::Format_ARGB32);
        const QTransform t = fills::patternBrushTransform(o.untransformed());
        w.writeStartElement(QStringLiteral("defs"));
        w.writeStartElement(QStringLiteral("pattern"));
        w.writeAttribute(QStringLiteral("id"), id);
        w.writeAttribute(QStringLiteral("patternUnits"), QStringLiteral("userSpaceOnUse"));
        w.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
        w.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
        w.writeAttribute(QStringLiteral("width"), QString::number(tile.width()));
        w.writeAttribute(QStringLiteral("height"), QString::number(tile.height()));
        w.writeAttribute(QStringLiteral("patternTransform"), matrixStr(t));
        w.writeEmptyElement(QStringLiteral("image"));
        w.writeAttribute(QStringLiteral("width"), QString::number(tile.width()));
        w.writeAttribute(QStringLiteral("height"), QString::number(tile.height()));
        w.writeAttribute(QStringLiteral("href"), QStringLiteral("data:image/png;base64,") + QString::fromLatin1(pngData(tile).toBase64()));
        w.writeEndElement();
        w.writeEndElement();
        return id;
    }
    void commonAttrs(const VObject &o) {
        if (o.opacity < 100) w.writeAttribute(QStringLiteral("opacity"), num(o.opacity / 100.0));
        const QString bm = blendCss(o.blend);
        if (!bm.isEmpty()) w.writeAttribute(QStringLiteral("style"), QStringLiteral("mix-blend-mode:%1").arg(bm));
    }
    void paintAttrs(const VObject &o, bool canFill, const QString &gradId) {
        if (canFill && o.fillOn) {
            if (!gradId.isEmpty()) w.writeAttribute(QStringLiteral("fill"), QStringLiteral("url(#%1)").arg(gradId));
            else {
                w.writeAttribute(QStringLiteral("fill"), colorStr(o.fill));
                if (o.fill.alpha() < 255) w.writeAttribute(QStringLiteral("fill-opacity"), num(o.fill.alphaF()));
            }
        } else w.writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));
        if (o.strokeOn && o.strokeWidth > 0) {
            w.writeAttribute(QStringLiteral("stroke"), colorStr(o.stroke));
            if (o.stroke.alpha() < 255) w.writeAttribute(QStringLiteral("stroke-opacity"), num(o.stroke.alphaF()));
            w.writeAttribute(QStringLiteral("stroke-width"), num(o.strokeWidth));
            if (o.type == ObjType::Path || o.type == ObjType::Line) {
                w.writeAttribute(QStringLiteral("stroke-linejoin"), QStringLiteral("round"));
                w.writeAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("round"));
            }
        }
        commonAttrs(o);
        if (!o.antialias) w.writeAttribute(QStringLiteral("shape-rendering"), QStringLiteral("crispEdges"));
        if (o.hasXform()) w.writeAttribute(QStringLiteral("transform"), matrixStr(o.xform));
    }
    void image(const QRectF &r, const QImage &img, const VObject &o, bool withXform) {
        pure = false;
        w.writeEmptyElement(QStringLiteral("image"));
        w.writeAttribute(QStringLiteral("x"), num(r.x()));
        w.writeAttribute(QStringLiteral("y"), num(r.y()));
        w.writeAttribute(QStringLiteral("width"), num(r.width()));
        w.writeAttribute(QStringLiteral("height"), num(r.height()));
        w.writeAttribute(QStringLiteral("preserveAspectRatio"), QStringLiteral("none"));
        commonAttrs(o);
        if (withXform && o.hasXform()) w.writeAttribute(QStringLiteral("transform"), matrixStr(o.xform));
        w.writeAttribute(QStringLiteral("href"), QStringLiteral("data:image/png;base64,") + QString::fromLatin1(pngData(img).toBase64()));
    }
    QString painterPathD(const QPainterPath &pp) {
        QString d;
        for (int i = 0; i < pp.elementCount(); ++i) {
            const QPainterPath::Element e = pp.elementAt(i);
            if (e.isMoveTo()) d += QStringLiteral("M%1 %2").arg(num(e.x), num(e.y));
            else if (e.isLineTo()) d += QStringLiteral("L%1 %2").arg(num(e.x), num(e.y));
            else if (e.isCurveTo() && i + 2 < pp.elementCount()) {
                const QPainterPath::Element c2 = pp.elementAt(i + 1), en = pp.elementAt(i + 2);
                d += QStringLiteral("C%1 %2 %3 %4 %5 %6").arg(num(e.x), num(e.y), num(c2.x), num(c2.y), num(en.x), num(en.y));
                i += 2;
            }
        }
        return d;
    }
    int maskN = 0;
    // a group as <g lw:group="1">; its mask as a <clipPath> (vector outline) or a luminance <mask>
    void group(const VObject &o) {
        w.writeStartElement(QStringLiteral("g"));
        w.writeAttribute(kLwNs, QStringLiteral("group"), QStringLiteral("1"));
        if (!o.name.isEmpty()) w.writeAttribute(QStringLiteral("inkscape:label"), o.name);
        commonAttrs(o);
        if (o.maskActive()) {
            const VObject &m = o.maskObj[0];
            const QString id = QStringLiteral("lwm%1").arg(++maskN);
            if (o.maskMode == VObject::MaskOutline && m.isVector() && m.type != ObjType::Line) {
                QPainterPath path = m.type == ObjType::Text ? (m.hasXform() ? m.xform.map(m.untransformed().textPath()) : m.textPath()) : m.shapePath();
                w.writeAttribute(QStringLiteral("clip-path"), QStringLiteral("url(#%1)").arg(id));
                w.writeStartElement(QStringLiteral("defs"));
                w.writeStartElement(QStringLiteral("clipPath"));
                w.writeAttribute(QStringLiteral("id"), id);
                w.writeAttribute(QStringLiteral("clipPathUnits"), QStringLiteral("userSpaceOnUse"));
                w.writeEmptyElement(QStringLiteral("path"));
                w.writeAttribute(QStringLiteral("d"), painterPathD(path));
                w.writeAttribute(QStringLiteral("clip-rule"), QStringLiteral("nonzero"));
                w.writeEndElement();
                w.writeEndElement();
            } else {
                // the mask as pixels: grayscale = its luminance x alpha, alpha masks = white with its alpha
                VObject mm = m;
                mm.visible = true;
                QRect area;
                QImage img = render::renderObject(mm, &area, QRect(-1000000, -1000000, 2000000, 2000000));
                img = img.convertToFormat(QImage::Format_ARGB32);
                if (o.maskMode == VObject::MaskOutline)
                    for (int y = 0; y < img.height(); ++y) {
                        QRgb *l = reinterpret_cast<QRgb *>(img.scanLine(y));
                        for (int x = 0; x < img.width(); ++x) l[x] = qRgba(255, 255, 255, qAlpha(l[x]));
                    }
                pure = false;
                w.writeAttribute(QStringLiteral("mask"), QStringLiteral("url(#%1)").arg(id));
                w.writeStartElement(QStringLiteral("defs"));
                w.writeStartElement(QStringLiteral("mask"));
                w.writeAttribute(QStringLiteral("id"), id);
                w.writeAttribute(QStringLiteral("maskUnits"), QStringLiteral("userSpaceOnUse"));
                w.writeAttribute(QStringLiteral("x"), num(area.x())); w.writeAttribute(QStringLiteral("y"), num(area.y()));
                w.writeAttribute(QStringLiteral("width"), num(area.width())); w.writeAttribute(QStringLiteral("height"), num(area.height()));
                if (!img.isNull()) {
                    w.writeEmptyElement(QStringLiteral("image"));
                    w.writeAttribute(QStringLiteral("x"), num(area.x())); w.writeAttribute(QStringLiteral("y"), num(area.y()));
                    w.writeAttribute(QStringLiteral("width"), num(area.width())); w.writeAttribute(QStringLiteral("height"), num(area.height()));
                    w.writeAttribute(QStringLiteral("preserveAspectRatio"), QStringLiteral("none"));
                    w.writeAttribute(QStringLiteral("href"), QStringLiteral("data:image/png;base64,") + QString::fromLatin1(pngData(img).toBase64()));
                }
                w.writeEndElement();
                w.writeEndElement();
            }
        }
        for (const VObject &c : o.children) object(c);
        w.writeEndElement();
    }
    void object(const VObject &o) {
        if (!o.visible || o.isWeb()) return;
        bool fx = false;
        for (const auto &e : o.effects) fx |= e.enabled;
        if (o.type == ObjType::Group && !fx) { group(o); return; }
        if (o.type == ObjType::Bitmap && !fx && !o.quickMask && !o.image.isNull()) {
            image(o.rect, o.image.convertToFormat(QImage::Format_ARGB32), o, true);
            return;
        }
        if (!exportsAsVector(o)) {
            // rendered exactly as on the canvas (opacity / blend applied by the image element)
            VObject plain = o;
            plain.opacity = 100;
            plain.blend = BlendMode::Normal;
            QRect area;
            const QImage img = render::renderObject(plain, &area, canvas);
            if (!img.isNull() && !area.isEmpty()) image(QRectF(area), img.convertToFormat(QImage::Format_ARGB32), o, false);
            return;
        }
        const QString gid = !(o.fillOn && o.type != ObjType::Line) ? QString()
                          : (o.fillKind == FillKind::Linear || o.fillKind == FillKind::Radial) ? gradient(o)
                          : o.fillKind == FillKind::Pattern ? pattern(o) : QString();
        switch (o.type) {
        case ObjType::Rect: {
            const QRectF r = o.rect.normalized();
            w.writeEmptyElement(QStringLiteral("rect"));
            w.writeAttribute(QStringLiteral("x"), num(r.x())); w.writeAttribute(QStringLiteral("y"), num(r.y()));
            w.writeAttribute(QStringLiteral("width"), num(r.width())); w.writeAttribute(QStringLiteral("height"), num(r.height()));
            if (o.roundness > 0) {
                const double rad = o.roundness / 100.0 * qMin(r.width(), r.height()) / 2.0;
                w.writeAttribute(QStringLiteral("rx"), num(rad)); w.writeAttribute(QStringLiteral("ry"), num(rad));
            }
            paintAttrs(o, true, gid);
            break;
        }
        case ObjType::Ellipse: {
            const QRectF r = o.rect.normalized();
            w.writeEmptyElement(QStringLiteral("ellipse"));
            w.writeAttribute(QStringLiteral("cx"), num(r.center().x())); w.writeAttribute(QStringLiteral("cy"), num(r.center().y()));
            w.writeAttribute(QStringLiteral("rx"), num(r.width() / 2)); w.writeAttribute(QStringLiteral("ry"), num(r.height() / 2));
            paintAttrs(o, true, gid);
            break;
        }
        case ObjType::AutoShape: {
            // one <path> per part, grouped; data-lw-* keeps the editable parameters
            const autoshape::Geometry g = autoshape::geometry(o.untransformed());
            auto dOf = [](const QPainterPath &pp) {
                QString d;
                for (int i = 0; i < pp.elementCount(); ++i) {
                    const QPainterPath::Element e = pp.elementAt(i);
                    if (e.isMoveTo()) d += QStringLiteral("M%1 %2").arg(num(e.x), num(e.y));
                    else if (e.isLineTo()) d += QStringLiteral("L%1 %2").arg(num(e.x), num(e.y));
                    else if (e.isCurveTo() && i + 2 < pp.elementCount()) {
                        const QPainterPath::Element c2 = pp.elementAt(i + 1), en = pp.elementAt(i + 2);
                        d += QStringLiteral("C%1 %2 %3 %4 %5 %6").arg(num(e.x), num(e.y), num(c2.x), num(c2.y), num(en.x), num(en.y));
                        i += 2;
                    }
                }
                return d;
            };
            auto closedD = [&](const QPainterPath &pp) {
                // close every subpath (QPainterPath::closeSubpath adds an explicit line back to the start)
                QString d = dOf(pp);
                d.replace(QLatin1Char('M'), QStringLiteral("ZM"));
                if (d.startsWith(QLatin1Char('Z'))) d.remove(0, 1);
                return d + QLatin1Char('Z');
            };
            // the first <path> carries lw:shape, lw:rect and one lw:<param> per parameter so Libreworks can
            // rebuild the live shape; every part is tagged lw:part (other SVG apps just see plain paths)
            bool first = true;
            auto meta = [&](const char *part) {
                w.writeAttribute(kLwNs, QStringLiteral("part"), QString::fromLatin1(part));
                if (!first) return;
                first = false;
                w.writeAttribute(kLwNs, QStringLiteral("shape"), o.shapeKind);
                const QRectF r = o.rect;
                auto ex = [](double v) { return QString::number(v, 'g', 12); };
                w.writeAttribute(kLwNs, QStringLiteral("rect"), QStringLiteral("%1 %2 %3 %4").arg(ex(r.x()), ex(r.y()), ex(r.width()), ex(r.height())));
                for (auto it = o.shapeParams.cbegin(); it != o.shapeParams.cend(); ++it) w.writeAttribute(kLwNs, it.key(), ex(it.value()));
                if (o.hasXform()) {   // full precision (the SVG transform attribute is rounded)
                    const QTransform &t = o.xform;
                    w.writeAttribute(kLwNs, QStringLiteral("transform"), QStringLiteral("matrix(%1 %2 %3 %4 %5 %6)")
                                     .arg(ex(t.m11()), ex(t.m12()), ex(t.m21()), ex(t.m22()), ex(t.dx()), ex(t.dy())));
                }
            };
            w.writeStartElement(QStringLiteral("g"));
            if (!g.fill.isEmpty()) {
                w.writeEmptyElement(QStringLiteral("path"));
                w.writeAttribute(QStringLiteral("d"), closedD(g.fill));
                w.writeAttribute(QStringLiteral("fill-rule"), g.fill.fillRule() == Qt::WindingFill ? QStringLiteral("nonzero") : QStringLiteral("evenodd"));
                paintAttrs(o, true, gid);
                meta("fill");
            }
            if (!g.open.isEmpty()) {
                VObject ln = o;
                ln.fillOn = false;
                w.writeEmptyElement(QStringLiteral("path"));
                w.writeAttribute(QStringLiteral("d"), dOf(g.open));
                paintAttrs(ln, false, QString());
                w.writeAttribute(QStringLiteral("stroke-linecap"), QStringLiteral("round"));
                w.writeAttribute(QStringLiteral("stroke-linejoin"), QStringLiteral("round"));
                meta("open");
            }
            if (!g.heads.isEmpty() && o.strokeOn) {
                VObject hd = o;
                hd.fillOn = true; hd.fill = o.stroke; hd.fillKind = FillKind::Solid; hd.strokeOn = false;
                w.writeEmptyElement(QStringLiteral("path"));
                w.writeAttribute(QStringLiteral("d"), closedD(g.heads));
                paintAttrs(hd, true, QString());
                meta("heads");
            }
            w.writeEndElement();
            break;
        }
        case ObjType::Line:
            w.writeEmptyElement(QStringLiteral("line"));
            w.writeAttribute(QStringLiteral("x1"), num(o.p1.x())); w.writeAttribute(QStringLiteral("y1"), num(o.p1.y()));
            w.writeAttribute(QStringLiteral("x2"), num(o.p2.x())); w.writeAttribute(QStringLiteral("y2"), num(o.p2.y()));
            paintAttrs(o, false, gid);
            break;
        case ObjType::Path:
            if (o.points.size() < 2) return;
            if (o.hasPointPressure() && o.strokeOn && o.strokeWidth > 0) {
                // Path Scrubber data: the fill as a plain path carrying the stroke and the per-point
                // size / opacity as lw: attributes (Libreworks rebuilds the object from it), and the
                // tapering stroke as filled outlines tagged lw:part="taper" for every other program
                auto ex = [](double v) { return QString::number(v, 'g', 6); };
                w.writeStartElement(QStringLiteral("g"));
                commonAttrs(o);
                if (o.hasXform()) w.writeAttribute(QStringLiteral("transform"), matrixStr(o.xform));
                VObject base = o;
                base.strokeOn = false;
                base.opacity = 100;
                base.blend = BlendMode::Normal;
                base.xform = QTransform();
                w.writeEmptyElement(QStringLiteral("path"));
                w.writeAttribute(QStringLiteral("d"), pathData(o));
                w.writeAttribute(QStringLiteral("fill-rule"), QStringLiteral("evenodd"));
                paintAttrs(base, true, gid);
                QStringList sz, op;
                for (int i = 0; i < o.points.size(); ++i) { sz << ex(o.pointSize(i)); op << ex(o.pointOpacity(i)); }
                w.writeAttribute(kLwNs, QStringLiteral("part"), QStringLiteral("pressure"));
                w.writeAttribute(kLwNs, QStringLiteral("size"), sz.join(QLatin1Char(' ')));
                w.writeAttribute(kLwNs, QStringLiteral("opacity"), op.join(QLatin1Char(' ')));
                w.writeAttribute(kLwNs, QStringLiteral("stroke"), colorStr(o.stroke));
                w.writeAttribute(kLwNs, QStringLiteral("stroke-opacity"), ex(o.stroke.alphaF()));
                w.writeAttribute(kLwNs, QStringLiteral("stroke-width"), ex(o.strokeWidth));
                for (const paths::TaperPiece &tp : paths::taperOutline(o.untransformed(), o.strokeWidth / 2)) {
                    QString d;
                    const QPainterPath &pp = tp.path;
                    for (int i = 0; i < pp.elementCount(); ++i) {
                        const QPainterPath::Element e = pp.elementAt(i);
                        if (e.isMoveTo()) d += QStringLiteral("%1M%2 %3").arg(i ? QStringLiteral("Z") : QString(), num(e.x), num(e.y));
                        else if (e.isLineTo()) d += QStringLiteral("L%1 %2").arg(num(e.x), num(e.y));
                        else if (e.isCurveTo() && i + 2 < pp.elementCount()) {
                            const QPainterPath::Element c2 = pp.elementAt(i + 1), en = pp.elementAt(i + 2);
                            d += QStringLiteral("C%1 %2 %3 %4 %5 %6").arg(num(e.x), num(e.y), num(c2.x), num(c2.y), num(en.x), num(en.y));
                            i += 2;
                        }
                    }
                    w.writeEmptyElement(QStringLiteral("path"));
                    w.writeAttribute(QStringLiteral("d"), d + QLatin1Char('Z'));
                    w.writeAttribute(QStringLiteral("fill"), colorStr(o.stroke));
                    const double fo = o.stroke.alphaF() * tp.opacity;
                    if (fo < 1) w.writeAttribute(QStringLiteral("fill-opacity"), num(fo));
                    if (!o.antialias) w.writeAttribute(QStringLiteral("shape-rendering"), QStringLiteral("crispEdges"));
                    w.writeAttribute(kLwNs, QStringLiteral("part"), QStringLiteral("taper"));
                }
                w.writeEndElement();
                break;
            }
            w.writeEmptyElement(QStringLiteral("path"));
            w.writeAttribute(QStringLiteral("d"), pathData(o));
            w.writeAttribute(QStringLiteral("fill-rule"), QStringLiteral("evenodd"));
            paintAttrs(o, true, gid);
            break;
        case ObjType::Text: {
            w.writeStartElement(QStringLiteral("text"));
            w.writeAttribute(QStringLiteral("font-family"), o.fontFamily);
            w.writeAttribute(QStringLiteral("font-size"), num(o.fontSize));
            if (o.bold) w.writeAttribute(QStringLiteral("font-weight"), QStringLiteral("bold"));
            if (o.italic) w.writeAttribute(QStringLiteral("font-style"), QStringLiteral("italic"));
            if (o.underline) w.writeAttribute(QStringLiteral("text-decoration"), QStringLiteral("underline"));
            w.writeAttribute(QStringLiteral("xml:space"), QStringLiteral("preserve"));
            paintAttrs(o, true, gid);
            for (const auto &ln : o.untransformed().textLines()) {
                w.writeStartElement(QStringLiteral("tspan"));
                w.writeAttribute(QStringLiteral("x"), num(ln.baseline.x()));
                w.writeAttribute(QStringLiteral("y"), num(ln.baseline.y()));
                w.writeCharacters(ln.text);
                w.writeEndElement();
            }
            w.writeEndElement();
            break;
        }
        default: break;
        }
    }
};
} // namespace

bool svgToState(const QByteArray &svgIn, DocState &st, const std::function<quint64()> &newId, const QString &baseDir, QStringList *notes, QString *err) {
    QByteArray svg = svgIn;
    if (isGzip(svg)) {
        bool ok;
        svg = gunzip(svgIn, &ok);
        if (!ok) { setErr(err, QStringLiteral("SVGZ: corrupt gzip data")); return false; }
    }
    Importer im(st, newId, baseDir);
    if (!im.run(svg, err)) return false;
    st.layers.clear();
    st.frames.clear();
    st.canvasColor = im.canvas;
    st.width = qBound(1, qRound(im.docW > 0 ? im.docW : 300), 30000);
    st.height = qBound(1, qRound(im.docH > 0 ? im.docH : 150), 30000);
    Frame f;
    f.name = QStringLiteral("Frame 1");
    f.delayMs = 100;
    if (im.unsupported) {
        QSvgRenderer r(svg);
        if (!r.isValid()) { setErr(err, QStringLiteral("SVG: could not be parsed")); return false; }
        QImage img(st.width, st.height, QImage::Format_ARGB32_Premultiplied);
        img.fill(0);
        { QPainter p(&img); r.render(&p); }
        VObject o;
        o.id = newId(); o.type = ObjType::Bitmap; o.name = QStringLiteral("SVG"); o.image = img;
        o.rect = QRectF(0, 0, st.width, st.height);
        o.fillOn = o.strokeOn = false;
        o.stampInterpolation();
        LayerInfo li; li.id = newId(); li.name = QStringLiteral("Layer 1");
        st.layers << li;
        f.layers.resize(1);
        f.layers[0] << o;
        if (notes) *notes << QStringLiteral("the SVG uses '%1', which has no %2 object equivalent: imported as one bitmap").arg(im.unsupportedWhat, QStringLiteral(LW_PRODUCT_NAME));
    } else {
        for (int i = 0; i < im.layerObjs.size(); ++i) {
            if (i == 0 && im.layerObjs[0].isEmpty() && im.layerObjs.size() > 1 && im.layerNames[0] == QLatin1String("Layer 1")) continue;
            LayerInfo li; li.id = newId(); li.name = im.layerNames[i]; li.visible = im.layerVisible[i];
            st.layers << li;
            f.layers << im.layerObjs[i];
        }
        if (st.layers.isEmpty()) { LayerInfo li; li.id = newId(); li.name = QStringLiteral("Layer 1"); st.layers << li; f.layers.resize(1); }
        if (notes) { im.notes.removeDuplicates(); *notes << im.notes; }
    }
    LayerInfo web; web.id = newId(); web.name = QStringLiteral("Web Layer"); web.web = true; web.shared = true;
    st.layers << web;
    f.layers.resize(st.layers.size());
    st.frames << f;
    return true;
}

bool isVectorDocument(const DocState &st, int frame) {
    if (frame < 0 || frame >= st.frames.size()) return false;
    bool any = false;
    const Frame &f = st.frames[frame];
    for (int li = 0; li < st.layers.size() && li < f.layers.size(); ++li) {
        if (!st.layers[li].visible || st.layers[li].web) continue;
        for (const VObject &o : f.layers[li]) {
            if (!o.visible) continue;
            if (!exportsAsVector(o)) return false;
            any = true;
        }
    }
    return any;
}

QByteArray stateToSvg(const DocState &st, int frame, bool *pureVector, QStringList *notes) {
    QByteArray out;
    QXmlStreamWriter w(&out);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(1);
    w.writeStartDocument();
    w.writeStartElement(QStringLiteral("svg"));
    w.writeDefaultNamespace(QStringLiteral("http://www.w3.org/2000/svg"));
    w.writeNamespace(QStringLiteral("http://www.inkscape.org/namespaces/inkscape"), QStringLiteral("inkscape"));
    w.writeNamespace(kLwNs, QStringLiteral("lw"));
    w.writeAttribute(QStringLiteral("version"), QStringLiteral("1.1"));
    w.writeAttribute(QStringLiteral("width"), QString::number(st.width));
    w.writeAttribute(QStringLiteral("height"), QString::number(st.height));
    w.writeAttribute(QStringLiteral("viewBox"), QStringLiteral("0 0 %1 %2").arg(st.width).arg(st.height));
    w.writeComment(QStringLiteral(" Created with %1 %2 ").arg(QStringLiteral(LW_PRODUCT_NAME), QStringLiteral(LW_VERSION)));
    SymbolScope scope(&st.symbols);
    Exporter ex(w, QRect(0, 0, st.width, st.height));
    if (st.canvasColor.alpha() > 0) {
        w.writeEmptyElement(QStringLiteral("rect"));
        w.writeAttribute(QStringLiteral("id"), QStringLiteral("canvas"));
        w.writeAttribute(QStringLiteral("width"), QString::number(st.width));
        w.writeAttribute(QStringLiteral("height"), QString::number(st.height));
        w.writeAttribute(QStringLiteral("fill"), colorStr(st.canvasColor));
        if (st.canvasColor.alpha() < 255) w.writeAttribute(QStringLiteral("fill-opacity"), num(st.canvasColor.alphaF()));
    }
    if (frame >= 0 && frame < st.frames.size()) {
        const Frame &f = st.frames[frame];
        for (int li = 0; li < st.layers.size() && li < f.layers.size(); ++li) {
            const LayerInfo &L = st.layers[li];
            if (L.web || !L.visible) continue;
            w.writeStartElement(QStringLiteral("g"));
            w.writeAttribute(QStringLiteral("inkscape:groupmode"), QStringLiteral("layer"));
            w.writeAttribute(QStringLiteral("inkscape:label"), L.name);
            for (const VObject &o : f.layers[li]) ex.object(o);
            w.writeEndElement();
        }
    }
    w.writeEndElement();
    w.writeEndDocument();
    if (pureVector) *pureVector = ex.pure;
    if (notes && !ex.pure) *notes << QStringLiteral("bitmaps and objects with live effects, textures or brush strokes are embedded as PNG images");
    return out;
}
} // namespace docio

namespace codec {
bool rasterizeSvg(const QByteArray &d, ImageSeq &out, QString *err) {
    QByteArray svg = d;
    if (docio::isGzip(d)) {
        bool ok;
        svg = docio::gunzip(d, &ok);
        if (!ok) { setErr(err, QStringLiteral("SVGZ: corrupt gzip data")); return false; }
    }
    QSvgRenderer r(svg);
    if (!r.isValid()) { setErr(err, QStringLiteral("SVG: could not be parsed")); return false; }
    QSize sz = r.defaultSize();
    if (sz.isEmpty()) sz = QSize(300, 150);
    sz = sz.boundedTo(QSize(30000, 30000));
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    { QPainter p(&img); r.render(&p); }
    out = ImageSeq();
    out.width = sz.width();
    out.height = sz.height();
    out.frames << AnimFrame{img.convertToFormat(QImage::Format_ARGB32), 100};
    return true;
}
}
