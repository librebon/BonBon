// v0.3.4 tests: pixel tools on bitmaps with a live Offset effect act where the pointer is (mouse
// driven, one per tool), lossless un-shift / re-shift, Lens Correction / Make Seamless refuse with a
// hint, Liquify sees the displayed image, and bitmaps trim to their pixels after edits.
#include "core/Document.h"
#include "core/Effects.h"
#include "core/Filters.h"
#include "core/PixelEdit.h"
#include "core/PixelScale.h"
#include "core/CorePrefs.h"
#include "core/Transform.h"
#include "core/Fills.h"
#include "core/Styles.h"
#include "core/Render.h"
#include <QLabel>
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/LiquifyDialog.h"
#include "ui/MainWindow.h"
#include "ui/PopupSlider.h"
#include "ui/ColorPicker.h"
#include "ui/Dialogs.h"
#include <QDialog>
#include <QScreen>
#include <QVBoxLayout>
#include "ui/Panels.h"
#include "ui/PropertyInspector.h"
#include "io/DocIO.h"
#include "io/LwdFormat.h"
#include "io/Zip.h"
#include "io/Codecs.h"
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QToolButton>
#include <QTreeWidget>
#include <QScrollBar>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QAction>
#include <QApplication>
#include <QRandomGenerator>
#include <QtTest>
#include <memory>


static void collectIds(const QVector<VObject> &v, QVector<quint64> &out) {
    for (const VObject &o : v) { out << o.id; collectIds(o.children, out); }
}
static VObject testShape(Document &d, QRectF r) {
    VObject o;
    o.type = ObjType::Rect;
    o.rect = r;
    o.id = d.newId();
    return o;
}
// a minimal RGB PSD: one 1x1 composite plus one layer record whose bounds and channel data come from the caller
static QByteArray craftPsd(qint32 top, qint32 left, qint32 bottom, qint32 right, quint16 comp, const QByteArray &chData) {
    QByteArray b;
    auto u8 = [&](int v) { b.append(char(v)); };
    auto u16 = [&](int v) { u8(v >> 8); u8(v & 255); };
    auto u32 = [&](quint32 v) { u16(int(v >> 16)); u16(int(v & 0xFFFF)); };
    b.append("8BPS"); u16(1); b.append(QByteArray(6, 0)); u16(3); u32(1); u32(1); u16(8); u16(3);
    u32(0); u32(0);   // colour mode data, image resources
    QByteArray rec;
    {
        QByteArray keep = b; b.clear();
        u32(quint32(top)); u32(quint32(left)); u32(quint32(bottom)); u32(quint32(right));
        u16(4);
        for (int id : {-1, 0, 1, 2}) { u16(id & 0xFFFF); u32(quint32(2 + chData.size())); }
        b.append("8BIMnorm"); u8(255); u8(0); u8(0); u8(0);
        u32(12); u32(0); u32(0); u8(3); b.append("bad");   // mask, blend ranges, Pascal name (4-padded)
        rec = b; b = keep;
    }
    QByteArray chans;
    for (int k = 0; k < 4; ++k) { chans.append(char(comp >> 8)); chans.append(char(comp & 255)); chans.append(chData); }
    QByteArray info;
    { QByteArray keep = b; b.clear(); u16(1); b.append(rec); b.append(chans); if (b.size() % 2) u8(0); info = b; b = keep; }
    u32(quint32(4 + info.size() + 4)); u32(quint32(info.size())); b.append(info); u32(0);
    u16(0); b.append(QByteArray(3, char(200)));   // raw 1x1 composite
    return b;
}

static void drainEvents() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

// reddish noise: every tool has something to change (red eye, replace colour, blur, ...)
static QImage noise(int w, int h, quint32 seed = 7) {
    QImage im(w, h, QImage::Format_ARGB32_Premultiplied);
    QRandomGenerator g(seed);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int v = 90 + int(g.bounded(80));
            im.setPixel(x, y, qRgb(qMin(255, v + 90), v / 3, v / 3));
        }
    return im;
}


// ---- in-place rename helpers
static VObject rshape(Document *d, const QRectF &r, const QString &name) {
    VObject o; o.type = ObjType::Rect; o.rect = r; o.fillOn = true; o.fill = QColor(40, 90, 200); o.id = d->newId(); o.name = name;
    return o;
}
static QTreeWidgetItem *rowFor(QTreeWidget *t, quint64 id, int kind) {
    for (QTreeWidgetItemIterator it(t); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toULongLong() == id && (*it)->data(0, Qt::UserRole + 1).toInt() == kind) return *it;
    return nullptr;
}
static QTreeWidgetItem *layerRow(QTreeWidget *t, int layer) {
    for (int i = 0; i < t->topLevelItemCount(); ++i)
        if (t->topLevelItem(i)->data(1, Qt::UserRole).toInt() == layer) return t->topLevelItem(i);
    return nullptr;
}
static QLineEdit *inlineEditor(QWidget *view) {
    for (QLineEdit *e : view->findChildren<QLineEdit *>(QStringLiteral("inlineRename"))) if (e->isVisible()) return e;
    return nullptr;
}

// ---- 0.3.4 performance work: the scalar 0.3.3 box blur, the reference the fast blur must match bit for bit
static void refBoxPass(const uchar *src, uchar *dst, int n, int stride, int ch, int r) {
    const int win = 2 * r + 1;
    for (int c = 0; c < ch; ++c) {
        int sum = 0;
        for (int i = -r; i <= r; ++i) sum += src[qBound(0, i, n - 1) * stride + c];
        for (int i = 0; i < n; ++i) {
            dst[i * stride + c] = uchar((sum + win / 2) / win);
            sum += src[qMin(i + r + 1, n - 1) * stride + c] - src[qMax(i - r, 0) * stride + c];
        }
    }
}
static void refBlur(uchar *data, int w, int h, int bpl, int ch, double radius) {
    if (radius <= 0.01 || w <= 0 || h <= 0) return;
    const int r = qMax(1, qRound(radius * 0.8));
    std::vector<uchar> line(size_t(qMax(w, h)) * ch), out(size_t(qMax(w, h)) * ch);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            uchar *row = data + size_t(y) * bpl;
            std::memcpy(line.data(), row, size_t(w) * ch);
            refBoxPass(line.data(), out.data(), w, ch, ch, r);
            std::memcpy(row, out.data(), size_t(w) * ch);
        }
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) std::memcpy(&line[size_t(y) * ch], data + size_t(y) * bpl + size_t(x) * ch, ch);
            refBoxPass(line.data(), out.data(), h, ch, ch, r);
            for (int y = 0; y < h; ++y) std::memcpy(data + size_t(y) * bpl + size_t(x) * ch, &out[size_t(y) * ch], ch);
        }
    }
}
static bool samePixels(const QImage &a, const QImage &b) {
    if (a.size() != b.size() || a.format() != b.format()) return false;
    const int rowBytes = (a.width() * a.depth() + 7) / 8;
    for (int y = 0; y < a.height(); ++y) if (std::memcmp(a.constScanLine(y), b.constScanLine(y), size_t(rowBytes))) return false;
    return true;
}

enum class Op { Drag, Stamp, Bucket, Clear, Cut, CAFill, Filter, Wand };

class V034 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;
    Document *d = nullptr;
    Canvas *cv = nullptr;
    quint64 id = 0;

    void newDoc(int mode, QPointF off = QPointF(20, 10), QRectF rect = QRectF(50, 50, 100, 100), QSize img = QSize(100, 100)) {
        d = Document::createNew(300, 300, Qt::white);
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        drainEvents();
        VObject o;
        o.type = ObjType::Bitmap;
        o.name = QStringLiteral("B");
        o.rect = rect;
        o.image = noise(img.width(), img.height());
        o.id = d->newId();
        id = o.id;
        if (mode >= 0) {
            LiveEffect e = LiveEffect::make(EffectType::Offset);
            e.offsetX = off.x();
            e.offsetY = off.y();
            e.undefinedAreas = mode;
            o.effects << e;
        }
        d->addObject(o);
        d->selectOnly(o.id);
        d->commit(QStringLiteral("add"));
        drainEvents();
    }
    void closeDoc() {
        if (d) d->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        drainEvents();
        d = nullptr;
    }
    QPoint vp(QPointF p) const { return cv->toView(p).toPoint(); }
    void drag(QPointF a, QPointF z, Qt::KeyboardModifiers m = Qt::NoModifier) {
        QTest::mouseMove(cv, vp(a));
        QTest::mousePress(cv, Qt::LeftButton, m, vp(a));
        for (int k = 1; k <= 8; ++k) QTest::mouseMove(cv, vp(a + (z - a) * (k / 8.0)));
        QTest::mouseRelease(cv, Qt::LeftButton, m, vp(z));
        drainEvents();
    }
    void waitIdle() {
        for (int i = 0; i < 100; ++i) { QTest::qWait(30); drainEvents(); }
    }
    static void toolDefaults(Tool t) {
        AppState &as = appState();
        as.setTool(t);
        as.brushSize = 10;
        as.brushEdge = 0;
        as.toolOpacity = 100;
        as.stroke = Qt::green;
        as.fill = Qt::green;
        as.rcFrom = QColor(200, 40, 40);
        as.rcTo = QColor(0, 0, 255);
        as.rcTolerance = 100;
        as.reTolerance = 100;
        as.tolerance = 255;
        as.useEntireDocument = false;
        as.fillSelectionOnly = false;
        as.strokeTarget = AppState::ClipToSelected;
    }

