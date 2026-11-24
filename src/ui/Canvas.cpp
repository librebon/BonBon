#include "PopupSlider.h"
#include "Canvas.h"
#include "PaintTrace.h"
#include "Brand.h"
#include "Shortcuts.h"
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include "io/ImageIO.h"
#include <QTabletEvent>
#include "core/AutoShape.h"
#include "core/FxExtra.h"
#include "ViewGeometry.h"
#include <cmath>
#include <cstring>
#include "core/Fills.h"
#include "core/Stroke.h"
#include "core/Effects.h"
#include "core/Filters.h"
#include "core/Render.h"
#include "core/Transform.h"
#include "core/PixelEdit.h"
#include "core/Paths.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QSet>
#include <QHash>
#include <QInputDialog>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QPainter>
#include <QGuiApplication>
#include <QPainterPathStroker>
#include <QtMath>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace paintinput {
namespace {
int g_depth = 0;
bool g_savedTab = true;
}
void begin()
{
    if (g_depth++ == 0) {
        // Keep MouseMove compression on so a high-rate pointer flood cannot
        // stall the event loop. Tablet samples stay uncompressed.
        g_savedTab = QCoreApplication::testAttribute(Qt::AA_CompressTabletEvents);
        QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents, false);
    }
}
void end()
{
    if (g_depth > 0 && --g_depth == 0)
        QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents, g_savedTab);
}
}

static bool isRetouch(Tool t) {
    return t == Tool::Blur || t == Tool::Sharpen || t == Tool::Dodge || t == Tool::Burn || t == Tool::Smudge || t == Tool::RubberStamp
           || t == Tool::Healing || t == Tool::SpotHealing || t == Tool::ReplaceColor || t == Tool::RedEye;
}

static const double kZoomLevels[] = {0.0625, 0.125, 0.25, 0.333, 0.5, 0.666, 1, 1.5, 2, 3, 4, 6, 8, 12, 16, 32, 64};

Canvas::Canvas(Document *doc, QWidget *parent) : QWidget(parent), m_doc(doc) {
    setAcceptDrops(true);
    setMouseTracking(true);
    setAttribute(Qt::WA_TabletTracking);   // pen hover: the eraser end switches tools before it touches
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 150);
    m_sprayDwell = new QTimer(this);
    m_sprayDwell->setInterval(33);
    connect(m_sprayDwell, &QTimer::timeout, this, [this] {
        if (!m_brush.active()) { stopSprayDwell(); return; }
        m_brush.dwell();
        if (!m_paintFlushScheduled) {
            m_paintFlushScheduled = true;
            QTimer::singleShot(0, this, [this]() { flushPaintEngine(); });
        }
    });
    // (A) Paint at ~60 Hz, never per motion event. Events only enqueue points.
    m_paintFrame = new QTimer(this);
    m_paintFrame->setInterval(16);
    m_paintFrame->setTimerType(Qt::PreciseTimer);
    connect(m_paintFrame, &QTimer::timeout, this, [this] { flushPaintFrame(); });
    m_liveJob = new QFutureWatcher<LiveJobResult>(this);
    connect(m_liveJob, &QFutureWatcher<LiveJobResult>::finished, this, [this] { applyLiveJob(); });
    auto dirty = [this] { m_cacheDirty = true; m_selOutlinesValid = false; if (!m_txApplying) ++m_docRev; update(); };
    connect(doc, &Document::changed, this, &Canvas::onDocChanged);
    connect(doc, &Document::frameChanged, this, dirty);
    connect(doc, &Document::selectionChanged, this, [this] { m_selOutlinesValid = false; update(); });
    connect(doc, &Document::structureChanged, this, [this] { m_selOutlinesValid = false; });
    connect(doc, &Document::historyChanged, this, [this] { m_selOutlinesValid = false; });
    connect(doc, &Document::viewSettingsChanged, this, qOverload<>(&QWidget::update));
    connect(&appState(), &AppState::toolChanged, this, [this](Tool) {
        if (m_cropActive && appState().tool() != Tool::Crop) cancelCrop();
        if (!m_penPts.isEmpty() && appState().tool() != m_penTool) finishPen();
        if (!m_poly.isEmpty() && appState().tool() != Tool::PolyLasso) cancelPolyLasso();
        if (appState().tool() == Tool::Crop && isVisible()) publishCrop();
        // Export Area: hidden while another tool is active, back (from the session memory) when re-selected
        if (m_areaActive && appState().tool() != Tool::ExportArea) { m_areaActive = false; if (isVisible()) publishExportArea(); }
        if (appState().tool() == Tool::ExportArea && isVisible()) {
            if (!m_areaActive && !m_doc->exportArea.isEmpty()) { m_area = m_doc->exportArea; m_areaActive = true; }
            publishExportArea();
        }
        m_tx.valid = false;
        updateTransformBar();
        updateCursor();
        update();
    });
    connect(&appState(), &AppState::optionsChanged, this, qOverload<>(&QWidget::update));
    connect(&appState(), &AppState::optionsChanged, this, [this] { if (effectiveTool() == Tool::PathScrubber) updateCursor(); });
    // Free Transform: exact angle field shown over the canvas while a transform tool is active
    m_txBar = new QFrame(this);
    m_txBar->setObjectName(QStringLiteral("transformBar"));
    m_txBar->setFrameShape(QFrame::StyledPanel);
    m_txBar->setAutoFillBackground(true);
    {
        auto *h = new QHBoxLayout(m_txBar);
        h->setContentsMargins(6, 3, 6, 3);
        h->setSpacing(4);
        auto *lbl = new QLabel(QStringLiteral("Rotate:"), m_txBar);
        m_txAngleBox = new PopupSliderDoubleField(m_txBar);
        m_txAngleBox->setObjectName(QStringLiteral("txAngle"));
        m_txAngleBox->setRange(-360, 360);
        m_txAngleBox->setDecimals(1);
        m_txAngleBox->setSuffix(QStringLiteral("\u00B0"));
        m_txAngleBox->setKeyboardTracking(false);
        m_txAngleBox->setToolTip(QStringLiteral("Exact rotation (degrees, clockwise) about the centre point"));
        lbl->setBuddy(m_txAngleBox);
        auto *reset = new QToolButton(m_txBar);
        reset->setObjectName(QStringLiteral("txCentreReset"));
        reset->setText(QStringLiteral("Centre"));
        reset->setToolTip(QStringLiteral("Move the centre of rotation back to the middle of the box"));
        h->addWidget(lbl);
        h->addWidget(m_txAngleBox);
        h->addWidget(reset);
        connect(m_txAngleBox, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (!ensureTransform()) return;
            if (qAbs(v - m_tx.angle) > 1e-9) rotateSelection(v - m_tx.angle);
            setFocus();
        });
        connect(reset, &QToolButton::clicked, this, [this] {
            if (!ensureTransform()) return;
            QPointF c;
            for (const QPointF &q : m_tx.quad) c += q / 4.0;
            setTransformCentre(c);
        });
    }
    m_txBar->hide();
    connect(doc, &Document::selectionChanged, this, [this] { updateTransformBar(); });
    connect(doc, &Document::changed, this, [this] { if (!m_txApplying) updateTransformBar(); });
    // Crop fields / buttons in the Property Inspector act on the visible canvas
    connect(&appState(), &AppState::cropEdited, this, [this](const QRectF &r) {
        if (!isVisible() || appState().tool() != Tool::Crop) return;
        m_cropActive = true;
        m_crop = r.normalized();
        publishCrop();
        update();
    });
    connect(&appState(), &AppState::cropCommand, this, [this](bool apply) {
        if (!isVisible() || !m_cropActive) return;
        if (apply) applyCrop(); else cancelCrop();
    });
    // Export Area fields / buttons in the Property Inspector
    connect(&appState(), &AppState::exportAreaEdited, this, [this](const QRect &r) {
        if (!isVisible() || appState().tool() != Tool::ExportArea) return;
        setExportArea(r);
    });
    connect(&appState(), &AppState::exportAreaCommand, this, [this](bool doExport) {
        if (!isVisible() || !m_areaActive) return;
        if (doExport) requestAreaExport(); else cancelExportArea();
    });
    m_ants.setInterval(160);
    connect(&m_ants, &QTimer::timeout, this, [this] {
        const bool antsVisible = !m_doc->pixelSelection.isEmpty() || m_cropActive || m_areaActive || m_drag == Drag::RubberBand
                                 || m_drag == Drag::PixelRect || m_drag == Drag::ZoomRect || m_drag == Drag::Lasso || m_drag == Drag::Knife || !m_poly.isEmpty();
        if (!antsVisible) return;
        m_antsPhase = (m_antsPhase + 1) % 8;
        update(antsRegion());   // only the dashed outlines move (P10)
    });
    m_ants.start();
    updateCursor();
}

// The view pixels the marching ants can touch: the outline strips of rectangles, the bounds of
// other shapes, padded for the antialiased 1 px pens. paintEvent draws exactly as for a full update.
QRegion Canvas::antsRegion() const {
    const QTransform t = QTransform::fromScale(m_zoom, m_zoom) * QTransform::fromTranslate(m_origin.x(), m_origin.y());
    QRegion r;
    auto box = [&](const QRectF &v) {
        r += QRect(QPoint(int(std::floor(v.left())) - 4, int(std::floor(v.top())) - 4), QPoint(int(std::ceil(v.right())) + 4, int(std::ceil(v.bottom())) + 4));
    };
    auto outline = [&](const QRectF &v0) {
        const QRectF v = v0.normalized();
        if (v.width() < 24 || v.height() < 24) { box(v); return; }
        box(QRectF(v.left(), v.top(), v.width(), 0));
        box(QRectF(v.left(), v.bottom(), v.width(), 0));
        box(QRectF(v.left(), v.top(), 0, v.height()));
        box(QRectF(v.right(), v.top(), 0, v.height()));
    };
    if (!m_doc->pixelSelection.isEmpty()) {
        QPainterPath asRect;
        asRect.addRect(m_doc->pixelSelection.boundingRect());
        if (asRect == m_doc->pixelSelection) outline(t.mapRect(m_doc->pixelSelection.boundingRect()));
        else box(t.map(m_doc->pixelSelection).boundingRect());
    }
    if (m_drag == Drag::RubberBand || m_drag == Drag::PixelRect || m_drag == Drag::ZoomRect) {
        if (m_drag == Drag::PixelRect && m_selOval) box(t.mapRect(m_rubber));
        else outline(t.mapRect(m_rubber));
    }
    if ((m_drag == Drag::Lasso || m_drag == Drag::Knife) && m_lasso.size() > 1) box(t.map(m_lasso).boundingRect());
    if (!m_poly.isEmpty()) {
        QPolygonF vl = t.map(m_poly);
        vl << t.map(polyConstrain(m_hoverDoc, QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier));
        box(vl.boundingRect());
    }
    if (m_cropActive) outline(t.mapRect(m_crop));
    if (m_areaActive) outline(t.mapRect(QRectF(m_area)));
    return r & rect();
}

static bool canEditPoints(const VObject *o)
{
    if (!o) return false;
    // Existing polygon slices keep editable points; new slices from Insert > Slice
    // are rectangles and use the rectangle transform frame.
    return o->usesPoints();
}


QRect Canvas::viewRectFromDoc(const QRectF &doc, int padPx) const
{
    const QRectF v(toView(doc.topLeft()), toView(doc.bottomRight()));
    return v.normalized().toAlignedRect().adjusted(-padPx, -padPx, padPx, padPx)
        .intersected(QRect(QPoint(0, 0), size()));
}

// Pasteboard, checkerboard and the document image for `wr`. Overlays stay out
// so the view cache can be blitted and then the live handles drawn on top.
void Canvas::paintCanvasBackdrop(QPainter &p, const QRect &wr)
{
    if (wr.isEmpty()) return;
    p.setClipRect(wr);
    p.fillRect(wr, appState().prefs.pasteboardColor);
    const DocState &s = m_doc->s;
    const double dpr = devicePixelRatioF();
    const QRectF cr = snappedTarget(m_origin, QSizeF(s.width, s.height), m_zoom, dpr);
    if (s.canvasColor.alpha() < 255 && !appState().prefs.checkerboard) {
        p.fillRect(cr, Qt::white);
    } else if (s.canvasColor.alpha() < 255) {
        const int cs = 8;
        p.save();
        p.setClipRect(cr);
        p.fillRect(cr, Qt::white);
        QRect vis = cr.toAlignedRect().intersected(wr);
        const int ox = int(std::floor(cr.left())), oy = int(std::floor(cr.top()));
        for (int y = vis.top() - ((vis.top() - oy) % cs + cs) % cs; y <= vis.bottom(); y += cs)
            for (int x = vis.left() - ((vis.left() - ox) % cs + cs) % cs; x <= vis.right(); x += cs)
                if ((((x - ox) / cs) + ((y - oy) / cs)) % 2) p.fillRect(x, y, cs, cs, QColor(0xCC, 0xCC, 0xCC));
        p.restore();
    }
    const QImage &img = m_cache.isNull() ? renderedFrame() : m_cache;
    QRectF visDoc = QRectF(toDoc(QPointF(wr.left(), wr.top())),
                           toDoc(QPointF(wr.right() + 1, wr.bottom() + 1)))
                        .normalized()
                        .intersected(QRectF(0, 0, s.width, s.height));
    if (!visDoc.isEmpty() && !img.isNull()) {
        QRect src = visDoc.toAlignedRect().intersected(img.rect());
        if (!src.isEmpty()) {
            const QRectF dst(QPointF(snappedCoord(m_origin.x(), src.left(), m_zoom, dpr),
                                     snappedCoord(m_origin.y(), src.top(), m_zoom, dpr)),
                             QPointF(snappedCoord(m_origin.x(), src.left() + src.width(), m_zoom, dpr),
                                     snappedCoord(m_origin.y(), src.top() + src.height(), m_zoom, dpr)));
            p.save();
            p.setClipRect(cr);
            p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
            p.drawImage(dst, img, QRectF(src));
            p.restore();
        }
    }
}

void Canvas::blitDocToViewCache(const QRect &docDirty)
{
    if (m_viewCache.isNull() || docDirty.isEmpty()) return;
    const QRect wr = viewRectFromDoc(QRectF(docDirty), 2);
    if (wr.isEmpty()) return;
    QPainter p(&m_viewCache);
    paintCanvasBackdrop(p, wr);
}

void Canvas::refreshViewCache()
{
    if (m_viewCacheZoom != m_zoom || m_viewCacheOrigin != m_origin || m_viewCacheWidget != size())
        m_viewCacheOk = false;
    if (!m_viewCacheOk || m_viewCache.size() != size()) {
        m_viewCache = QImage(size(), QImage::Format_ARGB32_Premultiplied);
        if (m_viewCache.isNull()) return;
        QPainter p(&m_viewCache);
        paintCanvasBackdrop(p, QRect(QPoint(0, 0), size()));
        p.end();
        m_viewCacheOk = true;
        m_viewCacheZoom = m_zoom;
        m_viewCacheOrigin = m_origin;
        m_viewCacheWidget = size();
        m_viewCacheDocDirty = QRect();
        return;
    }
    if (!m_viewCacheDocDirty.isEmpty()) {
        blitDocToViewCache(m_viewCacheDocDirty);
        m_viewCacheDocDirty = QRect();
    }
}

void Canvas::applyDocDirty(const QRectF &r, bool full)
{
    if (!full && !r.isEmpty() && !m_cacheDirty && !m_cache.isNull() && m_doc->onionFrames().isEmpty()) {
        // whole pixels touched, with a margin for antialiasing
        m_cacheDirtyRect |= r.toAlignedRect().adjusted(-2, -2, 2, 2);
        m_selOutlinesValid = false;
        if (!m_txApplying) ++m_docRev;
        QRect vr = viewRectFromDoc(r, 6);
        const QRect cur = brushCursorViewRect();
        vr = vr.united(m_lastCursorView).united(cur).united(liveStrokeViewRect());
        m_lastCursorView = cur;
        if (vr.isEmpty()) update();
        else update(vr);
        return;
    }
    m_cacheDirty = true;
    m_viewCacheOk = false;
    m_selOutlinesValid = false;
    if (!m_txApplying) ++m_docRev;
    update();
}

void Canvas::applyPaintFrameDirty()
{
    const QRectF r = m_paintFrameDirty;
    const bool full = m_paintFrameFull;
    m_paintFrameDirty = QRectF();
    m_paintFrameFull = false;
    if (full)
        applyDocDirty(QRectF(), true);
    else if (!r.isEmpty())
        applyDocDirty(r, false);
}

void Canvas::onDocChanged() {
    if (m_inPaintFrame) {
        QRectF r;
        if (m_doc->partialChange(&r)) m_paintFrameDirty |= r;
        else m_paintFrameFull = true;
        return;
    }
    QRectF r;
    applyDocDirty(r, !m_doc->partialChange(&r));
}

bool Canvas::strokeUsesLiveOverlay() const
{
    if (!m_brush.active()) return false;
    const brush::Params &p = m_brush.params();
    return !p.erase && !p.preserveTransparency && p.blend == BlendMode::Normal;
}

// Re-render one region of the cached frame in place, exactly as render::renderFrame paints it:
// the same background pixel, then every object that can reach the region, drawn with the full
// document clip (so effect buffers are the ones a full render makes) and a painter clip to r
void Canvas::renderDirtyRect(const QRect &dirty) {
    const DocState &st = m_doc->s;
    const QRect r = dirty & m_cache.rect();
    if (r.isEmpty()) return;
    ++m_partialRenders;
    QImage bgPix(1, 1, QImage::Format_ARGB32_Premultiplied);
    bgPix.fill(st.canvasColor);
    const QRgb bgRaw = reinterpret_cast<const QRgb *>(bgPix.constScanLine(0))[0];
    for (int y = r.top(); y <= r.bottom(); ++y) std::fill_n(reinterpret_cast<QRgb *>(m_cache.scanLine(y)) + r.left(), r.width(), bgRaw);
    const int frame = m_doc->curFrame;
    if (frame < 0 || frame >= st.frames.size()) return;
    QPainter p(&m_cache);
    p.setClipRect(r);
    SymbolScope scope(&st.symbols);
    const QRect clip(0, 0, st.width, st.height);
    const Frame &f = st.frames[frame];
    for (int li = 0; li < st.layers.size() && li < f.layers.size(); ++li) {
        if (!st.layers[li].visible || st.layers[li].web) continue;
        for (const VObject &o : f.layers[li]) {
            bool wraps = false;
            for (const auto &e : o.effects) wraps |= e.enabled && e.type == EffectType::Offset;
            if (!wraps) {
                const double m = o.effectMargin() + 4;
                if (!o.visualBounds().adjusted(-m, -m, m, m).toAlignedRect().intersects(r)) continue;
            }
            // Pass the dirty rect so renderObject can early-out; cache keys stay unclipped and
            // cache misses still build the full visual (pixel-identical FX edges).
            render::drawObject(p, o, r);
        }
    }
}

const QImage &Canvas::renderedFrame() {
    QRect justUpdated;
    if (!m_cacheDirty && !m_cache.isNull() && !m_cacheDirtyRect.isEmpty()) {
        justUpdated = m_cacheDirtyRect;
        renderDirtyRect(m_cacheDirtyRect);
    }
    m_cacheDirtyRect = QRect();
    if (m_cacheDirty || m_cache.isNull()) {
        m_viewCacheOk = false;
        const QVector<int> onion = m_doc->onionFrames();
        if (onion.isEmpty()) {
            m_cache = m_doc->render(m_doc->curFrame, true);
        } else {
            // onion skinning: other frames dimmed (fading with distance) under the current frame
            // a blank canvas-coloured image (render::renderFrame's background, without rendering the frame twice)
            m_cache = QImage(qMax(1, m_doc->s.width), qMax(1, m_doc->s.height), QImage::Format_ARGB32_Premultiplied);
            m_cache.fill(m_doc->s.canvasColor.alpha() ? m_doc->s.canvasColor.rgba() : 0u);
            QPainter op(&m_cache);
            for (int f : onion) {
                const int dist = qAbs(f - m_doc->curFrame);
                op.setOpacity(qBound(0.05, m_doc->onionOpacity / 100.0 / dist, 1.0));
                op.drawImage(0, 0, m_doc->render(f, false));
            }
            op.setOpacity(1);
            op.drawImage(0, 0, m_doc->render(m_doc->curFrame, false));
        }
        m_cacheDirty = false;
    } else if (!justUpdated.isEmpty()) {
        m_viewCacheDocDirty |= justUpdated;
    }
    return m_cache;
}

QImage Canvas::renderedFrameWithLive()
{
    const QImage &base = renderedFrame();
    if (!m_liveHasInk || m_liveLayer.isNull())
        return base;
    const VObject *lo = m_doc->find(m_paintTarget);
    if (!lo)
        return base;
    QImage out = base.copy();
    if (out.isNull())
        return base;
    QPainter p(&out);
    const QRectF doc = pxedit::imageToDoc(*lo).mapRect(
        QRectF(QPointF(0, 0), QSizeF(m_liveLayer.size())));
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(doc, m_liveLayer);
    p.end();
    return out;
}

// ---------------------------------------------------------------- view
void Canvas::setZoom(double z, QPointF anchor) {
    z = qBound(0.03, z, 64.0);
    if (anchor.x() < 0) anchor = QPointF(width() / 2.0, height() / 2.0);
    QPointF d = toDoc(anchor);
    m_zoom = z;
    m_origin = anchor - d * m_zoom;
    m_needFit = false;
    clampOrigin();
    update();
    emit zoomChanged(m_zoom);
    emit viewChanged();
}

double nextZoomLevel(double cur, bool in) {
    if (in) {
        for (double z : kZoomLevels) if (z > cur + 1e-6) return z;
        return kZoomLevels[std::size(kZoomLevels) - 1];
    }
    double best = kZoomLevels[0];
    for (double z : kZoomLevels) if (z < cur - 1e-6) best = z;
    return best;
}

void Canvas::zoomIn() { setZoom(nextZoomLevel(m_zoom, true)); }
void Canvas::zoomOut() { setZoom(nextZoomLevel(m_zoom, false)); }

void Canvas::setView(double z, QPointF c) {
    m_zoom = qBound(0.03, z, 64.0);
    m_origin = QPointF(width() / 2.0, height() / 2.0) - c * m_zoom;
    m_needFit = false;
    clampOrigin();
    update();
    emit zoomChanged(m_zoom);
    emit viewChanged();
}

void Canvas::pinToOrigin() {
    m_needFit = false;
    m_origin = QPointF(0, 0);
    update();
    emit viewChanged();
}

void Canvas::fitToWindow(bool zoomIn) {
    const double margin = 2.0 * qMax(0, appState().prefs.fitMargin);
    double z = qMin((width() - margin) / m_doc->s.width, (height() - margin) / m_doc->s.height);
    if (z <= 0) z = 1;
    if (!zoomIn && z > 1) z = 1;
    m_zoom = z;
    m_origin = QPointF((width() - m_doc->s.width * z) / 2.0, (height() - m_doc->s.height * z) / 2.0);
    update();
    emit zoomChanged(m_zoom);
    emit viewChanged();
}

void Canvas::zoomToRect(const QRectF &r) {
    if (r.width() <= 0 || r.height() <= 0) return;
    const double margin = 40;
    const double z = qMin((width() - margin) / r.width(), (height() - margin) / r.height());
    setView(z > 0 ? z : 1, r.center());
}

void Canvas::actualSize() {
    m_needFit = false;
    m_zoom = 1;
    m_origin = QPointF((width() - m_doc->s.width) / 2.0, (height() - m_doc->s.height) / 2.0);
    clampOrigin();
    update();
    emit zoomChanged(m_zoom);
    emit viewChanged();
}

void Canvas::clampOrigin() {
    // keep at least part of the canvas on screen
    const double w = m_doc->s.width * m_zoom, h = m_doc->s.height * m_zoom;
    m_origin.setX(qBound(-w + 40, m_origin.x(), width() - 40.0));
    m_origin.setY(qBound(-h + 40, m_origin.y(), height() - 40.0));
}

void Canvas::resizeEvent(QResizeEvent *) {
    if (m_needFit) fitToWindow(false);
    else { clampOrigin(); emit viewChanged(); }
}

// ---------------------------------------------------------------- painting
// ---------------------------------------------------------------- overlay drawing
// Every on-canvas overlay must stay readable over any artwork, black and white included.
// Two-tone strokes (a contrasting halo under the colour line), black/white marching ants
// and white-filled handles with a dark border, as Fireworks and Photoshop draw them.
namespace overlay {
QColor haloFor(const QColor &c) {
    return qGray(c.rgb()) < 140 ? QColor(255, 255, 255, 230) : QColor(0, 0, 0, 230);
}
// a coloured 1px line with a 3px contrasting halo underneath
template <typename Fn> void halo(QPainter &p, const QColor &c, Fn draw, Qt::PenStyle style = Qt::SolidLine) {
    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(haloFor(c), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    draw();
    p.setPen(QPen(c, 1, style));
    draw();
    p.restore();
}
// animated black/white marching ants: a solid white line with black dashes on top
template <typename Fn> void ants(QPainter &p, int phase, Fn draw) {
    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::white, 1));
    draw();
    QPen dash(Qt::black, 1, Qt::CustomDashLine);
    dash.setDashPattern({4, 4});
    dash.setDashOffset(phase);
    p.setPen(dash);
    draw();
    p.restore();
}
// handle: white (or given) fill with a dark border and a light outer ring
void handle(QPainter &p, const QRectF &r, const QColor &fill = Qt::white, bool ellipse = false) {
    p.save();
    p.setPen(QPen(QColor(255, 255, 255, 200), 1));
    p.setBrush(Qt::NoBrush);
    const QRectF o = r.adjusted(-1, -1, 1, 1);
    if (ellipse) p.drawEllipse(o); else p.drawRect(o);
    p.setPen(QPen(QColor(0x10, 0x10, 0x10), 1));
    p.setBrush(fill);
    if (ellipse) p.drawEllipse(r); else p.drawRect(r);
    p.restore();
}
// brush/eraser/retouch size circle: black and white concentric rings, readable on any pixel
void sizeCircle(QPainter &p, const QPointF &c, double r) {
    p.save();
    p.setBrush(Qt::NoBrush);
    r = qMax(r, 1.5);
    p.setPen(QPen(QColor(0, 0, 0, 220), 1));
    p.drawEllipse(c, r + 0.5, r + 0.5);
    p.setPen(QPen(QColor(255, 255, 255, 220), 1));
    p.drawEllipse(c, qMax(0.5, r - 0.5), qMax(0.5, r - 0.5));
    if (r < 4) {   // tiny tips: add a two-tone cross so the hot spot is still visible
        for (int pass = 0; pass < 2; ++pass) {
            p.setPen(QPen(pass ? QColor(Qt::white) : QColor(Qt::black), pass ? 1 : 3));
            p.drawLine(c - QPointF(6, 0), c - QPointF(r + 2, 0));
            p.drawLine(c + QPointF(r + 2, 0), c + QPointF(6, 0));
            p.drawLine(c - QPointF(0, 6), c - QPointF(0, r + 2));
            p.drawLine(c + QPointF(0, r + 2), c + QPointF(0, 6));
        }
    }
    p.restore();
}
void outlinedText(QPainter &p, const QRectF &r, int flags, const QString &text) {
    p.save();
    p.setPen(QColor(0, 0, 0, 200));
    for (const QPointF &o : {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)}) p.drawText(r.translated(o), flags, text);
    p.setPen(Qt::white);
    p.drawText(r, flags, text);
    p.restore();
}
} // namespace overlay

const QVector<Canvas::SelOutline> &Canvas::selectionOutlines(const QVector<VObject *> &sel, const QTransform &t) {
    QVector<quint64> ids;
    ids.reserve(sel.size());
    for (const VObject *o : sel) ids << o->id;
    if (m_selOutlinesValid && t == m_selOutlineXf && ids == m_selOutlineIds) return m_selOutlines;
    ++m_selOutlineBuilds;
    QPainterPathStroker st;
    st.setWidth(3);
    st.setCapStyle(Qt::RoundCap);
    st.setJoinStyle(Qt::RoundJoin);
    m_selOutlines.clear();
    m_selOutlines.reserve(sel.size());
    for (const VObject *o : sel) {
        SelOutline so;
        so.box = o->type == ObjType::Bitmap || o->type == ObjType::Text;
        if (so.box) {
            so.vb = t.mapRect(o->bounds());
            QPainterPath r;
            r.addRect(so.vb);
            so.halo = st.createStroke(r);
        } else {
            so.vp = t.map(o->shapePath());
            so.halo = st.createStroke(so.vp);
        }
        m_selOutlines << std::move(so);
    }
    m_selOutlineIds = std::move(ids);
    m_selOutlineXf = t;
    m_selOutlinesValid = true;
    return m_selOutlines;
}

