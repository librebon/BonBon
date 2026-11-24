#pragma once
#include <QImage>
#include <QPainter>
#include <QPointF>

// Bitmap retouching tools (Fireworks Blur / Sharpen / Dodge / Burn / Smudge / Rubber Stamp).
// All work on ARGB32_Premultiplied images in image pixel coordinates with a soft round tip.
// `strength` is 0..100 (intensity / exposure / pressure / opacity). `clip` (optional, Alpha8,
// same size as the image) limits the effect to a pixel selection.
namespace retouch {
enum class Range { Shadows, Midtones, Highlights };

// The tip the Blur / Sharpen / Dodge / Burn / Smudge / Stamp dabs use: Edge 0 (hard) .. 100 (soft)
// and Shape (round or square). Set for the duration of a stroke with TipScope; the default is
// a soft round tip.
struct Tip { int edge = 58; bool square = false; };
class TipScope {
public:
    explicit TipScope(Tip t);
    ~TipScope();
private:
    Tip m_prev;
};
double tipWeightAt(double dx, double dy, double radius, const Tip &t);
void blurDab(QImage &img, QPointF c, double size, int strength, const QImage *clip = nullptr);
void sharpenDab(QImage &img, QPointF c, double size, int strength, const QImage *clip = nullptr);
void dodgeBurnDab(QImage &img, QPointF c, double size, int exposure, Range range, bool burn, const QImage *clip = nullptr);
// Rubber Stamp: copies pixels from `src` (a snapshot taken when the stroke started) at c + offset.
void stampDab(QImage &img, const QImage &src, QPointF c, QPointF offset, double size, int opacity, const QImage *clip = nullptr);
// the same through a blend mode (Fireworks' Rubber Stamp Blend): the source dab is composited
void stampDabMode(QImage &img, const QImage &src, QPointF c, QPointF offset, double size, int opacity,
                  QPainter::CompositionMode mode, const QImage *clip = nullptr);

// Healing Brush: copies texture from `src` at c + offset (like the stamp) but keeps the
// destination's colour and lighting. M is a smooth membrane interpolated from the pixels
// around the tip (normalised blur weighted by 1 - tip). The source's luminance high-pass
// (luma(src) - luma(M(src))) is applied to M(dst), so dark texture stays a darker shade of
// the destination colour instead of a per-channel RGB Poisson shifting the hue. Alpha still
// uses the Poisson membrane. `edge` 0 (hard) .. 100 (soft) feathers the tip.
// Modes: 0 Normal, 1 Replace (no tip feathering), 2 Lighten, 3 Darken.
enum class HealMode { Normal = 0, Replace = 1, Lighten = 2, Darken = 3 };
void healDab(QImage &img, const QImage &src, QPointF c, QPointF offset, double size, int edge, HealMode mode,
             const QImage *clip = nullptr, QPoint srcOrigin = QPoint());   // src pixel (0,0) sits at srcOrigin
// Spot Healing: no source -- picks the best-matching patch around the spot (8 directions, one tip
// diameter away) and heals with it, so the spot takes on the surrounding colour and texture.
QPointF spotHealOffset(const QImage &img, QPointF c, double size);
void spotHealDab(QImage &img, QPointF c, double size, int edge, HealMode mode, const QImage *clip = nullptr);
double edgeWeight(double dist, double radius, int edge);   // tip falloff for an Edge (softness) 0..100

// Smudge drags colour along a stroke: `carry` holds the colour picked up so far.
class Smudger {
public:
    void start(const QImage &img, QPointF c, double size);
    // start carrying a solid colour (Fireworks' Smudge Color) instead of the pixels under the tip
    void startWithColor(const QColor &c, double size);
    void dab(QImage &img, QPointF c, int pressure, const QImage *clip = nullptr);
private:
    QImage m_carry;
    double m_size = 1;
};

// Positions of the dabs from a (exclusive) to b (inclusive) spaced by about `spacing`.
QVector<QPointF> dabsAlong(QPointF a, QPointF b, double spacing);
}