private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        w = std::make_unique<MainWindow>();
        w->resize(1000, 800);
        w->show();
        drainEvents();
    }
    void cleanupTestCase() { w.reset(); }

    // Every pixel tool, plain and with each Offset mode: the changed pixels centre on the pointer.
    void toolsActWhereThePointerIs_data() {
        QTest::addColumn<int>("tool");
        QTest::addColumn<int>("op");
        QTest::addColumn<int>("mode");
        struct T { Tool t; Op op; const char *n; };
        const T tools[] = {
            {Tool::Brush, Op::Drag, "Brush"}, {Tool::Pencil, Op::Drag, "Pencil"}, {Tool::Eraser, Op::Drag, "Eraser"},
            {Tool::Blur, Op::Drag, "Blur"}, {Tool::Sharpen, Op::Drag, "Sharpen"}, {Tool::Dodge, Op::Drag, "Dodge"},
            {Tool::Burn, Op::Drag, "Burn"}, {Tool::Smudge, Op::Drag, "Smudge"}, {Tool::RubberStamp, Op::Stamp, "RubberStamp"},
            {Tool::Healing, Op::Stamp, "Healing"}, {Tool::SpotHealing, Op::Drag, "SpotHealing"},
            {Tool::ReplaceColor, Op::Drag, "ReplaceColor"}, {Tool::RedEye, Op::Drag, "RedEye"},
            {Tool::ContentAware, Op::Drag, "ContentAware"}, {Tool::PaintBucket, Op::Bucket, "PaintBucket"},
            {Tool::Marquee, Op::Clear, "Marquee+Clear"}, {Tool::Marquee, Op::Cut, "Marquee+Cut"},
            {Tool::Marquee, Op::CAFill, "Marquee+CAFill"}, {Tool::Marquee, Op::Filter, "Marquee+Invert"},
            {Tool::Lasso, Op::Clear, "Lasso+Clear"},
        };
        const char *modes[] = {"plain", "wrap", "repeat", "transparent"};
        for (int m = -1; m <= 2; ++m)
            for (const T &t : tools)
                QTest::newRow(qPrintable(QStringLiteral("%1/%2").arg(QString::fromLatin1(t.n), QString::fromLatin1(modes[m + 1]))))
                    << int(t.t) << int(t.op) << m;
    }
    void toolsActWhereThePointerIs() {
        QFETCH(int, tool);
        QFETCH(int, op);
        QFETCH(int, mode);
        newDoc(mode);
        toolDefaults(Tool(tool));
        drainEvents();
        const QImage before = d->render(0, true);
        const QPointF P(110, 110);
        switch (Op(op)) {
        case Op::Drag: drag(P - QPointF(4, 0), P + QPointF(4, 0)); break;
        case Op::Stamp:
            QTest::mouseClick(cv, Qt::LeftButton, Qt::AltModifier, vp(P + QPointF(-30, 20)));
            drainEvents();
            drag(P - QPointF(4, 0), P + QPointF(4, 0));
            break;
        case Op::Bucket: {
            // a selection limits the flood to a small square around P
            appState().setTool(Tool::Marquee);
            drag(P - QPointF(5, 5), P + QPointF(5, 5));
            d->selectOnly(id);
            appState().setTool(Tool::PaintBucket);
            appState().fillSelectionOnly = true;
            QTest::mouseClick(cv, Qt::LeftButton, Qt::NoModifier, vp(P));
            drainEvents();
            break;
        }
        default: {
            if (Tool(tool) == Tool::Lasso) {
                QTest::mouseMove(cv, vp(P + QPointF(-5, -5)));
                QTest::mousePress(cv, Qt::LeftButton, Qt::NoModifier, vp(P + QPointF(-5, -5)));
                for (QPointF q : {QPointF(5, -5), QPointF(5, 5), QPointF(-5, 5), QPointF(-5, -5)}) QTest::mouseMove(cv, vp(P + q));
                QTest::mouseRelease(cv, Qt::LeftButton, Qt::NoModifier, vp(P + QPointF(-5, -5)));
                drainEvents();
            } else {
                drag(P - QPointF(5, 5), P + QPointF(5, 5));
            }
            d->selectOnly(id);
            if (Op(op) == Op::Clear) w->action(QStringLiteral("edit.clear"))->trigger();
            if (Op(op) == Op::Cut) w->action(QStringLiteral("edit.cut"))->trigger();
            if (Op(op) == Op::CAFill) w->contentAwareFill(1);
            if (Op(op) == Op::Filter) w->applyEffectFilter(LiveEffect::make(EffectType::Invert));
            break;
        }
        }
        if (Tool(tool) == Tool::ContentAware || Op(op) == Op::CAFill) waitIdle();
        d->clearPixelSelection();
        drainEvents();
        const QImage after = d->render(0, true);
        double sx = 0, sy = 0;
        long n = 0;
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 300; ++x)
                if (before.pixel(x, y) != after.pixel(x, y)) { sx += x; sy += y; ++n; }
        QVERIFY2(n > 0, "the tool changed nothing");
        const QPointF c(sx / n, sy / n);
        QVERIFY2(QLineF(c, P).length() < 4.0, qPrintable(QStringLiteral("changed pixels centre on (%1, %2), pointer at (110, 110)").arg(c.x()).arg(c.y())));
        // the live Offset is still live (and unchanged) afterwards
        const VObject *o = d->find(id);
        QVERIFY(o);
        if (mode >= 0) {
            QCOMPARE(o->effects.size(), 1);
            QCOMPARE(o->effects[0].offsetX, 20.0);
            QCOMPARE(o->effects[0].offsetY, 10.0);
        }
        QVERIFY(!d->pixelEditOpen());
        // one undo step brings back the exact original
        d->undo();
        QCOMPARE(d->render(0, true), before);
        closeDoc();
    }

    // The round trip display geometry -> original geometry is lossless (hidden pixels survive).
    void roundTripIsLossless() {
        for (int mode = 0; mode <= 2; ++mode) {
            VObject o;
            o.type = ObjType::Bitmap;
            o.rect = QRectF(0, 0, 64, 48);
            o.image = noise(64, 48, 3);
            LiveEffect a = LiveEffect::make(EffectType::Offset), b = a;
            a.offsetX = 13; a.offsetY = -7; a.undefinedAreas = mode;
            b.offsetX = -40; b.offsetY = 30; b.undefinedAreas = (mode + 1) % 3;
            o.effects << a << b;
            const VObject orig = o;
            pxedit::Session s;
            QVERIFY(pxedit::enter(o, s));
            QCOMPARE(o.effects[0].offsetX, 0.0);
            // the display image is what the renderer shows for the original
            QImage shown = fx::applyEffects(orig.image, orig.effects, orig.image.rect());
            QCOMPARE(o.image, shown.convertToFormat(o.image.format()));
            pxedit::leave(o, s);
            QCOMPARE(o.image, orig.image.convertToFormat(o.image.format()));
            QCOMPARE(o.effects[0].offsetX, 13.0);
            QCOMPARE(o.effects[1].offsetY, 30.0);
        }
    }

    // A bitmap whose image resolution differs from its rect: the offset is in canvas pixels.
    void scaledBitmap() {
        newDoc(LiveEffect::WrapAround, QPointF(20, 10), QRectF(50, 50, 100, 100), QSize(200, 200));
        toolDefaults(Tool::Pencil);
        const QImage before = d->render(0, true);
        drag(QPointF(106, 110), QPointF(114, 110));
        const QImage after = d->render(0, true);
        double sx = 0, sy = 0;
        long n = 0;
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 300; ++x)
                if (before.pixel(x, y) != after.pixel(x, y)) { sx += x; sy += y; ++n; }
        QVERIFY(n > 0);
        QVERIFY(QLineF(QPointF(sx / n, sy / n), QPointF(110, 110)).length() < 4.0);
        closeDoc();
    }

    // Lens Correction / Make Seamless cannot be inverted: pixel tools refuse with a hint.
    void irreversibleEffectsRefuse() {
        for (EffectType t : {EffectType::LensCorrection, EffectType::MakeSeamless}) {
            newDoc(-1);
            d->find(id)->effects << LiveEffect::make(t);
            d->commit(QStringLiteral("fx"));
            QString msg;
            auto c = connect(&appState(), &AppState::statusMessage, this, [&](const QString &m) { if (!m.isEmpty()) msg = m; });
            toolDefaults(Tool::Brush);
            const QImage before = d->find(id)->image;
            drag(QPointF(100, 100), QPointF(110, 100));
            disconnect(c);
            QCOMPARE(d->find(id)->image, before);
            QVERIFY2(msg.contains(effectName(t)), qPrintable(msg));
            closeDoc();
        }
    }

    // Liquify edits the image as displayed (offset applied), and the result lands back in place.
    void liquifySeesDisplayedImage() {
        newDoc(LiveEffect::WrapAround);
        const VObject *o = d->find(id);
        const QImage shown = fx::applyEffects(o->image, o->effects, o->image.rect());
        QImage seen;
        QVERIFY(w->liquifyFilter([&](LiquifyDialog &dlg) {
            seen = dlg.liquified();
            dlg.strokeImage(QPointF(55, 55), QPointF(65, 60));
        }));
        QCOMPARE(seen.convertToFormat(QImage::Format_ARGB32_Premultiplied), shown.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        QCOMPARE(d->find(id)->effects[0].offsetX, 20.0);
        closeDoc();
    }

    // ---- trim to the pixels (Fireworks bitmaps never keep transparent margins)
    void eraseTrimsBounds() {
        newDoc(-1);
        toolDefaults(Tool::Eraser);
        appState().brushSize = 30;
        // erase the right-most 20 columns
        for (int y = 40; y <= 160; y += 10) drag(QPointF(140, y), QPointF(175, y));
        const VObject *o = d->find(id);
        const QRect ob = filters::opaqueBounds(o->image);
        QCOMPARE(ob, o->image.rect());
        QVERIFY2(o->rect.right() < 131 && o->rect.left() == 50, qPrintable(QStringLiteral("rect %1 %2").arg(o->rect.left()).arg(o->rect.right())));
        // the remaining pixels did not move on the canvas
        QCOMPARE(o->image.pixel(10, 10), noise(100, 100).pixel(10, 10));
        closeDoc();
    }
    // ---- Pencil "Left/Right colours" (pixel art)
    void bdrag(Qt::MouseButton b, QPointF a, QPointF z, Qt::KeyboardModifiers m = Qt::NoModifier) {
        QTest::mouseMove(cv, vp(a));
        QTest::mousePress(cv, b, m, vp(a));
        for (int k = 1; k <= 8; ++k) QTest::mouseMove(cv, vp(a + (z - a) * (k / 8.0)));
        QTest::mouseRelease(cv, b, m, vp(z));
        drainEvents();
    }
    QRgb docPx(QPointF p) const {   // the bitmap's own pixel under a document point (no offset)
        const VObject *o = d->find(id);
        const QPoint ip = (p - o->rect.topLeft()).toPoint();
        return o->image.rect().contains(ip) ? o->image.pixel(ip) : 0u;
    }
    void pencilLeftRightColours() {
        AppState &as = appState();
        const QVariantMap saved = as.optionValues();
        // left unset follows the stroke colour
        as.pencilLeft = QColor();
        as.stroke = QColor(10, 20, 30);
        QCOMPARE(as.pencilColor(false), QColor(10, 20, 30));
        newDoc(-1);
        toolDefaults(Tool::Pencil);
        as.brushSize = 3;
        as.antialias = false;
        as.pixelPerfect = false;
        as.preserveTransparency = false;
        as.autoErase = false;
        // mode off: the right button does not paint, the left one paints the stroke colour
        as.pencilTwoColor = false;
        const QImage orig = d->find(id)->image;
        bdrag(Qt::RightButton, QPointF(70, 70), QPointF(90, 70));
        QCOMPARE(d->find(id)->image, orig);
        bdrag(Qt::LeftButton, QPointF(70, 70), QPointF(90, 70));
        QCOMPARE(docPx(QPointF(80, 70)), qRgb(0, 255, 0));
        d->undo();
        drainEvents();
        // mode on: left = the stroke colour by default, right = transparent (erase to alpha 0)
        as.pencilTwoColor = true;
        as.pencilLeft = QColor();
        as.pencilRight = QColor(0, 0, 0, 0);
        int h0 = d->historyIndex();
        bdrag(Qt::LeftButton, QPointF(70, 80), QPointF(90, 80));
        QCOMPARE(docPx(QPointF(80, 80)), qRgb(0, 255, 0));
        QCOMPARE(d->historyIndex(), h0 + 1);   // one undo step per stroke
        bdrag(Qt::RightButton, QPointF(70, 90), QPointF(90, 90));
        QCOMPARE(d->historyIndex(), h0 + 2);
        QCOMPARE(qAlpha(docPx(QPointF(80, 90))), 0);
        QVERIFY(qAlpha(docPx(QPointF(80, 100))) == 255);
        // explicit colours
        as.pencilLeft = QColor(255, 0, 0);
        as.pencilRight = QColor(0, 0, 255);
        bdrag(Qt::LeftButton, QPointF(70, 110), QPointF(90, 110));
        QCOMPARE(docPx(QPointF(80, 110)), qRgb(255, 0, 0));
        bdrag(Qt::RightButton, QPointF(70, 120), QPointF(90, 120));
        QCOMPARE(docPx(QPointF(80, 120)), qRgb(0, 0, 255));
        // the other button during a stroke neither restarts nor ends it
        QTest::mouseMove(cv, vp(QPointF(100, 130)));
        QTest::mousePress(cv, Qt::RightButton, {}, vp(QPointF(100, 130)));
        QTest::mouseMove(cv, vp(QPointF(105, 130)));
        QTest::mousePress(cv, Qt::LeftButton, {}, vp(QPointF(105, 130)));
        QTest::mouseRelease(cv, Qt::LeftButton, {}, vp(QPointF(108, 130)));
        QTest::mouseMove(cv, vp(QPointF(115, 130)));
        QTest::mouseRelease(cv, Qt::RightButton, {}, vp(QPointF(115, 130)));
        drainEvents();
        QCOMPARE(docPx(QPointF(112, 130)), qRgb(0, 0, 255));
        QCOMPARE(d->historyIndex(), h0 + 5);
        // Shift-click: a straight line from the end of the last stroke, with either button
        bdrag(Qt::LeftButton, QPointF(60, 140), QPointF(60, 140));
        bdrag(Qt::RightButton, QPointF(120, 140), QPointF(120, 140), Qt::ShiftModifier);
        QCOMPARE(docPx(QPointF(90, 140)), qRgb(0, 0, 255));
        QCOMPARE(docPx(QPointF(60, 140)), qRgb(0, 0, 255));
        // the right button shows no context menu while the mode is on
        QSignalSpy menus(cv, &Canvas::contextMenuRequested);
        QContextMenuEvent ce(QContextMenuEvent::Mouse, vp(QPointF(100, 100)), cv->mapToGlobal(vp(QPointF(100, 100))));
        QCoreApplication::sendEvent(cv, &ce);
        QCOMPARE(menus.count(), 0);
        // Preserve Transparency: the erased row stays transparent
        as.preserveTransparency = true;
        bdrag(Qt::LeftButton, QPointF(70, 90), QPointF(90, 90));
        QCOMPARE(qAlpha(docPx(QPointF(80, 90))), 0);
        as.preserveTransparency = false;
        // pixel perfect, 1 px: a transparent colour erases the 1 px line
        as.brushSize = 1;
        as.pixelPerfect = true;
        as.pencilRight = QColor(0, 0, 0, 0);
        bdrag(Qt::RightButton, QPointF(70.5, 135.5), QPointF(110.5, 135.5));
        {   // exactly one row of 1 px is erased (view rounding may put it on 134 or 135)
            int erased = 0;
            for (int y = 132; y <= 137; ++y) erased += qAlpha(docPx(QPointF(90.5, y + 0.5))) == 0;
            QCOMPARE(erased, 1);
        }
        as.pixelPerfect = false;
        as.brushSize = 3;
        // erasing the right edge trims the bitmap in the same step
        as.brushSize = 30;
        for (int y = 40; y <= 160; y += 10) bdrag(Qt::RightButton, QPointF(140, y), QPointF(175, y));
        QVERIFY2(d->find(id)->rect.right() < 131, qPrintable(QString::number(d->find(id)->rect.right())));
        closeDoc();
        // live Offset: the right colour lands under the pointer
        newDoc(LiveEffect::WrapAround);
        toolDefaults(Tool::Pencil);
        as.brushSize = 3;
        as.pencilTwoColor = true;
        as.pencilRight = QColor(0, 0, 255);
        const QImage before = d->render(0, true);
        bdrag(Qt::RightButton, QPointF(106, 110), QPointF(114, 110));
        const QImage after = d->render(0, true);
        double sx = 0, sy = 0;
        long n = 0;
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 300; ++x)
                if (before.pixel(x, y) != after.pixel(x, y)) { sx += x; sy += y; ++n; QCOMPARE(after.pixel(x, y), qRgb(0, 0, 255)); }
        QVERIFY(n > 0);
        QVERIFY(QLineF(QPointF(sx / n, sy / n), QPointF(110, 110)).length() < 2.0);
        // the Property Inspector: the option, both swatches, and the Transparent choice in their pop-up
        drainEvents();
        auto *pi = w->findChild<PropertyInspector *>();
        QVERIFY(pi);
        QCheckBox *chk = nullptr;
        ColorButton *left = nullptr;
        for (int i = 0; i < 50 && !left; ++i) {
            drainEvents();
            chk = pi->findChild<QCheckBox *>(QStringLiteral("opt_pencilTwoColor"));
            left = pi->findChild<ColorButton *>(QStringLiteral("opt_pencilLeft"));
        }
        QVERIFY(chk && left && pi->findChild<ColorButton *>(QStringLiteral("opt_pencilRight")));
        ColorPopup *pop = left->showPopup();
        auto *tb = pop->findChild<QToolButton *>(QStringLiteral("colorPopupTransparent"));
        QVERIFY(tb);
        tb->click();
        drainEvents();
        QCOMPARE(as.pencilLeft.alpha(), 0);
        // the options persist through the saved option values
        const QVariantMap vals = as.optionValues();
        as.pencilTwoColor = false;
        as.pencilLeft = Qt::yellow;
        as.pencilRight = Qt::yellow;
        as.setOptionValues(vals);
        QVERIFY(as.pencilTwoColor);
        QCOMPARE(as.pencilLeft.alpha(), 0);
        QCOMPARE(as.pencilRight, QColor(0, 0, 255));
        closeDoc();
        as.setOptionValues(saved);
        as.pencilTwoColor = false;
        as.pencilLeft = QColor();
        as.pencilRight = QColor(0, 0, 0, 0);
    }
    void clearAllLeavesEmptyBitmap() {
        newDoc(-1);
        d->selectAll();   // object selection
        d->selectOnly(id);
        toolDefaults(Tool::Marquee);
        drag(QPointF(30, 30), QPointF(200, 200));
        d->selectOnly(id);
        const int steps = d->history().size();
        w->action(QStringLiteral("edit.clear"))->trigger();
        drainEvents();
        const VObject *o = d->find(id);
        QVERIFY(o);
        QCOMPARE(o->image.size(), QSize(1, 1));
        QVERIFY(pxedit::isTransparent(*o));
        QCOMPARE(d->history().size(), steps + 1);   // the trim is part of the Clear step
        d->undo();
        QCOMPARE(d->find(id)->image.size(), QSize(100, 100));
        QCOMPARE(d->find(id)->rect, QRectF(50, 50, 100, 100));
        // painting on the empty bitmap and Fit Canvas still work
        d->redo();
        w->action(QStringLiteral("modify.fit"))->trigger();
        closeDoc();
    }
    void cutTrimsAndFitCanvas() {
        newDoc(-1);
        toolDefaults(Tool::Marquee);
        drag(QPointF(100, 30), QPointF(200, 200));   // the right half
        d->selectOnly(id);
        w->action(QStringLiteral("edit.cut"))->trigger();
        drainEvents();
        const VObject *o = d->find(id);
        QCOMPARE(o->rect, QRectF(50, 50, 50, 100));
        d->clearPixelSelection();
        w->action(QStringLiteral("modify.fit"))->trigger();
        QCOMPARE(QSize(d->s.width, d->s.height), QSize(50, 100));
        closeDoc();
    }
    // with a live Offset, a trim keeps the look exactly (the offset is applied to the pixels)
    void trimWithOffsetKeepsLook() {
        newDoc(LiveEffect::WrapAround);
        toolDefaults(Tool::Marquee);
        drag(QPointF(30, 30), QPointF(80, 200));   // the left 30 displayed columns
        d->selectOnly(id);
        w->action(QStringLiteral("edit.clear"))->trigger();
        drainEvents();
        d->clearPixelSelection();
        const VObject *o = d->find(id);
        QCOMPARE(o->rect, QRectF(80, 50, 70, 100));
        const QImage now = d->render(0, true);
        d->undo();
        QImage expect = d->render(0, true);
        {
            QPainter p(&expect);
            p.fillRect(QRect(50, 50, 30, 100), Qt::white);
        }
        QCOMPARE(now.convertToFormat(QImage::Format_RGB32), expect.convertToFormat(QImage::Format_RGB32));
        closeDoc();
    }

    // ---- Shift = fine control on the pop-up sliders (1/3 speed, no jump, every step reachable)
    void sliderShiftFineControl() {
        for (Qt::Orientation orient : {Qt::Vertical, Qt::Horizontal}) {
            const Qt::Orientation was = PopupSliderHub::orientation();
            PopupSliderHub::setOrientation(orient);
            PopupSliderField f(0, 1000);
            f.setValue(300);
            f.show();
            drainEvents();
            f.openPopup(true);
            SliderPopup *pop = f.popup();
            QVERIFY(pop && pop->isDragging());
            // the drag is relative to the triangle's centre (where the press was)
            const QPointF a = f.arrow()->mapToGlobal(QPointF(f.arrow()->width() / 2.0, orient == Qt::Vertical ? f.arrow()->height() / 2.0 : f.arrow()->height()));
            auto at = [&](double t) { return orient == Qt::Vertical ? a - QPointF(0, t) : a + QPointF(t, 0); };
            f.setValue(150);
            pop->dragTo(at(-30), Qt::NoModifier);
            const int base = f.value();
            pop->dragTo(at(0), Qt::NoModifier);
            const int normal = f.value() - base;
            QVERIFY(normal > 10);
            // Shift pressed at the same spot: no jump
            pop->dragTo(at(0), Qt::ShiftModifier);
            QCOMPARE(f.value(), base + normal);
            pop->dragTo(at(30), Qt::ShiftModifier);
            const int fine = f.value() - (base + normal);
            QVERIFY2(std::abs(fine * 3 - normal) <= 3, qPrintable(QStringLiteral("normal %1 fine %2").arg(normal).arg(fine)));
            // Shift released: no jump, normal speed again
            const int v = f.value();
            pop->dragTo(at(30), Qt::NoModifier);
            QCOMPARE(f.value(), v);
            pop->dragTo(at(60), Qt::NoModifier);
            QVERIFY(std::abs((f.value() - v) - normal) <= 1);
            pop->finishDrag();
            // slow 1 px moves with Shift reach every integer (the value accumulates unrounded)
            PopupSliderField h(0, 100);
            h.setValue(20);
            h.show();
            h.openPopup(true);
            pop = h.popup();
            const QPointF b = h.arrow()->mapToGlobal(QPointF(h.arrow()->width() / 2.0, orient == Qt::Vertical ? h.arrow()->height() / 2.0 : h.arrow()->height()));
            auto at2 = [&](double t) { return orient == Qt::Vertical ? b - QPointF(0, t) : b + QPointF(t, 0); };
            pop->dragTo(at2(3), Qt::ShiftModifier);
            QSet<int> seen;
            int last = h.value();
            for (int t = 4; t <= 60; ++t) {
                pop->dragTo(at2(t), Qt::ShiftModifier);
                QVERIFY(h.value() - last <= 1 && h.value() >= last);
                last = h.value();
                seen.insert(last);
            }
            QVERIFY2(seen.size() >= 12, qPrintable(QString::number(seen.size())));
            pop->finishDrag();
            PopupSliderHub::setOrientation(was);
        }
        // float fields get the finer resolution too, and Shift + wheel takes a fine step (0.3.5: step x 33%, rounded to the unit)
        PopupSliderDoubleField g(0, 10, 2);
        g.setValue(5);
        g.show();
        drainEvents();
        g.openPopup(true);
        SliderPopup *pop = g.popup();
        const bool vert = pop->orientation() == Qt::Vertical;
        const QPointF a = g.arrow()->mapToGlobal(QPointF(g.arrow()->width() / 2.0, vert ? g.arrow()->height() / 2.0 : g.arrow()->height()));
        auto at = [&](double t) { return vert ? a - QPointF(0, t) : a + QPointF(t, 0); };
        pop->dragTo(at(3), Qt::ShiftModifier);
        double prev = g.value();
        for (int t = 4; t < 30; ++t) {
            pop->dragTo(at(t), Qt::ShiftModifier);
            QVERIFY(g.value() - prev <= 0.03 + 1e-9);
            prev = g.value();
        }
        QVERIFY(g.value() > 5.2);
        pop->finishDrag();
        g.setValue(5);
        QWheelEvent we(QPointF(5, 5), g.mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&g, &we);
        QCOMPARE(g.value(), 5.03);
    }

    // ---- (D) no light / dark fringe along the edges of a scaled bitmap, with every scaler
    static QImage testTexture(bool gradient, QSize sz = QSize(37, 29)) {
        QImage im(sz, QImage::Format_ARGB32);
        for (int y = 0; y < sz.height(); ++y)
            for (int x = 0; x < sz.width(); ++x)
                im.setPixel(x, y, gradient ? qRgb(40 + x * 150 / sz.width(), 200 - y * 150 / sz.height(), 90) : qRgb(200, 30, 30));
        return im;
    }
    // edge row / column vs the next one in must change no more than the next step in (+ tolerance)
    static QString fringe(const QImage &in, int tol) {
        const QImage im = in.convertToFormat(QImage::Format_ARGB32);
        const int W = im.width(), H = im.height();
        auto lum = [&](int x, int y) { const QRgb c = im.pixel(x, y); return qRed(c) + qGreen(c) + qBlue(c); };
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
            if (qAlpha(im.pixel(x, y)) != 255) return QStringLiteral("alpha %1 at %2,%3").arg(qAlpha(im.pixel(x, y))).arg(x).arg(y);
        if (W < 4 || H < 4) return {};
        for (int y = 1; y < H - 1; ++y) {
            const int l = qAbs(lum(0, y) - lum(1, y)) - qAbs(lum(1, y) - lum(2, y));
            const int r = qAbs(lum(W - 1, y) - lum(W - 2, y)) - qAbs(lum(W - 2, y) - lum(W - 3, y));
            if (l > tol) return QStringLiteral("left edge row %1 off by %2").arg(y).arg(l);
            if (r > tol) return QStringLiteral("right edge row %1 off by %2").arg(y).arg(r);
        }
        for (int x = 1; x < W - 1; ++x) {
            const int t = qAbs(lum(x, 0) - lum(x, 1)) - qAbs(lum(x, 1) - lum(x, 2));
            const int b = qAbs(lum(x, H - 1) - lum(x, H - 2)) - qAbs(lum(x, H - 2) - lum(x, H - 3));
            if (t > tol) return QStringLiteral("top edge col %1 off by %2").arg(x).arg(t);
            if (b > tol) return QStringLiteral("bottom edge col %1 off by %2").arg(x).arg(b);
        }
        return {};
    }

    void scalersLeaveNoEdgeFringe() {
        for (int r = 0; r < pixelscale::kResampleCount; ++r)
            for (bool grad : {false, true})
                for (QSize to : {QSize(101, 83), QSize(17, 13), QSize(74, 58), QSize(60, 20), QSize(111, 87)}) {
                    const QImage src = testTexture(grad);
                    const QImage out = pixelscale::resample(src, to, r);
                    QCOMPARE(out.size(), to);
                    const QString f = fringe(out, grad ? 12 : 0);
                    QVERIFY2(f.isEmpty(), qPrintable(QStringLiteral("%1 %2 -> %3x%4: %5").arg(pixelscale::resampleKey(r))
                                                     .arg(grad ? "gradient" : "solid").arg(to.width()).arg(to.height()).arg(f)));
                    if (!grad) {   // a solid texture stays exactly solid
                        const QImage o = out.convertToFormat(QImage::Format_ARGB32);
                        for (int y = 0; y < o.height(); ++y) for (int x = 0; x < o.width(); ++x)
                            QCOMPARE(o.pixel(x, y), qRgb(200, 30, 30));
                    }
                }
        // integer pixel-art upscales (Modify > Upscale) too
        for (int m = 0; m < pixelscale::kMethodCount; ++m)
            for (int fct : {2, 3}) {
                const QString f = fringe(pixelscale::upscale(testTexture(false), pixelscale::Method(m), fct), 0);
                QVERIFY2(f.isEmpty(), qPrintable(pixelscale::key(pixelscale::Method(m)) + ": " + f));
            }
    }

    // The reported bug: Scale from the left handle leaves the left edge between pixels; the smooth
    // scalers drew that column with partial coverage (a light strip), Nearest did not.
    void scaledBitmapEdgesInDocument() {
        const int saved = coreprefs::interpolation;
        for (int r = 0; r < pixelscale::kResampleCount; ++r) {
            coreprefs::interpolation = r;
            for (bool grad : {false, true})
                for (double sx : {0.347, 1.733, 3.21}) for (bool viaXform : {true, false}) {
                    std::unique_ptr<Document> d(Document::createNew(400, 300, Qt::white));
                    VObject o; o.type = ObjType::Bitmap; o.image = testTexture(grad, QSize(50, 40)); o.rect = QRectF(250, 100, 50, 40); o.id = d->newId();
                    const QTransform t = QTransform::fromTranslate(300, 140).scale(sx, sx).translate(-300, -140);   // top-left handle
                    if (viaXform) xf::transformObject(o, t);
                    else o.rect = t.mapRect(o.rect);
                    d->addObject(o);
                    const QRectF b = o.bounds();
                    const QRect inside(QPoint(qRound(b.left()), qRound(b.top())), QPoint(qRound(b.right()) - 1, qRound(b.bottom()) - 1));
                    const QImage shot = d->render(0, true).copy(inside);
                    const QString f = fringe(shot, grad ? 12 : 0);
                    QVERIFY2(f.isEmpty(), qPrintable(QStringLiteral("%1 %2 x%3 %4: %5").arg(pixelscale::resampleKey(r)).arg(grad ? "gradient" : "solid")
                                                     .arg(sx).arg(viaXform ? "xform" : "rect").arg(f)));
                    // and baking the transform keeps the same pixels and an opaque, whole-pixel bitmap
                    if (viaXform) {
                        VObject baked = *d->find(o.id);
                        QVERIFY(xf::bakeBitmap(baked));
                        QCOMPARE(baked.rect, QRectF(inside));
                        QVERIFY2(fringe(baked.image, grad ? 12 : 0).isEmpty(), "baked");
                    }
                }
        }
        coreprefs::interpolation = saved;
    }

    void imageSizeKeepsEdgesClean() {
        for (int r : {0, 1, 2, 3, 4, 11}) {
            std::unique_ptr<Document> d(Document::createNew(97, 71, Qt::white));
            VObject o; o.type = ObjType::Bitmap; o.image = testTexture(true, QSize(41, 33)); o.rect = QRectF(13, 9, 41, 33); o.id = d->newId();
            d->addObject(o);
            d->scaleImage(173, 131, r);
            const VObject &s = *d->find(o.id);
            const QRectF b = s.bounds();
            QCOMPARE(b, QRectF(b.toRect()));   // whole pixels
            QVERIFY2(fringe(d->render(0, true).copy(b.toRect()), 12).isEmpty(), qPrintable(pixelscale::resampleKey(r)));
        }
    }

