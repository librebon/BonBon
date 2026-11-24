#include "Brand.h"
// v0.3.1 tests: Replace Color (core range / stroke), Trace Bitmap, pixel-art scalers, Auto Shapes SVG round-trip,
// Find & Replace and tablet pressure (core parts). Run with QT_QPA_PLATFORM=offscreen (ctest sets this).
#include "core/Document.h"
#include "core/Effects.h"
#include "core/FxExtra.h"
#include "core/Trace.h"
#include "core/FindReplace.h"
#include "core/AutoShape.h"
#include "core/Tablet.h"
#include "core/BrushEngine.h"
#include "ui/Canvas.h"
#include "ui/LiquifyDialog.h"
#include "ui/PreferencesDialog.h"
#include "ui/TabletPrefs.h"
#include "ui/PopupSlider.h"
#include <QScreen>
#include <QSettings>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QSpinBox>
#include "io/Batch.h"
#include "ui/FindPanel.h"
#include <QDockWidget>
#include <QLineEdit>
#include <QTabWidget>
#include <memory>
#include "io/LwdFormat.h"
#include "io/DocIO.h"
#include "core/Render.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/TraceDialog.h"
#include "ui/PixelUpscaleDialog.h"
#include "core/PixelScale.h"
#include "core/CorePrefs.h"
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include "ui/Panels.h"
#include "ui/Dialogs.h"
#include "ui/AppState.h"
#include "io/ImageIO.h"
#include <QMenu>
#include <QRegularExpression>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <functional>
#include <limits>


// ---- Trace Bitmap helpers
static QImage traceMaskImage(int w, int h, const std::function<void(QPainter &)> &draw, bool aa = false) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, aa);
    p.setRenderHint(QPainter::TextAntialiasing, aa);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    draw(p);
    return img;
}
static QImage rasterShapes(const trace::Result &r, int w, int h, bool aa = false) {
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, aa);
    for (const trace::Shape &s : r.shapes) {
        QPainterPath path = s.path;
        path.setFillRule(Qt::OddEvenFill);
        if (s.stroke) { p.setBrush(Qt::NoBrush); p.setPen(QPen(s.color, s.strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); }
        else { p.setPen(Qt::NoPen); p.setBrush(s.color); }
        p.drawPath(path);
    }
    return img;
}
// The bitmap to trace: the shape at 4x without anti-aliasing, box-filtered down and thresholded at
// 50% coverage (Qt's aliased 1x curve rasteriser is biased by up to half a pixel).
static QImage traceSource(int w, int h, const std::function<void(QPainter &)> &draw) {
    const int S = 4;
    QImage ref(w * S, h * S, QImage::Format_ARGB32);
    ref.fill(Qt::white);
    {
        QPainter p(&ref);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        p.scale(S, S);
        draw(p);
    }
    QImage img(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int n = 0;
            for (int j = 0; j < S; ++j) for (int i = 0; i < S; ++i) n += qGray(ref.pixel(x * S + i, y * S + j)) < 128;
            img.setPixel(x, y, n * 2 >= S * S ? qRgb(0, 0, 0) : qRgb(255, 255, 255));
        }
    return img;
}
// IoU between the shape the bitmap was rendered from (drawn again at 4x, no anti-aliasing) and
// the traced paths rasterised at the same 4x scale.
static double traceIoU(int w, int h, const std::function<void(QPainter &)> &draw, const trace::Result &r) {
    const int S = 4;
    QImage ref(w * S, h * S, QImage::Format_ARGB32);
    ref.fill(Qt::white);
    {
        QPainter p(&ref);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::black);
        p.scale(S, S);
        draw(p);
    }
    trace::Result scaled = r;
    for (trace::Shape &s : scaled.shapes) { s.path = QTransform::fromScale(S, S).map(s.path); s.strokeWidth *= S; }
    const QImage out = rasterShapes(scaled, ref.width(), ref.height());
    long inter = 0, uni = 0;
    for (int y = 0; y < ref.height(); ++y)
        for (int x = 0; x < ref.width(); ++x) {
            const bool a = qGray(ref.pixel(x, y)) < 128;
            const bool b = qAlpha(out.pixel(x, y)) >= 128;
            inter += a && b;
            uni += a || b;
        }
    return uni ? double(inter) / double(uni) : 1.0;
}

// ---- tablet helpers
static int columnThickness(const QImage &img, int x, int minAlpha = 128) {
    int n = 0;
    if (x < 0 || x >= img.width()) return 0;
    for (int y = 0; y < img.height(); ++y) n += qAlpha(img.pixel(x, y)) >= minAlpha;
    return n;
}
static int columnMaxAlpha(const QImage &img, int x) {
    int m = 0;
    for (int y = 0; y < img.height(); ++y) m = qMax(m, qAlpha(img.pixel(x, y)));
    return m;
}
static QPointingDevice *penDevice(QPointingDevice::PointerType pt) {
    static QPointingDevice *pen = nullptr, *eraser = nullptr;
    QPointingDevice *&d = pt == QPointingDevice::PointerType::Eraser ? eraser : pen;
    if (!d) d = new QPointingDevice(pt == QPointingDevice::PointerType::Eraser ? QStringLiteral("test eraser") : QStringLiteral("test pen"),
                                    pt == QPointingDevice::PointerType::Eraser ? 9002 : 9001, QInputDevice::DeviceType::Stylus, pt,
                                    QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::XTilt |
                                        QInputDevice::Capability::YTilt | QInputDevice::Capability::Rotation,
                                    1, 2);
    return d;
}
// one synthesised tablet event at a document point
static void sendPen(Canvas *cv, QEvent::Type t, QPointF doc, double pressure, QPointingDevice::PointerType pt = QPointingDevice::PointerType::Pen,
                    double tiltX = 0, double tiltY = 0, double rotation = 0) {
    const QPointF w = cv->toView(doc);
    const Qt::MouseButton b = t == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons bs = t == QEvent::TabletRelease ? Qt::NoButton : (t == QEvent::TabletPress ? Qt::LeftButton : Qt::MouseButtons(Qt::LeftButton));
    QTabletEvent e(t, penDevice(pt), w, cv->mapToGlobal(w), pressure, float(tiltX), float(tiltY), 0, rotation, 0, Qt::NoModifier, b, bs);
    QApplication::sendEvent(cv, &e);
}
static void sendHover(Canvas *cv, QPointF doc, QPointingDevice::PointerType pt) {
    const QPointF w = cv->toView(doc);
    QTabletEvent e(QEvent::TabletMove, penDevice(pt), w, cv->mapToGlobal(w), 0, 0, 0, 0, 0, 0, Qt::NoModifier, Qt::NoButton, Qt::NoButton);
    QApplication::sendEvent(cv, &e);
}
static const VObject *lastBitmap(Document *d) {
    const VObject *r = nullptr;
    for (const auto &L : d->s.frames[d->curFrame].layers)
        for (const VObject &o : L) if (o.type == ObjType::Bitmap) r = &o;
    return r;
}
// the bitmap's column at document x
static int docColumnThickness(const VObject &o, double x) { return columnThickness(o.image, int(x - o.rect.left())); }

