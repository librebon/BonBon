#pragma once
// v0.3.0 image algorithms: content-aware fill (PatchMatch), liquify, dithering and palettes, pixel-art
// outline, normal / AO maps, lens correction, smart sharpen, surface blur, bilateral denoise, gradient map.
// Unless stated otherwise every function takes any QImage and returns Format_ARGB32_Premultiplied, and is
// deterministic (fixed seeds, no threads), so the tests can check exact output.
#include "Object.h"
#include <QImage>
#include <QVector>
#include <functional>

namespace fx2 {

// --- colour ---
QImage gradientMap(const QImage &src, QVector<GradStop> stops, bool reverse, int opacity);
QColor gradientAt(const QVector<GradStop> &sortedStops, double t);
// Stretches the luminance range (0.5% clipped at each end) with one transform for all channels, so hues stay.
QImage autoContrast(const QImage &src);

// --- palettes and dithering ---
enum DitherMethod { DitherNone, FloydSteinberg, Atkinson, Jarvis, Stucki, Bayer2, Bayer4, Bayer8, kDitherMethods };
enum PaletteTarget { PalCustom, PalWeb, PalMono, PalRGB565, PalRGB332, PalAdaptive, PalGray, PalRGB555, PalRGB444, kPaletteTargets };
QStringList ditherMethodNames();
QStringList paletteTargetNames();
// The colours a palette target can produce (per-channel targets such as RGB565 have none: empty).
QVector<QRgb> targetPalette(int target, int nColors, const QImage &img, const QVector<QRgb> &custom);
// Quantizes to the target with the chosen method. strength 0..100 scales the diffused error / the ordered
// threshold. Alpha is kept (pixels with alpha 0 untouched). Serpentine scanning for error diffusion.
QImage dither(const QImage &src, int method, int target, int nColors, const QVector<QRgb> &custom,
              double strength = 100, bool serpentine = true);
// Per-channel bit depths for a C-header pixel format: dither to rBits/gBits/bBits (grey: all equal, mono=1 grey).
QImage ditherToBits(const QImage &src, int method, int rBits, int gBits, int bBits, bool grey, int aBits = 8);

QImage paletteSwap(const QImage &src, const QVector<QRgb> &from, const QVector<QRgb> &to, bool nearest);
// Unique opaque colours in first-seen order (scanning rows), at most `max` (0 = all).
QVector<QRgb> uniqueColors(const QImage &img, int max = 0);
// Replace every pixel whose colour (ignoring alpha) is exactly `from` with `to` (alpha kept).
// The exact case of replaceColorRange (tolerance 0, Solid, full strength).
QImage replaceColor(const QImage &src, QRgb from, QRgb to);

// --- Replace Color (v0.3.1): Filters > Adjust Color > Replace Color, the Replace Color brush and the Batch step.
// Pixels near `from` (RGB distance, 0..100 % of the largest possible) are replaced:
//   Solid  - by `to` (alpha kept);
//   Color  - by the hue and saturation of `to`, keeping the pixel's luminance (shading survives;
//            Photoshop Color Replacement / "Color" blend);
//   Shift  - by the pixel shifted in hue (-180..180), saturation and lightness (-100..100).
// tolerance 0 = exact matches only. softness (0..100 % of the tolerance) fades the match out towards the
// tolerance edge; 0 = hard. strength 0..1. hard: every pixel is either replaced or kept (no partial blend).
enum ReplaceRange { RcSolid = 0, RcColor = 1, RcShift = 2 };
struct ReplaceColorOpts {
    QRgb from = 0xff000000, to = 0xffffffff;
    double tolerance = 10, softness = 50;
    int range = RcSolid;
    double hue = 0, saturation = 0, lightness = 0;
    double strength = 1;
    bool hard = false;
};
double colorDistance(QRgb a, QRgb b);                           // 0..100 (RGB, alpha ignored)
double colorMatch(QRgb straightPixel, const ReplaceColorOpts &o); // 0..1 (0 for transparent pixels)
QRgb replacedColor(QRgb straightPixel, const ReplaceColorOpts &o); // the full replacement, alpha kept
// The whole image; `coverage` (optional Alpha8 of the same size) scales the strength per pixel.
QImage replaceColorRange(const QImage &src, const ReplaceColorOpts &o, const QImage &coverage = QImage());
// The matched range as a grey mask (white = replaced), for previews (Photoshop's "Selection" view).
QImage replaceColorMask(const QImage &src, const ReplaceColorOpts &o);
// A brush stroke: every dab is computed from the pixels the stroke started on (so overlapping dabs do not
// compound and the strength is a ceiling, as in Fireworks), through the maximum tip coverage so far.
class ReplaceColorStroke {
public:
    // `img` is the bitmap (ARGB32_Premultiplied); `clip` (optional Alpha8, image size) is the selection.
    void begin(const QImage &img, const ReplaceColorOpts &o, const QImage &clip = QImage());
    // A dab of diameter `size` px at `c` (image px). edge 0 (hard) .. 100 (soft); square tip;
    // pixelPerfect: aliased tip and all-or-nothing pixels. Returns the changed rect.
    QRect dab(QImage &img, QPointF c, double size, int edge, bool square, bool pixelPerfect);
    bool active() const { return !m_orig.isNull(); }
    const ReplaceColorOpts &options() const { return m_o; }
    void end() { m_orig = QImage(); m_cov = QImage(); m_clip = QImage(); }
private:
    ReplaceColorOpts m_o;
    QImage m_orig, m_cov, m_clip;
};

// --- Red Eye Removal (v0.3.1): the Red Eye tool (Rubber Stamp / Replace Color flyout).
// A pixel is "red-eye" by its redness (r - max(g, b)) / r, which ignores how bright it is; tolerance
// (0..100) lowers the redness needed (50 = about a third of r). Matched pixels are desaturated to
// the green / blue level (the pupil's own darkness) and darkened by `darken` (0..100). Bright,
// nearly neutral pixels (the catchlight / specular highlight) never match, so they stay.
struct RedEyeOpts {
    double tolerance = 50, darken = 50;
    double strength = 1;          // 0..1
};
double redEyeMatch(QRgb straightPixel, const RedEyeOpts &o);   // 0..1
QRgb redEyeFixed(QRgb straightPixel, const RedEyeOpts &o);     // the full correction, alpha kept
// A brush stroke over the pixels it started on, through the maximum (tip coverage x pressure) so far,
// like ReplaceColorStroke: going over the same place again never compounds.
class RedEyeStroke {
public:
    void begin(const QImage &img, const RedEyeOpts &o, const QImage &clip = QImage());
    // diameter `size` image px at `c`; edge 0..100 (soft edge); amount 0..1 scales the coverage (pen pressure).
    QRect dab(QImage &img, QPointF c, double size, int edge, bool square, double amount = 1.0);
    bool active() const { return !m_orig.isNull(); }
    void end() { m_orig = QImage(); m_cov = QImage(); m_clip = QImage(); }
private:
    RedEyeOpts m_o;
    QImage m_orig, m_cov, m_clip;
};

// --- pixel art ---
// Outline around (outer) or inside (inner) the shape where alpha >= alphaThreshold.
QImage outline(const QImage &src, bool inner, bool eightConnected, int width, const QColor &color, int alphaThreshold, int opacity = 100);
// Removes "L" corners from a 1-px freehand pixel path (Aseprite-style pixel-perfect).
QVector<QPoint> pixelPerfect(const QVector<QPoint> &pts);
// Bresenham line (both ends included).
QVector<QPoint> linePixels(QPoint a, QPoint b);

// --- game textures ---
// Height from luminance (source 0) or alpha (1). Sobel (op 0) or Scharr (1). OpenGL convention
// (green = +Y up) unless invertY (DirectX). wrap samples across the edges (tileable textures).
QImage normalMap(const QImage &src, int op, double strength, bool invertY, bool wrap, int source = 0);
QImage ambientOcclusion(const QImage &src, double radius, double strength, bool wrap, int source = 0);
// mode 0: offset-and-blend (fx::makeSeamless); mode 1: mirror-blend the edges across the seam.
QImage makeSeamless(const QImage &src, int mode, double width);

// --- photo ---
// p[0] distortion (-100..100, + removes barrel), p[1] red/cyan fringe, p[2] blue/yellow fringe (-100..100),
// p[3] vignette amount (-100 darken .. 100 lighten), p[4] vignette midpoint (0..100), p[5] vertical and
// p[6] horizontal perspective (-100..100), p[7] straighten angle (degrees).
QImage lensCorrection(const QImage &src, const double p[8]);
// blurKind 0 Gaussian, 1 lens (disk), 2 motion (angle). noise 0..100 suppresses small-detail sharpening.
QImage smartSharpen(const QImage &src, double amount, double radius, int blurKind, double angle, double noise);
QImage surfaceBlur(const QImage &src, int radius, int threshold);
QImage bilateralDenoise(const QImage &src, double sigmaSpatial, double sigmaRange, int passes);

// --- content-aware fill ---
// Fills the pixels where mask > 127 (mask: any format, its grey / alpha value) from the rest of the image,
// with multi-scale PatchMatch (7x7 patches, EM voting). Deterministic for a given seed.
// progress(0..100) may return false to cancel (the input is returned).
QImage contentAwareFill(const QImage &src, const QImage &mask, quint32 seed = 1,
                        const std::function<bool(int)> &progress = {});

// --- liquify ---
class Liquify {
public:
    enum Mode { ForwardWarp, Pinch, Bloat, TwirlCW, TwirlCCW, Reconstruct, kModes };
    static QStringList modeNames();
    explicit Liquify(const QImage &src = QImage());
    void setSource(const QImage &src);
    // One brush dab / segment: `from` -> `to` (forward warp uses the movement; the others apply at `to`).
    // pressure 0..1 (rate), radius in pixels.
    void stroke(Mode m, QPointF from, QPointF to, double radius, double pressure);
    void reset();
    bool isIdentity() const;
    QImage render() const;
    const QImage &source() const { return m_src; }
    QPointF displacement(int x, int y) const { return (x < 0 || y < 0 || x >= m_w || y >= m_h) ? QPointF() : m_d[size_t(y) * m_w + x]; }
private:
    QPointF sampleD(double x, double y) const;
    QImage m_src;
    int m_w = 0, m_h = 0;
    QVector<QPointF> m_d;   // backward map: output(x) = src(x + d(x))
};

// Applies one of the v0.3.0 effect types (used by fx::adjust).
bool apply(QImage &img, const LiveEffect &e);
// The Replace Color options a ReplaceColor live effect / filter stands for.
ReplaceColorOpts replaceOptsFrom(const LiveEffect &e);
}
