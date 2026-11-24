#include "Paths.h"
#include "AutoShape.h"
#include "Document.h"
#include "Object.h"
#include <QMap>
#include <QPainterPathStroker>
#include <QSet>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace paths {

static bool samePt(const QPointF &a, const QPointF &b, double eps = 1e-6) {
    return std::abs(a.x() - b.x()) < eps && std::abs(a.y() - b.y()) < eps;
}

QPointF bezierAt(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p3, double t) {
    const double u = 1 - t;
    return u * u * u * p0 + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * p3;
}

int nextInContour(const VObject &o, int i) {
    int a, b;
    o.contourRange(o.contourOfPoint(i), &a, &b);
    if (i + 1 < b) return i + 1;
    return (o.closed && b - a > 1) ? a : -1;
}

QPainterPath nodesToPath(const VObject &o) {
    QPainterPath path;
    const bool curves = o.hIn.size() == o.points.size() && o.hOut.size() == o.points.size();
    auto seg = [&](int i, int j) {
        if (curves && (o.hOut[i] != o.points[i] || o.hIn[j] != o.points[j])) path.cubicTo(o.hOut[i], o.hIn[j], o.points[j]);
        else path.lineTo(o.points[j]);
    };
    for (int c = 0; c < o.contourCount(); ++c) {
        int a, b;
        o.contourRange(c, &a, &b);
        if (b <= a) continue;
        path.moveTo(o.points[a]);
        for (int i = a; i + 1 < b; ++i) seg(i, i + 1);
        if (o.closed && b - a > 1) {
            seg(b - 1, a);
            path.closeSubpath();
        }
    }
    return path;
}

void fromPainterPath(VObject &o, const QPainterPath &p) {
    QPolygonF pts, in, out;
    QVector<int> starts;
    bool allClosed = true;
    int cStart = 0;
    auto finish = [&]() {
        const int n = pts.size() - cStart;
        if (n <= 0) return;
        if (n >= 2 && samePt(pts.last(), pts[cStart], 1e-4)) {
            in[cStart] = in.last();
            pts.removeLast(); in.removeLast(); out.removeLast();
        } else allClosed = false;
        if (pts.size() - cStart < 2) { // degenerate contour
            pts.resize(cStart); in.resize(cStart); out.resize(cStart);
            return;
        }
        if (cStart > 0) starts << cStart;
    };
    for (int i = 0; i < p.elementCount(); ++i) {
        const QPainterPath::Element e = p.elementAt(i);
        if (e.isMoveTo()) {
            finish();
            cStart = pts.size();
            pts << QPointF(e); in << QPointF(e); out << QPointF(e);
        } else if (e.isLineTo()) {
            if (!pts.isEmpty() && pts.size() > cStart && samePt(pts.last(), QPointF(e))) continue;
            pts << QPointF(e); in << QPointF(e); out << QPointF(e);
        } else if (e.isCurveTo() && i + 2 < p.elementCount()) {
            const QPointF c1 = p.elementAt(i), c2 = p.elementAt(i + 1), end = p.elementAt(i + 2);
            if (!pts.isEmpty()) out.last() = c1;
            pts << end; in << c2; out << end;
            i += 2;
        }
    }
    finish();
    // contour starts may need fixing after removals inside finish(): recompute from scratch is
    // unnecessary because finish() only trims the tail of the current contour.
    o.type = ObjType::Path;
    o.clearPointPressure();
    o.clearPointSense();
    o.points = pts;
    o.hIn = in;
    o.hOut = out;
    o.contours = starts;
    o.closed = pts.isEmpty() ? true : allClosed;
    o.dropTrivialHandles();
}

bool canConvert(const VObject &o) {
    switch (o.type) {
    case ObjType::Rect: case ObjType::Ellipse: case ObjType::Line: case ObjType::Text: case ObjType::Path: case ObjType::AutoShape: return true;
    default: return false;
    }
}

// Auto Shape -> path: the fill outline (closed) or the open line; arrowheads are returned separately.
static QPainterPath autoShapeBody(const VObject &o, QPainterPath *heads, bool *closed) {
    const VObject u = o.untransformed();
    const autoshape::Geometry g = autoshape::geometry(u);
    QPainterPath body = g.fill;
    body.addPath(g.open);
    *closed = g.open.isEmpty();
    if (heads) *heads = o.hasXform() ? o.xform.map(g.heads) : g.heads;
    return o.hasXform() ? o.xform.map(body) : body;
}

