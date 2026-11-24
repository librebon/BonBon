#pragma once
#include <QMap>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

struct VObject;

// Auto Shapes (v0.3.0): parametric vector shapes in the spirit of Fireworks MX 2004's Auto Shapes.
// An AutoShape object keeps its kind and parameters (VObject::shapeKind / shapeParams) and is drawn
// from them, so it stays an editable vector: the parameters are edited in the Property Inspector or by
// dragging the yellow diamond handles on the canvas. Geometry lives in VObject::rect (untransformed;
// rotation / skew go into VObject::xform like rectangles). Modify > Convert to Paths turns it into paths.
namespace autoshape {

struct Param {
    QString key, label;
    double min = 0, max = 100, def = 0;
    int decimals = 0;
    QStringList choices;   // non-empty: an enumerated parameter (index stored as a double)
    QString suffix;
};

QStringList kinds();                        // keys, UI order
QString label(const QString &kind);         // "Rounded Rectangle"
QString tip(const QString &kind);           // one-line description
bool isKind(const QString &kind);
QVector<Param> params(const QString &kind);
QMap<QString, double> defaults(const QString &kind);
bool isLineKind(const QString &kind);       // drawn as an open stroke (line, connector, spiral, open arc)
double param(const VObject &o, const QString &key);
void setParam(VObject &o, const QString &key, double v);   // clamped to the parameter's range

// Geometry in the object's untransformed coordinates.
struct Geometry {
    QPainterPath fill;   // closed area: filled with the fill, stroked with the stroke
    QPainterPath open;   // open strokes (lines, spiral, open arcs, open arrow heads): stroked only
    QPainterPath heads;  // solid arrow heads: filled with the stroke colour
};
Geometry geometry(const VObject &o);
Geometry geometry(const QString &kind, const QMap<QString, double> &params, const QRectF &rect);
QPainterPath outline(const VObject &o);     // everything (shapePath / hit test / bounds)
QPainterPath previewPath(const QString &kind, const QRectF &r); // defaults, for icons and the panel

// Yellow diamond handles, untransformed coordinates.
struct Handle { int id = 0; QPointF pos; QString tip; };
QVector<Handle> handles(const VObject &o);
// Drag handle `id` to `p` (untransformed coordinates). `alt`: the Fireworks Alt modifier
// (rounded rectangle: change only this corner).
void dragHandle(VObject &o, int id, const QPointF &p, bool alt = false);

// Make `o` an auto shape of `kind` with the default parameters (keeps rect, fill and stroke).
void init(VObject &o, const QString &kind);
// For line kinds: set the end points (document coordinates); rect becomes their bounding box.
void setEndpoints(VObject &o, const QPointF &a, const QPointF &b);
QPointF endpoint(const VObject &o, int which);  // line kinds: 0 start, 1 end
}
