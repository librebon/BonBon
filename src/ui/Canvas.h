#pragma once
#include "AppState.h"
#include "core/Document.h"
#include <QHash>
#include <QTimer>
#include <QElapsedTimer>
#include <QPainterPath>
#include "core/Retouch.h"
#include "core/FxExtra.h"
#include "core/BrushEngine.h"
#include <QWidget>
#include <QFutureWatcher>
#include <atomic>
#include <memory>
class QFrame;
class QDoubleSpinBox;

// Next step of the Fireworks zoom ladder (6.25% .. 6400%).
double nextZoomLevel(double z, bool in);

// Qt compresses high-frequency mouse and tablet moves by default. MouseMove
// compression stays on during a stroke so a pointer flood cannot stall the
// event loop; only tablet-event compression is turned off so every tablet
// sample is kept (paintinput::begin / end).
namespace paintinput {
void begin();
void end();
}

// The editing surface: zoom/pan, overlays and all tool interaction.
class Canvas : public QWidget {
    Q_OBJECT
public:
    // fill handle Shift-drag: the uniform scale + turn about c that takes `from` to `to`
    static QTransform gradSimilarity(const QPointF &c, const QPointF &from, const QPointF &to);
    explicit Canvas(Document *doc, QWidget *parent = nullptr);
    ~Canvas() override;
    Document *doc() const { return m_doc; }
    // Content-Aware Remove runs off the GUI thread; true while a fill is being computed.
    bool contentAwareBusy() const { return m_caJob != nullptr; }
    void cancelContentAware();   // drop a pending fill (its result is discarded)

    double zoom() const { return m_zoom; }
    void setZoom(double z, QPointF anchor = QPointF(-1, -1));
    void zoomIn();
    void zoomOut();
    // Fit All: the document fills the view exactly on the limiting axis (plus the fit-margin pref).
    // zoomIn=false caps at 100% (the automatic fit when a document opens).
    void fitToWindow(bool zoomIn = true);
    void zoomToRect(const QRectF &docRect);   // fit docRect (with a margin) and centre it
    void actualSize();
    void setView(double z, QPointF docCenter); // zoom with docCenter in the middle of the view
    // Place the canvas flush with the view origin (no margin). Does not clamp.
    void pinToOrigin();
    QPointF toDoc(const QPointF &w) const { return (w - m_origin) / m_zoom; }
    QPointF toView(const QPointF &d) const { return d * m_zoom + m_origin; }
    void panBy(const QPointF &d) { m_needFit = false; m_origin += d; clampOrigin(); update(); emit viewChanged(); }
    QRectF visibleDocRect() const { return QRectF(toDoc(QPointF(0, 0)), toDoc(QPointF(width(), height()))).normalized(); }
    QPointF docCenter() const { return toDoc(QPointF(width() / 2.0, height() / 2.0)); }

    bool cropActive() const { return m_cropActive; }
    void applyCrop();
    void cancelCrop();
    void publishCrop();
    // Export Area tool (Crop flyout)
    bool exportAreaActive() const { return m_areaActive; }
    QRect exportArea() const { return m_area; }
    void setExportArea(const QRect &r);     // show / move the area (document px); empty hides it
    void requestAreaExport();               // Enter / double-click inside: Export Preview of the area
    void cancelExportArea();                // Esc: forget the area
    void publishExportArea();
    void finishPen(bool close = false);
    void cancelPen();
    void nudge(QPointF d);
    void editText(VObject *o);
    const QImage &renderedFrame();
    // Document frame with the mid-stroke live layer composited (tests / sampling).
    // Does not mutate the document cache; paintEvent still blits the live layer itself.
    QImage renderedFrameWithLive();
    // Drop the cached frame so the next paint re-renders (e.g. Preferences interpolation).
    void invalidateFrameCache() { m_cacheDirty = true; m_cacheDirtyRect = QRect(); m_viewCacheOk = false; update(); }
    QRegion antsRegion() const;   // view pixels the marching ants touch; the ants timer repaints only these

