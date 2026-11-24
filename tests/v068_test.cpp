// 0.3.31: real Canvas + Brush, fast pointer jumps, growing / offset bitmaps,
// zoom, coalesced paints. Live frames must match the final redraw; nothing
// may be clipped to the bitmap's previous bounds.
#include "core/Document.h"
#include "core/Object.h"
#include "core/Stroke.h"
#include "core/BrushEngine.h"
#include "core/Filters.h"
#include "core/Effects.h"
#include "core/PixelEdit.h"
#include "ui/AppState.h"
#include "ui/MainWindow.h"
#include "ui/DocumentView.h"
#include "ui/Canvas.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QtTest>
#include <QSet>
#include <QFile>
#include <cmath>
#include <algorithm>

static const QString kShotDir = QStringLiteral("/workspace/review-0.3.31");

namespace {

void sendMouse(QWidget *w, QEvent::Type t, QPointF pos, Qt::MouseButton b, Qt::MouseButtons bs)
{
    QMouseEvent ev(t, pos, w->mapToGlobal(pos), b, bs, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

QPointingDevice *penDevice()
{
    static QPointingDevice *pen = nullptr;
    if (!pen)
        pen = new QPointingDevice(QStringLiteral("v068 pen"), 9068,
                                  QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                                  QInputDevice::Capability::Position | QInputDevice::Capability::Pressure
                                      | QInputDevice::Capability::XTilt | QInputDevice::Capability::YTilt,
                                  1, 2);
    return pen;
}

void sendPen(Canvas *cv, QEvent::Type t, QPointF doc, double pressure)
{
    const QPointF w = cv->toView(doc);
    const Qt::MouseButton b = t == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons bs = t == QEvent::TabletRelease ? Qt::NoButton
        : (t == QEvent::TabletPress ? Qt::LeftButton : Qt::MouseButtons(Qt::LeftButton));
    QTabletEvent e(t, penDevice(), w, cv->mapToGlobal(w), pressure, 0, 0, 0, 0, 0, Qt::NoModifier, b, bs);
    QApplication::sendEvent(cv, &e);
}

const VObject *onlyBitmap(Document *d)
{
    const VObject *bm = nullptr;
    int n = 0;
    for (int li = 0; li < d->s.layers.size(); ++li)
        for (const VObject &o : d->objects(li))
            if (o.type == ObjType::Bitmap) { bm = &o; ++n; }
    return n == 1 ? bm : nullptr;
}

void applyBrush(const QString &key, int size)
{
    appState().setTool(Tool::Brush);
    QCoreApplication::processEvents();
    const stroke::Params sp = stroke::paramsFor(key);
    const QStringList cats = brush::categoryNames();
    int ci = cats.indexOf(sp.category);
    if (ci < 0) ci = 0;
    appState().brushCategory = ci;
    appState().brushTip = brush::tipIndex(brush::Category(ci), sp.name);
    appState().brushSize = size;
    appState().strokeCustom = false;
    appState().strokeKind = key;
    appState().toolOpacity = 100;
    appState().brushTexture.clear();
    appState().brushTextureAmount = 0;
    appState().pressureSize = false;
    appState().pressureOpacity = false;
    appState().pressureFlow = false;
    appState().tiltAngle = false;
    appState().setStroke(QColor(20, 80, 200));
    QCoreApplication::processEvents();
    appState().brushCategory = ci;
    appState().brushTip = brush::tipIndex(brush::Category(ci), sp.name);
    appState().brushSize = size;
}

brush::Params brushParams(const QString &key, double sizePx, const QColor &c)
{
    brush::Params bp;
    const stroke::Params sp = stroke::paramsFor(key);
    const brush::Category cat = brush::categoryFromName(sp.category);
    brush::applyTip(bp, cat, brush::tipIndex(cat, sp.name));
    bp.size = qMax(1.0, sizePx);
    bp.opacity = 100;
    bp.color = c;
    bp.blend = BlendMode::Normal;
    return bp;
}

QImage blitBitmapToDoc(const VObject &bm, QSize doc)
{
    QImage out(doc, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.drawImage(bm.rect.topLeft(), bm.image.scaled(bm.rect.size().toSize(), Qt::IgnoreAspectRatio, Qt::FastTransformation));
    p.end();
    return out;
}

int missingInk(const QImage &ref, const QImage &got, int minAlpha = 8)
{
    if (ref.size() != got.size()) return ref.width() * ref.height() + 1;
    int n = 0;
    for (int y = 0; y < ref.height(); ++y) {
        const auto *pr = reinterpret_cast<const QRgb *>(ref.constScanLine(y));
        const auto *pg = reinterpret_cast<const QRgb *>(got.constScanLine(y));
        for (int x = 0; x < ref.width(); ++x)
            if (qAlpha(pr[x]) >= minAlpha && qAlpha(pg[x]) < minAlpha) ++n;
    }
    return n;
}

// Ink of `ref` that lies outside `oldDocRect` but is missing from `got`.
int clippedPastOldBounds(const QImage &ref, const QImage &got, const QRectF &oldDocRect, int minAlpha = 8)
{
    if (ref.size() != got.size()) return ref.width() * ref.height() + 1;
    const QRect old = oldDocRect.toAlignedRect();
    int n = 0;
    for (int y = 0; y < ref.height(); ++y) {
        const auto *pr = reinterpret_cast<const QRgb *>(ref.constScanLine(y));
        const auto *pg = reinterpret_cast<const QRgb *>(got.constScanLine(y));
        for (int x = 0; x < ref.width(); ++x) {
            if (old.contains(x, y)) continue;
            if (qAlpha(pr[x]) >= minAlpha && qAlpha(pg[x]) < minAlpha) ++n;
        }
    }
    return n;
}

// Ink lying strictly outside the bitmap's previous document rect
// (transparent document-space blit, alpha test).
int inkPastRect(const QImage &ink, const QRectF &oldDocRect, int minAlpha = 16)
{
    const QRect old = oldDocRect.toAlignedRect();
    int n = 0;
    for (int y = 0; y < ink.height(); ++y) {
        const auto *p = reinterpret_cast<const QRgb *>(ink.constScanLine(y));
        for (int x = 0; x < ink.width(); ++x)
            if (!old.contains(x, y) && qAlpha(p[x]) >= minAlpha) ++n;
    }
    return n;
}

static int rgbDist(QRgb a, QRgb b)
{
    return qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b)) + qAbs(qBlue(a) - qBlue(b));
}

// Stroke pixels of `ref` (not the canvas background) missing from `got`.
// Used on opaque document frames where alpha is always 255.
int missingPaint(const QImage &ref, const QImage &got, QRgb bg = qRgb(255, 255, 255), int minDist = 18)
{
    if (ref.size() != got.size()) return ref.width() * ref.height() + 1;
    int n = 0;
    for (int y = 0; y < ref.height(); ++y) {
        const auto *pr = reinterpret_cast<const QRgb *>(ref.constScanLine(y));
        const auto *pg = reinterpret_cast<const QRgb *>(got.constScanLine(y));
        for (int x = 0; x < ref.width(); ++x)
            if (rgbDist(pr[x], bg) >= minDist && rgbDist(pg[x], bg) < minDist / 2) ++n;
    }
    return n;
}

int paintPastRect(const QImage &frame, const QRectF &oldDocRect, QRgb bg = qRgb(255, 255, 255), int minDist = 18)
{
    const QRect old = oldDocRect.toAlignedRect();
    int n = 0;
    const int w = qMin(frame.width(), 1 << 20), h = qMin(frame.height(), 1 << 20);
    for (int y = 0; y < h; ++y) {
        const auto *p = reinterpret_cast<const QRgb *>(frame.constScanLine(y));
        for (int x = 0; x < w; ++x)
            if (!old.contains(x, y) && rgbDist(p[x], bg) >= minDist) ++n;
    }
    return n;
}

QImage referenceStroke(const QString &key, int size, const QColor &c, const QVector<QPointF> &docPts, QSize doc)
{
    QImage img(doc, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    brush::Stroke st;
    st.begin(img, brushParams(key, size, c));
    QRect dirty;
    if (!docPts.isEmpty()) {
        dirty |= st.segment(docPts.first(), docPts.first());
        for (int i = 1; i < docPts.size(); ++i)
            dirty |= st.segment(docPts[i - 1], docPts[i]);
    }
    dirty |= st.finish();
    if (!dirty.isEmpty()) st.composite(img, dirty);
    st.end();
    return img;
}

struct FastRun {
    QString key;
    QRectF startRect;
    QRectF endRect;
    QVector<qint64> frameNs;
    int clipped = 0;
    int missing = 0;
    int liveVsFinal = 0;
    int pastOld = 0;
    bool grew = false;
    QImage liveGrab;
    QImage finalGrab;
    QImage liveFrame;
    QImage finalFrame;
    QImage canvasInk;
    QImage refInk;
};

FastRun runFast(MainWindow &win, const QString &key, int size, double zoom,
                const QRectF &bmRect, const QVector<QPointF> &pts,
                bool coalesced, bool tablet, bool offsetEffect, bool grab)
{
    FastRun r;
    r.key = key;
    auto *doc = Document::createNew(900, 700, Qt::white);
    DocumentView *v = win.addDocument(doc);
    Canvas *cv = v->canvas();
    QCoreApplication::processEvents();
    cv->setZoom(zoom, QPointF(0, 0));
    QCoreApplication::processEvents();

    VObject o;
    o.type = ObjType::Bitmap;
    o.rect = bmRect;
    o.image = QImage(int(bmRect.width()), int(bmRect.height()), QImage::Format_ARGB32_Premultiplied);
    o.image.fill(Qt::transparent);
    if (offsetEffect) {
        LiveEffect e = LiveEffect::make(EffectType::Offset);
        e.offsetX = 18;
        e.offsetY = -12;
        e.undefinedAreas = LiveEffect::Transparent;
        o.effects << e;
    }
    o.id = doc->newId();
    doc->addObject(o);
    doc->setSelection({o.id});
    doc->commit(QStringLiteral("bitmap"));

    applyBrush(key, size);
    const QColor col(20, 80, 200);
    appState().setStroke(col);
    appState().strokeTarget = AppState::ExpandSelected;

    r.startRect = bmRect;
    const QSize docSz(doc->s.width, doc->s.height);

    auto flushUi = [&]() {
        // Drain the deferred flush and the dirty-region update() it posts.
        // Do not force a full-widget repaint(): that is not what the UI does
        // per frame, and it hid the cost of painting only the dirty view rect.
        QCoreApplication::processEvents();
    };

    QElapsedTimer t;
    if (tablet) {
        t.start();
        sendPen(cv, QEvent::TabletPress, pts.first(), 1.0);
        r.frameNs << t.nsecsElapsed();
        if (!coalesced) flushUi();
        for (int i = 1; i < pts.size(); ++i) {
            t.restart();
            sendPen(cv, QEvent::TabletMove, pts[i], 1.0);
            if (!coalesced) {
                flushUi();
                r.frameNs << t.nsecsElapsed();
            }
        }
        if (coalesced) {
            flushUi();
            r.frameNs << t.nsecsElapsed();
        }
        if (grab) {
            // Mid-stroke path-engine ink is finished by the worker; wait one
            // cycle so live-vs-final is the quality layer, not the cheap tail.
            cv->waitLiveStroke();
            r.liveGrab = cv->grab().toImage();
            r.liveFrame = cv->renderedFrameWithLive();
        }
        t.restart();
        sendPen(cv, QEvent::TabletRelease, pts.last(), 1.0);
        flushUi();
        r.frameNs << t.nsecsElapsed();
    } else {
        const QPointF v0 = cv->toView(pts.first());
        t.start();
        sendMouse(cv, QEvent::MouseMove, v0, Qt::NoButton, Qt::NoButton);
        sendMouse(cv, QEvent::MouseButtonPress, v0, Qt::LeftButton, Qt::LeftButton);
        r.frameNs << t.nsecsElapsed();
        if (!coalesced) flushUi();
        for (int i = 1; i < pts.size(); ++i) {
            t.restart();
            sendMouse(cv, QEvent::MouseMove, cv->toView(pts[i]), Qt::NoButton, Qt::LeftButton);
            if (!coalesced) {
                flushUi();
                r.frameNs << t.nsecsElapsed();
            }
        }
        if (coalesced) {
            flushUi();
            r.frameNs << t.nsecsElapsed();
        }
        if (grab) {
            // Mid-stroke path-engine ink is finished by the worker; wait one
            // cycle so live-vs-final is the quality layer, not the cheap tail.
            cv->waitLiveStroke();
            r.liveGrab = cv->grab().toImage();
            r.liveFrame = cv->renderedFrameWithLive();
        }
        t.restart();
        sendMouse(cv, QEvent::MouseButtonRelease, cv->toView(pts.last()), Qt::LeftButton, Qt::NoButton);
        flushUi();
        r.frameNs << t.nsecsElapsed();
    }

    const VObject *got = onlyBitmap(doc);
    if (got) {
        r.endRect = got->rect;
        r.grew = (got->rect != bmRect) || (got->image.size() != QSize(int(bmRect.width()), int(bmRect.height())));
        r.canvasInk = blitBitmapToDoc(*got, docSz);
        r.refInk = referenceStroke(key, size, col, pts, docSz);
        r.missing = missingInk(r.refInk, r.canvasInk);
        r.clipped = clippedPastOldBounds(r.refInk, r.canvasInk, bmRect);
        r.pastOld = inkPastRect(r.canvasInk, bmRect);
    }
    if (grab) {
        r.finalGrab = cv->grab().toImage();
        r.finalFrame = cv->renderedFrame().copy();
    }
    // Compare document frames, not the widget chrome: the selection outline
    // shrinks on trim and would otherwise look like a huge live-vs-final miss.
    if (!r.liveFrame.isNull() && !r.finalFrame.isNull())
        r.liveVsFinal = missingPaint(r.finalFrame, r.liveFrame);
    Q_UNUSED(v);
    return r;
}

double maxMs(const QVector<qint64> &ns)
{
    qint64 m = 0;
    for (qint64 t : ns) m = qMax(m, t);
    return m / 1e6;
}

double medianMs(QVector<qint64> ns)
{
    if (ns.isEmpty()) return 0;
    std::sort(ns.begin(), ns.end());
    const int n = ns.size();
    if (n % 2) return ns[n / 2] / 1e6;
    return 0.5 * (ns[n / 2 - 1] + ns[n / 2]) / 1e6;
}

void savePng(const QImage &img, const QString &name)
{
    if (img.isNull()) return;
    QDir().mkpath(kShotDir);
    img.save(kShotDir + QLatin1Char('/') + name);
}

} // namespace

class V068Test : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        Prefs p;
        p.enableWebFeatures = false;
        appState().setPrefs(p);
        appState().setTool(Tool::Brush);
        appState().strokeTarget = AppState::ExpandSelected;
        QCoreApplication::processEvents();
    }

