#pragma once
#include <QRectF>
#include <cmath>

// One device-pixel-aligned target rectangle for a document of size docSize shown at `zoom`
// with its top-left at `origin` (widget coordinates).  The transparency checkerboard is clipped
// to exactly this rectangle and the image is drawn into exactly the same rectangle, so the two
// can never disagree by a fractional pixel (the old "checkerboard sliver" along the
// bottom/right edges whose width changed with zoom).
inline double snapToDevice(double v, double dpr) { return std::round(v * dpr) / dpr; }

inline QRectF snappedTarget(QPointF origin, QSizeF docSize, double zoom, double dpr = 1.0) {
    if (dpr <= 0) dpr = 1.0;
    const double l = snapToDevice(origin.x(), dpr), t = snapToDevice(origin.y(), dpr);
    const double r = snapToDevice(origin.x() + docSize.width() * zoom, dpr);
    const double b = snapToDevice(origin.y() + docSize.height() * zoom, dpr);
    return QRectF(QPointF(l, t), QPointF(qMax(l, r), qMax(t, b)));
}

// Map a document x/y coordinate into the snapped target so sub-rectangles of the document
// (partial redraws) land on the same pixel edges as the full target.
inline double snappedCoord(double origin, double docCoord, double zoom, double dpr = 1.0) {
    return snapToDevice(origin + docCoord * zoom, dpr <= 0 ? 1.0 : dpr);
}
