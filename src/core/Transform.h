#pragma once
#include "Object.h"
#include <QPolygonF>
#include <QTransform>

// Object transforms: Free Transform, Scale, Skew, Distort, Perspective, rotation by any angle,
// Numeric Transform, Remove Transformations, Rotate Canvas.
namespace xf {

// True when t only flips / rotates by multiples of 90 degrees / scales along the axes and
// translates (rectangles stay axis-aligned rectangles).
bool isAxisAligned(const QTransform &t);
// Map an object through t (canvas coordinates). Paths, lines, text on a path and polygon
// hotspots bake t into their points; slices and rect hotspots take the bounds of the result;
// rectangles, ellipses, text, bitmaps and symbol instances compose t into their own transform
// (non-destructive) unless t keeps them axis-aligned and they have none yet.
void transformObject(VObject &o, const QTransform &t);
// Resample a transformed bitmap into a plain axis-aligned one (before pixel editing).
// Returns true when something changed.
bool bakeBitmap(VObject &o);
// Modify > Transform > Remove Transformations: drop the object's own transform, keeping it
// centred where it was. Returns false when there was nothing to remove.
bool removeTransform(VObject &o);
// The object's own rotation in degrees (clockwise on screen, 0 when untransformed).
double rotationOf(const VObject &o);
// Rotation by `degrees` (clockwise on screen) about c.
QTransform rotationAbout(const QPointF &c, double degrees);
// The four corners of r, clockwise from the top left.
QPolygonF corners(const QRectF &r);
// The projective map taking quad `from` onto quad `to` (4 points each); identity when degenerate.
QTransform quadMap(const QPolygonF &from, const QPolygonF &to, bool *ok = nullptr);

} // namespace xf
