#pragma once
#include "Brand.h"
#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QHash>
#include <QVariantMap>
#include <QString>
#include <QRectF>
#include "core/Object.h"
#include "Theme.h"
#include <QVector>

// Iterate with toolCount() / kLastTool (new tools are appended so saved indices stay valid).
enum class Tool {
    Pointer, Subselect, Scale, Skew, Distort, Perspective, Crop,
    Marquee, OvalMarquee, Lasso, PolyLasso, MagicWand, Brush, Pencil, Eraser,
    Blur, Sharpen, Dodge, Burn, Smudge, RubberStamp, Healing, SpotHealing, Eyedropper, PaintBucket, Gradient,
    Line, Pen, Rect, Ellipse, Polygon, Text, Knife, Freeform, ReshapeArea,
    Slice, Hotspot, CircleHotspot, PolyHotspot,
    // v0.3.0: Content-Aware Remove (Rubber Stamp flyout) and the Auto Shapes (shape flyout), in autoshape::kinds() order
    ContentAware,
    // v0.3.1: Replace Color (Rubber Stamp flyout, as in Fireworks)
    ReplaceColor,
    // v0.3.1: Export Area (Crop flyout, as in Fireworks)
    ExportArea,
    // v0.3.1: Red Eye Removal (Rubber Stamp / Replace Color flyout)
    RedEye,
    // v0.3.1: Path Scrubber (Freeform / Reshape Area flyout)
    PathScrubber,
    AsRoundRect, AsChamfer, AsBevel, AsPolygon, AsStar, AsSmartPolygon, AsArc, AsDoughnut, AsSpiral,
    AsArrow, AsArrowLine, AsConnector, AsGear, AsFrame, AsLShape, AsSpeech,
    Hand, Zoom,
    // 0.3.20: Polygon Slice (Slice flyout). Appended after Zoom to keep saved tool indices stable.
    PolySlice,
    // 0.3.20: Pointer flyout / Pen flyout. Appended so saved tool indices stay stable.
    SelectBehind, VectorPath, RedrawPath,
    // 0.3.34: Heart auto shape. Appended so saved tool indices stay stable.
    AsHeart
};

QString toolName(Tool t);
QString toolActionId(Tool t); // stable shortcuts:: catalog id (tool.* / tool.autoshape.*)
Tool toolFromActionId(const QString &id); // Tool::Pointer when unknown
QString toolShortcut(Tool t);
// Tools sharing one toolbox slot (flyout). Tools with the same shortcut key cycle on repeated presses.
QList<Tool> toolGroup(Tool t);
bool toolIsHotspot(Tool t);
bool toolIsSlice(Tool t);
bool toolAvailable(Tool t); // false for slice / hotspot tools when Enable web features is off
bool webFeaturesEnabled();
bool isWebFeatureAction(const QString &id); // slice/hotspot tools, Insert Slice/Hotspot, Export HTML/Slices/Image Map
inline constexpr Tool kLastTool = Tool::AsHeart;
inline constexpr int toolCount() { return int(kLastTool) + 1; }
bool toolIsStub(Tool t);
bool toolIsBitmap(Tool t);
inline bool toolIsAutoShape(Tool t) { return (int(t) >= int(Tool::AsRoundRect) && int(t) <= int(Tool::AsSpeech)) || t == Tool::AsHeart; }
QString toolAutoShapeKind(Tool t);          // "roundrect"...; empty for other tools
Tool autoShapeTool(const QString &kind);    // Tool::Pointer when unknown
// Scale, Skew, Distort, Perspective: the Free Transform tools (one toolbox flyout)
inline bool toolIsTransform(Tool t) { return t == Tool::Scale || t == Tool::Skew || t == Tool::Distort || t == Tool::Perspective; }
inline bool toolIsSelect(Tool t) { return t == Tool::Pointer || t == Tool::SelectBehind; }


