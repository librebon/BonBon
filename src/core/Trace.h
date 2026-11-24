#pragma once
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPolygon>
#include <QVector>
#include <atomic>

class Document;
struct VObject;

// Modify > Trace Bitmap (v0.3.1): bitmap -> filled vector paths.
// Own implementation of the method described in P. Selinger, "Potrace: a polygon-based tracing
// algorithm" (2003): boundary (crack) following on the pixel-corner lattice, speck removal (turd
// size), polygon fitting, corner / curve decision with alphamax, and optional curve merging.
// No Potrace source code is used.
namespace trace {

enum class Mode { Bw = 0, Colors = 1, PixelArt = 2, Centerline = 3 };

struct Options {
    Mode mode = Mode::Bw;
    // black and white / centreline
    int threshold = 128;            // luminance 0..255; darker than this is traced
    bool invert = false;            // trace the light pixels instead
    // shared
    int turdSize = 2;               // drop specks (and holes) of at most this many pixels
    double alphaMax = 1.0;          // 0 = every vertex a corner .. 1.334 = no corners
    bool optimize = true;           // merge consecutive curve segments
    double optTolerance = 0.2;      // px, max deviation when merging
    // colours
    int colors = 8;                 // 2..256 (adaptive palette)
    QVector<QRgb> palette;          // non-empty: map onto this palette (e.g. the indexed palette)
    bool stacked = true;            // stacked shapes (no gaps) or cut-out (no overlap)
    int smoothing = 0;              // majority-filter passes over the colour map (0..10)
    // transparency
    bool ignoreTransparent = true;  // alpha < 128 is background (not traced); else composite on white
    bool removeBackground = false;  // drop the background colour (the most common border colour)
};

struct Shape {
    QPainterPath path;   // image pixel coordinates; odd-even fill
    QColor color;
    bool stroke = false; // centreline: an open stroked path
    double strokeWidth = 0;
};

struct Result {
    QVector<Shape> shapes;  // bottom first
    int nodes = 0;          // anchor points in all paths
    bool cancelled = false;
};

Result trace(const QImage &img, const Options &o, const std::atomic_bool *cancel = nullptr);

// ---- building blocks (exposed for the tests)
// Closed boundary loops of the set pixels (mask w*h, non-zero = set) on the pixel-corner lattice,
// outer boundaries clockwise on screen, holes anticlockwise. Every lattice step is a vertex.
QVector<QPolygon> boundaries(const QVector<uchar> &mask, int w, int h);
double loopArea(const QPolygon &loop);                       // signed (positive = clockwise on screen)
QPolygon cornersOnly(const QPolygon &loop);                  // drops collinear vertices
// Smooth closed paths through the loops (odd-even fill reproduces the mask's region).
QPainterPath smoothLoops(const QVector<QPolygon> &loops, double alphaMax, bool optimize, double tol);
QPainterPath polygonLoops(const QVector<QPolygon> &loops);   // exact pixel staircase outlines
QVector<uchar> thin(const QVector<uchar> &mask, int w, int h); // Zhang-Suen skeleton
int countNodes(const QPainterPath &p);

// ---- document side
struct Insert {
    bool group = true;          // put the paths on a new layer "Trace of <name>"
    bool keepOriginal = true;   // else the bitmap is deleted
    bool hardEdges = false;     // paths drawn without anti-aliasing (pixel-art mode)
    bool asGroup = false;       // v0.3.2: else, one group "Trace of <name>" above the bitmap (when !group)
};
// The image-pixel -> canvas mapping of a bitmap object (its rect, then its transform).
QTransform imageToCanvas(const VObject &bitmap);
// Path objects for a result, in canvas coordinates (bottom first). Ids are not assigned.
QVector<VObject> toObjects(const VObject &bitmap, const Result &r, bool hardEdges = false);
// Inserts the traced paths of bitmap `bitmapId` (current frame) and selects them. Does not commit.
// Returns the new object ids (empty = nothing inserted, document unchanged).
QVector<quint64> insert(Document &d, quint64 bitmapId, const Result &r, const Insert &how);
}