    // ---- grid and guides (per document)
    // Snap a document point to the grid / guides when those snaps are on (within 8 screen px;
    // a 1 px grid therefore snaps every point to whole pixels).
    // `smart` adds the canvas edges / centre lines and other objects' edges / centres (Preferences
    // snapToCanvas / snapToObjects, 6 screen px at any zoom); `line` receives the smart target hit
    // (NaN when none). Snapping is suspended while Ctrl is held during a drag.
    QPointF snapPoint(QPointF d, bool smart = false) const;
    double snapCoord(double v, bool xAxis, double *moved = nullptr, bool smart = false, double *line = nullptr) const;
    static constexpr double kSmartSnapPx = 6.0;
    static constexpr Qt::KeyboardModifier kNoSnapModifier = Qt::ControlModifier;
    // the smart guide lines currently shown (document x of vertical lines, y of horizontal ones)
    const QVector<double> &snapLinesX() const { return m_snapX; }
    // a guide dragged out of a ruler (vertical = from the left ruler); positions are global
    void beginGuideDrag(bool vertical);
    void guideDragTo(QPoint globalPos);
    void endGuideDrag(QPoint globalPos);
    bool guideDragActive() const { return m_guideDrag; }

    // Tablet (v0.3.1): the pen sample the current event carries (pressure through the Preferences curve);
    // a mouse event reads as pressure 1.0 with no tilt. eraserEndActive: the pen's eraser end has switched
    // the tool to the Eraser (the tip switches back).
    tablet::Sample penSample() const {
        if (m_flushPenValid) return m_flushPen;
        return m_fromTablet ? m_pen : tablet::Sample();
    }
    bool eraserEndActive() const { return m_eraserSwitched; }
    static tablet::Sample sampleFromEvent(const class QTabletEvent *e);   // raw -> curve-mapped sample
signals:
    void zoomChanged(double z);
    void cursorMoved(QPointF docPos);
    void viewChanged();
    void openDocumentRequested(const QString &path);
    void editSymbolRequested(quint64 symbolId);
    // right click: the object under the pointer is selected first (MainWindow shows the menu)
    void contextMenuRequested(QPoint globalPos);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void tabletEvent(QTabletEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *) override;
    void leaveEvent(QEvent *) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

public:
    void updateCursor();
    // Path Scrubber: true while its cursor shows "-" (Decrease mode, or Increase with Alt held)
    bool scrubCursorMinus() const;
    // View-space rect of the painted brush-size circle (and its previous
    // position). Dirty widget updates must union both, or the circle leaves a
    // trail. The circle is an overlay, never stored in m_viewCache.
    QRect brushCursorViewRect() const;
    void revealBrushCursor();