// Edit > Preferences. Persisted with QSettings (see AppState::loadPrefs/savePrefs).
// undoSteps and interpolation live in core (coreprefs) and are mirrored there.
struct Prefs {
    int undoSteps = 200;
    int undoMemoryMB = 1024;        // bitmap memory one document's history may hold; the oldest steps go first
    int hdrToneMap = 2;             // HDR import (EXR / HDR / PFM): 0 Clamp, 1 Reinhard, 2 ACES, 3 Exposure
    double hdrExposure = 0;         // EV added before the tone-map operator
    QColor defaultStroke = brand::defaultStrokeColor();
    QColor defaultFill = Qt::white;
    bool mouseHighlight = true;
    QColor highlightColor = QColor(0xE0, 0x20, 0x20);
    QColor selectionColor = QColor(0x00, 0x66, 0xFF);
    QColor pasteboardColor = QColor(0x9C, 0x99, 0x93); // effective colour: the theme's unless pasteboardOverride
    // Interface theme (Preferences > Themes), see theme::list(). followSystem swaps to a dark /
    // light theme with the desktop's colour scheme. The overrides replace the theme's accent
    // (selection / highlight) and pasteboard colours.
    #if LW_IS_BONBON
    QString theme = QStringLiteral("bonbon");
#else
    QString theme = QStringLiteral("classic");
#endif
    bool themeFollowSystem = false;
    bool accentOverride = false;
    QColor accentColor = QColor(0x2A, 0x5D, 0xA8);
    bool pasteboardOverride = false;
    // Custom from colour: the seed theme ("custom") and the saved named themes ("user:<name>")
    QColor customSeed = QColor(0xC2, 0x3B, 0x7A);
    int customMode = 0;             // theme::SeedMode: 0 Auto, 1 Light, 2 Dark
    int customVibrancy = 40;        // 0..100
    QVector<theme::Spec> userThemes;
    int pickDistance = 4;           // screen pixels
    bool brushSizeCursors = true;   // painting tools show the brush footprint
    bool preciseCursors = false;    // crosshair instead of the default tool cursors
    int shiftFineSpeed = 33;        // pop-up slider fields: Shift fine control speed, % of normal (5..100, 0.3.5)
    int ctrlSnapIncrement = 10;     // sliders: Ctrl+drag / scroll / arrow snap step, in the slider's units (1..100, 0.3.33)
    int strokePreviewBg = 2;        // Edit Stroke preview: 0 White, 1 Black, 2 Auto (default; 0.3.34)
    int sliderOrientation = 1;      // pop-up slider fields: 0 horizontal, 1 vertical (default since 0.3.2; Fireworks' vertical sliders)
    int flyoutHoldMs = 150;         // press-and-hold delay before a tool flyout opens (Fireworks: short)
    int interpolation = 0;          // 0 Bicubic, 1 Bilinear, 2 Soft, 3 Nearest Neighbour
    int gridSize = 20;
    QColor gridColor = QColor(0, 120, 255, 60);
    bool checkerboard = true;       // checkerboard behind transparent canvases
    // By default the document sits flat on the pasteboard: no drop shadow and no border, the
    // document edge meets the pasteboard directly (canvas and preview panes). Both are opt-in.
    // Pointer / Scale / Crop drags snap edges and centres to the canvas edges and centre lines and
    // (smart guides) to other objects, within 6 screen px; hold Ctrl while dragging to suspend it.
    bool snapToCanvas = true;
    bool snapToObjects = true;
    bool canvasShadow = false;      // soft drop shadow behind the document
    bool canvasBorder = false;      // 1px neutral line just outside the document
    int recentFiles = 10;
    // Preferences > Tablet (v0.3.1): the pressure curve (control points, x raw -> y output), the size /
    // opacity floors at zero pressure (%), and whether the pen's eraser end switches to the Eraser.
    QVector<QPointF> tabletCurve = {QPointF(0, 0), QPointF(1, 1)};
    int tabletMinSize = 10;
    int tabletMinOpacity = 0;
    bool tabletEraser = true;
    int fitMargin = 0;              // Fit All: screen px left round the document (0: it fills the view on the limiting axis)
    // Fonts (0.3.6): system fonts and/or a custom folder of .ttf/.otf/.ttc/.pfb
    bool useSystemFonts = true;
    bool useCustomFonts = false;
    QString customFontDir;
    // 0.3.7: when on, last-used tool options carry to new documents and persist in QSettings
    bool carryToolSettings = false;
    bool pasteAtMouse = false;              // Paste centres on pointer when over the canvas
    bool useSystemIcons = false;           // 0.3.8: off = bundled GNOME-Wine (hue-shifted); on = system theme
    bool useSystemFileDialogs = false;     // 0.3.19: off = Qt non-native (themed) dialog; on = platform native
    bool enableWebFeatures = false;       // 0.3.22: off (default) hides the Web tools section, Insert Slice/Hotspot, and slice export commands. Existing slices stay visible on the canvas, in Layers, and in the inspector. Documents still load, round-trip and export unchanged.
};




