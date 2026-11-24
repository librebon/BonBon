#pragma once
#include "Object.h"
#include <QByteArray>

struct DocState;

namespace render {
void drawBase(QPainter &p, const VObject &o);
// Render the object (with effects) into an image; `area` receives its placement in canvas coords.
// Results are cached by object content (see lwd::objectContentKey) and clip; pixel-identical to the
// uncached path. Disable for golden tests that want a forced recompute.
QImage renderObject(const VObject &o, QRect *area, const QRect &clip);
void drawObject(QPainter &p, const VObject &o, const QRect &clip);
QImage renderFrame(const DocState &st, int frame, bool withBackground = true);
QImage renderObjects(const QVector<VObject> &objs, const QRect &area);
void setObjectVisualCacheEnabled(bool on);
void clearObjectVisualCache();
bool objectVisualCacheEnabled();
int objectVisualCacheEntries();
// Test hook: digest of a symbol body, including nested instance bodies.
QByteArray symbolBodyDigest(quint64 symbolId);
// Symbol-body blit cache (0.3.20): one raster per (body digest, frame, scale/interp).
void setSymbolBodyBlitCacheEnabled(bool on);
void clearSymbolBodyBlitCache();
bool symbolBodyBlitCacheEnabled();
int symbolBodyBlitCacheEntries();
}