    // ---- Free Transform (Scale / Skew / Distort / Perspective tools)
    // The transform box: four corners (clockwise from the top left, document coordinates), the
    // centre of rotation (draggable) and the angle the selection has been rotated by. The box
    // follows the selection; it is rebuilt when the selection or the document changes elsewhere.
    bool transformActive();
    QPolygonF transformQuad();
    QPointF transformCentre();
    void setTransformCentre(const QPointF &c);
    double transformAngle();
    // rotate the selection about the transform centre (clockwise degrees), one undo step
    bool rotateSelection(double degrees);
    // handle positions in view coordinates: 0-3 corners, 4-7 edge midpoints (top, right, bottom,
    // left), 8 the centre
    QPointF transformHandleView(int i);
    static double snapAngle(double degrees, double step = 15.0);

private:
    enum class Drag { None, Guide, Pan, Move, Resize, RubberBand, Create, Paint, PixelRect, Lasso, Crop, CropMove, Area, AreaMove, AreaHandle, PointEdit, ZoomRect, PenHandle, HandleEdit, Knife, Sculpt, Scrub, GradHandle, GradTool, SelMove, Transform, ShapeHandle, ContentAware, VectorPath, RedrawPath };
    Tool effectiveTool() const;
    quint64 hitObject(const QPointF &docPt, bool editableOnly) const;
    // Top-first stack of objects under a point (used by Select Behind).
    QVector<quint64> hitStack(const QPointF &docPt, bool editableOnly) const;
    // v0.3.2 groups: the ids from the layer-level object down to the innermost object under the point
    QVector<quint64> hitChain(const QPointF &docPt, bool editableOnly) const;
    // what a click picks: the Pointer the object at the current group level (the layer level, or a
    // member of the entered group), the Subselection tool the innermost object. *enterOut: the group
    // the click works in afterwards (0 = layer level)
    quint64 pickObject(const QPointF &docPt, bool subselect, quint64 *enterOut = nullptr) const;
    int handleAt(const QPointF &viewPt) const;
    QVector<QRectF> handleRects(const QRectF &docBounds) const;
    QRectF selectionViewRect();
    void beginPaint(const QPointF &d);
    void paintTo(const QPointF &d);
    void queuePaintPoint(const QPointF &d);
    bool queuedPaintNeedsGrow() const;
    bool queuedPaintNeedsPresent() const;
    void flushPaintFrame(bool drain = false);   // (A)(B): time-boxed ~8 ms of work at ~60 Hz; drain on release
    struct QueuedPaint { QPointF doc; tablet::Sample pen; };
    void flushPaintEngine();
    void endPaint();
    void retouchDab(const QPointF &imagePt, const tablet::Sample &pen = tablet::Sample());
    tablet::Sample m_pen, m_lastPen;     // the tablet sample of the event being handled / at m_lastDoc
    bool m_fromTablet = false;           // the mouse handlers are running for a tablet event
    bool m_eraserSwitched = false;
    Tool m_eraserRestore = Tool::Pointer;
    QPointF toImage(const VObject &o, const QPointF &d) const;
    void status(const QString &m) { emit appState().statusMessage(m); }
    void clampOrigin();
    void drawGrid(QPainter &p);
    void drawGuides(QPainter &p, const QRectF &canvasView);
    bool guideAt(const QPointF &viewPt, bool *vertical, int *index) const;
    bool snapsAtPress(Tool t) const;
    void finishGuide(bool keep);
    bool m_guideDrag = false;    // a guide is being dragged (from a ruler or on the canvas)
    bool m_guideVert = false;
    int m_guideIndex = -1;       // existing guide being moved, -1 = new
    double m_guidePos = 0;
    QPointF m_moveApplied;       // Pointer move: total delta applied so far
    // Free Transform session
    struct TxSession { bool valid = false; QSet<quint64> ids; quint64 rev = 0; QPolygonF quad; QPointF centre; double angle = 0; };
    TxSession m_tx;
    quint64 m_docRev = 0;        // bumps on every document change (rebuilds the transform box)
    bool m_txApplying = false;
    int m_txHandle = -1;         // 0-3 corners, 4-7 edges, 8 centre, 9 rotate
    QPolygonF m_txQuad0;
    QPointF m_txCentre0;
    double m_txAngle0 = 0;
    bool ensureTransform();
    int transformHandleAt(const QPointF &viewPt, bool *rotateZone);
    void transformTo(const QPointF &d, Qt::KeyboardModifiers mods);
    void applyTransform(const QTransform &t);
    void updateTransformBar();
    QFrame *m_txBar = nullptr;
    QDoubleSpinBox *m_txAngleBox = nullptr;

    Document *m_doc;
    double m_zoom = 1.0;
    QPointF m_origin{20, 20};
    bool m_needFit = true;
    Tool m_penTool = Tool::Pen; // Pen or Polygon Hotspot (shares the click-to-add-points UI)
    QImage m_cache;
    bool m_cacheDirty = true;
    QRect m_cacheDirtyRect;        // P4: region of m_cache to re-render (when !m_cacheDirty)
    // Widget-space copy of the pasteboard + canvas image (no overlays). Updated
    // only in the dirty document region so a paintEvent can blit just e->rect().
    QImage m_viewCache;
    bool m_viewCacheOk = false;
    QRect m_viewCacheDocDirty;
    double m_viewCacheZoom = -1;
    QPointF m_viewCacheOrigin;
    QSize m_viewCacheWidget;
    void onDocChanged();
    void renderDirtyRect(const QRect &r);
    QRect viewRectFromDoc(const QRectF &doc, int padPx = 3) const;
    void refreshViewCache();
    void blitDocToViewCache(const QRect &docDirty);
    void paintCanvasBackdrop(QPainter &p, const QRect &wr);
public:
    int partialRenders() const { return m_partialRenders; }   // tests
    // Live stroke layer (image space of the paint target). For mid-vs-final tests.
    QImage liveStrokeImage() const { return m_liveLayer; }
    QRectF liveStrokeDocRect() const;
    // Block until an in-flight full-path live job is applied (tests).
    void waitLiveStroke();
private:
    int m_partialRenders = 0;