bool toPath(VObject &o) {
    if (o.type == ObjType::Path) return true;
    if (!canConvert(o)) return false;
    const QString label = objTypeLabel(o.type);
    if (o.type == ObjType::Text) {
        QPainterPath tp = o.textPath();
        o.fillOn = true;
        o.strokeOn = false;
        fromPainterPath(o, tp);
        o.textOnPath = false;
    } else if (o.type == ObjType::Line) {
        QPolygonF pts{o.p1, o.p2};
        o.type = ObjType::Path;
        o.points = pts;
        o.closed = false;
        o.hIn.clear(); o.hOut.clear(); o.contours.clear();
    } else if (o.type == ObjType::AutoShape) {
        bool closed = true;
        const QPainterPath body = autoShapeBody(o, nullptr, &closed);
        const QString lab = autoshape::label(o.shapeKind);
        fromPainterPath(o, body);
        if (!closed) o.closed = false;
        o.xform = QTransform();   // already applied to the outline
        o.shapeKind.clear();
        o.shapeParams.clear();
        if (o.name == lab) o.name.clear();
    } else {
        fromPainterPath(o, o.shapePath());
        o.xform = QTransform();   // shapePath() already includes the transform
    }
    if (o.name.isEmpty() || o.name == label) o.name = QStringLiteral("Path");
    return true;
}

QPainterPath area(const VObject &o) {
    QPainterPath p = o.type == ObjType::Text ? o.textPath() : o.shapePath();
    p.setFillRule(Qt::OddEvenFill);
    return p;
}

static void splitCubic(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p3, double t,
                       QPointF out[7]) {
    auto lerp = [](const QPointF &a, const QPointF &b, double u) { return a + (b - a) * u; };
    QPointF a = lerp(p0, c1, t), b = lerp(c1, c2, t), c = lerp(c2, p3, t);
    QPointF d = lerp(a, b, t), e = lerp(b, c, t);
    QPointF f = lerp(d, e, t);
    out[0] = p0; out[1] = a; out[2] = d; out[3] = f; out[4] = e; out[5] = c; out[6] = p3;
}

int splitSegment(VObject &o, int i, double t) {
    const int j = nextInContour(o, i);
    if (j < 0) return -1;
    o.ensureHandles();
    QPointF r[7];
    splitCubic(o.points[i], o.hOut[i], o.hIn[j], o.points[j], t, r);
    const bool straight = o.hOut[i] == o.points[i] && o.hIn[j] == o.points[j];
    o.hOut[i] = straight ? o.points[i] : r[1];
    o.hIn[j] = straight ? o.points[j] : r[5];
    const int at = i + 1; // insert after i (for the closing segment that is the contour's end)
    auto lerpArr = [&](QVector<double> &v) {
        if (v.size() == o.points.size()) v.insert(at, v[i] + (v[j] - v[i]) * t);
        else v.clear();
    };
    lerpArr(o.ptSize);
    lerpArr(o.ptOpacity);
    lerpArr(o.ptPressure);
    lerpArr(o.ptSpeed);
    lerpArr(o.ptTime);
    o.points.insert(at, r[3]);
    o.hIn.insert(at, straight ? r[3] : r[2]);
    o.hOut.insert(at, straight ? r[3] : r[4]);
    for (int &s : o.contours) if (s >= at && s > i) ++s;
    return at;
}

static QPolygonF flattenSegment(const VObject &o, int i, int j, int steps) {
    QPolygonF poly;
    const bool curves = o.hIn.size() == o.points.size() && o.hOut.size() == o.points.size()
                        && (o.hOut[i] != o.points[i] || o.hIn[j] != o.points[j]);
    if (!curves) { poly << o.points[i] << o.points[j]; return poly; }
    for (int k = 0; k <= steps; ++k) poly << bezierAt(o.points[i], o.hOut[i], o.hIn[j], o.points[j], double(k) / steps);
    return poly;
}

static double segDist(const QPointF &p, const QPointF &a, const QPointF &b, double *u) {
    const QPointF d = b - a;
    const double l2 = QPointF::dotProduct(d, d);
    double t = l2 > 0 ? qBound(0.0, QPointF::dotProduct(p - a, d) / l2, 1.0) : 0.0;
    if (u) *u = t;
    return QLineF(p, a + d * t).length();
}

bool nearestSegment(const VObject &o, const QPointF &p, int *seg, double *t, double *dist) {
    if (o.type != ObjType::Path || o.points.size() < 2) return false;
    double best = 1e300;
    for (int i = 0; i < o.points.size(); ++i) {
        const int j = nextInContour(o, i);
        if (j < 0) continue;
        const QPolygonF poly = flattenSegment(o, i, j, 32);
        for (int k = 0; k + 1 < poly.size(); ++k) {
            double u;
            const double d = segDist(p, poly[k], poly[k + 1], &u);
            if (d < best) {
                best = d;
                if (seg) *seg = i;
                if (t) *t = (k + u) / (poly.size() - 1);
            }
        }
    }
    if (dist) *dist = best;
    return best < 1e300;
}

