// 0.3.22: the bitmap Brush grows the target to the stroke engine's true painted bounds
// so scatter, jitter, spray, texture and soft edges are never clipped or lost.
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
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtTest>

namespace {
void sendMouse(QWidget *w, QEvent::Type t, QPointF pos, Qt::MouseButton b, Qt::MouseButtons bs)
{
    QMouseEvent ev(t, pos, w->mapToGlobal(pos), b, bs, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}
void drag(Canvas *c, QPointF a, QPointF b, int steps = 8)
{
    const QPointF va = c->toView(a), vb = c->toView(b);
    sendMouse(c, QEvent::MouseMove, va, Qt::NoButton, Qt::NoButton);
    sendMouse(c, QEvent::MouseButtonPress, va, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= steps; ++i)
        sendMouse(c, QEvent::MouseMove, va + (vb - va) * (double(i) / steps), Qt::NoButton, Qt::LeftButton);
    sendMouse(c, QEvent::MouseButtonRelease, vb, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
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
const VObject *onlyBitmap(Document *d)
{
    const VObject *bm = nullptr;
    int n = 0;
    for (int li = 0; li < d->s.layers.size(); ++li)
        for (const VObject &o : d->objects(li))
            if (o.type == ObjType::Bitmap) { bm = &o; ++n; }
    return n == 1 ? bm : nullptr;
}
QImage renderUnclipped(const QString &key, double width, const QPainterPath &path, QSize sz, quint32 seed)
{
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    stroke::PaintInput in;
    in.params = stroke::paramsFor(key);
    in.width = width;
    in.color = QColor(20, 80, 200);
    in.seed = seed;
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    stroke::paint(p, path, in);
    p.end();
    return img;
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
QImage replayUnclipped(const VObject &bm, const QString &key, int size, const QPointF &a, const QPointF &b, const QColor &c)
{
    const double scale = bm.rect.width() > 0 ? bm.image.width() / bm.rect.width() : 1;
    const QPointF ia = pxedit::docToImage(bm, a), ib = pxedit::docToImage(bm, b);
    QImage u(bm.image.size(), QImage::Format_ARGB32_Premultiplied);
    u.fill(Qt::transparent);
    brush::Stroke st;
    st.begin(u, brushParams(key, size * scale, c));
    QRect dirty = st.segment(ia, ia);
    dirty |= st.segment(ia, ib);
    dirty |= st.finish();
    if (!dirty.isEmpty()) st.composite(u, dirty);
    st.end();
    return u;
}
int strokePixelDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()) return a.width() * a.height() + 1;
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const auto *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (qAlpha(pa[x]) < 2 && qAlpha(pb[x]) < 2) continue;
            if (pa[x] != pb[x]) ++n;
        }
    }
    return n;
}
}