private:
    // ---- (E) in-place renaming in the Layers / Frames panels
    struct RenameDoc { quint64 leaf = 0, inner = 0, outer = 0, masked = 0, mask = 0, other = 0; int webLayer = -1; };
    RenameDoc renameDoc() {
        RenameDoc r;
        appState().setTool(Tool::Pointer);
        w->activateWindow();
        (void)QTest::qWaitForWindowActive(w.get());
        d = Document::createNew(320, 240, Qt::white);
        w->addDocument(d);
        drainEvents();
        if (auto *cp = w->panel(QStringLiteral("Layers"))) { cp->show(); cp->setCollapsed(false); }
        if (auto *cp = w->panel(QStringLiteral("Frames"))) { cp->show(); cp->setCollapsed(false); }
        VObject a = rshape(d, QRectF(10, 10, 40, 40), QStringLiteral("Leaf")); d->addObject(a, 0, false); r.leaf = a.id;
        VObject b = rshape(d, QRectF(60, 10, 40, 40), QStringLiteral("Beside")); d->addObject(b, 0, false);
        d->setSelection({a.id, b.id}); r.inner = d->groupSelection(); d->find(r.inner)->name = QStringLiteral("Inner");
        VObject c = rshape(d, QRectF(110, 10, 40, 40), QStringLiteral("Third")); d->addObject(c, 0, false);
        d->setSelection({r.inner, c.id}); r.outer = d->groupSelection(); d->find(r.outer)->name = QStringLiteral("Outer");
        VObject m = rshape(d, QRectF(0, 0, 200, 200), QString()); m.type = ObjType::Ellipse;
        d->setSelection({r.outer}); r.masked = d->pasteAsMask(m);
        r.mask = d->find(r.masked)->maskObj.first().id;
        VObject o = rshape(d, QRectF(200, 150, 50, 40), QStringLiteral("Other"));
        const int l2 = d->addLayer(QStringLiteral("Second"));
        d->addObject(o, l2, false); r.other = o.id;
        r.webLayer = d->ensureWebLayer();
        d->clearSelection();
        d->commit(QStringLiteral("setup"));
        d->markClean();
        drainEvents();
        tree()->expandAll();
        drainEvents();
        return r;
    }
    QTreeWidget *tree() const { return w->findChild<LayersPanel *>()->findChild<QTreeWidget *>(); }
    QListWidget *framesList() const { return w->findChild<FramesPanel *>()->findChild<QListWidget *>(); }
    QPoint nameClickPoint(QTreeWidget *t, QTreeWidgetItem *it) {
        t->scrollToItem(it);
        drainEvents();
        const QRect r = t->visualItemRect(it);
        const QRect c = t->visualRect(t->indexFromItem(it, 2));
        return QPoint(c.left() + 12, r.center().y());
    }
    QLineEdit *dblClickName(QTreeWidget *t, QTreeWidgetItem *it) {
        const QPoint p = nameClickPoint(t, it);
        QTest::mouseClick(t->viewport(), Qt::LeftButton, {}, p);
        drainEvents();
        QTest::mouseDClick(t->viewport(), Qt::LeftButton, {}, p);
        drainEvents();
        return inlineEditor(t->viewport());
    }
    void typeAndEnter(QLineEdit *e, const QString &text) {
        e->selectAll();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
        drainEvents();
    }
    void closeRenameDoc() { if (d) d->markClean(); w->action(QStringLiteral("file.close"))->trigger(); drainEvents(); d = nullptr; }