double distanceToOutline(const VObject &o, const QPointF &p) {
    double d = 1e300;
    nearestSegment(o, p, nullptr, nullptr, &d);
    return d;
}

// ------------------------------------------------------------------- combine
static void copyAttributes(VObject &dst, const VObject &src) {
    const quint64 id = dst.id;
    const QPolygonF pts = dst.points, in = dst.hIn, out = dst.hOut;
    const QVector<int> cs = dst.contours;
    const bool cl = dst.closed;
    dst = src;
    dst.id = id;
    dst.type = ObjType::Path;
    dst.points = pts; dst.hIn = in; dst.hOut = out; dst.contours = cs; dst.closed = cl;
}

static void removeIds(Document &doc, const QSet<quint64> &ids) {
    for (auto &l : doc.frame().layers)
        l.erase(std::remove_if(l.begin(), l.end(), [&](const VObject &o) { return ids.contains(o.id); }), l.end());
}

static void appendContours(VObject &dst, const VObject &src) {
    dst.ensureHandles();
    VObject s = src;
    s.ensureHandles();
    const int base = dst.points.size();
    if (base > 0) dst.contours << base;
    for (int c : s.contours) dst.contours << base + c;
    dst.points << s.points;
    dst.hIn << s.hIn;
    dst.hOut << s.hOut;
}

bool combine(Document &doc, Combine op, QString *err) {
    QVector<VObject *> sel;
    for (VObject *o : doc.selectedEditable()) if (canConvert(*o)) sel << o;
    if (sel.size() < 2) {
        if (err) *err = QStringLiteral("Select two or more paths or shapes.");
        return false;
    }
    VObject *bottom = sel.first();
    VObject *top = sel.last();
    QSet<quint64> remove;
    QSet<quint64> newSel;
    switch (op) {
    case Combine::Union: case Combine::Intersect: {
        QPainterPath acc = area(*bottom);
        for (int i = 1; i < sel.size(); ++i)
            acc = op == Combine::Union ? acc.united(area(*sel[i])) : acc.intersected(area(*sel[i]));
        VObject res = *bottom;
        fromPainterPath(res, acc.simplified());
        copyAttributes(res, *bottom);
        res.name = QStringLiteral("Path");
        for (int i = 1; i < sel.size(); ++i) remove << sel[i]->id;
        *bottom = res;
        newSel << bottom->id;
        break;
    }
    case Combine::Join: {
        VObject res = *bottom;
        toPath(res);
        for (int i = 1; i < sel.size(); ++i) {
            VObject p = *sel[i];
            toPath(p);
            appendContours(res, p);
            res.closed = res.closed || p.closed;
            remove << sel[i]->id;
        }
        res.dropTrivialHandles();
        res.name = QStringLiteral("Composite Path");
        *bottom = res;
        newSel << bottom->id;
        break;
    }
    case Combine::Punch: case Combine::Crop: {
        const QPainterPath cutter = area(*top);
        remove << top->id;
        for (int i = 0; i + 1 < sel.size(); ++i) {
            VObject *o = sel[i];
            QPainterPath r = op == Combine::Punch ? area(*o).subtracted(cutter) : area(*o).intersected(cutter);
            if (r.isEmpty()) { remove << o->id; continue; }
            VObject res = *o;
            fromPainterPath(res, r.simplified());
            if (res.points.size() < 2) { remove << o->id; continue; }
            copyAttributes(res, *o);
            if (res.name == objTypeLabel(o->type) || res.name.isEmpty()) res.name = QStringLiteral("Path");
            *o = res;
            newSel << o->id;
        }
        break;
    }
    }
    removeIds(doc, remove);
    doc.setSelection(newSel);
    return true;
}

int split(Document &doc) {
    int n = 0;
    QSet<quint64> newSel = doc.selection;
    for (auto &l : doc.frame().layers) {
        for (int idx = 0; idx < l.size(); ++idx) {
            VObject &o = l[idx];
            if (!doc.selection.contains(o.id) || o.locked || o.type != ObjType::Path || o.contours.isEmpty()) continue;
            VObject src = o;
            src.ensureHandles();
            QVector<VObject> parts;
            for (int c = 0; c < src.contourCount(); ++c) {
                int a, b;
                src.contourRange(c, &a, &b);
                VObject p = src;
                p.points = src.points.mid(a, b - a);
                p.hIn = src.hIn.mid(a, b - a);
                p.hOut = src.hOut.mid(a, b - a);
                p.contours.clear();
                p.dropTrivialHandles();
                p.id = c == 0 ? src.id : doc.newId();
                if (c > 0) p.name = QStringLiteral("Path");
                parts << p;
                newSel << p.id;
            }
            l.remove(idx);
            for (int k = 0; k < parts.size(); ++k) l.insert(idx + k, parts[k]);
            idx += parts.size() - 1;
            ++n;
        }
    }
    if (n) doc.setSelection(newSel);
    return n;
}

