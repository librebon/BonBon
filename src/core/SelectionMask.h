#pragma once
// Raster operations on bitmap selections. A selection mask is a Format_Alpha8 image: 255 fully
// selected, 0 not selected, anything between partly selected (feathered / anti-aliased).
#include <QImage>
#include <QPainterPath>
#include <QRect>

namespace selmask {
// An empty (all zero) Alpha8 mask
QImage blank(QSize size);
// Rasterise a path (anti-aliased or not) into a mask of `size`, `origin` = canvas position of pixel (0,0)
QImage fromPath(const QPainterPath &p, QSize size, QPoint origin, bool antiAlias);
// Bounds of the non-zero pixels (empty when none)
QRect bounds(const QImage &mask);
// The outline marching ants follow: pixels >= 128 (or > 0 when nothing reaches 50%), in canvas coordinates
QPainterPath outline(const QImage &mask, QPoint origin);
// Combine: max (add), a*(1-b) (subtract), min (intersect). `b` is at the same size/origin as `a`.
QImage unite(const QImage &a, const QImage &b);
QImage subtract(const QImage &a, const QImage &b);
QImage intersect(const QImage &a, const QImage &b);
QImage invert(const QImage &a);
// Modify > Feather: gaussian blur over `radius` px (the edge fades over about the radius)
QImage feather(const QImage &m, double radius);
// Expand / Contract by `px` (distance transform; edges stay anti-aliased)
QImage expand(const QImage &m, double px);
QImage contract(const QImage &m, double px);
// A band `width` px wide centred on the selection's edge
QImage border(const QImage &m, double width);
// Rounds off jaggies and corners: blur by `radius`, then threshold at 50%
QImage smooth(const QImage &m, double radius);
// Pixels of `img` whose colour is within `tolerance` (0..255, per channel) of a colour under the
// selection (mask >= 128); `img` and `m` share the same pixel grid. Returns a mask of img's size.
QImage similar(const QImage &img, const QImage &m, int tolerance);
// Quick Mask: a mask as an editable greyscale image (white = selected) and back again
QImage toGrey(const QImage &m);
QImage fromGrey(const QImage &grey);
}
