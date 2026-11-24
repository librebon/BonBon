#pragma once
#include "Object.h"
#include <QImage>
#include <QPointF>
#include <QString>
#include <QVector>

// Pixel editing on bitmaps that carry live effects which move pixels (v0.3.4).
//
// The Offset live effect shows a bitmap's pixels shifted (wrapped, edge-repeated or with a
// transparent gap). Brushes, selections and filters must act on what the user sees, so a pixel
// edit first switches the bitmap into "display geometry": its image becomes exactly what the
// canvas shows (the Offset effects applied) and those effects' offsets are set to 0, so the
// on-screen result does not change. Every tool then maps canvas -> image with the plain rect
// mapping (docToImage). When the edit is committed the image is shifted back losslessly: pixels
// the effects hide (Transparent / Repeat Edge) are taken from the original, everything visible
// from the edited image, and the offsets are restored.
//
// Lens Correction and Make Seamless also move pixels but cannot be inverted; pixel edits on a
// bitmap with either enabled are refused with a hint (blockingEffect()).
namespace pxedit {

// canvas (document) coordinates -> image pixels of a bitmap (rect mapping)
QTransform docToImage(const VObject &o);
QPointF docToImage(const VObject &o, const QPointF &doc);
// image pixels -> canvas (inverse of docToImage; shared by Magic Wand / Select Similar)
QTransform imageToDoc(const VObject &o);

// Name of an enabled live effect that moves pixels in a way pixel edits cannot follow, or empty.
QString blockingEffect(const VObject &o);
// Status-bar hint for a refused pixel edit.
QString blockedMessage(const VObject &o, const QString &what);

// The image shift in image pixels an Offset effect applies to `o` (its doc-pixel offset scaled to the image).
QPoint imageShift(const VObject &o, const LiveEffect &e);

struct Session {
    quint64 id = 0;
    QImage orig;                 // the pixels before the edit (original geometry)
    QVector<int> effectIndex;    // the Offset effects that were zeroed
    QVector<QPointF> offsets;    // their offsets
    QVector<QPoint> shifts;      // their image shifts
    QVector<int> modes;          // their undefined-area modes
    QSize displaySize;
    QRectF rect;
};

// Switch `o` into display geometry. False when there is nothing to do (no Offset effect).
bool enter(VObject &o, Session &s);
// Back to the original geometry, losslessly; the offsets are restored. When the edit changed the
// image size or position (a stroke that grew the bitmap, Move pixels), the offset stays baked into
// the pixels (the result looks the same) and `*baked` is set.
void leave(VObject &o, const Session &s, bool *baked = nullptr);

// Trim a bitmap to the tight bounds of its non-transparent pixels, keeping every pixel where it is
// on the canvas. An all-transparent bitmap becomes a single transparent pixel at its old top-left.
// Returns true when the bitmap changed. (Bitmaps with an enabled Offset effect must be trimmed in
// display geometry -- see Document::endPixelEdits.)
bool trimToAlpha(VObject &o);
// Fully transparent (or empty) bitmap.
bool isTransparent(const VObject &o);

} // namespace pxedit