bool attachTextToPath(Document &doc, QString *err) {
    VObject *text = nullptr, *guide = nullptr;
    int n = 0;
    for (VObject *o : doc.selectedEditable()) {
        ++n;
        if (o->type == ObjType::Text && !text) text = o;
        else if ((o->type == ObjType::Path || o->type == ObjType::Rect || o->type == ObjType::Ellipse || o->type == ObjType::Line) && !guide) guide = o;
    }
    if (n != 2 || !text || !guide) {
        if (err) *err = QStringLiteral("Select one text block and one path to attach it to.");
        return false;
    }
    VObject g = *guide;
    toPath(g);
    if (g.points.size() < 2) { if (err) *err = QStringLiteral("The path is too short."); return false; }
    text->points = g.points;
    text->hIn = g.hIn;
    text->hOut = g.hOut;
    text->contours = g.contours;
    text->closed = g.closed;
    text->textOnPath = true;
    text->textPathOffset = 0;
    const quint64 textId = text->id;
    int index = -1;
    if (QVector<VObject> *c = doc.containerOf(guide->id, nullptr, &index)) c->remove(index);
    doc.selectOnly(textId);
    return true;
}

int detachTextFromPath(Document &doc) {
    QVector<VObject> guides;
    for (VObject *o : doc.selectedEditable()) {
        if (o->type != ObjType::Text || !o->textOnPath) continue;
        VObject g;
        g.type = ObjType::Path;
        g.name = QStringLiteral("Path");
        g.points = o->points;
        g.hIn = o->hIn;
        g.hOut = o->hOut;
        g.contours = o->contours;
        g.closed = o->closed;
        g.fillOn = false;
        g.strokeOn = true;
        g.stroke = Qt::black;
        g.strokeWidth = 1;
        const QPointF tl = o->bounds().topLeft();
        o->textOnPath = false;
        o->points.clear(); o->hIn.clear(); o->hOut.clear(); o->contours.clear();
        o->closed = false;
        o->rect = QRectF(tl, QSizeF(10, 10));
        o->fitTextBox();
        guides << g;
    }
    const QSet<quint64> keep = doc.selection;
    for (const VObject &g : guides) doc.addObject(g, -1, false);
    doc.setSelection(keep);
    return int(guides.size());
}

int convertToPaths(Document &doc) {
    int n = 0;
    QVector<VObject> extra;   // auto-shape arrowheads become their own filled paths
    QVector<int> extraLayer;
    for (VObject *o : doc.selectedEditable()) {
        if (o->type == ObjType::Path || !canConvert(*o)) continue;
        if (o->type == ObjType::AutoShape) {
            QPainterPath heads;
            bool closed;
            autoShapeBody(*o, &heads, &closed);
            if (!heads.isEmpty() && o->strokeOn) {
                VObject h = *o;
                h.id = 0;
                h.effects = o->effects;
                fromPainterPath(h, heads);
                h.xform = QTransform();
                h.shapeKind.clear(); h.shapeParams.clear();
                h.fillOn = true; h.fill = o->stroke; h.fillKind = FillKind::Solid; h.strokeOn = false;
                h.name = QStringLiteral("Arrowheads");
                int layer = -1;
                doc.find(o->id, &layer);
                extra << h; extraLayer << layer;
            }
        }
        toPath(*o);
        ++n;
    }
    for (int i = 0; i < extra.size(); ++i) doc.addObject(extra[i], extraLayer[i], false);
    return n;
}

// Ramer-Douglas-Peucker
static void rdp(const QPolygonF &p, int a, int b, double tol, QVector<bool> &keep) {
    if (b <= a + 1) return;
    double best = -1; int idx = -1;
    for (int i = a + 1; i < b; ++i) {
        const double d = segDist(p[i], p[a], p[b], nullptr);
        if (d > best) { best = d; idx = i; }
    }
    if (best > tol) {
        keep[idx] = true;
        rdp(p, a, idx, tol, keep);
        rdp(p, idx, b, tol, keep);
    }
}