private slots:
    void renameByDoubleClick() {
        RenameDoc r = renameDoc();
        QTreeWidget *t = tree();
        const int histBefore = d->historyIndex();
        // a layer row
        QLineEdit *e = dblClickName(t, layerRow(t, 0));
        QVERIFY(e);
        QCOMPARE(e->text(), d->s.layers[0].name);
        QCOMPARE(e->selectedText(), e->text());
        typeAndEnter(e, QStringLiteral("Artwork"));
        QCOMPARE(d->s.layers[0].name, QStringLiteral("Artwork"));
        QCOMPARE(d->historyIndex(), histBefore + 1);
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Rename Layer"));
        QVERIFY(d->isDirty());
        QCOMPARE(layerRow(t, 0)->text(2), QStringLiteral("Artwork"));
        // an object deep in the nested group, keeping the expanded state and the selection
        d->setSelection({r.other});
        drainEvents();
        QTreeWidgetItem *leaf = rowFor(t, r.leaf, 1);
        QVERIFY(leaf && leaf->parent() && leaf->parent()->parent());
        // selecting the row would change the selection: double-click straight on it
        QTest::mouseDClick(t->viewport(), Qt::LeftButton, {}, nameClickPoint(t, leaf));
        drainEvents();
        e = inlineEditor(t->viewport());
        QVERIFY(e);
        QCOMPARE(e->text(), QStringLiteral("Leaf"));
        typeAndEnter(e, QStringLiteral("  Deep leaf  "));
        QCOMPARE(d->find(r.leaf)->name, QStringLiteral("Deep leaf"));   // trimmed
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Rename Object"));
        QVERIFY(rowFor(t, r.leaf, 1));
        QVERIFY(rowFor(t, r.leaf, 1)->parent()->isExpanded() && rowFor(t, r.inner, 1)->parent()->isExpanded());
        // a group row: the editor starts from the bare name, not "Inner (2)"
        QTreeWidgetItem *g = rowFor(t, r.inner, 1);
        QVERIFY(g->text(2) != QStringLiteral("Inner"));
        e = dblClickName(t, g);
        QVERIFY(e);
        QCOMPARE(e->text(), QStringLiteral("Inner"));
        typeAndEnter(e, QStringLiteral("Inner group"));
        QCOMPARE(d->find(r.inner)->name, QStringLiteral("Inner group"));
        QVERIFY(rowFor(t, r.inner, 1)->text(2).startsWith(QStringLiteral("Inner group")));
        // undo takes back exactly one rename
        d->undo();
        drainEvents();
        QCOMPARE(d->find(r.inner)->name, QStringLiteral("Inner"));
        QCOMPARE(d->find(r.leaf)->name, QStringLiteral("Deep leaf"));
        // the mask row
        QTreeWidgetItem *mr = rowFor(t, r.mask, 2);
        QVERIFY(mr);
        e = dblClickName(t, mr);
        QVERIFY(e);
        typeAndEnter(e, QStringLiteral("Vignette"));
        QCOMPARE(d->find(r.masked)->maskObj.first().name, QStringLiteral("Vignette"));
        QVERIFY(rowFor(t, r.mask, 2)->text(2).startsWith(QStringLiteral("Vignette")));
        QVERIFY(rowFor(t, r.mask, 2)->text(2).contains(QStringLiteral("Mask")));
        closeRenameDoc();
    }

    void renameEscEmptyAndWebLayer() {
        RenameDoc r = renameDoc();
        QTreeWidget *t = tree();
        const int hist = d->historyIndex();
        // Esc cancels
        QLineEdit *e = dblClickName(t, rowFor(t, r.other, 1));
        QVERIFY(e);
        QTest::keyClicks(e, QStringLiteral("Nope"));
        QTest::keyClick(e, Qt::Key_Escape);
        drainEvents();
        QCOMPARE(d->find(r.other)->name, QStringLiteral("Other"));
        QCOMPARE(d->historyIndex(), hist);
        QVERIFY(!inlineEditor(t->viewport()));
        // empty / whitespace-only names are ignored
        for (const QString &blank : {QString(), QStringLiteral("   ")}) {
            e = dblClickName(t, rowFor(t, r.other, 1));
            QVERIFY(e);
            e->clear();
            if (!blank.isEmpty()) QTest::keyClicks(e, blank);
            QTest::keyClick(e, Qt::Key_Return);
            drainEvents();
            QCOMPARE(d->find(r.other)->name, QStringLiteral("Other"));
            QCOMPARE(d->historyIndex(), hist);
            QCOMPARE(rowFor(t, r.other, 1)->text(2), QStringLiteral("Other"));
        }
        // unchanged text adds no step
        e = dblClickName(t, rowFor(t, r.other, 1));
        QTest::keyClick(e, Qt::Key_Return);
        drainEvents();
        QCOMPARE(d->historyIndex(), hist);
        QVERIFY(!d->isDirty());
        // the Web Layer keeps its name
        QTreeWidgetItem *web = layerRow(t, r.webLayer);
        QVERIFY(web);
        QVERIFY(!(web->flags() & Qt::ItemIsEditable));
        QVERIFY(!dblClickName(t, web));
        const QString webName = d->s.layers[r.webLayer].name;
        QVERIFY(!w->findChild<LayersPanel *>()->startRename(layerRow(t, r.webLayer)));
        QCOMPARE(d->s.layers[r.webLayer].name, webName);
        // clicking the eye or lock column never opens the editor
        const QRect eye = t->visualRect(t->indexFromItem(rowFor(t, r.other, 1), 0));
        QTest::mouseDClick(t->viewport(), Qt::LeftButton, {}, eye.center());
        drainEvents();
        QVERIFY(!inlineEditor(t->viewport()));
        while (d->historyIndex() > hist) d->undo();   // the eye toggles
        drainEvents();
        // clicking away commits
        e = dblClickName(t, rowFor(t, r.other, 1));
        QVERIFY(e);
        e->selectAll();
        QTest::keyClicks(e, QStringLiteral("Clicked away"));
        e->setFocus();
        drainEvents();
        QVERIFY(e->hasFocus());
        // click on another row: the editor loses focus and commits
        QTest::mouseClick(t->viewport(), Qt::LeftButton, {}, nameClickPoint(t, layerRow(t, 0)));
        drainEvents();
        QCOMPARE(d->find(r.other)->name, QStringLiteral("Clicked away"));
        closeRenameDoc();
    }

    void renameByF2MenuAndContextMenu() {
        RenameDoc r = renameDoc();
        QTreeWidget *t = tree();
        auto *lp = w->findChild<LayersPanel *>();
        // F2 on the focused tree renames the current row (not the window's F2 Layers-panel toggle)
        t->setFocus();
        t->setCurrentItem(rowFor(t, r.outer, 1), 2, QItemSelectionModel::NoUpdate);
        QTest::keyClick(t, Qt::Key_F2);
        drainEvents();
        QLineEdit *e = inlineEditor(t->viewport());
        QVERIFY(e);
        QVERIFY(lp->isVisible() && !w->panel(QStringLiteral("Layers"))->isCollapsed());
        QCOMPARE(e->text(), QStringLiteral("Outer"));
        typeAndEnter(e, QStringLiteral("Outer F2"));
        QCOMPARE(d->find(r.outer)->name, QStringLiteral("Outer F2"));
        // options menu > Rename
        auto *opts = lp->findChild<QToolButton *>(QStringLiteral("layersOptions"));
        QVERIFY(opts);
        auto *menu = lp->findChild<QMenu *>(QStringLiteral("layersOptionsMenu"));
        QVERIFY(menu);
        QAction *ren = menu->findChild<QAction *>(QStringLiteral("layers.rename"));
        if (!ren) for (QAction *a : menu->actions()) if (a->objectName() == QLatin1String("layers.rename")) ren = a;
        QVERIFY(ren);
        t->setCurrentItem(rowFor(t, r.other, 1), 2, QItemSelectionModel::NoUpdate);
        ren->trigger();
        drainEvents();
        e = inlineEditor(t->viewport());
        QVERIFY(e);
        typeAndEnter(e, QStringLiteral("From menu"));
        QCOMPARE(d->find(r.other)->name, QStringLiteral("From menu"));
        // right-click > Rename: the context menu opens on the clicked row
        const QPoint p = nameClickPoint(t, rowFor(t, r.leaf, 1));
        QMetaObject::invokeMethod(t, "customContextMenuRequested", Q_ARG(QPoint, p));
        drainEvents();
        QVERIFY(menu->isVisible());
        QCOMPARE(t->currentItem(), rowFor(t, r.leaf, 1));
        menu->hide();
        ren->trigger();
        drainEvents();
        e = inlineEditor(t->viewport());
        QVERIFY(e);
        typeAndEnter(e, QStringLiteral("From context"));
        QCOMPARE(d->find(r.leaf)->name, QStringLiteral("From context"));
        closeRenameDoc();
    }

    void renameSurvivesRebuild() {
        RenameDoc r = renameDoc();
        QTreeWidget *t = tree();
        QLineEdit *e = dblClickName(t, rowFor(t, r.other, 1));
        QVERIFY(e);
        QTest::keyClicks(e, QStringLiteral("Typed"));
        // the document changes under the editor (another panel, a timer...): the tree must not rebuild now
        d->find(r.leaf)->opacity = 50;
        d->commit(QStringLiteral("Opacity"));
        drainEvents();
        QTest::qWait(50);
        drainEvents();
        QCOMPARE(inlineEditor(t->viewport()), e);
        QCOMPARE(e->text(), QStringLiteral("Typed"));
        QTest::keyClick(e, Qt::Key_Return);
        drainEvents();
        QCOMPARE(d->find(r.other)->name, QStringLiteral("Typed"));
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Rename Object"));
        QCOMPARE(d->history()[d->historyIndex() - 1].label, QStringLiteral("Opacity"));
        QCOMPARE(rowFor(t, r.other, 1)->text(2), QStringLiteral("Typed"));
        closeRenameDoc();
    }

    void renameInPropertyInspectorAndFrames() {
        RenameDoc r = renameDoc();
        // Property Inspector name field
        d->setSelection({r.other});
        drainEvents();
        auto *pi = w->findChild<PropertyInspector *>();
        QLineEdit *name = nullptr;
        for (QLineEdit *le : pi->findChildren<QLineEdit *>()) if (le->text() == QLatin1String("Other") && le->isVisible()) name = le;
        QVERIFY(name);
        const int hist = d->historyIndex();
        name->setFocus();
        name->selectAll();
        QTest::keyClicks(name, QStringLiteral(" PI name "));
        QTest::keyClick(name, Qt::Key_Return);
        drainEvents();
        QCOMPARE(d->find(r.other)->name, QStringLiteral("PI name"));
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Rename Object"));
        QCOMPARE(rowFor(tree(), r.other, 1)->text(2), QStringLiteral("PI name"));
        // empty is ignored and the field shows the name again
        name->selectAll();
        QTest::keyClick(name, Qt::Key_Delete);
        QTest::keyClick(name, Qt::Key_Return);
        drainEvents();
        QCOMPARE(d->find(r.other)->name, QStringLiteral("PI name"));
        QCOMPARE(d->historyIndex(), hist + 1);
        QCOMPARE(name->text(), QStringLiteral("PI name"));
        // Frames panel: double-click a frame
        d->addFrame(false);
        d->commit(QStringLiteral("Add Frame"));
        drainEvents();
        QListWidget *fl = framesList();
        QVERIFY(fl && fl->count() == 2);
        const QRect fr = fl->visualItemRect(fl->item(1));
        QTest::mouseDClick(fl->viewport(), Qt::LeftButton, {}, fr.center());
        drainEvents();
        QLineEdit *e = inlineEditor(fl->viewport());
        QVERIFY(e);
        QCOMPARE(e->text(), QStringLiteral("Frame 2"));
        typeAndEnter(e, QStringLiteral("Hover"));
        QCOMPARE(d->s.frames[1].name, QStringLiteral("Hover"));
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Rename Frame"));
        QVERIFY(fl->item(1)->text().startsWith(QStringLiteral("Hover")));
        // F2 on the list, Esc cancels
        fl->setFocus();
        fl->setCurrentRow(0);
        const int fh = d->historyIndex();
        QTest::keyClick(fl, Qt::Key_F2);
        drainEvents();
        e = inlineEditor(fl->viewport());
        QVERIFY(e);
        QTest::keyClicks(e, QStringLiteral("x"));
        QTest::keyClick(e, Qt::Key_Escape);
        drainEvents();
        QCOMPARE(d->s.frames[0].name, QStringLiteral("Frame 1"));
        QCOMPARE(d->historyIndex(), fh);
        QVERIFY(!inlineEditor(fl->viewport()));
        closeRenameDoc();
    }

    void renamedNamesRoundTrip() {
        std::unique_ptr<Document> doc(Document::createNew(64, 48, Qt::white));
        doc->s.layers[0].name = QStringLiteral("Renamed layer é");
        const int l2 = doc->addLayer(QStringLiteral("Top renamed"));
        VObject o = rshape(doc.get(), QRectF(4, 4, 20, 20), QStringLiteral("Renamed object"));
        doc->addObject(o, 0, false);
        doc->addObject(rshape(doc.get(), QRectF(30, 10, 20, 20), QStringLiteral("x")), l2, false);
        doc->s.frames[0].name = QStringLiteral("Renamed frame");
        // .lwd keeps every name
        QString err;
        std::unique_ptr<Document> back(lwd::deserialize(lwd::serialize(*doc), &err));
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->s.layers[0].name, QStringLiteral("Renamed layer é"));
        QCOMPARE(back->s.layers[l2].name, QStringLiteral("Top renamed"));
        QCOMPARE(back->find(o.id)->name, QStringLiteral("Renamed object"));
        QCOMPARE(back->s.frames[0].name, QStringLiteral("Renamed frame"));
        // layered formats keep the layer names
        for (Fmt f : {Fmt::PSD, Fmt::ORA, Fmt::XCF}) {
            ExportOptions eo; eo.format = f;
            QByteArray data;
            QVERIFY2(docio::exportDocument(*doc, 0, eo, data, &err), qPrintable(err));
            std::unique_ptr<Document> b(docio::openData(data, Fmt::Unknown, QString(), &err));
            QVERIFY2(b, qPrintable(err));
            QStringList names;
            for (const LayerInfo &l : b->s.layers) names << l.name;
            QVERIFY2(names.contains(QStringLiteral("Renamed layer é")) && names.contains(QStringLiteral("Top renamed")),
                     qPrintable(ImageIO::formatName(f) + ": " + names.join('/')));
        }
    }
private:
    // ---- (F) gradient handles: every visible handle of every gradient kind changes the render
    static QVector<FillKind> gradKinds() {
        return {FillKind::Linear, FillKind::Radial, FillKind::Cone, FillKind::Contour, FillKind::Satin, FillKind::Starburst,
                FillKind::Bars, FillKind::Ripples, FillKind::Waves, FillKind::Ellipse, FillKind::Rectangle};
    }
    quint64 gradDoc(FillKind k, ObjType type = ObjType::Rect) {
        d = Document::createNew(300, 240, Qt::white);
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        drainEvents();
        VObject o; o.type = type; o.rect = QRectF(50, 50, 180, 120); o.id = d->newId();
        o.gradient = {{0, QColor(10, 10, 60)}, {0.45, QColor(240, 200, 40)}, {1, QColor(255, 255, 255)}};
        fills::setFillKind(o, k);
        d->addObject(o, 0, false);
        d->selectOnly(o.id);
        d->commit(QStringLiteral("add"));
        drainEvents();
        return o.id;
    }
    static int diffCount(const QImage &a, const QImage &b) {
        int n = 0;
        for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x) if (a.pixel(x, y) != b.pixel(x, y)) ++n;
        return n;
    }
