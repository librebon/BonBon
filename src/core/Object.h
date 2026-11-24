#pragma once
#include <QColor>
#include <QFont>
#include <QImage>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QTransform>
#include "io/ImageIO.h"
#include "Stroke.h"

enum class ObjType { Bitmap, Rect, Ellipse, Line, Path, Text, Slice, Hotspot, Instance, AutoShape, Group };

// Fireworks symbol types (Library panel). A button symbol has four states:
// Up, Over, Down, Over While Down (stored as its frames).
enum class SymbolKind { Graphic, Animation, Button };
QString symbolKindKey(SymbolKind k);
QString symbolKindLabel(SymbolKind k);
SymbolKind symbolKindFromKey(const QString &k);
QStringList buttonStateNames();

// Web object shapes and behaviours (slices and hotspots live on the shared Web Layer).
enum class WebShape { Rect, Circle, Polygon };
enum class WebBehavior { None, SimpleRollover, SwapImage };

enum class BlendMode {
    Normal, Multiply, Screen, Overlay, Darken, Lighten, Difference,
    ColorDodge, ColorBurn, HardLight, SoftLight, Exclusion,
    // Fireworks MX 2004 modes that have no 1:1 QPainter composition mode
    // (except Erase = DestinationOut).
    Hue, Saturation, Color, Luminosity, Invert, Tint, Erase
};

QStringList blendModeNames();
QString blendModeKey(BlendMode m);
BlendMode blendModeFromKey(const QString &k);
QPainter::CompositionMode toComposition(BlendMode m);
bool blendIsCustom(BlendMode m); // Hue/Sat/Color/Luminosity/Invert/Tint: pixel-level
// Blit src onto dest at pos (device pixels). opacity is 0..1. clip is in dest pixels (empty = dest.rect()).
void blitBlend(QImage &dest, const QImage &src, const QPoint &pos, BlendMode m, qreal opacity = 1,
               const QRect &clip = QRect());
// Draw src at srcArea.topLeft() through p, using a custom blend when needed (may briefly end a
// QImage painter and restore its transform, clip, hints, opacity and composition mode).
void drawBlended(QPainter &p, const QImage &src, const QRect &srcArea, BlendMode m, qreal opacity = 1);

// Live Effects (Fireworks MX 2004 "Effects" menu in the Property Inspector).
enum class EffectType {
    DropShadow, InnerShadow, OuterGlow, InnerBevel, GaussianBlur,
    OuterBevel, InsetEmboss, RaisedEmboss,
    Blur, BlurMore, Sharpen, SharpenMore, UnsharpMask,
    HueSaturation, Levels, Curves, ColorFill, BrightnessContrast, Invert,
    Noise, FindEdges, ConvertToAlpha,
    // v0.2 additions
    InnerGlow, AutoLevels, MotionBlur, RadialBlur, ZoomBlur,
    Desaturate, Threshold, Posterize, ColorBalance, PhotoFilter,
    // v0.2.1 additions
    HighPass, Offset,
    // v0.2.3
    FeatherEdges,
    // v0.3.0
    GradientMap, AutoContrast, Dither, Outline, PaletteSwap, NormalMap, AmbientOcclusion,
    LensCorrection, SmartSharpen, SurfaceBlur, BilateralDenoise, MakeSeamless,
    // v0.3.1
    ReplaceColor
};

// Bevel button presets (Inner / Outer Bevel).
enum class BevelPreset { Raised, Highlighted, Inset, Inverted };
QStringList bevelPresetNames();

// Levels / Curves channels: 0 = RGB (master), 1 = Red, 2 = Green, 3 = Blue, 4 = Alpha (v0.3.0;
// not affected by the RGB master).
enum { ChRGB = 0, ChRed = 1, ChGreen = 2, ChBlue = 3, ChAlpha = 4, kChannels = 5 };

struct GradStop {
    double pos = 0;   // 0..1
    QColor color;
    bool operator==(const GradStop &o) const { return qFuzzyCompare(pos + 1, o.pos + 1) && color == o.color; }
};
QStringList channelNames();