void Canvas::paintEvent(QPaintEvent *e) {
    SymbolScope symScope(&m_doc->s.symbols);
    const QRect wr = e ? e->rect() : rect();
    painttrace::Scope _pt("paintEvent", -1, wr.x(), wr.y(),
        QStringLiteral("wr=%1x%2 backlog=%3 zoom=%4 dpr=%5")
            .arg(wr.width()).arg(wr.height())
            .arg(painttrace::moveBacklog())
            .arg(m_zoom)
            .arg(devicePixelRatioF()));
    painttrace::moveBacklog() = 0;
    renderedFrame();
    refreshViewCache();
    QPainter p(this);
    p.setClipRect(wr);
    if (!m_viewCache.isNull())
        p.drawImage(wr.topLeft(), m_viewCache, wr);
    else
        p.fillRect(wr, appState().prefs.pasteboardColor);
    const DocState &s = m_doc->s;
    const double dpr = devicePixelRatioF();
    // one snapped target for checkerboard, canvas image and frame
    const QRectF cr = snappedTarget(m_origin, QSizeF(s.width, s.height), m_zoom, dpr);
    if (appState().prefs.canvasShadow) drawCanvasShadow(p, cr);
    if (appState().prefs.canvasBorder) { // flat on the pasteboard: no shadow, at most a subtle 1px line just outside
        p.setPen(QPen(canvasBorderColor(), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cr.adjusted(-0.5, -0.5, 0.5, 0.5));
    }

    p.setRenderHint(QPainter::Antialiasing);
    if (m_doc->grid.show) drawGrid(p);
    drawGuides(p, cr);
    if (m_drag != Drag::None && (!m_snapX.isEmpty() || !m_snapY.isEmpty())) {
        // smart guides: thin two-tone lines across the view while snapped
        const QColor sc(0xFF, 0x00, 0xB4);
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        for (double x : m_snapX) {
            const double vx = std::floor(toView(QPointF(x, 0)).x()) + 0.5;
            overlay::halo(p, sc, [&] { p.drawLine(QPointF(vx, 0), QPointF(vx, height())); });
        }
        for (double y : m_snapY) {
            const double vy = std::floor(toView(QPointF(0, y)).y()) + 0.5;
            overlay::halo(p, sc, [&] { p.drawLine(QPointF(0, vy), QPointF(width(), vy)); });
        }
        p.restore();
    }
    QTransform t = QTransform::fromScale(m_zoom, m_zoom) * QTransform::fromTranslate(m_origin.x(), m_origin.y());

    // Web Layer overlay: translucent green slices and blue hotspots (View > Slices / Web Layer visibility)
    if (int w = m_doc->webLayer(); w >= 0 && appState().webLayerVisible && m_doc->s.layers[w].visible) {
        QFont f = font();
        f.setPixelSize(10);
        p.setFont(f);
        for (const VObject &o : m_doc->objects(w)) {
            if (!o.visible) continue;
            if (o.type == ObjType::Hotspot && !webFeaturesEnabled()) continue;
            const bool slice = o.type == ObjType::Slice;
            QPainterPath vp = t.map(o.shapePath());
            p.setPen(Qt::NoPen);
            p.setBrush(slice ? QColor(0x30, 0xC0, 0x30, 80) : QColor(0x30, 0x70, 0xE0, 80));
            p.drawPath(vp);
            overlay::halo(p, slice ? QColor(0x10, 0x80, 0x10) : QColor(0x10, 0x40, 0xB0), [&] { p.drawPath(vp); });
            if (slice) {
                // slice guides (red, as in Fireworks), with a light halo
                QRectF b = t.mapRect(o.bounds());
                overlay::halo(p, QColor(0xE0, 0x30, 0x30), [&] {
                    for (double x : {b.left(), b.right()}) p.drawLine(QPointF(x, cr.top()), QPointF(x, cr.bottom()));
                    for (double y : {b.top(), b.bottom()}) p.drawLine(QPointF(cr.left(), y), QPointF(cr.right(), y));
                });
            }
            QRectF b = vp.boundingRect();
            if (b.width() > 24 && b.height() > 12) {
                QString label = o.name;
                if (webFeaturesEnabled() && !o.link.isEmpty()) label += QStringLiteral("  ") + o.link;
                overlay::outlinedText(p, b.adjusted(3, 1, -2, 0), Qt::AlignLeft | Qt::AlignTop, label);
            }
        }
    }

    // hover highlight (red, as in Fireworks)
    if (appState().prefs.mouseHighlight && m_hoverId && m_drag == Drag::None && !m_doc->selection.contains(m_hoverId)) {
        if (VObject *o = m_doc->find(m_hoverId)) {
            const bool box = o->type == ObjType::Bitmap || o->type == ObjType::Text;
            const QPainterPath vp = box ? QPainterPath() : t.map(o->shapePath());
            const QRectF vb = box ? t.mapRect(o->bounds()) : QRectF();
            overlay::halo(p, appState().prefs.highlightColor, [&] { if (box) p.drawRect(vb); else p.drawPath(vp); });
        }
    }
    // selection
    const auto sel = m_doc->selectedObjects();
    if (!sel.isEmpty()) {
        // keep the selection colour, but give it a contrasting halo (text boxes included). The
        // outlines and their 3 px halo strokes are cached (P9); filling the cached stroke is
        // pixel-identical to stroking with overlay::halo's pen
        const QColor sc = appState().prefs.selectionColor;
        const QColor hc = overlay::haloFor(sc);
        p.save();
        for (const SelOutline &so : selectionOutlines(sel, t)) {
            p.setPen(Qt::NoPen);
            p.setBrush(hc);
            p.drawPath(so.halo);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(sc, 1));
            if (so.box) p.drawRect(so.vb); else p.drawPath(so.vp);
        }
        p.restore();
        Tool tool = effectiveTool();
        if (tool == Tool::Subselect && sel.size() == 1 && (canEditPoints(sel[0]) || sel[0]->type == ObjType::Line)) {
            QPolygonF pts = canEditPoints(sel[0]) ? sel[0]->points : QPolygonF({sel[0]->p1, sel[0]->p2});
            const VObject *so = sel[0];
            if (so->type == ObjType::Path && m_pointIndex >= 0 && m_pointIndex < pts.size() && m_pointIndex < so->hIn.size()) {
                // Bezier handles of the selected node
                QPointF v = toView(pts[m_pointIndex]);
                for (const QPointF &h : {so->hIn[m_pointIndex], so->hOut[m_pointIndex]}) {
                    if (QLineF(h, pts[m_pointIndex]).length() < 1e-6) continue;
                    QPointF hv = toView(h);
                    overlay::halo(p, appState().prefs.selectionColor, [&] { p.drawLine(v, hv); });
                    overlay::handle(p, QRectF(hv.x() - 3, hv.y() - 3, 6, 6), appState().prefs.selectionColor, true);
                }
            }
            for (int i = 0; i < pts.size(); ++i) {
                QPointF v = toView(pts[i]);
                overlay::handle(p, QRectF(v.x() - 3, v.y() - 3, 6, 6), i == m_pointIndex ? appState().prefs.selectionColor : QColor(Qt::white));
            }
        } else if (toolIsTransform(tool) && ensureTransform()) {
            // transform box: corners, edge midpoints and the centre of rotation
            QPolygonF qv;
            for (int i = 0; i < 4; ++i) qv << transformHandleView(i);
            overlay::halo(p, appState().prefs.selectionColor, [&] { p.drawPolygon(qv); }, Qt::DashLine);
            p.setRenderHint(QPainter::Antialiasing, false);
            for (int i = 0; i < 8; ++i) {
                const QPointF h = transformHandleView(i);
                overlay::handle(p, QRectF(qRound(h.x()) - 3.5, qRound(h.y()) - 3.5, 7, 7));
            }
            p.setRenderHint(QPainter::Antialiasing, true);
            const QPointF c = transformHandleView(8);
            for (int pass = 0; pass < 2; ++pass) {
                p.setPen(QPen(pass ? appState().prefs.selectionColor : QColor(Qt::white), pass ? 1.2 : 3));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(c, 4, 4);
                p.drawLine(c - QPointF(7, 0), c + QPointF(7, 0));
                p.drawLine(c - QPointF(0, 7), c + QPointF(0, 7));
            }
        } else if (toolIsSelect(tool) || tool == Tool::Subselect) {
            QRectF b = m_doc->selectionBounds();
            if (sel.size() > 1) {
                const QRectF vb = t.mapRect(b);
                overlay::halo(p, appState().prefs.selectionColor, [&] { p.drawRect(vb); }, Qt::DashLine);
            }
            p.setRenderHint(QPainter::Antialiasing, false);
            for (const QRectF &h : handleRects(b)) overlay::handle(p, h);
            p.setRenderHint(QPainter::Antialiasing, true);
        }
    }
    // fill handles. Gradients: circle = start (moves the whole gradient), squares = end and second
    // axis. Patterns (Fireworks style): circle = centre (moves the tile), two square-ended bars =
    // the tile's x and y axes (drag an end to scale / skew / turn that axis, a bar to turn both)
    if (VObject *g = gradientTarget()) {
        QPointF hd[3];
        fills::fillHandles(*g, hd);
        const QPointF a = toView(hd[0]), b = toView(hd[1]), c = toView(hd[2]);
        const bool pattern = g->fillKind == FillKind::Pattern;
        const bool second = pattern || fills::usesSecondAxis(g->fillKind);
        for (int pass = 0; pass < 2; ++pass) {
            p.setPen(QPen(pass ? QColor(Qt::black) : QColor(Qt::white), pass ? 1 : 3));
            p.drawLine(a, b);
            if (second) p.drawLine(a, c);
        }
        overlay::handle(p, QRectF(a.x() - 4.5, a.y() - 4.5, 9, 9), Qt::white, true);
        overlay::handle(p, QRectF(b.x() - 4, b.y() - 4, 8, 8), appState().prefs.selectionColor);
        const double cs = pattern ? 8 : 7;
        if (second) overlay::handle(p, QRectF(c.x() - cs / 2, c.y() - cs / 2, cs, cs), appState().prefs.selectionColor);
    }
    // Auto Shape diamonds (yellow, Fireworks style)
    if (VObject *o = autoShapeTarget()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const autoshape::Handle &h : autoshape::handles(*o)) {
            const QPointF c = toView(o->hasXform() ? o->xform.map(h.pos) : h.pos);
            const QPolygonF dm({c + QPointF(0, -5.5), c + QPointF(5.5, 0), c + QPointF(0, 5.5), c + QPointF(-5.5, 0)});
            p.setPen(QPen(QColor(0x60, 0x48, 0x00), 1));
            p.setBrush(QColor(0xFF, 0xD7, 0x00));
            p.drawPolygon(dm);
        }
    }
    // Content-Aware Remove: the mask being painted
    if ((m_drag == Drag::ContentAware && !m_caPath.isEmpty()) || (m_caJob && !m_caPending.isEmpty())) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(QColor(255, 40, 40, m_caJob ? 80 : 140), qMax(1.0, appState().brushSize * m_zoom * 1.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(t.map(m_drag == Drag::ContentAware ? m_caPath : m_caPending));
        p.restore();
    }
    // pixel selection (marching ants)
    if (!m_doc->pixelSelection.isEmpty()) {
        QPainterPath vp = t.map(m_doc->pixelSelection);
        overlay::ants(p, m_antsPhase, [&] { p.drawPath(vp); });
    }
    if (m_drag == Drag::RubberBand || m_drag == Drag::PixelRect || m_drag == Drag::ZoomRect) {
        const QRectF vr = t.mapRect(m_rubber);
        if (m_drag == Drag::RubberBand) p.fillRect(vr, QColor(0, 100, 255, 30));
        if (m_drag == Drag::PixelRect && m_selOval) overlay::ants(p, m_antsPhase, [&] { p.drawEllipse(vr); });
        else overlay::ants(p, m_antsPhase, [&] { p.drawRect(vr); });
    }
    if ((m_drag == Drag::Lasso || m_drag == Drag::Knife) && m_lasso.size() > 1) {
        const QPolygonF vl = t.map(m_lasso);
        overlay::ants(p, m_antsPhase, [&] { p.drawPolyline(vl); });
    }
    if (!m_poly.isEmpty()) {   // Polygon Lasso in progress: the sides so far, a rubber line and the start point
        QPolygonF vl = t.map(m_poly);
        vl << t.map(polyConstrain(m_hoverDoc, QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier));
        overlay::ants(p, m_antsPhase, [&] { p.drawPolyline(vl); });
        overlay::handle(p, QRectF(vl.first().x() - 3, vl.first().y() - 3, 6, 6), Qt::white);
    }
    if (m_cropActive) {
        QRectF vc = t.mapRect(m_crop);
        QPainterPath outside;
        outside.addRect(rect());
        QPainterPath inner;
        inner.addRect(vc);
        p.fillPath(outside.subtracted(inner), QColor(0, 0, 0, 110));
        overlay::ants(p, m_antsPhase, [&] { p.drawRect(vc); });
        // corner and edge handles of the crop box
        for (const QPointF &h : {vc.topLeft(), vc.topRight(), vc.bottomLeft(), vc.bottomRight(),
                                 QPointF(vc.center().x(), vc.top()), QPointF(vc.center().x(), vc.bottom()),
                                 QPointF(vc.left(), vc.center().y()), QPointF(vc.right(), vc.center().y())})
            overlay::handle(p, QRectF(h.x() - 3, h.y() - 3, 6, 6));
        overlay::outlinedText(p, vc.adjusted(6, 6, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                              QStringLiteral("%1 x %2  (Enter to crop, Esc to cancel)").arg(qRound(m_crop.width())).arg(qRound(m_crop.height())));
    }
    if (m_areaActive) {
        // Export Area: the outside dimmed, marching ants, 8 resize handles and an x, y  w x h readout
        const QRectF va = t.mapRect(QRectF(m_area));
        QPainterPath outside;
        outside.addRect(rect());
        QPainterPath inner;
        inner.addRect(va);
        p.fillPath(outside.subtracted(inner), QColor(0, 0, 0, 90));
        overlay::ants(p, m_antsPhase, [&] { p.drawRect(va); });
        for (const QPointF &h : {va.topLeft(), va.topRight(), va.bottomLeft(), va.bottomRight(),
                                 QPointF(va.center().x(), va.top()), QPointF(va.center().x(), va.bottom()),
                                 QPointF(va.left(), va.center().y()), QPointF(va.right(), va.center().y())})
            overlay::handle(p, QRectF(h.x() - 3, h.y() - 3, 6, 6));
        overlay::outlinedText(p, va.adjusted(6, 6, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                              QStringLiteral("%1, %2  %3 \u00D7 %4  (Enter or double-click to export, Esc to cancel)")
                                  .arg(m_area.x()).arg(m_area.y()).arg(m_area.width()).arg(m_area.height()));
    }
    if (!m_penPts.isEmpty()) {
        const QColor sc = m_penTool == Tool::PolySlice ? QColor(0x10, 0x80, 0x10)
                         : m_penTool == Tool::PolyHotspot ? QColor(0x10, 0x40, 0xB0)
                         : appState().prefs.selectionColor;
        if ((m_penTool == Tool::PolySlice || m_penTool == Tool::PolyHotspot) && m_penPts.size() >= 2) {
            QPolygonF fill = t.map(m_penPts);
            fill << t.map(m_hoverDoc);
            p.setPen(Qt::NoPen);
            p.setBrush(m_penTool == Tool::PolySlice ? QColor(0x30, 0xC0, 0x30, 80) : QColor(0x30, 0x70, 0xE0, 80));
            p.drawPolygon(fill);
        }
        QPolygonF vp = t.map(m_penPts);
        QPainterPath pp(m_penPts.first());
        const bool curves = m_penIn.size() == m_penPts.size();
        for (int i = 1; i < m_penPts.size(); ++i) {
            if (curves) pp.cubicTo(m_penOut[i - 1], m_penIn[i], m_penPts[i]);
            else pp.lineTo(m_penPts[i]);
        }
        const QPainterPath vpath = t.map(pp);
        overlay::halo(p, sc, [&] { p.drawPath(vpath); });
        if (m_drag != Drag::PenHandle) {
            QPainterPath rubber(m_penPts.last());
            if (curves) rubber.cubicTo(m_penOut.last(), m_hoverDoc, m_hoverDoc);
            else rubber.lineTo(m_hoverDoc);
            const QPainterPath vr = t.map(rubber);
            overlay::halo(p, sc, [&] { p.drawPath(vr); }, Qt::DashLine);
        }
        if (curves && QLineF(m_penOut.last(), m_penPts.last()).length() > 1e-6) {
            for (const QPointF &h : {m_penIn.last(), m_penOut.last()}) {
                const QPointF hv = toView(h);
                overlay::halo(p, sc, [&] { p.drawLine(vp.last(), hv); });
                overlay::handle(p, QRectF(hv.x() - 3, hv.y() - 3, 6, 6), sc, true);
            }
        }
        for (int i = 0; i < vp.size(); ++i)
            overlay::handle(p, QRectF(vp[i].x() - 3, vp[i].y() - 3, 6, 6), i == 0 ? QColor(Qt::white) : sc);
    }
    if (m_freehand.size() >= 2) {
        const QColor sc = appState().prefs.selectionColor;
        overlay::halo(p, sc, [&] { p.drawPolyline(t.map(m_freehand)); });
    }
    // Rubber Stamp source marker (follows the brush while cloning)
    Tool tool = effectiveTool();
    if ((tool == Tool::RubberStamp || tool == Tool::Healing) && m_stampSet) {
        QPointF sp = m_drag == Drag::Paint ? m_lastDoc + m_stampOffset : m_stampSrc;
        if (m_drag != Drag::Paint && m_stampOffsetSet && appState().stampAligned) sp = m_hoverDoc + m_stampOffset;
        QPointF v = toView(sp);
        overlay::halo(p, QColor(0xE0, 0x30, 0x30), [&] {
            p.drawEllipse(v, 5, 5);
            p.drawLine(v - QPointF(8, 0), v + QPointF(8, 0));
            p.drawLine(v - QPointF(0, 8), v + QPointF(0, 8));
        });
    }
    // Live stroke layer: after the view-cache blit, so a cache update cannot
    // erase ink. Not stored in m_viewCache. Drawn before the size circle.
    if (!m_liveLayer.isNull() && (m_drag == Drag::Paint || !m_liveLayer.isNull())) {
        if (VObject *lo = m_doc->find(m_paintTarget)) {
            const QRectF doc = pxedit::imageToDoc(*lo).mapRect(
                QRectF(QPointF(0, 0), QSizeF(m_liveLayer.size())));
            const QRectF vr = t.mapRect(doc);
            if (!vr.isEmpty() && wr.intersects(vr.toAlignedRect().adjusted(-2, -2, 2, 2))) {
                p.save();
                p.setRenderHint(QPainter::SmoothPixmapTransform, !qFuzzyCompare(m_zoom, 1.0));
                p.setRenderHint(QPainter::Antialiasing, true);
                p.drawImage(vr, m_liveLayer);
                p.restore();
            }
            // Cheap unclipped tail from the last quality suffix to the cursor,
            // so a long path-engine flood still shows ink up to the pointer.
            if (m_brush.active() && m_brush.usesEngine()
                && m_brush.enginePathCount() > m_liveSuffixAt) {
                const QPainterPath &ep = m_brush.enginePath();
                const int n = ep.elementCount();
                // Only the last ~1.25 brush widths: a long QPen tail was too
                // visible at 30–40 px. The worker replaces this within one cycle.
                const double maxTail = qMax(16.0, double(appState().brushSize) * 1.25);
                int from = n - 1;
                double acc = 0;
                QPointF startIp;
                bool haveStart = false;
                // Walk back only along the unpainted suffix so a 2-point
                // 270 px jump cannot become a full-length QPen tail.
                for (int i = n - 1; i > m_liveSuffixAt && i > 0; --i) {
                    const QPointF a(ep.elementAt(i - 1).x, ep.elementAt(i - 1).y);
                    const QPointF b(ep.elementAt(i).x, ep.elementAt(i).y);
                    const double len = QLineF(a, b).length();
                    if (acc + len >= maxTail) {
                        const double take = maxTail - acc;
                        const double u = (len > 1e-9) ? (take / len) : 0.0;
                        startIp = b + (a - b) * u;
                        haveStart = true;
                        from = i;
                        break;
                    }
                    acc += len;
                    from = i - 1;
                }
                from = qMax(from, m_liveSuffixAt);
                if (from < n || haveStart) {
                    QPainterPath tail;
                    auto toDoc = [&](const QPointF &ip) { return pxedit::imageToDoc(*lo).map(ip); };
                    if (haveStart) tail.moveTo(toDoc(startIp));
                    for (int i = from; i < n; ++i) {
                        const QPointF ip(ep.elementAt(i).x, ep.elementAt(i).y);
                        if (tail.elementCount() == 0) tail.moveTo(toDoc(ip));
                        else tail.lineTo(toDoc(ip));
                    }
                    p.save();
                    p.setRenderHint(QPainter::Antialiasing, true);
                    const double w = qMax(1.0, appState().brushSize * m_zoom);
                    QColor tc = appState().stroke;
                    tc.setAlpha(qBound(80, tc.alpha(), 200));
                    p.setPen(QPen(tc, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    p.setBrush(Qt::NoBrush);
                    p.drawPath(t.map(tail));
                    p.restore();
                }
            }
        }
    }
    // Brush-size circle is an overlay, never part of m_viewCache, and is drawn
    // last so a dirty update that missed the previous ring cannot leave a trail
    // on top of the stroke. update() callers must union brushCursorViewRect().
    const bool pathBrush = tool == Tool::Freeform || tool == Tool::ReshapeArea || tool == Tool::PathScrubber;
    if ((tool == Tool::Brush || tool == Tool::Eraser || isRetouch(tool) || tool == Tool::ContentAware || pathBrush)
        && underMouse() && m_drag != Drag::Pan && (appState().prefs.brushSizeCursors || pathBrush)) {
        double r = (tool == Tool::PathScrubber ? appState().psSize : pathBrush ? appState().pathToolSize : appState().brushSize) * m_zoom / 2.0;
        overlay::sizeCircle(p, toView(m_hoverDoc), r);
    }
}

// ---------------------------------------------------------------- helpers
// the uniform scale + turn about c that takes `from` to `to` (identity when `from` sits on c)
QTransform Canvas::gradSimilarity(const QPointF &c, const QPointF &from, const QPointF &to) {
    const QPointF a = from - c, b = to - c;
    const double la2 = a.x() * a.x() + a.y() * a.y();
    if (la2 < 1e-12) return QTransform();
    // complex division b / a = scale * e^(i angle)
    const double re = (b.x() * a.x() + b.y() * a.y()) / la2, im = (b.y() * a.x() - b.x() * a.y()) / la2;
    return QTransform::fromTranslate(-c.x(), -c.y()) * QTransform(re, im, -im, re, 0, 0) * QTransform::fromTranslate(c.x(), c.y());
}

VObject *Canvas::gradientTarget() const {
    const Tool t = effectiveTool();
    if (!toolIsSelect(t) && t != Tool::Subselect && !toolIsTransform(t) && t != Tool::Gradient && t != Tool::PaintBucket) return nullptr;
    VObject *o = m_doc->singleSelected();
    if (!o || !o->isVector() || !o->fillOn || o->type == ObjType::Line) return nullptr;
    if (fills::isGradient(o->fillKind)) return o;
    // pattern handles: Pointer (and Subselect) and Paint Bucket, as in Fireworks
    if (o->fillKind == FillKind::Pattern && (toolIsSelect(t) || t == Tool::Subselect || t == Tool::PaintBucket)) return o;
    return nullptr;
}

int Canvas::gradHandleAt(const QPointF &v) const {
    VObject *g = gradientTarget();
    if (!g) return -1;
    QPointF h[3];
    fills::fillHandles(*g, h);
    const bool pattern = g->fillKind == FillKind::Pattern;
    // end handles first: they sit on top of the start circle when the vector is short
    for (int i : {1, 2, 0}) {
        if (i == 2 && !pattern && !fills::usesSecondAxis(g->fillKind)) continue;
        if (QLineF(toView(h[i]), v).length() <= 6) return i;
    }
    if (pattern) { // on a bar (not its ends): 3 = turn both axes about the centre
        const QPointF a = toView(h[0]);
        for (int i : {1, 2}) {
            const QPointF b = toView(h[i]), ab = b - a;
            const double l2 = QPointF::dotProduct(ab, ab);
            if (l2 < 1) continue;
            const double t = QPointF::dotProduct(v - a, ab) / l2;
            if (t > 0.15 && t < 0.95 && QLineF(a + ab * t, v).length() <= 4) return 3;
        }
    }
    return -1;
}

VObject *Canvas::autoShapeTarget() const {
    const Tool t = effectiveTool();
    if (!toolIsSelect(t) && t != Tool::Subselect && !toolIsAutoShape(t)) return nullptr;
    VObject *o = m_doc->singleSelected();
    return (o && o->type == ObjType::AutoShape && !o->locked) ? o : nullptr;
}

int Canvas::shapeHandleAt(const QPointF &v) const {
    VObject *o = autoShapeTarget();
    if (!o) return -1;
    const auto hs = autoshape::handles(*o);
    for (int i = hs.size() - 1; i >= 0; --i) {
        const QPointF dp = o->hasXform() ? o->xform.map(hs[i].pos) : hs[i].pos;
        const QPointF d = toView(dp) - v;
        if (std::abs(d.x()) + std::abs(d.y()) <= 7) return hs[i].id;
    }
    return -1;
}

static QCursor rotateCursor() {
    static QCursor c = [] {
        QPixmap pm(24, 24);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        for (int pass = 0; pass < 2; ++pass) {
            p.setPen(QPen(pass ? QColor(Qt::black) : QColor(Qt::white), pass ? 1.5 : 3.5, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(QRectF(5, 5, 14, 14), 30 * 16, 240 * 16);
            p.drawLine(QPointF(17.5, 6), QPointF(19.5, 10.5));
            p.drawLine(QPointF(19.5, 10.5), QPointF(14.5, 10.5));
        }
        return QCursor(pm, 12, 12);
    }();
    return c;
}

Tool Canvas::effectiveTool() const {
    if (m_space) return Tool::Hand;
    return appState().tool();
}

bool Canvas::scrubCursorMinus() const {
    return (appState().psMode == 1) != m_altHeld;
}

// Path Scrubber cursor: a small crosshair with a + or - badge
static QCursor scrubCursor(bool minus) {
    static QCursor cache[2];
    QCursor &c = cache[minus ? 1 : 0];
    if (c.pixmap().isNull()) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        for (int pass = 0; pass < 2; ++pass) {
            p.setPen(QPen(pass ? QColor(Qt::black) : QColor(Qt::white), pass ? 1.0 : 3.0, Qt::SolidLine, Qt::FlatCap));
            p.drawLine(QPointF(10.5, 3), QPointF(10.5, 8)); p.drawLine(QPointF(10.5, 13), QPointF(10.5, 18));
            p.drawLine(QPointF(3, 10.5), QPointF(8, 10.5)); p.drawLine(QPointF(13, 10.5), QPointF(18, 10.5));
        }
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(23, 23), 6.5, 6.5);
        p.setPen(QPen(Qt::black, 1.6, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(19.5, 23), QPointF(26.5, 23));
        if (!minus) p.drawLine(QPointF(23, 19.5), QPointF(23, 26.5));
        p.end();
        c = QCursor(pm, 10, 10);
    }
    return c;
}

QRect Canvas::brushCursorViewRect() const
{
    const Tool tool = effectiveTool();
    const bool pathBrush = tool == Tool::Freeform || tool == Tool::ReshapeArea || tool == Tool::PathScrubber;
    if (!((tool == Tool::Brush || tool == Tool::Eraser || isRetouch(tool) || tool == Tool::ContentAware || pathBrush)
          && underMouse() && m_drag != Drag::Pan && (appState().prefs.brushSizeCursors || pathBrush)))
        return QRect();
    const double r0 = (tool == Tool::PathScrubber ? appState().psSize
                       : pathBrush ? appState().pathToolSize
                                   : appState().brushSize) * m_zoom / 2.0;
    const double r = qMax(r0, 1.5);
    // sizeCircle: 1 px rings at r±0.5, and a 3 px cross out to 6 px when r < 4.
    double ext = r + 0.5 + 1.0;
    if (r < 4.0) ext = qMax(ext, 6.0 + 1.5);
    ext += 2.0;
    const QPointF c = toView(m_hoverDoc);
    const double dpr = devicePixelRatioF();
    const int x0 = int(std::floor((c.x() - ext) * dpr));
    const int y0 = int(std::floor((c.y() - ext) * dpr));
    const int x1 = int(std::ceil((c.x() + ext) * dpr));
    const int y1 = int(std::ceil((c.y() + ext) * dpr));
    const QRect vr(QPoint(int(std::floor(x0 / dpr)), int(std::floor(y0 / dpr))),
                   QPoint(int(std::ceil(x1 / dpr)) - 1, int(std::ceil(y1 / dpr)) - 1));
    return vr.intersected(QRect(QPoint(0, 0), size()));
}

void Canvas::revealBrushCursor()
{
    const QRect now = brushCursorViewRect();
    const QRect u = m_lastCursorView.united(now);
    m_lastCursorView = now;
    if (!u.isEmpty()) update(u);
}

void Canvas::updateCursor() {
    const Tool et = effectiveTool();
    if (et == Tool::PathScrubber) { setCursor(scrubCursor(scrubCursorMinus())); return; }
    const bool painting = et == Tool::Brush || et == Tool::Eraser || et == Tool::Blur || et == Tool::Sharpen || et == Tool::Dodge
                          || et == Tool::Burn || et == Tool::Smudge || et == Tool::RubberStamp
                          || et == Tool::Healing || et == Tool::SpotHealing || et == Tool::ReplaceColor || et == Tool::RedEye;
    if (painting && !appState().prefs.brushSizeCursors) { setCursor(Qt::CrossCursor); return; }
    if (appState().prefs.preciseCursors && et != Tool::Hand && !toolIsSelect(et) && et != Tool::Subselect && !toolIsTransform(et) && et != Tool::Text) {
        setCursor(painting ? Qt::BlankCursor : Qt::CrossCursor);
        return;
    }
    switch (et) {
    case Tool::Hand: setCursor(m_drag == Drag::Pan ? Qt::ClosedHandCursor : Qt::OpenHandCursor); break;
    case Tool::Pointer: case Tool::SelectBehind: case Tool::Subselect: case Tool::Scale: case Tool::Skew: case Tool::Distort: case Tool::Perspective: setCursor(Qt::ArrowCursor); break;
    case Tool::Text: setCursor(Qt::IBeamCursor); break;
    case Tool::Zoom: case Tool::Eyedropper: case Tool::PaintBucket: case Tool::MagicWand: setCursor(Qt::PointingHandCursor); break;
    case Tool::Brush: case Tool::Eraser: case Tool::Blur: case Tool::Sharpen: case Tool::Dodge: case Tool::Burn:
    case Tool::Smudge: case Tool::RubberStamp: case Tool::Healing: case Tool::SpotHealing: case Tool::ReplaceColor: case Tool::RedEye: setCursor(Qt::BlankCursor); break;
    default: setCursor(Qt::CrossCursor); break;
    }
}

QVector<VObject *> Canvas::scrubTargets() const {
    QVector<VObject *> out;
    for (VObject *o : m_doc->selectedEditable())
        if (o->type == ObjType::Path && !o->textOnPath && o->points.size() >= 2) out << o;
    return out;
}

// one Path Scrubber dab: raises (or lowers) the pressure data of the anchors under the brush
void Canvas::scrubDab(const QPointF &d, bool alt) {
    const AppState &as = appState();
    double size = as.psSize;
    if (as.pathPressure) size *= qMax(0.1, tablet::withFloor(penSample().pressure, as.prefs.tabletMinSize / 100.0));
    const double r = qMax(0.5, size / 2);
    const double sign = ((as.psMode == 0) != alt) ? 1.0 : -1.0;
    const double delta = sign * as.psStrength / 100.0 * 0.15;
    for (VObject *o : scrubTargets()) {
        const QPointF p = o->hasXform() ? o->xform.inverted().map(d) : d;
        if (!m_scrubPrepared.contains(o->id)) {
            if (paths::distanceToOutline(*o, p) > r) continue;
            // anchors close enough together for the data to vary under the brush
            if (paths::subdivideForPressure(*o, qBound(0.5, size / 4, 50.0))) m_scrubChanged = true;
            m_scrubPrepared.insert(o->id);
        }
        if (paths::scrub(*o, p, r, delta, as.psAffectSize, as.psAffectOpacity)) m_scrubChanged = true;
    }
}

quint64 Canvas::hitObject(const QPointF &d, bool editableOnly) const {
    const auto &f = m_doc->s.frames[m_doc->curFrame];
    const double tol = appState().prefs.pickDistance / m_zoom;
    for (int li = f.layers.size() - 1; li >= 0; --li) {
        const LayerInfo &L = m_doc->s.layers[li];
        if (!L.visible || (editableOnly && L.locked)) continue;
        if (L.web && !appState().webLayerVisible) continue;
        for (int i = f.layers[li].size() - 1; i >= 0; --i) {
            const VObject &o = f.layers[li][i];
            if (!o.visible || (editableOnly && o.locked)) continue;
            if (o.type == ObjType::Hotspot && !webFeaturesEnabled()) continue;
            if (o.hitTest(d, tol)) return o.id;
        }
    }
    return 0;
}

QVector<quint64> Canvas::hitStack(const QPointF &d, bool editableOnly) const {
    QVector<quint64> ids;
    const auto &f = m_doc->s.frames[m_doc->curFrame];
    const double tol = appState().prefs.pickDistance / m_zoom;
    for (int li = f.layers.size() - 1; li >= 0; --li) {
        const LayerInfo &L = m_doc->s.layers[li];
        if (!L.visible || (editableOnly && L.locked)) continue;
        if (L.web && !appState().webLayerVisible) continue;
        for (int i = f.layers[li].size() - 1; i >= 0; --i) {
            const VObject &o = f.layers[li][i];
            if (!o.visible || (editableOnly && o.locked)) continue;
            if (o.type == ObjType::Hotspot && !webFeaturesEnabled()) continue;
            if (o.hitTest(d, tol)) ids.append(o.id);
        }
    }
    return ids;
}

QVector<quint64> Canvas::hitChain(const QPointF &d, bool editableOnly) const {
    QVector<quint64> chain;
    const quint64 top = hitObject(d, editableOnly);
    if (!top) return chain;
    const double tol = appState().prefs.pickDistance / m_zoom;
    const VObject *o = const_cast<Document *>(m_doc)->find(top);
    while (o) {
        chain << o->id;
        const VObject *next = nullptr;
        if (o->type == ObjType::Group)
            for (int i = o->children.size() - 1; i >= 0 && !next; --i) {
                const VObject &c = o->children[i];
                if (c.visible && !(editableOnly && c.locked) && c.hitTest(d, tol)) next = &c;
            }
        o = next;
    }
    return chain;
}

quint64 Canvas::pickObject(const QPointF &d, bool subselect, quint64 *enterOut) const {
    const QVector<quint64> chain = hitChain(d, false);
    if (chain.isEmpty()) { if (enterOut) *enterOut = m_doc->enteredGroup; return 0; }
    if (subselect) {
        // Fireworks: Subselection picks the leaf without entering its group, so the Pointer
        // afterwards still picks the whole group
        if (enterOut) *enterOut = 0;
        return chain.last();
    }
    const int k = m_doc->enteredGroup ? int(chain.indexOf(m_doc->enteredGroup)) : -1;
    if (k >= 0 && k + 1 < chain.size()) { if (enterOut) *enterOut = m_doc->enteredGroup; return chain[k + 1]; }
    if (enterOut) *enterOut = 0;   // a click outside the entered group leaves it
    return chain.first();
}

QVector<QRectF> Canvas::handleRects(const QRectF &b) const {
    QRectF v(toView(b.topLeft()), toView(b.bottomRight()));
    const double hs = 3.5;
    QPointF pts[8] = {v.topLeft(), {v.center().x(), v.top()}, v.topRight(), {v.right(), v.center().y()},
                      v.bottomRight(), {v.center().x(), v.bottom()}, v.bottomLeft(), {v.left(), v.center().y()}};
    QVector<QRectF> out;
    for (auto &pt : pts) out << QRectF(qRound(pt.x()) - hs, qRound(pt.y()) - hs, hs * 2, hs * 2);
    return out;
}

int Canvas::handleAt(const QPointF &vp) const {
    if (m_doc->selection.isEmpty()) return -1;
    auto rects = handleRects(const_cast<Document *>(m_doc)->selectionBounds());
    for (int i = 0; i < rects.size(); ++i)
        if (rects[i].adjusted(-3, -3, 3, 3).contains(vp)) return i;
    return -1;
}

// canvas -> image pixels. Pixel tools run inside a Document pixel edit (beginPixelEdit), which puts a
// bitmap with a live Offset into display geometry, so the plain rect mapping lands where the user points.
QPointF Canvas::toImage(const VObject &o, const QPointF &d) const { return pxedit::docToImage(o, d); }

static QPointF constrain45(const QPointF &a, const QPointF &b) {
    QPointF d = b - a;
    double ang = qAtan2(d.y(), d.x());
    double snap = qRound(ang / (M_PI / 4)) * (M_PI / 4);
    double len = qSqrt(d.x() * d.x() + d.y() * d.y());
    return a + QPointF(qCos(snap) * len, qSin(snap) * len);
}

// ---------------------------------------------------------------- pixel selections

namespace {
// Bitmap the Magic Wand should sample (Fireworks): selected bitmap under the click in image
// space, else the topmost unlocked bitmap. Text/vectors above are ignored so their outlines are
// not cut into the selection when the user later copies or glows the mask.
VObject *bitmapForWand(Document *doc, const QPointF &d) {
    auto covers = [&](const VObject &o) {
        if (o.type != ObjType::Bitmap || o.image.isNull() || !o.visible || o.locked) return false;
        return o.image.rect().contains(pxedit::docToImage(o, d).toPoint());
    };
    for (VObject *o : doc->selectedEditable())
        if (covers(*o)) return o;
    if (doc->curFrame < 0 || doc->curFrame >= doc->s.frames.size()) return nullptr;
    const Frame &f = doc->s.frames[doc->curFrame];
    for (int li = f.layers.size() - 1; li >= 0; --li) {
        if (li >= doc->s.layers.size()) continue;
        const LayerInfo &L = doc->s.layers[li];
        if (!L.visible || L.locked || L.web) continue;
        for (int i = f.layers[li].size() - 1; i >= 0; --i) {
            const VObject &o = f.layers[li][i];
            if (!covers(o)) continue;
            return doc->find(o.id);
        }
    }
    return nullptr;
}
} // namespace

void Canvas::commitPixelSelection(const QPainterPath &p, Qt::KeyboardModifiers mods, Tool tool) {
    const AppState &as = appState();
    int edge = as.marqueeEdge;
    double feather = as.marqueeFeather;
    if (tool == Tool::Lasso || tool == Tool::PolyLasso) { edge = as.lassoEdge; feather = as.lassoFeather; }
    else if (tool == Tool::MagicWand) { edge = as.wandEdge; feather = as.wandFeather; }
    m_doc->combinePixelSelection(p, mods, edge, feather);
    update();
}

QPointF Canvas::polyConstrain(QPointF d, bool shift) const {
    if (!shift || m_poly.isEmpty()) return d;
    const QPointF a = m_poly.last();
    QLineF l(a, d);
    const double ang = qRound(l.angle() / 45.0) * 45.0;
    QLineF r = QLineF::fromPolar(l.length(), ang);
    return a + QPointF(r.dx(), r.dy());
}

void Canvas::finishPolyLasso() {
    QPainterPath p;
    if (m_poly.size() > 2) { p.addPolygon(m_poly); p.closeSubpath(); }
    const Qt::KeyboardModifiers mods = m_polyMods;
    m_poly.clear();
    if (!p.isEmpty() || !(mods & (Qt::ShiftModifier | Qt::AltModifier))) commitPixelSelection(p, mods, Tool::PolyLasso);
    update();
}

void Canvas::cancelPolyLasso() {
    m_poly.clear();
    update();
}

// Rubber Stamp / Healing Brush source pixels in `o`'s image space: the object itself, or with
// "Use Entire Document" everything visible flattened.
QImage Canvas::stampSource(const VObject &o) {
    if (!appState().useEntireDocument) return o.image.copy();
    QImage src(o.image.size(), QImage::Format_ARGB32_Premultiplied);
    src.fill(Qt::transparent);
    QPainter p(&src);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setTransform(QTransform::fromTranslate(-o.rect.left(), -o.rect.top()) *
                   QTransform::fromScale(o.image.width() / qMax(1.0, o.rect.width()), o.image.height() / qMax(1.0, o.rect.height())));
    p.drawImage(QPointF(0, 0), renderedFrame());
    return src;
}

// ---------------------------------------------------------------- bitmap painting
void Canvas::beginPaint(const QPointF &d) {
    painttrace::Scope _pt("beginPaint", -1, d.x(), d.y());
    painttrace::moveBacklog() = 0;
    painttrace::paintToCount() = 0;
    if (m_paintStoreBound)
        detachPaintStore(m_doc->find(m_paintTarget));
    Tool tool = effectiveTool();
    m_lastPen = penSample();
    m_paintNew = false;
    m_paintTarget = 0;
    if ((tool == Tool::RubberStamp || tool == Tool::Healing) && ((m_mods & Qt::AltModifier) || !m_stampSet)) {
        m_stampSrc = d;
        m_stampSet = true;
        m_stampOffsetSet = false;
        m_drag = Drag::None;
        status(QStringLiteral("%3: source set at %1, %2 -- now drag to %4 (Alt-click sets a new source)")
                   .arg(qRound(d.x())).arg(qRound(d.y())).arg(toolName(tool), tool == Tool::Healing ? QStringLiteral("heal") : QStringLiteral("clone")));
        update();
        return;
    }
    VObject *sel = m_doc->singleSelected();
    int layer = -1;
    const bool drawTool = tool == Tool::Brush || tool == Tool::Pencil;
    const int target = drawTool ? appState().strokeTarget : int(AppState::ClipToSelected);
    m_paintGrow = false;
    m_paintClipped = false;
    m_paintGrewOnce = false;
    m_paintViewExtra = QRectF();
    if (target != AppState::NewBitmapPerStroke && sel && sel->type == ObjType::Bitmap && m_doc->find(sel->id, &layer) &&
        !m_doc->s.layers[layer].locked && !sel->locked) {
        if (!m_doc->beginPixelEdit(*sel)) { status(pxedit::blockedMessage(*sel, toolName(tool))); m_drag = Drag::None; return; }
        m_paintTarget = sel->id;
        m_paintGrow = drawTool && target == AppState::ExpandSelected;
        m_paintOrigRect = sel->rect;
    }
    if (!m_paintTarget && (tool == Tool::Eraser || isRetouch(tool))) {
        quint64 h = hitObject(d, true);
        VObject *o = h ? m_doc->find(h) : nullptr;
        if (o && o->type == ObjType::Bitmap) {
            if (!m_doc->beginPixelEdit(*o)) { status(pxedit::blockedMessage(*o, toolName(tool))); m_drag = Drag::None; return; }
            m_paintTarget = h;
        }
        else { status(QStringLiteral("%1: select or click a bitmap object").arg(toolName(tool))); m_drag = Drag::None; return; }
    }
    if (!m_paintTarget) {
        if (m_doc->s.layers[m_doc->drawingLayer()].locked) { status(QStringLiteral("The current layer is locked")); m_drag = Drag::None; return; }
        VObject o;
        o.type = ObjType::Bitmap;
        o.name = QStringLiteral("Bitmap");
        // starts as a single pixel under the pointer and grows with the stroke, so nothing is lost
        // outside any fixed bounds (not even off the canvas)
        o.rect = QRectF(qFloor(d.x()), qFloor(d.y()), 1, 1);
        o.image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
        o.image.fill(Qt::transparent);
        o.stampInterpolation();
        m_paintGrow = true;
        m_paintOrigRect = QRectF();
        o.id = m_doc->newId();
        m_paintTarget = o.id;
        m_paintNew = true;
        m_doc->addObject(o);
        if (VObject *n = m_doc->find(o.id)) m_doc->beginPixelEdit(*n);   // trimmed on commit
    }
    m_drag = Drag::Paint;
    paintinput::begin();
    if (isRetouch(tool)) {
        VObject *o = m_doc->find(m_paintTarget);
        if (o->image.format() != QImage::Format_ARGB32_Premultiplied) o->image = o->image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        m_retouchClip = QImage();
        if (!m_doc->pixelSelection.isEmpty()) {
            QTransform t = QTransform::fromTranslate(-o->rect.left(), -o->rect.top()) *
                           QTransform::fromScale(o->image.width() / qMax(1.0, o->rect.width()), o->image.height() / qMax(1.0, o->rect.height()));
            m_retouchClip = m_doc->selectionMask(t, o->image.size());   // honours the selection's Edge (feather)
        }
        const double scale = o->image.width() / qMax(1.0, o->rect.width());
        if (tool == Tool::RubberStamp || tool == Tool::Healing) {
            if (!m_stampOffsetSet || !appState().stampAligned) { m_stampOffset = m_stampSrc - d; m_stampOffsetSet = true; }
            m_stampSnap = stampSource(*o);
        } else if (tool == Tool::ReplaceColor) {
            const QPointF ip = toImage(*o, d);
            const QPoint px(qFloor(ip.x()), qFloor(ip.y()));
            const QRgb under = o->image.rect().contains(px) ? qUnpremultiply(o->image.pixel(px)) : 0;
            if (m_mods & Qt::AltModifier) {
                // Alt-click: sample the From colour from the image (no paint)
                m_doc->revert();
                m_drag = Drag::None;
                if (qAlpha(under)) {
                    appState().rcFrom = QColor::fromRgb(under);
                    appState().notifyOptions();
                    status(QStringLiteral("Replace Color: From set to %1").arg(appState().rcFrom.name()));
                }
                return;
            }
            const AppState &as = appState();
            fx2::ReplaceColorOpts ro;
            ro.from = (as.rcFromImage && qAlpha(under)) ? under : as.rcFrom.rgba();
            ro.to = as.rcTo.rgba();
            ro.tolerance = as.rcTolerance;
            ro.softness = as.rcPixelPerfect ? 0 : 50;
            ro.range = as.rcRange == 1 ? fx2::RcColor : fx2::RcSolid;
            ro.strength = as.toolOpacity / 100.0;
            ro.hard = as.rcPixelPerfect;
            m_rcStroke.begin(o->image, ro, m_retouchClip);
        } else if (tool == Tool::RedEye) {
            fx2::RedEyeOpts eo;
            eo.tolerance = appState().reTolerance;
            eo.darken = appState().reDarken;
            eo.strength = appState().toolOpacity / 100.0;
            m_reStroke.begin(o->image, eo, m_retouchClip);
        } else if (tool == Tool::Smudge) {
            // Smudge Color starts the stroke with a colour; Use Entire Document picks up all visible layers
            if (appState().smudgeUseColor) m_smudger.startWithColor(appState().smudgeColor, appState().brushSize * scale);
            else m_smudger.start(appState().useEntireDocument ? stampSource(*o) : o->image, toImage(*o, d), appState().brushSize * scale);
        }
        retouchDab(toImage(*o, d), m_lastPen);
        m_lastDoc = d;
        {
            const double pad = qMax(1.0, double(appState().brushSize)) + 4.0;
            m_doc->notifyChanged(QRectF(d, d).adjusted(-pad, -pad, pad, pad));
        }
        m_liveStrokeOverlay = false;
        m_paintFlushClock.start();
        if (m_paintFrame && !m_paintFrame->isActive())
            m_paintFrame->start();
        return;
    }
    // Pencil Left/Right colours: Shift-click draws a straight line from the end of the last stroke
    const bool line = tool == Tool::Pencil && appState().pencilTwoColor && (m_mods & Qt::ShiftModifier) && m_pencilLineValid;
    const QPointF start = line ? m_pencilLineEnd : d;
    m_lastDoc = start;
    beginBrushStroke(start);
    clearLiveStroke();
    m_liveStrokeOverlay = strokeUsesLiveOverlay();
    if (m_paintGrow) {
        // Pre-size cap buffers to the visible document (plus a pad) so a
        // zoom < 1 stroke that covers the window does not realloc 20+ MB
        // mid-frame. Cost sits on the press, not on subsequent paintFrames.
        const int pad = int(appState().brushSize) * 2 + 256;
        // 1.4x the visible document covers a stroke that starts slightly off
        // the widget (50% zoom zigzag was ~100 px wider than width()/zoom).
        const int capW = qBound(512,
            ((qMin(4096, int(std::ceil(double(width()) / qMax(0.05, m_zoom) * 1.4)) + pad) + 255) / 256) * 256,
            4096);
        const int capH = qBound(512,
            ((qMin(4096, int(std::ceil(double(height()) / qMax(0.05, m_zoom) * 1.4)) + pad) + 255) / 256) * 256,
            4096);
        auto reserveStore = [](QImage &store, int w, int h) {
            if (store.width() >= w && store.height() >= h
                && store.format() == QImage::Format_ARGB32_Premultiplied)
                return;
            QImage ns(w, h, QImage::Format_ARGB32_Premultiplied);
            ns.fill(0);
            if (!store.isNull()) {
                QPainter p(&ns);
                p.setCompositionMode(QPainter::CompositionMode_Source);
                p.drawImage(0, 0, store);
            }
            store = ns;
        };
        reserveStore(m_paintStore, capW, capH);
        if (m_liveStrokeOverlay)
            reserveStore(m_liveStore, capW, capH);
        m_brush.reserveCapacity(QSize(capW, capH));
    }
    if (m_liveStrokeOverlay) {
        if (VObject *lo = m_doc->find(m_paintTarget)) {
            growLiveLayer(QPoint(0, 0), lo->image.size());
            if (!m_liveLayer.isNull())
                m_liveLayer.fill(0);
        }
    }
    paintTo(start);   // the first segment only dabs its end point
    if (line) paintTo(d);
    presentLiveFrame();
    m_paintFlushClock.start();
    if (m_paintFrame && !m_paintFrame->isActive())
        m_paintFrame->start();
}

void Canvas::retouchDab(const QPointF &ip, const tablet::Sample &pen) {
    VObject *o = m_doc->find(m_paintTarget);
    if (!o) return;
    const AppState &as = appState();
    const double scale = o->image.width() / qMax(1.0, o->rect.width());
    // pen pressure: the size and the strength (intensity / exposure / opacity), with the Preferences floors
    const double pr = qBound(0.0, pen.pressure, 1.0);
    const double sizeK = as.pressureSize ? tablet::withFloor(pr, as.prefs.tabletMinSize / 100.0) : 1.0;
    const double opK = as.pressureOpacity ? tablet::withFloor(pr, as.prefs.tabletMinOpacity / 100.0) : 1.0;
    const double size = qMax(1.0, as.brushSize * scale * sizeK);
    const QImage *clip = m_retouchClip.isNull() ? nullptr : &m_retouchClip;
    const int k = qMax(1, qRound(as.toolIntensity * opK));
    const int stampOpacity = qMax(1, qRound(as.toolOpacity * opK));
    const retouch::TipScope tipScope(retouch::Tip{as.brushEdge, as.tipShape == 1});
    switch (effectiveTool()) {
    case Tool::Blur: retouch::blurDab(o->image, ip, size, k, clip); break;
    case Tool::Sharpen: retouch::sharpenDab(o->image, ip, size, k, clip); break;
    case Tool::Dodge: retouch::dodgeBurnDab(o->image, ip, size, k, retouch::Range(as.toneRange), false, clip); break;
    case Tool::Burn: retouch::dodgeBurnDab(o->image, ip, size, k, retouch::Range(as.toneRange), true, clip); break;
    case Tool::Smudge: m_smudger.dab(o->image, ip, k, clip); break;
    case Tool::RubberStamp:
        if (as.toolBlend == int(BlendMode::Normal))
            retouch::stampDab(o->image, m_stampSnap, ip, m_stampOffset * scale, size, stampOpacity, clip);
        else
            retouch::stampDabMode(o->image, m_stampSnap, ip, m_stampOffset * scale, size, stampOpacity,
                                  toComposition(BlendMode(qBound(0, as.toolBlend, int(BlendMode::Exclusion)))), clip);
        break;
    case Tool::Healing:
        retouch::healDab(o->image, m_stampSnap, ip, m_stampOffset * scale, size, as.brushEdge, retouch::HealMode(as.healMode), clip);
        break;
    case Tool::SpotHealing: retouch::spotHealDab(o->image, ip, size, as.brushEdge, retouch::HealMode(as.healMode), clip); break;
    case Tool::ReplaceColor:
        m_rcStroke.dab(o->image, ip, as.rcPixelPerfect ? qMax(1.0, std::round(size)) : size, as.brushEdge, as.tipShape == 1, as.rcPixelPerfect);
        break;
    case Tool::RedEye:   // pressure: size (above) and the strength as the dab's coverage
        m_reStroke.dab(o->image, ip, size, as.brushEdge, as.tipShape == 1, opK);
        break;
    default: break;
    }
}

void Canvas::queuePaintPoint(const QPointF &d)
{
    // Capture the sample now: a later flush runs with m_fromTablet false, so
    // penSample() would be the default (and a tablet stroke would lose pressure).
    m_paintQueued.append({d, m_fromTablet ? m_pen : tablet::Sample()});
    // Motion only enqueues while a flush is already running (posted-move drain).
    if (m_inPaintFrame)
        return;
    // Flush now when the new polyline would grow the bitmap (otherwise a
    // mid-stroke grab before the 16 ms timer would still show the old clip)
    // or when a frame budget has elapsed. Otherwise the 16 ms timer folds
    // a high-rate burst into the next paintFrame.
    if (queuedPaintNeedsGrow()
        || queuedPaintNeedsPresent()
        || !m_paintFlushClock.isValid()
        || m_paintFlushClock.elapsed() >= 16)
        flushPaintFrame();
    else if (m_paintFrame && !m_paintFrame->isActive())
        m_paintFrame->start();
}

bool Canvas::queuedPaintNeedsGrow() const
{
    if (!m_paintGrow || m_paintQueued.isEmpty()) return false;
    const VObject *o = m_doc->find(m_paintTarget);
    if (!o || o->image.isNull()) return false;
    QRectF docNeed(m_lastDoc, m_lastDoc);
    for (const QueuedPaint &q : m_paintQueued)
        docNeed = docNeed.united(QRectF(q.doc, q.doc));
    const double pad = qMax(8.0, double(appState().brushSize)) + 32.0;
    docNeed.adjust(-pad, -pad, pad, pad);
    const QRectF need = pxedit::docToImage(*o).mapRect(docNeed);
    if (need.isEmpty()) return false;
    return !QRect(QPoint(0, 0), o->image.size()).contains(need.toAlignedRect());
}

bool Canvas::queuedPaintNeedsPresent() const
{
    // Flush now if the unpainted tail is longer than the cheap QPen
    // approximation. A 16 ms wait after a long jump would leave a visible
    // gap (and mid-stroke tests grab before the timer).
    if (m_paintQueued.isEmpty()) return false;
    const double lim = qMax(16.0, double(appState().brushSize) * 1.25);
    QPointF last = m_lastDoc;
    double acc = 0;
    for (const QueuedPaint &q : m_paintQueued) {
        acc += QLineF(last, q.doc).length();
        last = q.doc;
        if (acc > lim) return true;
    }
    return false;
}

void Canvas::flushPaintFrame(bool drain)
{
    // Fold any MouseMove events already posted to this widget into the queue
    // so one frame sees the whole burst (tablet events are not compressed).
    // Guard first so those deliveries only enqueue (they re-enter here).
    if (m_inPaintFrame)
        return;
    m_inPaintFrame = true;
    if (!drain)
        QCoreApplication::sendPostedEvents(this, QEvent::MouseMove);
    if (m_paintQueued.isEmpty()) {
        m_inPaintFrame = false;
        if (m_paintFrame) m_paintFrame->stop();
        return;
    }
    QVector<QueuedPaint> pts;
    pts.swap(m_paintQueued);
    painttrace::Scope _pt("paintFrame", -1, 0, 0,
        QStringLiteral("queued=%1").arg(pts.size()));
    m_paintFrameDirty = QRectF();
    m_paintFrameFull = false;
    QElapsedTimer budget;
    budget.start();
    // (B) Grow in document space for the whole pending polyline first, so a
    // 1×1 start bitmap cannot collapse toImage() bounds and skip later grows.
    // Do not invalidate the whole view cache: only the grown pad is dirty,
    // and a SmoothPixmapTransform rebuild at zoom < 1 was stalling 50% zoom.
    if (m_paintGrow) {
        if (VObject *o = m_doc->find(m_paintTarget)) {
            QRectF docNeed(m_lastDoc, m_lastDoc);
            for (const QueuedPaint &q : pts) docNeed = docNeed.united(QRectF(q.doc, q.doc));
            const double pad = qMax(8.0, double(appState().brushSize)) + 32.0;
            docNeed.adjust(-pad, -pad, pad, pad);
            const QRectF need = pxedit::docToImage(*o).mapRect(docNeed);
            if (!need.isEmpty())
                ensurePaintTargetFor(need);
        }
    }
    // (A) Time-box raster so a heavy tip cannot stall the event loop.
    // Path-engine paintTo only appends (cheap); dab work and the live present
    // share a 12 ms budget. Leftover dabs stay queued for the next tick.
    // endPaint drains the rest so release cannot drop unpainted points.
    int done = 0;
    for (; done < pts.size(); ++done) {
        m_flushPen = pts[done].pen;
        m_flushPenValid = true;
        paintTo(pts[done].doc);
        m_flushPenValid = false;
        if (!drain && !m_brush.usesEngine() && budget.elapsed() >= 12 && done + 1 < pts.size()) {
            ++done;
            break;
        }
    }
    QVector<QueuedPaint> rest;
    if (done < pts.size()) {
        rest.reserve(pts.size() - done + m_paintQueued.size());
        for (int i = done; i < pts.size(); ++i) rest.append(pts[i]);
        rest.append(m_paintQueued);
        m_paintQueued.swap(rest);
    }
    // Mid-stroke ink for a Normal overlay stroke stays on the live layer.
    // Grow notifications and base-modifying tools (retouch, eraser, blend,
    // preserve-transparency) write the document; apply those dirties after
    // the raster so the view updates mid-stroke without a full cache rebuild.
    if (!m_paintViewExtra.isEmpty()) {
        const QRectF extra = m_paintViewExtra;
        m_paintViewExtra = QRectF();
        m_doc->notifyChanged(extra);
    }
    if (!m_liveStrokeOverlay && m_brush.active() && m_brush.usesEngine())
        flushPaintEngine();
    presentLiveFrame();
    applyPaintFrameDirty();
    m_paintFlushClock.start();
    m_inPaintFrame = false;
    _pt.extra += QStringLiteral(" did=%1 left=%2").arg(done).arg(m_paintQueued.size());
    if (m_paintQueued.isEmpty()) {
        if (m_paintFrame) m_paintFrame->stop();
    } else if (m_paintFrame && !m_paintFrame->isActive()) {
        m_paintFrame->start();
    }
}

QRectF Canvas::liveStrokeDocRect() const
{
    if (m_liveLayer.isNull()) return QRectF();
    const VObject *o = m_doc->find(m_paintTarget);
    if (!o) return QRectF();
    QRect r = m_liveDirty.isEmpty() ? QRect(QPoint(0, 0), m_liveLayer.size()) : m_liveDirty;
    r = r.intersected(QRect(QPoint(0, 0), m_liveLayer.size()));
    if (r.isEmpty()) return QRectF();
    return pxedit::imageToDoc(*o).mapRect(QRectF(r));
}

QRect Canvas::liveStrokeViewRect() const
{
    const QRectF doc = liveStrokeDocRect();
    if (doc.isEmpty()) return QRect();
    const int pad = int(std::ceil(qMax(8.0, double(appState().brushSize) * m_zoom))) + 4;
    return viewRectFromDoc(doc, pad);
}

void Canvas::clearLiveStroke()
{
    m_liveStrokeOverlay = false;
    m_liveLayer = QImage();
    m_liveBase = QImage();
    m_liveCkptN = 0;
    m_liveSuffixAt = 0;
    m_liveDirty = QRect();
    m_liveGen = 0;
    m_liveHasInk = false;
    m_liveJobWanted = false;
    // m_liveStore keeps its capacity for the next stroke.
}

void Canvas::growLiveLayer(QPoint shift, QSize size)
{
    if (size.width() < 1 || size.height() < 1) return;
    const bool had = !m_liveLayer.isNull();
    if (!had && m_drag != Drag::Paint) return;
    const int capW = qMax(m_liveStore.width(), ((qMax(1, size.width()) + 255) / 256) * 256);
    const int capH = qMax(m_liveStore.height(), ((qMax(1, size.height()) + 255) / 256) * 256);
    const QSize old = m_liveLayer.size();
    const bool storeOk = !m_liveStore.isNull()
        && m_liveStore.format() == QImage::Format_ARGB32_Premultiplied
        && m_liveStore.width() >= capW && m_liveStore.height() >= capH;
    if (!storeOk) {
        QImage ns(capW, capH, QImage::Format_ARGB32_Premultiplied);
        ns.fill(0);
        if (had) {
            QPainter p(&ns);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(shift, m_liveLayer);
        }
        m_liveStore = ns;
    } else if (had && shift.isNull() && size.width() >= old.width() && size.height() >= old.height()) {
        // In-place right/bottom pad: fill only the new strips.
        QPainter p(&m_liveStore);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        if (size.width() > old.width())
            p.fillRect(QRect(old.width(), 0, size.width() - old.width(), size.height()), Qt::transparent);
        if (size.height() > old.height())
            p.fillRect(QRect(0, old.height(), qMin(size.width(), old.width()), size.height() - old.height()), Qt::transparent);
    } else if (had) {
        QImage prev = m_liveLayer.copy();
        QPainter p(&m_liveStore);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(QPoint(0, 0), size), Qt::transparent);
        p.drawImage(shift, prev);
    } else {
        QPainter p(&m_liveStore);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(QPoint(0, 0), size), Qt::transparent);
    }
    m_liveLayer = QImage(m_liveStore.bits(), size.width(), size.height(),
                         m_liveStore.bytesPerLine(), QImage::Format_ARGB32_Premultiplied);
    if (!shift.isNull()) {
        ++m_liveGen;
        // Base is stale after a left/up grow; the next worker will replace it.
        if (!m_liveBase.isNull() && (m_liveBase.size() != size || shift != QPoint(0, 0)))
            m_liveBase = QImage();
    }
    m_liveDirty = m_liveDirty.isEmpty() ? QRect(QPoint(0, 0), size)
                                        : m_liveDirty.translated(shift).united(QRect(QPoint(0, 0), size));
}

void Canvas::presentLiveFrame()
{
    if (!m_brush.active() || m_liveLayer.isNull()) {
        revealBrushCursor();
        return;
    }
    // The live layer and the path-engine buffer must cover the grown paint
    // target. Shrinking to a stale engineSize() clipped mid-stroke ink (oil
    // splatters grew the bitmap to 729x261 while the engine stayed 261x261).
    QSize sz = m_brush.engineSize();
    if (const VObject *lo = m_doc->find(m_paintTarget)) {
        const QSize target = lo->image.size();
        if (m_brush.active() && !target.isEmpty() && m_brush.engineSize() != target)
            m_brush.grow(QPoint(0, 0), target);
        sz = m_brush.engineSize().expandedTo(target);
    }
    sz = sz.expandedTo(m_liveLayer.size());
    if (!sz.isEmpty() && m_liveLayer.size() != sz)
        growLiveLayer(QPoint(0, 0), sz);

    if (m_brush.usesEngine()) {
        const int n = m_brush.enginePathCount();
        const QPainterPath &ep = m_brush.enginePath();
        if (n > m_liveSuffixAt && ep.elementCount() > m_liveSuffixAt) {
            // Time-box the UI-thread quality suffix (and cap length) so a
            // flood cannot paint hundreds of px of Alien/Glow here. The last
            // ~1 brush width is a cheap tail until the worker lands.
            // When the frame queued no leftover points, catch up the remaining
            // suffix so mid-stroke matches the brush without waiting for the
            // worker (oil bristle was otherwise ~250 px short of the final).
            const bool catchUp = m_paintQueued.isEmpty();
            const double kMaxQualLen = catchUp ? 1e9 : 160.0;
            const int kMaxQualMs = catchUp ? 10 : 4;
            QElapsedTimer qt;
            qt.start();
            int to = m_liveSuffixAt;
            double acc = 0;
            QPointF prev(ep.elementAt(m_liveSuffixAt).x, ep.elementAt(m_liveSuffixAt).y);
            for (int i = m_liveSuffixAt + 1; i < n; ++i) {
                const QPointF pt(ep.elementAt(i).x, ep.elementAt(i).y);
                acc += QLineF(prev, pt).length();
                prev = pt;
                to = i;
                if (acc >= kMaxQualLen || qt.elapsed() >= kMaxQualMs) break;
            }
            if (to > m_liveSuffixAt) {
                brush::PathSnapshot snap = m_brush.capturePathSnapshot(m_liveSuffixAt, m_liveGen);
                snap.fromIndex = m_liveSuffixAt;
                // Restrict the snapshot path to `to` by cropping via fromIndex..to
                // renderPathLayer paints fromIndex..end; build a cropped snap.
                if (to + 1 < snap.path.elementCount()) {
                    QPainterPath cut;
                    for (int i = 0; i <= to; ++i) {
                        const auto e = snap.path.elementAt(i);
                        const QPointF pt(e.x, e.y);
                        if (i == 0) cut.moveTo(pt);
                        else cut.lineTo(pt);
                    }
                    snap.path = cut;
                    if (snap.dist.size() > to + 1) snap.dist.resize(to + 1);
                    if (snap.pressure.size() > to + 1) snap.pressure.resize(to + 1);
                    if (snap.speed.size() > to + 1) snap.speed.resize(to + 1);
                    if (snap.time.size() > to + 1) snap.time.resize(to + 1);
                    snap.elementCount = to + 1;
                }
                const QImage suf = brush::renderPathLayer(snap);
                if (!suf.isNull()) {
                    QPainter p(&m_liveLayer);
                    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                    p.drawImage(0, 0, suf);
                    m_liveHasInk = true;
                }
                // Last *painted vertex index*, not n. Setting suffixAt to n
                // (one past the last vertex) made the next 1-vertex append
                // skip the suffix loop, leaving mid-stroke gaps between jumps.
                m_liveSuffixAt = to;
            }
            const QRectF pb = m_brush.pendingPaintedBounds();
            if (!pb.isEmpty())
                m_liveDirty = m_liveDirty.united(pb.toAlignedRect());
        }
        startLiveJob();
    }
    // Dab tips already copyStroke each segment in paintTo.

    const QRect vr = liveStrokeViewRect().united(brushCursorViewRect()).united(m_lastCursorView);
    m_lastCursorView = brushCursorViewRect();
    if (vr.isEmpty()) update();
    else update(vr);
}

void Canvas::startLiveJob()
{
    if (!m_brush.active() || !m_brush.usesEngine() || !m_liveJob) return;
    const int n = m_brush.enginePathCount();
    if (n <= 0 || n <= m_liveCkptN) return;
    if (m_liveJob->isRunning()) {
        m_liveJobWanted = true;
        return;
    }
    const brush::PathSnapshot snap = m_brush.capturePathSnapshot(0, m_liveGen);
    m_liveJobWanted = false;
    // setFuture from the watcher's finished() slot is unsafe; defer only then.
    auto launch = [this, snap]() {
        if (!m_liveJob || m_liveJob->isRunning()) {
            m_liveJobWanted = true;
            return;
        }
        if (!m_brush.active() || !m_brush.usesEngine()) return;
        m_liveJob->setFuture(QtConcurrent::run([snap]() {
            LiveJobResult r;
            r.layer = brush::renderPathLayer(snap);
            r.elementCount = snap.elementCount;
            r.size = snap.size;
            r.gen = snap.gen;
            return r;
        }));
    };
    if (m_applyingLiveJob)
        QTimer::singleShot(0, this, launch);
    else
        launch();
}

void Canvas::applyLiveJob()
{
    if (!m_liveJob || !m_liveJob->isFinished()) return;
    // A watcher that has never been given a future is "finished" with no
    // results; result() would dereference a null d-pointer.
    if (m_liveJob->future().resultCount() < 1) return;
    if (!m_doc || m_liveLayer.isNull()) return;
    const LiveJobResult r = m_liveJob->result();
    const bool sizeOk = !r.layer.isNull()
        && r.gen == m_liveGen
        && r.layer.width() <= m_liveLayer.width()
        && r.layer.height() <= m_liveLayer.height();
    m_applyingLiveJob = true;
    if (sizeOk && m_brush.active() && !m_liveLayer.isNull()) {
        auto adoptBase = [&]() {
            if (r.layer.size() == m_liveLayer.size()) {
                m_liveBase = r.layer;
            } else {
                QImage nb(m_liveLayer.size(), QImage::Format_ARGB32_Premultiplied);
                nb.fill(0);
                QPainter p(&nb);
                p.setCompositionMode(QPainter::CompositionMode_Source);
                p.drawImage(0, 0, r.layer);
                p.end();
                m_liveBase = nb;
            }
        };
        if (m_liveSuffixAt > r.elementCount) {
            // The UI has already painted a quality suffix past this job.
            // A Source blit of the shorter result would wipe those pixels
            // and leave mid-stroke gaps until a later job lands.
            if (r.elementCount >= m_liveCkptN)
                adoptBase();
            m_liveCkptN = qMax(m_liveCkptN, r.elementCount);
            m_liveHasInk = true;
            if (m_brush.enginePathCount() > r.elementCount)
                m_liveJobWanted = true;
        } else {
            adoptBase();
            m_liveCkptN = r.elementCount;
            // Blit the worker result into the capacity store (do not throw it away).
            if (m_liveStore.isNull() || m_liveStore.width() < m_liveLayer.width()
                || m_liveStore.height() < m_liveLayer.height()
                || m_liveStore.format() != QImage::Format_ARGB32_Premultiplied)
                growLiveLayer(QPoint(0, 0), m_liveBase.size().expandedTo(m_liveLayer.size()));
            {
                QPainter p(&m_liveStore);
                p.setCompositionMode(QPainter::CompositionMode_Source);
                p.drawImage(0, 0, m_liveBase);
            }
            m_liveLayer = QImage(m_liveStore.bits(), m_liveLayer.width(), m_liveLayer.height(),
                                 m_liveStore.bytesPerLine(), QImage::Format_ARGB32_Premultiplied);
            // Last painted vertex index, not the count (see presentLiveFrame).
            m_liveSuffixAt = qMax(0, r.elementCount - 1);
            m_liveDirty = m_liveLayer.rect();
            m_liveHasInk = true;
            presentLiveFrame();
        }
    } else {
        m_liveJobWanted = m_brush.active() && m_brush.usesEngine();
    }
    if (m_liveJobWanted && m_drag == Drag::Paint) {
        m_liveJobWanted = false;
        startLiveJob();
    }
    m_applyingLiveJob = false;
}

void Canvas::waitLiveStroke()
{
    // Drain queued points so the worker (and the caller) see the current path.
    flushPaintFrame(true);
    if (!m_liveJob) return;
    if (m_liveJobWanted && !m_liveJob->isRunning())
        startLiveJob();
    // Mid-stroke tests (and endPaint reuse) must still kick a job when the
    // path has moved past the last applied checkpoint.
    if (m_brush.active() && m_brush.usesEngine()
        && m_brush.enginePathCount() > m_liveCkptN && !m_liveJob->isRunning())
        startLiveJob();
    if (m_liveJob->isRunning())
        m_liveJob->waitForFinished();
    if (m_liveJob->isFinished() && m_liveJob->future().resultCount() >= 1)
        applyLiveJob();
}

void Canvas::paintTo(const QPointF &d) {
    painttrace::Scope _pt("paintTo", -1, d.x(), d.y());
    ++painttrace::paintToCount();
    VObject *o = m_doc->find(m_paintTarget);
    if (!o) return;
    if (o->image.format() != QImage::Format_ARGB32_Premultiplied) o->image = o->image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    Tool tool = effectiveTool();
    if (m_pp) m_ppPts << d;
    if (isRetouch(tool)) {
        const AppState &as = appState();
        const double scale = o->image.width() / qMax(1.0, o->rect.width());
        const tablet::Sample cur = penSample();
        // pressure interpolated along the segment; Flow: lighter pressure spaces the dabs further apart
        const double pMid = (m_lastPen.pressure + cur.pressure) / 2;
        const double sizeK = as.pressureSize ? tablet::withFloor(qBound(0.0, pMid, 1.0), as.prefs.tabletMinSize / 100.0) : 1.0;
        const double flowK = as.pressureFlow ? qMax(0.125, tablet::withFloor(qBound(0.0, pMid, 1.0), as.prefs.tabletMinOpacity / 100.0)) : 1.0;
        const double spacing = qMax(1.0, as.brushSize * scale * sizeK / 4.0 / flowK);
        const QPointF docA = m_lastDoc;
        const QPointF ia = toImage(*o, m_lastDoc), ib = toImage(*o, d);
        const double len = QLineF(ia, ib).length();
        for (const QPointF &ip : retouch::dabsAlong(ia, ib, spacing))
            retouchDab(ip, tablet::lerp(m_lastPen, cur, len > 1e-9 ? QLineF(ia, ip).length() / len : 1.0));
        m_lastPen = cur;
        m_lastDoc = d;
        // 0.3.15: dirty-rect around the segment tip path
        {
            const double pad = qMax(1.0, double(as.brushSize)) + 4.0;
            const QRectF docDirty = QRectF(docA, d).normalized().adjusted(-pad, -pad, pad, pad);
            m_doc->notifyChanged(docDirty);
        }
        return;
    }
    bool grew = false;
    {
        const QRectF need = m_brush.paintedBounds(toImage(*o, m_lastDoc), toImage(*o, d));
        const QRectF area = pxedit::imageToDoc(*o).mapRect(need);
        if (m_paintGrow) {
            // Grow to the segment's full painted bounds (radius, spatter, glow,
            // blur, texture pad) before any paint, so the live stroke is never
            // clipped to the previous bitmap size.
            grew = ensurePaintTargetFor(need);
            o = m_doc->find(m_paintTarget);
            if (!o) return;
            if (grew)
                _pt.extra = QStringLiteral("grew=1 img=%1x%2").arg(o->image.width()).arg(o->image.height());
        } else if (!o->rect.contains(area)) {
            m_paintClipped = true;
        }
    }
    const tablet::Sample cur = penSample();
    const QRect dirty = m_brush.segment(toImage(*o, m_lastDoc), toImage(*o, d), m_lastPen, cur);
    m_lastPen = cur;
    m_lastDoc = d;
    // Path-engine tips defer the (potentially full) paint until the end of this
    // event-loop turn so a tablet burst is processed once per frame, not once
    // per event. Dab-engine segment() still paints immediately and returns dirty.
    // If this event grew the bitmap, flush immediately: a paintEvent between
    // the realloc and the 0-timer would otherwise show an expanded empty pad
    // with the stroke still clipped at the previous bounds.
    if (m_liveStrokeOverlay && !m_liveLayer.isNull()) {
        // Live-layer stroke: keep the base bitmap untouched; dabs are copied
        // incrementally here (small dirty rects). Path-engine suffixes are
        // painted in presentLiveFrame.
        if (!dirty.isEmpty()) {
            if (!m_brush.usesEngine()) {
                m_brush.copyStroke(m_liveLayer, dirty, true);
                m_liveHasInk = true;
            }
            m_liveDirty = m_liveDirty.united(dirty);
        }
        return;
    }
    if (m_brush.engineDeferred() && dirty.isEmpty()) {
        if (m_inPaintFrame)
            return;
        if (grew) {
            flushPaintEngine();
            return;
        }
        if (!m_paintFlushScheduled) {
            m_paintFlushScheduled = true;
            QTimer::singleShot(0, this, [this]() { flushPaintEngine(); });
        }
        return;
    }
    m_brush.composite(o->image, dirty);
    // 0.3.15: dirty-rect canvas update — only the dab neighbourhood, not the whole frame
    if (!dirty.isEmpty()) {
        const QTransform toDocXf = pxedit::imageToDoc(*o);
        const QRectF docDirty = toDocXf.mapRect(QRectF(dirty)).adjusted(-2, -2, 2, 2);
        m_doc->notifyChanged(docDirty);
    } else {
        m_doc->notifyChanged();
    }
}

void Canvas::flushPaintEngine()
{
    painttrace::Scope _pt("flush", -1, 0, 0);
    m_paintFlushScheduled = false;
    if (!m_brush.active()) return;
    VObject *o = m_doc->find(m_paintTarget);
    if (!o) return;
    if (o->image.format() != QImage::Format_ARGB32_Premultiplied)
        o->image = o->image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (m_paintGrow) {
        const QRectF need = m_brush.pendingPaintedBounds();
        if (!need.isEmpty()) {
            ensurePaintTargetFor(need);
            o = m_doc->find(m_paintTarget);
            if (!o) return;
        }
    }
    const QRect dirty = m_brush.flushEngine();
    if (dirty.isEmpty() && m_paintViewExtra.isEmpty()) return;
    if (!dirty.isEmpty())
        m_brush.composite(o->image, dirty);
    const QTransform toDocXf = pxedit::imageToDoc(*o);
    QRectF docDirty = toDocXf.mapRect(QRectF(dirty)).adjusted(-4, -4, 4, 4);
    if (!m_paintViewExtra.isEmpty()) {
        docDirty = docDirty.united(m_paintViewExtra);
        m_paintViewExtra = QRectF();
    }
    m_doc->notifyChanged(docDirty);
}

// Press inside the bitmap selection: start dragging the outline (pixels = false) or the pixels
// under it, cut out of their bitmap (duplicate: copied). False when the press is outside.
bool Canvas::beginSelectionMove(const QPointF &d, bool pixels, bool duplicate) {
    if (m_noSelMove || m_doc->pixelSelection.isEmpty() || !m_doc->selectionContains(d)) return false;
    if (!pixels && (m_mods & (Qt::ShiftModifier | Qt::AltModifier))) return false;   // add / subtract instead
    m_selMoveTotal = QPointF();
    m_selMoveApplied = QPoint();
    m_floatId = 0;
    if (pixels) {
        const QRect sel = m_doc->pixelSelectionBounds();
        VObject *src = nullptr;
        auto ok = [&](VObject *o) {
            if (o && o->type == ObjType::Bitmap && o->hasXform() && o->visible && !o->locked && o->bounds().toAlignedRect().intersects(sel)) xf::bakeBitmap(*o);
            return o && o->type == ObjType::Bitmap && !o->image.isNull() && o->visible && !o->locked &&
                   o->rect.toAlignedRect().intersects(sel) && o->image.size() == o->rect.size().toSize();
        };
        for (VObject *o : m_doc->selectedEditable()) if (ok(o)) { src = o; break; }
        if (!src) {
            auto &objs = m_doc->objects(m_doc->drawingLayer());
            for (int i = objs.size() - 1; i >= 0 && !src; --i) if (ok(&objs[i])) src = &objs[i];
        }
        if (!src) {
            status(QStringLiteral("Move pixels: there is no unscaled bitmap under the selection"));
            return true;   // swallow the press: nothing to move
        }
        if (!m_doc->beginPixelEdit(*src)) { status(pxedit::blockedMessage(*src, QStringLiteral("Move pixels"))); return true; }
        if (src->image.format() != QImage::Format_ARGB32_Premultiplied) src->image = src->image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        m_floatId = src->id;
        m_floatRect = QRect(src->rect.topLeft().toPoint(), src->image.size());
        src->rect = QRectF(m_floatRect);
        m_floatSel = sel;
        const QImage mask = m_doc->canvasSelectionMask(sel);
        m_floatPixels = QImage(sel.size(), QImage::Format_ARGB32_Premultiplied);
        m_floatPixels.fill(Qt::transparent);
        {
            QPainter p(&m_floatPixels);
            p.drawImage(m_floatRect.topLeft() - sel.topLeft(), src->image);
            p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            p.drawImage(0, 0, mask);
        }
        m_floatHole = src->image;
        if (!duplicate) {
            QPainter p(&m_floatHole);
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            p.drawImage(sel.topLeft() - m_floatRect.topLeft(), mask);
        }
        src->image = m_floatHole;
        QPainter p(&src->image);
        p.drawImage(sel.topLeft() - m_floatRect.topLeft(), m_floatPixels);
    }
    m_drag = Drag::SelMove;
    return true;
}

void Canvas::selectionMoveTo(const QPointF &d) {
    m_selMoveTotal = d - m_pressDoc;
    const QPoint off(qRound(m_selMoveTotal.x()), qRound(m_selMoveTotal.y()));
    if (off == m_selMoveApplied) return;
    m_doc->moveSelection(QPointF(off - m_selMoveApplied));
    m_selMoveApplied = off;
    if (VObject *o = m_floatId ? m_doc->find(m_floatId) : nullptr) {
        // the bitmap grows to hold the pixels wherever they are dropped
        const QRect dst = m_floatSel.translated(off);
        const QRect r = m_floatRect.united(dst);
        QImage img(r.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.drawImage(m_floatRect.topLeft() - r.topLeft(), m_floatHole);
        p.drawImage(dst.topLeft() - r.topLeft(), m_floatPixels);
        p.end();
        o->image = img;
        o->rect = QRectF(r);
    }
    m_doc->notifyChanged();
    update();
}

// Alpha8 mask of the pixel selection in `o`'s image space (null when nothing is selected)
QImage Canvas::paintClip(const VObject &o) const {
    if (m_doc->pixelSelection.isEmpty()) return QImage();
    return m_doc->selectionMask(pxedit::docToImage(o), o.image.size());
}

// Start a Brush / Pencil / Eraser stroke on the paint target with the tool's options.
void Canvas::beginBrushStroke(const QPointF &d) {
    VObject *o = m_doc->find(m_paintTarget);
    if (!o) return;
    if (o->image.format() != QImage::Format_ARGB32_Premultiplied) o->image = o->image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const AppState &as = appState();
    const Tool tool = effectiveTool();
    const double scale = o->image.width() / qMax(1.0, o->rect.width());
    brush::Params bp;
    bp.size = qMax(1.0, as.brushSize * scale);
    bp.edge = as.brushEdge;
    bp.square = as.tipShape == 1;
    bp.opacity = as.toolOpacity;
    bp.blend = BlendMode(qBound(0, as.toolBlend, int(BlendMode::Exclusion)));
    bp.color = as.stroke;
    bp.preserveTransparency = as.preserveTransparency && tool != Tool::Eraser;
    if (tool == Tool::Brush) {
        const auto cat = brush::Category(qBound(0, as.brushCategory, int(brush::Category::Libreworks)));
        brush::applyTip(bp, cat, as.brushTip);   // 0.3.6: tip within the category
        if (!as.brushTexture.isEmpty()) { bp.texture = as.brushTexture; bp.textureAmount = as.brushTextureAmount; }
        else if (bp.textureAmount == 0) bp.textureAmount = as.brushTextureAmount;
        bp.size = qMax(1.0, as.brushSize * scale);   // tip must not override the size the user set
        bp.edge = as.brushEdge;
        bp.square = as.tipShape == 1;
        bp.opacity = as.toolOpacity;
        bp.blend = BlendMode(qBound(0, as.toolBlend, int(BlendMode::Exclusion)));
        bp.color = as.stroke;
        bp.preserveTransparency = as.preserveTransparency;
        const QString tipKey = stroke::canonicalKey(bp.strokeKey);
        auto it = as.brushStrokeByTip.constFind(tipKey);
        if (it != as.brushStrokeByTip.cend()) {
            bp.strokeCustom = true;
            bp.strokeParams = it.value();
        }
    } else if (tool == Tool::Pencil) {
        bp.pencil = true;
        bp.edge = 0;
        bp.antialias = as.antialias;
        // Left/Right colours: the button picks the colour; a transparent one erases to alpha 0
        const QColor two = as.pencilColor(m_paintButton == Qt::RightButton);
        const QPoint ip = toImage(*o, d).toPoint();
        if (as.pencilTwoColor) {
            if (two.alpha() == 0) {
                bp.erase = true;
                bp.blend = BlendMode::Normal;
                bp.preserveTransparency = false;
            } else {
                bp.color = two;
            }
        } else if (as.autoErase && o->image.rect().contains(ip)) {
            // Auto Erase: starting on a pixel of the stroke colour draws in the fill colour instead
            const QColor under = o->image.pixelColor(ip);
            if (under.alpha() == 255 && under.rgb() == as.stroke.rgb()) bp.color = as.fill;
        }
    } else if (tool == Tool::Eraser) {
        bp.erase = true;
        bp.blend = BlendMode::Normal;
    }
    // tablet: what the pressure changes for this tool (the Pencil: its size only), the Preferences floors,
    // and tilt / rotation for a calligraphic nib
    bp.pressure = (as.pressureSize ? brush::PressureSize : 0) |
                  (tool != Tool::Pencil && as.pressureOpacity ? brush::PressureOpacity : 0) |
                  (tool != Tool::Pencil && as.pressureFlow ? brush::PressureFlow : 0);
    bp.minSize = as.prefs.tabletMinSize / 100.0;
    bp.minOpacity = as.prefs.tabletMinOpacity / 100.0;
    bp.tilt = tool == Tool::Brush && as.tiltAngle;
    m_brush.begin(o->image, bp);
    m_brush.setEngineDeferred(true);
    m_brush.setClip(paintClip(*o));
    {
        const stroke::Params sp = bp.strokeCustom ? bp.strokeParams : stroke::paramsFor(bp.strokeKey);
        if (sp.look == stroke::Look::SprayCan && sp.sprayLook >= 1)
            startSprayDwell();
        else
            stopSprayDwell();
    }
    m_brushSize0 = o->image.size();
    m_paintFlushScheduled = false;
    m_pp = tool == Tool::Pencil && as.pixelPerfect && !as.antialias && bp.size <= 1.5;
    m_ppPts.clear();
    if (m_pp) {
        m_ppBase = o->image.copy();
        m_ppBaseRect = o->rect;
        m_ppPts << d;
        m_ppColor = bp.color;
        m_ppErase = bp.erase;
    }
}

// Re-plot a 1 px pencil stroke from its points with the L-corners removed (Aseprite-style).
void Canvas::finishPixelPerfect(VObject *o) {
    m_pp = false;
    if (!o || m_ppPts.isEmpty()) return;
    QVector<QPoint> seq;
    auto px = [&](const QPointF &d) { const QPointF ip = toImage(*o, d); return QPoint(qFloor(ip.x()), qFloor(ip.y())); };
    QPoint prev = px(m_ppPts.first());
    seq << prev;
    for (int i = 1; i < m_ppPts.size(); ++i) {
        const QPoint cur = px(m_ppPts[i]);
        if (cur == prev) continue;
        const QVector<QPoint> line = fx2::linePixels(prev, cur);
        for (int k = 1; k < line.size(); ++k) seq << line[k];
        prev = cur;
    }
    seq = fx2::pixelPerfect(seq);
    const AppState &as = appState();
    QImage img(o->image.size(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const QPointF btl = toImage(*o, m_ppBaseRect.topLeft());
    const QPoint off(qRound(btl.x()), qRound(btl.y()));
    {
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(off, m_ppBase);
    }
    const QImage clip = paintClip(*o);
    const QImage base = img.copy();
    QPainter p(&img);
    p.setOpacity(as.toolOpacity / 100.0);
    p.setCompositionMode(m_ppErase ? QPainter::CompositionMode_DestinationOut
                                   : toComposition(BlendMode(qBound(0, as.toolBlend, int(BlendMode::Exclusion)))));
    QSet<quint64> done;
    for (const QPoint &q : seq) {
        if (!img.rect().contains(q)) continue;
        const quint64 key = (quint64(quint32(q.x())) << 32) | quint32(q.y());
        if (done.contains(key)) continue;   // a stroke crossing itself paints each pixel once
        done.insert(key);
        if (!clip.isNull() && qAlpha(clip.pixel(q)) < 128) continue;
        if (m_ppErase) { p.fillRect(QRect(q, QSize(1, 1)), Qt::black); continue; }
        if (as.preserveTransparency && qAlpha(base.pixel(q)) == 0) continue;
        p.fillRect(QRect(q, QSize(1, 1)), m_ppColor);
    }
    p.end();
    o->image = img;
    m_ppBase = QImage();
}

// Grow the paint target (keeping its pixels and position, transparent padding) so that
// docArea lies inside it. Grows in chunks so a long stroke does not reallocate every dab;
// endPaint trims the padding again.
QPoint Canvas::growPaintTarget(VObject *o, const QRectF &docArea) {
    const double sx = o->rect.width() > 0 ? o->image.width() / o->rect.width() : 1;
    const double sy = o->rect.height() > 0 ? o->image.height() / o->rect.height() : 1;
    const QPointF tl = toImage(*o, docArea.topLeft()), br = toImage(*o, docArea.bottomRight());
    QRect need(QPoint(qFloor(tl.x()), qFloor(tl.y())), QPoint(qCeil(br.x()), qCeil(br.y())));
    const QRect cur(QPoint(0, 0), o->image.size());
    if (cur.contains(need)) return {};
    // First growth of a stroke uses a tight 64 px pad so the opening frame
    // does not allocate a 256-aligned buffer on every side. Further growth
    // is in 256 px chunks, reused from m_paintStore when it already fits.
    const int step = m_paintGrewOnce ? 256 : 64;
    m_paintGrewOnce = true;
    auto chunk = [&](int overflow) -> int {
        if (overflow <= 0) return 0;
        return ((overflow + step - 1) / step) * step;
    };
    QRect u = cur.united(need);
    if (need.left() < cur.left()) u.setLeft(u.left() - chunk(cur.left() - need.left()));
    if (need.top() < cur.top()) u.setTop(u.top() - chunk(cur.top() - need.top()));
    if (need.right() > cur.right()) u.setRight(u.right() + chunk(need.right() - cur.right()));
    if (need.bottom() > cur.bottom()) u.setBottom(u.bottom() + chunk(need.bottom() - cur.bottom()));
    const QSize sz = u.size();
    const int capW = qMax(m_paintStore.width(), ((qMax(1, sz.width()) + 255) / 256) * 256);
    const int capH = qMax(m_paintStore.height(), ((qMax(1, sz.height()) + 255) / 256) * 256);
    const bool bound = m_paintStoreBound && !m_paintStore.isNull()
        && o->image.constBits() == m_paintStore.constBits();
    const bool storeFits = bound
        && m_paintStore.format() == QImage::Format_ARGB32_Premultiplied
        && m_paintStore.width() >= sz.width() && m_paintStore.height() >= sz.height();
    if (m_paintStore.width() < capW || m_paintStore.height() < capH
        || m_paintStore.format() != QImage::Format_ARGB32_Premultiplied) {
        QImage ns(capW, capH, QImage::Format_ARGB32_Premultiplied);
        ns.fill(Qt::transparent);
        {
            QPainter p(&ns);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(-u.topLeft(), o->image);
        }
        m_paintStore = ns;
    } else if (storeFits && u.topLeft() == QPoint(0, 0)) {
        QPainter p(&m_paintStore);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        if (sz.width() > cur.width())
            p.fillRect(QRect(cur.width(), 0, sz.width() - cur.width(), sz.height()), Qt::transparent);
        if (sz.height() > cur.height())
            p.fillRect(QRect(0, cur.height(), qMin(sz.width(), cur.width()), sz.height() - cur.height()), Qt::transparent);
    } else if (storeFits) {
        // Left/up growth inside an already-reserved store: move the used
        // pixels only, do not fill the entire capacity.
        QImage used(o->image.bits(), cur.width(), cur.height(),
                    m_paintStore.bytesPerLine(), QImage::Format_ARGB32_Premultiplied);
        const QImage old = used.copy();
        QPainter p(&m_paintStore);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(QPoint(0, 0), sz), Qt::transparent);
        p.drawImage(-u.topLeft(), old);
    } else {
        QImage old = o->image.copy();
        QPainter p(&m_paintStore);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(QPoint(0, 0), sz), Qt::transparent);
        p.drawImage(-u.topLeft(), old);
    }
    o->image = QImage(m_paintStore.bits(), sz.width(), sz.height(),
                      m_paintStore.bytesPerLine(), QImage::Format_ARGB32_Premultiplied);
    m_paintStoreBound = true;
    o->rect = QRectF(o->rect.left() + u.left() / sx, o->rect.top() + u.top() / sy, u.width() / sx, u.height() / sy);
    return -u.topLeft();
}

void Canvas::detachPaintStore(VObject *o)
{
    if (!m_paintStoreBound) return;
    if (o && !o->image.isNull() && !m_paintStore.isNull()
        && o->image.constBits() == m_paintStore.constBits())
        o->image = o->image.copy();
    m_paintStoreBound = false;
}

bool Canvas::ensurePaintTargetFor(const QRectF &imageBounds)
{
    VObject *o = m_doc->find(m_paintTarget);
    if (!o || !m_paintGrow || imageBounds.isEmpty()) return false;
    const QRectF oldRect = o->rect;
    const QSize oldSize = o->image.size();
    const QRectF area = pxedit::imageToDoc(*o).mapRect(imageBounds);
    const QPoint shift = growPaintTarget(o, area);
    o = m_doc->find(m_paintTarget);
    if (!o) return false;
    const bool grew = o->image.size() != oldSize || !shift.isNull() || o->rect != oldRect;
    if (grew) {
        m_paintViewExtra = m_paintViewExtra.united(oldRect).united(o->rect).adjusted(-2, -2, 2, 2);
        if (m_brush.active() && (o->image.size() != m_brushSize0 || !shift.isNull())) {
            m_brush.grow(shift, o->image.size());
            m_brush.setClip(paintClip(*o));
            m_brushSize0 = o->image.size();
        }
        growLiveLayer(shift, o->image.size());
    }
    return grew;
}

void Canvas::startSprayDwell()
{
    if (m_sprayDwell && !m_sprayDwell->isActive())
        m_sprayDwell->start();
}
void Canvas::stopSprayDwell()
{
    if (m_sprayDwell && m_sprayDwell->isActive())
        m_sprayDwell->stop();
}

void Canvas::endPaint() {
    painttrace::Scope _pt("endPaint", -1, 0, 0, QStringLiteral("paintTos=%1").arg(painttrace::paintToCount()));
    if (m_paintFrame) m_paintFrame->stop();
    flushPaintFrame(true);
    stopSprayDwell();
    paintinput::end();
    Tool tool = effectiveTool();
    if (VObject *o = m_doc->find(m_paintTarget)) {
        if (m_brush.active()) {
            // Finish may paint a slightly larger halo than the last flushed
            // dirty; grow first so the committed stroke is not clipped either.
            if (m_paintGrow)
                ensurePaintTargetFor(m_brush.pendingPaintedBounds());
            o = m_doc->find(m_paintTarget);
            QRect dirty;
            if (m_brush.usesEngine()) {
                // Reuse a current worker result; otherwise one full-path paint.
                waitLiveStroke();
                if (m_liveCkptN < m_brush.enginePathCount()) {
                    startLiveJob();
                    waitLiveStroke();
                }
                if (m_liveCkptN == m_brush.enginePathCount() && !m_liveBase.isNull()
                    && m_liveBase.size() == o->image.size()) {
                    dirty = m_brush.adoptEngineLayer(m_liveBase);
                    m_brush.finish();
                } else {
                    dirty = m_brush.repaintEngineFull();
                    m_brush.finish();
                }
            } else {
                dirty = m_brush.finish();
                // Dab finish() is empty because mid-stroke already stamped the
                // live layer. Only blit when that layer actually has ink, or a
                // ClipToSelected miss would rewrite the QImage and record a
                // phantom undo step.
                if (dirty.isEmpty() && m_liveHasInk)
                    dirty = QRect(QPoint(0, 0), o->image.size());
            }
            // Skip the blit when finish() is empty: a no-op (e.g. ClipToSelected far
            // outside the bitmap) must not rewrite the QImage, or cacheKey-based
            // historyDigest would record a phantom undo step.
            if (!dirty.isEmpty()) {
                m_brush.composite(o->image, dirty);
                const QRectF docDirty = pxedit::imageToDoc(*o).mapRect(QRectF(dirty)).adjusted(-2, -2, 2, 2);
                m_doc->notifyChanged(docDirty);
            }
        }
    }
    clearLiveStroke();
    m_brush.end();
    m_rcStroke.end();
    m_reStroke.end();
    detachPaintStore(m_doc->find(m_paintTarget));
    if (m_pp) finishPixelPerfect(m_doc->find(m_paintTarget));
    if (tool == Tool::Pencil) { m_pencilLineEnd = m_lastDoc; m_pencilLineValid = true; }
    if (VObject *o = m_doc->find(m_paintTarget)) {
        // a new bitmap that received no pixels is dropped; everything else is trimmed to its
        // pixels by commit() (Document::endPixelEdits), in the same undo step
        if (m_paintNew && pxedit::isTransparent(*o)) {
            m_doc->revert();
            m_drag = Drag::None;
            return;
        }
    }
    if (m_paintClipped)
        status(QStringLiteral("%1: the stroke was clipped to the selected bitmap (Stroke target: Paint into selected bitmap only)").arg(toolName(tool)));
    m_doc->commit(toolName(tool));
    if (const QString n = m_doc->takePixelEditNote(); !n.isEmpty()) status(n);
}

// ---------------------------------------------------------------- mouse
// Grid lines, drawn only over the visible part of the canvas. When a cell would be smaller than
// 4 screen pixels the grid is thinned out to every k-th line (fainter), so a 1 px grid shows as
// a pixel grid from 400% up and never turns the canvas into a solid wash.
void Canvas::drawGrid(QPainter &p) {
    const GridSettings &g = m_doc->grid;
    const DocState &s = m_doc->s;
    const QRectF vis = QRectF(toDoc(QPointF(0, 0)), toDoc(QPointF(width(), height()))).intersected(QRectF(0, 0, s.width, s.height));
    if (vis.isEmpty()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    const QPointF tl = toView(vis.topLeft()), br = toView(vis.bottomRight());
    for (int axis = 0; axis < 2; ++axis) {
        const int step = axis == 0 ? g.x : g.y;
        const double cell = step * m_zoom;
        int k = 1;
        QColor c = g.color;
        if (cell < 4) { k = int(std::ceil(12.0 / cell)); c.setAlphaF(c.alphaF() * 0.6); }
        const int every = step * k;
        const double lo = axis == 0 ? vis.left() : vis.top(), hi = axis == 0 ? vis.right() : vis.bottom();
        const int extent = axis == 0 ? s.width : s.height;
        QVector<QLineF> lines;
        for (qint64 v = qMax<qint64>(every, qint64(std::ceil(lo / every)) * every); v <= hi && v < extent; v += every) {
            const double pos = std::round(axis == 0 ? toView(QPointF(v, 0)).x() : toView(QPointF(0, v)).y());
            lines << (axis == 0 ? QLineF(pos, tl.y(), pos, br.y()) : QLineF(tl.x(), pos, br.x(), pos));
        }
        p.setPen(QPen(c, 0));
        p.drawLines(lines);
    }
    p.restore();
}

void Canvas::drawGuides(QPainter &p, const QRectF &cr) {
    const GuideSettings &g = m_doc->guides;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    const QRectF area = cr.united(QRectF(rect()));
    auto line = [&](bool vert, double pos) {
        const QPointF v = toView(QPointF(pos, pos));
        if (vert) p.drawLine(QLineF(std::round(v.x()), area.top(), std::round(v.x()), area.bottom()));
        else p.drawLine(QLineF(area.left(), std::round(v.y()), area.right(), std::round(v.y())));
    };
    if (g.show) {
        p.setPen(QPen(g.color, 0));
        for (int i = 0; i < g.v.size(); ++i) if (!(m_guideDrag && m_guideVert && m_guideIndex == i)) line(true, g.v[i]);
        for (int i = 0; i < g.h.size(); ++i) if (!(m_guideDrag && !m_guideVert && m_guideIndex == i)) line(false, g.h[i]);
    }
    if (m_guideDrag) {
        p.setPen(QPen(g.color, 0, Qt::DashLine));
        line(m_guideVert, m_guidePos);
    }
    p.restore();
}

void Canvas::smartTargets() const {
    if (m_tgtValid) return;
    m_tgtX.clear(); m_tgtY.clear();
    const Prefs &pr = appState().prefs;
    if (pr.snapToCanvas) {
        const double w = m_doc->s.width, h = m_doc->s.height;
        m_tgtX << 0 << w / 2 << w;
        m_tgtY << 0 << h / 2 << h;
    }
    if (pr.snapToObjects) {
        for (int l = 0; l < m_doc->s.layers.size(); ++l) {
            if (!m_doc->s.layers[l].visible) continue;
            for (const VObject &o : m_doc->objects(l)) {
                if (!o.visible || m_doc->selection.contains(o.id)) continue;
                const QRectF b = o.bounds();
                if (b.isEmpty() && b.width() <= 0 && b.height() <= 0) continue;
                m_tgtX << b.left() << b.center().x() << b.right();
                m_tgtY << b.top() << b.center().y() << b.bottom();
            }
        }
    }
    m_tgtValid = true;
}

double Canvas::snapCoord(double v, bool xAxis, double *moved, bool smart, double *line) const {
    if (line) *line = qQNaN();
    if (m_noSnap) { if (moved) *moved = qInf(); return v; }
    const double thr = qMax(8.0 / m_zoom, 0.5);
    double best = v, bestD = thr + 1e-9;
    bool hit = false;
    if (m_doc->grid.snap) {
        const int g = xAxis ? m_doc->grid.x : m_doc->grid.y;
        const double c = std::round(v / g) * g;
        if (qAbs(c - v) <= bestD) { best = c; bestD = qAbs(c - v); hit = true; }
    }
    if (m_doc->guides.snap && m_doc->guides.show)
        for (double gpos : xAxis ? m_doc->guides.v : m_doc->guides.h)
            if (qAbs(gpos - v) < bestD || (!hit && qAbs(gpos - v) <= bestD)) { best = gpos; bestD = qAbs(gpos - v); hit = true; }
    if (smart) {
        // canvas edges / centre and other objects: a fixed distance on screen, whatever the zoom
        smartTargets();
        const double sthr = kSmartSnapPx / m_zoom;
        for (double tpos : xAxis ? m_tgtX : m_tgtY) {
            const double dd = qAbs(tpos - v);
            if (dd <= sthr + 1e-9 && (dd < bestD || (!hit && dd <= bestD) || (line && qIsNaN(*line) && dd <= bestD))) {
                best = tpos; bestD = dd; hit = true;
                if (line) *line = tpos;
            }
        }
    }
    if (moved) *moved = hit ? best - v : qInf();
    return best;
}

QPointF Canvas::snapPoint(QPointF d, bool smart) const {
    if (!smart) return QPointF(snapCoord(d.x(), true), snapCoord(d.y(), false));
    double lx, ly;
    const QPointF r(snapCoord(d.x(), true, nullptr, true, &lx), snapCoord(d.y(), false, nullptr, true, &ly));
    m_snapX.clear(); m_snapY.clear();
    if (!qIsNaN(lx)) m_snapX << lx;
    if (!qIsNaN(ly)) m_snapY << ly;
    return r;
}

// The adjustment that snaps the nearest of r's left/centre/right (top/middle/bottom) to the grid,
// guides, canvas and objects; records the smart guide lines to draw.
QPointF Canvas::snapRectDelta(const QRectF &r, bool skipX, bool skipY) {
    m_snapX.clear(); m_snapY.clear();
    QPointF adj(0, 0);
    for (int axis = 0; axis < 2; ++axis) {
        if (axis == 0 ? skipX : skipY) continue;
        const double c[3] = {axis == 0 ? r.left() : r.top(), axis == 0 ? r.center().x() : r.center().y(), axis == 0 ? r.right() : r.bottom()};
        double bestAdj = qInf(), bestLine = qQNaN();
        for (double v : c) {
            double m = 0, ln = 0;
            snapCoord(v, axis == 0, &m, true, &ln);
            if (qIsFinite(m) && qAbs(m) < qAbs(bestAdj) - 1e-9) { bestAdj = m; bestLine = ln; }
        }
        if (!qIsFinite(bestAdj)) continue;
        (axis == 0 ? adj.rx() : adj.ry()) = bestAdj;
        if (!qIsNaN(bestLine)) {
            // every target the snapped rect now touches gets a line (e.g. left edge and centre at once)
            auto &lines = axis == 0 ? m_snapX : m_snapY;
            smartTargets();
            for (double v : c)
                for (double tpos : axis == 0 ? m_tgtX : m_tgtY)
                    if (qAbs(v + bestAdj - tpos) < 1e-6 && !lines.contains(tpos)) lines << tpos;
        }
    }
    return adj;
}

bool Canvas::snapsAtPress(Tool t) const {
    switch (t) {
    case Tool::Rect: case Tool::Ellipse: case Tool::Polygon: case Tool::Line: case Tool::Pen: case Tool::PolyHotspot: case Tool::PolySlice:
    case Tool::AsRoundRect: case Tool::AsChamfer: case Tool::AsBevel: case Tool::AsPolygon: case Tool::AsStar: case Tool::AsSmartPolygon:
    case Tool::AsArc: case Tool::AsDoughnut: case Tool::AsSpiral: case Tool::AsArrow: case Tool::AsArrowLine: case Tool::AsConnector:
    case Tool::AsGear: case Tool::AsFrame: case Tool::AsLShape: case Tool::AsSpeech: case Tool::AsHeart:
    case Tool::Slice: case Tool::Hotspot: case Tool::CircleHotspot: case Tool::Crop: case Tool::Marquee:
    case Tool::OvalMarquee: case Tool::PolyLasso:
        return true;
    default: return false;
    }
}

bool Canvas::guideAt(const QPointF &vp, bool *vertical, int *index) const {
    const GuideSettings &g = m_doc->guides;
    if (!g.show || g.lock) return false;
    for (int i = 0; i < g.v.size(); ++i)
        if (qAbs(toView(QPointF(g.v[i], 0)).x() - vp.x()) <= 3) { *vertical = true; *index = i; return true; }
    for (int i = 0; i < g.h.size(); ++i)
        if (qAbs(toView(QPointF(0, g.h[i])).y() - vp.y()) <= 3) { *vertical = false; *index = i; return true; }
    return false;
}

void Canvas::beginGuideDrag(bool vertical) {
    m_guideDrag = true;
    m_guideVert = vertical;
    m_guideIndex = -1;
    m_guidePos = -1e9;
    if (!m_doc->guides.show) { GuideSettings g = m_doc->guides; g.show = true; m_doc->setGuides(g); }
    update();
}

void Canvas::guideDragTo(QPoint globalPos) {
    if (!m_guideDrag) return;
    const QPointF d = toDoc(mapFromGlobal(QPointF(globalPos)));
    double v = std::round(m_guideVert ? d.x() : d.y());
    if (m_doc->grid.snap) v = std::round(snapCoord(v, m_guideVert));
    m_guidePos = v;
    setCursor(m_guideVert ? Qt::SplitHCursor : Qt::SplitVCursor);
    update();
}

void Canvas::endGuideDrag(QPoint globalPos) {
    if (!m_guideDrag) return;
    guideDragTo(globalPos);
    // dropped back onto a ruler or outside the view: a new guide is cancelled, a moved one removed
    finishGuide(rect().contains(mapFromGlobal(globalPos)));
}

void Canvas::finishGuide(bool keep) {
    GuideSettings g = m_doc->guides;
    QVector<double> &list = m_guideVert ? g.v : g.h;
    if (m_guideIndex >= 0 && m_guideIndex < list.size()) list.removeAt(m_guideIndex);
    if (keep) list << m_guidePos;
    m_guideDrag = false;
    m_guideIndex = -1;
    if (!(g == m_doc->guides)) m_doc->setGuides(g);
    status(keep ? QStringLiteral("%1 guide at %2 px").arg(m_guideVert ? QStringLiteral("Vertical") : QStringLiteral("Horizontal")).arg(m_guidePos)
                : QStringLiteral("Guide removed"));
    updateCursor();
    update();
}

void Canvas::mousePressEvent(QMouseEvent *e) {
    // Pencil Left/Right colours: the other button while a stroke is under way is ignored
    if (m_drag == Drag::Paint && pencilTwoColorActive() && (e->button() == Qt::LeftButton || e->button() == Qt::RightButton)) return;
    m_tgtValid = false;   // smart snap targets are collected afresh for each drag
    m_noSnap = false;
    m_snapX.clear(); m_snapY.clear();
    setFocus();
    const QPointF rawD = toDoc(e->position());
    const QPointF d = e->button() == Qt::LeftButton && snapsAtPress(effectiveTool()) ? snapPoint(rawD) : rawD;
    m_pressDoc = m_lastDoc = d;
    m_pressView = e->position();
    m_mods = e->modifiers();
    m_moved = false;
    Tool tool = effectiveTool();
    if (e->button() == Qt::MiddleButton) tool = Tool::Hand;
    const bool twoColor = pencilTwoColorActive() && (e->button() == Qt::LeftButton || e->button() == Qt::RightButton);
    if (e->button() == Qt::RightButton && !twoColor) return;
    if (m_doc->s.layers.isEmpty()) return;
    m_paintButton = twoColor ? e->button() : Qt::LeftButton;

    if (toolIsStub(tool)) {
        status(QStringLiteral("The %1 tool is not available yet").arg(toolName(tool)));
        return;
    }
    if (e->button() == Qt::LeftButton) {
        const int sh = shapeHandleAt(e->position());
        if (sh >= 0) {
            m_drag = Drag::ShapeHandle;
            m_shapeHandle = sh;
            return;
        }
    }
    if (toolIsSelect(tool) || tool == Tool::Subselect || toolIsTransform(tool) || tool == Tool::Gradient || tool == Tool::PaintBucket) {
        const int gh = gradHandleAt(e->position());
        if (gh >= 0) {
            VObject *g = gradientTarget();
            m_drag = Drag::GradHandle;
            m_gradHandle = gh;
            fills::fillHandles(*g, m_gradStart);
            return;
        }
    }
    if (toolIsTransform(tool) && e->button() == Qt::LeftButton) {
        const int th = transformHandleAt(e->position(), nullptr);
        if (th >= 0) {
            m_txHandle = th;
            m_txQuad0 = m_tx.quad;
            m_txCentre0 = m_tx.centre;
            m_txAngle0 = m_tx.angle;
            m_startObjs.clear();
            for (VObject *o : m_doc->selectedEditable()) m_startObjs.insert(o->id, *o);
            m_drag = Drag::Transform;
            return;
        }
    }
    if (toolIsSelect(tool) || tool == Tool::Subselect || toolIsTransform(tool)) {
        bool vert = false;
        int gi = -1;
        if (guideAt(e->position(), &vert, &gi)) {
            m_drag = Drag::Guide;
            m_guideDrag = true;
            m_guideVert = vert;
            m_guideIndex = gi;
            m_guidePos = vert ? m_doc->guides.v[gi] : m_doc->guides.h[gi];
            update();
            return;
        }
    }
    // the Pointer on a pixel selection drags the pixels (Alt: a copy of them)
    if (toolIsSelect(tool) && e->button() == Qt::LeftButton && !m_doc->pixelSelection.isEmpty() &&
        beginSelectionMove(d, true, m_mods & Qt::AltModifier))
        return;
    switch (tool) {
    case Tool::Gradient: {
        quint64 hit = hitObject(d, true);
        VObject *o = hit ? m_doc->find(hit) : m_doc->singleSelected();
        if (!o || !o->isVector() || o->locked) { status(QStringLiteral("Gradient: drag across a shape to set its gradient")); return; }
        m_doc->selectOnly(o->id);
        m_gradConverted = !fills::isGradient(o->fillKind) || !o->fillOn;
        if (m_gradConverted) {
            const AppState &as = appState();
            fills::setFillKind(*o, FillKind(qBound(int(FillKind::Linear), as.gradientKind, int(FillKind::Rectangle))));
            // the tool's own colours (Edit...), fill edge and texture
            if (as.gradientStops.size() >= 2) o->gradient = as.gradientStops;
            o->antialias = as.fillEdge != 0;
            o->feather = as.fillEdge == 2 ? as.fillFeather : 0;
            o->texture = as.brushTextureAmount > 0 ? as.brushTexture : QString();
            o->textureAmount = as.brushTextureAmount;
        }
        m_drag = Drag::GradTool;
        m_creatingId = o->id;
        update();
        return;
    }
    case Tool::Hand:
        m_drag = Drag::Pan;
        updateCursor();
        return;
    case Tool::Zoom:
        m_drag = Drag::ZoomRect; // click zooms in (Alt: out), drag zooms to the rectangle
        m_rubber = QRectF(d, d);
        return;
    case Tool::Pointer: case Tool::SelectBehind: case Tool::Scale: case Tool::Skew: case Tool::Distort: case Tool::Perspective: case Tool::Subselect: {
        if (tool == Tool::Subselect) {
            VObject *o = m_doc->singleSelected();
            if (o && (canEditPoints(o) || o->type == ObjType::Line)) {
                QPolygonF pts = canEditPoints(o) ? o->points : QPolygonF({o->p1, o->p2});
                // handles of the selected node first (they may sit on top of other nodes)
                if (o->type == ObjType::Path && m_pointIndex >= 0 && m_pointIndex < o->hIn.size()) {
                    const QPointF hs[2] = {o->hIn[m_pointIndex], o->hOut[m_pointIndex]};
                    for (int side = 1; side >= 0; --side)
                        if (QLineF(hs[side], o->points[m_pointIndex]).length() > 1e-6 && QLineF(toView(hs[side]), e->position()).length() < 6) {
                            m_handleSide = side;
                            const QPointF other = hs[1 - side];
                            m_handleMirror = QLineF(other, o->points[m_pointIndex]).length() > 1e-6;
                            m_handleFresh = false;
                            m_drag = Drag::HandleEdit;
                            return;
                        }
                }
                for (int i = 0; i < pts.size(); ++i)
                    if (QLineF(toView(pts[i]), e->position()).length() < 6) {
                        m_pointIndex = i;
                        if (o->type == ObjType::Path && (e->modifiers() & Qt::AltModifier)) {
                            // Alt-drag a node: pull out symmetric curve handles
                            o->ensureHandles();
                            o->hIn[i] = o->hOut[i] = o->points[i];
                            m_handleSide = 1;
                            m_handleMirror = true;
                            m_handleFresh = true;
                            m_drag = Drag::HandleEdit;
                        } else m_drag = Drag::PointEdit;
                        update();
                        return;
                    }
            }
        }
        int h = toolIsSelect(tool) ? handleAt(e->position()) : -1;
        if (h >= 0) {
            m_handle = h;
            m_drag = Drag::Resize;
            m_startBounds = m_doc->selectionBounds();
            m_startObjs.clear();
            for (VObject *o : m_doc->selectedEditable()) m_startObjs.insert(o->id, *o);
            return;
        }
        quint64 enter = 0;
        quint64 hit = 0;
        if (tool == Tool::SelectBehind && !(e->modifiers() & Qt::ShiftModifier)) {
            const QVector<quint64> stack = hitStack(d, false);
            const double same = qMax(3.0 / m_zoom, appState().prefs.pickDistance / m_zoom);
            if (!stack.isEmpty() && m_behindCycling && QLineF(m_behindLastDoc, d).length() <= same) {
                int i = stack.indexOf(m_behindLastId);
                hit = stack[(i >= 0 ? i + 1 : 0) % stack.size()];
            } else {
                hit = stack.isEmpty() ? 0 : stack.first();
            }
            m_behindCycling = !stack.isEmpty();
            m_behindLastDoc = d;
            m_behindLastId = hit;
        } else {
            m_behindCycling = false;
            hit = pickObject(d, tool == Tool::Subselect, &enter);
            if (enter != m_doc->enteredGroup && !(e->modifiers() & Qt::ShiftModifier)) {
                if (enter) m_doc->enterGroup(enter); else m_doc->enteredGroup = 0;
            }
        }
        if (hit) {
            int layer = 0;
            m_doc->find(hit, &layer);
            if (e->modifiers() & Qt::ShiftModifier) {
                QSet<quint64> s = m_doc->selection;
                if (s.contains(hit)) s.remove(hit); else s.insert(hit);
                m_doc->setSelection(s);
            } else if (!m_doc->selection.contains(hit)) {
                m_doc->selectOnly(hit);
            }
            if (m_doc->curLayer != layer) { m_doc->curLayer = layer; m_doc->notifyStructure(); }
            m_pointIndex = -1;
            m_drag = m_doc->selection.contains(hit) ? Drag::Move : Drag::None;
            m_startBounds = m_doc->selectionBounds();
            m_moveApplied = QPointF();
            return;
        }
        if (!(e->modifiers() & Qt::ShiftModifier)) m_doc->clearSelection();
        m_drag = Drag::RubberBand;
        m_rubber = QRectF(d, d);
        return;
    }
    case Tool::ExportArea: {
        if (m_areaActive) {
            const int h = areaHandleAt(e->position());
            if (h >= 0) { m_drag = Drag::AreaHandle; m_areaHandle = h; m_areaStart = m_area; return; }
            if (QRectF(m_area).contains(d)) { m_drag = Drag::AreaMove; m_areaStart = m_area; return; }
        }
        const QPoint q = areaSnap(d, !(e->modifiers() & kNoSnapModifier));
        m_pressDoc = m_lastDoc = QPointF(q);
        m_areaActive = true;
        m_area = QRect(q, QSize(0, 0));
        m_drag = Drag::Area;
        publishExportArea();
        update();
        return;
    }
    case Tool::Crop:
        if (m_cropActive && m_crop.contains(d)) { m_drag = Drag::CropMove; return; }
        m_cropActive = true;
        m_crop = QRectF(d, d);
        m_drag = Drag::Crop;
        update();
        return;
    case Tool::Marquee: case Tool::OvalMarquee: {
        m_spaceLast = d;
        if (beginSelectionMove(d, m_mods & Qt::ControlModifier, (m_mods & Qt::ControlModifier) && (m_mods & Qt::AltModifier))) return;
        m_drag = Drag::PixelRect;
        m_selOval = tool == Tool::OvalMarquee;
        m_selShiftFree = !(m_mods & Qt::ShiftModifier);
        m_selAltFree = !(m_mods & Qt::AltModifier);
        const AppState &as = appState();
        if (as.marqueeStyle == AppState::StyleFixedSize) m_rubber = QRectF(d, QSizeF(as.marqueeFixedW, as.marqueeFixedH));
        else m_rubber = QRectF(d, d);
        update();
        return;
    }
    case Tool::PolyLasso: {
        if (m_poly.isEmpty() && beginSelectionMove(d, m_mods & Qt::ControlModifier, (m_mods & Qt::ControlModifier) && (m_mods & Qt::AltModifier))) return;
        const double closeR = 5 / m_zoom;
        if (m_poly.isEmpty()) { m_poly = QPolygonF({d}); m_polyMods = m_mods; m_hoverDoc = d; update(); return; }
        if (m_poly.size() >= 3 && QLineF(d, m_poly.first()).length() <= closeR) { finishPolyLasso(); return; }
        const QPointF q = polyConstrain(d, m_mods & Qt::ShiftModifier);
        if (QLineF(q, m_poly.last()).length() >= 0.5 / m_zoom) m_poly << q;
        update();
        return;
    }
    case Tool::Lasso:
        if (beginSelectionMove(d, m_mods & Qt::ControlModifier, (m_mods & Qt::ControlModifier) && (m_mods & Qt::AltModifier))) return;
        m_drag = Drag::Lasso;
        m_lasso = QPolygonF({d});
        return;
    case Tool::MagicWand: {
        if (beginSelectionMove(d, m_mods & Qt::ControlModifier, (m_mods & Qt::ControlModifier) && (m_mods & Qt::AltModifier))) return;
        const AppState &as = appState();
        QImage mask;
        QTransform toCanvas;
        if (as.wandSampleAllLayers) {
            // optional: sample the flattened canvas (objects above/below affect the mask)
            const QImage &img = renderedFrame();
            const QPoint pt = d.toPoint();
            if (!img.rect().contains(pt)) return;
            mask = filters::floodMask(img, pt, as.tolerance, as.contiguous);
        } else {
            // Fireworks default: only the active bitmap's own pixels (docToImage mapping)
            VObject *o = bitmapForWand(m_doc, d);
            if (!o) { status(QStringLiteral("Magic Wand: select or click a bitmap object")); return; }
            if (o->hasXform()) xf::bakeBitmap(*o);
            const QPoint ip = toImage(*o, d).toPoint();
            if (!o->image.rect().contains(ip)) return;
            mask = filters::floodMask(o->image, ip, as.tolerance, as.contiguous);
            toCanvas = pxedit::imageToDoc(*o);
        }
        QPainterPath path = filters::maskToPath(mask, toCanvas);
        commitPixelSelection(path, e->modifiers(), Tool::MagicWand);
        return;
    }
    case Tool::Eyedropper: {
        const QImage &img = renderedFrame();
        QPoint pt = d.toPoint();
        if (!img.rect().contains(pt)) return;
        // Sample: point, or the average of a 3x3 / 5x5 block
        const int rad = appState().eyedropperSample == 2 ? 2 : appState().eyedropperSample == 1 ? 1 : 0;
        double sr = 0, sg = 0, sb = 0, sa = 0;
        for (int yy = pt.y() - rad; yy <= pt.y() + rad; ++yy)
            for (int xx = pt.x() - rad; xx <= pt.x() + rad; ++xx) {
                const QPoint q(qBound(0, xx, img.width() - 1), qBound(0, yy, img.height() - 1));
                const QColor pc = img.pixelColor(q);
                const double a = pc.alphaF();
                sr += pc.redF() * a; sg += pc.greenF() * a; sb += pc.blueF() * a; sa += a;
            }
        QColor c = sa > 0 ? QColor::fromRgbF(float(sr / sa), float(sg / sa), float(sb / sa)) : img.pixelColor(pt);
        c.setAlpha(255);
        if (e->modifiers() & Qt::ShiftModifier) appState().setStroke(c); else appState().setFill(c);
        status(QStringLiteral("Picked %1").arg(c.name().toUpper()));
        return;
    }
    case Tool::PaintBucket: {
        const AppState &as = appState();
        quint64 hit = hitObject(d, true);
        VObject *o = hit ? m_doc->find(hit) : nullptr;
        // Fill selected object: a click anywhere fills the selected vector object(s)
        if (as.fillSelectedObject && (!o || !o->isVector())) {
            VObject *sel = m_doc->singleSelected();
            if (sel && sel->isVector() && !sel->locked) o = sel;
        }
        // Fill Selection Only: with a pixel selection, fill all of it on the bitmap under it
        if (!o && as.fillSelectionOnly && !m_doc->pixelSelection.isEmpty()) {
            VObject *sel = m_doc->singleSelected();
            if (sel && sel->type == ObjType::Bitmap) o = sel;
        }
        if (!o) { status(QStringLiteral("Paint Bucket: click a shape or a bitmap")); return; }
        if (o->type == ObjType::Bitmap) {
            if (o->locked) return;
            if (!m_doc->beginPixelEdit(*o)) { status(pxedit::blockedMessage(*o, QStringLiteral("Paint Bucket"))); return; }
            QPointF ip = toImage(*o, d);
            o->image = o->image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QImage mask;
            const QImage sel = paintClip(*o);
            if (as.fillSelectionOnly && !sel.isNull()) mask = sel;
            else {
                if (!o->image.rect().contains(ip.toPoint())) { m_doc->endPixelEdits(false); return; }
                mask = filters::floodMask(o->image, ip.toPoint(), as.tolerance, true).convertToFormat(QImage::Format_Alpha8);
                if (!sel.isNull()) {   // a flood never leaves the selection
                    QPainter mp(&mask);
                    mp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                    mp.drawImage(0, 0, sel);
                }
                // Edge: Hard keeps the flood's pixels, Anti-Alias softens its boundary, Feather blurs it
                if (as.fillEdge == 1) fx::blur(mask, 0.6);
                else if (as.fillEdge == 2) fx::blur(mask, as.fillFeather / 2.0);
            }
            brush::fillMask(o->image, mask, as.fill, as.toolOpacity, BlendMode(qBound(0, as.toolBlend, int(BlendMode::Exclusion))),
                            as.brushTexture, as.brushTextureAmount);
        } else if (o->isVector()) {
            if (o->locked) return;
            o->fillOn = true;
            o->fill = as.fill;
            if (fills::isGradient(o->fillKind)) fills::setFillKind(*o, FillKind::Solid);
            o->antialias = as.fillEdge != 0;
            o->feather = as.fillEdge == 2 ? as.fillFeather : 0;
            o->texture = as.brushTextureAmount > 0 ? as.brushTexture : QString();
            o->textureAmount = as.brushTextureAmount;
            if (as.toolOpacity < 100) o->fill.setAlphaF(as.toolOpacity / 100.0);
        } else return;
        m_doc->commit(QStringLiteral("Paint Bucket"));
        return;
    }
    case Tool::Brush: case Tool::Pencil: case Tool::Eraser:
    case Tool::Blur: case Tool::Sharpen: case Tool::Dodge: case Tool::Burn: case Tool::Smudge: case Tool::RubberStamp:
    case Tool::Healing: case Tool::SpotHealing: case Tool::ReplaceColor: case Tool::RedEye:
        beginPaint(d);
        return;
    case Tool::ContentAware: {
        // paint over what to remove; on release the painted area is filled from its surroundings
        if (m_caJob) { status(QStringLiteral("Content-Aware Remove: still filling the previous stroke...")); return; }
        VObject *sel = m_doc->singleSelected();
        VObject *o = (sel && sel->type == ObjType::Bitmap && !sel->locked && sel->visible) ? sel : nullptr;
        if (!o) {
            const quint64 h = hitObject(d, true);
            VObject *ho = h ? m_doc->find(h) : nullptr;
            if (ho && ho->type == ObjType::Bitmap && !ho->locked && ho->visible) o = ho;
        }
        int layer = -1;
        if (!o || !m_doc->find(o->id, &layer) || m_doc->s.layers[layer].locked) {
            status(QStringLiteral("Content-Aware Remove: select or click an unlocked bitmap object"));
            return;
        }
        if (!m_doc->beginPixelEdit(*o)) { status(pxedit::blockedMessage(*o, QStringLiteral("Content-Aware Remove"))); return; }
        m_paintTarget = o->id;
        m_caPath = QPainterPath(d);
        m_caPath.lineTo(d + QPointF(0.01, 0));
        m_drag = Drag::ContentAware;
        update();
        return;
    }
    case Tool::Slice: case Tool::Hotspot: case Tool::CircleHotspot:
        if (int w = m_doc->webLayer(); w >= 0 && m_doc->s.layers[w].locked) { status(QStringLiteral("The Web Layer is locked")); return; }
        if (!appState().webLayerVisible) { appState().webLayerVisible = true; appState().notifyOptions(); }
        m_drag = Drag::Create;
        m_creatingId = 0;
        return;
    case Tool::Rect: case Tool::Ellipse: case Tool::Polygon: case Tool::Line:
    case Tool::AsRoundRect: case Tool::AsChamfer: case Tool::AsBevel: case Tool::AsPolygon: case Tool::AsStar: case Tool::AsSmartPolygon:
    case Tool::AsArc: case Tool::AsDoughnut: case Tool::AsSpiral: case Tool::AsArrow: case Tool::AsArrowLine: case Tool::AsConnector:
    case Tool::AsGear: case Tool::AsFrame: case Tool::AsLShape: case Tool::AsSpeech: case Tool::AsHeart: {
        if (m_doc->s.layers[m_doc->drawingLayer()].locked) { status(QStringLiteral("The current layer is locked")); return; }
        m_drag = Drag::Create;
        m_creatingId = 0;
        return;
    }
    case Tool::VectorPath: {
        if (m_doc->s.layers[m_doc->drawingLayer()].locked) { status(QStringLiteral("The current layer is locked")); return; }
        m_freehand = QPolygonF({d});
        m_freehandPress = { qBound(0.0, (penSample().pressure > 0 ? penSample().pressure : 1.0), 1.0) };
        m_freehandSpeed = { 1.0 };
        m_drag = Drag::VectorPath;
        status(QStringLiteral("Vector Path: drag to draw a smoothed path"));
        return;
    }
    case Tool::RedrawPath: {
        VObject *o = m_doc->singleSelected();
        if (!o || o->type != ObjType::Path || o->points.size() < 2) {
            status(QStringLiteral("Redraw Path: select a path first"));
            return;
        }
        m_redrawId = o->id;
        m_freehand = QPolygonF({d});
        m_freehandPress = { qBound(0.0, (penSample().pressure > 0 ? penSample().pressure : 1.0), 1.0) };
        m_freehandSpeed = { 1.0 };
        m_drag = Drag::RedrawPath;
        status(QStringLiteral("Redraw Path: drag along the segment to replace"));
        return;
    }
    case Tool::Pen: case Tool::PolyHotspot: case Tool::PolySlice: {
        if (tool == Tool::Pen && m_doc->s.layers[m_doc->drawingLayer()].locked) { status(QStringLiteral("The current layer is locked")); return; }
        if ((tool == Tool::PolyHotspot || tool == Tool::PolySlice)) {
            if (int w = m_doc->webLayer(); w >= 0 && m_doc->s.layers[w].locked) { status(QStringLiteral("The Web Layer is locked")); return; }
            if (!appState().webLayerVisible) { appState().webLayerVisible = true; appState().notifyOptions(); }
        }
        m_penTool = tool;
        if (m_penPts.size() > 2 && QLineF(toView(m_penPts.first()), e->position()).length() < 7) { finishPen(true); return; }
        if (m_penIn.size() != m_penPts.size()) { m_penIn = m_penPts; m_penOut = m_penPts; }
        m_penPts << d;
        m_penIn << d;
        m_penOut << d;
        m_hoverDoc = d;
        if (tool == Tool::Pen) m_drag = Drag::PenHandle; // dragging pulls out curve handles
        status(tool == Tool::Pen ? QStringLiteral("Pen: click to add points, double-click or Enter to finish, click the first point to close")
             : tool == Tool::PolySlice ? QStringLiteral("Polygon Slice: click to add points, double-click or click the first point to close, Esc to cancel")
             : QStringLiteral("Polygon Hotspot: click to add points, double-click or Enter to finish"));
        update();
        return;
    }
    case Tool::Knife: {
        if (m_doc->selectedEditable().isEmpty()) { status(QStringLiteral("Knife: select the paths to cut first")); return; }
        m_lasso = QPolygonF({d});
        m_drag = Drag::Knife;
        return;
    }
    case Tool::Freeform: case Tool::ReshapeArea: {
        VObject *o = m_doc->singleSelected();
        if (!o || !paths::canConvert(*o) || o->type == ObjType::Text) { status(QStringLiteral("Select a single path to reshape")); return; }
        const double size = appState().pathToolSize;
        paths::densify(*o, qBound(1.0, size / 8.0, 8.0));
        m_sculptPull = tool == Tool::ReshapeArea || paths::distanceToOutline(*o, d) <= qMax(3.0 / m_zoom, 2.0);
        m_drag = Drag::Sculpt;
        m_lastDoc = d;
        m_doc->notifyChanged();
        return;
    }
    case Tool::PathScrubber: {
        // the selected paths, else the path under the pointer (selected first)
        if (scrubTargets().isEmpty()) {
            const quint64 hit = hitObject(d, true);
            VObject *o = hit ? m_doc->find(hit) : nullptr;
            if (o && o->type == ObjType::Path && !o->textOnPath) m_doc->selectOnly(hit);
        }
        if (scrubTargets().isEmpty()) { status(QStringLiteral("Path Scrubber: select or click a vector path")); return; }
        m_altHeld = e->modifiers() & Qt::AltModifier;
        m_drag = Drag::Scrub;
        m_scrubPrepared.clear();
        m_scrubChanged = false;
        m_scrubCarry = 0;
        m_lastDoc = d;
        scrubDab(d, m_altHeld);
        m_doc->notifyChanged();
        updateCursor();
        return;
    }
    case Tool::Text: {
        quint64 hit = hitObject(d, true);
        VObject *o = hit ? m_doc->find(hit) : nullptr;
        if (o && o->type == ObjType::Text) { m_doc->selectOnly(hit); editText(o); return; }
        if (m_doc->s.layers[m_doc->curLayer].locked) { status(QStringLiteral("The current layer is locked")); return; }
        bool ok = false;
        QString text = QInputDialog::getMultiLineText(this, QStringLiteral("Text"), QStringLiteral("Enter text:"), QString(), &ok);
        if (!ok || text.isEmpty()) return;
        VObject t;
        t.type = ObjType::Text;
        t.text = text;
        t.fontFamily = appState().fontFamily;
        t.fontSize = appState().fontSize;
        t.bold = appState().bold;
        t.italic = appState().italic;
        t.underline = appState().underline;
        t.textAlign = appState().textAlign;
        t.textOrientation = appState().textOrientation;
        t.textDirection = appState().textDirection;
        t.paraIndent = appState().paraIndent;
        t.paraSpaceBefore = appState().paraSpaceBefore;
        t.paraSpaceAfter = appState().paraSpaceAfter;
        t.kerning = appState().kerning;
        t.leading = appState().leading;
        t.hScale = appState().hScale;
        t.baselineShift = appState().baselineShift;
        t.textAA = appState().textAA;
        t.fillOn = true;
        t.fill = appState().textColor;
        t.strokeOn = false;
        t.rect = QRectF(d, QSizeF(10, 10));
        t.fitTextBox();
        t.name = text.section('\n', 0, 0).left(24);
        m_doc->addObject(t);
        m_doc->commit(QStringLiteral("Text"));
        return;
    }
    default: break;
    }
}

void Canvas::mouseMoveEvent(QMouseEvent *e) {
    QPointF d = toDoc(e->position());
    m_hoverDoc = d;
    emit cursorMoved(d);
    if (QLineF(e->position(), m_pressView).length() > 2) m_moved = true;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    if (m_drag != Drag::None) {
        const bool off = e->modifiers() & kNoSnapModifier;   // hold Ctrl: no snapping for now
        if (off != m_noSnap) { m_noSnap = off; if (off) { m_snapX.clear(); m_snapY.clear(); } }
    }
    if (m_drag == Drag::Crop) d = snapPoint(d, true);
    else if (m_drag == Drag::Create || m_drag == Drag::PixelRect || m_drag == Drag::PointEdit) d = snapPoint(d);
    switch (m_drag) {
    case Drag::Guide:
        guideDragTo(e->globalPosition().toPoint());
        return;
    case Drag::None: {
        Tool tool = effectiveTool();
        if (!m_penPts.isEmpty()) m_hoverDoc = snapPoint(d);
        if (!m_poly.isEmpty()) { m_hoverDoc = d; update(); }
        if (toolIsSelect(tool) || tool == Tool::Subselect || toolIsTransform(tool)) {
            bool vert = false;
            int gi = -1;
            if (guideAt(e->position(), &vert, &gi) && handleAt(e->position()) < 0) { setCursor(vert ? Qt::SplitHCursor : Qt::SplitVCursor); return; }
        }
        quint64 h = (toolIsSelect(tool) || tool == Tool::Subselect || toolIsTransform(tool)) ? hitObject(d, false) : 0;
        if (toolIsTransform(tool)) {
            const int th = transformHandleAt(e->position(), nullptr);
            static const Qt::CursorShape shapes[8] = {Qt::SizeFDiagCursor, Qt::SizeBDiagCursor, Qt::SizeFDiagCursor, Qt::SizeBDiagCursor,
                                                      Qt::SizeVerCursor, Qt::SizeHorCursor, Qt::SizeVerCursor, Qt::SizeHorCursor};
            if (th == 9) setCursor(rotateCursor());
            else if (th == 8) setCursor(Qt::SizeAllCursor);
            else if (th >= 0) setCursor(tool == Tool::Scale ? shapes[th] : Qt::CrossCursor);
            else if (h && m_doc->selection.contains(h)) setCursor(Qt::SizeAllCursor);
            else setCursor(Qt::ArrowCursor);
        } else if (toolIsSelect(tool)) {
            int hd = handleAt(e->position());
            static const Qt::CursorShape shapes[8] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor,
                                                      Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor};
            if (hd >= 0) setCursor(shapes[hd]);
            else if (h && m_doc->selection.contains(h)) setCursor(Qt::SizeAllCursor);
            else setCursor(Qt::ArrowCursor);
        }
        if (h != m_hoverId) { m_hoverId = h; update(); }
        if (tool == Tool::Brush || tool == Tool::Eraser || isRetouch(tool) || tool == Tool::ContentAware || !m_penPts.isEmpty()) update();
        return;
    }
    case Drag::VectorPath: case Drag::RedrawPath:
        if (m_freehand.isEmpty() || QLineF(m_freehand.last(), d).length() >= 0.6) {
            const double len = m_freehand.isEmpty() ? 0.0 : QLineF(m_freehand.last(), d).length();
            m_freehand << d;
            const double pr = qBound(0.0, (penSample().pressure > 0 ? penSample().pressure : 1.0), 1.0);
            const double sz = qMax(1.0, appState().strokeWidth * 0.8);
            m_freehandPress << pr;
            m_freehandSpeed << qBound(0.05, len / sz, 2.0);
        }
        update();
        return;
    case Drag::Pan:
        m_origin += e->position() - m_pressView;
        m_needFit = false;
        m_pressView = e->position();
        clampOrigin();
        update();
        emit viewChanged();
        return;
    case Drag::Move: {
        if (!m_moved) return;
        QPointF total = d - m_pressDoc;
        if (shift) { // constrain to the dominant axis of the whole drag
            if (qAbs(total.x()) > qAbs(total.y())) total.setY(0); else total.setX(0);
        }
        // snap the moved bounds (nearest of edges and centre) to the grid, guides, canvas and other objects
        total += snapRectDelta(m_startBounds.translated(total), shift && total.x() == 0, shift && total.y() == 0);
        const QPointF delta = total - m_moveApplied;
        m_moveApplied = total;
        m_lastDoc = m_pressDoc + total;
        // P4: top-level objects only re-render their old and new extents (a moved group member
        // can change its group's effects anywhere, so that re-renders the frame)
        const auto moving = m_doc->selectedEditable();
        QSet<const VObject *> top;
        for (const auto &layer : std::as_const(m_doc->s).frames[m_doc->curFrame].layers) for (const VObject &o : layer) top.insert(&o);
        bool partial = !moving.isEmpty();
        QRectF dirty;
        for (VObject *o : moving) {
            partial &= top.contains(o);
            dirty |= o->visualBounds();
            o->moveBy(delta);
            dirty |= o->visualBounds();
        }
        if (partial) m_doc->notifyChanged(dirty); else m_doc->notifyChanged();
        return;
    }
    case Drag::Resize: {
        QRectF b = m_startBounds;
        QPointF dd = d - m_pressDoc;
        double l = b.left(), t = b.top(), r = b.right(), btm = b.bottom();
        switch (m_handle) {
        case 0: l += dd.x(); t += dd.y(); break;
        case 1: t += dd.y(); break;
        case 2: r += dd.x(); t += dd.y(); break;
        case 3: r += dd.x(); break;
        case 4: r += dd.x(); btm += dd.y(); break;
        case 5: btm += dd.y(); break;
        case 6: l += dd.x(); btm += dd.y(); break;
        case 7: l += dd.x(); break;
        }
        // snap the dragged edges (grid, guides, canvas edges / centre lines, other objects)
        m_snapX.clear(); m_snapY.clear();
        auto edge = [this](double &v, bool x) { double ln; v = snapCoord(v, x, nullptr, true, &ln); if (!qIsNaN(ln)) (x ? m_snapX : m_snapY) << ln; };
        if (m_handle == 0 || m_handle == 6 || m_handle == 7) edge(l, true);
        if (m_handle >= 2 && m_handle <= 4) edge(r, true);
        if (m_handle <= 2) edge(t, false);
        if (m_handle >= 4 && m_handle <= 6) edge(btm, false);
        QRectF nb = QRectF(QPointF(l, t), QPointF(r, btm)).normalized();
        if (shift && m_handle % 2 == 0 && b.width() > 0 && b.height() > 0) {
            double s = qMax(nb.width() / b.width(), nb.height() / b.height());
            QSizeF ns(b.width() * s, b.height() * s);
            QPointF anchor = m_handle == 0 ? b.bottomRight() : m_handle == 2 ? b.bottomLeft() : m_handle == 4 ? b.topLeft() : b.topRight();
            nb = QRectF(anchor, ns);
            if (m_handle == 0) nb.moveBottomRight(anchor);
            else if (m_handle == 2) nb.moveBottomLeft(anchor);
            else if (m_handle == 6) nb.moveTopRight(anchor);
        }
        if (nb.width() < 1) nb.setWidth(1);
        if (nb.height() < 1) nb.setHeight(1);
        for (auto it = m_startObjs.constBegin(); it != m_startObjs.constEnd(); ++it) {
            VObject *o = m_doc->find(it.key());
            if (!o) continue;
            *o = it.value();
            QRectF ob = it.value().bounds();
            double sx = b.width() > 0 ? nb.width() / b.width() : 1, sy = b.height() > 0 ? nb.height() / b.height() : 1;
            QRectF mapped(nb.left() + (ob.left() - b.left()) * sx, nb.top() + (ob.top() - b.top()) * sy, ob.width() * sx, ob.height() * sy);
            o->setBounds(mapped);
        }
        m_doc->notifyChanged();
        return;
    }
    case Drag::Transform:
        transformTo(d, e->modifiers());
        return;
    case Drag::SelMove:
        selectionMoveTo(d);
        return;
    case Drag::RubberBand: case Drag::PixelRect: {
        if (m_drag == Drag::RubberBand) { m_rubber = QRectF(m_pressDoc, d).normalized(); update(); return; }
        if (m_spaceRepos) {   // Space: reposition the marquee being drawn
            const QPointF by = d - m_spaceLast;
            m_pressDoc += by;
            m_rubber.translate(by);
            m_spaceLast = d;
            update();
            return;
        }
        m_spaceLast = d;
        const Qt::KeyboardModifiers now = e->modifiers();
        if (!(now & Qt::ShiftModifier)) m_selShiftFree = true;
        if (!(now & Qt::AltModifier)) m_selAltFree = true;
        const bool square = m_selShiftFree && (now & Qt::ShiftModifier);   // circle / square
        const bool centre = m_selAltFree && (now & Qt::AltModifier);       // from the centre
        const AppState &as = appState();
        if (as.marqueeStyle == AppState::StyleFixedSize) {   // the fixed box follows the pointer
            const QSizeF sz(as.marqueeFixedW, as.marqueeFixedH);
            m_rubber = centre ? QRectF(d - QPointF(sz.width() / 2, sz.height() / 2), sz) : QRectF(d, sz);
            update();
            return;
        }
        double dx = d.x() - m_pressDoc.x(), dy = d.y() - m_pressDoc.y();
        if (as.marqueeStyle == AppState::StyleFixedRatio && as.marqueeRatioW > 0 && as.marqueeRatioH > 0) {
            const double k = as.marqueeRatioH / as.marqueeRatioW;
            const double w = qMax(qAbs(dx), qAbs(dy) / k);
            dx = dx < 0 ? -w : w;
            dy = dy < 0 ? -w * k : w * k;
        } else if (square) {
            const double s = qMax(qAbs(dx), qAbs(dy));
            dx = dx < 0 ? -s : s;
            dy = dy < 0 ? -s : s;
        }
        m_rubber = centre ? QRectF(m_pressDoc - QPointF(dx, dy), m_pressDoc + QPointF(dx, dy)).normalized()
                          : QRectF(m_pressDoc, m_pressDoc + QPointF(dx, dy)).normalized();
        update();
        return;
    }
    case Drag::Lasso:
        m_lasso << d;
        update();
        return;
    case Drag::ZoomRect:
        m_rubber = QRectF(m_pressDoc, d).normalized();
        update();
        return;
    case Drag::Crop:
        m_crop = QRectF(m_pressDoc, d).normalized();
        publishCrop();
        update();
        return;
    case Drag::Area: case Drag::AreaMove: case Drag::AreaHandle: {
        const bool edges = !(e->modifiers() & kNoSnapModifier);
        const QPoint q = areaSnap(d, edges);
        const QPoint p0 = m_pressDoc.toPoint();
        const int W = m_doc->s.width, H = m_doc->s.height;
        if (m_drag == Drag::Area) {
            m_area = QRect(QPoint(qMin(p0.x(), q.x()), qMin(p0.y(), q.y())), QPoint(qMax(p0.x(), q.x()) - 1, qMax(p0.y(), q.y()) - 1));
            if (shift) {   // Shift: a square
                const int side = qMax(qAbs(q.x() - p0.x()), qAbs(q.y() - p0.y()));
                m_area = QRect(q.x() < p0.x() ? p0.x() - side : p0.x(), q.y() < p0.y() ? p0.y() - side : p0.y(), side, side) & QRect(0, 0, W, H);
            }
        } else if (m_drag == Drag::AreaMove) {
            QPoint delta = QPoint(qRound(d.x() - m_pressDoc.x()), qRound(d.y() - m_pressDoc.y()));
            QRect r = m_areaStart.translated(delta);
            if (edges) {   // canvas-edge snapping of the moved rectangle (8 screen px)
                const int tol = qMax(1, qRound(8 / m_zoom));
                if (qAbs(r.left()) <= tol) r.moveLeft(0);
                else if (qAbs(r.left() + r.width() - W) <= tol) r.moveLeft(W - r.width());
                if (qAbs(r.top()) <= tol) r.moveTop(0);
                else if (qAbs(r.top() + r.height() - H) <= tol) r.moveTop(H - r.height());
            }
            // keep the whole rectangle on the canvas
            r.moveLeft(qBound(0, r.left(), qMax(0, W - r.width())));
            r.moveTop(qBound(0, r.top(), qMax(0, H - r.height())));
            m_area = r;
        } else {
            int l = m_areaStart.left(), t = m_areaStart.top(), r = m_areaStart.left() + m_areaStart.width(), b = m_areaStart.top() + m_areaStart.height();
            const int h = m_areaHandle;
            if (h == 0 || h == 2 || h == 6) l = q.x();
            if (h == 1 || h == 3 || h == 7) r = q.x();
            if (h == 0 || h == 1 || h == 4) t = q.y();
            if (h == 2 || h == 3 || h == 5) b = q.y();
            m_area = QRect(QPoint(qMin(l, r), qMin(t, b)), QPoint(qMax(l, r) - 1, qMax(t, b) - 1));
        }
        publishExportArea();
        update();
        return;
    }
    case Drag::CropMove: {
        m_crop.translate(d - m_lastDoc);
        m_lastDoc = d;
        const QPointF adj = snapRectDelta(m_crop, false, false);
        m_crop.translate(adj);
        m_lastDoc += adj;
        publishCrop();
        update();
        return;
    }
    case Drag::Paint: {
        ++painttrace::moveBacklog();
        painttrace::Scope _pt("move", painttrace::eventLatencyMs(e), d.x(), d.y(),
            QStringLiteral("backlog=%1").arg(painttrace::moveBacklog()));
        queuePaintPoint(d);
        revealBrushCursor();
        return;
    }
    case Drag::PenHandle: {
        if (m_penPts.isEmpty()) return;
        const QPointF a = m_penPts.last();
        QPointF h = d;
        if (shift) { // constrain to 45 degrees
            QLineF l(a, d);
            l.setAngle(qRound(l.angle() / 45.0) * 45.0);
            h = l.p2();
        }
        m_penOut.last() = h;
        m_penIn.last() = 2 * a - h;
        update();
        return;
    }
    case Drag::HandleEdit: {
        VObject *o = m_doc->singleSelected();
        if (!o || o->type != ObjType::Path || m_pointIndex < 0 || m_pointIndex >= o->points.size()) return;
        o->ensureHandles();
        const QPointF a = o->points[m_pointIndex];
        QPointF &mine = m_handleSide ? o->hOut[m_pointIndex] : o->hIn[m_pointIndex];
        QPointF &other = m_handleSide ? o->hIn[m_pointIndex] : o->hOut[m_pointIndex];
        mine = d;
        if (m_handleFresh) other = 2 * a - d;
        else if (m_handleMirror && !(e->modifiers() & Qt::AltModifier)) {
            const double len = qMax(QLineF(a, other).length(), 1e-9);
            QLineF dir(a, d);
            if (dir.length() > 1e-9) {
                // Alt-dragged fresh handles mirror exactly; existing smooth nodes keep the other handle's length
                dir.setLength(len);
                other = 2 * a - dir.p2();
            }
        }
        m_doc->notifyChanged();
        return;
    }
    case Drag::ShapeHandle: {
        VObject *o = autoShapeTarget();
        if (!o) break;
        QPointF u = d;
        if (o->hasXform()) u = o->xform.inverted().map(d);
        autoshape::dragHandle(*o, m_shapeHandle, u, e->modifiers() & Qt::AltModifier);
        {
            // show the shape's numeric parameters in the status bar while dragging
            QStringList vals;
            for (const autoshape::Param &p : autoshape::params(o->shapeKind)) {
                if (!p.choices.isEmpty() || (p.key.size() == 2 && (p.key[0] == QLatin1Char('x') || p.key[0] == QLatin1Char('y')))) continue;
                const double v = autoshape::param(*o, p.key);
                vals << QStringLiteral("%1 %2").arg(p.label).arg(QString::number(v, 'f', p.decimals)) + p.suffix;
            }
            status(autoshape::label(o->shapeKind) + QStringLiteral(": ") + vals.join(QStringLiteral(", ")));
        }
        m_doc->notifyChanged();
        break;
    }
    case Drag::ContentAware: {
        m_caPath.lineTo(d);
        update();
        break;
    }
    case Drag::GradHandle: {
        VObject *g = gradientTarget();
        if (!g) break;
        QPointF delta = d - m_pressDoc;
        QPointF h[3] = {m_gradStart[0], m_gradStart[1], m_gradStart[2]};
        if (m_gradHandle == 0) for (auto &x : h) x += delta;
        else if (m_gradHandle == 3) { // a pattern bar: turn both axes about the centre (Ctrl: 15 degree steps; Shift until 0.3.6)
            double ang = QLineF(h[0], d).angleTo(QLineF(h[0], m_pressDoc));   // clockwise on screen, degrees
            if (e->modifiers() & Qt::ControlModifier) ang = qRound(ang / 15.0) * 15.0;
            const QTransform r = QTransform::fromTranslate(-h[0].x(), -h[0].y()) * QTransform().rotate(ang) * QTransform::fromTranslate(h[0].x(), h[0].y());
            h[1] = r.map(h[1]);
            h[2] = r.map(h[2]);
        } else {
            QPointF np = h[m_gradHandle] + delta;
            // Ctrl: constrain the dragged handle to 45 degrees around the start / centre
            // (Shift did this before 0.3.6)
            if (e->modifiers() & Qt::ControlModifier) {
                QLineF l(h[0], np);
                l.setAngle(qRound(l.angle() / 45.0) * 45.0);
                np = l.p2();
            }
            if (e->modifiers() & Qt::ShiftModifier) {
                // Shift: scale every handle equally - the scale (and turn) that takes the dragged
                // handle from where it started to the pointer is applied to the other handle(s)
                // too, about the start / centre, so the fill keeps its shape and proportions
                const QTransform sim = gradSimilarity(h[0], h[m_gradHandle], np);
                for (int i : {1, 2}) h[i] = sim.map(h[i]);
                h[m_gradHandle] = np;
            } else {
                h[m_gradHandle] = np;
                // one-axis kinds hide the second handle: keep it square to the vector so the
                // bands stay perpendicular to it as the end handle turns
                if (m_gradHandle == 1 && g->fillKind != FillKind::Pattern && !fills::usesSecondAxis(g->fillKind)) {
                    const QPointF u = h[1] - h[0];
                    const double lu = std::hypot(u.x(), u.y());
                    const QPointF v0 = m_gradStart[2] - m_gradStart[0];
                    const double lv = qMax(1.0, std::hypot(v0.x(), v0.y()));
                    if (lu > 1e-6) h[2] = h[0] + QPointF(u.y(), -u.x()) * (lv / lu);
                }
            }
        }
        fills::setFillHandles(*g, h);
        m_doc->notifyChanged();
        break;
    }
    case Drag::GradTool: {
        VObject *g = m_doc->find(m_creatingId);
        if (!g) break;
        QPointF to = d;
        if (e->modifiers() & Qt::ShiftModifier) { QLineF l(m_pressDoc, d); l.setAngle(qRound(l.angle() / 45.0) * 45.0); to = l.p2(); }
        if (QLineF(m_pressDoc, to).length() >= 1) fills::setGradientVector(*g, m_pressDoc, to);
        m_doc->notifyChanged();
        break;
    }
    case Drag::Knife:
        m_lasso << d;
        update();
        return;
    case Drag::Sculpt: {
        VObject *o = m_doc->singleSelected();
        if (!o || o->type != ObjType::Path) return;
        double size = appState().pathToolSize;
        // Pressure: a tablet pen's pressure scales the tool (a mouse always reports full pressure)
        if (appState().pathPressure) size *= qMax(0.1, tablet::withFloor(penSample().pressure, appState().prefs.tabletMinSize / 100.0));
        const QPointF delta = d - m_lastDoc;
        if (m_sculptPull) {
            const double strength = effectiveTool() == Tool::ReshapeArea ? appState().reshapeStrength / 100.0 : 1.0;
            paths::pull(*o, m_lastDoc, delta, size / 2.0 + (effectiveTool() == Tool::Freeform ? size : 0), strength);
        } else paths::push(*o, d, size / 2.0);
        paths::refine(*o, qBound(1.0, size / 6.0, 8.0));
        m_lastDoc = d;
        m_doc->notifyChanged();
        return;
    }
    case Drag::Scrub: {
        const bool alt = e->modifiers() & Qt::AltModifier;
        if (alt != m_altHeld) { m_altHeld = alt; updateCursor(); }
        double size = appState().psSize;
        if (appState().pathPressure) size *= qMax(0.1, tablet::withFloor(penSample().pressure, appState().prefs.tabletMinSize / 100.0));
        const double spacing = qMax(1.0, size / 8);
        const QLineF L(m_lastDoc, d);
        const double len = L.length();
        double pos = spacing - m_scrubCarry;
        while (pos <= len && len > 0) { scrubDab(L.pointAt(pos / len), alt); pos += spacing; }
        m_scrubCarry = len - (pos - spacing);
        m_lastDoc = d;
        m_doc->notifyChanged();
        return;
    }
    case Drag::PointEdit: {
        VObject *o = m_doc->singleSelected();
        if (!o) return;
        if (o->type == ObjType::Path && m_pointIndex < o->points.size()) {
            const QPointF delta = d - o->points[m_pointIndex];
            o->points[m_pointIndex] = d;
            if (m_pointIndex < o->hIn.size()) o->hIn[m_pointIndex] += delta;
            if (m_pointIndex < o->hOut.size()) o->hOut[m_pointIndex] += delta;
        } else if (canEditPoints(o) && m_pointIndex < o->points.size()) {
            o->points[m_pointIndex] = d;
            if (o->isPolygonWeb()) o->rect = o->points.boundingRect();
        }
        else if (o->type == ObjType::Line) (m_pointIndex == 0 ? o->p1 : o->p2) = d;
        m_doc->notifyChanged();
        return;
    }
    case Drag::Create: {
        if (!m_moved) return;
        Tool tool = effectiveTool();
        const AppState &as = appState();
        if (!m_creatingId) {
            VObject o;
            o.type = tool == Tool::Rect ? ObjType::Rect : tool == Tool::Ellipse ? ObjType::Ellipse : tool == Tool::Slice ? ObjType::Slice
                   : toolIsHotspot(tool) ? ObjType::Hotspot : tool == Tool::Polygon ? ObjType::Path : ObjType::Line;
            if (tool == Tool::CircleHotspot) o.webShape = WebShape::Circle;
            o.fillOn = as.fillOn && o.type != ObjType::Line;
            o.fill = as.fill;
            o.strokeOn = as.strokeOn || o.type == ObjType::Line;
            o.stroke = as.stroke;
            o.strokeWidth = as.strokeWidth;
            o.strokeKind = as.strokeKind;
            o.strokeCustom = as.strokeCustom;
            o.strokeParams = as.strokeCustom ? as.strokeParams : stroke::paramsFor(as.strokeKind);
            o.roundness = o.type == ObjType::Rect ? as.roundness : 0;
            // fill edge: Hard / Anti-Alias / Feather
            o.antialias = as.fillEdge != 0;
            o.feather = as.fillEdge == 2 ? as.fillFeather : 0;
            if (o.isWeb()) { o.link = as.linkUrl; o.alt = as.linkAlt; o.target = as.linkTarget; }
            o.name = tool == Tool::Polygon ? (as.polygonStar ? QStringLiteral("Star") : QStringLiteral("Polygon")) : objTypeLabel(o.type);
            if (tool == Tool::Polygon) o.closed = true;
            if (toolIsAutoShape(tool)) {
                const QString kind = toolAutoShapeKind(tool);
                autoshape::init(o, kind);
                o.shapeParams = as.autoShapeParamsFor(kind);
                o.roundness = 0;
                if (autoshape::isLineKind(kind)) { o.fillOn = false; o.strokeOn = true; }
                o.name = autoshape::label(kind);
            }
            o.id = m_doc->newId();
            m_creatingId = o.id;
            m_doc->addObject(o);
        }
        VObject *o = m_doc->find(m_creatingId);
        if (!o) return;
        if (o->type == ObjType::AutoShape && autoshape::isLineKind(o->shapeKind) && o->shapeKind != QLatin1String("spiral")) {
            autoshape::setEndpoints(*o, m_pressDoc, shift ? constrain45(m_pressDoc, d) : d);
        } else if (tool == Tool::Polygon) {
            // dragged from the centre out to a corner; Shift keeps the angle to 15 degree steps
            QLineF l(m_pressDoc, d);
            double ang = l.angle();
            if (shift) ang = qRound(ang / 15.0) * 15.0;
            o->points = polygonPoints(m_pressDoc, l.length(), -ang, as.polygonSides, as.polygonStar,
                                      as.polygonAutoAngle ? -1 : as.polygonAngle);
            o->hIn.clear();
            o->hOut.clear();
        } else if (o->type == ObjType::Line) {
            o->p1 = m_pressDoc;
            o->p2 = shift ? constrain45(m_pressDoc, d) : d;
        } else {
            QPointF end = d;
            if (shift) {
                double s = qMax(qAbs(d.x() - m_pressDoc.x()), qAbs(d.y() - m_pressDoc.y()));
                end = m_pressDoc + QPointF(d.x() < m_pressDoc.x() ? -s : s, d.y() < m_pressDoc.y() ? -s : s);
            }
            QRectF r(m_pressDoc, end);
            if (e->modifiers() & Qt::AltModifier) r = QRectF(m_pressDoc * 2 - end, end);
            o->rect = r.normalized();
        }
        m_doc->notifyChanged();
        return;
    }
    }
}

// Corners of a regular polygon or star centred on `c` with outer radius `r`; the first corner
// points at `angleDeg` (degrees, screen coordinates). For a star, `pointAngle` is the angle at
// each tip in degrees, or < 0 for Fireworks' automatic angle (a regular {n/2} star).
QPolygonF Canvas::polygonPoints(QPointF c, double r, double angleDeg, int sides, bool star, double pointAngle) {
    QPolygonF out;
    sides = qBound(3, sides, 360);
    const double a0 = qDegreesToRadians(angleDeg);
    if (!star) {
        for (int i = 0; i < sides; ++i) {
            const double a = a0 + i * 2 * M_PI / sides;
            out << c + QPointF(r * std::cos(a), r * std::sin(a));
        }
        return out;
    }
    const double half = M_PI / sides;
    double inner;
    if (pointAngle < 0) inner = sides >= 5 ? r * std::cos(2 * half) / std::cos(half) : r * 0.5;
    else {
        const double t = std::tan(qDegreesToRadians(qBound(1.0, pointAngle, 179.0)) / 2);
        inner = r * t / (std::sin(half) + t * std::cos(half));
    }
    inner = qBound(0.0, inner, r);
    for (int i = 0; i < sides; ++i) {
        const double ao = a0 + i * 2 * M_PI / sides;
        out << c + QPointF(r * std::cos(ao), r * std::sin(ao));
        out << c + QPointF(inner * std::cos(ao + half), inner * std::sin(ao + half));
    }
    return out;
}

bool Canvas::pencilTwoColorActive() const {
    return appState().pencilTwoColor && effectiveTool() == Tool::Pencil;
}

void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    // Pencil Left/Right colours: only the button that started the stroke ends it
    if (m_drag == Drag::Paint && pencilTwoColorActive() && e->button() != m_paintButton &&
        (e->button() == Qt::LeftButton || e->button() == Qt::RightButton))
        return;
    Drag drag = m_drag;
    m_drag = Drag::None;
    QPointF d = toDoc(e->position());
    m_noSnap = e->modifiers() & kNoSnapModifier;
    if (drag == Drag::Crop) d = snapPoint(d, true);
    else if (drag == Drag::Create || drag == Drag::PixelRect) d = snapPoint(d);
    m_noSnap = false;
    m_tgtValid = false;
    if (!m_snapX.isEmpty() || !m_snapY.isEmpty()) { m_snapX.clear(); m_snapY.clear(); update(); }
    switch (drag) {
    case Drag::Guide: {
        guideDragTo(e->globalPosition().toPoint());
        finishGuide(rect().contains(e->position().toPoint()));
        break;
    }
    case Drag::Pan: updateCursor(); break;
    case Drag::ZoomRect: {
        QRectF r = m_rubber;
        m_rubber = QRectF();
        if (m_moved && r.width() * m_zoom > 4 && r.height() * m_zoom > 4)
            setView(qMin(width() / r.width(), height() / r.height()), r.center());
        else if (e->modifiers() & Qt::AltModifier) setZoom(nextZoomLevel(m_zoom, false), e->position());
        else setZoom(nextZoomLevel(m_zoom, true), e->position());
        break;
    }
    case Drag::Move:
        if (m_moved) m_doc->commit(QStringLiteral("Move"));
        break;
    case Drag::Resize:
        if (m_moved) m_doc->commit(QStringLiteral("Transform"));
        m_startObjs.clear();
        break;
    case Drag::Transform: {
        static const char *labels[] = {"Scale", "Skew", "Distort", "Perspective"};
        const Tool tl = appState().tool();
        const QString label = m_txHandle == 9 ? QStringLiteral("Rotate")
                            : QString::fromLatin1(labels[tl == Tool::Skew ? 1 : tl == Tool::Distort ? 2 : tl == Tool::Perspective ? 3 : 0]);
        if (m_moved && m_txHandle != 8) {
            m_txApplying = true;
            m_doc->commit(label);
            m_txApplying = false;
        }
        m_startObjs.clear();
        m_txHandle = -1;
        updateTransformBar();
        update();
        break;
    }
    case Drag::PointEdit:
        if (m_moved) m_doc->commit(QStringLiteral("Edit Points"));
        break;
    case Drag::PenHandle:
        break;
    case Drag::VectorPath:
        finishVectorPath();
        break;
    case Drag::RedrawPath:
        finishRedrawPath();
        break;
    case Drag::HandleEdit:
        if (VObject *o = m_doc->singleSelected()) o->dropTrivialHandles();
        m_doc->commit(QStringLiteral("Edit Points"));
        break;
    case Drag::ShapeHandle:
        if (m_moved) m_doc->commit(QStringLiteral("Edit Auto Shape"));
        m_shapeHandle = -1;
        break;
    case Drag::ContentAware:
        finishContentAware();
        break;
    case Drag::GradHandle:
        if (m_moved) {
            const VObject *g = m_doc->singleSelected();
            m_doc->commit(g && g->fillKind == FillKind::Pattern ? QStringLiteral("Move Pattern Handle") : QStringLiteral("Move Gradient Handle"));
        }
        m_gradHandle = -1;
        break;
    case Drag::GradTool:
        if (m_moved || m_gradConverted) m_doc->commit(QStringLiteral("Gradient"));
        m_creatingId = 0;
        break;
    case Drag::Knife:
        if (m_lasso.size() >= 2 && paths::knife(*m_doc, m_lasso)) m_doc->commit(QStringLiteral("Knife"));
        m_lasso.clear();
        break;
    case Drag::Scrub:
        if (m_scrubChanged) m_doc->commit(QStringLiteral("Path Scrubber"));
        m_scrubPrepared.clear();
        m_altHeld = e->modifiers() & Qt::AltModifier;
        updateCursor();
        break;
    case Drag::Sculpt:
        m_doc->commit(effectiveTool() == Tool::ReshapeArea ? QStringLiteral("Reshape Area") : QStringLiteral("Freeform"));
        break;
    case Drag::RubberBand: {
        QSet<quint64> s = (m_mods & Qt::ShiftModifier) ? m_doc->selection : QSet<quint64>();
        const auto &f = m_doc->s.frames[m_doc->curFrame];
        if (const VObject *g = m_doc->enteredGroup ? m_doc->find(m_doc->enteredGroup) : nullptr) {
            // inside an entered group the marquee picks its members
            for (const auto &o : g->children)
                if (o.visible && !o.locked && m_rubber.intersects(o.bounds()) && m_rubber.width() > 1) s.insert(o.id);
        } else {
            for (int li = 0; li < f.layers.size(); ++li) {
                if (!m_doc->s.layers[li].visible || m_doc->s.layers[li].locked) continue;
                for (const auto &o : f.layers[li])
                    if (o.visible && !o.locked && m_rubber.intersects(o.bounds()) && m_rubber.width() > 1) s.insert(o.id);
            }
        }
        m_doc->setSelection(s);
        update();
        break;
    }
    case Drag::SelMove: {
        selectionMoveTo(d);
        const bool pixels = m_floatId != 0;
        if (!pixels && m_selMoveApplied.isNull() && QLineF(e->position(), m_pressView).length() < 3) {
            // a click (no drag) inside the selection does what the tool does anywhere else
            m_noSelMove = true;
            QMouseEvent press(QEvent::MouseButtonPress, m_pressView, e->globalPosition(), Qt::LeftButton, Qt::LeftButton, m_mods);
            mousePressEvent(&press);
            QMouseEvent rel(QEvent::MouseButtonRelease, m_pressView, e->globalPosition(), Qt::LeftButton, Qt::NoButton, m_mods);
            mouseReleaseEvent(&rel);
            m_noSelMove = false;
            return;
        }
        m_floatId = 0;
        m_floatHole = m_floatPixels = QImage();
        if (pixels) m_doc->commit(m_mods & Qt::AltModifier ? QStringLiteral("Duplicate Pixels") : QStringLiteral("Move Pixels"));
        update();
        break;
    }
    case Drag::PixelRect: {
        m_spaceRepos = false;
        QPainterPath p;
        // marquees snap to whole pixels (as in Fireworks): no half-covered edge pixels that would
        // blend a filter into a soft 1 px frame
        m_rubber = QRectF(QPointF(qRound(m_rubber.left()), qRound(m_rubber.top())), QPointF(qRound(m_rubber.right()), qRound(m_rubber.bottom())));
        if (m_rubber.width() >= 1 && m_rubber.height() >= 1) {
            if (m_selOval) p.addEllipse(m_rubber);
            else p.addRect(m_rubber);
            QPainterPath canvas;
            canvas.addRect(QRectF(0, 0, m_doc->s.width, m_doc->s.height));
            p = p.intersected(canvas);
        }
        commitPixelSelection(p, m_mods, m_selOval ? Tool::OvalMarquee : Tool::Marquee);
        break;
    }
    case Drag::Lasso: {
        QPainterPath p;
        if (m_lasso.size() > 2) { p.addPolygon(m_lasso); p.closeSubpath(); }
        m_lasso.clear();
        commitPixelSelection(p, m_mods, Tool::Lasso);
        break;
    }
    case Drag::Area:
        if (m_area.width() < 1 || m_area.height() < 1) {
            // a click without a drag: keep the remembered area if there is one
            m_area = m_doc->exportArea;
            m_areaActive = !m_area.isEmpty();
            publishExportArea();
            update();
            break;
        }
        [[fallthrough]];
    case Drag::AreaMove: case Drag::AreaHandle:
        if (m_area.isEmpty()) { m_area = m_areaStart; }
        m_doc->exportArea = m_area;
        m_areaHandle = -1;
        publishExportArea();
        status(QStringLiteral("Export area %1, %2  %3 x %4: press Enter or double-click inside to export, Esc to cancel")
                   .arg(m_area.x()).arg(m_area.y()).arg(m_area.width()).arg(m_area.height()));
        update();
        break;
    case Drag::Crop:
        if (m_crop.width() < 2 || m_crop.height() < 2) cancelCrop();
        else status(QStringLiteral("Press Enter or double-click inside the crop box to crop"));
        break;
    case Drag::Paint:
        endPaint();
        break;
    case Drag::Create:
        if (m_creatingId) {
            VObject *o = m_doc->find(m_creatingId);
            bool tiny = !o || (o->type == ObjType::AutoShape && autoshape::isLineKind(o->shapeKind) ? (o->rect.width() < 1 && o->rect.height() < 1)
                               : o->type == ObjType::Line ? QLineF(o->p1, o->p2).length() < 1
                               : o->type == ObjType::Path ? (o->bounds().width() < 1 && o->bounds().height() < 1)
                               : (o->rect.width() < 1 || o->rect.height() < 1));
            if (tiny) m_doc->revert();
            else m_doc->commit(toolName(effectiveTool()));
            m_creatingId = 0;
        }
        break;
    default: break;
    }
    Q_UNUSED(d);
    update();
}

void Canvas::finishContentAware() {
    VObject *o = m_paintTarget ? m_doc->find(m_paintTarget) : nullptr;
    const QPainterPath path = m_caPath;
    m_caPath = QPainterPath();
    m_paintTarget = 0;
    // the target may have gone while painting (undo, delete, another document state)
    if (!o || o->type != ObjType::Bitmap || o->image.isNull() || o->image.width() <= 0 || o->image.height() <= 0 || m_caJob) { m_doc->endPixelEdits(false); update(); return; }
    QImage mask(o->image.size(), QImage::Format_Alpha8);
    if (mask.isNull()) { m_doc->endPixelEdits(false); update(); return; }   // allocation failed
    mask.fill(0);
    const double sx = o->image.width() / qMax(1e-9, o->rect.width()), sy = o->image.height() / qMax(1e-9, o->rect.height());
    {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing, false);
        mp.scale(sx, sy);
        mp.translate(-o->rect.topLeft());
        // the brush is in canvas pixels; the pen width is scaled with the painter
        mp.setPen(QPen(Qt::black, qMax(1.0, double(appState().brushSize)), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        mp.drawPath(path);
    }
    // a pixel selection limits the brush, as for the other retouch tools
    if (!m_doc->pixelSelection.isEmpty()) {
        const QTransform t = QTransform::fromTranslate(-o->rect.left(), -o->rect.top()) * QTransform::fromScale(sx, sy);
        const QImage clip = m_doc->selectionMask(t, o->image.size());
        QPainter mp(&mask);
        mp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        if (!clip.isNull()) mp.drawImage(0, 0, clip);
        else mp.fillRect(mask.rect(), Qt::transparent);   // the selection does not reach this bitmap
    }
    bool any = false;
    for (int y = 0; y < mask.height() && !any; ++y) {
        const uchar *m = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) if (m[x] > 127) { any = true; break; }
    }
    if (!any) {
        status(QStringLiteral("Content-Aware Remove: nothing to remove (the stroke is outside the bitmap or the selection)"));
        m_doc->endPixelEdits(false);
        update();
        return;
    }
    // Fill off the GUI thread (a large hole can take seconds): the worker owns copies of the pixels.
    const QImage src = o->image;
    m_caTarget = o->id;
    m_caSrcKey = src.cacheKey();
    m_caPending = path;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_caCancel = cancel;
    m_caJob = new QFutureWatcher<QImage>(this);
    connect(m_caJob, &QFutureWatcher<QImage>::finished, this, &Canvas::applyContentAware);
    QApplication::setOverrideCursor(Qt::BusyCursor);
    status(QStringLiteral("Content-Aware Remove: filling..."));
    m_caJob->setFuture(QtConcurrent::run([src, mask, cancel] {
        return fx2::contentAwareFill(src, mask, 1, [cancel](int) { return !cancel->load(); });
    }));
    update();
}

void Canvas::applyContentAware() {
    QFutureWatcher<QImage> *job = m_caJob;
    if (!job) return;
    m_caJob = nullptr;
    const bool cancelled = m_caCancel && m_caCancel->load();
    m_caCancel.reset();
    const QImage res = cancelled ? QImage() : job->result();
    job->deleteLater();
    QApplication::restoreOverrideCursor();
    m_caPending = QPainterPath();
    VObject *o = m_caTarget ? m_doc->find(m_caTarget) : nullptr;
    m_caTarget = 0;
    if (cancelled || res.isNull()) { m_doc->endPixelEdits(false); update(); return; }
    if (!o || o->type != ObjType::Bitmap || o->image.cacheKey() != m_caSrcKey || o->image.size() != res.size()) {
        status(QStringLiteral("Content-Aware Remove: the bitmap changed while filling; the fill was discarded"));
        m_doc->endPixelEdits(false);
        update();
        return;
    }
    o->image = res;
    m_doc->commit(QStringLiteral("Content-Aware Remove"));
    if (const QString n = m_doc->takePixelEditNote(); !n.isEmpty()) { status(n); update(); return; }
    status(QString());
    update();
}

void Canvas::cancelContentAware() {
    if (!m_caJob) return;
    if (m_caCancel) m_caCancel->store(true);
    QFutureWatcher<QImage> *job = m_caJob;
    m_caJob = nullptr;
    m_caCancel.reset();
    m_caTarget = 0;
    m_caPending = QPainterPath();
    m_doc->endPixelEdits(false);
    job->disconnect(this);
    // the worker only touches its own copies; it stops at its next progress check
    job->deleteLater();
    QApplication::restoreOverrideCursor();
    update();
}

Canvas::~Canvas() {
    cancelContentAware();
    if (m_liveJob && m_liveJob->isRunning())
        m_liveJob->waitForFinished();
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    QPointF d = toDoc(e->position());
    Tool tool = effectiveTool();
    if (tool == Tool::PolyLasso && !m_poly.isEmpty()) {
        // the second click of the double-click already added a point
        if (m_poly.size() > 1 && QLineF(m_poly.last(), m_poly[m_poly.size() - 2]).length() < 3 / m_zoom) m_poly.removeLast();
        finishPolyLasso();
        return;
    }
    if ((tool == Tool::Pen || tool == Tool::PolyHotspot || tool == Tool::PolySlice) && !m_penPts.isEmpty()) {
        // the second click of the double-click already added a point
        if (m_penPts.size() > 1 && QLineF(m_penPts.last(), m_penPts[m_penPts.size() - 2]).length() < 3 / m_zoom) {
            m_penPts.removeLast();
            if (m_penIn.size() > m_penPts.size()) { m_penIn.removeLast(); m_penOut.removeLast(); }
        }
        m_drag = Drag::None;
        finishPen(false);
        return;
    }
    if (tool == Tool::Crop && m_cropActive && m_crop.contains(d)) { applyCrop(); return; }
    if (tool == Tool::ExportArea && m_areaActive && QRectF(m_area).contains(d)) { m_drag = Drag::None; requestAreaExport(); return; }
    if (toolIsSelect(tool) || tool == Tool::Subselect) {
        quint64 hit = pickObject(d, tool == Tool::Subselect);
        VObject *o = hit ? m_doc->find(hit) : nullptr;
        if (o && o->locked) o = nullptr;
        if (o && o->type == ObjType::Group) {
            // double-click a group: work inside it (Fireworks); the member under the pointer is selected
            m_doc->enterGroup(o->id);
            const quint64 member = pickObject(d, false);
            if (member && member != o->id) m_doc->selectOnly(member);
            m_drag = Drag::None;
            m_doc->notifyChanged();
            update();
            return;
        }
        if (o && o->type == ObjType::Text) editText(o);
        else if (o && o->type == ObjType::Instance) emit editSymbolRequested(o->symbolId);
    }
}

// Library panel drags carry "<document pointer>:<symbol id>"; only symbols of this document drop here.
static quint64 droppedSymbol(const QMimeData *m, const Document *doc) {
    const QByteArray d = m ? m->data(QStringLiteral("application/x-libreworks-symbol")) : QByteArray();
    const int c = d.indexOf(':');
    if (c < 0 || d.left(c).toULongLong() != quintptr(doc)) return 0;
    return d.mid(c + 1).toULongLong();
}

// True when the drop contains importable images and/or native .lwd/.bon documents.
static bool mimeHasLocalImages(const QMimeData *m) {
    if (!m || !m->hasUrls()) return false;
    for (const QUrl &u : m->urls()) {
        if (!u.isLocalFile()) continue;
        const QString path = u.toLocalFile();
        if (brand::isNativeDocumentPath(path)) return true;
        QFile f(path);
        QByteArray head;
        if (f.open(QIODevice::ReadOnly)) head = f.read(64);
        Fmt fmt = ImageIO::detect(head);
        if (fmt == Fmt::Unknown) fmt = ImageIO::fromExtension(path);
        if (fmt != Fmt::Unknown && ImageIO::canRead(fmt)) return true;
    }
    return false;
}

void Canvas::dragEnterEvent(QDragEnterEvent *e) {
    if (droppedSymbol(e->mimeData(), m_doc) || mimeHasLocalImages(e->mimeData())) e->acceptProposedAction();
    else e->ignore();
}

void Canvas::dragMoveEvent(QDragMoveEvent *e) {
    if (droppedSymbol(e->mimeData(), m_doc) || mimeHasLocalImages(e->mimeData())) e->acceptProposedAction();
    else e->ignore();
}

void Canvas::dropEvent(QDropEvent *e) {
    if (const quint64 id = droppedSymbol(e->mimeData(), m_doc)) {
        if (!m_doc->insertInstance(id, toDoc(e->position())).isEmpty()) m_doc->commit(QStringLiteral("Insert Symbol"));
        e->acceptProposedAction();
        return;
    }
    if (!mimeHasLocalImages(e->mimeData())) { e->ignore(); return; }
    const QPointF docPt = toDoc(e->position());
    QSet<quint64> sel;
    int n = 0;
    QStringList documents;
    for (const QUrl &u : e->mimeData()->urls()) {
        if (!u.isLocalFile()) continue;
        const QString path = u.toLocalFile();
        if (brand::isNativeDocumentPath(path)) { documents << path; continue; }
        ImageSeq seq;
        QString err;
        if (!ImageIO::read(path, seq, &err) || seq.frames.isEmpty()) continue;
        VObject o;
        o.id = m_doc->newId();
        o.type = ObjType::Bitmap;
        o.name = QFileInfo(path).fileName();
        o.image = seq.frames[0].image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        o.stampInterpolation();
        // place so the drop point is the top-left of the (pre-trim) image, then trim transparent borders
        o.rect = QRectF(docPt + QPointF(n * 12, n * 12), o.image.size());
        o.fillOn = o.strokeOn = false;
        pxedit::trimToAlpha(o);
        m_doc->objects(m_doc->drawingLayer()).append(o);
        sel.insert(o.id);
        ++n;
        // animated: remaining frames into subsequent document frames
        for (int i = 1; i < seq.frames.size(); ++i) {
            while (m_doc->curFrame + i >= m_doc->s.frames.size()) {
                Frame f; f.layers.resize(m_doc->s.layers.size());
                f.name = QStringLiteral("Frame %1").arg(m_doc->s.frames.size() + 1);
                m_doc->s.frames.append(f);
            }
            VObject fi = o;
            fi.id = m_doc->newId();
            fi.image = seq.frames[i].image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            fi.rect = QRectF(docPt + QPointF(n * 12, n * 12), fi.image.size());
            pxedit::trimToAlpha(fi);
            m_doc->s.frames[m_doc->curFrame + i].layers[m_doc->drawingLayer()].append(fi);
        }
    }
    if (n == 0 && documents.isEmpty()) { e->ignore(); return; }
    if (n > 0) {
        m_doc->commit(n == 1 ? QStringLiteral("Drop Image") : QStringLiteral("Drop Images"));
        m_doc->setSelection(sel);
    }
    e->acceptProposedAction();
    // Queue opens until dropEvent returns — opening a document can re-enter the window.
    for (const QString &path : documents) emit openDocumentRequested(path);
}

void Canvas::wheelEvent(QWheelEvent *e) {
    if (e->modifiers() & Qt::ControlModifier) {
        double f = e->angleDelta().y() > 0 ? 1.25 : 0.8;
        setZoom(m_zoom * f, e->position());
    } else {
        QPointF delta = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 2.0 : QPointF(e->pixelDelta());
        if (e->modifiers() & Qt::ShiftModifier) delta = QPointF(delta.y(), delta.x());
        m_origin += delta;
        m_needFit = false;
        clampOrigin();
        update();
        emit viewChanged();
    }
    e->accept();
}

// ---------------------------------------------------------------- tablet (v0.3.1)
tablet::Sample Canvas::sampleFromEvent(const QTabletEvent *e) {
    tablet::Sample s;
    const QPointingDevice *dev = e->pointingDevice();
    // a device that does not report pressure presses at 1.0, like a mouse
    const bool hasPressure = !dev || dev->capabilities().testFlag(QInputDevice::Capability::Pressure);
    const double raw = hasPressure ? e->pressure() : 1.0;
    s.pressure = tablet::mapCurve(appState().prefs.tabletCurve, raw);
    s.tiltX = e->xTilt();
    s.tiltY = e->yTilt();
    s.rotation = e->rotation();
    s.hasTilt = !dev || dev->capabilities().testAnyFlags(QInputDevice::Capability::XTilt | QInputDevice::Capability::YTilt |
                                                          QInputDevice::Capability::Rotation) ||
                s.tiltX != 0 || s.tiltY != 0 || s.rotation != 0;
    return s;
}

// The pen drives the same press / move / release code as the mouse, with its sample in m_pen; the event is
// accepted so Qt sends no extra synthesised mouse event. The eraser end switches to the Eraser while it is in
// use (Preferences > Tablet), and the tip brings the previous tool back.
void Canvas::tabletEvent(QTabletEvent *e) {
    AppState &as = appState();
    const bool eraserEnd = e->pointerType() == QPointingDevice::PointerType::Eraser;
    const bool idle = m_drag == Drag::None;
    if (as.prefs.tabletEraser && idle && (e->type() == QEvent::TabletPress || e->type() == QEvent::TabletMove)) {
        if (eraserEnd && !m_eraserSwitched && as.tool() != Tool::Eraser) {
            m_eraserRestore = as.tool();
            m_eraserSwitched = true;
            as.setTool(Tool::Eraser);
            status(QStringLiteral("Eraser (pen eraser end)"));
        } else if (!eraserEnd && m_eraserSwitched) {
            m_eraserSwitched = false;
            if (as.tool() == Tool::Eraser) as.setTool(m_eraserRestore);
        }
    }
    QEvent::Type mt;
    switch (e->type()) {
    case QEvent::TabletPress: mt = QEvent::MouseButtonPress; break;
    case QEvent::TabletRelease: mt = QEvent::MouseButtonRelease; break;
    case QEvent::TabletMove: mt = QEvent::MouseMove; break;
    default: e->ignore(); return;
    }
    m_pen = sampleFromEvent(e);
    if (mt == QEvent::MouseButtonRelease) m_pen.pressure = m_lastPen.pressure;   // a lifted pen reads 0: keep the stroke's end
    Qt::MouseButton button = e->button();
    if (mt != QEvent::MouseMove && button == Qt::NoButton) button = Qt::LeftButton;
    Qt::MouseButtons buttons = e->buttons();
    if (mt == QEvent::MouseButtonPress) buttons |= button;
    if (mt == QEvent::MouseButtonRelease) buttons &= ~button;
    QMouseEvent me(mt, e->position(), e->globalPosition(), mt == QEvent::MouseMove ? Qt::NoButton : button, buttons, e->modifiers(),
                   e->pointingDevice());
    m_fromTablet = true;
    switch (mt) {
    case QEvent::MouseButtonPress: mousePressEvent(&me); break;
    case QEvent::MouseButtonRelease: mouseReleaseEvent(&me); break;
    default: mouseMoveEvent(&me); break;
    }
    m_fromTablet = false;
    e->accept();
}

bool Canvas::event(QEvent *e) {
    if (e->type() == QEvent::ShortcutOverride) {
        auto *k = static_cast<QKeyEvent *>(e);
        auto &sm = shortcuts::Manager::get();
        // tool-in-progress: take confirm / cancel / remove-last over menu shortcuts
        if (!m_poly.isEmpty()) {
            if (sm.matches(QStringLiteral("canvas.confirm"), k) || sm.matches(QStringLiteral("canvas.cancel"), k)
                || sm.matches(QStringLiteral("canvas.removeLastPoint"), k)) {
                e->accept();
                return true;
            }
        }
        if ((m_areaActive && effectiveTool() == Tool::ExportArea) || m_cropActive || !m_penPts.isEmpty()) {
            if (sm.matches(QStringLiteral("canvas.confirm"), k) || sm.matches(QStringLiteral("canvas.cancel"), k)) {
                e->accept();
                return true;
            }
        }
    }
    return QWidget::event(e);
}

void Canvas::keyPressEvent(QKeyEvent *e) {
    auto &sm = shortcuts::Manager::get();
    // temporary pan (default Space) — hold
    if (sm.matches(QStringLiteral("canvas.tempPan"), e)) {
        if (m_drag == Drag::PixelRect) {
            if (!e->isAutoRepeat()) m_spaceRepos = true;
            return;
        }
        if (!e->isAutoRepeat() && m_drag == Drag::None) {
            m_space = true;
            updateCursor();
            return;
        }
    }
    // Alt as a held modifier for cursors stays fixed (see Help > Modifiers)
    if (e->key() == Qt::Key_Alt && !m_altHeld) { m_altHeld = true; updateCursor(); }

    if (sm.matches(QStringLiteral("canvas.confirm"), e)) {
        if (m_cropActive) { applyCrop(); return; }
        if (m_areaActive && effectiveTool() == Tool::ExportArea) { requestAreaExport(); return; }
        if (!m_penPts.isEmpty()) { finishPen(false); return; }
        if (!m_poly.isEmpty()) { finishPolyLasso(); return; }
    }
    if (!m_poly.isEmpty() && sm.matches(QStringLiteral("canvas.removeLastPoint"), e)) {
        m_poly.removeLast();
        update();
        return;
    }
    if (sm.matches(QStringLiteral("canvas.cancel"), e)) {
        if (m_cropActive) { cancelCrop(); return; }
        if (m_areaActive) { cancelExportArea(); return; }
        if (!m_penPts.isEmpty()) { cancelPen(); return; }
        if (!m_poly.isEmpty()) { cancelPolyLasso(); return; }
        if (!m_doc->pixelSelection.isEmpty()) { m_doc->clearPixelSelection(); update(); return; }
        if (m_doc->enteredGroup) {
            const quint64 g = m_doc->enteredGroup;
            m_doc->exitGroup();
            m_doc->selectOnly(g);
            m_doc->notifyChanged();
            update();
            return;
        }
        m_doc->clearSelection();
        return;
    }
    if (sm.matches(QStringLiteral("canvas.nudgeLeft10"), e)) { nudge({-10, 0}); return; }
    if (sm.matches(QStringLiteral("canvas.nudgeRight10"), e)) { nudge({10, 0}); return; }
    if (sm.matches(QStringLiteral("canvas.nudgeUp10"), e)) { nudge({0, -10}); return; }
    if (sm.matches(QStringLiteral("canvas.nudgeDown10"), e)) { nudge({0, 10}); return; }
    if (sm.matches(QStringLiteral("canvas.nudgeLeft"), e)) { nudge({-1, 0}); return; }
    if (sm.matches(QStringLiteral("canvas.nudgeRight"), e)) { nudge({1, 0}); return; }
    if (sm.matches(QStringLiteral("canvas.nudgeUp"), e)) { nudge({0, -1}); return; }
    if (sm.matches(QStringLiteral("canvas.nudgeDown"), e)) { nudge({0, 1}); return; }
    QWidget::keyPressEvent(e);
}

void Canvas::keyReleaseEvent(QKeyEvent *e) {
    auto &sm = shortcuts::Manager::get();
    const bool tempPanKey = sm.matches(QStringLiteral("canvas.tempPan"), e)
        || [&] {
            // release may not include the same modifiers; also match the bare key of any tempPan sequence
            for (const QKeySequence &k : sm.sequences(QStringLiteral("canvas.tempPan")))
                if (!k.isEmpty() && k[0].key() == e->key()) return true;
            return false;
        }();
    if (tempPanKey && m_spaceRepos) {
        if (!e->isAutoRepeat()) m_spaceRepos = false;
        return;
    }
    if (e->key() == Qt::Key_Alt && m_altHeld) { m_altHeld = false; updateCursor(); }
    if (tempPanKey && !e->isAutoRepeat()) {
        m_space = false;
        if (m_drag == Drag::Pan) m_drag = Drag::None;
        updateCursor();
        return;
    }
    QWidget::keyReleaseEvent(e);
}

void Canvas::leaveEvent(QEvent *) {
    if (m_hoverId) { m_hoverId = 0; }
    const QRect old = m_lastCursorView;
    m_lastCursorView = QRect();
    if (!old.isEmpty()) update(old);
    else update();
}

// ---------------------------------------------------------------- commands
void Canvas::nudge(QPointF d) {
    // a selection tool moves the marquee itself (1 px; Shift: 10 px)
    const Tool t = appState().tool();
    if (!m_doc->pixelSelection.isEmpty() &&
        (t == Tool::Marquee || t == Tool::OvalMarquee || t == Tool::Lasso || t == Tool::PolyLasso || t == Tool::MagicWand)) {
        m_doc->moveSelection(d);
        update();
        return;
    }
    auto objs = m_doc->selectedEditable();
    if (objs.isEmpty()) return;
    for (VObject *o : objs) o->moveBy(d);
    m_doc->commit(QStringLiteral("Move"));
}

void Canvas::applyCrop() {
    if (!m_cropActive) return;
    QRect r = m_crop.toRect().normalized();
    m_cropActive = false;
    if (r.width() < 1 || r.height() < 1) { publishCrop(); update(); return; }
    m_doc->resizeCanvas(r.width(), r.height(), -r.topLeft());
    m_doc->clearPixelSelection();
    m_doc->commit(QStringLiteral("Crop Document"));
    publishCrop();
    fitToWindow();
}

void Canvas::cancelCrop() {
    m_cropActive = false;
    publishCrop();
    update();
}

// Mirror the crop box into AppState so the Property Inspector's Crop fields follow it.
void Canvas::publishCrop() {
    AppState &as = appState();
    as.cropActive = m_cropActive;
    as.cropRect = m_cropActive ? m_crop.normalized() : QRectF(0, 0, m_doc->s.width, m_doc->s.height);
    emit as.cropChanged();
}

// ---------------------------------------------------------------- Export Area
QPoint Canvas::areaSnap(const QPointF &d, bool edges) const {
    // whole pixels, then the canvas edges within 8 screen px; the result is clamped to the canvas
    const int W = m_doc->s.width, H = m_doc->s.height;
    int x = qRound(d.x()), y = qRound(d.y());
    if (edges) {
        const double tol = 8 / m_zoom;
        if (qAbs(d.x()) <= tol) x = 0; else if (qAbs(d.x() - W) <= tol) x = W;
        if (qAbs(d.y()) <= tol) y = 0; else if (qAbs(d.y() - H) <= tol) y = H;
    }
    return QPoint(qBound(0, x, W), qBound(0, y, H));
}

int Canvas::areaHandleAt(const QPointF &v) const {
    if (!m_areaActive) return -1;
    const QRectF va(toView(QPointF(m_area.topLeft())), toView(QPointF(m_area.x() + m_area.width(), m_area.y() + m_area.height())));
    const QPointF hs[8] = {va.topLeft(), va.topRight(), va.bottomLeft(), va.bottomRight(),
                           QPointF(va.center().x(), va.top()), QPointF(va.center().x(), va.bottom()),
                           QPointF(va.left(), va.center().y()), QPointF(va.right(), va.center().y())};
    for (int i = 0; i < 8; ++i)
        if (qAbs(v.x() - hs[i].x()) <= 5 && qAbs(v.y() - hs[i].y()) <= 5) return i;
    return -1;
}

void Canvas::setExportArea(const QRect &r) {
    const QRect a = r.normalized() & QRect(0, 0, m_doc->s.width, m_doc->s.height);
    m_areaActive = !a.isEmpty();
    m_area = a;
    m_doc->exportArea = a;
    publishExportArea();
    update();
}

void Canvas::requestAreaExport() {
    if (!m_areaActive || m_area.isEmpty()) return;
    m_doc->exportArea = m_area;
    appState().lastExportArea = m_area;
    const QRect r = m_area;
    emit appState().exportAreaRequested(r);
}

void Canvas::cancelExportArea() {
    m_areaActive = false;
    m_area = QRect();
    m_doc->exportArea = QRect();
    publishExportArea();
    update();
}

// Mirror the export area into AppState so the Property Inspector's fields follow it.
void Canvas::publishExportArea() {
    AppState &as = appState();
    as.exportAreaActive = m_areaActive;
    as.exportAreaRect = m_areaActive ? m_area : QRect();
    emit as.exportAreaChanged();
}

void Canvas::finishPen(bool close) {
    if (m_penTool == Tool::PolyHotspot || m_penTool == Tool::PolySlice) {
        if (m_penPts.size() >= 3) {
            VObject o;
            o.type = m_penTool == Tool::PolySlice ? ObjType::Slice : ObjType::Hotspot;
            o.webShape = WebShape::Polygon;
            o.link = appState().linkUrl;
            o.alt = appState().linkAlt;
            o.target = appState().linkTarget;
            o.points = m_penPts;
            o.rect = o.points.boundingRect();
            o.closed = true;
            o.name = m_penTool == Tool::PolySlice ? QStringLiteral("Slice") : QStringLiteral("Hotspot");
            m_doc->addObject(o);
            m_doc->commit(m_penTool == Tool::PolySlice ? QStringLiteral("Polygon Slice") : QStringLiteral("Polygon Hotspot"));
        }
        m_penPts.clear();
        m_penIn.clear();
        m_penOut.clear();
        update();
        return;
    }
    if (m_penPts.size() >= 2) {
        VObject o;
        o.type = ObjType::Path;
        o.points = m_penPts;
        if (m_penIn.size() == m_penPts.size()) { o.hIn = m_penIn; o.hOut = m_penOut; o.dropTrivialHandles(); }
        o.closed = close;
        o.fillOn = close && appState().fillOn;
        o.fill = appState().fill;
        o.strokeOn = true;
        o.stroke = appState().stroke;
        o.strokeWidth = appState().strokeWidth;
        o.strokeKind = appState().strokeKind;
        o.strokeCustom = appState().strokeCustom;
        o.strokeParams = appState().strokeCustom ? appState().strokeParams : stroke::paramsFor(appState().strokeKind);
        o.antialias = appState().fillEdge != 0;
        o.feather = appState().fillEdge == 2 ? appState().fillFeather : 0;
        o.name = QStringLiteral("Path");
        m_doc->addObject(o);
        m_doc->commit(QStringLiteral("Pen"));
    }
    m_penPts.clear();
    m_penIn.clear();
    m_penOut.clear();
    update();
}

void Canvas::cancelPen() {
    m_penPts.clear();
    m_penIn.clear();
    m_penOut.clear();
    update();
}

static QVector<double> remapPolyValues(const QPolygonF &raw, const QVector<double> &vals, const QPolygonF &simp)
{
    QVector<double> out;
    if (raw.size() != vals.size() || simp.isEmpty() || raw.isEmpty()) return out;
    // Douglas-Peucker keeps a subset of the raw vertices; match by pointer-identity of points.
    QHash<quint64, double> at;
    auto key = [](const QPointF &p) {
        return (quint64(qint32(qRound(p.x() * 64))) << 32) ^ quint32(qint32(qRound(p.y() * 64)));
    };
    for (int i = 0; i < raw.size(); ++i) at[key(raw[i])] = vals[i];
    out.reserve(simp.size());
    for (const QPointF &p : simp) {
        const auto it = at.constFind(key(p));
        if (it != at.cend()) { out << *it; continue; }
        // Fallback: nearest raw vertex
        int best = 0;
        double bd = 1e99;
        for (int i = 0; i < raw.size(); ++i) {
            const double d = QLineF(raw[i], p).length();
            if (d < bd) { bd = d; best = i; }
        }
        out << vals[best];
    }
    return out;
}

void Canvas::finishVectorPath()
{
    QPolygonF raw = m_freehand;
    QVector<double> rawP = m_freehandPress;
    QVector<double> rawS = m_freehandSpeed;
    m_freehand.clear();
    m_freehandPress.clear();
    m_freehandSpeed.clear();
    if (raw.size() < 2) { update(); return; }
    const int prec = qBound(1, appState().pathPrecision, 10);
    const double tol = (11 - prec) * 0.35;
    const QPolygonF pts = paths::simplifyPolyline(raw, qMax(0.15, tol), false);
    if (pts.size() < 2) { update(); return; }
    VObject o;
    o.type = ObjType::Path;
    o.points = pts;
    o.closed = false;
    o.fillOn = false;
    o.strokeOn = appState().strokeOn;
    o.stroke = appState().stroke;
    o.strokeWidth = appState().strokeWidth;
    o.strokeKind = appState().strokeKind;
    o.strokeCustom = appState().strokeCustom;
    o.strokeParams = appState().strokeCustom ? appState().strokeParams : stroke::paramsFor(appState().strokeKind);
    o.antialias = appState().fillEdge != 0;
    o.feather = appState().fillEdge == 2 ? appState().fillFeather : 0;
    o.name = QStringLiteral("Path");
    if (rawP.size() == raw.size()) o.ptPressure = remapPolyValues(raw, rawP, pts);
    if (rawS.size() == raw.size()) o.ptSpeed = remapPolyValues(raw, rawS, pts);
    if (appState().pathPressure && o.strokeOn) {
        const double pr = qBound(0.15, m_pen.pressure > 0 ? m_pen.pressure : 1.0, 1.0);
        o.strokeWidth = qMax(0.25, o.strokeWidth * pr);
    }
    m_doc->addObject(o);
    m_doc->commit(QStringLiteral("Vector Path"));
    update();
}

void Canvas::finishRedrawPath()
{
    QPolygonF raw = m_freehand;
    QVector<double> rawP = m_freehandPress;
    QVector<double> rawS = m_freehandSpeed;
    const quint64 id = m_redrawId;
    m_freehand.clear();
    m_freehandPress.clear();
    m_freehandSpeed.clear();
    m_redrawId = 0;
    VObject *o = m_doc->find(id);
    if (!o || o->type != ObjType::Path || raw.size() < 2 || o->points.size() < 2) { update(); return; }
    auto nearest = [](const QPolygonF &pts, const QPointF &p) {
        int best = 0;
        double bd = 1e99;
        for (int i = 0; i < pts.size(); ++i) {
            const double d = QLineF(pts[i], p).length();
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    };
    int a = nearest(o->points, raw.first());
    int b = nearest(o->points, raw.last());
    if (a > b) qSwap(a, b);
    const int prec = qBound(1, appState().pathPrecision, 10);
    const QPolygonF mid = paths::simplifyPolyline(raw, qMax(0.15, (11 - prec) * 0.35), false);
    QPolygonF next;
    for (int i = 0; i < a; ++i) next << o->points[i];
    next += mid;
    for (int i = b + 1; i < o->points.size(); ++i) next << o->points[i];
    if (next.size() < 2) { update(); return; }
    auto spliceSense = [&](QVector<double> &arr, const QVector<double> &rawV) {
        const bool had = arr.size() == o->points.size();
        const bool haveNew = rawV.size() == raw.size();
        if (!had && !haveNew) { arr.clear(); return; }
        QVector<double> midV;
        if (haveNew) midV = remapPolyValues(raw, rawV, mid);
        QVector<double> out;
        for (int i = 0; i < a; ++i) out << (had ? (i < arr.size() ? arr[i] : 1.0) : 1.0);
        if (haveNew) out += midV;
        else for (int i = 0; i < mid.size(); ++i) out << 1.0;
        for (int i = b + 1; i < o->points.size(); ++i) out << (had ? (i < arr.size() ? arr[i] : 1.0) : 1.0);
        arr = out;
    };
    spliceSense(o->ptPressure, rawP);
    spliceSense(o->ptSpeed, rawS);
    o->points = next;
    o->hIn.clear();
    o->hOut.clear();
    o->rect = o->points.boundingRect();
    m_doc->commit(QStringLiteral("Redraw Path"));
    update();
}

void Canvas::editText(VObject *o) {
    bool ok = false;
    QString text = QInputDialog::getMultiLineText(this, QStringLiteral("Edit Text"), QStringLiteral("Text:"), o->text, &ok);
    if (!ok || text == o->text) return;
    quint64 id = o->id;
    if (VObject *t = m_doc->find(id)) {
        t->text = text;
        t->fitTextBox();
        m_doc->commit(QStringLiteral("Edit Text"));
    }
}

// ---------------------------------------------------------------- Free Transform
double Canvas::snapAngle(double degrees, double step) { return std::round(degrees / step) * step; }

bool Canvas::ensureTransform() {
    if (!toolIsTransform(appState().tool())) { m_tx.valid = false; return false; }
    const auto objs = m_doc->selectedEditable();
    if (objs.isEmpty()) { m_tx.valid = false; return false; }
    QSet<quint64> ids;
    for (VObject *o : objs) ids.insert(o->id);
    if (m_tx.valid && m_tx.ids == ids && m_tx.rev == m_docRev) return true;
    m_tx = TxSession();
    m_tx.valid = true;
    m_tx.ids = ids;
    m_tx.rev = m_docRev;
    // one rotated / skewed object: the box follows its own transform
    if (objs.size() == 1 && objs[0]->usesXform() && objs[0]->hasXform() && objs[0]->xform.type() != QTransform::TxProject) {
        const VObject &o = *objs[0];
        m_tx.quad = o.xform.map(xf::corners(o.untransformed().bounds()));
        m_tx.angle = xf::rotationOf(o);
    } else {
        m_tx.quad = xf::corners(m_doc->selectionBounds());
        m_tx.angle = 0;
    }
    QPointF c;
    for (const QPointF &q : m_tx.quad) c += q / 4.0;
    m_tx.centre = c;
    return true;
}

bool Canvas::transformActive() { return ensureTransform(); }
QPolygonF Canvas::transformQuad() { return ensureTransform() ? m_tx.quad : QPolygonF(); }
QPointF Canvas::transformCentre() { return ensureTransform() ? m_tx.centre : QPointF(); }
double Canvas::transformAngle() { return ensureTransform() ? m_tx.angle : 0.0; }
void Canvas::setTransformCentre(const QPointF &c) {
    if (!ensureTransform()) return;
    m_tx.centre = c;
    update();
}

QPointF Canvas::transformHandleView(int i) {
    if (!ensureTransform()) return {};
    const QPolygonF &q = m_tx.quad;
    QPointF d;
    if (i < 4) d = q[i];
    else if (i < 8) d = (q[i - 4] + q[(i - 3) % 4]) / 2.0;
    else d = m_tx.centre;
    return toView(d);
}

void Canvas::updateTransformBar() {
    if (!m_txBar) return;
    const bool on = ensureTransform();
    if (on) {
        QSignalBlocker b(m_txAngleBox);
        double a = std::fmod(m_tx.angle, 360.0);
        if (a > 180) a -= 360;
        if (a <= -180) a += 360;
        m_txAngleBox->setValue(a);
        m_txBar->adjustSize();
        m_txBar->move(8, 8);
        m_txBar->raise();
    }
    m_txBar->setVisible(on);
}

int Canvas::transformHandleAt(const QPointF &vp, bool *rotateZone) {
    if (rotateZone) *rotateZone = false;
    if (!ensureTransform()) return -1;
    if (QLineF(transformHandleView(8), vp).length() <= 6) return 8;
    for (int i = 0; i < 8; ++i)
        if (QLineF(transformHandleView(i), vp).length() <= 6) return i;
    QPolygonF qv;
    for (int i = 0; i < 4; ++i) qv << transformHandleView(i);
    if (!qv.containsPoint(vp, Qt::OddEvenFill)) {
        // outside the box, close to it: rotate
        double dist = 1e9;
        for (int i = 0; i < 4; ++i) {
            const QLineF edge(qv[i], qv[(i + 1) % 4]);
            const QPointF dv = edge.p2() - edge.p1();
            const double len2 = QPointF::dotProduct(dv, dv);
            const double t = len2 > 0 ? qBound(0.0, QPointF::dotProduct(vp - edge.p1(), dv) / len2, 1.0) : 0;
            dist = qMin(dist, QLineF(edge.p1() + dv * t, vp).length());
        }
        if (dist <= 30) { if (rotateZone) *rotateZone = true; return 9; }
    }
    return -1;
}

void Canvas::applyTransform(const QTransform &t) {
    m_txApplying = true;
    for (auto it = m_startObjs.constBegin(); it != m_startObjs.constEnd(); ++it) {
        VObject *o = m_doc->find(it.key());
        if (!o) continue;
        *o = it.value();
        xf::transformObject(*o, t);
    }
    m_doc->notifyChanged();
    m_txApplying = false;
}

bool Canvas::rotateSelection(double degrees) {
    if (!ensureTransform() || qAbs(degrees) < 1e-9) return false;
    m_startObjs.clear();
    for (VObject *o : m_doc->selectedEditable()) m_startObjs.insert(o->id, *o);
    const QTransform t = xf::rotationAbout(m_tx.centre, degrees);
    applyTransform(t);
    m_startObjs.clear();
    m_tx.quad = t.map(m_tx.quad);
    m_tx.angle += degrees;
    m_txApplying = true;
    m_doc->commit(QStringLiteral("Rotate"));
    m_txApplying = false;
    updateTransformBar();
    update();
    return true;
}

void Canvas::transformTo(const QPointF &d, Qt::KeyboardModifiers mods) {
    const QPolygonF &q0 = m_txQuad0;
    const QPointF delta = d - m_pressDoc;
    const bool shift = mods & Qt::ShiftModifier, alt = mods & Qt::AltModifier;
    if (m_txHandle == 8) { m_tx.centre = m_txCentre0 + delta; update(); return; }
    QPolygonF qn = q0;
    if (m_txHandle == 9) {
        const QPointF c = m_txCentre0;
        double a = qRadiansToDegrees(qAtan2(d.y() - c.y(), d.x() - c.x()) - qAtan2(m_pressDoc.y() - c.y(), m_pressDoc.x() - c.x()));
        if (shift) a = snapAngle(m_txAngle0 + a) - m_txAngle0;   // Shift: 15 degree steps of the total rotation
        qn = xf::rotationAbout(c, a).map(q0);
        m_tx.angle = m_txAngle0 + a;
        m_tx.centre = c;
    } else {
        const Tool tool = appState().tool();
        static const int edgeA[4] = {0, 1, 3, 0}, edgeB[4] = {1, 2, 2, 3};   // top, right, bottom, left
        const QPointF u = q0[1] - q0[0], v = q0[3] - q0[0];
        const auto along = [](const QPointF &dv, const QPointF &axis) {
            const double l2 = QPointF::dotProduct(axis, axis);
            return l2 > 1e-12 ? axis * (QPointF::dotProduct(dv, axis) / l2) : QPointF();
        };
        if (tool == Tool::Scale) {
            // work in the box's own (possibly rotated / distorted) parameter space
            static const QPolygonF unit({QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)});
            bool ok = false;
            const QTransform toBox = xf::quadMap(unit, q0, &ok);
            bool inv = false;
            const QTransform fromBox = toBox.inverted(&inv);
            if (!ok || !inv) return;
            const QPointF pm = fromBox.map(d);
            double s0 = 0, t0 = 0, s1 = 1, t1 = 1;
            const int h = m_txHandle;
            const bool left = h == 0 || h == 3 || h == 7, right = h == 1 || h == 2 || h == 5;
            const bool top = h == 0 || h == 1 || h == 4, bottom = h == 2 || h == 3 || h == 6;
            if (left) s0 = pm.x();
            if (right) s1 = pm.x();
            if (top) t0 = pm.y();
            if (bottom) t1 = pm.y();
            if (alt) {   // Alt: scale about the centre
                if (left) s1 = 1 - s0;
                if (right) s0 = 1 - s1;
                if (top) t1 = 1 - t0;
                if (bottom) t0 = 1 - t1;
            }
            if (shift && h < 4) {   // Shift on a corner: keep the proportions
                const double k = qMax(qAbs(s1 - s0), qAbs(t1 - t0));
                const double ks = (s1 - s0) < 0 ? -k : k, kt = (t1 - t0) < 0 ? -k : k;
                if (alt) { s0 = 0.5 - ks / 2; s1 = 0.5 + ks / 2; t0 = 0.5 - kt / 2; t1 = 0.5 + kt / 2; }
                else {
                    if (left) s0 = s1 - ks; else s1 = s0 + ks;
                    if (top) t0 = t1 - kt; else t1 = t0 + kt;
                }
            }
            if (qAbs(s1 - s0) < 1e-4 || qAbs(t1 - t0) < 1e-4) return;
            qn = toBox.map(QPolygonF({QPointF(s0, t0), QPointF(s1, t0), QPointF(s1, t1), QPointF(s0, t1)}));
        } else if (m_txHandle >= 4) {
            const int e = m_txHandle - 4;
            const QPointF axis = (e == 0 || e == 2) ? u : v;
            const QPointF mv = tool == Tool::Distort ? delta : along(delta, axis);   // Skew / Perspective: slide the edge along itself
            qn[edgeA[e]] += mv;
            qn[edgeB[e]] += mv;
        } else {
            const int c = m_txHandle;
            const QPointF du = along(delta, u), dv = along(delta, v);
            const bool horizontal = QPointF::dotProduct(du, du) >= QPointF::dotProduct(dv, dv);
            const int partnerH = c == 0 ? 1 : c == 1 ? 0 : c == 2 ? 3 : 2;   // same top / bottom edge
            const int partnerV = c == 0 ? 3 : c == 3 ? 0 : c == 1 ? 2 : 1;   // same left / right edge
            if (tool == Tool::Distort) qn[c] += delta;
            else if (tool == Tool::Skew) {
                const QPointF mv = horizontal ? du : dv;
                qn[c] += mv;
                qn[horizontal ? partnerH : partnerV] += mv;
            } else {   // Perspective: the corner and its partner move apart / together symmetrically
                const QPointF mv = horizontal ? du : dv;
                qn[c] += mv;
                qn[horizontal ? partnerH : partnerV] -= mv;
            }
        }
        // the centre keeps its place relative to the box
        bool ok = false;
        const QTransform rel = xf::quadMap(q0, qn, &ok);
        if (ok) m_tx.centre = rel.map(m_txCentre0);
    }
    bool ok = false;
    const QTransform t = xf::quadMap(q0, qn, &ok);
    if (!ok) return;
    m_tx.quad = qn;
    applyTransform(t);
    updateTransformBar();
}

// Right click (Fireworks MX 2004): an unselected object under the pointer becomes the selection,
// a click on empty canvas drops the object selection (a pixel selection is kept), then the window
// shows its context menu.
void Canvas::contextMenuEvent(QContextMenuEvent *e) {
    if (m_doc && pencilTwoColorActive() && e->reason() == QContextMenuEvent::Mouse) { e->accept(); return; }   // right drag paints
    if (!m_doc || m_drag != Drag::None || m_doc->s.layers.isEmpty()) { e->ignore(); return; }
    const QPointF d = toDoc(e->pos());
    const bool inPixelSel = !m_doc->pixelSelection.isEmpty() && m_doc->pixelSelection.contains(d);
    const quint64 hit = hitObject(d, true);
    if (hit) {
        if (!m_doc->selection.contains(hit)) m_doc->selectOnly(hit);
    } else if (!inPixelSel && m_doc->pixelSelection.isEmpty()) {
        m_doc->clearSelection();
    }
    update();
    e->accept();
    emit contextMenuRequested(e->globalPos());
}
