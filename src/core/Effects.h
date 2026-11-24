#pragma once
#include "Object.h"

namespace fx {
// Blur `ch` interleaved 8-bit channels in place, approximating a gaussian with 3 box passes.
void blurBuffer(uchar *data, int w, int h, int bpl, int ch, double radius);
// Gaussian-ish blur of an ARGB32_Premultiplied or Alpha8 image.
void blur(QImage &img, double radius);
QImage alphaMask(const QImage &argb);                 // Alpha8
QImage colorize(const QImage &mask, const QColor &c, double opacity); // ARGB32_Premultiplied
void boostMask(QImage &mask, double factor);
void invertMask(QImage &mask);
void maskBy(QImage &argb, const QImage &mask);        // multiply argb by mask alpha

// Apply one pixel-adjustment effect (effectIsAdjustment) destructively.
void adjust(QImage &img, const LiveEffect &e);
QImage highPass(const QImage &src, double radius, bool keepColor);
QImage offset(const QImage &src, int dx, int dy, int undefinedAreas);  // LiveEffect::Undefined
// For every pixel of a w x h grid, the offset to the nearest pixel with site[] set (2-pass sweep).
void nearestSites(const QVector<char> &site, int w, int h, QVector<QPoint> &vec);
QImage featherEdges(const QImage &src, double radius, bool centered, double softness);
QImage makeSeamless(const QImage &src, double blendWidth);
QVector<uchar> curveLut(const QPolygonF &pts);  // < 2 points = identity
QVector<uchar> levelsLut(const LevelsChannel &c);
QVector<int> histogram(const QImage &img, int channel); // ChRGB = luminance; transparent pixels ignored
LevelsChannel autoLevelsChannel(const QImage &img, int channel); // clips 0.5% at each end

// Apply all live effects of `o` to its rendered base image (object only, transparent elsewhere).
// `objRect` is the object's own bounds in the buffer's pixels (the buffer is padded around it);
// Offset wraps within that box (united with whatever earlier effects drew), never the padding.
QImage applyEffects(const QImage &base, const QVector<LiveEffect> &effects, const QRect &objRect = QRect());
// Bounding box of the pixels with alpha > 0.
QRect alphaBounds(const QImage &img);
}