// Working tool state (active tool, colours, tool options). Held on AppState for the UI,
// but owned per document (Document::toolEnv) since 0.3.7 — tab switches swap it.
// Colour of the optional 1px document border (neutral, a little darker than the default pasteboard).
inline QColor canvasBorderColor() { return QColor(0x80, 0x80, 0x80); }
// Optional drop shadow behind the document target `r` (drawn first, entirely outside/under the document).
void drawCanvasShadow(class QPainter &p, const QRectF &r);

class AppState : public QObject {
    Q_OBJECT
public:
    static AppState &get();

    Tool tool() const { return m_tool; }
    void setTool(Tool t);

    QColor stroke = brand::defaultStrokeColor();
    QColor fill = Qt::white;
    bool strokeOn = true;
    bool fillOn = true;
    double strokeWidth = 1;
    double roundness = 0;
    int brushSize = 6;
    int pathToolSize = 30;    // Freeform / Reshape Area size
    int pathPrecision = 5;    // Vector Path / Redraw Path: 1 loose .. 10 exact
    int reshapeStrength = 50; // Reshape Area strength (%)
    int gradientKind = 3;     // Gradient tool: FillKind applied to solid objects (3 = Linear)
    int tolerance = 32;
    int toolIntensity = 50;   // Blur/Sharpen intensity, Dodge/Burn exposure, Smudge pressure, Stamp opacity
    int toneRange = 1;        // Dodge/Burn: 0 shadows, 1 midtones, 2 highlights
    bool stampAligned = true; // Rubber Stamp / Healing Brush: source follows the pointer across strokes
    // Marquee / Oval Marquee: Style and Edge; Lasso / Polygon Lasso and Magic Wand: Edge
    enum MarqueeStyle { StyleNormal = 0, StyleFixedRatio = 1, StyleFixedSize = 2 };
    int marqueeStyle = StyleNormal;
    double marqueeRatioW = 1, marqueeRatioH = 1;   // Fixed Ratio
    int marqueeFixedW = 64, marqueeFixedH = 64;    // Fixed Size (px)
    int marqueeEdge = 0, lassoEdge = 0, wandEdge = 0; // Document::SelEdge
    double marqueeFeather = 10, lassoFeather = 10, wandFeather = 10;
    // Healing Brush / Spot Healing: blend mode of the healed pixels (0 Normal, 1 Replace, 2 Lighten, 3 Darken)
    int healMode = 0;
    int brushEdge = 50;                             // tip softness 0 (hard) .. 100 (soft) for Healing / retouch
    bool useEntireDocument = false;                 // Stamp / Healing / Smudge: sample all visible layers
    bool wandSampleAllLayers = false;               // Magic Wand / Select Similar: sample flattened canvas (off = active bitmap only)
    // Tip and paint options. Brush, Pencil, Eraser, the retouch tools, Paint Bucket and Gradient each
    // keep their own copy of the "per-tool" ones (see perToolKeys()): switching tools swaps them.
    int tipShape = 0;            // 0 round, 1 square
    int toolOpacity = 100;       // 1..100 %
    int toolBlend = 0;           // BlendMode
    int brushCategory = 0;       // brush::Category
    int brushTip = 0;            // tip within brushCategory (0.3.6)
    QString brushTexture;        // empty = none (fills::textureNames())
    int brushTextureAmount = 0;  // 0..100 %
    bool autoErase = false;      // Pencil: starting on the stroke colour paints with the fill colour
    bool preserveTransparency = false;   // Brush / Pencil: only recolour existing pixels
    bool smudgeUseColor = false; // Smudge: start every stroke with smudgeColor instead of the pixels under it
    QColor smudgeColor = Qt::black;
    // Replace Color tool (v0.3.1). Size / Edge / Opacity (strength) are the per-tool brushSize / brushEdge / toolOpacity.
    QColor rcFrom = QColor(255, 0, 0);
    QColor rcTo = QColor(0, 0, 255);
    bool rcFromImage = false;    // From: false = the From swatch, true = the image colour under the pointer when a stroke starts
    int rcTolerance = 20;        // 0..100 %
    int rcRange = 0;             // 0 Solid, 1 Color (keep the shading)
    bool rcPixelPerfect = false; // aliased tip and all-or-nothing pixels
    // Red Eye tool (v0.3.1). Size / Edge / Strength are the per-tool brushSize / brushEdge / toolOpacity.
    int reTolerance = 50;        // 0..100: how little red is enough
    int reDarken = 50;           // 0..100: how much darker the pupil becomes
    int eyedropperSample = 0;    // 0 point, 1 3x3 average, 2 5x5 average
    bool fillSelectionOnly = false;   // Paint Bucket: fill the whole pixel selection, no flood
    bool fillSelectedObject = true;   // Paint Bucket / Gradient: a click fills the selected vector object
    int fillEdge = 1;            // Paint Bucket / Gradient: 0 hard, 1 anti-alias, 2 feather
    int fillFeather = 4;
    // Auto Shape tools: parameters for new shapes, per kind (missing keys: autoshape::defaults)
    QMap<QString, QMap<QString, double>> autoShapeParams;
    QMap<QString, double> autoShapeParamsFor(const QString &kind) const;
    bool pixelPerfect = false;   // Pencil: pixel-perfect cleanup of 1px strokes (removes L-shaped corners)
    // Pencil "Left/Right colours" (pixel art): the left button paints pencilLeft, the right button
    // pencilRight; an invalid pencilLeft follows the stroke colour, a transparent one erases to alpha 0
    bool pencilTwoColor = false;
    QColor pencilLeft;
    QColor pencilRight = QColor(0, 0, 0, 0);
    QColor pencilColor(bool right) const { return right ? pencilRight : (pencilLeft.isValid() ? pencilLeft : stroke); }
    int polygonSides = 5;        // Polygon tool
    bool polygonStar = false;
    bool polygonAutoAngle = true;
    double polygonAngle = 38;    // star point angle in degrees (smaller = sharper)
    bool pathPressure = false;   // Freeform / Reshape Area / Path Scrubber: tablet pressure scales the size
    // Path Scrubber (v0.3.1): edits the per-point pressure data of vector paths
    int psMode = 0;              // 0 Increase (+), 1 Decrease (-); Alt flips while dragging, X swaps
    bool psAffectSize = true;
    bool psAffectOpacity = false;
    int psStrength = 50;         // 1..100 %
    int psSize = 30;             // brush diameter in px
    QString linkUrl, linkAlt, linkTarget;   // Slice / hotspot tools: link given to new objects
    QString strokeKind = QStringLiteral("basic.hard");   // vector tools: stroke category of new objects
    stroke::Params strokeParams;                        // custom Edit Stroke values for new objects
    bool strokeCustom = false;
    // Per-tip custom stroke params for the bitmap Brush. Separate from the vector
    // defaults above so editing one does not change the other.
    QHash<QString, stroke::Params> brushStrokeByTip;
    int textAlign = 0;           // Text tool: 0 left, 1 center, 2 right, 3 justify
    int textOrientation = 0;     // 0 horizontal, 1 vertical
    int textDirection = 0;       // 0 LTR, 1 RTL
    double paraIndent = 0, paraSpaceBefore = 0, paraSpaceAfter = 0;
    QVector<GradStop> gradientStops;        // Gradient tool: colours applied (empty = fill colour to white)
    static QString encodeStops(const QVector<GradStop> &s);
    static QVector<GradStop> decodeStops(const QString &s);
    // Brush / Pencil "Stroke target": where a stroke's pixels go.
    enum StrokeTarget { NewBitmapPerStroke = 0, ExpandSelected = 1, ClipToSelected = 2 };
    int strokeTarget = ExpandSelected; // Fireworks grows the selected bitmap to take the stroke
    bool contiguous = true;
    bool antialias = true;
    // Tablet (v0.3.1), per painting tool: what the pen pressure changes, and whether tilt / rotation steers
    // a calligraphic nib (Brush). The Pencil follows the pressure with its size only.
    bool pressureSize = true;
    bool pressureOpacity = false;
    bool pressureFlow = false;
    bool tiltAngle = true;
    QString fontFamily = QStringLiteral("Sans Serif");
    double fontSize = 28;
    QColor textColor = Qt::black;
    bool bold = false, italic = false, underline = false;
    double kerning = 0, leading = 100, hScale = 100, baselineShift = 0;   // Text tool defaults
    TextAA textAA = TextAA::Smooth;
    bool webLayerVisible = true;
    // Crop tool: the visible canvas's crop box (document px), mirrored for the Property Inspector
    QRectF cropRect;
    bool cropActive = false;
    // Export Area tool: the visible canvas's export rectangle (document px, integer), mirrored for the
    // Property Inspector. lastExportArea is the last rectangle used for an export this session (Batch "Use saved area").
    QRect exportAreaRect;
    bool exportAreaActive = false;
    QRect lastExportArea;
    bool showRulers = false;    // View > Rulers (application-wide, remembered)