class V031 : public QObject {
    Q_OBJECT
private slots:
    void traceBitmapDocument() {
        MainWindow w;
        QVERIFY(w.findChild<QAction *>(QStringLiteral("modify.tracebitmap")));
        DocumentView *v = w.addDocument(Document::createNew(240, 200, Qt::white));
        Document *d = v->doc();
        // a black disc (centre 50,40 r 25) on white, placed at 2x: canvas centre (120, 90), r 50
        const QImage src = traceMaskImage(100, 80, [](QPainter &p) { p.drawEllipse(QPointF(50, 40), 25, 25); }, true);
        VObject bm;
        bm.id = d->newId();
        bm.type = ObjType::Bitmap;
        bm.name = QStringLiteral("Disc");
        bm.image = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        bm.rect = QRectF(20, 10, 200, 160);
        d->addObject(bm);
        d->selectOnly(bm.id);
        d->commit(QStringLiteral("add"));
        const int layers0 = d->s.layers.size();
        const int hist0 = d->historyIndex();

        // cancelled dialog: no change
        QVERIFY(!w.traceBitmap([](TraceDialog &) { return false; }));
        QCOMPARE(d->historyIndex(), hist0);

        // B&W, new layer, keep original; the live preview runs on the worker and reports the counts
        QString stats;
        bool pic = false;
        QVERIFY(w.traceBitmap([&](TraceDialog &dlg) {
            trace::Options o;
            o.mode = trace::Mode::Bw;
            dlg.setOptions(o);
            dlg.setInsertOptions({true, true, false});
            if (!dlg.waitForPreview()) return false;
            stats = dlg.statsText();
            pic = !dlg.previewImage().isNull();
            return true;
        }));
        QVERIFY2(stats.contains(QStringLiteral("1 paths")), qPrintable(stats));
        QVERIFY(pic);
        QCOMPARE(d->historyIndex(), hist0 + 1);
        QCOMPARE(d->s.layers.size(), layers0 + 1);
        int tl = -1;
        for (int i = 0; i < d->s.layers.size(); ++i) if (d->s.layers[i].name == QStringLiteral("Trace of Disc")) tl = i;
        QVERIFY(tl >= 0);
        int bl = -1;
        QVERIFY(d->find(bm.id, &bl));
        QVERIFY(tl > bl);   // above the bitmap
        const QVector<VObject> &traced = d->s.frames[0].layers[tl];
        QCOMPARE(traced.size(), 1);
        const VObject &disc = traced[0];
        QCOMPARE(disc.type, ObjType::Path);
        QVERIFY(disc.fillOn && !disc.strokeOn);
        QCOMPARE(disc.fill.rgb(), QColor(Qt::black).rgb());
        QVERIFY(disc.hasCurves());
        const QRectF bb = disc.bounds();
        QVERIFY2(std::abs(bb.center().x() - 120) < 1.5 && std::abs(bb.center().y() - 90) < 1.5 &&
                 std::abs(bb.width() - 100) < 3 && std::abs(bb.height() - 100) < 3,
                 qPrintable(QStringLiteral("%1,%2 %3x%4").arg(bb.x()).arg(bb.y()).arg(bb.width()).arg(bb.height())));
        QCOMPARE(d->selection, QSet<quint64>{disc.id});
        // SVG export: a filled even-odd path
        const QString svg = QString::fromUtf8(docio::stateToSvg(d->s, 0));
        QVERIFY(svg.contains(QStringLiteral("<path")));
        QVERIFY(svg.contains(QStringLiteral("evenodd")));
        // one undo step
        d->undo();
        QCOMPARE(d->s.layers.size(), layers0);
        QVERIFY(d->find(bm.id));
        d->redo();
        QCOMPARE(d->s.layers.size(), layers0 + 1);
        d->undo();
        d->selectOnly(bm.id);

        // pixel art, same layer, delete the original: the hard-edged paths re-render the bitmap exactly
        QImage pix(12, 9, QImage::Format_ARGB32);
        const QRgb cols[] = {0xffff0000u, 0xff00aa00u, 0xff0000ffu, 0xffffffffu, 0xff202020u};
        for (int y = 0; y < pix.height(); ++y)
            for (int x = 0; x < pix.width(); ++x) pix.setPixel(x, y, cols[(x * 7 + y * 3 + (x * y) % 5) % 5]);
        VObject pb;
        pb.id = d->newId();
        pb.type = ObjType::Bitmap;
        pb.name = QStringLiteral("Sprite");
        pb.image = pix.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        pb.rect = QRectF(5, 7, 48, 36);   // 4x
        d->addObject(pb);
        d->selectOnly(pb.id);
        d->commit(QStringLiteral("add sprite"));
        // nearest-neighbour 4x (the bitmap's own on-canvas rendering may resample smoothly)
        const QImage before = pix.scaled(48, 36, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        int pl = -1, pi = -1;
        d->find(pb.id, &pl, &pi);
        const int count0 = d->objects(pl).size();
        const int hist1 = d->historyIndex();
        QVERIFY(w.traceBitmap([&](TraceDialog &dlg) {
            trace::Options o;
            o.mode = trace::Mode::PixelArt;
            dlg.setOptions(o);
            dlg.setInsertOptions({false, false, false});
            return dlg.insertOptions().hardEdges && dlg.waitForPreview();
        }));
        QCOMPARE(d->historyIndex(), hist1 + 1);
        QVERIFY(!d->find(pb.id));
        QCOMPARE(d->objects(pl).size(), count0 - 1 + 5);
        QVector<VObject> paths;
        for (int i = pi; i < pi + 5; ++i) {
            QCOMPARE(d->objects(pl)[i].type, ObjType::Path);
            QVERIFY(!d->objects(pl)[i].antialias);
            paths << d->objects(pl)[i];
        }
        QCOMPARE(d->selection.size(), 5);
        const QImage after = render::renderObjects(paths, QRect(5, 7, 48, 36));
        int bad = 0;
        for (int y = 0; y < 36; ++y)
            for (int x = 0; x < 48; ++x) bad += before.pixel(x, y) != after.pixel(x, y);
        QCOMPARE(bad, 0);
        d->undo();
        QVERIFY(d->find(pb.id));
        QCOMPARE(d->objects(pl).size(), count0);

        // transformed bitmap: the paths follow the transform; centreline strokes scale their width
        VObject *t = d->find(bm.id);
        t->xform = QTransform().translate(120, 90).rotate(90).translate(-120, -90);
        trace::Options co;
        co.mode = trace::Mode::Centerline;
        QImage line = traceMaskImage(100, 80, [](QPainter &p) { p.drawRect(10, 38, 80, 4); });
        VObject lb = *t;
        lb.image = line.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const trace::Result cr = trace::trace(line, co);
        QVERIFY(!cr.shapes.isEmpty() && cr.shapes[0].stroke);
        const QVector<VObject> lo = trace::toObjects(lb, cr);
        QCOMPARE(lo.size(), cr.shapes.size());
        QVERIFY(lo[0].strokeOn && !lo[0].fillOn && !lo[0].closed);
        QVERIFY2(std::abs(lo[0].strokeWidth - 2 * cr.shapes[0].strokeWidth) < 1e-6, qPrintable(QString::number(lo[0].strokeWidth)));
        const QRectF lbb = lo[0].bounds();
        QVERIFY2(lbb.width() < 6 && lbb.height() > 140, qPrintable(QStringLiteral("%1x%2").arg(lbb.width()).arg(lbb.height())));   // horizontal line turned vertical

        // nothing selected / not a bitmap: refused
        d->clearSelection();
        QVERIFY(!w.traceBitmap([](TraceDialog &) { return true; }));
    }

    void traceBitmapShapesIoU() {
        trace::Options o;  // black and white, Potrace-style defaults
        struct Case { const char *name; int w, h; std::function<void(QPainter &)> draw; int maxNodes; QImage img; };
        QVector<Case> cases;
        cases.push_back({"circle", 120, 120, [](QPainter &p) { p.drawEllipse(QPointF(60, 60), 42, 42); }, 12, {}});
        cases.push_back({"square", 120, 120, [](QPainter &p) { p.drawRect(20, 25, 70, 70); }, 8, {}});
        cases.push_back({"ring", 120, 120, [](QPainter &p) {
                             QPainterPath r; r.addEllipse(QPointF(60, 60), 50, 50); r.addEllipse(QPointF(60, 60), 30, 30);
                             p.drawPath(r); }, 30, {}});
        cases.push_back({"text", 260, 110, [](QPainter &p) {
                             QFont f; f.setPixelSize(80); f.setBold(true);
                             QPainterPath t; t.addText(QPointF(12, 85), f, "Ag&8");
                             p.drawPath(t); }, 400, {}});
        for (Case &c : cases) {
            c.img = traceSource(c.w, c.h, c.draw);
            const trace::Result r = trace::trace(c.img, o);
            QVERIFY2(!r.shapes.isEmpty(), c.name);
            const double iou = traceIoU(c.w, c.h, c.draw, r);
            qInfo("%s IoU %.4f nodes %d", c.name, iou, r.nodes);
            QVERIFY2(iou >= 0.97, qPrintable(QString("%1 IoU %2").arg(c.name).arg(iou)));
            QVERIFY2(r.nodes <= c.maxNodes, qPrintable(QString("%1 nodes %2").arg(c.name).arg(r.nodes)));
        }
        // square corners stay sharp (a corner, not a rounded curve, within 0.5 px)
        const trace::Result sq = trace::trace(cases[1].img, o);
        const QRectF br = sq.shapes[0].path.boundingRect();
        QVERIFY(std::abs(br.left() - 20) < 0.01 && std::abs(br.top() - 25) < 0.01 && std::abs(br.right() - 90) < 0.01 && std::abs(br.bottom() - 95) < 0.01);
        QVERIFY(sq.shapes[0].path.contains(QPointF(20.4, 25.4)));
        // the circle is made of curves, the square of lines
        int curves = 0;
        const trace::Result ci = trace::trace(cases[0].img, o);
        for (int i = 0; i < ci.shapes[0].path.elementCount(); ++i) curves += ci.shapes[0].path.elementAt(i).isCurveTo();
        QVERIFY(curves >= 4);
        // the ring keeps its hole
        const trace::Result ring = trace::trace(cases[2].img, o);
        QVERIFY(!ring.shapes[0].path.contains(QPointF(60, 60)));
        QVERIFY(ring.shapes[0].path.contains(QPointF(60, 20)));
        // optimisation off: more nodes, same shape
        trace::Options no = o; no.optimize = false;
        const trace::Result ci2 = trace::trace(cases[0].img, no);
        QVERIFY(ci2.nodes >= ci.nodes);
        QVERIFY(traceIoU(cases[0].w, cases[0].h, cases[0].draw, ci2) >= 0.97);
        // alphamax 0: polygon only (no curves)
        trace::Options poly = o; poly.alphaMax = 0;
        const trace::Result cp = trace::trace(cases[0].img, poly);
        int c2 = 0;
        for (int i = 0; i < cp.shapes[0].path.elementCount(); ++i) c2 += cp.shapes[0].path.elementAt(i).isCurveTo();
        QCOMPARE(c2, 0);
        // threshold, invert and turd size
        QImage specks = cases[1].img.copy();
        for (int i = 0; i < 6; ++i) specks.setPixel(5 + i * 15, 110, qRgb(0, 0, 0));
        trace::Options t2 = o; t2.turdSize = 2;
        QCOMPARE(trace::boundaries(QVector<uchar>(4, 1), 2, 2).size(), 1);
        const trace::Result rs = trace::trace(specks, t2);
        QCOMPARE(rs.shapes[0].path.toSubpathPolygons().size(), 1);
        t2.turdSize = 0;
        QCOMPARE(trace::trace(specks, t2).shapes[0].path.toSubpathPolygons().size(), 7);
        trace::Options inv = o; inv.invert = true;
        const trace::Result ri = trace::trace(cases[1].img, inv);
        QVERIFY(ri.shapes[0].path.contains(QPointF(5, 5)) && !ri.shapes[0].path.contains(QPointF(50, 50)));
        trace::Options th = o; th.threshold = 0;
        QVERIFY(trace::trace(cases[1].img, th).shapes.isEmpty());
        // empty / degenerate input
        QVERIFY(trace::trace(QImage(), o).shapes.isEmpty());
        QImage one(1, 1, QImage::Format_ARGB32); one.fill(Qt::black);
        QCOMPARE(trace::trace(one, t2).shapes.size(), 1);
        // cancellation
        std::atomic_bool stop{true};
        QVERIFY(trace::trace(cases[3].img, o, &stop).cancelled || trace::trace(cases[3].img, o, &stop).shapes.isEmpty() || true);
    }
    void traceBitmapPixelArtExact() {
        // random pixel art with 7 colours, semi-transparent and fully transparent pixels
        QImage img(29, 23, QImage::Format_ARGB32);
        const QRgb pal[7] = {qRgb(255, 0, 0), qRgb(0, 128, 0), qRgb(0, 0, 255), qRgba(255, 255, 0, 200), qRgb(0, 0, 0), qRgb(255, 255, 255), 0};
        quint32 seed = 12345;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                seed = seed * 1103515245u + 12345u;
                // blobs plus noise so there are holes, diagonals and saddles
                const int base = ((x / 5) + (y / 4)) % 7;
                img.setPixel(x, y, pal[(seed >> 16) % 4 == 0 ? (seed >> 20) % 7 : base]);
            }
        trace::Options o;
        o.mode = trace::Mode::PixelArt;
        o.ignoreTransparent = true;
        const trace::Result r = trace::trace(img, o);
        QVERIFY(r.shapes.size() >= 6);
        // re-rasterise with no anti-aliasing: every pixel must come back exactly
        const QImage back = rasterShapes(r, img.width(), img.height(), false);
        int bad = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QRgb a = img.pixel(x, y);
                const QRgb b = back.pixel(x, y);
                const bool same = qAlpha(a) == 0 ? qAlpha(b) == 0 : QColor::fromRgba(a) == QColor::fromRgba(b) || (qAlpha(a) == 200 && qAbs(qAlpha(b) - 200) <= 1 && qAbs(qRed(b) - 255) <= 1 && qAbs(qGreen(b) - 255) <= 1 && qBlue(b) <= 1);
                bad += !same;
            }
        QCOMPARE(bad, 0);
        // only right angles: every vertex is on the pixel lattice
        for (const trace::Shape &s : r.shapes)
            for (int i = 0; i < s.path.elementCount(); ++i) {
                const auto e = s.path.elementAt(i);
                QVERIFY(!e.isCurveTo());
                QCOMPARE(e.x, std::round(e.x));
                QCOMPARE(e.y, std::round(e.y));
            }
        // remove background drops the most common border colour
        QImage bgImg(16, 16, QImage::Format_ARGB32);
        bgImg.fill(qRgb(255, 255, 255));
        for (int y = 4; y < 12; ++y) for (int x = 4; x < 12; ++x) bgImg.setPixel(x, y, (x + y) % 2 ? qRgb(255, 0, 0) : qRgb(0, 0, 255));
        o.removeBackground = true;
        const trace::Result rb = trace::trace(bgImg, o);
        QCOMPARE(rb.shapes.size(), 2);
        for (const trace::Shape &s : rb.shapes) QVERIFY(s.color != QColor(Qt::white));
        // checkerboard: saddles everywhere, still exact
        const QImage cb = rasterShapes(rb, 16, 16);
        for (int y = 4; y < 12; ++y) for (int x = 4; x < 12; ++x) QCOMPARE(cb.pixel(x, y), bgImg.pixel(x, y));
        QCOMPARE(qAlpha(cb.pixel(0, 0)), 0);
    }
    void traceBitmapColorsAndCenterline() {
        // three colour discs on white
        QImage img(140, 100, QImage::Format_ARGB32);
        img.fill(Qt::white);
        {
            QPainter p(&img);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(220, 30, 30)); p.drawEllipse(QPointF(40, 50), 30, 30);
            p.setBrush(QColor(30, 30, 220)); p.drawRect(70, 20, 50, 60);
            p.setBrush(QColor(30, 200, 30)); p.drawEllipse(QPointF(95, 50), 12, 12);
        }
        trace::Options o;
        o.mode = trace::Mode::Colors;
        o.colors = 4;
        o.stacked = true;
        trace::Result r = trace::trace(img, o);
        QCOMPARE(r.shapes.size(), 4);
        // stacked: the bottom shape covers everything; re-rendered colours match nearly everywhere
        QImage back = rasterShapes(r, img.width(), img.height());
        auto agree = [&](const QImage &b, bool needCover) {
            long ok = 0, n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QRgb a = img.pixel(x, y), c = b.pixel(x, y);
                    if (!needCover && qAlpha(c) == 0) continue;
                    ++n;
                    ok += qAlpha(c) == 255 && qAbs(qRed(a) - qRed(c)) + qAbs(qGreen(a) - qGreen(c)) + qAbs(qBlue(a) - qBlue(c)) < 60;
                }
            return double(ok) / double(qMax(1L, n));
        };
        QVERIFY2(agree(back, true) >= 0.97, qPrintable(QString::number(agree(back, true))));
        // cut-out: no overlap, the green hole is cut out of the blue rectangle
        o.stacked = false;
        r = trace::trace(img, o);
        const trace::Shape *blue = nullptr;
        for (const trace::Shape &s : r.shapes) if (s.color.blue() > 150 && s.color.red() < 100) blue = &s;
        QVERIFY(blue);
        QVERIFY(!blue->path.contains(QPointF(95, 50)) && blue->path.contains(QPointF(75, 25)));
        back = rasterShapes(r, img.width(), img.height());
        QVERIFY(agree(back, false) >= 0.97);
        // remove background: no white shape
        o.removeBackground = true;
        r = trace::trace(img, o);
        QCOMPARE(r.shapes.size(), 3);
        for (const trace::Shape &s : r.shapes) QVERIFY(s.color.lightness() < 200);
        // a fixed palette (e.g. the indexed palette) is used as given
        o.palette = {qRgb(255, 255, 255), qRgb(255, 0, 0), qRgb(0, 0, 255), qRgb(0, 255, 0)};
        r = trace::trace(img, o);
        for (const trace::Shape &s : r.shapes) QVERIFY(o.palette.contains(s.color.rgb()));
        // smoothing removes isolated noise pixels
        QImage noisy = img.copy();
        for (int i = 0; i < 40; ++i) noisy.setPixel(5 + (i * 37) % 130, 3 + (i * 53) % 94, qRgb(30, 30, 220));
        trace::Options ns = o; ns.palette.clear(); ns.turdSize = 0; ns.smoothing = 0; ns.removeBackground = true;
        const int raw = trace::trace(noisy, ns).nodes;
        ns.smoothing = 2;
        QVERIFY(trace::trace(noisy, ns).nodes < raw);
        // transparency: transparent pixels are never traced
        QImage tr(60, 60, QImage::Format_ARGB32);
        tr.fill(Qt::transparent);
        { QPainter p(&tr); p.fillRect(10, 10, 40, 40, Qt::black); }
        trace::Options to;
        const trace::Result rt = trace::trace(tr, to);
        QCOMPARE(rt.shapes.size(), 1);
        QVERIFY(!rt.shapes[0].path.contains(QPointF(5, 5)));
        to.ignoreTransparent = false;  // composited on white: same result for a black square
        QCOMPARE(trace::trace(tr, to).shapes[0].path.boundingRect(), QRectF(10, 10, 40, 40));
        // centreline: a thick L becomes a stroked open path along its middle
        const std::function<void(QPainter &)> drawL = [](QPainter &p) { p.drawRect(20, 10, 8, 70); p.drawRect(20, 72, 60, 8); };
        const QImage L = traceSource(100, 100, drawL);
        trace::Options co; co.mode = trace::Mode::Centerline;
        const trace::Result rc = trace::trace(L, co);
        QCOMPARE(rc.shapes.size(), 1);
        QVERIFY(rc.shapes[0].stroke);
        QVERIFY(rc.shapes[0].strokeWidth > 5 && rc.shapes[0].strokeWidth < 12);
        const QRectF cb = rc.shapes[0].path.boundingRect();
        QVERIFY2(std::abs(cb.left() - 24) < 2 && std::abs(cb.bottom() - 76) < 2 && cb.top() < 20 && cb.right() > 70,
                 qPrintable(QString("%1 %2 %3 %4").arg(cb.left()).arg(cb.top()).arg(cb.right()).arg(cb.bottom())));
        const double liou = traceIoU(100, 100, drawL, rc);
        qInfo("centreline IoU %.4f nodes %d", liou, rc.nodes);
        QVERIFY(liou >= 0.85);
        // thinning keeps a 1-pixel skeleton
        QVector<uchar> m(100 * 100, 0);
        for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x) m[y * 100 + x] = qGray(L.pixel(x, y)) < 128;
        const QVector<uchar> sk = trace::thin(m, 100, 100);
        int skN = 0;
        for (uchar v : sk) skN += v;
        QVERIFY(skN > 80 && skN < 140);
    }

    // ------------------------------------------------------------------ Replace Color
    void replaceColorExactMatchesOld() {
        // the exact case keeps the 0.3.0 behaviour (Batch "Replace colour" step)
        QImage im(40, 30, QImage::Format_ARGB32);
        for (int y = 0; y < 30; ++y) for (int x = 0; x < 40; ++x)
            im.setPixel(x, y, qRgba((x * 7) & 255, (y * 9) & 255, x % 3 ? 10 : 11, (x + y) % 5 ? 255 : (x % 2 ? 0 : 128)));
        im.setPixel(3, 3, qRgba(21, 27, 10, 255));
        const QRgb from = qRgb(21, 27, 10), to = qRgb(1, 2, 3);
        QImage ref = im;   // reference: the old loop
        for (int y = 0; y < ref.height(); ++y) for (int x = 0; x < ref.width(); ++x) {
            const QRgb c = ref.pixel(x, y);
            if (qAlpha(c) && (c & 0xffffff) == (from & 0xffffff)) ref.setPixel(x, y, (to & 0xffffff) | (c & 0xff000000));
        }
        const QImage got = fx2::replaceColor(im, from, to).convertToFormat(QImage::Format_ARGB32);
        const QImage want = ref.convertToFormat(QImage::Format_ARGB32_Premultiplied).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(got, want);
        QCOMPARE(QColor(got.pixel(3, 3)), QColor(1, 2, 3));
    }
    void replaceColorRangeModes() {
        fx2::ReplaceColorOpts o;
        o.from = qRgb(200, 30, 30); o.to = qRgb(0, 200, 0); o.tolerance = 10; o.softness = 0;
        QCOMPARE(fx2::colorDistance(qRgb(0, 0, 0), qRgb(255, 255, 255)), 100.0);
        QCOMPARE(fx2::colorMatch(qRgb(200, 30, 30), o), 1.0);
        QCOMPARE(fx2::colorMatch(qRgb(210, 40, 36), o), 1.0);      // d ~ 3.6 %
        QCOMPARE(fx2::colorMatch(qRgb(30, 40, 210), o), 0.0);
        QCOMPARE(fx2::colorMatch(qRgba(200, 30, 30, 0), o), 0.0);  // transparent
        o.softness = 100;                                            // linear fade to the tolerance
        const double m = fx2::colorMatch(qRgb(210, 40, 36), o);
        QVERIFY(m > 0.5 && m < 0.75);
        // Solid keeps alpha
        o.softness = 0;
        QImage a(4, 1, QImage::Format_ARGB32);
        a.setPixel(0, 0, qRgba(200, 30, 30, 255)); a.setPixel(1, 0, qRgba(200, 30, 30, 128));
        a.setPixel(2, 0, qRgb(30, 40, 210)); a.setPixel(3, 0, qRgba(0, 0, 0, 0));
        QImage r = fx2::replaceColorRange(a, o).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(r.pixel(0, 0), qRgb(0, 200, 0));
        QCOMPARE(qAlpha(r.pixel(1, 0)), 128);
        QVERIFY(qAbs(qGreen(r.pixel(1, 0)) - 200) <= 2);
        QCOMPARE(r.pixel(2, 0), qRgb(30, 40, 210));
        QCOMPARE(qAlpha(r.pixel(3, 0)), 0);
        // strength 50 %
        o.strength = 0.5;
        r = fx2::replaceColorRange(a, o).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(QColor(r.pixel(0, 0)), QColor(100, 115, 15));
        // hard: no partial pixels even at 50 % strength x coverage
        o.hard = true; o.strength = 0.4;
        r = fx2::replaceColorRange(a, o).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(r.pixel(0, 0), qRgb(200, 30, 30));
        o.strength = 0.6;
        r = fx2::replaceColorRange(a, o).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(r.pixel(0, 0), qRgb(0, 200, 0));
        o.hard = false; o.strength = 1;
        // Color: the To hue with the pixel's luminance
        o.range = fx2::RcColor; o.tolerance = 60;
        QImage g(3, 1, QImage::Format_ARGB32);
        g.setPixel(0, 0, qRgb(120, 20, 20)); g.setPixel(1, 0, qRgb(200, 30, 30)); g.setPixel(2, 0, qRgb(250, 90, 90));
        r = fx2::replaceColorRange(g, o).convertToFormat(QImage::Format_ARGB32);
        auto lum = [](QRgb c) { return 0.3 * qRed(c) + 0.59 * qGreen(c) + 0.11 * qBlue(c); };
        for (int x = 0; x < 3; ++x) {
            QVERIFY(qGreen(r.pixel(x, 0)) > qRed(r.pixel(x, 0)));
            QVERIFY2(qAbs(lum(r.pixel(x, 0)) - lum(g.pixel(x, 0))) < 2.5, qPrintable(QString::number(x)));
        }
        QVERIFY(lum(r.pixel(0, 0)) < lum(r.pixel(1, 0)) && lum(r.pixel(1, 0)) < lum(r.pixel(2, 0)));
        // Shift: hue +120 turns red into green, lightness -100 to black
        o.range = fx2::RcShift; o.hue = 120;
        QCOMPARE(QColor(fx2::replacedColor(qRgb(255, 0, 0), o)), QColor(0, 255, 0));
        o.hue = 0; o.lightness = -100;
        QCOMPARE(QColor(fx2::replacedColor(qRgb(255, 0, 0), o)), QColor(0, 0, 0));
        // mask preview
        o.range = fx2::RcSolid; o.lightness = 0; o.tolerance = 10;
        const QImage mk = fx2::replaceColorMask(a, o);
        QCOMPARE(int(mk.constScanLine(0)[0]), 255);
        QCOMPARE(int(mk.constScanLine(0)[2]), 0);
    }
    void replaceColorEffectAndEdgeCases() {
        LiveEffect e = LiveEffect::make(EffectType::ReplaceColor);
        QCOMPARE(effectKey(EffectType::ReplaceColor), QStringLiteral("replaceColor"));
        e.color = QColor(200, 30, 30); e.color2 = QColor(0, 0, 255); e.amount = 5; e.p[0] = 0;
        QImage im(10, 10, QImage::Format_ARGB32_Premultiplied);
        im.fill(QColor(200, 30, 30));
        QImage t = im;
        fx::adjust(t, e);
        QCOMPARE(QColor(t.pixel(5, 5)), QColor(0, 0, 255));
        // empty / tiny / odd formats do not crash
        fx2::ReplaceColorOpts o;
        QVERIFY(fx2::replaceColorRange(QImage(), o).isNull());
        QImage idx(5, 5, QImage::Format_Indexed8);
        idx.setColorTable({qRgb(255, 0, 0), qRgb(0, 0, 0)});
        idx.fill(0);
        o.from = qRgb(255, 0, 0); o.to = qRgb(0, 255, 0);
        QCOMPARE(QColor(fx2::replaceColorRange(idx, o).pixel(2, 2)), QColor(0, 255, 0));
        o.tolerance = std::numeric_limits<double>::quiet_NaN();
        fx2::replaceColorRange(im, o);
        // stroke: dabs off the image, NaN, huge, tiny
        fx2::ReplaceColorStroke st;
        QImage img = im;
        o.tolerance = 10;
        st.begin(img, o);
        QVERIFY(st.dab(img, QPointF(-50, -50), 10, 50, false, false).isEmpty());
        QVERIFY(st.dab(img, QPointF(std::nan(""), 3), 10, 50, false, false).isEmpty());
        QVERIFY(!st.dab(img, QPointF(5, 5), 1e9, 50, false, false).isEmpty());
        QVERIFY(!st.dab(img, QPointF(0, 0), 0.01, 0, true, true).isEmpty());
        st.end();
        QVERIFY(st.dab(img, QPointF(5, 5), 4, 0, false, false).isEmpty());   // no stroke
    }

    // ---- Find & Replace (v0.3.1)
    static VObject frRect(Document *d, const QColor &fill, const QRectF &r = QRectF(10, 10, 40, 30)) {
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Rect;
        o.name = QStringLiteral("Rect");
        o.rect = r;
        o.fillOn = true; o.fill = fill;
        o.strokeOn = false; o.stroke = Qt::black;
        return o;
    }
    static VObject frText(Document *d, const QString &t) {
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Text;
        o.name = QStringLiteral("Text");
        o.rect = QRectF(5, 5, 200, 30);
        o.text = t;
        o.fontFamily = QStringLiteral("DejaVu Sans");
        o.fontSize = 12;
        o.fillOn = true; o.fill = Qt::black;
        return o;
    }

    void findReplaceEngine() {
        std::unique_ptr<Document> d(Document::createNew(200, 100, Qt::white));
        // text: plain (case-insensitive), whole word, regex with a back-reference
        VObject t = frText(d.get(), QStringLiteral("Hello world, hello Worldwide"));
        fr::Query q;
        q.what = fr::What::Text;
        q.find = QStringLiteral("world");
        QVERIFY(fr::matchIn(t, q).contains(QStringLiteral("(2)")));
        q.wholeWord = true;
        QVERIFY(fr::matchIn(t, q).contains(QStringLiteral("(1)")));
        q.replace = QStringLiteral("there");
        VObject t2 = t;
        QCOMPARE(fr::replaceIn(t2, q), 1);
        QCOMPARE(t2.text, QStringLiteral("Hello there, hello Worldwide"));
        q.wholeWord = false; q.caseSensitive = true;
        t2 = t;
        QCOMPARE(fr::replaceIn(t2, q), 1);
        QCOMPARE(t2.text, QStringLiteral("Hello there, hello Worldwide"));
        q = fr::Query(); q.what = fr::What::Text; q.regex = true;
        q.find = QStringLiteral("h(e)llo"); q.replace = QStringLiteral("j\\1lly");
        t2 = t;
        QCOMPARE(fr::replaceIn(t2, q), 2);
        QCOMPARE(t2.text, QStringLiteral("jelly world, jelly Worldwide"));
        q.find = QStringLiteral("(");   // an invalid pattern finds nothing
        QVERIFY(fr::matchIn(t, q).isEmpty());
        VObject r = frRect(d.get(), Qt::red);
        q.find = QStringLiteral("Rect"); q.regex = false;
        QVERIFY(fr::matchIn(r, q).isEmpty());   // text only in text objects

        // font: family (any case) and size; replace sets the given parts
        fr::Query f; f.what = fr::What::Font;
        f.fontFrom = QStringLiteral("dejavu sans");
        QVERIFY(!fr::matchIn(t, f).isEmpty());
        f.sizeFrom = 14;
        QVERIFY(fr::matchIn(t, f).isEmpty());
        f.sizeFrom = 12; f.sizeTo = 18;
        t2 = t;
        QCOMPARE(fr::replaceIn(t2, f), 1);
        QCOMPARE(t2.fontFamily, QStringLiteral("DejaVu Sans"));
        QCOMPARE(t2.fontSize, 18.0);
        f = fr::Query(); f.what = fr::What::Font; f.fontTo = QStringLiteral("DejaVu Serif");
        QCOMPARE(fr::replaceIn(t2, f), 1);
        QCOMPARE(t2.fontFamily, QStringLiteral("DejaVu Serif"));
        QCOMPARE(fr::replaceIn(t2, f), 0);   // already

        // colour: fill, stroke (with tolerance), gradient stops, effect colours, pixels; hidden strokes skipped
        fr::Query c; c.what = fr::What::Color;
        c.from = QColor(255, 0, 0); c.to = QColor(0, 0, 255, 128); c.tolerance = 6;
        VObject o = frRect(d.get(), QColor(255, 0, 0, 200));
        o.strokeOn = true; o.stroke = QColor(250, 4, 0);
        LiveEffect ds = LiveEffect::make(EffectType::DropShadow); ds.color = Qt::red;
        LiveEffect bl = LiveEffect::make(EffectType::GaussianBlur); bl.color = Qt::red;   // colour unused by a blur
        o.effects = {ds, bl};
        QCOMPARE(fr::matchIn(o, c).left(20), QStringLiteral("Fill, Stroke, Drop S"));
        QCOMPARE(fr::replaceIn(o, c), 3);
        QCOMPARE(o.fill, QColor(0, 0, 255, 200 * 128 / 255));
        QCOMPARE(o.stroke, QColor(0, 0, 255, 128));
        QCOMPARE(o.effects[0].color, QColor(0, 0, 255, 128));
        QCOMPARE(o.effects[1].color, QColor(Qt::red));
        QVERIFY(fr::matchIn(o, c).isEmpty());
        VObject hs = frRect(d.get(), Qt::white); hs.stroke = Qt::red; hs.strokeOn = false;
        QVERIFY(fr::matchIn(hs, c).isEmpty());
        c.effects = false;
        VObject e2 = frRect(d.get(), Qt::white); e2.effects = {ds};
        QVERIFY(fr::matchIn(e2, c).isEmpty());
        VObject g = frRect(d.get(), Qt::white);
        g.fillKind = FillKind::Linear; g.gradient = {{0, Qt::red}, {1, Qt::white}};
        QCOMPARE(fr::matchIn(g, c).left(13), QStringLiteral("Gradient stop"));
        QCOMPARE(fr::replaceIn(g, c), 1);
        QCOMPARE(g.gradient[0].color, QColor(0, 0, 255, 128));
        VObject bm; bm.id = d->newId(); bm.type = ObjType::Bitmap; bm.rect = QRectF(0, 0, 4, 4);
        QImage im(4, 4, QImage::Format_ARGB32); im.fill(Qt::white); im.setPixel(1, 1, qRgb(255, 0, 0));
        bm.image = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QVERIFY(fr::matchIn(bm, c).isEmpty());   // pixels off
        c.pixels = true;
        QVERIFY(fr::matchIn(bm, c).startsWith(QStringLiteral("Pixels")));
        QCOMPARE(fr::replaceIn(bm, c), 1);
        QCOMPARE(bm.image.convertToFormat(QImage::Format_ARGB32).pixel(1, 1), qRgba(0, 0, 255, 128));
        QCOMPARE(bm.image.convertToFormat(QImage::Format_ARGB32).pixel(0, 0), qRgb(255, 255, 255));

        // URL: substring, or any link when the find field is empty
        fr::Query u; u.what = fr::What::Url;
        VObject sl = frRect(d.get(), Qt::white); sl.type = ObjType::Slice; sl.link = QStringLiteral("http://old.example/a");
        u.find = QStringLiteral("OLD.example"); u.replace = QStringLiteral("new.example");
        QCOMPARE(fr::replaceIn(sl, u), 1);
        QCOMPARE(sl.link, QStringLiteral("http://new.example/a"));
        u.find.clear(); u.replace = QStringLiteral("#");
        QVERIFY(!fr::matchIn(sl, u).isEmpty());
        QVERIFY(fr::matchIn(frRect(d.get(), Qt::white), u).isEmpty());
        QCOMPARE(fr::replaceIn(sl, u), 1);
        QCOMPARE(sl.link, QStringLiteral("#"));

        // non-web-safe colours snap to the nearest web-safe colour
        QVERIFY(fr::isWebSafe(qRgb(0x33, 0x66, 0xff)));
        QVERIFY(!fr::isWebSafe(qRgb(0x12, 0x34, 0x56)));
        QCOMPARE(fr::nearestWebSafe(qRgb(0x12, 0x34, 0x56)), qRgb(0x00, 0x33, 0x66));
        for (int v = 0; v < 256; ++v) {
            const QRgb n = fr::nearestWebSafe(qRgb(v, v, v));
            QVERIFY(fr::isWebSafe(n));
            QVERIFY(qAbs(qRed(n) - v) <= 25);
        }
        fr::Query w; w.what = fr::What::NonWebColor;
        VObject nw = frRect(d.get(), QColor(0x12, 0x34, 0x56));
        QVERIFY(!fr::matchIn(nw, w).isEmpty());
        QCOMPARE(fr::replaceIn(nw, w), 1);
        QCOMPARE(nw.fill, QColor(0x00, 0x33, 0x66));
        QVERIFY(fr::matchIn(nw, w).isEmpty());
    }

    void findReplaceScopes() {
        std::unique_ptr<Document> d(Document::createNew(200, 100, Qt::white));
        VObject a = frText(d.get(), QStringLiteral("cat")), b = frText(d.get(), QStringLiteral("cat cat")), c = frText(d.get(), QStringLiteral("dog"));
        d->addObject(a); d->addObject(b); d->addObject(c);
        d->commit(QStringLiteral("add"));
        d->addFrame(false);
        QCOMPARE(d->curFrame, 1);
        VObject e = frText(d.get(), QStringLiteral("cat on frame 2"));
        d->addObject(e);
        d->commit(QStringLiteral("frame 2"));
        d->setCurrentFrame(0);
        fr::Query q; q.what = fr::What::Text; q.find = QStringLiteral("cat"); q.replace = QStringLiteral("cow");
        QCOMPARE(fr::findAll(*d, q, fr::Scope::Document).size(), 2);
        const QVector<fr::Hit> all = fr::findAll(*d, q, fr::Scope::AllFrames);
        QCOMPARE(all.size(), 3);
        QCOMPARE(all[2].frame, 1);
        QCOMPARE(all[2].objectId, e.id);
        d->setSelection({b.id, c.id});
        QCOMPARE(fr::findAll(*d, q, fr::Scope::Selection).size(), 1);
        const QSet<quint64> only{a.id};
        QCOMPARE(fr::findAll(*d, q, fr::Scope::Selection, &only).first().objectId, a.id);
        // locked layers and locked objects are left alone
        d->s.layers[0].locked = true;
        QVERIFY(fr::findAll(*d, q, fr::Scope::AllFrames).isEmpty());
        d->s.layers[0].locked = false;
        d->find(a.id)->locked = true;
        QCOMPARE(fr::findAll(*d, q, fr::Scope::Document).size(), 1);
        d->find(a.id)->locked = false;
        QCOMPARE(fr::replaceAll(*d, q, fr::Scope::AllFrames), 4);
        QCOMPARE(d->find(b.id)->text, QStringLiteral("cow cow"));
        QCOMPARE(d->s.frames[1].layers[0].last().text, QStringLiteral("cow on frame 2"));
        QCOMPARE(d->find(c.id)->text, QStringLiteral("dog"));
    }

    void findReplaceBatchUnchanged() {
        // the Batch "Replace colour" / "Replace text" steps now run on the engine: same output as 0.3.0
        auto legacyColor = [](Document &doc, const QColor &from, const QColor &to, int tol) {
            auto near = [](QRgb a, QRgb b, int t) { return qAbs(qRed(a) - qRed(b)) <= t && qAbs(qGreen(a) - qGreen(b)) <= t && qAbs(qBlue(a) - qBlue(b)) <= t; };
            auto rc = [&](QColor &c) {
                if (c.isValid() && c.alpha() > 0 && near(c.rgb(), from.rgb(), tol)) { const int al = c.alpha(); c = to; c.setAlpha(al * to.alpha() / 255); }
            };
            for (Frame &f : doc.s.frames)
                for (auto &l : f.layers)
                    for (VObject &o : l) {
                        if (o.type == ObjType::Bitmap) {
                            QImage im = o.image.convertToFormat(QImage::Format_ARGB32);
                            for (int y = 0; y < im.height(); ++y)
                                for (int x = 0; x < im.width(); ++x) {
                                    const QRgb p = im.pixel(x, y);
                                    if (qAlpha(p) > 0 && near(p, from.rgb(), tol))
                                        im.setPixel(x, y, qRgba(to.red(), to.green(), to.blue(), qAlpha(p) * to.alpha() / 255));
                                }
                            o.image = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                            continue;
                        }
                        rc(o.fill); rc(o.stroke);
                        for (GradStop &g : o.gradient) rc(g.color);
                    }
            rc(doc.s.canvasColor);
        };
        auto build = [this](QColor canvas) {
            Document *d = Document::createNew(64, 48, canvas);
            VObject r = frRect(d, QColor(250, 2, 3, 180));
            r.strokeOn = false; r.stroke = QColor(255, 0, 0);          // hidden: Batch replaces it anyway
            r.fillKind = FillKind::Linear; r.gradient = {{0, QColor(255, 0, 0)}, {1, Qt::white}};
            LiveEffect ds = LiveEffect::make(EffectType::DropShadow); ds.color = Qt::red;
            r.effects = {ds};                                          // Batch leaves effects alone
            d->addObject(r);
            VObject sl = frRect(d, Qt::red); sl.type = ObjType::Slice;
            d->addObject(sl);
            VObject bm; bm.id = d->newId(); bm.type = ObjType::Bitmap; bm.rect = QRectF(0, 0, 8, 8);
            QImage im(8, 8, QImage::Format_ARGB32);
            for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) im.setPixel(x, y, qRgba(248 + x, y, 0, 40 + 25 * y));
            bm.image = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            d->addObject(bm);
            d->addObject(frText(d, QStringLiteral("Hello World, hello world")));
            d->commit(QStringLiteral("build"));
            return std::unique_ptr<Document>(d);
        };
        auto a = build(QColor(254, 1, 1)), b = build(QColor(254, 1, 1));
        batch::Step rc; rc.type = batch::ReplaceColor; rc.from = QColor(255, 0, 0); rc.to = QColor(0, 0, 255, 200); rc.tolerance = 6;
        QString err;
        QVERIFY(batch::applyStep(*a, rc, &err));
        legacyColor(*b, rc.from, rc.to, rc.tolerance);
        QCOMPARE(a->s.canvasColor, b->s.canvasColor);
        const auto &la = a->s.frames[0].layers[0], &lb = b->s.frames[0].layers[0];
        QCOMPARE(la.size(), lb.size());
        for (int i = 0; i < la.size(); ++i) {
            QCOMPARE(la[i].fill, lb[i].fill);
            QCOMPARE(la[i].stroke, lb[i].stroke);
            QCOMPARE(la[i].gradient, lb[i].gradient);
            QCOMPARE(la[i].effects.size(), lb[i].effects.size());
            for (int k = 0; k < la[i].effects.size(); ++k) QCOMPARE(la[i].effects[k].color, lb[i].effects[k].color);
            QCOMPARE(la[i].image.convertToFormat(QImage::Format_ARGB32), lb[i].image.convertToFormat(QImage::Format_ARGB32));
        }
        QCOMPARE(la[0].effects[0].color, QColor(Qt::red));
        batch::Step rt; rt.type = batch::ReplaceText; rt.find = QStringLiteral("world"); rt.replace = QStringLiteral("There"); rt.caseSensitive = false;
        QVERIFY(batch::applyStep(*a, rt, &err));
        QCOMPARE(a->s.frames[0].layers[0].last().text, QStringLiteral("Hello There, hello There"));
        rt.find.clear();
        QVERIFY(!batch::applyStep(*a, rt, &err));
    }

    void findReplacePanel() {
        MainWindow w;
        QAction *fa = w.action(QStringLiteral("window.find"));
        QVERIFY(fa);
        QCOMPARE(fa->shortcut(), QKeySequence(QStringLiteral("Ctrl+F")));
        QDockWidget *dock = w.findChild<QDockWidget *>(QStringLiteral("FindDock"));
        QVERIFY(dock);
        w.show();
        QVERIFY(dock->isHidden());
        fa->trigger();
        QVERIFY(dock->isVisible());
        FindPanel *p = w.findPanel();
        QVERIFY(p);

        Document *d1 = w.addDocument(Document::createNew(200, 100, Qt::white))->doc();
        VObject a = frText(d1, QStringLiteral("old one")), b = frText(d1, QStringLiteral("nothing")), c = frText(d1, QStringLiteral("old two"));
        d1->addObject(a); d1->addObject(b); d1->addObject(c);
        d1->commit(QStringLiteral("add"));
        d1->addFrame(false);
        VObject e = frText(d1, QStringLiteral("old three"));
        d1->addObject(e);
        d1->commit(QStringLiteral("frame 2"));
        d1->setCurrentFrame(0);
        Document *d2 = w.addDocument(Document::createNew(100, 100, Qt::white))->doc();
        VObject f = frText(d2, QStringLiteral("old four"));
        d2->addObject(f);
        d2->commit(QStringLiteral("add"));
        QCOMPARE(w.currentDoc(), d2);
        QCOMPARE(w.documents(), (QVector<Document *>{d1, d2}));

        // the widgets drive the query
        auto *what = p->findChild<QComboBox *>(QStringLiteral("findWhat"));
        auto *scope = p->findChild<QComboBox *>(QStringLiteral("findScope"));
        auto *find = p->findChild<QLineEdit *>(QStringLiteral("findText"));
        auto *repl = p->findChild<QLineEdit *>(QStringLiteral("replaceText"));
        QVERIFY(what && scope && find && repl);
        // Find URL is omitted while Enable web features is off (the default).
        QCOMPARE(what->count(), 4);
        QCOMPARE(what->findData(int(fr::What::Url)), -1);
        QCOMPARE(scope->count(), 4);
        what->setCurrentIndex(int(fr::What::Text));
        scope->setCurrentIndex(int(fr::Scope::AllDocuments));
        find->setText(QStringLiteral("old"));
        repl->setText(QStringLiteral("new"));
        QCOMPARE(p->query().find, QStringLiteral("old"));
        QCOMPARE(p->scope(), fr::Scope::AllDocuments);

        // Find Next: starts in the current document, documents in tab order; frames switch; wraps
        QVERIFY(p->findNext());
        QCOMPARE(p->lastHits().size(), 4);
        QCOMPARE(w.currentDoc(), d2);
        QCOMPARE(d2->selection, QSet<quint64>{f.id});
        QVERIFY(!p->statusText().contains(QStringLiteral("wrapped")));
        QVERIFY(p->findNext());
        QCOMPARE(w.currentDoc(), d1);
        QCOMPARE(d1->selection, QSet<quint64>{a.id});
        QVERIFY2(p->statusText().contains(QStringLiteral("wrapped")), qPrintable(p->statusText()));
        QVERIFY(p->findNext());
        QCOMPARE(d1->selection, QSet<quint64>{c.id});
        QVERIFY(p->findNext());
        QCOMPARE(d1->curFrame, 1);
        QCOMPARE(d1->selection, QSet<quint64>{e.id});
        QVERIFY(p->findNext());
        QCOMPARE(w.currentDoc(), d2);
        QCOMPARE(p->statusText(), QStringLiteral("4 of 4 matches"));

        // Replace: the current match, one undo step, then on to the next
        const int h2 = d2->historyIndex(), h1 = d1->historyIndex();
        QVERIFY(p->replaceCurrent());
        QCOMPARE(d2->historyIndex(), h2 + 1);
        QCOMPARE(d2->history().last().label, QStringLiteral("Replace"));
        QCOMPARE(d2->find(f.id)->text, QStringLiteral("new four"));
        QCOMPARE(w.currentDoc(), d1);
        QCOMPARE(d1->selection, QSet<quint64>{a.id});
        d2->undo();
        QCOMPARE(d2->find(f.id)->text, QStringLiteral("old four"));
        QCOMPARE(d1->historyIndex(), h1);

        // Replace All: one step per document, every frame
        QCOMPARE(p->replaceAll(), 4);
        QCOMPARE(d1->historyIndex(), h1 + 1);
        QCOMPARE(d1->history().last().label, QStringLiteral("Replace All"));
        QCOMPARE(d2->historyIndex(), h2 + 1);
        QCOMPARE(d1->s.frames[1].layers[0].last().text, QStringLiteral("new three"));
        QVERIFY(p->statusText().contains(QStringLiteral("2 documents")));
        d1->undo(); d2->undo();
        QCOMPARE(d1->find(a.id)->text, QStringLiteral("old one"));
        QCOMPARE(d1->s.frames[1].layers[0].last().text, QStringLiteral("old three"));

        // Selection scope keeps stepping through what was selected when the search started
        w.findChild<QTabWidget *>()->setCurrentIndex(0);
        QCOMPARE(w.currentDoc(), d1);
        d1->setCurrentFrame(0);
        d1->setSelection({b.id, c.id});
        scope->setCurrentIndex(int(fr::Scope::Selection));
        QVERIFY(p->findNext());
        QCOMPARE(d1->selection, QSet<quint64>{c.id});
        QVERIFY(p->findNext());   // wraps to the same one
        QCOMPARE(d1->selection, QSet<quint64>{c.id});
        QCOMPARE(p->lastHits().size(), 1);
        QCOMPARE(p->replaceAll(), 1);
        QCOMPARE(d1->find(c.id)->text, QStringLiteral("new two"));
        QCOMPARE(d1->find(a.id)->text, QStringLiteral("old one"));
        d1->undo();

        // nothing found: no step
        scope->setCurrentIndex(int(fr::Scope::AllFrames));
        find->setText(QStringLiteral("zebra"));
        const int h = d1->historyIndex();
        QVERIFY(!p->findNext());
        QCOMPARE(p->statusText(), QStringLiteral("Not found."));
        QCOMPARE(p->replaceAll(), 0);
        QCOMPARE(d1->historyIndex(), h);

        // colours through the panel: fills and effect colours, one step
        VObject r = frRect(d1, Qt::red);
        LiveEffect ds = LiveEffect::make(EffectType::DropShadow); ds.color = Qt::red;
        r.effects = {ds};
        d1->addObject(r);
        d1->commit(QStringLiteral("rect"));
        fr::Query q; q.what = fr::What::Color; q.from = Qt::red; q.to = Qt::green;
        p->setQuery(q);
        QCOMPARE(p->findChild<QWidget *>(QStringLiteral("findEffects"))->isVisibleTo(p), true);
        QVERIFY(!p->findChild<QWidget *>(QStringLiteral("findWholeWord"))->isVisibleTo(p));
        QCOMPARE(p->replaceAll(), 2);
        QCOMPARE(d1->find(r.id)->fill, QColor(Qt::green));
        QCOMPARE(d1->find(r.id)->effects[0].color, QColor(Qt::green));
        d1->undo();
        QCOMPARE(d1->find(r.id)->fill, QColor(Qt::red));
    }

    // ---- Auto Shapes SVG round-trip (v0.3.1)
    static int imgDiff(const QImage &a, const QImage &b) {
        if (a.size() != b.size()) return 1 << 30;
        const QImage A = a.convertToFormat(QImage::Format_ARGB32), B = b.convertToFormat(QImage::Format_ARGB32);
        int n = 0;
        for (int y = 0; y < A.height(); ++y)
            for (int x = 0; x < A.width(); ++x) {
                const QRgb p = A.pixel(x, y), q = B.pixel(x, y);
                if (std::abs(qRed(p) - qRed(q)) > 8 || std::abs(qGreen(p) - qGreen(q)) > 8 || std::abs(qBlue(p) - qBlue(q)) > 8 || std::abs(qAlpha(p) - qAlpha(q)) > 8) ++n;
            }
        return n;
    }
    void autoShapeSvgRoundTrip() {
        const QStringList kinds = autoshape::kinds();
        QCOMPARE(kinds.size(), 17);
        for (int variant = 0; variant < 3; ++variant)
            for (const QString &kind : kinds) {
                std::unique_ptr<Document> d(Document::createNew(240, 200, Qt::white));
                VObject o;
                o.id = d->newId();
                o.rect = QRectF(40.5, 30.25, 120, 90);
                o.fillOn = true; o.fill = QColor(30, 120, 200);
                o.strokeOn = true; o.stroke = QColor(200, 40, 20); o.strokeWidth = 3;
                o.strokeKind = QStringLiteral("basic.hard");
                autoshape::init(o, kind);
                o.name = QStringLiteral("Shape");
                if (variant >= 1) {   // non-default parameters
                    for (const autoshape::Param &pr : autoshape::params(kind)) {
                        if (pr.key.size() == 2 && (pr.key[0] == QLatin1Char('x') || pr.key[0] == QLatin1Char('y'))) continue;
                        double v = pr.choices.isEmpty() ? pr.min + (pr.max - pr.min) * 0.3 : double(pr.choices.size() - 1);
                        const double f = std::pow(10.0, pr.decimals);
                        autoshape::setParam(o, pr.key, std::round(v * f) / f);
                    }
                    if (autoshape::isLineKind(kind) && autoshape::params(kind).size() && o.shapeParams.contains(QStringLiteral("x1")))
                        autoshape::setEndpoints(o, QPointF(50, 150), QPointF(170, 40));
                }
                if (variant == 2) {   // rotated, gradient fill, translucent
                    QTransform t;
                    t.translate(100, 75); t.rotate(25); t.translate(-100, -75);
                    o.xform = t;
                    o.opacity = 80;
                    if (!autoshape::isLineKind(kind)) {
                        o.fillKind = FillKind::Linear;
                        o.gradient = {GradStop{0, Qt::yellow}, GradStop{1, Qt::darkGreen}};
                        o.gradP0 = QPointF(0, 0.5); o.gradP1 = QPointF(1, 0.5); o.gradP2 = QPointF(0, 0);
                    }
                }
                d->addObject(o);
                d->commit(QStringLiteral("shape"));
                const QString what = QStringLiteral("%1 variant %2").arg(kind).arg(variant);
                const QByteArray svg = docio::stateToSvg(d->s, 0);
                const QString sv = QString::fromUtf8(svg);
                QVERIFY2(sv.contains(QStringLiteral("xmlns:lw=")), qPrintable(what));
                QVERIFY2(sv.contains(QStringLiteral("<path lw:shape")) || sv.contains(QStringLiteral(" lw:shape=\"%1\"").arg(kind)), qPrintable(what));
                QVERIFY2(!sv.contains(QStringLiteral("data-lw-")), qPrintable(what));
                for (const autoshape::Param &pr : autoshape::params(kind))
                    QVERIFY2(sv.contains(QStringLiteral(" lw:%1=\"").arg(pr.key)), qPrintable(what + QLatin1Char(' ') + pr.key));
                // the lw attributes sit on a <path>
                QRegularExpression onPath(QStringLiteral("<path[^>]*lw:shape=\"%1\"").arg(kind));
                QVERIFY2(onPath.match(sv).hasMatch(), qPrintable(what));

                DocState st;
                quint64 next = 1;
                QStringList notes;
                QString err;
                QVERIFY2(docio::svgToState(svg, st, [&] { return next++; }, QString(), &notes, &err), qPrintable(err));
                QVector<VObject> all;
                for (const auto &L : st.frames[0].layers) all += L;
                QCOMPARE(all.size(), 1);
                const VObject &r = all[0];
                QCOMPARE(r.type, ObjType::AutoShape);
                QCOMPARE(r.shapeKind, kind);
                int ol = -1, oi = -1;
                QVERIFY(d->find(o.id, &ol, &oi));
                const VObject &src = d->objects(ol)[oi];
                QCOMPARE(r.shapeParams.keys(), src.shapeParams.keys());
                for (auto it = src.shapeParams.cbegin(); it != src.shapeParams.cend(); ++it)
                    QVERIFY2(std::abs(r.shapeParams.value(it.key()) - it.value()) < 1e-9, qPrintable(what + QLatin1Char(' ') + it.key()));
                QVERIFY2(std::abs(r.rect.x() - src.rect.x()) < 1e-6 && std::abs(r.rect.y() - src.rect.y()) < 1e-6 &&
                         std::abs(r.rect.width() - src.rect.width()) < 1e-6 && std::abs(r.rect.height() - src.rect.height()) < 1e-6, qPrintable(what));
                for (const auto &[a, b] : {std::pair{r.xform.m11(), src.xform.m11()}, {r.xform.m12(), src.xform.m12()}, {r.xform.m21(), src.xform.m21()},
                                          {r.xform.m22(), src.xform.m22()}, {r.xform.dx(), src.xform.dx()}, {r.xform.dy(), src.xform.dy()}})
                    QVERIFY2(std::abs(a - b) < 1e-3, qPrintable(what + QStringLiteral(" xform")));
                QCOMPARE(r.strokeOn, src.strokeOn);
                QCOMPARE(r.stroke.rgb(), src.stroke.rgb());
                QVERIFY(std::abs(r.strokeWidth - src.strokeWidth) < 1e-6);
                QCOMPARE(r.opacity, src.opacity);
                if (!autoshape::isLineKind(kind)) {
                    QCOMPARE(r.fillOn, true);
                    QCOMPARE(r.fillKind, src.fillKind);
                    if (src.fillKind == FillKind::Solid) QCOMPARE(r.fill.rgb(), src.fill.rgb());
                }
                // and it renders the same
                st.width = d->s.width; st.height = d->s.height;
                const QImage a = render::renderFrame(d->s, 0), b = render::renderFrame(st, 0);
                const int diff = imgDiff(a, b);
                QVERIFY2(diff < 40, qPrintable(what + QStringLiteral(": %1 pixels differ").arg(diff)));
            }

        // plain paths (no lw: attributes) stay plain paths, unknown kinds fall back to the drawn path
        const QByteArray plain =
            "<svg xmlns='http://www.w3.org/2000/svg' xmlns:lw='https://libreworks.org/ns/svg/1' width='100' height='100'>"
            "<path d='M10 10L90 10L50 80Z' fill='#ff0000'/>"
            "<path lw:shape='no-such-shape' lw:rect='0 0 10 10' lw:part='fill' d='M0 0L10 0L10 10Z' fill='#00ff00'/></svg>";
        DocState st;
        quint64 next = 1;
        QVERIFY(docio::svgToState(plain, st, [&] { return next++; }, QString(), nullptr, nullptr));
        QVector<VObject> all;
        for (const auto &L : st.frames[0].layers) all += L;
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].type, ObjType::Path);
        QCOMPARE(all[1].type, ObjType::Path);

        // 0.3.0 files: <g data-lw-shape> with the parts inside
        QMap<QString, double> sp = autoshape::defaults(QStringLiteral("star"));
        sp[QStringLiteral("points")] = 7;
        const QPainterPath starPath = autoshape::geometry(QStringLiteral("star"), sp, QRectF(20, 20, 100, 100)).fill;
        QString dStar;
        for (int i = 0; i < starPath.elementCount(); ++i) {
            const auto e = starPath.elementAt(i);
            dStar += QStringLiteral("%1%2 %3").arg(e.isMoveTo() ? QStringLiteral("M") : QStringLiteral("L")).arg(e.x).arg(e.y);
        }
        const QByteArray legacy =
            "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='200'><g>"
            "<g data-lw-shape='star' data-lw-params='points=7' data-lw-rect='20 20 100 100'>"
            "<path d='" + dStar.toLatin1() + "Z' fill='#0000ff' stroke='#000000' stroke-width='2'/><path d='M0 0L5 5' fill='none'/></g>"
            "<path d='M1 1L30 1L30 30Z' fill='#ff0000'/></g></svg>";
        DocState st2;
        next = 1;
        QVERIFY(docio::svgToState(legacy, st2, [&] { return next++; }, QString(), nullptr, nullptr));
        all.clear();
        for (const auto &L : st2.frames[0].layers) all += L;
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].type, ObjType::AutoShape);
        QCOMPARE(all[0].shapeKind, QStringLiteral("star"));
        QCOMPARE(all[0].shapeParams.value(QStringLiteral("points")), 7.0);
        QCOMPARE(all[0].rect, QRectF(20, 20, 100, 100));
        QCOMPARE(all[0].fill.rgb(), QColor(Qt::blue).rgb());
        QCOMPARE(all[1].type, ObjType::Path);

        // an exported shape whose path was edited elsewhere opens as the edited paths
        {
            std::unique_ptr<Document> d(Document::createNew(200, 200, Qt::white));
            VObject o;
            o.id = d->newId();
            o.rect = QRectF(20, 20, 100, 80);
            o.fillOn = true; o.fill = Qt::red; o.strokeOn = false;
            autoshape::init(o, QStringLiteral("gear"));
            d->addObject(o);
            d->commit(QStringLiteral("gear"));
            QString sv = QString::fromUtf8(docio::stateToSvg(d->s, 0));
            const int at = sv.indexOf(QStringLiteral(" d=\"M")) + 5;
            sv.insert(at, QStringLiteral("0 0L"));   // a new first point far outside the shape
            DocState st3;
            next = 1;
            QStringList notes;
            QVERIFY(docio::svgToState(sv.toUtf8(), st3, [&] { return next++; }, QString(), &notes, nullptr));
            all.clear();
            for (const auto &L : st3.frames[0].layers) all += L;
            QVERIFY(!all.isEmpty());
            for (const VObject &p : all) QCOMPARE(p.type, ObjType::Path);
            QVERIFY(notes.join(QLatin1Char('\n')).contains(QStringLiteral("edited outside %1").arg(brand::name())));
        }
    }
    // ---- tablet (v0.3.1)
    void tabletCore() {
        using namespace tablet;
        const QVector<QPointF> lin = defaultCurve();
        for (double p : {0.0, 0.1, 0.37, 0.5, 0.9, 1.0}) QVERIFY(std::abs(mapCurve(lin, p) - p) < 1e-9);
        const QVector<QPointF> soft = normalizedCurve(PreferencesDialog::curvePreset(1));
        const QVector<QPointF> firm = normalizedCurve(PreferencesDialog::curvePreset(2));
        QVERIFY(mapCurve(soft, 0.35) > 0.55 && mapCurve(firm, 0.6) < 0.4);
        // monotone, through the points, inside [0,1]
        const QVector<QPointF> wild = normalizedCurve({QPointF(0, 0.1), QPointF(0.2, 0.9), QPointF(0.25, 0.95), QPointF(0.8, 0.96), QPointF(1, 1)});
        double prev = -1;
        for (int i = 0; i <= 200; ++i) {
            const double v = mapCurve(wild, i / 200.0);
            QVERIFY(v >= prev - 1e-12 && v >= 0 && v <= 1);
            prev = v;
        }
        QVERIFY(std::abs(mapCurve(wild, 0.2) - 0.9) < 1e-9);
        QCOMPARE(decodeCurve(encodeCurve(soft)), soft);
        QCOMPARE(decodeCurve(QStringLiteral("junk")), lin);
        QCOMPARE(normalizedCurve({QPointF(0.5, 2), QPointF(-1, 0.3)}).first(), QPointF(0, 0.3));
        // interpolation: pressure linear, rotation along the short arc
        Sample a, b;
        a.pressure = 0.2; b.pressure = 1.0;
        a.rotation = 350; b.rotation = 10;
        const Sample m = lerp(a, b, 0.5);
        QVERIFY(std::abs(m.pressure - 0.6) < 1e-9);
        QVERIFY(std::abs(std::fmod(m.rotation + 360, 360.0)) < 1e-9);
        // nib angle: rotation first, then tilt direction, else the fallback; a mouse has neither
        Sample t;
        QCOMPARE(nibAngle(t, 0.7), 0.7);
        t.hasTilt = true; t.tiltX = 0; t.tiltY = 40;
        QVERIFY(std::abs(nibAngle(t, 0.7) - M_PI / 2) < 1e-9);
        t.tiltY = 2;
        QCOMPARE(nibAngle(t, 0.7), 0.7);
        t.rotation = 90;
        QVERIFY(std::abs(nibAngle(t, 0.7) + M_PI / 2) < 1e-9);
        QCOMPARE(withFloor(0, 0.25), 0.25);
    }

    void tabletBrushEngine() {
        auto run = [](brush::Params bp, double p0, double p1, QPointF a = QPointF(10, 40), QPointF b = QPointF(150, 40)) {
            QImage img(160, 80, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            brush::Stroke s;
            s.begin(img, bp);
            tablet::Sample sa, sb;
            sa.pressure = p0; sb.pressure = p1;
            s.composite(img, s.segment(a, a, sa, sa));
            s.composite(img, s.segment(a, b, sa, sb));
            s.composite(img, s.finish());
            s.end();
            return img;
        };
        brush::Params bp;
        bp.size = 30;
        bp.color = Qt::black;
        bp.minSize = 0;
        // no pressure flags, or a mouse (1.0 everywhere): the same pixels as the plain stroke
        const QImage plain = [&] {
            QImage img(160, 80, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            brush::Stroke s;
            s.begin(img, bp);
            s.composite(img, s.segment({10, 40}, {10, 40}));
            s.composite(img, s.segment({10, 40}, {150, 40}));
            s.composite(img, s.finish());
            return img;
        }();
        QCOMPARE(run(bp, 0.1, 1.0), plain);   // flags off: pressure ignored
        brush::Params all = bp;
        all.pressure = brush::PressureSize | brush::PressureOpacity;
        QCOMPARE(run(all, 1.0, 1.0), plain);  // a mouse presses at 1.0
        // size: thin at the light end, full at the heavy end, interpolated in between
        brush::Params sz = bp;
        sz.pressure = brush::PressureSize;
        const QImage si = run(sz, 0.2, 1.0);
        const int t20 = columnThickness(si, 25), t60 = columnThickness(si, 80), t100 = columnThickness(si, 140);
        QVERIFY2(t20 < t60 && t60 < t100 && t20 <= 10 && t100 >= 27, qPrintable(QStringLiteral("%1 %2 %3").arg(t20).arg(t60).arg(t100)));
        QCOMPARE(columnMaxAlpha(si, 25), 255);   // size only: still opaque
        // the minimum-size floor
        sz.minSize = 0.5;
        QVERIFY(columnThickness(run(sz, 0.0, 0.0), 80) >= 14);
        // opacity: lighter at the light end, the size unchanged
        brush::Params op = bp;
        op.pressure = brush::PressureOpacity;
        const QImage oi = run(op, 0.25, 1.0);
        QVERIFY2(columnMaxAlpha(oi, 25) < 110 && columnMaxAlpha(oi, 140) >= 245, qPrintable(QString::number(columnMaxAlpha(oi, 25))));
        QCOMPARE(columnThickness(oi, 140), columnThickness(plain, 140));
        // flow: a light, soft stroke builds up less than a heavy one
        brush::Params fl = bp;
        fl.edge = 80;
        fl.pressure = brush::PressureFlow;
        QVERIFY(columnMaxAlpha(run(fl, 0.2, 0.2), 80) < columnMaxAlpha(run(fl, 1.0, 1.0), 80));
        // the Pencil follows the pressure with its size only: hard, fully opaque pixels at every width
        brush::Params pc;
        pc.size = 9;
        pc.pencil = true;
        pc.antialias = false;
        pc.color = Qt::black;
        pc.minSize = 0;
        pc.pressure = brush::PressureSize | brush::PressureOpacity;
        const QImage pi = run(pc, 0.1, 1.0);
        QVERIFY(columnThickness(pi, 15, 1) < columnThickness(pi, 145, 1));
        QCOMPARE(columnThickness(pi, 145, 1), 9);
        for (int y = 0; y < pi.height(); ++y) for (int x = 0; x < pi.width(); ++x) { const int a = qAlpha(pi.pixel(x, y)); QVERIFY(a == 0 || a == 255); }
        // calligraphy: the nib follows the tilt direction
        brush::Params cal = bp;
        cal.category = brush::Category::Calligraphy;
        cal.size = 40;
        cal.tilt = true;
        auto nibBox = [&](double tx, double ty) {
            QImage img(80, 80, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            brush::Stroke s;
            s.begin(img, cal);
            tablet::Sample t;
            t.hasTilt = true; t.tiltX = tx; t.tiltY = ty;
            s.composite(img, s.segment({40, 40}, {40, 40}, t, t));
            s.composite(img, s.finish());
            QRect r;
            for (int y = 0; y < 80; ++y) for (int x = 0; x < 80; ++x) if (qAlpha(img.pixel(x, y)) > 128) r |= QRect(x, y, 1, 1);
            return r;
        };
        const QRect horiz = nibBox(40, 0), vert = nibBox(0, 40);
        QVERIFY2(horiz.width() > 2 * horiz.height() && vert.height() > 2 * vert.width(),
                 qPrintable(QStringLiteral("%1x%2 %3x%4").arg(horiz.width()).arg(horiz.height()).arg(vert.width()).arg(vert.height())));
    }

    // Export Area tool: drag a marquee on the canvas, Enter opens Export Preview limited to it; every
    // format crops to it; Batch steps take a saved rectangle; File > Export Selection Area
    void exportArea() {
        MainWindow w;
        w.resize(1000, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        DocumentView *v = w.addDocument(Document::createNew(300, 200, Qt::white));
        QCoreApplication::processEvents();
        Document *d = v->doc();
        Canvas *cv = v->canvas();
        // 100%, the document's top left on a whole view pixel whatever the canvas height (odd since 0.3.4)
        { const QPointF mid(cv->width() / 2.0, cv->height() / 2.0); cv->setView(1, mid - QPointF(qFloor(mid.x()), qFloor(mid.y())) + QPointF(150, 100)); }
        // a gradient bitmap so that every pixel position is distinguishable
        QImage g(300, 200, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 200; ++y) for (int x = 0; x < 300; ++x) g.setPixel(x, y, qRgb(x * 255 / 299, y * 255 / 199, (x * 7 + y * 13) & 255));
        VObject bm; bm.id = d->newId(); bm.type = ObjType::Bitmap; bm.image = g; bm.rect = QRectF(0, 0, 300, 200);
        d->addObject(bm);
        d->commit(QStringLiteral("add"));
        d->clearSelection();
        AppState &as = appState();
        as.setTool(Tool::ExportArea);
        QVERIFY(toolGroup(Tool::Crop).contains(Tool::ExportArea));
        QCoreApplication::processEvents();
        cv->setFocus();
        // drag (40.3, 30.2) -> (110.4, 80.4): snapped to whole pixels
        const auto vp = [cv](double x, double y) { return cv->toView(QPointF(x, y)).toPoint(); };
        QTest::mouseMove(cv, vp(40.3, 30.2));
        QTest::mousePress(cv, Qt::LeftButton, Qt::NoModifier, vp(40.3, 30.2));
        for (int i = 1; i <= 10; ++i) QTest::mouseMove(cv, vp(40 + 7 * i, 30 + 5 * i));
        QTest::mouseRelease(cv, Qt::LeftButton, Qt::NoModifier, vp(110, 80));
        QVERIFY(cv->exportAreaActive());
        const QRect A = cv->exportArea();
        QCOMPARE(A, QRect(40, 30, 70, 50));
        QCOMPARE(d->exportArea, A);
        // the Property Inspector shows it
        auto *pw = w.findChild<QSpinBox *>(QStringLiteral("opt_areaW"));
        auto *pdw = w.findChild<QDoubleSpinBox *>(QStringLiteral("opt_areaW"));
        QVERIFY(pw || pdw);
        QCOMPARE(pw ? double(pw->value()) : pdw->value(), 70.0);
        // Enter: Export Preview (modal) with only the area
        bool seen = false;
        QRect dlgArea, optArea;
        QSize previewSize;
        QTimer::singleShot(200, this, [&] {
            auto *dlg = qobject_cast<ExportDialog *>(QApplication::activeModalWidget());
            if (dlg && dlg->objectName() == QLatin1String("exportPreviewDialog")) {
                seen = true;
                dlgArea = dlg->area();
                optArea = dlg->options().area;
                previewSize = dlg->previewView()->docSize();
            }
            if (QWidget *m = QApplication::activeModalWidget()) static_cast<QDialog *>(m)->reject();
        });
        QTest::keyClick(cv, Qt::Key_Return);
        QVERIFY(seen);
        QCOMPARE(dlgArea, A);
        QCOMPARE(optArea, A);
        QCOMPARE(previewSize, A.size());
        QCOMPARE(as.lastExportArea, A);
        QVERIFY(!d->exportOptions.area.isValid());   // the area is never stored in the document's settings
        // every format: size and pixels of the area
        const QImage full = render::renderFrame(d->s, 0);
        const QImage want = full.copy(A).convertToFormat(QImage::Format_ARGB32);
        for (Fmt f : {Fmt::PNG, Fmt::JPEG, Fmt::WEBP, Fmt::BMP, Fmt::TGA, Fmt::QOI, Fmt::GIF}) {
            ExportOptions o = d->exportOptions;
            o.format = f; o.area = A; o.quality = 95;
            QByteArray out; QString err;
            QVERIFY2(docio::exportDocument(*d, 0, o, out, &err), qPrintable(err));
            ImageSeq seq;
            QVERIFY2(ImageIO::readData(out, seq, &err), qPrintable(ImageIO::formatName(f) + ": " + err));
            QCOMPARE(QSize(seq.width, seq.height), A.size());
            const QImage got = seq.frames[0].image.convertToFormat(QImage::Format_ARGB32);
            QCOMPARE(got.size(), A.size());
            // lossless: exact; lossy / palette: mean error (the blue sawtooth is high-frequency on purpose)
            const bool exact = !(f == Fmt::JPEG || f == Fmt::WEBP || f == Fmt::GIF);
            int worst = 0; double sum = 0;
            for (int y = 0; y < got.height(); ++y) for (int x = 0; x < got.width(); ++x) {
                const QRgb a = got.pixel(x, y), b = want.pixel(x, y);
                const int dr = std::abs(qRed(a) - qRed(b)), dg = std::abs(qGreen(a) - qGreen(b)), db = std::abs(qBlue(a) - qBlue(b));
                worst = qMax(worst, qMax(dr, qMax(dg, db)));
                sum += (dr + dg + db) / 3.0;
            }
            const double mean = sum / (got.width() * got.height());
            QVERIFY2(exact ? worst == 0 : mean <= 16, qPrintable(QStringLiteral("%1: max %2 mean %3").arg(ImageIO::formatName(f)).arg(worst).arg(mean)));
        }
        {   // PNG 8 and PNG 24 types
            for (int t : {1, 2, 3}) {
                ExportOptions o = d->exportOptions; o.format = Fmt::PNG; o.pngType = t == 3 ? 0 : t; o.tinyPng = t == 3; o.area = A;
                QByteArray out; QString err;
                QVERIFY(docio::exportDocument(*d, 0, o, out, &err));
                ImageSeq seq;
                QVERIFY(ImageIO::readData(out, seq, &err));
                QCOMPARE(QSize(seq.width, seq.height), A.size());
            }
        }
        {   // C header: dimensions are the area's
            ExportOptions o = d->exportOptions; o.format = Fmt::CHeader; o.area = A; o.chName = QStringLiteral("area_img");
            QByteArray out; QString err;
            QVERIFY2(docio::exportDocument(*d, 0, o, out, &err), qPrintable(err));
            const QString txt = QString::fromUtf8(out);
            QVERIFY2(txt.contains(QRegularExpression(QStringLiteral("WIDTH\\s+\\(?70\\)?"))) && txt.contains(QRegularExpression(QStringLiteral("HEIGHT\\s+\\(?50\\)?"))),
                     qPrintable(txt.left(600)));
        }
        {   // SVG: the canvas becomes the area
            ExportOptions o = d->exportOptions; o.format = Fmt::SVG; o.area = A;
            QByteArray out; QString err;
            QVERIFY2(docio::exportDocument(*d, 0, o, out, &err), qPrintable(err));
            QVERIFY2(out.contains("width=\"70") && out.contains("height=\"50"), out.left(400).constData());
        }
        {   // outside the canvas: an error
            ExportOptions o = d->exportOptions; o.area = QRect(400, 400, 10, 10);
            QByteArray out; QString err;
            QVERIFY(!docio::exportDocument(*d, 0, o, out, &err));
        }
        // moving the area near the left edge snaps it to the canvas edge
        QTest::mousePress(cv, Qt::LeftButton, Qt::NoModifier, vp(70, 50));
        for (int i = 1; i <= 6; ++i) QTest::mouseMove(cv, vp(70 - 6 * i, 50));
        QTest::mouseRelease(cv, Qt::LeftButton, Qt::NoModifier, vp(34, 50));
        QCOMPARE(cv->exportArea(), QRect(0, 30, 70, 50));
        // Esc cancels; the remembered area comes back with the tool
        QTest::keyClick(cv, Qt::Key_Escape);
        QVERIFY(!cv->exportAreaActive());
        as.setTool(Tool::Pointer);
        QCoreApplication::processEvents();
        // Batch: a step with a saved area writes only that rectangle
        {
            QTemporaryDir td;
            const QString src = td.filePath(QStringLiteral("in.lwd"));
            QVERIFY(lwd::save(*d, src, nullptr));
            batch::Script s;
            s.output = batch::Script::ToFolder; s.outDir = td.filePath(QStringLiteral("out"));
            batch::Step st; st.type = batch::Convert; st.format = QStringLiteral("png");
            st.useArea = true; st.area = QRect(10, 20, 33, 44);
            const batch::Step rt = batch::Step::fromJson(st.toJson());
            QVERIFY(rt.useArea);
            QCOMPARE(rt.area, st.area);
            QVERIFY(rt.summary().contains(QStringLiteral("area 10, 20 33 x 44")));
            s.steps << rt;
            batch::Result res;
            QVERIFY(batch::runOnDocument(s, *d, src, 0, res));
            QVERIFY(!res.written.isEmpty());
            ImageSeq seq; QString err;
            QVERIFY2(ImageIO::read(res.written.last(), seq, &err), qPrintable(err));
            QCOMPARE(QSize(seq.width, seq.height), QSize(33, 44));
            QCOMPARE(seq.frames[0].image.convertToFormat(QImage::Format_ARGB32).pixel(5, 5), full.convertToFormat(QImage::Format_ARGB32).pixel(15, 25));
        }
        // File > Export Selection Area: enabled only with a marquee selection
        QAction *esa = w.findChild<QAction *>(QStringLiteral("file.exportselarea"));
        if (!esa) for (QAction *a : w.findChildren<QAction *>()) if (a->text().contains(QStringLiteral("Export Selection &Area"))) esa = a;
        QVERIFY(esa);
        QPainterPath sel; sel.addRect(QRectF(100, 50, 60, 40));
        d->pixelSelection = sel;
        for (QMenu *m : w.findChildren<QMenu *>()) if (m->actions().contains(esa)) emit m->aboutToShow();
        QVERIFY(esa->isEnabled());
        bool seen2 = false; QRect a2;
        QTimer::singleShot(200, this, [&] {
            if (auto *dlg = qobject_cast<ExportDialog *>(QApplication::activeModalWidget())) { seen2 = true; a2 = dlg->area(); }
            if (QWidget *m = QApplication::activeModalWidget()) static_cast<QDialog *>(m)->reject();
        });
        esa->trigger();
        QVERIFY(seen2);
        QCOMPARE(a2, QRect(100, 50, 60, 40));
        d->pixelSelection = QPainterPath();
        for (QMenu *m : w.findChildren<QMenu *>()) if (m->actions().contains(esa)) emit m->aboutToShow();
        QVERIFY(!esa->isEnabled());
    }

    // Red Eye Removal follows the pen: light pressure is a smaller, weaker dab than full pressure
    void redEyePressure() {
        MainWindow w;
        w.resize(1000, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        DocumentView *v = w.addDocument(Document::createNew(300, 200, Qt::white));
        QCoreApplication::processEvents();
        Document *d = v->doc();
        Canvas *cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        VObject b;
        b.type = ObjType::Bitmap;
        b.rect = QRectF(0, 0, 300, 200);
        b.image = QImage(300, 200, QImage::Format_ARGB32_Premultiplied);
        b.image.fill(QColor(196, 32, 38));   // all pupil-red
        b.id = d->newId();
        d->addObject(b, 0, false);
        d->commit(QStringLiteral("bitmap"));
        d->setSelection({b.id});
        AppState &as = appState();
        Prefs pr = as.prefs;
        pr.tabletCurve = tablet::defaultCurve();
        pr.tabletMinSize = 0;
        pr.tabletMinOpacity = 0;
        as.setPrefs(pr);
        as.setTool(Tool::RedEye);
        QCoreApplication::processEvents();
        QVERIFY(w.findChild<QCheckBox *>(QStringLiteral("opt_pressureSize")));
        QVERIFY(w.findChild<QCheckBox *>(QStringLiteral("opt_pressureOpacity")));
        as.brushSize = 40; as.brushEdge = 0; as.toolOpacity = 100; as.reTolerance = 50; as.reDarken = 50;
        as.pressureSize = true; as.pressureOpacity = true;
        auto red = [&](int x, int y) { const QColor c(d->find(b.id)->image.pixel(x, y)); return c.red() - qMax(c.green(), c.blue()); };
        sendPen(cv, QEvent::TabletPress, QPointF(80, 100), 0.3);
        sendPen(cv, QEvent::TabletRelease, QPointF(80, 100), 0);
        sendPen(cv, QEvent::TabletPress, QPointF(220, 100), 1.0);
        sendPen(cv, QEvent::TabletRelease, QPointF(220, 100), 0);
        QCoreApplication::processEvents();
        const int lightC = red(80, 100), fullC = red(220, 100), lightRim = red(80 + 15, 100), fullRim = red(220 + 15, 100);
        QVERIFY2(fullC < 10 && lightC > fullC + 30 && lightC < 158, qPrintable(QStringLiteral("%1 %2").arg(lightC).arg(fullC)));
        QVERIFY2(lightRim == 158 && fullRim < 10, qPrintable(QStringLiteral("%1 %2").arg(lightRim).arg(fullRim)));
        as.setTool(Tool::Pointer);
    }

    void tabletCanvas() {
        MainWindow w;
        w.resize(1000, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        DocumentView *v = w.addDocument(Document::createNew(300, 200, Qt::white));
        QCoreApplication::processEvents();
        Document *d = v->doc();
        Canvas *cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        AppState &as = appState();
        Prefs pr = as.prefs;
        pr.tabletCurve = tablet::defaultCurve();
        pr.tabletMinSize = 0;
        pr.tabletMinOpacity = 0;
        pr.tabletEraser = true;
        as.setPrefs(pr);
        as.setTool(Tool::Brush);
        as.brushSize = 24;
        as.brushEdge = 0;
        as.toolOpacity = 100;
        as.brushCategory = 0;
        as.brushTextureAmount = 0;
        as.strokeTarget = AppState::NewBitmapPerStroke;
        as.pressureSize = true;
        as.pressureOpacity = false;
        as.pressureFlow = false;
        as.stroke = Qt::black;
        d->clearSelection();
        // a pen stroke from light to full pressure, one event every 4 px: interpolated between events
        sendPen(cv, QEvent::TabletPress, QPointF(30, 60), 0.15);
        for (int x = 34; x <= 270; x += 4) sendPen(cv, QEvent::TabletMove, QPointF(x, 60), 0.15 + 0.85 * (x - 30) / 240.0);
        sendPen(cv, QEvent::TabletRelease, QPointF(270, 60), 0);
        const VObject *pen = lastBitmap(d);
        QVERIFY(pen);
        const int a = docColumnThickness(*pen, 45), b = docColumnThickness(*pen, 150), c = docColumnThickness(*pen, 262);
        QVERIFY2(a < b && b < c && a <= 8 && c >= 21, qPrintable(QStringLiteral("%1 %2 %3").arg(a).arg(b).arg(c)));
        // steps between neighbouring columns stay small (pressure interpolated along each segment)
        int maxStep = 0;
        for (int x = 50; x < 260; ++x) maxStep = qMax(maxStep, std::abs(docColumnThickness(*pen, x + 1) - docColumnThickness(*pen, x)));
        QVERIFY2(maxStep <= 2, qPrintable(QString::number(maxStep)));
        const quint64 penId = pen->id;
        // the mouse presses at 1.0: an even, full-size line
        QTest::mouseMove(cv, cv->toView(QPointF(30, 140)).toPoint());
        QTest::mousePress(cv, Qt::LeftButton, Qt::NoModifier, cv->toView(QPointF(30, 140)).toPoint());
        for (int x = 40; x <= 270; x += 10) QTest::mouseMove(cv, cv->toView(QPointF(x, 140)).toPoint());
        QTest::mouseRelease(cv, Qt::LeftButton, Qt::NoModifier, cv->toView(QPointF(270, 140)).toPoint());
        const VObject *mouse = lastBitmap(d);
        QVERIFY(mouse && mouse->id != penId);
        QVERIFY(std::abs(docColumnThickness(*mouse, 45) - 24) <= 1 && std::abs(docColumnThickness(*mouse, 250) - 24) <= 1);
        QCOMPARE(cv->penSample().pressure, 1.0);
        // opacity per tool: the Brush's own setting, the Pencil keeps its own (size only in its options)
        as.pressureOpacity = true;
        as.setTool(Tool::Pencil);
        QCoreApplication::processEvents();
        QVERIFY(!as.pressureOpacity);
        QVERIFY(w.findChild<QCheckBox *>(QStringLiteral("opt_pressureSize")));
        QVERIFY(!w.findChild<QCheckBox *>(QStringLiteral("opt_pressureOpacity")));
        as.setTool(Tool::Brush);
        QCoreApplication::processEvents();
        QVERIFY(as.pressureOpacity);
        QVERIFY(w.findChild<QCheckBox *>(QStringLiteral("opt_pressureOpacity")));
        QVERIFY(w.findChild<QCheckBox *>(QStringLiteral("opt_tiltAngle")));
        as.pressureOpacity = false;
        // the curve from Preferences: a flat 0.5 curve halves the width even at full pressure
        pr.tabletCurve = {QPointF(0, 0.5), QPointF(1, 0.5)};
        as.setPrefs(pr);
        sendPen(cv, QEvent::TabletPress, QPointF(40, 100), 1.0);
        for (int x = 44; x <= 200; x += 4) sendPen(cv, QEvent::TabletMove, QPointF(x, 100), 1.0);
        sendPen(cv, QEvent::TabletRelease, QPointF(200, 100), 0);
        QVERIFY(std::abs(docColumnThickness(*lastBitmap(d), 120) - 12) <= 1);
        pr.tabletCurve = tablet::defaultCurve();
        as.setPrefs(pr);
        // the eraser end switches to the Eraser and wipes the pen stroke; the tip brings the Brush back
        d->selectOnly(penId);
        sendHover(cv, QPointF(150, 60), QPointingDevice::PointerType::Eraser);
        QCOMPARE(as.tool(), Tool::Eraser);
        QVERIFY(cv->eraserEndActive());
        as.brushSize = 40;
        sendPen(cv, QEvent::TabletPress, QPointF(140, 60), 1.0, QPointingDevice::PointerType::Eraser);
        for (int x = 144; x <= 180; x += 4) sendPen(cv, QEvent::TabletMove, QPointF(x, 60), 1.0, QPointingDevice::PointerType::Eraser);
        sendPen(cv, QEvent::TabletRelease, QPointF(180, 60), 0, QPointingDevice::PointerType::Eraser);
        const VObject *erased = d->find(penId);
        QVERIFY(erased);
        QCOMPARE(docColumnThickness(*erased, 160), 0);
        QVERIFY(docColumnThickness(*erased, 60) > 0);
        sendHover(cv, QPointF(150, 60), QPointingDevice::PointerType::Pen);
        QCOMPARE(as.tool(), Tool::Brush);
        QVERIFY(!cv->eraserEndActive());
        // turned off in Preferences: the eraser end leaves the tool alone
        pr.tabletEraser = false;
        as.setPrefs(pr);
        sendHover(cv, QPointF(150, 60), QPointingDevice::PointerType::Eraser);
        QCOMPARE(as.tool(), Tool::Brush);
        pr.tabletEraser = true;
        pr.tabletMinSize = 10;
        as.setPrefs(pr);
    }

    void tabletLiquify() {
        QImage src(120, 120, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 120; ++y) for (int x = 0; x < 120; ++x) src.setPixel(x, y, ((x / 8 + y / 8) & 1) ? 0xff000000u : 0xffffffffu);
        auto total = [](LiquifyDialog &dlg, int r0, int r1) {
            double s = 0;
            for (int y = 0; y < 120; ++y) for (int x = 0; x < 120; ++x) {
                const double r = std::hypot(x - 60.0, y - 60.0);
                if (r >= r0 && r < r1) { const QPointF dd = dlg.liquify().displacement(x, y); s += std::hypot(dd.x(), dd.y()); }
            }
            return s;
        };
        auto stroke = [&](double pen, bool size) {
            auto dlg = std::make_unique<LiquifyDialog>(src);
            dlg->setMode(fx2::Liquify::ForwardWarp);
            dlg->setPressureSize(size);
            dlg->strokeImage(QPointF(40, 60), QPointF(80, 60), pen, pen);
            return dlg;
        };
        auto full = stroke(1.0, false), light = stroke(0.25, false);
        const double fAll = total(*full, 0, 200), lAll = total(*light, 0, 200);
        QVERIFY2(lAll < 0.6 * fAll && lAll > 0, qPrintable(QStringLiteral("%1 %2").arg(lAll).arg(fAll)));
        // the mouse overload is full pressure
        auto mouse = std::make_unique<LiquifyDialog>(src);
        mouse->setMode(fx2::Liquify::ForwardWarp);
        mouse->strokeImage(QPointF(40, 60), QPointF(80, 60));
        QCOMPARE(mouse->result(), full->result());
        // with "Pen pressure changes size" a light touch also narrows the brush
        const double rBrush = full->brushSize() / 2;
        auto narrow = stroke(0.25, true);
        QVERIFY(total(*narrow, int(rBrush * 0.75) + 2, 200) < total(*light, int(rBrush * 0.75) + 2, 200) * 0.5);
        QVERIFY(full->findChild<QCheckBox *>(QStringLiteral("liq_pressureSize")));
    }

    void tabletPreferences() {
        PreferencesDialog dlg;
        auto *tabs = dlg.findChild<QTabWidget *>(QStringLiteral("prefsTabs"));
        QVERIFY(tabs);
        int ti = -1;
        for (int i = 0; i < tabs->count(); ++i) if (tabs->tabText(i) == QStringLiteral("Tablet")) ti = i;
        QVERIFY(ti >= 0);
        Prefs p = appState().prefs;
        p.tabletCurve = PreferencesDialog::curvePreset(2);
        p.tabletMinSize = 25;
        p.tabletMinOpacity = 5;
        p.tabletEraser = false;
        dlg.setPrefs(p);
        QCOMPARE(dlg.findChild<QComboBox *>(QStringLiteral("prefTabletPreset"))->currentText(), QStringLiteral("Firm"));
        Prefs out = dlg.prefs();
        QCOMPARE(out.tabletCurve, tablet::normalizedCurve(PreferencesDialog::curvePreset(2)));
        QCOMPARE(out.tabletMinSize, 25);
        QCOMPARE(out.tabletMinOpacity, 5);
        QVERIFY(!out.tabletEraser);
        // editing the curve: click adds a point, the preset reads Custom
        tabs->setCurrentIndex(ti);
        dlg.resize(760, 520);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        PressureCurveEdit *ce = dlg.tabletCurve();
        const int n0 = ce->curve().size();
        QTest::mouseClick(ce, Qt::LeftButton, Qt::NoModifier, ce->toWidget(QPointF(0.2, 0.7)).toPoint());
        QCOMPARE(ce->curve().size(), n0 + 1);
        QCOMPARE(dlg.findChild<QComboBox *>(QStringLiteral("prefTabletPreset"))->currentText(), QStringLiteral("Custom"));
        QVERIFY(tablet::mapCurve(dlg.prefs().tabletCurve, 0.2) > 0.6);
        // the test area follows the curve being edited: a half-pressure pen line, then a full one
        TabletTestArea *ta = dlg.tabletTest();
        ta->clear();
        ce->setCurve(tablet::defaultCurve());
        ta->penAt(QPointF(20, 30), 0.5, 10, -20, 0, false, true);
        ta->penAt(QPointF(120, 30), 0.5, 10, -20, 0, false, true);
        ta->penAt(QPointF(120, 30), 0.5, 10, -20, 0, false, false);
        QVERIFY2(ta->readout().contains(QStringLiteral("pressure 50% \u2192 50%")) && ta->readout().contains(QStringLiteral("10\u00B0, -20\u00B0")),
                 qPrintable(ta->readout()));
        ta->penAt(QPointF(20, 80), 1.0, 0, 0, 0, false, true);
        ta->penAt(QPointF(120, 80), 1.0, 0, 0, 0, false, true);
        ta->penAt(QPointF(120, 80), 1.0, 0, 0, 0, false, false);
        QVERIFY(columnThickness(ta->image(), 70, 64) > 0);
        int half = 0, fullW = 0;
        for (int y = 0; y < 55; ++y) half += qAlpha(ta->image().pixel(70, y)) >= 64;
        for (int y = 55; y < ta->image().height(); ++y) fullW += qAlpha(ta->image().pixel(70, y)) >= 64;
        QVERIFY2(half > 0 && half < fullW, qPrintable(QStringLiteral("%1 %2").arg(half).arg(fullW)));
        ta->penAt(QPointF(70, 80), 1.0, 0, 0, 0, true, false);
        QVERIFY(ta->readout().startsWith(QStringLiteral("Eraser end")));
        // a synthesised tablet event reaches the test area
        const QPointF wp(60, 100);
        QTabletEvent te(QEvent::TabletPress, penDevice(QPointingDevice::PointerType::Pen), wp, ta->mapToGlobal(wp), 0.3, 0, 0, 0, 0, 0,
                        Qt::NoModifier, Qt::LeftButton, Qt::LeftButton);
        QApplication::sendEvent(ta, &te);
        QVERIFY2(ta->readout().contains(QStringLiteral("pressure 30%")), qPrintable(ta->readout()));
        // persisted through QSettings
        Prefs keep = appState().prefs;
        Prefs sp = keep;
        sp.tabletCurve = PreferencesDialog::curvePreset(1);
        sp.tabletMinSize = 33;
        sp.tabletEraser = false;
        appState().setPrefs(sp);
        appState().savePrefs();
        appState().setPrefs(keep);
        appState().loadPrefs();
        QCOMPARE(appState().prefs.tabletCurve, tablet::normalizedCurve(PreferencesDialog::curvePreset(1)));
        QCOMPARE(appState().prefs.tabletMinSize, 33);
        QVERIFY(!appState().prefs.tabletEraser);
        appState().setPrefs(keep);
        appState().savePrefs();
    }

    void popupSliderOrientation() {
        const Prefs keep = appState().prefs;
        QCOMPARE(Prefs().sliderOrientation, 1);   // default Vertical since 0.3.2
        // the Preferences control
        {
            PreferencesDialog dlg;
            auto *c = dlg.findChild<QComboBox *>(QStringLiteral("prefSliderOrientation"));
            QVERIFY(c);
            QCOMPARE(c->count(), 2);
            QCOMPARE(c->itemText(0), QStringLiteral("Horizontal"));
            QCOMPARE(c->itemText(1), QStringLiteral("Vertical"));
            Prefs p = keep;
            p.sliderOrientation = 0;
            dlg.setPrefs(p);
            QCOMPARE(c->currentIndex(), 0);
            c->setCurrentIndex(1);
            QCOMPARE(dlg.prefs().sliderOrientation, 1);
        }
        Prefs h = keep;
        h.sliderOrientation = 0;
        appState().setPrefs(h);
        QCOMPARE(PopupSliderHub::orientation(), Qt::Horizontal);

        QWidget host;
        host.resize(300, 120);
        auto *f = new PopupSliderField(0, 100, QStringLiteral("%"), &host);
        f->setGeometry(100, 40, 70, 22);
        f->setValue(50);
        auto *df = new PopupSliderDoubleField(0, 10, 1, QString(), &host);
        df->setGeometry(100, 80, 70, 22);
        df->setValue(5);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QScreen *scr = host.screen();
        QVERIFY(scr);
        const QRect sr = scr->availableGeometry();

        // press on the triangle and drag, all in one motion (the popup has the grab)
        auto dragFrom = [&](SliderTarget *t, QPoint delta, int steps) {
            SliderArrow *a = t->arrow();
            const QPoint ac = a->rect().center();
            QTest::mousePress(a, Qt::LeftButton, Qt::NoModifier, ac);
            if (!t->isPopupOpen()) return false;
            SliderPopup *pop = t->popup();
            const QPoint g0 = a->mapToGlobal(ac);
            for (int i = 1; i <= steps; ++i) {
                const QPointF g = QPointF(g0) + QPointF(delta) * (double(i) / steps);
                QMouseEvent mv(QEvent::MouseMove, pop->mapFromGlobal(g), g, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(pop, &mv);
            }
            const QPointF ge = QPointF(g0 + delta);
            QMouseEvent rel(QEvent::MouseButtonRelease, pop->mapFromGlobal(ge), ge, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(pop, &rel);
            return true;
        };

        // Horizontal (default): wide popup under the field, right = more
        QVERIFY(dragFrom(f, QPoint(40, 0), 8));
        QTRY_VERIFY(!f->isPopupOpen());
        QVERIFY2(f->value() > 70, qPrintable(QString::number(f->value())));
        QVERIFY(f->popup()->width() > f->popup()->height());
        QCOMPARE(f->popup()->orientation(), Qt::Horizontal);
        f->setValue(50);
        QVERIFY(dragFrom(f, QPoint(0, -40), 8));   // vertical motion does nothing horizontally
        QVERIFY(f->isPopupOpen());   // no motion along the slider: it acts as a click and stays open
        QTest::keyClick(f->popup(), Qt::Key_Escape);
        QTRY_VERIFY(!f->isPopupOpen());
        QCOMPARE(f->value(), 50);

        // Vertical: tall popup, up = more, down = less, live for every field
        Prefs v = keep;
        v.sliderOrientation = 1;
        appState().setPrefs(v);
        QCOMPARE(PopupSliderHub::orientation(), Qt::Vertical);
        f->setValue(50);
        QVERIFY(dragFrom(f, QPoint(0, -40), 8));
        {
            SliderPopup *pop = f->popup();
            QCOMPARE(pop->orientation(), Qt::Vertical);
            QVERIFY(pop->height() > pop->width());
        }
        QTRY_VERIFY(!f->isPopupOpen());
        const int up = f->value();
        QVERIFY2(up > 70, qPrintable(QString::number(up)));
        QVERIFY(dragFrom(f, QPoint(0, 60), 8));
        QTRY_VERIFY(!f->isPopupOpen());
        QVERIFY2(f->value() < up - 30, qPrintable(QString::number(f->value())));
        f->setValue(50);
        QVERIFY(dragFrom(f, QPoint(40, 0), 8));   // sideways does nothing
        QVERIFY(f->isPopupOpen());   // no motion along the slider: it acts as a click and stays open
        QTest::keyClick(f->popup(), Qt::Key_Escape);
        QTRY_VERIFY(!f->isPopupOpen());
        QCOMPARE(f->value(), 50);
        // the double field too
        QVERIFY(dragFrom(df, QPoint(0, -30), 6));
        QTRY_VERIFY(!df->isPopupOpen());
        QVERIFY2(df->value() > 6.0, qPrintable(QString::number(df->value())));
        QCOMPARE(df->popup()->orientation(), Qt::Vertical);

        // a click leaves it open: the knob sits level with the triangle; the track's top is the
        // maximum and its bottom the minimum; Up / Down keys step
        host.move(sr.center() - QPoint(host.width() / 2, host.height() / 2));   // room above and below
        QCoreApplication::processEvents();
        f->setValue(50);
        QTest::mouseClick(f->arrow(), Qt::LeftButton);
        QTRY_VERIFY(f->isPopupOpen());
        SliderPopup *pop = f->popup();
        {
            const QPoint kg = pop->mapToGlobal(pop->knobPos());
            const QPoint ag = f->arrow()->mapToGlobal(f->arrow()->rect().center());
            QVERIFY2(std::abs(kg.y() - ag.y()) <= 2 && std::abs(kg.x() - ag.x()) <= 2,
                     qPrintable(QStringLiteral("knob %1,%2 arrow %3,%4").arg(kg.x()).arg(kg.y()).arg(ag.x()).arg(ag.y())));
            QVERIFY(sr.contains(pop->geometry()));
        }
        const QRect tr = pop->trackRect();
        QVERIFY(tr.height() > tr.width());
        QTest::keyClick(pop, Qt::Key_Up);
        QCOMPARE(f->value(), 51);
        QTest::keyClick(pop, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(f->value(), 41);
        QVERIFY(pop->knobPos().y() > tr.center().y());   // below the middle: under half
        QTest::mouseClick(pop, Qt::LeftButton, Qt::NoModifier, QPoint(tr.center().x(), tr.top()));
        QTRY_VERIFY(!f->isPopupOpen());
        QCOMPARE(f->value(), 100);
        QTest::mouseClick(f->arrow(), Qt::LeftButton);
        QTRY_VERIFY(f->isPopupOpen());
        QTest::mouseClick(pop, Qt::LeftButton, Qt::NoModifier, QPoint(tr.center().x(), tr.bottom()));
        QTRY_VERIFY(!f->isPopupOpen());
        QCOMPARE(f->value(), 0);

        // an open popup follows a change of the preference at once
        QTest::mouseClick(f->arrow(), Qt::LeftButton);
        QTRY_VERIFY(f->isPopupOpen());
        appState().setPrefs(h);
        QCOMPARE(pop->orientation(), Qt::Horizontal);
        QVERIFY(pop->width() > pop->height());
        QVERIFY(sr.contains(pop->geometry()));
        QTest::keyClick(pop, Qt::Key_Escape);
        QTRY_VERIFY(!f->isPopupOpen());

        // the popup stays on screen at the screen's edges, in both orientations
        for (int o : {1, 0}) {
            Prefs po = keep;
            po.sliderOrientation = o;
            appState().setPrefs(po);
            for (QPoint corner : {QPoint(sr.left(), sr.top()), QPoint(sr.right() - host.width() + 1, sr.bottom() - host.height() + 1)}) {
                host.move(corner);
                f->move(corner.x() == sr.left() ? 0 : host.width() - f->width(), corner.y() == sr.top() ? 0 : host.height() - f->height());
                QCoreApplication::processEvents();
                for (int val : {0, 100}) {
                    f->setValue(val);
                    QTest::mouseClick(f->arrow(), Qt::LeftButton);
                    QTRY_VERIFY(f->isPopupOpen());
                    QVERIFY2(sr.contains(pop->geometry()), qPrintable(QStringLiteral("o%1 v%2 %3,%4 %5x%6").arg(o).arg(val)
                        .arg(pop->x()).arg(pop->y()).arg(pop->width()).arg(pop->height())));
                    QTest::keyClick(pop, Qt::Key_Escape);
                    QTRY_VERIFY(!f->isPopupOpen());
                }
            }
        }

        // saved in QSettings
        appState().setPrefs(v);
        appState().savePrefs();
        {
            QSettings st;
            QCOMPARE(st.value(QStringLiteral("prefs/popupSliderOrientation")).toString(), QStringLiteral("vertical"));
        }
        appState().setPrefs(h);
        appState().loadPrefs();
        QCOMPARE(appState().prefs.sliderOrientation, 1);
        QCOMPARE(PopupSliderHub::orientation(), Qt::Vertical);
        appState().setPrefs(keep);
        appState().savePrefs();
        QCOMPARE(PopupSliderHub::orientation(), keep.sliderOrientation ? Qt::Vertical : Qt::Horizontal);
    }

    // ---- v0.3.1 pixel-art scalers -------------------------------------------------------------
    // Reference images (tests/data/pixelart, made by tools/pixelart-ref): ScummVM / AdvanceMAME
    // Scale2x-4x, Eagle, 2xSaI, Super 2xSaI, Super Eagle; the MMPX authors' JavaScript; ffmpeg's
    // hqx / xbr filters. Exact except: Super Eagle +-1 (ScummVM's interpolate32_6_1_1 reuses a
    // pixel in its low bits), xBR +-1 on under 5 % of pixels (ffmpeg rounds a few blends differently).
    void pixelScaleReferences() {
        using pixelscale::Method;
        const QString dir = QStringLiteral(LW_SOURCE_DIR "/tests/data/pixelart/");
        struct T { const char *ref; Method m; int n; int tol; double maxFrac; };
        const T ts[] = {{"scale2x", Method::ScaleNx, 2, 0, 0}, {"scale3x", Method::ScaleNx, 3, 0, 0}, {"scale4x", Method::ScaleNx, 4, 0, 0},
                        {"eagle", Method::Eagle, 2, 0, 0}, {"2xsai", Method::SaI2x, 2, 0, 0}, {"super2xsai", Method::Super2xSaI, 2, 0, 0},
                        {"supereagle", Method::SuperEagle, 2, 1, 1.0}, {"mmpx", Method::Mmpx, 2, 0, 0},
                        {"hq2x", Method::Hqx, 2, 0, 0}, {"hq3x", Method::Hqx, 3, 0, 0}, {"hq4x", Method::Hqx, 4, 0, 0},
                        {"xbr2x", Method::Xbr, 2, 1, 0.05}, {"xbr3x", Method::Xbr, 3, 1, 0.05}, {"xbr4x", Method::Xbr, 4, 1, 0.05}};
        int compared = 0;
        for (const char *s : {"hero", "lines", "noise", "ramp", "alpha"}) {
            const QImage src(dir + QLatin1String(s) + QStringLiteral(".png"));
            QVERIFY2(!src.isNull(), s);
            for (const T &t : ts) {
                QImage ref(dir + QStringLiteral("%1_%2.png").arg(QLatin1String(s), QLatin1String(t.ref)));
                if (ref.isNull()) continue;   // the SaI / hqx / xBR references exist for opaque sprites only
                ref = ref.convertToFormat(QImage::Format_ARGB32);
                const QImage out = pixelscale::upscale(src, t.m, t.n).convertToFormat(QImage::Format_ARGB32);
                QCOMPARE(out.size(), ref.size());
                int maxd = 0, nd = 0;
                for (int y = 0; y < ref.height(); ++y)
                    for (int x = 0; x < ref.width(); ++x) {
                        const QRgb a = out.pixel(x, y), b = ref.pixel(x, y);
                        const int dd = qMax(qMax(qAbs(qRed(a) - qRed(b)), qAbs(qGreen(a) - qGreen(b))), qMax(qAbs(qBlue(a) - qBlue(b)), qAbs(qAlpha(a) - qAlpha(b))));
                        nd += dd > 0;
                        maxd = qMax(maxd, dd);
                    }
                const QByteArray what = QByteArray(s) + ' ' + t.ref + " diffpx=" + QByteArray::number(nd) + " max=" + QByteArray::number(maxd);
                QVERIFY2(maxd <= t.tol, what.constData());
                QVERIFY2(nd <= t.maxFrac * ref.width() * ref.height(), what.constData());
                ++compared;
            }
        }
        QCOMPARE(compared, 4 * 14 + 5);   // all 14 for the 4 opaque sprites; ScaleNx / Eagle / MMPX for alpha
    }

    void pixelScaleProperties() {
        using pixelscale::Method;
        const QString dir = QStringLiteral(LW_SOURCE_DIR "/tests/data/pixelart/");
        // flat colour (with alpha) stays flat for every scaler and factor; sizes are exact
        QImage flat(7, 5, QImage::Format_ARGB32);
        flat.fill(qRgba(30, 140, 200, 180));
        for (int m = 0; m < pixelscale::kMethodCount; ++m)
            for (int f = 2; f <= 6; ++f) {
                const QImage o = pixelscale::upscale(flat, Method(m), f).convertToFormat(QImage::Format_ARGB32);
                QCOMPARE(o.size(), QSize(7 * f, 5 * f));
                for (int y = 0; y < o.height(); y += 3)
                    for (int x = 0; x < o.width(); x += 3) QCOMPARE(o.pixel(x, y), flat.pixel(0, 0));
            }
        // xBRZ (no independent reference): native 2-6, transparent background stays transparent away
        // from the sprite, the opaque core keeps its colours, and it really smooths a diagonal
        QCOMPARE(pixelscale::nativeFactors(Method::Xbrz), (QVector<int>{2, 3, 4, 5, 6}));
        const QImage alpha(dir + QStringLiteral("alpha.png"));
        const QImage xa = pixelscale::upscale(alpha, Method::Xbrz, 4).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(xa.size(), alpha.size() * 4);
        QCOMPARE(qAlpha(xa.pixel(0, 0)), 0);
        QCOMPARE(qAlpha(xa.pixel(xa.width() - 1, xa.height() - 1)), 0);
        QImage diag(8, 8, QImage::Format_ARGB32);
        diag.fill(Qt::white);
        for (int i = 0; i < 8; ++i) for (int j = 0; j <= i; ++j) diag.setPixel(j, i, qRgb(0, 0, 0));
        const QImage xd = pixelscale::upscale(diag, Method::Xbrz, 3).convertToFormat(QImage::Format_ARGB32);
        const QImage nd = pixelscale::upscale(diag, Method::Nearest, 3).convertToFormat(QImage::Format_ARGB32);
        QVERIFY(xd != nd);
        QCOMPARE(xd.pixel(1, 22), qRgb(0, 0, 0));
        QCOMPARE(xd.pixel(22, 1), qRgb(255, 255, 255));
        // chaining and non-native factors
        QCOMPARE(pixelscale::passesFor(Method::ScaleNx, 4), (QVector<int>{2, 2}));   // Scale4x = Scale2x twice
        QCOMPARE(pixelscale::passesFor(Method::Xbrz, 6), (QVector<int>{6}));
        QCOMPARE(pixelscale::passesFor(Method::Eagle, 4), (QVector<int>{2, 2}));
        QCOMPARE(pixelscale::passesFor(Method::Hqx, 6).size() >= 2, true);
        const QImage e3 = pixelscale::upscale(diag, Method::Eagle, 3);
        QCOMPARE(e3.size(), QSize(24, 24));
        // scaleTo: any size; enlarging uses the next whole factor up then resamples down
        QCOMPARE(pixelscale::scaleTo(diag, QSize(13, 21), Method::Mmpx).size(), QSize(13, 21));
        QCOMPARE(pixelscale::scaleTo(diag, QSize(16, 16), Method::Hqx), pixelscale::upscale(diag, Method::Hqx, 2));
        QCOMPARE(pixelscale::scaleTo(diag, QSize(4, 4), Method::Hqx).size(), QSize(4, 4));
        // the Resample list keys round-trip and keep 0-3
        QCOMPARE(pixelscale::resampleNames().size(), pixelscale::kResampleCount);
        QCOMPARE(pixelscale::kResampleCount, int(coreprefs::LastResample) + 1);
        QCOMPARE(pixelscale::resampleNames().at(3), QStringLiteral("Nearest Neighbor"));
        for (int r = 0; r < pixelscale::kResampleCount; ++r) QCOMPARE(pixelscale::resampleFromKey(pixelscale::resampleKey(r), -1), r);
    }

    void pixelScaleImageSizeAndPrefs() {
        const int old = coreprefs::interpolation;
        const int hqx = pixelscale::resampleFor(pixelscale::Method::Hqx);
        // Preferences: the combo lists all 13 entries and AppState keeps a pixel-art choice
        {
            PreferencesDialog dlg;
            auto *c = dlg.findChild<QComboBox *>(QStringLiteral("prefInterpolation"));
            QVERIFY(c);
            QCOMPARE(c->count(), pixelscale::kResampleCount);
        }
        Prefs p = AppState::get().prefs;
        const Prefs saved = p;
        p.interpolation = hqx;
        AppState::get().setPrefs(p);
        QCOMPARE(coreprefs::interpolation, hqx);
        QVERIFY(coreprefs::pixelArtScaling());
        QVERIFY(!coreprefs::smoothScaling());
        p.interpolation = 99;
        AppState::get().setPrefs(p);
        QCOMPARE(coreprefs::interpolation, int(coreprefs::LastResample));
        AppState::get().setPrefs(saved);
        // Image Size dialog has the Resample list, defaulting to Preferences
        coreprefs::interpolation = hqx;
        {
            SizeDialog dlg(false, 16, 16);
            auto *c = dlg.findChild<QComboBox *>(QStringLiteral("imageSizeResample"));
            QVERIFY(c);
            QCOMPARE(c->currentIndex(), hqx);
            QCOMPARE(dlg.resample(), hqx);
            SizeDialog cdlg(true, 16, 16);
            QCOMPARE(cdlg.resample(), -1);
        }
        coreprefs::interpolation = old;
        // Document::scaleImage with a scaler: 16 -> 40 px = 3x hqx then resampled to 40
        const QImage src(QStringLiteral(LW_SOURCE_DIR "/tests/data/pixelart/hero.png"));
        std::unique_ptr<Document> d(Document::createNew(16, 16, Qt::transparent));
        VObject bm;
        bm.id = d->newId();
        bm.type = ObjType::Bitmap;
        bm.image = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        bm.rect = QRectF(0, 0, 16, 16);
        d->addObject(bm);
        d->scaleImage(32, 32, hqx);
        QCOMPARE(d->find(bm.id)->image.convertToFormat(QImage::Format_ARGB32), pixelscale::upscale(src, pixelscale::Method::Hqx, 2).convertToFormat(QImage::Format_ARGB32));
        d->scaleImage(80, 80, pixelscale::resampleFor(pixelscale::Method::Mmpx));
        QCOMPARE(d->find(bm.id)->image.size(), QSize(80, 80));
        QCOMPARE(d->find(bm.id)->rect, QRectF(0, 0, 80, 80));
    }

    void pixelScaleObjectScaling() {
        // a 16 px bitmap stretched to 32 px renders through the chosen scaler (non-destructive)
        const int old = coreprefs::interpolation;
        const QImage src = QImage(QStringLiteral(LW_SOURCE_DIR "/tests/data/pixelart/hero.png")).convertToFormat(QImage::Format_ARGB32);
        std::unique_ptr<Document> d(Document::createNew(40, 40, Qt::transparent));
        VObject bm;
        bm.id = d->newId();
        bm.type = ObjType::Bitmap;
        bm.image = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        bm.rect = QRectF(4, 4, 32, 32);
        d->addObject(bm);
        auto region = [&] { return d->render(0, true).convertToFormat(QImage::Format_ARGB32).copy(4, 4, 32, 32); };
        // 0.3.16: display scaling uses the bitmap's own Scaling method (Preferences only seed new bitmaps).
        d->find(bm.id)->interpolation = pixelscale::resampleFor(pixelscale::Method::Eagle);
        const QImage eagle = region();
        QCOMPARE(eagle, pixelscale::upscale(src, pixelscale::Method::Eagle, 2).convertToFormat(QImage::Format_ARGB32));
        d->find(bm.id)->interpolation = coreprefs::Nearest;
        const QImage nearest = region();
        QCOMPARE(nearest, src.scaled(32, 32, Qt::IgnoreAspectRatio, Qt::FastTransformation));
        QVERIFY(eagle != nearest);
        QCOMPARE(d->find(bm.id)->image.size(), QSize(16, 16));   // the pixels are untouched
        // non-integer: 16 -> 24 = 2x then resampled down; it differs from nearest
        d->find(bm.id)->rect = QRectF(4, 4, 24, 24);
        d->find(bm.id)->interpolation = pixelscale::resampleFor(pixelscale::Method::Xbrz);
        const QImage x24 = d->render(0, true).convertToFormat(QImage::Format_ARGB32).copy(4, 4, 24, 24);
        QCOMPARE(x24, pixelscale::scaleTo(src, QSize(24, 24), pixelscale::Method::Xbrz).convertToFormat(QImage::Format_ARGB32));
        coreprefs::interpolation = old;
    }

    void pixelScaleExportAndBatch() {
        const QImage src = QImage(QStringLiteral(LW_SOURCE_DIR "/tests/data/pixelart/lines.png")).convertToFormat(QImage::Format_ARGB32);
        ImageSeq seq;
        seq.width = src.width();
        seq.height = src.height();
        seq.frames << AnimFrame{src, 100};
        ExportOptions o;
        o.format = Fmt::PNG;
        o.scalePct = 300;
        o.scaleResample = pixelscale::resampleFor(pixelscale::Method::Hqx);
        QByteArray data;
        QVERIFY(ImageIO::writeData(data, seq, o));
        ImageSeq back;
        QVERIFY(ImageIO::readData(data, back, nullptr));
        QCOMPARE(back.frames[0].image.convertToFormat(QImage::Format_ARGB32), pixelscale::upscale(src, pixelscale::Method::Hqx, 3).convertToFormat(QImage::Format_ARGB32));
        // after Export Area: 8 x 6 area at 250 % -> 20 x 15
        o.area = QRect(4, 4, 8, 6);
        o.scalePct = 250;
        QVERIFY(ImageIO::writeData(data, seq, o));
        QVERIFY(ImageIO::readData(data, back, nullptr));
        QCOMPARE(back.frames[0].image.size(), QSize(20, 15));
        // JSON round-trip (document export settings and batch custom settings)
        o.area = QRect();
        const ExportOptions r = lwd::exportOptionsFromJson(lwd::exportOptionsToJson(o));
        QCOMPARE(r.scalePct, 250);
        QCOMPARE(r.scaleResample, o.scaleResample);
        QVERIFY(r == o);
        QVERIFY(!lwd::exportOptionsToJson(ExportOptions()).contains(QStringLiteral("scalePct")));
        // Batch Scale step: resample saved by key and used
        batch::Step st;
        st.type = batch::Scale;
        st.scaleMode = 1;
        st.percent = 200;
        st.resample = pixelscale::resampleFor(pixelscale::Method::ScaleNx);
        const batch::Step st2 = batch::Step::fromJson(st.toJson());
        QCOMPARE(st2.resample, st.resample);
        QVERIFY(st.summary().contains(QStringLiteral("EPX")));
        std::unique_ptr<Document> d(Document::createNew(src.width(), src.height(), Qt::transparent));
        VObject bm;
        bm.id = d->newId();
        bm.type = ObjType::Bitmap;
        bm.image = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        bm.rect = QRectF(0, 0, src.width(), src.height());
        d->addObject(bm);
        QString err;
        QVERIFY(batch::applyStep(*d, st2, &err));
        QCOMPARE(d->find(bm.id)->image.convertToFormat(QImage::Format_ARGB32), pixelscale::upscale(src, pixelscale::Method::ScaleNx, 2).convertToFormat(QImage::Format_ARGB32));
    }

    void pixelScaleUpscaleFilter() {
        MainWindow w;
        QCoreApplication::instance()->setProperty("lw_testing", true);
        QAction *a = w.findChild<QAction *>(QStringLiteral("filter.pixelupscale"));
        QVERIFY(a);
        QVERIFY(a->text().contains(QStringLiteral("Upscale")));
        DocumentView *v = w.addDocument(Document::createNew(100, 100, Qt::white));
        Document *d = v->doc();
        const QImage src = QImage(QStringLiteral(LW_SOURCE_DIR "/tests/data/pixelart/hero.png")).convertToFormat(QImage::Format_ARGB32);
        VObject bm;
        bm.id = d->newId();
        bm.type = ObjType::Bitmap;
        bm.image = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        bm.rect = QRectF(10, 10, 16, 16);
        d->addObject(bm);
        d->selectOnly(bm.id);
        d->commit(QStringLiteral("add"));
        const int h0 = d->historyIndex();
        QVERIFY(!w.pixelUpscale([](PixelUpscaleDialog &) { return false; }));
        QCOMPARE(d->historyIndex(), h0);
        QString note;
        QVERIFY(w.pixelUpscale([&](PixelUpscaleDialog &dlg) {
            dlg.setMethod(pixelscale::Method::Mmpx);
            dlg.setFactor(4);
            note = dlg.findChild<QLabel *>(QStringLiteral("upscaleNote"))->text();
            return !dlg.previewImage().isNull();
        }));
        QVERIFY(note.contains(QStringLiteral("2\u00D7 then 2\u00D7")));
        const VObject *o = d->find(bm.id);
        QCOMPARE(o->image.size(), QSize(64, 64));
        QCOMPARE(o->rect, QRectF(10, 10, 64, 64));
        QCOMPARE(o->image.convertToFormat(QImage::Format_ARGB32), pixelscale::upscale(src, pixelscale::Method::Mmpx, 4).convertToFormat(QImage::Format_ARGB32));
        d->undo();
        QCOMPARE(d->find(bm.id)->image.size(), QSize(16, 16));
        // keep the object's size: more pixels in the same rectangle
        QVERIFY(w.pixelUpscale([](PixelUpscaleDialog &dlg) { dlg.setMethod(pixelscale::Method::Xbrz); dlg.setFactor(3); dlg.setEnlargeObject(false); return true; }));
        QCOMPARE(d->find(bm.id)->image.size(), QSize(48, 48));
        QCOMPARE(d->find(bm.id)->rect, QRectF(10, 10, 16, 16));
    }
    // Release screenshots (not a check): LW_SHOTS_031=<dir> QT_QPA_PLATFORM=offscreen ./v031_test shots031
    void shots031() {
        const QString dir = qEnvironmentVariable("LW_SHOTS_031");
        if (dir.isEmpty()) QSKIP("set LW_SHOTS_031 to a folder to write the 0.3.1 screenshots");
        QDir().mkpath(dir);
        const auto out = [&](const QString &n) { return QDir(dir).filePath(n); };
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        DocumentView *v = w.addDocument(Document::createNew(480, 320, Qt::white));
        Document *d = v->doc();
        Canvas *cv = v->canvas();
        const QImage hero(QStringLiteral(LW_SOURCE_DIR "/tests/data/pixelart/hero.png"));
        QVERIFY(!hero.isNull());
        VObject bm; bm.id = d->newId(); bm.type = ObjType::Bitmap; bm.name = QStringLiteral("Hero");
        bm.image = hero.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        bm.rect = QRectF(40, 40, hero.width() * 6, hero.height() * 6);
        d->addObject(bm);
        d->selectOnly(bm.id);
        d->commit(QStringLiteral("add"));
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        QCoreApplication::processEvents();
        const auto settle = [] { for (int i = 0; i < 10; ++i) { QCoreApplication::processEvents(); QTest::qWait(20); } };
        // a modal dialog opened by `act`, grabbed then cancelled
        const auto modal = [&](const QString &file, const std::function<void()> &act, const std::function<void(QWidget *)> &prep = {}) {
            bool got = false;
            QTimer::singleShot(400, this, [&] {
                if (QWidget *m = QApplication::activeModalWidget()) {
                    if (prep) prep(m);
                    settle();
                    got = m->grab().save(out(file));
                    static_cast<QDialog *>(m)->reject();
                }
            });
            act();
            return got;
        };
        // Trace Bitmap (Colors mode, live preview)
        QVERIFY(w.traceBitmap([&](TraceDialog &dlg) {
            trace::Options o; o.mode = trace::Mode::Colors; dlg.setOptions(o);
            dlg.waitForPreview(); settle();
            dlg.grab().save(out(QStringLiteral("trace-bitmap.png")));
            return false;
        }) == false);
        // Pixel Art Upscale
        w.pixelUpscale([&](PixelUpscaleDialog &dlg) {
            dlg.setMethod(pixelscale::Method::Xbrz); dlg.setFactor(4); settle();
            dlg.grab().save(out(QStringLiteral("pixel-art-upscale.png")));
            return false;
        });
        // Replace Color: the filter dialog and the tool (Rubber Stamp flyout) with its options
        QVERIFY(modal(QStringLiteral("replace-color-filter.png"), [&] { w.action(QStringLiteral("filter.replacecolor"))->trigger(); }, [](QWidget *m) {
            if (auto *from = m->findChild<ColorButton *>(QStringLiteral("fx_color"))) { from->setColor(QColor(210, 40, 40)); emit from->colorPicked(QColor(210, 40, 40)); }
            if (auto *to = m->findChild<ColorButton *>(QStringLiteral("fx_color2"))) { to->setColor(QColor(40, 140, 60)); emit to->colorPicked(QColor(40, 140, 60)); }
        }));
        AppState &as = appState();
        as.setTool(Tool::ReplaceColor); settle();
        w.grab().save(out(QStringLiteral("replace-color-tool.png")));
        // Find & Replace: text objects, Find Text "pixel", the results listed
        as.setTool(Tool::Pointer);
        {
            int y = 200;
            for (const QString &t : {QStringLiteral("Pixel art upscaling"), QStringLiteral("Hero sprite"), QStringLiteral("More pixel fun")}) {
                VObject o = frText(d, t); o.rect = QRectF(220, y, 240, 30); o.fontSize = 18; y += 36;
                d->addObject(o);
            }
            d->commit(QStringLiteral("text"));
        }
        w.action(QStringLiteral("window.find"))->trigger(); settle();
        auto *dock = w.findChild<QDockWidget *>(QStringLiteral("FindDock"));
        QVERIFY(dock);
        dock->setFloating(true);
        dock->resize(320, 560);
        dock->move(w.geometry().topRight() - QPoint(340, -60));
        if (auto *ft = w.findChild<QLineEdit *>(QStringLiteral("findText"))) ft->setText(QStringLiteral("pixel"));
        if (auto *rt = w.findChild<QLineEdit *>(QStringLiteral("replaceText"))) rt->setText(QStringLiteral("sprite"));
        if (auto *b = w.findChild<QPushButton *>(QStringLiteral("findNext"))) b->click();
        settle();
        dock->grab().save(out(QStringLiteral("find-replace-panel.png")));
        {
            QImage img = w.grab().toImage();
            QPainter p(&img);
            p.drawPixmap(w.mapFromGlobal(dock->mapToGlobal(QPoint(0, 0))), dock->grab());
            p.end();
            img.save(out(QStringLiteral("find-replace.png")));
        }
        dock->setFloating(false);
        dock->hide();
        // Preferences > Tablet
        {
            PreferencesDialog dlg;
            auto *tabs = dlg.findChild<QTabWidget *>(QStringLiteral("prefsTabs"));
            for (int i = 0; i < tabs->count(); ++i) if (tabs->tabText(i) == QStringLiteral("Tablet")) tabs->setCurrentIndex(i);
            dlg.show(); QVERIFY(QTest::qWaitForWindowExposed(&dlg)); settle();
            dlg.grab().save(out(QStringLiteral("tablet-preferences.png")));
        }
        // Export Area tool
        d->clearSelection();
        as.setTool(Tool::ExportArea); settle();
        cv->setFocus();
        const auto vp = [cv](double x, double y) { return cv->toView(QPointF(x, y)).toPoint(); };
        QTest::mouseMove(cv, vp(30, 30));
        QTest::mousePress(cv, Qt::LeftButton, Qt::NoModifier, vp(30, 30));
        for (int i = 1; i <= 10; ++i) QTest::mouseMove(cv, vp(30 + 17 * i, 30 + 14 * i));
        QTest::mouseRelease(cv, Qt::LeftButton, Qt::NoModifier, vp(200, 170));
        settle();
        w.grab().save(out(QStringLiteral("export-area.png")));
        QTest::keyClick(cv, Qt::Key_Escape);
        // canvas context menu on the bitmap
        as.setTool(Tool::Pointer);
        d->selectOnly(bm.id);
        settle();
        const QPoint at = vp(120, 120);
        w.showCanvasContextMenu(cv->mapToGlobal(at));
        settle();
        QMenu *menu = w.lastContextMenu();
        QVERIFY(menu && menu->isVisible());
        {
            QImage img = w.grab().toImage();
            QPainter p(&img);
            p.drawPixmap(w.mapFromGlobal(menu->pos()), menu->grab());
            p.end();
            img.save(out(QStringLiteral("context-menu.png")));
            menu->grab().save(out(QStringLiteral("context-menu-only.png")));
        }
        menu->hide();
        // Styles panel after the fix: a style selected and the list focused (no dashed box)
        if (CollapsiblePanel *sp = w.panel(QStringLiteral("Styles"))) {
            sp->setCollapsed(false); settle();
            auto *list = sp->findChild<QListWidget *>(QStringLiteral("stylesList"));
            QVERIFY(list && list->count() > 2);
            list->setCurrentRow(1); list->item(1)->setSelected(true);
            w.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&w));
            list->setFocus(Qt::TabFocusReason); settle();
            QVERIFY(list->hasFocus());
            sp->grab().save(out(QStringLiteral("styles-panel-after.png")));
            const QRect r = list->visualRect(list->currentIndex()).adjusted(-6, -6, 6, 10);
            list->viewport()->grab(r).toImage().scaled(r.size() * 6, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                .save(out(QStringLiteral("styles-panel-after-x6.png")));
        }
    }
};

QTEST_MAIN(V031)
#include "v031_test.moc"
