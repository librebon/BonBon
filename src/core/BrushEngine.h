#pragma once
#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QVector>
#include <QPainterPath>
#include <QElapsedTimer>
#include "Object.h"
#include "Tablet.h"

// Bitmap Brush / Pencil / Eraser strokes, Fireworks style: the dabs of a stroke build a coverage
// mask (0..1) over the target, and the target is recomposited from a snapshot taken when the stroke
// began: target = base (+) colour x mask x opacity x texture, through the blend mode. So opacity
// applies to the stroke as a whole (overlapping dabs do not build up, except where the brush
// category accumulates, like the Air Brush), and the edge, shape, texture, Preserve Transparency
// and erasing all come out of the same place.
namespace brush {
enum class Category { Basic, AirBrush, Calligraphy, Charcoal, Crayon, FeltTip, Watercolor,
                      Pencil, Oil, Dashed, Random, Unnatural, Libreworks };  // new values must stay after Watercolor
QStringList categoryNames();

struct Params {
    double size = 6;                 // tip diameter in target pixels
    int edge = 0;                    // 0 hard (anti-aliased) .. 100 soft
    bool square = false;
    Category category = Category::Basic;
    QString texture;                 // fills::textureNames(), empty = none
    int textureAmount = 0;           // 0..100
    int opacity = 100;               // 1..100
    BlendMode blend = BlendMode::Normal;
    QColor color = Qt::black;
    bool erase = false;              // Eraser: removes coverage from the target
    bool preserveTransparency = false;
    bool pencil = false;             // Pencil: square, 1px-precise tip, aliased unless antialias
    bool antialias = true;
    // Tablet (v0.3.1): what the pen pressure changes (PressureSize | PressureOpacity | PressureFlow; the
    // Pencil uses only the size), the floors from Preferences > Tablet, and tilt / rotation steering the
    // Calligraphy nib. Pressures arrive already through the pressure curve; a mouse presses at 1.0.
    int pressure = 0;
    double minSize = 0.1;            // 0..1 of the size at zero pressure
    double minOpacity = 0.0;         // 0..1 of the opacity / flow at zero pressure
    bool tilt = false;
    double nibRatio = 0.25;          // Calligraphy: nib width / length (Basic 0.2, Bamboo 0.3, Quill 0.1, Ribbon 0.05)
    double nibAngle = -45;           // degrees, Fireworks Shape angle; tilt can override
    QString strokeKey = QStringLiteral("basic.hard");
    stroke::Params strokeParams;
    bool strokeCustom = false;
};
QString categoryName(Category c);
Category categoryFromName(const QString &name);

// tip / type within a category (Fireworks stroke name); first tip is the pre-0.3.6 default
QStringList tipNames(Category c);
int tipIndex(Category c, const QString &tip);
void applyTip(Params &p, Category c, int tipIndex); // distinct parameters per tip
// Canonical stroke key for a tip. Empty if the tip has none.
QString tipStrokeKey(Category c, int tipIndex);
// Seed Edit Stroke from the live Brush tip (PI size/edge/shape/texture, and
// dab-engine defaults for ink/flow/build-up), not from the vector preset alone.
// Opening the dialog and clicking OK with these params must not change pixels.
stroke::Params liveEditStrokeParams(const Params &bp);
enum PressureFlags { PressureSize = 1, PressureOpacity = 2, PressureFlow = 4 };

// Immutable snapshot of a path-engine stroke, safe to paint on a worker thread.
struct PathSnapshot {
    QPainterPath path;
    QVector<double> dist, pressure, speed, time;
    Params params;
    quint32 seed = 1;
    int cellCs = 0;
    QSize size;
    QImage under;          // for looks that sample the base (Cellular)
    int fromIndex = 0;     // 0 = whole path; else suffix starting at this vertex
    int elementCount = 0;  // identity of this snapshot (path.elementCount() when taken)
    quint64 gen = 0;       // Canvas increment on a left/up grow, to drop stale jobs
};

// Paint `snap` into a transparent premultiplied image of snap.size.
// Thread-safe: uses only the snapshot (per-thread under-sample cache).
QImage renderPathLayer(const PathSnapshot &snap);

class Stroke {
public:
    void begin(const QImage &target, const Params &p);
    bool active() const { return !m_base.isNull(); }
    void end();
    const Params &params() const { return m_p; }
    // Alpha8, target-sized: a pixel selection limiting the stroke (null = none)
    void setClip(const QImage &clip) { m_clip = clip; }
    // dabs from a to b (a itself only on the first call); returns the target area that changed
    QRect segment(QPointF a, QPointF b);
    // the same with pen samples at both ends, interpolated for every dab in between
    QRect segment(QPointF a, QPointF b, const tablet::Sample &sa, const tablet::Sample &sb);
    // the target grew: its old pixel (0,0) is now at `shift`, its size is `size`
    void grow(QPoint shift, QSize size);
    // Pre-size cap buffers so later grows at zoom < 1 stay in-place on the UI thread.
    void reserveCapacity(QSize cap);
    // True painted radius in target pixels (dab extent + 2 px margin).
    double paintRadius() const;
    // Area a segment from a to b (plus the path so far) can paint, in target pixels.
    QRectF paintedBounds(QPointF a, QPointF b) const;
    // Painted bounds of the path accumulated so far (empty if none).
    QRectF pendingPaintedBounds() const;
    // recomposite `r` of the target from the snapshot and the mask
    void composite(QImage &target, const QRect &r);
    // Full engine paint once at stroke end. Returns the union dirty rect (empty if unused).
    QRect finish();
    // When set, path-engine segment() only extends the path; call flushEngine()
    // once per frame to paint the accumulated points together.
    void setEngineDeferred(bool on) { m_engineDeferred = on; }
    bool engineDeferred() const { return m_engineDeferred; }
    bool usesEngine() const { return m_useEngine; }
    int enginePathCount() const { return m_enginePath.elementCount(); }
    const QPainterPath &enginePath() const { return m_enginePath; }
    QSize engineSize() const { return QSize(m_w, m_h); }
    PathSnapshot capturePathSnapshot(int fromIndex = 0, quint64 gen = 0) const;
    // Install a previously rendered engine layer (worker result) as the current
    // paint, without rasterizing again. Size must match the current target.
    QRect adoptEngineLayer(const QImage &layer);
    // Write only the stroke (engine paint or colour×mask) into `layer` over `r`.
    // `replace` uses Source (dab mask is authoritative); otherwise SourceOver (suffix).
    void copyStroke(QImage &layer, const QRect &r, bool replace) const;
    QRect flushEngine();
    // Repaint the accumulated path in one full pass (same samples). For live-vs-final tests.
    QRect repaintEngineFull();
    // Spray Can airbrush: accumulate paint while the pointer is held still.
    QRect dwell();
    // Override the path-engine seed (default is a hash of the stroke key).
    void setEngineSeed(quint32 seed) { m_engineSeed = seed; }
    // Edit Stroke preview / option-matrix tests: synthesise a left-to-right
    // pressure and speed ramp so Sensitivity sliders are visible.
    void setPreviewSense(bool on) { m_previewSense = on; }
    // Paint a complete path (cubic preview curve, or a pre-built path). Uses the
    // dab engine or stroke::paint according to the tip, same as a live stroke.
    QRect strokePath(const QPainterPath &path);
    QRect engineDirty() const { return m_engineDirty; }
    float maskAt(int x, int y) const { return m_mask[y * m_w + x]; }
private:
    friend void fillMask(QImage &, const QImage &, const QColor &, int, BlendMode, const QString &, int, bool);
    void dab(QPointF c, QRect *dirty, const tablet::Sample &s);
    void pencilLine(QPointF a, QPointF b, QRect *dirty, int n);
    void paintEngine(bool full = true);
    double sizeAt(const tablet::Sample &s) const;
    double dabStep(const tablet::Sample &s) const;
    QImage m_base, m_clip, m_tex;
    QVector<float> m_mask;
    int m_w = 0, m_h = 0;
    int m_capW = 0, m_capH = 0;   // allocated size of m_base / m_enginePaint (>= m_w, m_h)
    Params m_p;
    bool m_first = true;
    double m_left = 0;               // distance to the next dab
    bool m_useEngine = false;        // shared stroke::paint path (vector + bitmap)
    QPainterPath m_enginePath;
    QImage m_enginePaint;
    QRect m_engineDirty, m_engineLastDirty;
    quint32 m_engineSeed = 1;
    double m_engineSpeed = 1;
    double m_enginePressure = 1;
    QVector<double> m_engineDist, m_enginePressures, m_engineSpeeds, m_engineTimes;
    QElapsedTimer m_engineClock;
    double m_enginePathLen = 0;
    QPointF m_engineLast;
    bool m_engineHasLast = false;  // last is the last painted point, not (0,0)-ambiguous
    int m_enginePaintedN = 0;     // engine path vertices already stamped (additive spray)
    bool m_engineFinalized = false;
    QRectF m_enginePathBounds;     // control-point bounds of m_enginePath (no copy to append)
    int m_cellCs = 0;              // locked Cellular cell size for this stroke (0 = unset)
    bool m_engineDeferred = false; // Canvas coalesces path-engine paints to one per frame
    bool m_enginePending = false;
    bool m_engineLastWasFull = false;
    bool m_previewSense = false;
    struct EngineCkpt { int index = 0; QRect rect; QImage tile; };
    QVector<EngineCkpt> m_ckpts;
    void takeEngineCheckpoint();
    void clearEngineCheckpoints() { m_ckpts.clear(); }
};

// One-off fill of a coverage mask (Alpha8, the image's size) -- the Paint Bucket.
void fillMask(QImage &img, const QImage &mask, const QColor &c, int opacity, BlendMode blend, const QString &texture,
              int textureAmount, bool preserveTransparency = false);

// Edit Stroke preview: paint the standard S-curve with the same engine the Brush
// uses for this tip (dab or path), on a transparent buffer, then scale to fit.
QImage renderPreviewRaw(const Params &bp, const QSize &dest, quint32 seed = 0);
QImage renderPreview(const Params &bp, const QSize &dest, const QColor &background = QColor(250, 250, 248), quint32 seed = 0);
}