    // Diagnostic + assertion: a fast stroke on a tiny bitmap must grow and
    // must not lose ink past the old bounds, for every factory tip.
    void fastStrokeNoClipAllTips()
    {
        MainWindow win;
        win.show();
        QCoreApplication::processEvents();
        const QRectF bm(120, 140, 40, 36);
        // 160–220 px jumps, well inside the 900×700 document.
        const QVector<QPointF> pts{QPointF(130, 150), QPointF(300, 155), QPointF(520, 230), QPointF(740, 400)};
        int fail = 0;
        QString keys;
        QString report;
        for (const auto &pr : stroke::presets()) {
            const FastRun r = runFast(win, pr.key, 24, 1.0, bm, pts, false, false, false, false);
            const double late = maxMs(r.frameNs);
            report += QStringLiteral("%1 grew=%2 past=%3 late=%4ms start=%5x%6 end=%7x%8\n")
                          .arg(pr.key).arg(int(r.grew)).arg(r.pastOld).arg(late, 0, 'f', 2)
                          .arg(r.startRect.width()).arg(r.startRect.height())
                          .arg(r.endRect.width()).arg(r.endRect.height());
            if (!r.grew || r.pastOld < 8) {
                ++fail;
                keys += pr.key + QStringLiteral("(past=%1 grew=%2) ").arg(r.pastOld).arg(int(r.grew));
                savePng(r.canvasInk, QStringLiteral("fail-%1-canvas.png").arg(pr.key));
            }
        }
        QDir().mkpath(kShotDir);
        QFile f(kShotDir + QStringLiteral("/v068-fast-all-tips.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(report.toUtf8());
        QVERIFY2(fail == 0, qPrintable(QStringLiteral("%1 tips clipped or did not grow: %2").arg(fail).arg(keys)));
    }

    void coalescedTabletOffsetZooms()
    {
        MainWindow win;
        win.show();
        QCoreApplication::processEvents();
        const QRectF bm(160, 180, 36, 32);
        const QVector<QPointF> pts{QPointF(170, 190), QPointF(360, 200), QPointF(560, 330), QPointF(750, 500)};
        const QStringList keys{QStringLiteral("basic.hard"), QStringLiteral("unnatural.alienPaint"),
                               QStringLiteral("lw.sprayCan"), QStringLiteral("oil.broadSplatter")};
        const QList<double> zooms{0.5, 1.0, 2.0, 4.0};
        int fail = 0;
        QString msg;
        for (const QString &key : keys) {
            for (double z : zooms) {
                for (int mode = 0; mode < 3; ++mode) {
                    const bool coalesced = mode != 0;
                    const bool tablet = mode == 2;
                    const bool offset = z == 1.0;
                    const FastRun r = runFast(win, key, 28, z, bm, pts, coalesced, tablet, offset, false);
                    if (!r.grew || r.pastOld < 8) {
                        ++fail;
                        msg += QStringLiteral("%1 z=%2 coal=%3 tab=%4 off=%5 past=%6 grew=%7; ")
                                   .arg(key).arg(z).arg(int(coalesced)).arg(int(tablet)).arg(int(offset))
                                   .arg(r.pastOld).arg(int(r.grew));
                    }
                }
            }
        }
        QVERIFY2(fail == 0, qPrintable(msg));
    }


    void midStrokeCrossesOldBounds()
    {
        MainWindow win;
        win.resize(1100, 900);
        win.show();
        QCoreApplication::processEvents();
        const QRectF bm(200, 220, 24, 24);
        const QPointF a(210, 230), b(480, 240); // 270 px jump
        const QStringList keys{QStringLiteral("basic.hard"), QStringLiteral("unnatural.alienPaint"),
                               QStringLiteral("lw.sprayCan"), QStringLiteral("oil.broadSplatter"),
                               QStringLiteral("unnatural.3dGlow"), QStringLiteral("watercolor.heavy")};
        int fail = 0;
        QString msg;
        for (const QString &key : keys) {
            auto *doc = Document::createNew(900, 700, Qt::white);
            DocumentView *v = win.addDocument(doc);
            Canvas *cv = v->canvas();
            QCoreApplication::processEvents();
            cv->setZoom(1.0, QPointF(0, 0));
            VObject o;
            o.type = ObjType::Bitmap;
            o.rect = bm;
            o.image = QImage(24, 24, QImage::Format_ARGB32_Premultiplied);
            o.image.fill(Qt::transparent);
            o.id = doc->newId();
            doc->addObject(o);
            doc->setSelection({o.id});
            doc->commit(QStringLiteral("bitmap"));
            applyBrush(key, 40);
            appState().setStroke(QColor(20, 80, 200));
            appState().strokeTarget = AppState::ExpandSelected;
            const QPointF va = cv->toView(a), vb = cv->toView(b);
            sendMouse(cv, QEvent::MouseButtonPress, va, Qt::LeftButton, Qt::LeftButton);
            sendMouse(cv, QEvent::MouseMove, vb, Qt::NoButton, Qt::LeftButton);
            // Grab without draining the 0-timer: flush-on-grow must already have
            // painted, or the widget would show a clip at the previous bounds.
            const QImage preGrab = cv->grab().toImage();
            // Mid-stroke ink lives on the live layer above the untouched base
            // bitmap; the document cache alone would still look clipped.
            const QImage preFrame = cv->renderedFrameWithLive();
            const VObject *got = onlyBitmap(doc);
            QVERIFY2(got, qPrintable(key));
            const QImage ink = blitBitmapToDoc(*got, QSize(900, 700));
            const int pastInk = inkPastRect(ink, bm);
            const int pastFrame = paintPastRect(preFrame, bm);
            savePng(ink, QStringLiteral("after-mid-%1-ink.png").arg(key));
            savePng(preGrab, QStringLiteral("after-mid-%1-grab.png").arg(key));
            qWarning("%s mid rect=%s img=%dx%d pastInk=%d pastFrame=%d grew=%d",
                     qPrintable(key),
                     qPrintable(QStringLiteral("%1,%2 %3x%4").arg(got->rect.x()).arg(got->rect.y()).arg(got->rect.width()).arg(got->rect.height())),
                     got->image.width(), got->image.height(), pastInk, pastFrame, int(got->rect != bm));
            // The base bitmap is intentionally untouched until release, so
            // pastInk may be 0 mid-stroke. The live-composited frame must
            // already show ink past the old bounds (no hidden clip).
            if (pastFrame < 50) {
                ++fail;
                msg += key + QStringLiteral(" pastInk=%1 pastFrame=%2 rect=%3x%4; ")
                           .arg(pastInk).arg(pastFrame).arg(got->rect.width()).arg(got->rect.height());
            }
            QCoreApplication::processEvents();
            sendMouse(cv, QEvent::MouseButtonRelease, vb, Qt::LeftButton, Qt::NoButton);
            QCoreApplication::processEvents();
            const VObject *got2 = onlyBitmap(doc);
            if (got2 && inkPastRect(blitBitmapToDoc(*got2, QSize(900, 700)), bm) < 50) {
                ++fail;
                msg += key + QStringLiteral(" lost ink after commit; ");
            }
            Q_UNUSED(v);
        }
        QVERIFY2(fail == 0, qPrintable(msg));
    }

    void liveFramesMatchFinalAndBudget()
    {
        MainWindow win;
        win.resize(1100, 900);
        win.show();
        QCoreApplication::processEvents();
        const QRectF bm(140, 160, 40, 36);
        const QVector<QPointF> pts{QPointF(150, 170), QPointF(340, 180), QPointF(450, 230),
                                   QPointF(560, 300), QPointF(650, 390), QPointF(760, 480)};
        const QStringList keys{
            QStringLiteral("basic.hard"),
            QStringLiteral("unnatural.alienPaint"),
            QStringLiteral("unnatural.3dGlow"),
            QStringLiteral("lw.inkWash"),
            QStringLiteral("watercolor.heavy"),
            QStringLiteral("oil.bristle"),
            QStringLiteral("charcoal.soft")
        };
        // Exact factory keys may differ; resolve by name below if needed.
        QStringList use = keys;
        QSet<QString> have;
        for (const auto &pr : stroke::presets()) have.insert(pr.key);
        // Fall back to a tip whose name matches if the guessed key is missing.
        auto resolve = [&](const QString &want) {
            if (have.contains(want)) return want;
            for (const auto &pr : stroke::presets())
                if (pr.key.contains(want.section('.', 1), Qt::CaseInsensitive)
                    || stroke::paramsFor(pr.key).name.contains(want.section('.', 1), Qt::CaseInsensitive))
                    return pr.key;
            return want;
        };

        int fail = 0;
        QString msg;
        QString report;
#ifdef LW_SANITIZE
        const double budgetMs = 80.0;
#else
        // Product target is under 8 ms standalone. The official cap is the
        // median frame time, 16 ms Release, so a loaded suite can still catch
        // real regressions without failing on a single noisy max.
        const double budgetMs = 16.0;
#endif
        for (const QString &raw : use) {
            const QString key = resolve(raw);
            // Per-event frames (not one coalesced burst) so the median is a
            // real per-frame number in the UI event loop.
            const FastRun r = runFast(win, key, 32, 1.0, bm, pts, false, false, false, true);
            const double late = medianMs(r.frameNs);
            const double peak = maxMs(r.frameNs);
            report += QStringLiteral("%1 median=%2ms max=%3ms past=%4 liveFinal=%5 grew=%6\n")
                          .arg(key).arg(late, 0, 'f', 2).arg(peak, 0, 'f', 2).arg(r.pastOld).arg(r.liveVsFinal).arg(int(r.grew));
            const QString tag = key;
            if (key.contains(QLatin1String("alien"), Qt::CaseInsensitive)) {
                savePng(r.liveGrab, QStringLiteral("fast-alien-live.png"));
                savePng(r.finalGrab, QStringLiteral("fast-alien-final.png"));
                savePng(r.canvasInk, QStringLiteral("fast-alien-canvas.png"));
                savePng(r.refInk, QStringLiteral("fast-alien-ref.png"));
            }
            if (key.contains(QLatin1String("hard"))) {
                savePng(r.liveGrab, QStringLiteral("fast-hard-live.png"));
                savePng(r.finalGrab, QStringLiteral("fast-hard-final.png"));
                savePng(r.canvasInk, QStringLiteral("fast-hard-canvas.png"));
                savePng(r.refInk, QStringLiteral("fast-hard-ref.png"));
            }
            if (!r.grew || r.pastOld < 8) {
                ++fail;
                msg += QStringLiteral("%1 no ink past old bounds (past=%2 grew=%3); ").arg(tag).arg(r.pastOld).arg(int(r.grew));
            }
            if (r.liveVsFinal > 400) {
                ++fail;
                msg += QStringLiteral("%1 live-vs-final %2 px; ").arg(tag).arg(r.liveVsFinal);
            }
            if (late > budgetMs) {
                ++fail;
                msg += QStringLiteral("%1 frame %2ms > %3ms; ").arg(tag).arg(late, 0, 'f', 1).arg(budgetMs, 0, 'f', 0);
            }
            Q_UNUSED(raw);
        }
        QFile f(kShotDir + QStringLiteral("/v068-frame-times.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(report.toUtf8());
        QVERIFY2(fail == 0, qPrintable(msg + QLatin1Char('\n') + report));
    }
};

QTEST_MAIN(V068Test)
#include "v068_test.moc"