    Drag m_drag = Drag::None;
    Qt::KeyboardModifiers m_mods;
    // smart snapping during a drag
    QPointF snapRectDelta(const QRectF &r, bool shiftAxisX, bool shiftAxisY);   // adjustment that snaps r's edges/centre
    void smartTargets() const;
    mutable QVector<double> m_tgtX, m_tgtY;   // cached for the current drag
    mutable bool m_tgtValid = false;
    mutable QVector<double> m_snapX, m_snapY;
    bool m_noSnap = false;
    QPointF m_pressDoc, m_lastDoc, m_hoverDoc;
    QRect m_lastCursorView;
    // (A) Motion events only enqueue points; a 16 ms timer paints them in one
    // frame so a 1000 Hz flood cannot run paintTo/grow on every event.
    QTimer *m_paintFrame = nullptr;
    QVector<QueuedPaint> m_paintQueued;
    tablet::Sample m_flushPen;
    bool m_flushPenValid = false;
    bool m_inPaintFrame = false;
    QElapsedTimer m_paintFlushClock;
    QRectF m_paintFrameDirty;
    bool m_paintFrameFull = false; // non-partial notify during the frame
    // Live overlay is only for Normal, non-erase, non-preserve-transparency
    // ink: SourceOver of the stroke layer onto the untouched base. Eraser,
    // blend modes, preserve-transparency and retouch write the document and
    // present through the dirty-rect cache (applyPaintFrameDirty).
    bool m_liveStrokeOverlay = false;
    void applyDocDirty(const QRectF &r, bool full);
    void applyPaintFrameDirty();
    bool strokeUsesLiveOverlay() const;
    // Live stroke layer: premultiplied, paint-target image space, above the
    // untouched base bitmap. Composited after the view-cache blit.
    QImage m_liveLayer;       // presentation: last full paint + suffix (view of m_liveStore)
    QImage m_liveStore;       // capacity buffer for m_liveLayer, grown in 256 px chunks
    QImage m_liveBase;        // last completed worker full-path result
    int m_liveCkptN = 0;      // path vertices in m_liveBase
    int m_liveSuffixAt = 0;   // vertices already painted onto m_liveLayer
    QRect m_liveDirty;        // image-space dirty of the last present
    quint64 m_liveGen = 0;    // bumps on a left/up grow (stale-job token)
    bool m_liveHasInk = false; // actual paint on the live layer (not a no-op clip)
    struct LiveJobResult {
        QImage layer;
        int elementCount = 0;
        QSize size;
        quint64 gen = 0;
    };
    QFutureWatcher<LiveJobResult> *m_liveJob = nullptr;
    bool m_liveJobWanted = false;
    bool m_applyingLiveJob = false;
    void presentLiveFrame();
    void growLiveLayer(QPoint shift, QSize size);
    void startLiveJob();
    void applyLiveJob();
    void clearLiveStroke();
    QRect liveStrokeViewRect() const;
    QPointF m_pressView;
    bool m_moved = false;
    int m_handle = -1;
    QRectF m_startBounds;
    QHash<quint64, VObject> m_startObjs;
    quint64 m_creatingId = 0;
    quint64 m_paintTarget = 0;
    bool m_paintNew = false;
    bool m_paintGrow = false;    // the target bitmap grows to take the stroke (Stroke target: new / expand)
    bool m_paintClipped = false; // clip mode: part of the stroke fell outside the bitmap
    QRectF m_paintOrigRect;      // target bounds when the stroke started (kept when trimming)
    QPoint growPaintTarget(VObject *o, const QRectF &docArea);   // returns where the old pixel (0,0) went
    // Grow the paint target to cover imageBounds (target pixels) before a segment
    // is painted. True if the bitmap size or origin changed.
    bool ensurePaintTargetFor(const QRectF &imageBounds);
    void detachPaintStore(VObject *o);
    QImage m_paintStore;            // reusable premultiplied buffer for live growth
    bool m_paintStoreBound = false; // o->image currently wraps m_paintStore
    bool m_paintGrewOnce = false;   // first growth of this stroke uses a tighter pad
    QRectF m_paintViewExtra;        // grown object union, included in the next notify
    void beginBrushStroke(const QPointF &d);
    QImage paintClip(const VObject &o) const;
public:
    static QPolygonF polygonPoints(QPointF c, double r, double angleDeg, int sides, bool star, double pointAngle);
private:
    brush::Stroke m_brush;
    // Pencil "Pixel perfect": the stroke is re-plotted on release without L-corners
    bool m_pp = false;
    QImage m_ppBase;
    QRectF m_ppBaseRect;
    QVector<QPointF> m_ppPts;
    QColor m_ppColor;
    bool m_ppErase = false;          // the pixel-perfect stroke erases (a transparent Pencil colour)
    // Pencil "Left/Right colours": the button painting the current stroke, and the end of the last
    // Pencil stroke (Shift-click draws a straight line from it)
    Qt::MouseButton m_paintButton = Qt::LeftButton;
    QPointF m_pencilLineEnd;
    bool m_pencilLineValid = false;
    bool pencilTwoColorActive() const;
    void finishPixelPerfect(VObject *o);
    QSize m_brushSize0;
    bool m_paintFlushScheduled = false;
    QTimer *m_sprayDwell = nullptr;
    void startSprayDwell();
    void stopSprayDwell();
    // retouch tools
    QImage m_retouchClip;        // pixel selection in image coordinates (Alpha8) or null
    QImage m_stampSnap;          // Rubber Stamp source pixels (snapshot at stroke start)
    QPointF m_stampSrc;          // Rubber Stamp source point (document coordinates)
    QPointF m_stampOffset;       // source - destination (document coordinates)
    bool m_stampSet = false, m_stampOffsetSet = false;
    retouch::Smudger m_smudger;
    fx2::RedEyeStroke m_reStroke;         // Red Eye tool: the same, for the red-eye correction
    fx2::ReplaceColorStroke m_rcStroke;   // Replace Color tool: the stroke's original pixels and coverage
public:
    QPointF stampSource() const { return m_stampSrc; }
private:
    QPolygonF m_lasso;
    // Oval Marquee / modifier state of a marquee drag (Shift/Alt held at press combine with the
    // selection; pressed again during the drag they constrain / draw from the centre)
    bool m_selOval = false, m_selShiftFree = false, m_selAltFree = false;
    // dragging a bitmap selection: the outline alone, or (Ctrl / Pointer) the pixels under it
    bool beginSelectionMove(const QPointF &d, bool pixels, bool duplicate);
    void selectionMoveTo(const QPointF &d);
    bool m_noSelMove = false;           // replaying a click inside the selection as a normal press
    bool m_spaceRepos = false;          // Space held while drawing a marquee: move it instead
    QPointF m_spaceLast;
    QPointF m_selMoveTotal;             // pointer travel since the press
    QPoint m_selMoveApplied;            // whole-pixel offset applied so far
    quint64 m_floatId = 0;              // bitmap whose pixels are being moved (0: outline only)
    QImage m_floatHole, m_floatPixels;  // source with the selection lifted out / the lifted pixels
    QRect m_floatRect, m_floatSel;
    // Polygon Lasso: points clicked so far (empty = not drawing)
    QPolygonF m_poly;
    Qt::KeyboardModifiers m_polyMods;
    void finishPolyLasso();
    void cancelPolyLasso();
    QPointF polyConstrain(QPointF d, bool shift) const;
    void commitPixelSelection(const QPainterPath &p, Qt::KeyboardModifiers mods, Tool tool);
    QImage stampSource(const VObject &o);
public:
    bool polyLassoActive() const { return !m_poly.isEmpty(); }
    int polyLassoPoints() const { return m_poly.size(); }
    QVector<quint64> objectsUnder(const QPointF &docPt, bool editableOnly = false) const { return hitStack(docPt, editableOnly); }
private:
    QRectF m_rubber;
    QRectF m_crop;
    bool m_cropActive = false;
    QRect m_area;                           // Export Area (document px, whole pixels)
    bool m_areaActive = false;
    int m_areaHandle = -1;                  // AreaHandle: 0 TL 1 TR 2 BL 3 BR 4 T 5 B 6 L 7 R
    QRect m_areaStart;                      // the area at press (move / resize)
    QPoint areaSnap(const QPointF &d, bool edges = true) const;   // whole pixels + the canvas edges
    int areaHandleAt(const QPointF &viewPt) const;
    QPolygonF m_penPts, m_penIn, m_penOut; // Pen anchors and their (absolute) Bezier handles
    QPolygonF m_freehand;                   // Vector Path / Redraw Path stroke in progress
    QVector<double> m_freehandPress, m_freehandSpeed;
    quint64 m_redrawId = 0;
    QPointF m_behindLastDoc;
    quint64 m_behindLastId = 0;
    bool m_behindCycling = false;
    void finishVectorPath();
    void finishRedrawPath();
    int m_handleSide = 0;                   // HandleEdit: 0 = in handle, 1 = out handle
    bool m_handleMirror = false;
    bool m_handleFresh = false;             // handles being pulled out of a corner (Alt-drag): always symmetric
    int m_gradHandle = -1;                  // gradient handle being dragged (0 start, 1 end, 2 second axis)
    QPointF m_gradStart[3];                 // handle positions (doc) at press
    bool m_gradConverted = false;           // Gradient tool turned a solid fill into a gradient
    int gradHandleAt(const QPointF &viewPt) const;
    // Auto Shapes: the selected auto shape whose yellow diamonds are shown, and the diamond under a view point
    VObject *autoShapeTarget() const;
    int shapeHandleAt(const QPointF &viewPt) const;
    int m_shapeHandle = -1;
    // Content-Aware Remove tool: the painted mask (canvas px, Alpha8) and the target bitmap
    QImage m_caMask;
    QPainterPath m_caPath;
    void finishContentAware();
    // the fill in progress: the worker gets copies of the image and mask; the result is applied
    // only if the target still exists with the very image the fill started from
    QFutureWatcher<QImage> *m_caJob = nullptr;
    std::shared_ptr<std::atomic_bool> m_caCancel;
    quint64 m_caTarget = 0;
    qint64 m_caSrcKey = 0;
    QPainterPath m_caPending;   // shown while the fill runs
    void applyContentAware();
    VObject *gradientTarget() const;        // single selected object with a gradient fill
    bool m_sculptPull = false;              // Freeform: pulling the outline (else pushing)
    // Path Scrubber drag: paths already subdivided for this drag, distance since the last dab
    QSet<quint64> m_scrubPrepared;
    double m_scrubCarry = 0;
    bool m_scrubChanged = false;
    bool m_altHeld = false;                 // Alt flips the Path Scrubber's mode (and its cursor)
    QVector<VObject *> scrubTargets() const;
    void scrubDab(const QPointF &docPt, bool alt);
    quint64 m_hoverId = 0;
    int m_pointIndex = -1;
    bool m_space = false;
    QTimer m_ants;
    int m_antsPhase = 0;
    // selection outlines (P9): per-object view-space outline and its 3 px halo stroke, rebuilt
    // only when the selection, the geometry or the view transform changes
    struct SelOutline { bool box; QRectF vb; QPainterPath vp; QPainterPath halo; };
    QVector<SelOutline> m_selOutlines;
    QVector<quint64> m_selOutlineIds;
    QTransform m_selOutlineXf;
    bool m_selOutlinesValid = false;
    const QVector<SelOutline> &selectionOutlines(const QVector<VObject *> &sel, const QTransform &t);
public:
    int selectionOutlineBuilds() const { return m_selOutlineBuilds; }   // tests
private:
    int m_selOutlineBuilds = 0;
};
