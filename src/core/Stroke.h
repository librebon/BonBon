#pragma once
#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QImage>

// Shared Fireworks-style stroke engine: one Params + preset list for vector
// strokes and the bitmap Brush tool. See /workspace/brushes-0.3.20/fireworks-brushes.md.
namespace stroke {

struct Sensitivity {
    int pressure = 0;    // 0..100
    int speed = 0;
    int horizontal = 0;
    int vertical = 0;
    int random = 0;
    bool operator==(const Sensitivity &o) const {
        return pressure == o.pressure && speed == o.speed && horizontal == o.horizontal
            && vertical == o.vertical && random == o.random;
    }
};

enum class ColorVariation { None = 0, Random, Uniform, Complementary, Hue, Shadow };
enum class Look {
    Pen, PenRound, Hard1, Soft, Air, Spray, Flat, Grain, Dash, DoubleDash,
    Bristle, Dots, Squares, Confetti, Squiggle, Fur, Marker, Highlighter, Water,
    Outline, ThreeD, ThreeDGlow, Chameleon, FluidSplatter, PaintSplatter,
    Toothpaste, ToxicWaste, AlienPaint, WetCalligraphy, WaterRandom,
    // Libreworks category (not Fireworks)
    NeonTube, Gouache, InkWash, PixelPencil, RibbonTwist, Chain, Rope, Stitch,
    Glitter, Smoke, Fire, Rainbow, Lightning, SprayCan, ImageStamp, Cellular
};

struct Params {
    QString key, category, name;
    // Options tab
    int inkAmount = 100;       // 0..100
    int spacing = 25;          // percent of tip size
    int flowRate = 0;          // 0..100 (airbrush)
    bool buildUp = false;
    QString texture;
    int textureAmount = 0;     // 0..100
    int edgeTexture = 0;
    QString edgeEffect;
    int tips = 1;
    int tipSpacing = 0;
    ColorVariation colorVar = ColorVariation::None;
    int dashOn[3] = {0, 0, 0};
    int dashOff[3] = {0, 0, 0};
    // Shape tab
    bool square = false;
    double size = 4;           // factory tip size (object strokeWidth usually overrides)
    int edge = 0;              // 0..100 softness
    double aspect = 1;         // 1 = round; < 1 = flat nib (width/length)
    double angle = 0;          // degrees, 0 = horizontal; Calligraphy factory uses -45
    // Sensitivity tab (verified properties + Angle for the nib)
    Sensitivity sizeSens, inkSens, satSens, hueSens, angleSens;
    // Renderer look (derived from the Fireworks preset; not a Fireworks control)
    Look look = Look::Pen;
    double lookA = 0;          // per-look numeric (spacing / nib / alpha / dash)
    double lookB = 0;
    QString stampSource;       // Image Stamp: file path or "symbol:<id>"
    // Cellular automaton (Libreworks)
    QString cellRule = QStringLiteral("B3/S23");  // B/S, "BB" (Brian's Brain), or "W30"/"W90"/"W110"
    int cellSize = 3;          // 1..32 px
    int cellSteps = 2;         // generations per dab
    int seedDensity = 40;      // 0..100
    QColor cellAgeColor;       // invalid = auto (lighter / complement of the stroke colour)
    // Bitmap Brush only; default off so existing strokes and old files are unchanged.
    bool cellSampleUnder = false;  // cell colour from the pre-stroke target, blended with the brush
    bool cellFollowEdges = false;  // seeds and boundaries follow edges already in the bitmap
    // Spray Can (0.3.29): 0 = legacy stamped-dot look (documents without sprayLook),
    // 1 = airbrush / spray-paint. Factory new strokes are 1. Optional lwd v7 keys.
    int sprayLook = 0;
    int sprayDensity = 50;         // 0..100
    int spraySpread = 50;          // 0..100
    bool sprayDrips = false;       // default off; legacy look keeps its own slow drips
    bool operator==(const Params &o) const;
};

struct Preset { QString key, category, name; };

const QVector<Preset> &presets();
QStringList categories();                              // Fireworks CS5 order, then Libreworks
QStringList namesInCategory(const QString &category);
QString keyFor(const QString &category, const QString &name);
QString canonicalKey(const QString &keyOrAlias);       // old keys, category-only, aliases
QString label(const QString &key);
Params paramsFor(const QString &key);
const Params *factory(const QString &key);
// True for looks the bitmap Brush must paint as a path (calligraphy, 3D, Cellular, …).
// False for simple circular/soft/air tips, which use the dab engine (pressure, click-to-stamp).
bool usesPathEngine(Look look);
// Looks whose output depends on the whole path (live tail-only paint would jump on release).
bool pathGlobalLook(Look look);
bool pathGlobalLook(const Params &p);
// True when the Edge (softness) control should be enabled and applied.
bool usesEdgeSoftness(Look look);

// Edit Stroke / Brush PI: which controls actually affect a look. Disabled
// controls stay visible and use the normal disabled widget style.
enum class Option {
    InkAmount, Spacing, Flow, BuildUp, Texture, Tips, TipSpacing,
    ColorVariation, Dash, Square, Size, Edge, Aspect, Angle,
    SizePressure, SizeSpeed, InkPressure, InkSpeed,
    SaturationSens, HueSens, AngleSens,
    EdgeTexture, EdgeEffect,
    CellRule, CellSize, CellSteps, SeedDensity, CellAgeColor,
    CellSampleUnder, CellFollowEdges,
    StampSource,
    SprayDensity, SpraySpread, SprayDrips
};
bool optionApplies(Look look, Option opt);
bool optionApplies(const Params &p, Option opt);
QString optionLabel(Option opt);
const QVector<Option> &allOptions();

// Extra paint margin beyond width/2 (splatter, fur, glow, …).
double extent(const Params &p, double width);
// Maximum distance from the path that paint() can cover, in the same units as width.
// Includes scatter, jitter, spray, drips, texture fringe and edge softness (no extra margin).
double paintedRadius(const Params &p, double width);
QRectF paintedBounds(const QPainterPath &path, const Params &p, double width);

struct PaintInput {
    Params params;
    double width = 1;
    QColor color = Qt::black;
    bool closedShape = false;
    int align = 0;             // 0 centre, 1 inside, 2 outside
    bool sharpRect = false;
    quint32 seed = 1;
    QImage stampImage;         // Image Stamp; empty = built-in star
    // Pre-stroke snapshot of the target bitmap (bitmap Brush). Path coordinates match this
    // image's pixel space. Empty for vector strokes and the Edit Stroke preview.
    QImage underImage;
    // Bitmap Brush live stroke: lock Cellular cell size so the grid does not coarsen mid-stroke.
    // 0 = vector / complete path, which keeps the 0.3.21 MAXN coarsening.
    int cellSizeLock = 0;
    // When set, a locked Cellular grid is clipped to this rect plus a CA halo, so live
    // cost stays O(the dirty area) instead of the whole path bounds.
    QRect gridLimit;
    double pressure = 1.0;     // 0..1 (tablet or 1 for mouse / vector)
    double speed = 1.0;        // 1 = normal; lower = slower (Spray Can drips, Sumi-e bleed)
    // Optional per-distance samples (bitmap Brush live preview). Empty = use pressure/speed.
    QVector<double> sampleDist, samplePressure, sampleSpeed;
    // Optional elapsed milliseconds from the start of the stroke (Spray Can dwell).
    // Empty = synthesize timing from distance and speed (no dwell).
    QVector<double> sampleTime;
    // Absolute path distance at the start of `path` (0 for a full stroke). Used so
    // a suffix paint keeps dash phase and other distance-based looks aligned.
    double pathOrigin = 0;
    double pressureAt(double dist) const;
    double speedAt(double dist) const;
    double timeAt(double dist) const;
};

// Left-to-right pressure / speed ramp for the Edit Stroke preview (and tests).
// Does not change stock document paints, which leave the sample vectors empty
// (pressure=1, speed=1: identity for every sensitivity slider).
void fillPreviewSenseRamp(PaintInput &in, const QPainterPath &path);

void paint(QPainter &p, const QPainterPath &path, const PaintInput &in);
QImage loadStampImage(const QString &source);  // file path or "symbol:<id>"; null if missing
// Drop the process-wide under-sample planes (call from brush::Stroke::end()).
void releaseUnderSampleCache();

// Standard S-curve used by the Edit Stroke preview, in `inner` coordinates.
QPainterPath previewCurve(const QRectF &inner);
struct PreviewLayout {
    QSize work;
    int margin = 0;
    double scale = 1;          // width and path are multiplied by this so huge tips fit
    QPainterPath path;
};
// Working buffer, scale and S-curve for a preview of `dest`.
PreviewLayout previewLayout(const Params &p, double width, const QSize &dest);
// Working-buffer size for a preview of `dest`, padded by paintedRadius (capped).
QSize previewWorkSize(const Params &p, double width, const QSize &dest);
// Scale a transparent painted buffer into `dest` on `background`, uniformly fitted.
QImage fitPreview(const QImage &painted, const QSize &dest, const QColor &background, int pad = 8);
// Paint the preview curve into an unscaled transparent working image (same policy as renderPreview).
QImage renderPreviewRaw(const PaintInput &in, const QSize &dest);
// Vector-style preview: same engine as object strokes (stroke::paint), scaled to fit `dest`.
QImage renderPreview(const PaintInput &in, const QSize &dest, const QColor &background = QColor(250, 250, 248));
// Default seed used when the dialog is not bound to a specific object.
constexpr quint32 kPreviewSeed = 0xC0FFEEu;

QJsonObject toJson(const Params &p);                   // only fields that differ from the factory, plus key
Params fromJson(const QJsonObject &j, const Params &base = {});

// Identity of factory params (names excluded) — used by uniqueness tests.
QString paramFingerprint(const Params &p);

} // namespace stroke