QPolygonF simplifyPolyline(const QPolygonF &pts, double tol, bool closed) {
    if (pts.size() < 3) return pts;
    QPolygonF p = pts;
    if (closed) p << pts.first();
    QVector<bool> keep(p.size(), false);
    keep[0] = keep[p.size() - 1] = true;
    if (closed) { // split at the point farthest from the start so a closed shape keeps its extent
        int far = 0; double fd = -1;
        for (int i = 1; i < pts.size(); ++i) { double d = QLineF(pts[0], pts[i]).length(); if (d > fd) { fd = d; far = i; } }
        keep[far] = true;
        rdp(p, 0, far, tol, keep);
        rdp(p, far, p.size() - 1, tol, keep);
    } else rdp(p, 0, p.size() - 1, tol, keep);
    QPolygonF out;
    for (int i = 0; i < p.size(); ++i) if (keep[i]) out << p[i];
    if (closed) out.removeLast();
    return out;
}

static QPolygonF flattenContour(const VObject &o, int c, double spacing) {
    int a, b;
    o.contourRange(c, &a, &b);
    QPolygonF poly;
    for (int i = a; i < b; ++i) {
        const int j = nextInContour(o, i);
        if (j < 0) { poly << o.points[i]; break; }
        const QPolygonF seg = flattenSegment(o, i, j, 48);
        double len = 0;
        for (int k = 0; k + 1 < seg.size(); ++k) len += QLineF(seg[k], seg[k + 1]).length();
        const int steps = qMax(1, int(std::ceil(len / qMax(0.5, spacing))));
        const bool curve = seg.size() > 2;
        for (int s = 0; s < steps; ++s) {
            const double t = double(s) / steps;
            poly << (curve ? bezierAt(o.points[i], o.hOut[i], o.hIn[j], o.points[j], t) : o.points[i] + (o.points[j] - o.points[i]) * t);
        }
    }
    return poly;
}

static void setPolylines(VObject &o, const QVector<QPolygonF> &cs) {
    o.points.clear(); o.hIn.clear(); o.hOut.clear(); o.contours.clear();
    o.clearPointPressure();
    o.clearPointSense();
    for (const QPolygonF &c : cs) {
        if (c.size() < 2) continue;
        if (!o.points.isEmpty()) o.contours << o.points.size();
        o.points << c;
    }
}

int simplify(Document &doc, double amount) {
    int n = 0;
    for (VObject *o : doc.selectedEditable()) {
        if (!canConvert(*o) || o->type == ObjType::Line) continue;
        toPath(*o);
        QVector<QPolygonF> cs;
        for (int c = 0; c < o->contourCount(); ++c) {
            QPolygonF f = flattenContour(*o, c, 1.0);
            QPolygonF s = simplifyPolyline(f, qMax(0.01, amount), o->closed);
            if (o->closed && s.size() < 3) continue;
            cs << s;
        }
        if (cs.isEmpty()) continue;
        setPolylines(*o, cs);
        ++n;
    }
    return n;
}

int expandStroke(Document &doc, double width, Qt::PenJoinStyle join, Qt::PenCapStyle cap, double miter) {
    int n = 0;
    for (VObject *o : doc.selectedEditable()) {
        if (!canConvert(*o)) continue;
        QPainterPathStroker st;
        st.setWidth(qMax(0.1, width));
        st.setJoinStyle(join);
        st.setCapStyle(cap);
        st.setMiterLimit(miter);
        QPainterPath src = o->type == ObjType::Text ? o->textPath() : o->shapePath();
        QPainterPath r = st.createStroke(src);
        r.setFillRule(Qt::WindingFill);
        r = r.simplified();
        const QColor c = o->strokeOn ? o->stroke : o->fill;
        fromPainterPath(*o, r);
        if (o->points.size() < 2) continue;
        o->fillOn = true;
        o->fill = c;
        o->strokeOn = false;
        if (o->name.isEmpty() || o->name == objTypeLabel(ObjType::Line) || o->name == objTypeLabel(ObjType::Rect)
            || o->name == objTypeLabel(ObjType::Ellipse)) o->name = QStringLiteral("Path");
        ++n;
    }
    return n;
}

int insetPath(Document &doc, double width, bool inside) {
    int n = 0;
    for (VObject *o : doc.selectedEditable()) {
        if (!canConvert(*o) || o->type == ObjType::Line) continue;
        QPainterPath a = area(*o);
        QPainterPathStroker st;
        st.setWidth(qMax(0.1, 2 * width));
        st.setJoinStyle(Qt::MiterJoin);
        QPainterPath ring = st.createStroke(a);
        ring.setFillRule(Qt::WindingFill);
        QPainterPath r = inside ? a.subtracted(ring.simplified()) : a.united(ring.simplified());
        if (r.isEmpty()) continue;
        const QColor f = o->fill;
        const bool wasText = o->type == ObjType::Text;
        fromPainterPath(*o, r.simplified());
        if (wasText) { o->fillOn = true; o->fill = f; o->strokeOn = false; }
        if (o->name.isEmpty() || o->name == objTypeLabel(ObjType::Rect) || o->name == objTypeLabel(ObjType::Ellipse)) o->name = QStringLiteral("Path");
        ++n;
    }
    return n;
}