private slots:
    void gradientHandlesAllChangeRender_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("tool");
        const QPair<Tool, const char *> tools[] = {{Tool::Pointer, "pointer"}, {Tool::Subselect, "subselect"},
                                                   {Tool::Gradient, "gradient"}, {Tool::PaintBucket, "bucket"}};
        for (FillKind k : gradKinds())
            for (const auto &t : tools)
                QTest::newRow(qPrintable(fills::fillKindLabels().value(int(k)) + QLatin1Char('/') + QLatin1String(t.second))) << int(k) << int(t.first);
    }
    void gradientHandlesAllChangeRender() {
        QFETCH(int, kind);
        QFETCH(int, tool);
        const FillKind k = FillKind(kind);
        const QVector<int> handles = fills::usesSecondAxis(k) ? QVector<int>{0, 1, 2} : QVector<int>{0, 1};
        for (int h : handles) {
            const quint64 oid = gradDoc(k);
            appState().setTool(Tool(tool));
            drainEvents();
            const QImage before = render::renderFrame(d->s, 0);
            VObject *o = d->find(oid);
            const QPointF hp[3] = {fills::handleToDoc(*o, o->gradP0), fills::handleToDoc(*o, o->gradP1), fills::handleToDoc(*o, o->gradP2)};
            // start: move the centre; end: turn and stretch; second axis: skew
            const QPointF to = h == 0 ? hp[0] + QPointF(-30, 18) : h == 1 ? hp[1] + QPointF(-25, 30) : hp[2] + QPointF(35, -10);
            const int hist = d->historyIndex();
            drag(hp[h], to);
            o = d->find(oid);
            const QPointF now = fills::handleToDoc(*o, h == 0 ? o->gradP0 : h == 1 ? o->gradP1 : o->gradP2);
            QVERIFY2(QLineF(now, to).length() < 1.5, qPrintable(QStringLiteral("handle %1 did not follow the drag").arg(h)));
            QVERIFY2(d->historyIndex() == hist + 1, "one undo step per handle drag");
            const QImage after = render::renderFrame(d->s, 0);
            const int changed = diffCount(before, after);
            QVERIFY2(changed > 200, qPrintable(QStringLiteral("%1 handle %2 changed only %3 pixels").arg(fills::fillKindLabels().value(int(k))).arg(h).arg(changed)));
            d->undo();
            drainEvents();
            QCOMPARE(diffCount(before, render::renderFrame(d->s, 0)), 0);
            closeDoc();
        }
        appState().setTool(Tool::Pointer);
    }
    // Contour, Fireworks MX 2004 style: start = peak, end length = spread, axes = ellipse / skew / turn
    void contourHandlesShapeTheSpread() {
        auto grey = [](const VObject &o) {
            QRect area;
            QImage im = render::renderObject(o, &area, QRect(0, 0, 400, 400));
            return std::make_pair(im.convertToFormat(QImage::Format_ARGB32), area);
        };
        auto lum = [](const QImage &im, const QRect &a, QPointF p) { return qGray(im.pixel((p - QPointF(a.topLeft())).toPoint())); };
        VObject o; o.type = ObjType::Rect; o.rect = QRectF(20, 20, 200, 120); o.id = 1;
        o.gradient = {{0, Qt::black}, {1, Qt::white}};
        fills::setFillKind(o, FillKind::Contour);
        QVERIFY(fills::contourHandlesDefault(o));
        const auto [base, a0] = grey(o);
        // the peak follows the start handle
        VObject m = o;
        const QPointF peak(70, 60);
        const QPointF delta = peak - fills::handleToDoc(o, o.gradP0);
        m.gradP0 = fills::docToHandle(o, fills::handleToDoc(o, o.gradP0) + delta);
        m.gradP1 = fills::docToHandle(o, fills::handleToDoc(o, o.gradP1) + delta);
        m.gradP2 = fills::docToHandle(o, fills::handleToDoc(o, o.gradP2) + delta);
        {
            const auto [im, a] = grey(m);
            QVERIFY2(lum(im, a, peak) > 235, qPrintable(QString::number(lum(im, a, peak))));
            QVERIFY(lum(im, a, peak) > lum(im, a, QPointF(170, 100)) + 30);
            QVERIFY(lum(base, a0, QPointF(70, 60)) + 30 < lum(im, a, peak));
        }
        // a longer end handle widens the spread (brighter away from the peak), a shorter one narrows it
        VObject wide = o, narrow = o;
        const QPointF c = fills::handleToDoc(o, o.gradP0);
        wide.gradP1 = fills::docToHandle(o, c + (fills::handleToDoc(o, o.gradP1) - c) * 2);
        wide.gradP2 = fills::docToHandle(o, c + (fills::handleToDoc(o, o.gradP2) - c) * 2);
        narrow.gradP1 = fills::docToHandle(o, c + (fills::handleToDoc(o, o.gradP1) - c) * 0.5);
        narrow.gradP2 = fills::docToHandle(o, c + (fills::handleToDoc(o, o.gradP2) - c) * 0.5);
        const QPointF probe(60, 50);
        const auto [wi, wa] = grey(wide);
        const auto [ni, na] = grey(narrow);
        QVERIFY2(lum(wi, wa, probe) > lum(base, a0, probe) + 10 && lum(base, a0, probe) > lum(ni, na, probe) + 10,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(lum(wi, wa, probe)).arg(lum(base, a0, probe)).arg(lum(ni, na, probe))));
        // only the X axis longer: brighter along X than along Y at the same distance from the centre
        // (on a square, where the plain look is symmetric)
        VObject sq = o; sq.rect = QRectF(20, 20, 140, 140);
        const QPointF sc = fills::handleToDoc(sq, sq.gradP0);
        const auto [sb, sba] = grey(sq);
        VObject ex = sq;
        ex.gradP1 = fills::docToHandle(sq, sc + (fills::handleToDoc(sq, sq.gradP1) - sc) * 2.5);
        const auto [ei, ea] = grey(ex);
        const int gx = lum(ei, ea, sc + QPointF(40, 0)) - lum(sb, sba, sc + QPointF(40, 0));
        const int gy = lum(ei, ea, sc + QPointF(0, 40)) - lum(sb, sba, sc + QPointF(0, 40));
        QVERIFY2(gx > gy + 5, qPrintable(QStringLiteral("%1 %2").arg(gx).arg(gy)));
        // turning the stretched axes by 90 degrees turns the ellipse
        VObject turned = sq;
        const double r = (fills::handleToDoc(sq, sq.gradP1) - sc).x();
        turned.gradP1 = fills::docToHandle(sq, sc + QPointF(0, -r * 2.5));
        turned.gradP2 = fills::docToHandle(sq, sc + QPointF(-r, 0));
        const auto [ti, ta] = grey(turned);
        QVERIFY(diffCount(ti, ei) > 500);
        QVERIFY(lum(ti, ta, sc + QPointF(0, 40)) - lum(sb, sba, sc + QPointF(0, 40)) > lum(ti, ta, sc + QPointF(40, 0)) - lum(sb, sba, sc + QPointF(40, 0)) + 5);
        // skewed axes skew the spread
        VObject sk = o;
        sk.gradP2 = fills::docToHandle(o, fills::handleToDoc(o, o.gradP2) + QPointF(50, 0));
        QVERIFY(diffCount(grey(sk).first, base) > 500);
        // moving the handles back gives exactly the default look again
        VObject back = sk; fills::resetContourHandles(back);
        QCOMPARE(diffCount(grey(back).first, base), 0);
    }
    // the default handles keep the plain distance look (t = D / Dmax), in old files too
    void contourDefaultLookUnchanged() {
        VObject o; o.type = ObjType::Ellipse; o.rect = QRectF(10, 10, 150, 90); o.id = 1;
        o.gradient = {{0, Qt::black}, {1, Qt::white}};
        fills::setFillKind(o, FillKind::Linear);
        o.gradP0 = {0.1, 0.2}; o.gradP1 = {0.9, 0.7};
        VObject c = o;
        fills::setFillKind(c, FillKind::Contour);   // switching into Contour starts from its defaults
        QVERIFY(fills::contourHandlesDefault(c));
        // a version 3 (0.3.3) file with stray handles on a Contour fill still loads with the plain look
        std::unique_ptr<Document> doc(Document::createNew(200, 120, Qt::white));
        VObject stray = c; stray.gradP0 = {0.2, 0.3}; stray.gradP1 = {0.6, 0.9}; stray.gradP2 = {0.3, 0.1}; stray.id = doc->newId();
        doc->addObject(stray, 0, false);
        QMap<QString, QByteArray> files;
        QVERIFY(zip::read(lwd::serialize(*doc), files));
        QJsonObject root = QJsonDocument::fromJson(files.value(QStringLiteral("document.json"))).object();
        QCOMPARE(root[QStringLiteral("version")].toInt(), 7); // 0.3.20
        for (int v : {3, 4}) {
            root[QStringLiteral("version")] = v;
            QVector<zip::Entry> entries;
            for (auto it = files.cbegin(); it != files.cend(); ++it)
                entries.append({it.key(), it.key() == QLatin1String("document.json") ? QJsonDocument(root).toJson() : it.value()});
            QString err;
            std::unique_ptr<Document> r(lwd::deserialize(zip::write(entries), &err));
            QVERIFY2(r, qPrintable(err));
            const VObject &lo = r->objects(0).first();
            QCOMPARE(fills::contourHandlesDefault(lo), v == 3);   // version 4 keeps the dragged handles
        }
        // an untouched Contour keeps its default handles through a rotation
        VObject rot = c;
        rot.type = ObjType::Path; rot.closed = true;
        rot.points = QPolygonF({{10, 10}, {160, 10}, {160, 100}, {10, 100}});
        rot.hIn = rot.hOut = rot.points;
        xf::transformObject(rot, QTransform().rotate(30));
        QVERIFY(fills::contourHandlesDefault(rot));
        VObject lin = o; lin.type = ObjType::Path; lin.closed = true; lin.points = rot.points; lin.hIn = lin.hOut = lin.points;
        const QPointF before = fills::handleToDoc(lin, lin.gradP1);
        xf::transformObject(lin, QTransform().translate(5, 0));
        QCOMPARE(fills::handleToDoc(lin, lin.gradP1), before + QPointF(5, 0));   // other gradients still carry their handles
        // the plain look: the deepest point is the last stop, the outline the first, t grows with D
        QRect area;
        const QImage im = render::renderObject(c, &area, QRect(0, 0, 400, 400)).convertToFormat(QImage::Format_ARGB32);
        const QPoint ctr = (QPointF(85, 55) - QPointF(area.topLeft())).toPoint();
        QVERIFY(qGray(im.pixel(ctr)) > 240);
        QVERIFY(qGray(im.pixel(ctr + QPoint(-60, 0))) < qGray(im.pixel(ctr + QPoint(-30, 0))));
    }
    // one-axis kinds (Linear, Bars) show no second handle; turning the end handle keeps the bands square to it
    void oneAxisKindsKeepBandsSquare() {
        for (FillKind k : {FillKind::Linear, FillKind::Bars}) {
            const quint64 oid = gradDoc(k);
            appState().setTool(Tool::Pointer);
            drainEvents();
            VObject *o = d->find(oid);
            const QPointF p0 = fills::handleToDoc(*o, o->gradP0), p1 = fills::handleToDoc(*o, o->gradP1);
            // a click on where the hidden handle would be is not a handle
            QVERIFY(!fills::usesSecondAxis(k));
            drag(p1, p0 + QPointF(0, 100));   // turn the vector to point down
            o = d->find(oid);
            const QPointF u = fills::handleToDoc(*o, o->gradP1) - fills::handleToDoc(*o, o->gradP0);
            const QPointF v = fills::handleToDoc(*o, o->gradP2) - fills::handleToDoc(*o, o->gradP0);
            QVERIFY2(std::abs(u.x() * v.x() + u.y() * v.y()) < 1e-6 * (1 + std::hypot(u.x(), u.y()) * std::hypot(v.x(), v.y())),
                     qPrintable(fills::fillKindLabels().value(int(k))));
            // horizontal bands now: every row is one colour
            const QImage im = render::renderFrame(d->s, 0);
            for (int y = 60; y < 160; y += 7) QCOMPARE(im.pixel(80, y), im.pixel(200, y));
            closeDoc();
        }
    }

    // ---- item 2: pattern fill handles ----
    quint64 patternDoc(const QString &name = QStringLiteral("Bricks")) {
        const quint64 oid = gradDoc(FillKind::Linear);
        VObject *o = d->find(oid);
        o->fillKind = FillKind::Pattern;
        o->pattern = name;
        d->commit(QStringLiteral("pattern"));
        drainEvents();
        return oid;
    }
    // the identity handles paint exactly the pre-0.3.4 brush (tile at the bounds' top-left)
    void patternDefaultLookUnchanged() {
        VObject o; o.type = ObjType::Rect; o.rect = QRectF(37.5, 21.25, 150, 90);
        o.fillKind = FillKind::Pattern;
        for (const QString &n : fills::patternNames()) {
            o.pattern = n;
            QCOMPARE(fills::patternBrushTransform(o), QTransform::fromTranslate(o.rect.left(), o.rect.top()));
            QImage a(240, 160, QImage::Format_ARGB32_Premultiplied), b = a;
            a.fill(Qt::white); b.fill(Qt::white);
            { QPainter p(&a); p.setRenderHint(QPainter::Antialiasing); fills::fillShape(p, o, o.shapePath()); }
            { QPainter p(&b); p.setRenderHint(QPainter::Antialiasing);
              QBrush br(fills::patternTile(n)); br.setTransform(QTransform::fromTranslate(o.rect.left(), o.rect.top()));
              p.fillPath(o.shapePath(), br); }
            QCOMPARE(diffCount(a, b), 0);
        }
        // the default handles: centre in the middle, bars ending on the right and bottom edges,
        // and putting them back where they are is still the identity
        QPointF h[3];
        fills::patternHandles(o, h);
        QCOMPARE(h[0], o.rect.center());
        QCOMPARE(h[1], QPointF(o.rect.right(), o.rect.center().y()));
        QCOMPARE(h[2], QPointF(o.rect.center().x(), o.rect.bottom()));
        QVERIFY(fills::setPatternHandles(o, h));
        QVERIFY(o.patternXform.isIdentity());
        // degenerate bars are refused
        const QPointF flat[3] = {h[0], h[1], h[0] + (h[1] - h[0]) * 0.5};
        QVERIFY(!fills::setPatternHandles(o, flat));
        QVERIFY(o.patternXform.isIdentity());
    }
    void patternHandlesDrag_data() {
        QTest::addColumn<int>("tool");
        QTest::addColumn<int>("handle");
        for (const auto &t : {qMakePair(Tool::Pointer, "pointer"), qMakePair(Tool::PaintBucket, "bucket")})
            for (int h : {0, 1, 2, 3})
                QTest::newRow(qPrintable(QStringLiteral("%1/%2").arg(QLatin1String(t.second)).arg(h))) << int(t.first) << h;
    }
    void patternHandlesDrag() {
        QFETCH(int, tool);
        QFETCH(int, handle);
        const quint64 oid = patternDoc();
        appState().setTool(Tool(tool));
        drainEvents();
        const QImage before = render::renderFrame(d->s, 0);
        VObject *o = d->find(oid);
        QPointF h[3];
        fills::patternHandles(*o, h);
        const QTransform t0 = o->patternXform;
        const int hist = d->historyIndex();
        if (handle == 3) {
            // grab the X bar in the middle and swing it down 30 degrees: both axes turn together
            const QPointF grab = (h[0] + h[1]) / 2;
            QLineF l(h[0], grab); l.setAngle(l.angle() - 30);
            drag(grab, l.p2());
            QPointF n[3];
            fills::patternHandles(*d->find(oid), n);
            QVERIFY2(std::abs(QLineF(n[0], n[1]).angleTo(l) ) < 1.0 || std::abs(QLineF(n[0], n[1]).angleTo(l) - 360) < 1.0, "the bar turns with the pointer");
        } else {
            const QPointF to = handle == 0 ? h[0] + QPointF(-13, 7) : handle == 1 ? h[1] + QPointF(-31, 17) : h[2] + QPointF(19, -23);
            drag(h[handle], to);
            o = d->find(oid);
            QPointF n[3];
            fills::patternHandles(*o, n);
            QVERIFY2(QLineF(n[handle], to).length() < 1.5, "the handle follows the drag");
        }
        o = d->find(oid);
        const QTransform &t = o->patternXform;
        QVERIFY(!t.isIdentity());
        switch (handle) {
        case 0: // the centre: offset only
            QCOMPARE(t.m11(), 1.0); QCOMPARE(t.m22(), 1.0); QCOMPARE(t.m12(), 0.0); QCOMPARE(t.m21(), 0.0);
            QVERIFY(std::abs(t.dx() + 13) < 1.5 && std::abs(t.dy() - 7) < 1.5);
            break;
        case 1: // the X bar alone: Y axis untouched
            QCOMPARE(t.m21(), t0.m21()); QCOMPARE(t.m22(), t0.m22());
            QVERIFY(std::abs(t.m11() - 1) > 0.05 && std::abs(t.m12()) > 0.05);
            break;
        case 2: // the Y bar alone: X axis untouched
            QCOMPARE(t.m11(), t0.m11()); QCOMPARE(t.m12(), t0.m12());
            QVERIFY(std::abs(t.m22() - 1) > 0.05 && std::abs(t.m21()) > 0.05);
            break;
        case 3: { // turned 30 degrees about the centre, unscaled
            const double a = std::atan2(t.m12(), t.m11()) * 180 / M_PI;
            QVERIFY2(std::abs(std::abs(a) - 30) < 2.5, qPrintable(QString::number(a)));
            QVERIFY(std::abs(std::hypot(t.m11(), t.m12()) - 1) < 1e-6);
            QVERIFY(std::abs(std::hypot(t.m21(), t.m22()) - 1) < 1e-6);
            QVERIFY(std::abs(t.dx()) < 1e-9 && std::abs(t.dy()) < 1e-9);
            break;
        }
        }
        QVERIFY2(d->historyIndex() == hist + 1, "one undo step per handle drag");
        const int changed = diffCount(before, render::renderFrame(d->s, 0));
        QVERIFY2(changed > 500, qPrintable(QStringLiteral("only %1 pixels changed").arg(changed)));
        d->undo();
        drainEvents();
        QVERIFY(d->find(oid)->patternXform.isIdentity());
        QCOMPARE(diffCount(before, render::renderFrame(d->s, 0)), 0);
        closeDoc();
        appState().setTool(Tool::Pointer);
    }
    // the handles follow the object's own transform (they sit on the tile as painted)
    void patternHandlesOnTransformedObject() {
        VObject o; o.type = ObjType::Rect; o.rect = QRectF(50, 40, 120, 80);
        o.fillKind = FillKind::Pattern; o.pattern = QStringLiteral("Stripes");
        o.xform = QTransform().translate(110, 80).rotate(25).scale(1.5, 0.75).translate(-110, -80);
        QPointF h[3];
        fills::patternHandles(o, h);
        QCOMPARE(h[0], o.xform.map(o.rect.center()));
        QVERIFY(fills::setPatternHandles(o, h));
        QVERIFY(o.patternXform.isIdentity());
        const QPointF moved[3] = {h[0] + QPointF(5, 3), h[1] + QPointF(5, 3), h[2] + QPointF(5, 3)};
        QVERIFY(fills::setPatternHandles(o, moved));
        QPointF back[3];
        fills::patternHandles(o, back);
        for (int i = 0; i < 3; ++i) QVERIFY(QLineF(back[i], moved[i]).length() < 1e-6);
    }
    void patternXformRoundTrips() {
        Document *doc = Document::createNew(360, 150, Qt::white);
        VObject o; o.type = ObjType::Ellipse; o.rect = QRectF(20, 20, 140, 100); o.id = doc->newId();
        o.fillKind = FillKind::Pattern; o.pattern = QStringLiteral("Scales");
        o.patternXform = QTransform(0.8, 0.35, -0.2, 1.6, 7.5, -4.25);
        doc->addObject(o, 0, false);
        VObject plain = o; plain.id = doc->newId(); plain.patternXform = QTransform(); plain.rect = QRectF(200, 20, 140, 100);
        doc->addObject(plain, 0, false);
        const QByteArray data = lwd::serialize(*doc);
        std::unique_ptr<Document> back(lwd::deserialize(data));
        QVERIFY(back);
        QCOMPARE(back->find(o.id)->patternXform, o.patternXform);
        QVERIFY(back->find(plain.id)->patternXform.isIdentity());
        QCOMPARE(diffCount(render::renderFrame(doc->s, 0), render::renderFrame(back->s, 0)), 0);
        // the default writes nothing new, so older readers see the same file
        {
            std::unique_ptr<Document> only(Document::createNew(50, 50, Qt::white));
            VObject p2 = plain; p2.id = only->newId();
            only->addObject(p2, 0, false);
            QVERIFY(!lwd::serialize(*only).contains("patternXform"));
        }
        // styles carry it
        VObject target; target.type = ObjType::Rect; target.rect = QRectF(0, 0, 10, 10);
        Style st = Style::fromObject(o, QStringLiteral("p"));
        QVERIFY(st.applyTo(target));
        QCOMPARE(target.patternXform, o.patternXform);
        Style viaJson;
        QVERIFY(styles::fromJson(styles::toJson(st), viaJson));
        QCOMPARE(viaJson.attrs.patternXform, o.patternXform);
        // SVG: a userSpaceOnUse <pattern> whose patternTransform is the brush transform
        bool pure = false;
        const QByteArray svg = docio::stateToSvg(doc->s, 0, &pure);
        QVERIFY(pure);
        if (qEnvironmentVariableIsSet("LW_DUMP_PATTERN_SVG")) {   // for an outside renderer to compare against
            QFile f(qEnvironmentVariable("LW_DUMP_PATTERN_SVG"));
            if (f.open(QIODevice::WriteOnly)) f.write(svg);
            render::renderFrame(doc->s, 0).save(qEnvironmentVariable("LW_DUMP_PATTERN_SVG") + QStringLiteral(".png"));
        }
        QVERIFY(svg.contains("<pattern"));
        QVERIFY(svg.contains("patternUnits=\"userSpaceOnUse\""));
        const QTransform bt = fills::patternBrushTransform(o);
        const QString m = QStringLiteral("patternTransform=\"matrix(%1 ").arg(QString::number(bt.m11(), 'f', 3).remove(QRegularExpression(QStringLiteral("\\.?0+$"))));
        QVERIFY2(svg.contains(m.toUtf8()), svg.left(600).constData());
        QVERIFY(svg.contains("fill=\"url(#lwp"));
        delete doc;
    }

    // the handles are drawn on the canvas (centre circle, two square-ended bars) for Pointer and
    // Paint Bucket only; LW_SHOT_DIR saves pattern-handles.png
    void patternHandlesDrawn() {
        const quint64 oid = patternDoc();
        VObject *o = d->find(oid);
        QPointF h[3];
        fills::patternHandles(*o, h);
        // a turned, stretched, skewed tile: X bar longer and turned, Y bar leaning
        QLineF x(h[0], h[1]); x.setAngle(20); x.setLength(x.length() * 0.6);
        const QPointF nh[3] = {h[0] + QPointF(-10, 5), x.p2() + QPointF(-10, 5), h[2] + QPointF(-25, -5)};
        QVERIFY(fills::setPatternHandles(*o, nh));
        d->commit(QStringLiteral("handles"));
        drainEvents();
        // no brush outline or hover feedback: the pointer leaves the canvas first
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(cv, &leave);
        auto shot = [&](Tool t) { appState().setTool(t); drainEvents(); QCoreApplication::sendEvent(cv, &leave); cv->update(); drainEvents(); return cv->grab().toImage(); };
        const QImage none = shot(Tool::Brush), ptr = shot(Tool::Pointer), bucket = shot(Tool::PaintBucket), grad = shot(Tool::Gradient);
        QVERIFY(diffCount(none, grad) == 0);    // pattern handles are not the Gradient tool's
        QVERIFY(diffCount(none, ptr) > 40);
        QVERIFY(diffCount(none, bucket) > 40);
        // the handles sit where fills::patternHandles says
        fills::patternHandles(*d->find(oid), h);
        for (const QPointF &p : h) {
            const QPoint c = cv->toView(p).toPoint();
            for (const QImage *im : {&ptr, &bucket})
                QVERIFY2(none.pixel(c) != im->pixel(c) || none.pixel(c + QPoint(2, 0)) != im->pixel(c + QPoint(2, 0)), "a handle is drawn");
        }
        const QString dir = qEnvironmentVariable("LW_SHOT_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            appState().setTool(Tool::Pointer);
            cv->setZoom(2, cv->toView(d->find(oid)->bounds().center()));
            drainEvents();
            w->grab().save(QDir(dir).filePath(QStringLiteral("pattern-handles.png")));
        }
        closeDoc();
        appState().setTool(Tool::Pointer);
    }

    // ---- G: the eyedropper (ScreenColorPicker) ----
