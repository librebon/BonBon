#pragma once
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
#include <QVector>

struct VObject;
class Document;

// Bezier path editing (Fireworks MX 2004 Modify > Combine Path / Alter Path, Knife, Freeform,
// Reshape Area). Document operations act on the editable selection of the current frame and do
// not commit; callers commit with a label.
namespace paths {

QPainterPath nodesToPath(const VObject &o);
// Replace o's geometry by `p` (o becomes a Path; curves are kept as handles).
void fromPainterPath(VObject &o, const QPainterPath &p);
bool canConvert(const VObject &o);   // rect, ellipse, line, text, path
bool toPath(VObject &o);             // converts o in place (keeps fill/stroke/effects)
QPainterPath area(const VObject &o); // filled area used by the combine operations

// cubic Bezier helpers
QPointF bezierAt(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p3, double t);
// splits segment i -> i+1 (or the closing segment) of o at t; returns the new point's index
int splitSegment(VObject &o, int i, double t);
// segment endpoints: index of the point after i in its contour (-1 if i ends an open contour)
int nextInContour(const VObject &o, int i);
double distanceToOutline(const VObject &o, const QPointF &p);
// nearest point on the outline: segment start index and t
bool nearestSegment(const VObject &o, const QPointF &p, int *seg, double *t, double *dist);

enum class Combine { Union, Intersect, Punch, Crop, Join };
bool combine(Document &doc, Combine op, QString *err = nullptr);
int split(Document &doc);                    // compound paths -> one path per contour
int convertToPaths(Document &doc);
// Text > Attach to Path: one text block + one path/rect/ellipse/line selected; the text takes the
// path's geometry as its baseline guide and the path object is removed.
bool attachTextToPath(Document &doc, QString *err = nullptr);
int detachTextFromPath(Document &doc); // recreates the guide as a 1 px black path           // rect/ellipse/line/text -> paths
int simplify(Document &doc, double amount);  // amount: tolerance in px
int expandStroke(Document &doc, double width, Qt::PenJoinStyle join, Qt::PenCapStyle cap, double miter = 4);
int insetPath(Document &doc, double width, bool inside);
// Knife: cuts the selected paths along the polyline; the pieces become separate open paths.
// Returns the number of objects cut.
int knife(Document &doc, const QPolygonF &cut);

// Freeform / Reshape Area helpers
void densify(VObject &o, double spacing);    // straight nodes no more than `spacing` apart
void refine(VObject &o, double maxLen);      // splits straight segments longer than maxLen
bool push(VObject &o, const QPointF &c, double radius);
bool pull(VObject &o, const QPointF &c, const QPointF &delta, double radius, double strength);

QPolygonF simplifyPolyline(const QPolygonF &pts, double tol, bool closed);

// Path Scrubber (v0.3.1): per-anchor pressure data (VObject::ptSize / ptOpacity)
struct PressureSample { QPointF p; double size = 1, opacity = 1; };
// the outline sampled about every `step` px, per contour (closed contours end on their start),
// with the data interpolated linearly along each segment
QVector<QVector<PressureSample>> pressureSamples(const VObject &o, double step);
// splits segments longer than maxLen (curves exactly, the data interpolated) so pressure can vary
// along them; stops at maxPoints. Returns true if any point was added.
bool subdivideForPressure(VObject &o, double maxLen, int maxPoints = 4000);
// adds `delta` (negative = decrease) times a smooth falloff to the size and / or opacity of every
// anchor within `radius` of c, creating the data at 1.0 first. Returns true if a value changed.
bool scrub(VObject &o, const QPointF &c, double radius, double delta, bool size, bool opacity);
// the tapering stroke as filled outlines (SVG export): one piece per run of equal opacity
// (quantised to 1/32), halfWidth = half the full stroke width at size 1.0
struct TaperPiece { QPainterPath path; double opacity = 1; };
QVector<TaperPiece> taperOutline(const VObject &o, double halfWidth);
}
