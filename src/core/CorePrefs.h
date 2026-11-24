#pragma once
#include <Qt>
#include <QRgb>

// Process-wide preferences that affect the document core. The UI's
// Preferences dialog writes these; they are not stored in documents.
namespace coreprefs {
// 4 and above are the pixel-art scalers in pixelscale's Resample list (EPX / Scale2x ... MMPX)
enum Interpolation { Bicubic = 0, Bilinear = 1, Soft = 2, Nearest = 3, FirstPixelArt = 4, LastResample = 12 };
inline int undoSteps = 200;            // history entries kept per document
inline int undoMemoryMB = 1024;        // bitmap memory the history of one document may hold (oldest steps go first)
inline int interpolation = Bicubic;    // bitmap resampling when scaling

// defaults for the grid of new documents (Preferences > Editing); each document keeps its own
inline int gridSize = 20;
inline QRgb gridColor = qRgba(0, 120, 255, 60);

// pixel-art scalers keep hard pixels wherever a scaler cannot be used (rotation, skew, previews)
inline bool smoothScaling() { return interpolation < Nearest; }
inline bool pixelArtScaling() { return interpolation >= FirstPixelArt && interpolation <= LastResample; }
}