struct LevelsChannel {
    int inBlack = 0, inWhite = 255, outBlack = 0, outWhite = 255;
    double gamma = 1.0;
    bool isIdentity() const { return inBlack == 0 && inWhite == 255 && outBlack == 0 && outWhite == 255 && gamma == 1.0; }
    bool operator==(const LevelsChannel &o) const { return inBlack == o.inBlack && inWhite == o.inWhite && outBlack == o.outBlack && outWhite == o.outWhite && gamma == o.gamma; }
};

struct LiveEffect {
    EffectType type = EffectType::DropShadow;
    bool enabled = true;
    double distance = 7;    // shadows, motion blur distance
    double angle = 315;     // degrees, 0 = right, counter-clockwise (Fireworks convention)
    double softness = 4;    // blur radius
    QColor color = Qt::black;
    QColor color2 = Qt::white;   // second colour (Replace Color: the To colour; `color` is From)
    int opacity = 65;       // 0..100
    double width = 4;       // glow width / bevel width / emboss width
    // bevels
    BevelPreset preset = BevelPreset::Raised;
    // emboss
    bool showObject = true;
    // unsharp mask (amount %), noise (amount), radial/zoom blur (amount), photo filter (density %)
    double amount = 50;
    int threshold = 0;      // unsharp mask threshold, Threshold level
    int quality = 20;       // radial / zoom blur samples
    int posterize = 4;      // Posterize levels per channel
    bool colorNoise = false;
    double brightness = 0, contrast = 0; // -100..100
    // hue / saturation (hue -180..180, saturation & lightness -100..100)
    double hue = 0, saturation = 0, lightness = 0;
    bool colorize = false;
    // levels / curves, per channel (curve points in 0..255 sorted by x; empty = identity)
    LevelsChannel levels[kChannels];
    QPolygonF curves[kChannels];
    // color balance: [range shadows/midtones/highlights][cyan-red, magenta-green, yellow-blue] in -100..100
    double balance[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    bool preserveLuminosity = true; // color balance, photo filter
    // color fill
    BlendMode blend = BlendMode::Normal;
    // high pass (radius = softness): keep colour or produce a grey high-pass
    bool keepColor = true;
    // offset: pixels to shift (positive = right / down) and what fills the uncovered areas
    enum Undefined { WrapAround = 0, RepeatEdge = 1, Transparent = 2 };
    double offsetX = 0, offsetY = 0;
    int undefinedAreas = WrapAround;
    // feather edges: radius = width; inside (fade inward from the edge) or centred on the edge;
    // falloff = amount, 0 hard .. 100 soft
    bool featherCentered = false;
    // v0.3.0 effects share generic parameters (see LiveEffect::make and fx2:: for their meaning per type)
    int mode = 0, mode2 = 0;    // method / variant selectors
    bool flag = false, flag2 = false;
    double p[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    QVector<GradStop> stops;    // gradient map
    QVector<QRgb> palette, palette2; // dither custom palette; palette swap from / to

    static LiveEffect make(EffectType t);
};

QString effectName(EffectType t);
QString effectCategory(EffectType t); // Effects menu submenu
bool effectIsAdjustment(EffectType t); // pixel adjustment (usable as a destructive filter on bitmaps)
QString effectKey(EffectType t);
EffectType effectFromKey(const QString &k, bool *ok = nullptr);
QList<EffectType> allEffectTypes();

// Fireworks MX 2004 text anti-alias modes (Property Inspector dropdown).
enum class TextAA { None, Crisp, Strong, Smooth, System };
QStringList textAANames();
QString textAAKey(TextAA a);
TextAA textAAFromKey(const QString &k, bool *ok = nullptr);

QString objTypeKey(ObjType t);
QString objTypeLabel(ObjType t);
ObjType objTypeFromKey(const QString &k, bool *ok = nullptr);

// Fill categories (Property Inspector "Fill category"). The gradient kinds follow Fireworks MX 2004.
enum class FillKind { Solid, WebDither, Pattern, Linear, Radial, Cone, Contour, Satin, Starburst, Bars, Ripples, Waves, Ellipse, Rectangle };
struct VObject {
    quint64 id = 0;
    ObjType type = ObjType::Rect;
    QString name;

    QRectF rect;           // Rect, Ellipse, Text box, Bitmap placement
    QPointF p1, p2;        // Line
    QPolygonF points;      // Path anchor points (all contours, in order)
    bool closed = false;   // Path (applies to every contour)
    // Path Bezier handles, absolute coordinates, one per point (a handle equal to its point is a
    // corner). Both empty = a polyline.
    QPolygonF hIn, hOut;
    QVector<int> contours; // Path: start indices of the 2nd, 3rd... contour (compound paths)
    // Path pressure data (v0.3.1, the Path Scrubber): one value per anchor point, interpolated along
    // each segment. Size 0..2 scales the stroke width (1 = as set), opacity 0..1 scales the stroke's
    // alpha. Empty, or a count that no longer matches the points, means uniform 1.0.
    QVector<double> ptSize, ptOpacity;
    // Raw tablet / mouse samples, one per anchor (lwd v7 optional keys). Empty = identity
    // (pressure 1, speed 1). Redraws and exports re-apply the current Sensitivity sliders.
    QVector<double> ptPressure, ptSpeed;
    // Elapsed milliseconds from the start of the stroke (Spray Can dwell). Optional v7 key.
    QVector<double> ptTime;
    static constexpr double kMaxPointSize = 2.0;
    static constexpr double kMaxPointSpeed = 4.0;
    static constexpr double kMaxPointTime = 24 * 60 * 60 * 1000.0;
    bool hasPointPressure() const;          // a Path whose data is not uniformly 1.0
    bool hasPointSense() const;             // raw pressure/speed present and not all identity
    double pointSize(int i) const;          // 1.0 without data
    double pointOpacity(int i) const;
    double pointPressure(int i) const;      // 1.0 without data
    double pointSpeed(int i) const;
    double maxPointSize() const;            // >= 1 (stroke bounds)
    void ensurePointPressure();             // both arrays sized like points (new values 1.0)
    void clearPointPressure() { ptSize.clear(); ptOpacity.clear(); }
    void clearPointSense() { ptPressure.clear(); ptSpeed.clear(); ptTime.clear(); }
    QImage image;          // Bitmap (ARGB32_Premultiplied)
    // Display/bake interpolation for this bitmap (-1 = unset). Stamped from Preferences when the
    // bitmap is created, imported, pasted or dropped. The Property Inspector Scaling dropdown
    // edits it per object; transforms / pixel edits keep the chosen method. Changing Preferences
    // must not alter existing bitmaps (0.3.16).
    int interpolation = -1;

    // Auto Shape (v0.3.0): kind key and parameters (see core/AutoShape.h); geometry from `rect`
    QString shapeKind;
    QMap<QString, double> shapeParams;

    // text
    QString text;
    QString fontFamily = QStringLiteral("Sans Serif");
    double fontSize = 24;
    bool bold = false, italic = false, underline = false;
    double kerning = 0;        // tracking, % of the em (Fireworks "Kerning"), -100..100+
    double leading = 100;      // line spacing, % of the font's natural line spacing
    double hScale = 100;       // horizontal glyph scale, %
    double baselineShift = 0;  // px, positive raises the text
    int textAlign = 0;     // 0 left, 1 center, 2 right, 3 justify
    int textOrientation = 0; // 0 horizontal, 1 vertical
    int textDirection = 0;   // 0 LTR, 1 RTL
    double paraIndent = 0;   // first-line indent, px
    double paraSpaceBefore = 0, paraSpaceAfter = 0; // paragraph spacing, px
    TextAA textAA = TextAA::Smooth;

    bool fillOn = true;
    QColor fill = QColor(0x66, 0x99, 0xCC);
    bool strokeOn = true;
    QColor stroke = Qt::black;
    double strokeWidth = 1;
    double roundness = 0;  // 0..100 % for rectangles
    bool antialias = true; // fill edge: false = Hard
    // fill category, gradient, pattern, texture and feathered edge
    FillKind fillKind = FillKind::Solid;
    QVector<GradStop> gradient;           // empty = derived from `fill` (fill colour to white)
    QPointF gradP0{0, 0.5}, gradP1{1, 0.5}, gradP2{0, 0}; // handles, normalised to bounds()
    QString pattern = QStringLiteral("Bricks"); // procedural pattern name, or "Custom" (patternImage)
    QImage patternImage;
    QString texture;                      // empty = none; "Procedural" = textureProc
    QString textureProc;                  // procedural texture parameters (proc::Params JSON)
    QString patternProc;                  // pattern "Procedural": proc::Params JSON
    // pattern fill handles (0.3.4): the tile's x / y axes (m11 m12 / m21 m22: scale, rotation and
    // skew, about the centre handle) and the centre handle's offset from the bounds' centre
    // (dx dy). Identity = the tile at the bounds' top-left, unscaled (the pre-0.3.4 look).
    QTransform patternXform;
    int textureAmount = 0;                // 0..100 %
    double feather = 0;                   // > 0 = Feather fill edge (px)
    QString strokeKind = QStringLiteral("basic.hard"); // stroke category preset key
    bool strokeCustom = false;               // true: strokeParams (Edit Stroke) override the factory preset
    stroke::Params strokeParams;             // Edit Stroke options / shape / sensitivity (format 7)
    int strokeAlign = 0;                  // 0 centred, 1 inside, 2 outside
    QString strokeTexture;                // stroke texture (same names as the fill texture; "Procedural" = strokeTextureProc)
    QString strokeTextureProc;
    int strokeTextureAmount = 0;          // 0..100 %
    quint32 strokeSeed = 0;               // stored random-stroke seed; 0 = derive from id
    // text attached to a path: the guide path lives in points/hIn/hOut/closed
    bool textOnPath = false;
    double textPathOffset = 0;            // px along the path
    bool textPathReverse = false;         // Text > Reverse Direction

    int opacity = 100;     // 0..100
    BlendMode blend = BlendMode::Normal;
    bool visible = true;
    bool locked = false;
    bool quickMask = false;   // Select > Quick Mask: a greyscale mask shown as a red overlay
    QVector<LiveEffect> effects;

    // web (Slice / Hotspot)
    WebShape webShape = WebShape::Rect;  // slices/hotspots: rect, circle (ellipse in rect) or polygon (points)
    QString link, alt, target;
    WebBehavior behavior = WebBehavior::None;
    quint64 swapTarget = 0;  // swap image: slice whose image changes (0 = this slice)
    int swapFrame = 1;       // swap image / rollover: frame index shown on mouse over
    bool sliceCustomOpt = false; // per-slice optimize settings
    ExportOptions sliceOpt;

    // symbol instance: `rect` is where the symbol's bounds are placed (scaled to fit)
    quint64 symbolId = 0;
    int symFrame = 0;        // animation symbols: frame shown; buttons: state (0 = Up)

    // Group (v0.3.2, Fireworks MX 2004 style). `children` are bottom first; the group's geometry,
    // moves and transforms are its children's (a group never has its own xform). Opacity, blend
    // and Live Effects apply to the rendered group as a unit.
    // Mask: `maskObj` holds 0 or 1 object (any type). maskMode 0 = path outline for a vector mask
    // or the alpha channel for a bitmap mask; 1 = grayscale (white shows, black hides, transparent
    // hides). Nothing shows outside the mask object. A linked mask moves and transforms with the
    // group; an unlinked one stays where it is. A disabled mask is kept but not applied.
    QVector<VObject> children;
    QVector<VObject> maskObj;
    int maskMode = 0;
    bool maskEnabled = true;
    bool maskLinked = true;
    enum MaskMode { MaskOutline = 0, MaskGrayscale = 1 };
    bool hasMask() const { return type == ObjType::Group && !maskObj.isEmpty(); }
    bool maskActive() const { return hasMask() && maskEnabled; }
    // calls f(VObject&) on this object and everything nested in it (children and mask), depth first
    template <class F> void forEachNested(F &&f) {
        f(*this);
        for (VObject &c : children) c.forEachNested(f);
        for (VObject &m : maskObj) m.forEachNested(f);
    }
    template <class F> void forEachNested(F &&f) const {
        f(*this);
        for (const VObject &c : children) c.forEachNested(f);
        for (const VObject &m : maskObj) m.forEachNested(f);
    }

    // Non-destructive transform (Free Transform, Skew, Distort, Perspective, Numeric Transform):
    // maps the object's own geometry (rect, image, text box, symbol) onto the canvas. Paths and
    // lines bake transforms into their points instead; see usesXform().
    QTransform xform;
    bool hasXform() const { return !xform.isIdentity(); }
    bool usesXform() const {
        return type == ObjType::Rect || type == ObjType::Ellipse || type == ObjType::AutoShape || type == ObjType::Bitmap || type == ObjType::Instance ||
               (type == ObjType::Text && !textOnPath);
    }
    VObject untransformed() const { VObject c = *this; c.xform = QTransform(); return c; }

    QRectF bounds() const;           // geometric bounds
    QRectF visualBounds() const;     // bounds including stroke & effects
    QRectF artworkBounds() const;    // tight bounds for clipboard / File > New: exact bitmap pixels, otherwise object bounds with stroke and effects but no antialias pad
    void moveBy(const QPointF &d);
    void setBounds(const QRectF &nb); // scales geometry into nb
    QPainterPath shapePath() const;
    bool hitTest(const QPointF &p, double tolerance) const;
    QFont font() const;              // includes the anti-alias style strategy / hinting
    struct TextLine { QPointF baseline; QString text; };
    double lineAdvance() const;          // baseline-to-baseline distance (leading applied)
    QVector<TextLine> textLines() const; // laid-out lines (word wrapped to the box)
    QPainterPath textPath() const;   // glyph outlines of textLines() (or laid out along the path)
    QPainterPath guidePath() const;  // text on a path: the guide
    void fitTextBox();               // size the text box to its content
    bool hasCurves() const;          // Path has non-trivial handles
    void ensureHandles();            // hIn/hOut sized like points
    void dropTrivialHandles();       // clears hIn/hOut when every handle sits on its point
    int contourCount() const { return type == ObjType::Path ? int(contours.size()) + 1 : 1; }
    void contourRange(int c, int *start, int *end) const; // [start, end)
    int contourOfPoint(int i) const;
    bool isWeb() const { return type == ObjType::Slice || type == ObjType::Hotspot; }
    bool isVector() const { return type != ObjType::Bitmap && type != ObjType::Instance && type != ObjType::Group && !isWeb(); }
    bool isInstance() const { return type == ObjType::Instance; }
    bool isPolygonWeb() const { return isWeb() && webShape == WebShape::Polygon; }
    bool usesPoints() const { return type == ObjType::Path || isPolygonWeb(); }
    QString displayName() const;
    double effectMargin() const;
    // Conservative pixel reach of enabled effects (size-aware for Radial/Zoom blur).
    double effectReach() const;
    // Bitmap Scaling method (see field `interpolation`).
    int effectiveInterpolation() const; // live Preferences default when unset (-1)
    void stampInterpolation();          // record Preferences default (new bitmaps only)
    // Stamp if this is an unset bitmap (no-op otherwise). Prefer over ad-hoc type checks.
    void ensureInterpolationStamped();
};

// A symbol (Library panel). Its objects are stored in their own coordinate space; an
// instance maps bounds() onto its rect.
struct Symbol {
    quint64 id = 0;
    QString name;
    SymbolKind kind = SymbolKind::Graphic;
    QVector<QVector<VObject>> frames;  // animation frames / button states; graphic: one frame
    // 0.3.17: body digest for instance visual-cache keys (includes nested instance digests).
    // Cleared when frames change; recomputed lazily while a SymbolScope is active.
    mutable QByteArray bodyKeyCache;
    void invalidateBodyKey() const { bodyKeyCache.clear(); }
    QRectF bounds() const;             // union of the geometric bounds of every frame
    QRectF visualBounds() const;       // including strokes and effects
    QRectF artworkBounds() const;      // like visualBounds, but without the antialias pad
    int frameCount() const { return qMax(1, int(frames.size())); }
};

#include <QTransform>
// Symbol lookup while rendering: a document installs its symbol table for the duration of a
// render (RAII); instances drawn outside any scope draw nothing.
class SymbolScope {
public:
    explicit SymbolScope(const QVector<Symbol> *syms);
    ~SymbolScope();
    SymbolScope(const SymbolScope &) = delete;
    SymbolScope &operator=(const SymbolScope &) = delete;
private:
    const QVector<Symbol> *m_prev;
};
const Symbol *findSymbol(quint64 id);                     // in the current scope
QTransform instanceTransform(const VObject &inst, const Symbol &sym); // symbol coords -> canvas

// Tight size for File > New when these objects are on the clipboard.
// Uses the artwork box size (ceil of width/height), not toAlignedRect(), so a
// fractional origin does not add a pixel on each axis.
QSize clipboardCanvasSize(const QVector<VObject> &objs);
QSize artworkPixelSize(const QRectF &area);