class V061Test : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        Prefs p;
        p.enableWebFeatures = false;
        appState().setPrefs(p);
        appState().setTool(Tool::Brush);
        appState().strokeCustom = false;
        appState().strokeTarget = AppState::ExpandSelected;
        appState().toolOpacity = 100;
    }

    void seventySixPresets()
    {
        QCOMPARE(stroke::presets().size(), 76);
    }

    void paintedRadiusCoversEngineOutput()
    {
        const double w = 24;
        int fail = 0;
        QString keys;
        for (const auto &pr : stroke::presets()) {
            QPainterPath path;
            path.moveTo(180, 160);
            path.lineTo(240, 160);
            const stroke::Params sp = stroke::paramsFor(pr.key);
            const QRectF allowed = stroke::paintedBounds(path, sp, w).adjusted(-3, -3, 3, 3);
            const QImage img = renderUnclipped(pr.key, w, path, QSize(420, 340), 0x9e3779b9u);
            const QRect ink = filters::opaqueBounds(img);
            if (ink.isEmpty()) continue;
            if (!allowed.contains(QRectF(ink))) {
                ++fail;
                keys += pr.key + QStringLiteral(" ink=%1,%2 %3x%4 allowed=%5,%6 %7x%8; ")
                            .arg(ink.x()).arg(ink.y()).arg(ink.width()).arg(ink.height())
                            .arg(allowed.x(), 0, 'f', 1).arg(allowed.y(), 0, 'f', 1)
                            .arg(allowed.width(), 0, 'f', 1).arg(allowed.height(), 0, 'f', 1);
            }
        }
        QVERIFY2(fail == 0, qPrintable(QStringLiteral("%1 presets exceeded paintedRadius: %2").arg(fail).arg(keys)));
    }

    void canvasGrowthKeepsEveryBrush()
    {
        MainWindow win;
        win.show();
        QCoreApplication::processEvents();
        appState().strokeTarget = AppState::ExpandSelected;
        const QColor col(20, 80, 200);
        int fail = 0;
        QString keys;
        const int size = 24;
        for (const auto &pr : stroke::presets()) {
            auto *doc = Document::createNew(360, 260, Qt::white);
            DocumentView *v = win.addDocument(doc);
            Canvas *cv = v->canvas();
            QCoreApplication::processEvents();

            VObject o;
            o.type = ObjType::Bitmap;
            o.rect = QRectF(80, 90, 48, 40);
            o.image = QImage(48, 40, QImage::Format_ARGB32_Premultiplied);
            o.image.fill(Qt::transparent);
            o.id = doc->newId();
            doc->addObject(o);
            doc->setSelection({o.id});
            doc->commit(QStringLiteral("bitmap"));

            applyBrush(pr.key, size);
            appState().setStroke(col);
            // A short 50 px run can stay inside the old size/2 pad when a
            // per-dab RNG roll is compact. A longer stroke still catches a
            // clipped growth without depending on one lucky splat.
            const QPointF a(100, 110), b(260, 112);
            drag(cv, a, b, 1);

            const VObject *bm = onlyBitmap(doc);
            if (!bm || filters::opaqueBounds(bm->image).isEmpty()) {
                ++fail; keys += pr.key + QStringLiteral("(no ink) "); continue;
            }
            // Same engine, same path, default seed and live speed samples.
            // A bare stroke::paint with a different path, seed, or constant
            // speed is not a fair reference: after per-dab RNG, a short
            // stroke can stay compact, and Spray Can spread follows speed.
            QImage engine(QSize(400, 320), QImage::Format_ARGB32_Premultiplied);
            engine.fill(Qt::transparent);
            brush::Stroke st;
            st.begin(engine, brushParams(pr.key, size, col));
            QRect dirty = st.segment(a, a);
            dirty |= st.segment(a, b);
            dirty |= st.finish();
            if (!dirty.isEmpty()) st.composite(engine, dirty);
            st.end();
            const QRect ui = filters::opaqueBounds(engine);
            const QRectF pathBox = QRectF(a, b).normalized();
            auto overflow = [](const QRectF &ink, const QRectF &pb) {
                return QVector<double>{
                    qMax(0.0, pb.left() - ink.left()),
                    qMax(0.0, pb.top() - ink.top()),
                    qMax(0.0, ink.right() - pb.right()),
                    qMax(0.0, ink.bottom() - pb.bottom())
                };
            };
            const auto want = overflow(QRectF(ui), pathBox);
            const auto got = overflow(bm->rect, pathBox);
            // Fair same-engine compare: any large overflow missing from the
            // grown bitmap is a clipped stroke, compact or wide.
            auto lost = [&](double w, double g) { return (w - g) >= 8; };
            if (ui.isEmpty() ? false : (lost(want[0], got[0]) || lost(want[1], got[1])
                                        || lost(want[2], got[2]) || lost(want[3], got[3]))) {
                ++fail;
                keys += pr.key + QStringLiteral("(unclipped LTRB %1,%2,%3,%4 grown %5,%6,%7,%8) ")
                            .arg(want[0], 0, 'f', 0).arg(want[1], 0, 'f', 0)
                            .arg(want[2], 0, 'f', 0).arg(want[3], 0, 'f', 0)
                            .arg(got[0], 0, 'f', 0).arg(got[1], 0, 'f', 0)
                            .arg(got[2], 0, 'f', 0).arg(got[3], 0, 'f', 0);
            }
            Q_UNUSED(v);
        }
        QVERIFY2(fail == 0, qPrintable(QStringLiteral("%1 brushes lost pixels vs unclipped render: %2").arg(fail).arg(keys)));
    }

    void pastEdgeAndOffsetBitmap()
    {
        MainWindow win;
        win.show();
        QCoreApplication::processEvents();
        appState().strokeTarget = AppState::ExpandSelected;
        const QColor col(20, 80, 200);

        auto run = [&](const QString &key, int size, const QRectF &bm, const QPointF &a, const QPointF &b, bool offset) {
            auto *doc = Document::createNew(400, 280, Qt::white);
            DocumentView *v = win.addDocument(doc);
            Canvas *cv = v->canvas();
            QCoreApplication::processEvents();
            VObject o;
            o.type = ObjType::Bitmap;
            o.rect = bm;
            o.image = QImage(int(bm.width()), int(bm.height()), QImage::Format_ARGB32_Premultiplied);
            o.image.fill(Qt::transparent);
            o.id = doc->newId();
            if (offset) {
                LiveEffect e = LiveEffect::make(EffectType::Offset);
                e.offsetX = 14;
                e.offsetY = -10;
                e.undefinedAreas = LiveEffect::Transparent;
                o.effects << e;
            }
            doc->addObject(o);
            doc->setSelection({o.id});
            doc->commit(QStringLiteral("bitmap"));
            applyBrush(key, size);
            appState().setStroke(col);
            drag(cv, a, b, 1);
            const VObject *got = onlyBitmap(doc);
            QVERIFY2(got, qPrintable(key));
            QVERIFY2(!filters::opaqueBounds(got->image).isEmpty(), qPrintable(key + QStringLiteral(" painted nothing")));
            const QRectF ink = got->rect; // trimmed to content
            const double oldPad = size / 2.0 + 2.0;
            const QRectF oldLimit = QRectF(a, b).normalized().adjusted(-oldPad, -oldPad, oldPad, oldPad).united(bm);
            // Cellular live strokes lock cell size (no mid-stroke MAXN coarsening), so their
            // ink may stay inside the old tight pad. Splatter / wide-scatter tips must still grow.
            if (key != QStringLiteral("lw.cellular")) {
                QVERIFY2(!oldLimit.contains(ink) || ink.width() > oldLimit.width() + 1 || ink.height() > oldLimit.height() + 1,
                         qPrintable(QStringLiteral("%1 offset=%2 bounds still inside the old tight pad (%3x%4 vs %5x%6)")
                                        .arg(key).arg(int(offset))
                                        .arg(ink.width()).arg(ink.height()).arg(oldLimit.width()).arg(oldLimit.height())));
            }
            if (!offset) {
                const int diff = strokePixelDiff(replayUnclipped(*got, key, size, a, b, col), got->image);
                QVERIFY2(diff == 0, qPrintable(QStringLiteral("%1 lost %2 px vs unclipped replay").arg(key).arg(diff)));
            }
            Q_UNUSED(v);
        };
        run(QStringLiteral("unnatural.fluidSplatter"), 32, QRectF(90, 90, 40, 36), QPointF(150, 108), QPointF(180, 110), false);
        run(QStringLiteral("unnatural.fluidSplatter"), 32, QRectF(90, 90, 40, 36), QPointF(150, 108), QPointF(180, 110), true);
        run(QStringLiteral("lw.cellular"), 28, QRectF(100, 100, 36, 32), QPointF(145, 116), QPointF(175, 116), false);
        run(QStringLiteral("oil.broadSplatter"), 36, QRectF(80, 90, 40, 36), QPointF(130, 108), QPointF(260, 108), false);
    }

    void trimToContentStillRuns()
    {
        MainWindow win;
        win.show();
        QCoreApplication::processEvents();
        auto *doc = Document::createNew(300, 200, Qt::white);
        DocumentView *v = win.addDocument(doc);
        Canvas *cv = v->canvas();
        appState().strokeTarget = AppState::NewBitmapPerStroke;
        applyBrush(QStringLiteral("basic.hard"), 6);
        appState().setStroke(Qt::red);
        drag(cv, QPointF(40, 50), QPointF(90, 50));
        const VObject *bm = onlyBitmap(doc);
        QVERIFY(bm);
        QVERIFY(bm->rect.contains(QPointF(40, 50)) && bm->rect.contains(QPointF(90, 50)));
        QVERIFY2(bm->rect.width() < 80 && bm->rect.height() < 20,
                 qPrintable(QStringLiteral("expected trim, got %1x%2").arg(bm->rect.width()).arg(bm->rect.height())));
        Q_UNUSED(v);
    }
};

QTEST_MAIN(V061Test)
#include "v061_test.moc"
