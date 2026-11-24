#pragma once
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

// Pixel-art scalers (v0.3.1): Nearest Neighbor, EPX / Scale2x-4x (AdvanceMAME), Eagle, 2xSaI,
// Super 2xSaI, Super Eagle, hq2x-hq4x, xBR 2x-4x, xBRZ 2x-6x and MMPX.
//
// Every scaler works on straight (non-premultiplied) ARGB and treats pixels outside the image as
// copies of the nearest edge pixel. A factor the algorithm has no native pass for is reached by
// chaining native passes (EPX 4x = 2x then 2x, MMPX 8x = 2x three times); a size no chain hits
// exactly (3x for a 2x-only scaler, 2.5x, or a non-uniform size) is reached by running the
// smallest chain at least as large and resampling the result down to the requested size.
//
// Sources: hqx and xBR are libxbr-standalone (third_party/libxbr, LGPL-2.1+, from FFmpeg / hqx);
// xBRZ is Zenju's xBRZ 1.8 (third_party/xbrz, GPL-3); 2xSaI / Super 2xSaI / Super Eagle follow
// Derek Liauw Kie Fa's algorithms as published in ScummVM (GPL); Scale2x/3x/4x follow Andrea
// Mazzoleni's AdvanceMAME specification; MMPX follows McGuire & Gagiu's reference (MIT).
namespace pixelscale {

enum class Method { Nearest = 0, ScaleNx, Eagle, SaI2x, Super2xSaI, SuperEagle, Hqx, Xbr, Xbrz, Mmpx };
constexpr int kMethodCount = 10;

QString name(Method m);            // "EPX / Scale2x-4x", "hqx (hq2x-hq4x)", ...
QString key(Method m);             // stable id: "nearest", "scalenx", "eagle", "2xsai", "super2xsai", "supereagle", "hqx", "xbr", "xbrz", "mmpx"
QVector<int> nativeFactors(Method m);   // single-pass factors, e.g. {2, 3, 4} for hqx, {2..6} for xBRZ
// The passes used for an integer factor: an exact chain of native factors with the fewest passes
// when one exists, else the chain with the smallest product above `factor` (then resampled down).
QVector<int> passesFor(Method m, int factor);

// One native pass (factor must be in nativeFactors(m)); out is factor x the size, Format_ARGB32.
QImage pass(const QImage &src, Method m, int factor);
// Integer upscale by any factor >= 1 (chained passes; resampled down when the chain overshoots).
QImage upscale(const QImage &src, Method m, int factor);
// Scale to any size: the next integer factor up that covers the target in both directions is
// produced with the scaler, then resampled down (smooth area resampling) to exactly `size`.
// Shrinking (target no larger than the source in both directions) uses nearest neighbour.
QImage scaleTo(const QImage &src, const QSize &size, Method m);

// The Preferences / Image Size / Export / Batch "Resample" list: indices 0-3 are coreprefs'
// Bicubic, Bilinear, Soft and Nearest Neighbor; 4 + n are the pixel-art scalers after Nearest
// (n = 0 EPX / Scale2x-4x ... 8 MMPX). Saved settings and documents keep these numbers.
constexpr int kFirstPixelArt = 4;
constexpr int kResampleCount = kFirstPixelArt + kMethodCount - 1;   // == coreprefs::LastResample + 1
QStringList resampleNames();                    // display names, index = resample number
QString resampleKey(int resample);              // "bicubic", "bilinear", "soft", "nearest", then key(m)
int resampleFromKey(const QString &key, int fallback);
bool isPixelArt(int resample);                  // 4 and above
Method methodFor(int resample);                 // Nearest for 0-3
int resampleFor(Method m);                      // 3 for Nearest
// Smooth resampling (v0.3.4): separable Catmull-Rom (Bicubic), triangle (Bilinear) or cubic B-spline
// (Soft) in premultiplied space, centre-aligned, edge pixels clamped (never sampling transparent or
// white outside the image), kernel widened when shrinking. Opaque in = opaque out, and a solid image
// stays exactly solid. Returns ARGB32_Premultiplied unless the source was ARGB32 / RGB32 (kept).
enum class Filter { Bicubic, Bilinear, Soft };
QImage filterResample(const QImage &src, const QSize &size, Filter f);
// Resample `src` to `size` with a Resample list entry (-1 = the Preferences default).
QImage resample(const QImage &src, const QSize &size, int resample = -1);

} // namespace pixelscale