// ------------------------------------------------------------------- knife
static bool segIntersect(const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d, double *ta) {
    const QPointF r = b - a, s = d - c;
    const double den = r.x() * s.y() - r.y() * s.x();
    if (std::abs(den) < 1e-12) return false;
    const QPointF q = c - a;
    const double t = (q.x() * s.y() - q.y() * s.x()) / den;
    const double u = (q.x() * r.y() - q.y() * r.x()) / den;
    if (t < 0 || t > 1 || u < 0 || u > 1) return false;
    *ta = t;
    return true;
}

int knife(Document &doc, const QPolygonF &cut) {
    if (cut.size() < 2) return 0;
    int cutCount = 0;
    QSet<quint64> newSel;
    for (auto &l : doc.frame().layers) {
        for (int idx = 0; idx < l.size(); ++idx) {
            if (!doc.selection.contains(l[idx].id) || l[idx].locked || !canConvert(l[idx])) continue;
            VObject o = l[idx];
            toPath(o);
            o.ensureHandles();
            // find crossings per segment (segment start index -> list of t)
            QMap<int, QVector<double>> hits;
            QPolygonF nodeCuts; // cuts passing (almost) exactly through an existing node
            for (int i = 0; i < o.points.size(); ++i) {
                const int j = nextInContour(o, i);
                if (j < 0) continue;
                const QPolygonF seg = flattenSegment(o, i, j, 64);
                const int ns = seg.size() - 1;
                for (int k = 0; k < ns; ++k)
                    for (int m = 0; m + 1 < cut.size(); ++m) {
                        double ta;
                        if (segIntersect(seg[k], seg[k + 1], cut[m], cut[m + 1], &ta)) {
                            const double t = (k + ta) / ns;
                            if (t > 1e-4 && t < 1 - 1e-4) hits[i] << t;
                            else nodeCuts << (t <= 1e-4 ? o.points[i] : o.points[j]);
                        }
                    }
            }
            if (hits.isEmpty() && nodeCuts.isEmpty()) { newSel << o.id; continue; }
            // split segments, last segment first so earlier indices stay valid
            QVector<bool> isCut(o.points.size(), false);
            QList<int> segs = hits.keys();
            std::sort(segs.begin(), segs.end(), std::greater<int>());
            for (int i : segs) {
                QVector<double> ts = hits[i];
                std::sort(ts.begin(), ts.end());
                ts.erase(std::unique(ts.begin(), ts.end(), [](double a, double b) { return std::abs(a - b) < 1e-4; }), ts.end());
                // split from the last t backwards: splitting at t keeps [0,t] at index i
                for (int k = ts.size() - 1; k >= 0; --k) {
                    const double t = k == ts.size() - 1 ? ts[k] : ts[k] / ts[k + 1];
                    const int at = splitSegment(o, i, t);
                    isCut.insert(at, true);
                }
            }
            for (const QPointF &np : nodeCuts)
                for (int k = 0; k < o.points.size(); ++k)
                    if (QLineF(o.points[k], np).length() < 1e-6) isCut[k] = true;
            // break contours at cut points
            QVector<VObject> pieces;
            VObject rest = o;
            rest.points.clear(); rest.hIn.clear(); rest.hOut.clear(); rest.contours.clear();
            auto piece = [&](const QVector<int> &ids) {
                VObject p = o;
                p.points.clear(); p.hIn.clear(); p.hOut.clear(); p.contours.clear();
                for (int k : ids) { p.points << o.points[k]; p.hIn << o.hIn[k]; p.hOut << o.hOut[k]; }
                p.hIn.first() = p.points.first();
                p.hOut.last() = p.points.last();
                p.closed = false;
                p.dropTrivialHandles();
                if (p.points.size() >= 2) pieces << p;
            };
            for (int c = 0; c < o.contourCount(); ++c) {
                int a, b;
                o.contourRange(c, &a, &b);
                QVector<int> cuts;
                for (int k = a; k < b; ++k) if (isCut[k]) cuts << k;
                if (cuts.isEmpty()) {
                    VObject tmp = o;
                    tmp.points = o.points.mid(a, b - a); tmp.hIn = o.hIn.mid(a, b - a); tmp.hOut = o.hOut.mid(a, b - a);
                    tmp.contours.clear();
                    appendContours(rest, tmp);
                    continue;
                }
                if (o.closed) {
                    const int n = b - a;
                    for (int q = 0; q < cuts.size(); ++q) {
                        const int from = cuts[q];
                        const int to = cuts[(q + 1) % cuts.size()];
                        QVector<int> ids;
                        int k = from;
                        do { ids << k; k = a + (k - a + 1) % n; } while (k != to);
                        ids << to;
                        piece(ids);
                    }
                } else {
                    QVector<int> ids;
                    for (int k = a; k < b; ++k) {
                        ids << k;
                        if (isCut[k]) { piece(ids); ids = {k}; }
                    }
                    piece(ids);
                }
            }
            QVector<VObject> out;
            if (!rest.points.isEmpty()) { rest.dropTrivialHandles(); out << rest; }
            out << pieces;
            for (int k = 0; k < out.size(); ++k) {
                out[k].id = k == 0 ? o.id : doc.newId();
                out[k].name = QStringLiteral("Path");
                newSel << out[k].id;
            }
            l.remove(idx);
            for (int k = 0; k < out.size(); ++k) l.insert(idx + k, out[k]);
            idx += out.size() - 1;
            ++cutCount;
        }
    }
    if (cutCount) doc.setSelection(newSel);
    return cutCount;
}

