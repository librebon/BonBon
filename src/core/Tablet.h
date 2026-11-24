#pragma once
// Graphics tablet support (v0.3.1): pen samples, the pressure curve and interpolation between events.
// A mouse is a pen that always presses at 1.0 with no tilt.
#include <QPointF>
#include <QString>
#include <QVector>

namespace tablet {

struct Sample {
    double pressure = 1.0;   // 0..1 (after the pressure curve when it comes from Canvas)
    double tiltX = 0;        // degrees, -60..60 (Qt's xTilt / yTilt)
    double tiltY = 0;
    double rotation = 0;     // degrees, barrel rotation (art pens), 0 when not reported
    bool hasTilt = false;    // the device reports tilt or rotation
};

// The Preferences > Tablet curve: control points in [0,1]^2 (x = raw pressure, y = output), sorted by x.
// A monotone cubic (Fritsch-Carlson) runs through them, so the curve never overshoots or turns back.
QVector<QPointF> defaultCurve();                       // (0,0) (1,1): linear
QVector<QPointF> normalizedCurve(const QVector<QPointF> &pts);  // clamped, sorted, end points at x = 0 and 1
double mapCurve(const QVector<QPointF> &pts, double p);
QString encodeCurve(const QVector<QPointF> &pts);      // "x,y;x,y"
QVector<QPointF> decodeCurve(const QString &s);        // defaultCurve() when invalid

// Linear interpolation between two samples (rotation along the shorter arc).
Sample lerp(const Sample &a, const Sample &b, double t);
// Calligraphic nib angle in radians: the barrel rotation when the pen reports one, otherwise the tilt
// direction (azimuth) when the pen is tilted by more than a few degrees, otherwise `fallback`.
double nibAngle(const Sample &s, double fallback);
// value = minimum + (1 - minimum) * pressure, for the size and opacity floors (minimum 0..1)
inline double withFloor(double pressure, double minimum) { return minimum + (1.0 - minimum) * pressure; }
}
