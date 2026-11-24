#pragma once
#include "Object.h"
#include <QBrush>
#include <QPainter>

// Fill categories (solid, web dither, patterns, the Fireworks gradient types), textures,
// feathered fill edges and the stroke categories (brush presets).
namespace fills {

QStringList fillKindLabels();                 // in FillKind order
QString fillKindKey(FillKind k);
FillKind fillKindFromKey(const QString &key);
bool isGradient(FillKind k);
// false for the kinds whose look depends only on the main vector (Linear, Bars): no second handle
bool usesSecondAxis(FillKind k);

QVector<GradStop> effectiveGradient(const VObject &o); // o.gradient, or fill colour -> white
QStringList gradientPresetNames();
QVector<GradStop> gradientPreset(const QString &name);
QColor sampleGradient(const QVector<GradStop> &stops, double t);
// Switch o to `k`, giving it default handles (and a gradient if it has none).
void setFillKind(VObject &o, FillKind k);
// the Contour handles that reproduce the plain (pre-0.3.4) distance-from-edge look
void resetContourHandles(VObject &o);
bool contourHandlesDefault(const VObject &o);
// Gradient handle positions in document coordinates, and back.
QPointF handleToDoc(const VObject &o, const QPointF &n);
QPointF docToHandle(const VObject &o, const QPointF &d);
// Set the gradient vector from a Gradient tool drag (doc coordinates).
void setGradientVector(VObject &o, const QPointF &from, const QPointF &to);
// Gradient value 0..1 at a document point (unclamped kinds wrap); Contour is approximated radially.

// Pattern fill handles: the brush transform (tile pixels -> document) and the three handles
// (centre, end of the X bar, end of the Y bar) in document coordinates, and back.
QTransform patternBrushTransform(const VObject &o);
QImage patternImage(const VObject &o);   // the tile o's Pattern fill repeats
void patternHandles(const VObject &o, QPointF h[3]);
bool setPatternHandles(VObject &o, const QPointF h[3]);   // false (unchanged) when the bars are degenerate
// the fill handles of a gradient or a pattern, in document coordinates, and back
void fillHandles(const VObject &o, QPointF h[3]);
void setFillHandles(VObject &o, const QPointF h[3]);

QStringList patternNames();
QImage patternTile(const QString &name); // colour tile
QStringList textureNames();
QImage textureTile(const QString &name); // grayscale (Format_Grayscale8)

// Fill `path` with o's fill (category, texture and edge). Honours the painter's transform.
void fillShape(QPainter &p, const VObject &o, const QPainterPath &path);

struct StrokePreset { QString key, category, name; };
const QVector<StrokePreset> &strokePresets();
QStringList strokeCategories();
QStringList strokeNamesInCategory(const QString &category);
QString strokeKey(const QString &category, const QString &name);
QString resolveStrokeKind(const QString &keyOrCategory); // category-only (pre-0.3.6) -> first type
double strokeExtent(const VObject &o);    // extra margin beyond width/2 (sprays, fur, etc.)
// Stroke `path` with o's stroke (category and placement).
void strokeShape(QPainter &p, const VObject &o, const QPainterPath &path, bool closedShape);
}