    void setStroke(const QColor &c) { stroke = c; emit colorsChanged(); }
    void setFill(const QColor &c) { fill = c; emit colorsChanged(); }
    void swapColors() { std::swap(stroke, fill); emit colorsChanged(); }
    void defaultColors() { stroke = prefs.defaultStroke; fill = prefs.defaultFill; emit colorsChanged(); }

    Prefs prefs;
    void setPrefs(const Prefs &p); // applies, mirrors core prefs, emits prefsChanged
    // Valid during prefsChanged: true when setPrefs actually changed these groups.
    bool lastPrefsChangedFontSources() const { return m_lastPrefsChangedFontSources; }
    bool lastPrefsChangedWebFeatures() const { return m_lastPrefsChangedWebFeatures; }
    void applyFontSources(); // reload system/custom fonts from prefs
    void applyUiFontForSources(); // after prefsChanged: fallback or restore the UI font
    // Fonts the UI should list: system families, custom-folder families, or both.
    // Custom-only with an empty folder returns an empty list (the UI font still falls back).
    QStringList fontSourceFamilies() const;
    static QString fallbackUiFontFamily();
    void loadPrefs();              // from QSettings
    void savePrefs() const;
    void notifyOptions() { emit optionsChanged(); } // tool options live on the current document (0.3.7)
    void loadToolOptions();        // no-op kept for call sites; tool options are per-document
    // Snapshot / restore the working tool environment (active tool, colours, options, per-tool slots).
    // Empty object = built-in defaults. Stored on Document::toolEnv and in .lwd v5+.
    QJsonObject captureEnvironment() const;
    void applyEnvironment(const QJsonObject &env); // empty → defaults; does not mark docs dirty
    static QJsonObject defaultEnvironment();
    void saveCarriedEnvironment() const; // QSettings when prefs.carryToolSettings
    bool loadCarriedEnvironment();       // true if a carried env was applied
    // Every tool option by key (what is persisted; tests compare snapshots).
    QVariantMap optionValues() const;
    void setOptionValues(const QVariantMap &m);
    static QStringList perToolKeys();       // options each painting tool keeps for itself
    static bool hasToolSlot(Tool t);
    static QVariantMap defaultSlot(Tool t);

signals:
    void toolChanged(Tool t);
    void colorsChanged();
    void optionsChanged();
    void statusMessage(const QString &msg);
    void prefsChanged();
    void cropChanged();                    // the canvas crop box moved (cropRect / cropActive)
    void cropEdited(const QRectF &r);      // Property Inspector typed a crop box
    void cropCommand(bool apply);          // Property Inspector Crop / Cancel
    void exportAreaChanged();              // the canvas export area moved (exportAreaRect / exportAreaActive)
    void exportAreaEdited(const QRect &r); // Property Inspector typed an export area
    void exportAreaCommand(bool doExport); // Property Inspector Export... / Cancel
    void exportAreaRequested(const QRect &r); // Enter / double-click inside: open Export Preview for that area

private:
    AppState();
    Tool m_tool = Tool::Pointer;
    QHash<int, QVariantMap> m_slots;   // per-tool options of the tools not active now
    bool m_lastPrefsChangedFontSources = false;
    bool m_lastPrefsChangedWebFeatures = false;
};

inline AppState &appState() { return AppState::get(); }
