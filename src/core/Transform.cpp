#include "Transform.h"
#include <cmath>
#include "Fills.h"
#include "Render.h"
#include <QPainter>
#include <QtMath>

namespace xf {

static bool near0(double v) { return qAbs(v) < 1e-9; }

bool isAxisAligned(const QTransform &t) {
    if (t.type() == QTransform::TxProject) return false;
    return (near0(t.m12()) && near0(t.m21())) || (near0(t.m11()) && near0(t.m22()));
}

QPolygonF corners(const QRectF &r) { return QPolygonF({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}); }

QTransform quadMap(const QPolygonF &from, const QPolygonF &to, bool *ok) {
    QTransform t;
    const bool good = from.size() == 4 && to.size() == 4 && QTransform::quadToQuad(from, to, t);
    if (ok) *ok = good;
    return good ? t : QTransform();
}

QTransform rotationAbout(const QPointF &c, double degrees) {
    QTransform r;
    r.rotate(degrees);   // y down: positive = clockwise on screen
    return QTransform::fromTranslate(-c.x(), -c.y()) * r * QTransform::fromTranslate(c.x(), c.y());
}

double rotationOf(const VObject &o) {
    if (!o.hasXform()) return 0;
    double a = qRadiansToDegrees(qAtan2(o.xform.m12(), o.xform.m11()));
    if (qAbs(a) < 1e-9) a = 0;
    return a;
}

void transformObject(VObject &o, const QTransform &t) {
    if (t.isIdentity()) return;
    if (o.type == ObjType::Group) {
        // a group transforms as a unit: each member through the same map (a linked mask too)
        for (VObject &c : o.children) transformObject(c, t);
        if (o.maskLinked) for (VObject &m : o.maskObj) transformObject(m, t);
        return;
    }
    const bool axis = isAxisAligned(t);
    if (o.usesXform() && (o.hasXform() || !axis || o.type == ObjType::Instance)) {
        // non-destructive: the object's own geometry stays as it is
        o.xform = o.xform * t;
        if (o.xform.isIdentity()) o.xform = QTransform();
        return;
    }
    // gradient handles are stored relative to the bounds: carry them through the transform
    // (an untouched Contour keeps its default handles: its look follows the outline by itself)
    const bool grad = o.isVector() && fills::isGradient(o.fillKind)
                   && !(o.fillKind == FillKind::Contour && fills::contourHandlesDefault(o));
    QPointF gh[3];
    if (grad) { gh[0] = fills::handleToDoc(o, o.gradP0); gh[1] = fills::handleToDoc(o, o.gradP1); gh[2] = fills::handleToDoc(o, o.gradP2); }
    switch (o.type) {
    case ObjType::Line: o.p1 = t.map(o.p1); o.p2 = t.map(o.p2); break;
    case ObjType::Path: o.points = t.map(o.points); o.hIn = t.map(o.hIn); o.hOut = t.map(o.hOut); break;
    case ObjType::Text:   // text on a path (other text took the non-destructive branch or is axis-aligned)
        if (o.textOnPath) { o.points = t.map(o.points); o.hIn = t.map(o.hIn); o.hOut = t.map(o.hOut); break; }
        if (!(near0(t.m12()) && near0(t.m21())) || t.m11() < 0 || t.m22() < 0) { o.xform = t; break; }
        o.rect = t.mapRect(o.rect);
        break;
    case ObjType::Bitmap: {
        // flips and quarter turns are exact on the pixel grid: bake them
        QTransform lin(t.m11(), t.m12(), t.m21(), t.m22(), 0, 0);
        const QRectF r = t.mapRect(o.rect);
        const bool unit = qFuzzyCompare(qAbs(t.m11()) + qAbs(t.m12()), 1.0) && qFuzzyCompare(qAbs(t.m21()) + qAbs(t.m22()), 1.0);
        if (!unit) { o.xform = t; break; }
        o.image = o.image.transformed(lin, Qt::FastTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        o.rect = r;
        break;
    }
    case ObjType::Slice:
    case ObjType::Hotspot:
        if (o.webShape == WebShape::Polygon) { o.points = t.map(o.points); o.rect = o.points.boundingRect(); break; }
        o.rect = t.mapRect(o.rect);
        break;
    default: o.rect = t.mapRect(o.rect); break;
    }
    if (grad) {
        o.gradP0 = fills::docToHandle(o, t.map(gh[0]));
        o.gradP1 = fills::docToHandle(o, t.map(gh[1]));
        o.gradP2 = fills::docToHandle(o, t.map(gh[2]));
    }
}

bool bakeBitmap(VObject &o) {
    if (o.type != ObjType::Bitmap || !o.hasXform() || o.image.isNull()) return false;
    // an axis-aligned scale is drawn snapped to whole pixels (Render.cpp): bake the same pixels
    const QRectF b = o.bounds();
    const bool axis = o.xform.type() <= QTransform::TxScale;
    QRect area = b.toAlignedRect();
    if (axis) {
        const int l = int(std::round(b.left())), t = int(std::round(b.top()));
        area = QRect(l, t, qMax(1, int(std::round(b.right())) - l), qMax(1, int(std::round(b.bottom())) - t));
    }
    if (area.isEmpty()) return false;
    QImage img(area.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    {
        QPainter p(&img);
        p.translate(-area.topLeft());
        VObject c = o;
        c.effects.clear();
        c.quickMask = false;
        c.opacity = 100;
        c.blend = BlendMode::Normal;
        c.visible = true;
        render::drawBase(p, c);
    }
    o.image = img;
    o.rect = QRectF(area);
    o.xform = QTransform();
    return true;
}

bool removeTransform(VObject &o) {
    if (o.type == ObjType::Group) {
        bool any = false;
        for (VObject &c : o.children) any |= removeTransform(c);
        if (o.maskLinked) for (VObject &m : o.maskObj) any |= removeTransform(m);
        return any;
    }
    if (!o.hasXform()) return false;
    const QPointF c = o.bounds().center();
    o.xform = QTransform();
    const QPointF c0 = o.bounds().center();
    o.moveBy(c - c0);
    return true;
}

} // namespace xf
