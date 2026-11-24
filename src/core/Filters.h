#pragma once
#include <QImage>
#include <QPainterPath>

namespace filters {
enum class Kind { Invert, Desaturate, Blur, BlurMore, GaussianBlur, Sharpen, SharpenMore, BrightnessContrast };
// Applies destructively; `param1`/`param2` are kind specific (radius, brightness, contrast).
QImage apply(const QImage &src, Kind k, double param1 = 0, double param2 = 0);
// Flood fill style region selection; returns a mask (Format_Alpha8, 255 = selected).
QImage floodMask(const QImage &img, QPoint seed, int tolerance, bool contiguous = true);
QPainterPath maskToPath(const QImage &mask, const QTransform &toCanvas);
QRect opaqueBounds(const QImage &img);
}