// ------------------------------------------------------------------- freeform
void densify(VObject &o, double spacing) {
    if (!toPath(o)) return;
    QVector<QPolygonF> cs;
    for (int c = 0; c < o.contourCount(); ++c) cs << flattenContour(o, c, spacing);
    setPolylines(o, cs);
}

void refine(VObject &o, double maxLen) {
    if (o.type != ObjType::Path || o.hasCurves()) return;
    o.hIn.clear(); o.hOut.clear();
    for (int i = 0; i < o.points.size(); ++i) {
        const int j = nextInContour(o, i);
        if (j < 0) continue;
        if (QLineF(o.points[i], o.points[j]).length() > maxLen) {
            const QPointF m = (o.points[i] + o.points[j]) / 2;
            auto midArr = [&](QVector<double> &v) {
                if (v.size() == o.points.size()) v.insert(i + 1, (v[i] + v[j]) / 2);
            };
            midArr(o.ptSize);
            midArr(o.ptOpacity);
            midArr(o.ptPressure);
            midArr(o.ptTime);
            midArr(o.ptSpeed);
            o.points.insert(i + 1, m);
            for (int &s : o.contours) if (s > i) ++s;
            --i; // re-check the first half
        }
    }
}

bool push(VObject &o, const QPointF &c, double r) {
    bool moved = false;
    for (int i = 0; i < o.points.size(); ++i) {
        const QPointF d = o.points[i] - c;
        const double len = std::hypot(d.x(), d.y());
        if (len >= r || len < 1e-9) continue;
        const QPointF np = c + d * (r / len);
        const QPointF delta = np - o.points[i];
        o.points[i] = np;
        if (i < o.hIn.size()) o.hIn[i] += delta;
        if (i < o.hOut.size()) o.hOut[i] += delta;
        moved = true;
    }
    return moved;
}

bool pull(VObject &o, const QPointF &c, const QPointF &delta, double r, double strength) {
    bool moved = false;
    for (int i = 0; i < o.points.size(); ++i) {
        const double d = QLineF(o.points[i], c).length();
        if (d >= r) continue;
        const double f = 1 - (d / r) * (d / r);
        const QPointF m = delta * (f * f * strength);
        o.points[i] += m;
        if (i < o.hIn.size()) o.hIn[i] += m;
        if (i < o.hOut.size()) o.hOut[i] += m;
        moved = true;
    }
    return moved;
}


// ------------------------------------------------------------- path scrubber
static double ctrlLength(const VObject &o, int i, int j) {
    const bool curves = o.hIn.size() == o.points.size() && o.hOut.size() == o.points.size()
                        && (o.hOut[i] != o.points[i] || o.hIn[j] != o.points[j]);
    if (!curves) return QLineF(o.points[i], o.points[j]).length();
    return (QLineF(o.points[i], o.hOut[i]).length() + QLineF(o.hOut[i], o.hIn[j]).length() + QLineF(o.hIn[j], o.points[j]).length()
            + QLineF(o.points[i], o.points[j]).length()) / 2;
}