private:
    static void sendMouse(QWidget *target, QEvent::Type type, const QPoint &global, Qt::MouseButton b = Qt::NoButton) {
        const QPoint local = target->mapFromGlobal(global);
        QMouseEvent e(type, QPointF(local), QPointF(global), b, type == QEvent::MouseMove ? Qt::MouseButtons() : Qt::MouseButtons(b), Qt::NoModifier);
        QApplication::sendEvent(target, &e);
    }
    // the colour of the canvas as displayed at its centre
    QPoint canvasCentre(QColor *shown) {
        const QPoint c = cv->rect().center();
        const QImage g = cv->grab().toImage();
        *shown = QColor::fromRgb(g.pixel(int(c.x() * g.devicePixelRatio()), int(c.y() * g.devicePixelRatio())));
        return cv->mapToGlobal(c);
    }
private slots:
    void pickerSamplesWhatIsShown_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("screen") << int(ScreenColorPicker::Screen);
        QTest::newRow("app windows") << int(ScreenColorPicker::AppWindows);
    }
    void pickerSamplesWhatIsShown() {
        QFETCH(int, mode);
        const bool headless = QGuiApplication::platformName() == QLatin1String("offscreen");
        QCOMPARE(ScreenColorPicker::autoMode(), headless ? ScreenColorPicker::AppWindows : ScreenColorPicker::Screen);
        if (headless && mode == ScreenColorPicker::Screen)
            QSKIP("no real screen offscreen (the picker_screens test runs this on Xvfb)");
        d = Document::createNew(300, 300, QColor(200, 30, 40));
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        drainEvents();
        QColor shown;
        const QPoint g = canvasCentre(&shown);
        QCOMPARE(shown.rgb(), QColor(200, 30, 40).rgb());
        ScreenColorPicker *p = ScreenColorPicker::start(cv, ScreenColorPicker::Mode(mode));
        QVERIFY(p && ScreenColorPicker::active() == p);
        QCOMPARE(int(p->mode()), mode);
        QVERIFY(p->isVisible());
        QCOMPARE(QApplication::activePopupWidget(), static_cast<QWidget *>(p));
        QSignalSpy hov(p, &ScreenColorPicker::hovered), picked(p, &ScreenColorPicker::picked);
        // a panel (the Tools panel) samples too
        auto *tp = w->findChild<QWidget *>(QStringLiteral("ToolsPanel"));
        if (tp && tp->isVisible()) {
            const QPoint tc = tp->rect().center();
            const QImage ti = tp->grab().toImage();
            QCOMPARE(p->sampleAt(tp->mapToGlobal(tc)).rgb(), ti.pixel(tc));
        }
        sendMouse(p, QEvent::MouseMove, g);
        QVERIFY(!hov.isEmpty());
        QCOMPARE(hov.last().at(0).value<QColor>().rgb(), shown.rgb());
        QCOMPARE(p->current().rgb(), shown.rgb());
        QVERIFY(!p->geometry().contains(g));   // the preview is beside the pointer, not under it
        // the preview: magnified view centred on the pointer, swatch with the colour
        const QImage pv = p->grab().toImage();
        QCOMPARE(QColor(pv.pixel(10, 92)).rgb(), shown.rgb());
        QCOMPARE(QColor(pv.pixel(4 + 1 + 5 * 7 + 3, 4 + 1 + 5 * 7 + 3)).rgb(), shown.rgb());
        sendMouse(p, QEvent::MouseButtonPress, g, Qt::LeftButton);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.at(0).at(0).value<QColor>().rgb(), shown.rgb());
        QVERIFY(!ScreenColorPicker::active());
        drainEvents();
        closeDoc();
    }

    void pickerCancels() {
        for (int how = 0; how < 3; ++how) {
            ScreenColorPicker *p = ScreenColorPicker::start(w.get(), ScreenColorPicker::Screen);
            QSignalSpy c(p, &ScreenColorPicker::cancelled), picked(p, &ScreenColorPicker::picked);
            if (how == 0) QTest::keyClick(p, Qt::Key_Escape);
            else if (how == 1) sendMouse(p, QEvent::MouseButtonPress, w->mapToGlobal(QPoint(100, 100)), Qt::RightButton);
            else ScreenColorPicker::start(w.get(), ScreenColorPicker::Screen)->cancel();   // a new pick cancels the running one
            QCOMPARE(c.size(), 1);
            QCOMPARE(picked.size(), 0);
            QVERIFY(!ScreenColorPicker::active());
            drainEvents();
        }
    }

    // from a modal dialog's colour button: the canvas behind the dialog can be picked, the
    // dialog previews while hovering and goes back to its colour on cancel
    void pickerWorksFromModalDialogs() {
        d = Document::createNew(300, 300, QColor(20, 160, 90));
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        drainEvents();
        QColor shown;
        const QPoint g = canvasCentre(&shown);
        QDialog dlg(w.get());
        dlg.setModal(true);
        auto *lay = new QVBoxLayout(&dlg);
        auto *b = new ColorButton(&dlg, false);
        b->setColor(Qt::black);
        lay->addWidget(b);
        dlg.show();
        drainEvents();
        QVERIFY(QApplication::activeModalWidget() == &dlg);
        QSignalSpy prev(b, &ColorButton::colorPreview), pickedSpy(b, &ColorButton::colorPicked);
        for (int pass = 0; pass < 2; ++pass) {
            b->startEyedropper();
            ScreenColorPicker *p = ScreenColorPicker::active();
            QVERIFY(p);
            sendMouse(p, QEvent::MouseMove, g);
            QVERIFY(!prev.isEmpty());
            QCOMPARE(prev.last().at(0).value<QColor>().rgb(), shown.rgb());   // live preview
            if (pass == 0) {
                QTest::keyClick(p, Qt::Key_Escape);
                QCOMPARE(prev.last().at(0).value<QColor>().rgb(), QColor(Qt::black).rgb());   // restored
                QCOMPARE(b->color().rgb(), QColor(Qt::black).rgb());
                QCOMPARE(pickedSpy.size(), 0);
            } else {
                sendMouse(p, QEvent::MouseButtonPress, g, Qt::LeftButton);
                QCOMPARE(pickedSpy.size(), 1);
                QCOMPARE(b->color().rgb(), shown.rgb());
            }
            drainEvents();
        }
        // the swatch pop-up: hovering a swatch previews, a click picks, the hex field takes a value
        ColorPopup *pop = b->showPopup();
        QVERIFY(pop->findChild<QToolButton *>(QStringLiteral("eyedropper")));
        const int i = 5 * 18;   // row 5, column 0: web-safe #0000FF
        QCOMPARE(ColorPopup::swatches().size(), 18 * 13);
        QCOMPARE(ColorPopup::swatches().at(i).rgb(), QColor(0, 0, 255).rgb());
        const QPoint sp = pop->swatchRect(i).center();
        QTest::mouseMove(pop, sp);
        QMouseEvent mv(QEvent::MouseMove, QPointF(sp), QPointF(pop->mapToGlobal(sp)), Qt::NoButton, {}, Qt::NoModifier);
        QApplication::sendEvent(pop, &mv);
        QCOMPARE(prev.last().at(0).value<QColor>().rgb(), QColor(0, 0, 255).rgb());
        QTest::mouseClick(pop, Qt::LeftButton, {}, sp);
        drainEvents();
        QCOMPARE(b->color().rgb(), QColor(0, 0, 255).rgb());
        pop = b->showPopup();
        auto *hex = pop->findChild<QLineEdit *>(QStringLiteral("colorPopupHex"));
        QVERIFY(hex);
        QCOMPARE(hex->text(), QStringLiteral("#0000FF"));
        hex->setFocus();
        hex->selectAll();
        QTest::keyClicks(hex, QStringLiteral("12ab3c"));
        QTest::keyClick(hex, Qt::Key_Return);
        drainEvents();
        QCOMPARE(b->color().rgb(), QColor(0x12, 0xAB, 0x3C).rgb());
        dlg.close();
        drainEvents();
        closeDoc();
    }

    // device pixels on high-DPI screens: the device pixel under the logical point
    void pickerMapsDevicePixels() {
        QImage img(200, 100, QImage::Format_ARGB32);
        img.fill(Qt::blue);
        for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x) img.setPixel(x, y, qRgb(255, 0, 0));
        img.setPixel(199, 99, qRgb(0, 255, 0));
        QCOMPARE(ScreenColorPicker::sampleImage(img, 2, {49, 10}).rgb(), qRgb(255, 0, 0));
        QCOMPARE(ScreenColorPicker::sampleImage(img, 2, {50, 10}).rgb(), qRgb(0, 0, 255));
        QCOMPARE(ScreenColorPicker::sampleImage(img, 1.5, {66, 10}).rgb(), qRgb(255, 0, 0));   // device 99
        QCOMPARE(ScreenColorPicker::sampleImage(img, 1.5, {67, 10}).rgb(), qRgb(0, 0, 255));   // device 101
        QCOMPARE(ScreenColorPicker::sampleImage(img, 2, {99, 49}).rgb(), qRgb(0, 255, 0));
        QCOMPARE(ScreenColorPicker::sampleImage(img, 2, {500, 500}).rgb(), qRgb(0, 255, 0));   // clamped
        QCOMPARE(ScreenColorPicker::sampleImage(img, 1, {120, 10}).rgb(), qRgb(0, 0, 255));
        QVERIFY(!ScreenColorPicker::sampleImage(QImage(), 1, {0, 0}).isValid());
    }

    // several monitors, one of them high-DPI (run by the "picker_screens" test with an
    // offscreen configuration of two screens; skipped with one)
    void pickerAcrossScreens() {
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (screens.size() < 2) QSKIP("one screen (the picker_screens test runs this with two)");
        const QColor cols[2] = {QColor(230, 20, 20), QColor(20, 20, 230)};
        std::unique_ptr<QWidget> wins[2];
        for (int i = 0; i < 2; ++i) {
            wins[i] = std::make_unique<QWidget>();
            wins[i]->setAutoFillBackground(true);
            QPalette pal = wins[i]->palette();
            pal.setColor(QPalette::Window, cols[i]);
            wins[i]->setPalette(pal);
            wins[i]->setScreen(screens[i]);
            wins[i]->setGeometry(QRect(screens[i]->geometry().topLeft() + QPoint(40, 40), QSize(200, 150)));
            wins[i]->show();
        }
        drainEvents();
        // offscreen has no real screen to grab: there the app-window sampler crosses the screens
        const auto mode = QGuiApplication::platformName() == QLatin1String("offscreen") ? ScreenColorPicker::AppWindows
                                                                                       : ScreenColorPicker::Screen;
        ScreenColorPicker *p = ScreenColorPicker::start(wins[0].get(), mode);
        QCOMPARE(p->mode(), mode);
        for (int i = 0; i < 2; ++i) {
            const QPoint g = wins[i]->mapToGlobal(QPoint(100, 75));
            QVERIFY2(screens[i]->geometry().contains(g), qPrintable(QString::number(i)));
            QCOMPARE(p->sampleAt(g).rgb(), cols[i].rgb());
            sendMouse(p, QEvent::MouseMove, g);
            QCOMPARE(p->current().rgb(), cols[i].rgb());
            // the preview follows to that screen (a screen change settles the geometry a beat later)
            QTRY_VERIFY(QGuiApplication::screenAt(p->geometry().center()) == screens[i]);
            QCOMPARE(p->windowHandle()->screen(), screens[i]);
        }
        qInfo() << "screens:" << screens[0]->geometry() << screens[0]->devicePixelRatio() << screens[1]->geometry() << screens[1]->devicePixelRatio();
        p->cancel();
        drainEvents();
    }

    // B2: duplicating a layer gives fresh ids to everything, nested children included
    void duplicateLayerFreshNestedIds() {
        std::unique_ptr<Document> dp(Document::createNew(600, 600, Qt::white));
        Document &d = *dp;
        d.addObject(testShape(d, QRectF(0, 0, 10, 10)), 0, false);
        VObject g = testShape(d, QRectF(0, 0, 30, 30));
        g.type = ObjType::Group;
        g.children << testShape(d, QRectF(0, 0, 5, 5));
        VObject inner = testShape(d, QRectF(5, 5, 5, 5));
        inner.type = ObjType::Group;
        inner.children << testShape(d, QRectF(6, 6, 2, 2));
        g.children << inner;
        d.addObject(g, 0, false);
        d.addFrame(true);
        const int n = d.s.layers.size();
        const int at = d.duplicateLayer(0);
        QVERIFY(at >= 0);
        QCOMPARE(d.s.layers.size(), n + 1);
        QVector<quint64> ids;
        for (const Frame &f : d.s.frames) for (const auto &l : f.layers) collectIds(l, ids);
        QSet<quint64> uniq(ids.begin(), ids.end());
        QCOMPARE(uniq.size(), ids.size());
        QCOMPARE(d.s.frames[0].layers[at].size(), 2);
        QCOMPARE(d.s.frames[0].layers[at][1].children.size(), 2);
        QCOMPARE(d.s.frames[0].layers[at][1].children[1].children.size(), 1);
    }

    // B3 / B7: the Web Layer is not duplicated, does not move, and nothing moves above it
    void webLayerStaysOnTop() {
        std::unique_ptr<Document> dp(Document::createNew(600, 600, Qt::white));
        Document &d = *dp;
        d.addLayer(QStringLiteral("B"));
        const int web = d.ensureWebLayer();
        QCOMPARE(web, d.s.layers.size() - 1);
        const int n = d.s.layers.size();
        QCOMPARE(d.duplicateLayer(web), -1);
        QCOMPARE(d.s.layers.size(), n);
        d.moveLayer(0, web);
        d.moveLayer(web, 0);
        QCOMPARE(d.webLayer(), n - 1);
        QVERIFY(d.s.layers.last().web);
        const QString first = d.s.layers[0].name;
        d.moveLayer(0, 1);   // still allowed below it
        QCOMPARE(d.s.layers[1].name, first);
        // a duplicate of the layer under the Web Layer lands below it
        QVERIFY(d.duplicateLayer(n - 2) >= 0);
        QCOMPARE(d.webLayer(), d.s.layers.size() - 1);
    }

    // B5: undo history memory budget (bitmaps), the last step stays undoable
    void undoMemoryCap() {
        const int was = coreprefs::undoMemoryMB, wasSteps = coreprefs::undoSteps;
        coreprefs::undoMemoryMB = 4;
        coreprefs::undoSteps = 200;
        {
            std::unique_ptr<Document> dp(Document::createNew(600, 600, Qt::white));
            Document &d = *dp;
            VObject b = testShape(d, QRectF(0, 0, 512, 512));
            b.type = ObjType::Bitmap;
            b.image = QImage(512, 512, QImage::Format_ARGB32_Premultiplied);   // 1 MB
            b.image.fill(0xff000000);
            d.addObject(b, 0, false);
            d.commit(QStringLiteral("add"));
            int steps = 1;
            for (int i = 0; i < 20; ++i) {
                QImage im(512, 512, QImage::Format_ARGB32_Premultiplied);
                im.fill(qRgb(i * 10, 0, 0));
                d.find(b.id)->image = im;
                d.commit(QStringLiteral("paint"));
                ++steps;
            }
            QVERIFY2(d.historyBytes() <= (qint64(4) << 20) + (qint64(1) << 20), qPrintable(QString::number(d.historyBytes())));
            int undone = 0;
            while (d.canUndo()) { d.undo(); ++undone; }
            QVERIFY(undone >= 1);
            QVERIFY2(undone < steps, qPrintable(QString::number(undone)));
            // undoing restored a different bitmap (the previous step), not garbage
            QVERIFY(!d.find(b.id)->image.isNull());
        }
        {
            coreprefs::undoMemoryMB = 0;   // 0 = no memory limit: every step kept
            std::unique_ptr<Document> dp(Document::createNew(600, 600, Qt::white));
            Document &d = *dp;
            VObject b = testShape(d, QRectF(0, 0, 64, 64));
            b.type = ObjType::Bitmap;
            b.image = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
            b.image.fill(0);
            d.addObject(b, 0, false);
            d.commit(QStringLiteral("add"));
            for (int i = 0; i < 10; ++i) { d.find(b.id)->image.fill(qRgb(i, 0, 0)); d.commit(QStringLiteral("p")); }
            int undone = 0;
            while (d.canUndo()) { d.undo(); ++undone; }
            QCOMPARE(undone, 11);
        }
        coreprefs::undoMemoryMB = was;
        coreprefs::undoSteps = wasSteps;
    }

    // B6: crafted PSD/XCF headers must not allocate gigabytes, overflow, or crash
    void craftedLayeredFilesAreRejected() {
        struct Case { qint32 t, l, b, r; quint16 comp; QByteArray data; };
        const QByteArray tiny(4, char(0x7f));
        const QList<Case> cases = {
            {0, 0, 299999, 299999, 0, tiny},                 // 90 GB raw plane from 4 bytes
            {0, 0, 299999, 299999, 1, tiny},                 // RLE, counts missing
            {0, 0, 20000, 20000, 0, tiny},                   // under the side cap, over the pixel cap
            {INT_MIN, INT_MIN, INT_MAX, INT_MAX, 0, tiny},   // int overflow in right - left
            {0, 0, 1, 1, 0, QByteArray(1, char(9))},          // a valid 1x1 layer
        };
        for (const Case &c : cases) {
            const QByteArray f = craftPsd(c.t, c.l, c.b, c.r, c.comp, c.data);
            codec::LayeredImage li;
            QString err;
            QElapsedTimer t; t.start();
            const bool ok = codec::readPsd(f, li, &err);
            QVERIFY2(t.elapsed() < 5000, "slow");
            if (ok) for (const codec::RasterLayer &L : li.layers) QVERIFY(qint64(L.image.width()) * L.image.height() <= 1);
        }
        codec::LayeredImage good;
        QVERIFY(codec::readPsd(craftPsd(0, 0, 1, 1, 0, QByteArray(1, char(9))), good, nullptr));
        QCOMPARE(good.layers.size(), 1);
        QCOMPARE(good.layers[0].image.size(), QSize(1, 1));

        // random byte damage of real files: no crash, no runaway allocation (ASan run checks the rest)
        codec::LayeredImage src;
        src.width = 70; src.height = 50;
        for (int k = 0; k < 2; ++k) {
            codec::RasterLayer L;
            L.name = QStringLiteral("L%1").arg(k);
            L.image = QImage(70 - k * 20, 50 - k * 10, QImage::Format_ARGB32);
            for (int y = 0; y < L.image.height(); ++y) for (int x = 0; x < L.image.width(); ++x) L.image.setPixel(x, y, qRgba(x * 3, y * 5, (x * y) & 255, 128 + k * 100));
            L.offset = QPoint(k * 7, k * 3);
            src.layers << L;
        }
        QByteArray psd, xcf;
        QVERIFY(codec::writePsd(psd, src, nullptr));
        QVERIFY(codec::writeXcf(xcf, src, nullptr));
        QRandomGenerator rng(0x0304);
        for (const QByteArray *orig : {&psd, &xcf}) {
            for (int it = 0; it < 300; ++it) {
                QByteArray f = *orig;
                const int edits = 1 + rng.bounded(8);
                for (int e = 0; e < edits; ++e) {
                    const int pos = rng.bounded(int(f.size()));
                    f[pos] = char(rng.bounded(4) == 0 ? 0xff : rng.bounded(256));
                }
                if (rng.bounded(5) == 0) f.truncate(rng.bounded(int(f.size())));
                codec::LayeredImage li;
                if (orig == &psd) codec::readPsd(f, li, nullptr); else codec::readXcf(f, li, nullptr);
                for (const codec::RasterLayer &L : li.layers) QVERIFY(qint64(L.image.width()) * L.image.height() <= (qint64(1) << 28));
            }
        }
    }

    // P6: the threaded strip blur is bit-identical to the 0.3.3 scalar blur (all sizes, radii, 1 and 4 channels)
    void fastBlurMatchesReference() {
        QRandomGenerator g(0x3406);
        const QList<QSize> sizes = {QSize(1, 1), QSize(2, 9), QSize(300, 1), QSize(257, 5), QSize(513, 301), QSize(700, 640)};
        for (QSize sz : sizes) for (double rad : {0.3, 1.0, 2.5, 7.0, 33.0, 140.0, 300.0}) for (int ch : {1, 4}) {
            QImage a(sz, ch == 4 ? QImage::Format_ARGB32_Premultiplied : QImage::Format_Alpha8);
            for (int y = 0; y < a.height(); ++y) { uchar *p = a.scanLine(y); for (int x = 0; x < a.bytesPerLine(); ++x) p[x] = uchar(g.bounded(256)); }
            if (ch == 4) a = a.convertToFormat(QImage::Format_ARGB32).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QImage b = a.copy();
            fx::blur(a, rad);
            refBlur(b.bits(), b.width(), b.height(), b.bytesPerLine(), ch, rad);
            QVERIFY2(samePixels(a, b), qPrintable(QStringLiteral("%1x%2 r=%3 ch=%4").arg(sz.width()).arg(sz.height()).arg(rad).arg(ch)));
        }
    }

    // P5: the onion-skin frame is the canvas colour, the dimmed neighbours, then the current frame
    void onionSkinFrameUnchanged() {
        d = Document::createNew(200, 150, QColor(250, 240, 230));
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        d->addObject(rshape(d, QRectF(10, 10, 80, 60), "a"), 0, false);
        d->addFrame(false);
        { VObject o = rshape(d, QRectF(60, 40, 100, 90), "b"); o.fill = QColor(200, 20, 20, 160); d->addObject(o, 0, false); }
        d->addFrame(false);
        { VObject o = rshape(d, QRectF(5, 90, 120, 50), "c"); o.fill = QColor(20, 160, 60); d->addObject(o, 0, false); }
        d->setCurrentFrame(1);
        for (int mode : {1, 2, 3}) for (bool clear : {false, true}) {
            d->s.canvasColor = clear ? QColor(Qt::transparent) : QColor(250, 240, 230);
            d->setOnion(mode, 55);
            d->notifyChanged();
            drainEvents();
            QImage expect(200, 150, QImage::Format_ARGB32_Premultiplied);
            expect.fill(d->s.canvasColor.alpha() ? d->s.canvasColor.rgba() : 0u);
            {
                QPainter p(&expect);
                for (int f : d->onionFrames()) { p.setOpacity(qBound(0.05, d->onionOpacity / 100.0 / qAbs(f - d->curFrame), 1.0)); p.drawImage(0, 0, d->render(f, false)); }
                p.setOpacity(1);
                p.drawImage(0, 0, d->render(d->curFrame, false));
            }
            QVERIFY(!d->onionFrames().isEmpty());
            QVERIFY(samePixels(cv->renderedFrame(), expect));
        }
        d->setOnion(0);
    }

    // P10: between two ants phases only pixels inside antsRegion() change, so the partial repaint is exact
    void antsRepaintRegionCoversChanges() {
        d = Document::createNew(400, 300, Qt::white);
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        drainEvents();
        QPainterPath rectSel; rectSel.addRect(QRectF(40, 30, 200, 150));
        QPainterPath ellSel; ellSel.addEllipse(QRectF(100, 60, 180, 120)); ellSel.addRect(QRectF(10, 200, 50, 40));
        for (const QPainterPath &sel : {rectSel, ellSel}) {
            d->pixelSelection = sel;
            d->notifyChanged();
            drainEvents();
            const QImage a = cv->grab().toImage();
            // The ants timer is 160 ms. A fixed 400 ms wait can miss a tick under
            // ASan after a long suite, so poll until the dashes actually move.
            QImage b;
            int changed = 0;
            QElapsedTimer wait;
            wait.start();
            while (wait.elapsed() < 2000) {
                QTest::qWait(80);
                drainEvents();
                b = cv->grab().toImage();
                QCOMPARE(a.size(), b.size());
                changed = 0;
                for (int y = 0; y < a.height(); ++y) {
                    for (int x = 0; x < a.width(); ++x)
                        if (a.pixel(x, y) != b.pixel(x, y)) { changed = 1; break; }
                    if (changed) break;
                }
                if (changed) break;
            }
            const QRegion reg = cv->antsRegion();
            QVERIFY(!reg.isEmpty());
            QCOMPARE(a.size(), b.size());
            const qreal dpr = a.devicePixelRatio();
            changed = 0;
            for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width(); ++x)
                if (a.pixel(x, y) != b.pixel(x, y)) {
                    ++changed;
                    QVERIFY2(reg.contains(QPoint(int(x / dpr), int(y / dpr))), qPrintable(QStringLiteral("changed pixel %1,%2 outside the ants region").arg(x).arg(y)));
                }
            QVERIFY(changed > 0);   // the ants did move
        }
        d->pixelSelection = QPainterPath();
    }

    // P2: frame thumbnails are cached; an edit re-renders only the edited frame, selection changes none
    void frameThumbnailsCached() {
        d = Document::createNew(320, 240, Qt::white);
        w->addDocument(d);
        drainEvents();
        auto *fp = w->findChild<FramesPanel *>();
        QVERIFY(fp);
        if (!fp->isVisible()) QSKIP("Frames panel hidden in this layout");
        auto *list = fp->findChild<QListWidget *>();
        d->addObject(rshape(d, QRectF(10, 10, 100, 80), "a"), 0, false);
        d->addFrame(true);
        d->addFrame(false);
        d->addObject(rshape(d, QRectF(150, 100, 60, 60), "b"), 0, false);
        d->commit(QStringLiteral("frames"));
        drainEvents();
        QCOMPARE(list->count(), 3);
        auto same = [&] {
            for (int i = 0; i < 3; ++i) {
                const QImage want = d->render(i, true).scaled(32, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                if (list->item(i)->icon().pixmap(want.size()).toImage().convertToFormat(want.format()) != want) return false;
            }
            return true;
        };
        QVERIFY(same());
        int n0 = fp->renderedThumbCount();
        d->selectAll(); drainEvents(); d->clearSelection(); drainEvents();
        d->setCurrentFrame(0); drainEvents(); d->setCurrentFrame(2); drainEvents();
        QCOMPARE(fp->renderedThumbCount(), n0);   // nothing changed: nothing rendered
        d->find(d->objects(0).last().id)->moveBy(QPointF(30, 0));
        d->commit(QStringLiteral("Move"));
        drainEvents();
        QCOMPARE(fp->renderedThumbCount(), n0 + 1);   // only the edited frame
        QVERIFY(same());
        d->undo(); drainEvents();
        QVERIFY(same());
        d->s.canvasColor = QColor(10, 20, 30); d->commit(QStringLiteral("Canvas")); drainEvents();
        QVERIFY(same());
    }

    void optimizeEstimateOffUiThread() {
        d = Document::createNew(320, 240, Qt::white);
        w->addDocument(d);
        drainEvents();
        auto *op = w->findChild<OptimizePanel *>();
        QVERIFY(op);
        if (!op->isVisible()) QSKIP("Optimize panel hidden in this layout");
        auto settle = [&] { QTRY_VERIFY_WITH_TIMEOUT(!op->estimatePending(), 10000); drainEvents(); };
        auto shown = [&] {
            for (QLabel *l : op->findChildren<QLabel *>()) if (l->text().startsWith(QStringLiteral("Estimated: "))) return l->text();
            return QString();
        };
        settle();
        d->addObject(rshape(d, QRectF(10, 10, 100, 80), "a"), 0, false);
        d->commit(QStringLiteral("add"));
        const int n0 = op->estimatesRun();
        settle();
        QVERIFY(op->estimatesRun() > n0);
        // the same text a synchronous encode on the UI thread gives
        const EncodeResult r = encodePreview(*d, d->exportOptions, d->curFrame);
        QVERIFY(r.error.isEmpty());
        QVERIFY2(shown().startsWith(QStringLiteral("Estimated: ") + describeSize(r.bytes)), qPrintable(shown()));
        // a burst of edits is estimated once it settles, not once per edit
        const int n1 = op->estimatesRun();
        for (int i = 0; i < 10; ++i) { d->find(d->objects(0).last().id)->moveBy(QPointF(1, 0)); d->notifyChanged(); QCoreApplication::processEvents(); }
        settle();
        QVERIFY2(op->estimatesRun() - n1 <= 2, qPrintable(QString::number(op->estimatesRun() - n1)));
        // hidden: nothing is estimated until it is shown again
        op->hide();
        const int n2 = op->estimatesRun();
        d->find(d->objects(0).last().id)->moveBy(QPointF(5, 0)); d->commit(QStringLiteral("Move"));
        QTest::qWait(1300);
        settle();
        QCOMPARE(op->estimatesRun(), n2);
        op->show();
        settle();
        QVERIFY(op->estimatesRun() > n2);
        // the snapshot is independent of the document
        Document *snap = d->snapshot();
        QCOMPARE(snap->s.frames.size(), d->s.frames.size());
        QCOMPARE(snap->render(0, true), d->render(0, true));
        QVERIFY(snap->thread() == nullptr && snap->parent() == nullptr);
        delete snap;
    }

    void layersSelectionSyncMatchesRebuild() {
        d = Document::createNew(320, 240, Qt::white);
        w->addDocument(d);
        drainEvents();
        auto *lp = w->findChild<LayersPanel *>();
        QVERIFY(lp);
        auto *tree = lp->findChild<QTreeWidget *>();
        QVector<quint64> ids;
        for (int i = 0; i < 6; ++i) {
            VObject o = rshape(d, QRectF(10 + i * 20, 10, 15, 15), QStringLiteral("s%1").arg(i));
            d->addObject(o, 0, false);
            ids << o.id;
        }
        d->setSelection({ids[0], ids[1]});
        const quint64 g1 = d->groupSelection();
        d->setSelection({ids[2], ids[3]});
        const quint64 g2 = d->groupSelection();
        d->clearSelection();
        d->commit(QStringLiteral("setup"));
        drainEvents();
        using State = std::tuple<QSet<quint64>, QSet<quint64>, quint64>;   // selected rows, open groups, current row
        auto state = [&] {
            QSet<quint64> sel, open;
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                const quint64 id = (*it)->data(0, Qt::UserRole).toULongLong();
                if ((*it)->isSelected()) sel << id;
                if ((*it)->parent() && (*it)->isExpanded()) open << id;
            }
            return State(sel, open, tree->currentItem() ? tree->currentItem()->data(0, Qt::UserRole).toULongLong() : 0);
        };
        auto check = [&](const QSet<quint64> &sel) {
            QTreeWidgetItem *first = tree->topLevelItem(0);
            d->setSelection(sel);
            drainEvents();
            QVERIFY(tree->topLevelItem(0) == first);   // synced in place, not rebuilt
            const State synced = state();
            d->notifyStructure();                      // a full rebuild of the same state
            drainEvents();
            QVERIFY(tree->topLevelItem(0) != first);
            QCOMPARE(state(), synced);
        };
        check({ids[0]});            // inside g1: opens it
        check({ids[4], ids[5]});    // g1 closes again
        check({g1, g2});
        check({ids[3], ids[0]});    // both groups open
        check({});
        QTreeWidgetItem *g1Row = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->data(0, Qt::UserRole).toULongLong() == g1) g1Row = *it;
        QVERIFY(g1Row && g1Row->childCount() == 2);
        tree->expandItem(g1Row);   // the user opens a group: it stays open
        check({ids[5]});
        check({});
    }

    void saveReusesUnchangedEncodes() {
        std::unique_ptr<Document> doc(Document::createNew(200, 150, Qt::white));
        VObject b; b.type = ObjType::Bitmap; b.id = doc->newId(); b.rect = QRectF(10, 10, 64, 48);
        b.image = QImage(64, 48, QImage::Format_ARGB32_Premultiplied); b.image.fill(qRgb(200, 30, 30));
        doc->addObject(b, 0, false);
        VObject p = rshape(doc.get(), QRectF(100, 20, 50, 50), QStringLiteral("pat"));
        p.fillKind = FillKind::Pattern; p.pattern = QStringLiteral("Custom"); p.patternImage = QImage(8, 8, QImage::Format_ARGB32_Premultiplied); p.patternImage.fill(qRgb(0, 0, 255));
        doc->addObject(p, 0, false);
        const QByteArray a1 = lwd::serialize(*doc), a2 = lwd::serialize(*doc);
        QCOMPARE(a2, a1);   // the cached encodes and preview give the same file
        auto load = [](const QByteArray &ba) { return std::unique_ptr<Document>(lwd::deserialize(ba)); };
        auto preview = [](const QByteArray &ba) { QMap<QString, QByteArray> f; zip::read(ba, f); return QImage::fromData(f.value(QStringLiteral("preview.png"))); };
        // an edit in place is saved (the image's cache key changes) and shows in the preview
        doc->find(b.id)->image.setPixel(3, 4, qRgb(1, 2, 3));
        const QByteArray a3 = lwd::serialize(*doc);
        QVERIFY(a3 != a1);
        QCOMPARE(load(a3)->find(b.id)->image.pixel(3, 4), qRgb(1, 2, 3));
        doc->find(p.id)->patternImage.setPixel(0, 0, qRgb(9, 9, 9));
        doc->find(b.id)->image.fill(qRgb(0, 200, 0));
        const QByteArray a4 = lwd::serialize(*doc);
        QCOMPARE(load(a4)->find(p.id)->patternImage.pixel(0, 0), qRgb(9, 9, 9));
        QVERIFY(preview(a4) != preview(a3));
        QCOMPARE(preview(a4), doc->render(0, true).scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(preview(a4).format()));
    }

    // P9: selection outlines are cached across repaints, and rebuilt (matching a fresh build)
    // when an object moves, the selection changes or the view zooms
    void selectionOutlineCache() {
        d = Document::createNew(300, 200, Qt::white);
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        d->addObject(rshape(d, QRectF(10, 10, 80, 60), "a"), 0, false);
        { VObject o = rshape(d, QRectF(120, 40, 90, 70), "b"); o.type = ObjType::Ellipse; d->addObject(o, 0, false); }
        { VObject o; o.type = ObjType::Bitmap; o.name = QStringLiteral("bm"); o.rect = QRectF(30, 100, 50, 40); o.image = noise(50, 40); o.id = d->newId(); d->addObject(o, 0, false); }
        d->selectAll();
        drainEvents();
        cv->repaint();
        const int n0 = cv->selectionOutlineBuilds();
        cv->repaint(); cv->repaint();
        QCOMPARE(cv->selectionOutlineBuilds(), n0);
        const quint64 first = d->objects(0).first().id;
        d->find(first)->moveBy(QPointF(7, 3));
        d->notifyChanged();
        const QImage moved = cv->grab().toImage();
        QVERIFY(cv->selectionOutlineBuilds() > n0);
        const int n1 = cv->selectionOutlineBuilds();
        d->clearSelection(); d->selectAll(); drainEvents();
        const QImage fresh = cv->grab().toImage();
        QVERIFY(cv->selectionOutlineBuilds() > n1);
        QVERIFY(samePixels(moved, fresh));
        const int n2 = cv->selectionOutlineBuilds();
        cv->setZoom(2, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        cv->repaint();
        QVERIFY(cv->selectionOutlineBuilds() > n2);
    }

    // P4: dragging a top-level object re-renders only its old and new extents, and the cached
    // frame stays identical to a full render (effects, blend modes, opacity, text and a group nearby)
    void dragMoveDirtyRectMatchesFullRender() {
        d = Document::createNew(320, 220, QColor(240, 235, 225));
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        { VObject o; o.type = ObjType::Bitmap; o.name = QStringLiteral("bg"); o.rect = QRectF(0, 0, 320, 220); o.image = noise(320, 220); o.id = d->newId(); d->addObject(o, 0, false); }
        { VObject o = rshape(d, QRectF(20, 20, 70, 50), "shadow"); o.effects << LiveEffect::make(EffectType::DropShadow); d->addObject(o, 0, false); }
        { VObject o = rshape(d, QRectF(110, 30, 80, 60), "blend"); o.type = ObjType::Ellipse; o.blend = BlendMode::Multiply; o.opacity = 60;
          o.effects << LiveEffect::make(EffectType::GaussianBlur); d->addObject(o, 0, false); }
        // (no Text: Qt's glyph cache can shift an untouched text by a fraction of a pixel between renders)
        { VObject o = rshape(d, QRectF(40, 120, 150, 40), "plain"); o.strokeOn = true; o.strokeWidth = 5; d->addObject(o, 0, false); }
        { VObject o = rshape(d, QRectF(210, 120, 60, 60), "glow"); o.effects << LiveEffect::make(EffectType::OuterGlow); d->addObject(o, 0, false); }
        d->resetHistory();
        appState().setTool(Tool::Pointer);
        drainEvents();
        cv->repaint();
        for (const char *name : {"shadow", "blend", "glow"}) {
            quint64 oid = 0;
            for (const VObject &o : d->objects(0)) if (o.name == QLatin1String(name)) oid = o.id;
            QVERIFY(oid);
            d->selectOnly(oid);
            drainEvents();
            cv->repaint();
            QVERIFY(samePixels(cv->renderedFrame(), d->render(d->curFrame, true)));
            const QPointF a = d->find(oid)->bounds().center(), z = a + QPointF(-37, 23);
            const int before = cv->partialRenders();
            QTest::mouseMove(cv, vp(a));
            QTest::mousePress(cv, Qt::LeftButton, Qt::NoModifier, vp(a));
            for (int k = 1; k <= 6; ++k) {
                QTest::mouseMove(cv, vp(a + (z - a) * (k / 6.0)));
                cv->repaint();
                QVERIFY2(samePixels(cv->renderedFrame(), d->render(d->curFrame, true)), name);
            }
            QVERIFY2(cv->partialRenders() > before, name);
            QTest::mouseRelease(cv, Qt::LeftButton, Qt::NoModifier, vp(z));
            drainEvents();
            cv->repaint();
            QVERIFY2(samePixels(cv->renderedFrame(), d->render(d->curFrame, true)), name);
        }
        // undo after the moves: a full change, still exact
        d->undo();
        drainEvents();
        QVERIFY(samePixels(cv->renderedFrame(), d->render(d->curFrame, true)));
    }

    void portalPickColorReply() {
        QCOMPARE(ScreenColorPicker::colorFromPortalResults({{QStringLiteral("color"), QVariantList{1.0, 0.0, 0.2}}}).rgb(), qRgb(255, 0, 51));
        QVERIFY(!ScreenColorPicker::colorFromPortalResults({}).isValid());
        QVERIFY(!ScreenColorPicker::colorFromPortalResults({{QStringLiteral("color"), QVariantList{1.0, 0.0}}}).isValid());
        QVERIFY(!ScreenColorPicker::colorFromPortalResults({{QStringLiteral("color"), QVariantList{2.0, 0.0, 0.0}}}).isValid());
    }
};

QTEST_MAIN(V034)
#include "v034_test.moc"
