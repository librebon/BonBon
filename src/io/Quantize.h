#pragma once
#include <QImage>
#include <QVector>
#include <QStringList>

namespace quant {
enum PaletteType { Adaptive = 0, Web216 = 1, Grayscale = 2, BlackWhite = 3, WebSnap = 4, Exact = 5, Custom = 6 };
QStringList paletteNames();                 // UI order
QVector<int> paletteOrder();                // PaletteType per UI row
// Load a palette file (.act Adobe Colour Table, .gpl GIMP, .pal JASC / RIFF, or a .png/.gif's colours).
QVector<QRgb> loadPalette(const QString &path, QString *err = nullptr);
bool savePalette(const QString &path, const QVector<QRgb> &pal, QString *err = nullptr); // .act, .gpl or .pal (JASC)
// Build a palette of at most maxColors opaque colours from the given images.
QVector<QRgb> buildPalette(const QVector<QImage> &imgs, int maxColors, int type, const QVector<QRgb> &custom = {});
// Map an ARGB32 image onto the palette. Pixels with alpha < 128 get `transparentIndex` (if >= 0).
// Palette indices are offset by `indexOffset` (used to reserve slot 0 for transparency).
// ditherAmount: 0 (none) .. 100 (full Floyd-Steinberg). loss: 0..100, reuses the previous pixel's
// colour when it is close enough, so LZW finds longer runs (smaller lossy GIFs).
QVector<uchar> mapImage(const QImage &img, const QVector<QRgb> &pal, int ditherAmount, int loss, int transparentIndex, int indexOffset);
}