QVector<QVector<PressureSample>> pressureSamples(const VObject &o, double step) {
    QVector<QVector<PressureSample>> out;
    if (o.type != ObjType::Path || o.points.size() < 2) return out;
    step = std::isfinite(step) ? qBound(0.05, step, 1e4) : 1.0;
    const bool curves = o.hIn.size() == o.points.size() && o.hOut.size() == o.points.size();
    for (int c = 0; c < o.contourCount(); ++c) {
        int a, b;
        o.contourRange(c, &a, &b);
        if (b - a < 2) continue;
        QVector<PressureSample> cs;
        auto seg = [&](int i, int j) {
            const double len = ctrlLength(o, i, j);
            const int n = qBound(1, int(std::ceil(len / step)), 100000);
            const bool curved = curves && (o.hOut[i] != o.points[i] || o.hIn[j] != o.points[j]);
            for (int k = cs.isEmpty() ? 0 : 1; k <= n; ++k) {
                const double t = double(k) / n;
                PressureSample s;
                s.p = curved ? bezierAt(o.points[i], o.hOut[i], o.hIn[j], o.points[j], t) : o.points[i] + (o.points[j] - o.points[i]) * t;
                s.size = o.pointSize(i) + (o.pointSize(j) - o.pointSize(i)) * t;
                s.opacity = o.pointOpacity(i) + (o.pointOpacity(j) - o.pointOpacity(i)) * t;
                cs << s;
            }
        };
        for (int i = a; i + 1 < b; ++i) seg(i, i + 1);
        if (o.closed) seg(b - 1, a);
        out << cs;
    }
    return out;
}

bool subdivideForPressure(VObject &o, double maxLen, int maxPoints) {
    if (o.type != ObjType::Path || o.points.size() < 2 || !std::isfinite(maxLen)) return false;
    maxLen = qMax(0.5, maxLen);
    bool any = false;
    for (int i = 0; i < o.points.size() && o.points.size() < maxPoints; ++i) {
        const int j = nextInContour(o, i);
        if (j < 0) continue;
        const double len = ctrlLength(o, i, j);
        if (len <= maxLen) continue;
        const int k = qMin(int(std::ceil(len / maxLen)), maxPoints);
        if (splitSegment(o, i, 1.0 / k) >= 0) any = true;   // the rest (i + 1 -> j) is split on the next pass
    }
    if (any) o.dropTrivialHandles();
    return any;
}

bool scrub(VObject &o, const QPointF &c, double radius, double delta, bool size, bool opacity) {
    if (o.type != ObjType::Path || (!size && !opacity) || !(radius > 0) || !std::isfinite(delta) || delta == 0) return false;
    bool changed = false;
    for (int i = 0; i < o.points.size(); ++i) {
        const double d = QLineF(o.points[i], c).length();
        if (d >= radius) continue;
        const double f = 1 - (d / radius) * (d / radius);
        const double dv = delta * f * f;
        o.ensurePointPressure();
        if (size) {
            const double v = qBound(0.0, o.pointSize(i) + dv * VObject::kMaxPointSize / 2, VObject::kMaxPointSize);
            if (v != o.ptSize[i]) { o.ptSize[i] = v; changed = true; }
        }
        if (opacity) {
            const double v = qBound(0.0, o.pointOpacity(i) + dv, 1.0);
            if (v != o.ptOpacity[i]) { o.ptOpacity[i] = v; changed = true; }
        }
    }
    return changed;
}

QVector<TaperPiece> taperOutline(const VObject &o, double halfWidth) {
    QVector<TaperPiece> out;
    const auto all = pressureSamples(o, qBound(0.5, halfWidth / 2, 4.0));
    for (const auto &cs : all) {
        const int n = cs.size();
        if (n < 2) continue;
        QVector<QPointF> nrm(n);
        for (int k = 0; k < n; ++k) {
            const QPointF t = cs[qMin(n - 1, k + 1)].p - cs[qMax(0, k - 1)].p;
            const double l = std::hypot(t.x(), t.y());
            nrm[k] = l > 1e-9 ? QPointF(-t.y() / l, t.x() / l) : (k ? nrm[k - 1] : QPointF(0, 1));
        }
        auto q = [](double v) { return std::round(qBound(0.0, v, 1.0) * 32) / 32; };
        int s = 0;
        while (s < n - 1) {
            int e = s + 1;
            const double op = q((cs[s].opacity + cs[s + 1].opacity) / 2);
            while (e < n - 1 && q((cs[e].opacity + cs[e + 1].opacity) / 2) == op) ++e;
            QPolygonF poly;
            for (int k = s; k <= e; ++k) poly << cs[k].p + nrm[k] * (halfWidth * cs[k].size);
            for (int k = e; k >= s; --k) poly << cs[k].p - nrm[k] * (halfWidth * cs[k].size);
            QPainterPath pp;
            pp.setFillRule(Qt::WindingFill);
            pp.addPolygon(poly);
            pp.closeSubpath();
            // round ends of an open contour (and soften the joints between runs)
            for (int k : {s, e}) {
                const double r = halfWidth * cs[k].size;
                if (r > 0.01 && (!o.closed || (k != 0 && k != n - 1))) pp.addEllipse(cs[k].p, r, r);
            }
            if (op > 0) out << TaperPiece{pp, op};
            s = e;
        }
    }
    return out;
}

}
