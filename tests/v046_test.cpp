// v0.3.16: per-object bitmap Scaling (interpolation); Property Inspector dropdown;
// symbol-body in instance visual cache key.
#include "core/Document.h"
#include "core/Effects.h"
#include "core/Object.h"
#include "core/CorePrefs.h"
#include "core/Render.h"
#include "core/PixelScale.h"
#include "io/LwdFormat.h"
#include "ui/PropertyInspector.h"
#include "ui/MainWindow.h"
#include "Brand.h"
#include <QApplication>
#include <QComboBox>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

static void drainEvents() {
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

static bool samePixels(const QImage &a, const QImage &b) {
    if (a.size() != b.size()) return false;
    QImage A = a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage B = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < A.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(A.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(B.constScanLine(y));
        for (int x = 0; x < A.width(); ++x) if (pa[x] != pb[x]) return false;
    }
    return true;
}

static QImage checker(int w, int h) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        QRgb *p = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const bool on = ((x / 4) ^ (y / 4)) & 1;
            p[x] = on ? qRgba(220, 40, 40, 255) : qRgba(40, 40, 220, 255);
        }
    }
    return img;
}

static VObject makeScaledBitmap(Document &d, const QSize &src, const QRectF &place, int method) {
    VObject o;
    o.type = ObjType::Bitmap;
    o.id = d.newId();
    o.image = checker(src.width(), src.height());
    o.rect = place;
    o.fillOn = o.strokeOn = false;
    o.interpolation = method;
    return o;
}

class V046 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;
private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        render::setObjectVisualCacheEnabled(true);
        render::clearObjectVisualCache();
        w = std::make_unique<MainWindow>();
        w->resize(1400, 900);
        w->show();
        drainEvents();
    }
    void cleanupTestCase() { w.reset(); }
    void cleanup() {
        coreprefs::interpolation = coreprefs::Bicubic;
        render::clearObjectVisualCache();
    }

    // Changing Preferences must not alter bitmaps that already carry a stamped method.
    void prefChangeLeavesExistingBitmapPixelsIdentical() {
        const int saved = coreprefs::interpolation;
        coreprefs::interpolation = coreprefs::Bicubic;
        Document *d = Document::createNew(200, 160, Qt::white);
        VObject bmp = makeScaledBitmap(*d, QSize(40, 30), QRectF(20, 20, 120, 90), coreprefs::Bicubic);
        LiveEffect sh = LiveEffect::make(EffectType::DropShadow);
        sh.enabled = true;
        bmp.effects = {sh};
        d->addObject(bmp, 0, false);
        render::clearObjectVisualCache();
        QImage before = d->render(0, true);

        coreprefs::interpolation = coreprefs::Nearest;
        render::clearObjectVisualCache();
        QImage after = d->render(0, true);
        QVERIFY2(samePixels(before, after),
                 "changing Preferences interpolation must not change an already-stamped bitmap");
        QCOMPARE(d->find(bmp.id)->interpolation, int(coreprefs::Bicubic));
        coreprefs::interpolation = saved;
        delete d;
    }

    // Transforms / pixel edits keep the bitmap's own Scaling; only an explicit change updates it.
    void transformKeepsOwnMethod() {
        const int saved = coreprefs::interpolation;
        coreprefs::interpolation = coreprefs::Bicubic;
        Document *d = Document::createNew(200, 160, Qt::white);
        VObject bmp = makeScaledBitmap(*d, QSize(40, 30), QRectF(10, 10, 40, 30), coreprefs::Nearest);
        d->addObject(bmp, 0, false);
        quint64 id = bmp.id;

        coreprefs::interpolation = coreprefs::Soft; // would have been adopted under the old design
        VObject *p = d->find(id);
        p->setBounds(QRectF(10, 10, 120, 90));
        QCOMPARE(p->interpolation, int(coreprefs::Nearest)); // still Nearest

        QVERIFY(d->beginPixelEdit(*p));
        QCOMPARE(p->interpolation, int(coreprefs::Nearest));
        d->endPixelEdits(false);

        // Image Size with an explicit resample updates Scaling to that choice.
        d->scaleImage(200, 160, coreprefs::Bilinear);
        p = d->find(id);
        QCOMPARE(p->interpolation, int(coreprefs::Bilinear));
        coreprefs::interpolation = saved;
        delete d;
    }

    // New bitmaps (addObject / unset) pick up the Preferences default.
    void newBitmapsPickUpPrefDefault() {
        const int saved = coreprefs::interpolation;
        coreprefs::interpolation = coreprefs::Soft;
        Document *d = Document::createNew(100, 80, Qt::white);
        VObject o;
        o.type = ObjType::Bitmap;
        o.image = checker(20, 20);
        o.rect = QRectF(5, 5, 20, 20);
        o.fillOn = o.strokeOn = false;
        QCOMPARE(o.interpolation, -1);
        d->addObject(o, 0, true);
        QVERIFY(!d->selection.isEmpty());
        VObject *p = d->find(*d->selection.begin());
        QVERIFY(p);
        const quint64 id1 = p->id;
        QCOMPARE(p->interpolation, int(coreprefs::Soft));

        coreprefs::interpolation = coreprefs::Nearest;
        VObject o2;
        o2.type = ObjType::Bitmap;
        o2.image = checker(10, 10);
        o2.rect = QRectF(30, 5, 10, 10);
        o2.fillOn = o2.strokeOn = false;
        d->addObject(o2, 0, true);
        VObject *p2 = d->find(*d->selection.begin());
        QVERIFY(p2);
        QCOMPARE(p2->interpolation, int(coreprefs::Nearest));
        // The first bitmap stays Soft.
        QCOMPARE(d->find(id1)->interpolation, int(coreprefs::Soft));
        coreprefs::interpolation = saved;
        delete d;
    }

    // Property Inspector Scaling dropdown changes render, is undoable, multi-select + mixed.
    void propertyInspectorScalingDropdown() {
        const int saved = coreprefs::interpolation;
        coreprefs::interpolation = coreprefs::Bicubic;
        Document *d = Document::createNew(220, 160, Qt::white);
        VObject a = makeScaledBitmap(*d, QSize(40, 30), QRectF(10, 10, 100, 75), coreprefs::Bicubic);
        VObject b = makeScaledBitmap(*d, QSize(40, 30), QRectF(110, 10, 100, 75), coreprefs::Bicubic);
        d->addObject(a, 0, false);
        d->addObject(b, 0, false);
        d->commit(QStringLiteral("add"));
        w->addDocument(d); // MainWindow takes ownership of the tab's document
        drainEvents();

        auto *pi = w->findChild<PropertyInspector *>();
        QVERIFY(pi);
        auto *combo = pi->findChild<QComboBox *>(QStringLiteral("bitmapScaling"));
        QVERIFY(combo);
        QCOMPARE(combo->count(), pixelscale::resampleNames().size());

        d->selectOnly(a.id);
        drainEvents();
        QVERIFY(combo->isVisible() || combo->parentWidget()->isVisible());
        QCOMPARE(combo->currentIndex(), int(coreprefs::Bicubic));

        render::clearObjectVisualCache();
        QImage before = d->render(0, true);
        combo->setCurrentIndex(coreprefs::Nearest);
        drainEvents();
        QCOMPARE(d->find(a.id)->interpolation, int(coreprefs::Nearest));
        render::clearObjectVisualCache();
        QImage after = d->render(0, true);
        QVERIFY2(!samePixels(before, after), "Changing Scaling must re-render the bitmap");

        // Undo restores the previous method and look.
        QVERIFY(d->canUndo());
        d->undo();
        drainEvents();
        QCOMPARE(d->find(a.id)->interpolation, int(coreprefs::Bicubic));
        render::clearObjectVisualCache();
        QVERIFY(samePixels(before, d->render(0, true)));

        // Multi-select: apply Nearest to both; then differ them and expect a mixed blank index.
        d->setSelection({a.id, b.id});
        drainEvents();
        combo->setCurrentIndex(coreprefs::Nearest);
        drainEvents();
        QCOMPARE(d->find(a.id)->interpolation, int(coreprefs::Nearest));
        QCOMPARE(d->find(b.id)->interpolation, int(coreprefs::Nearest));

        d->find(b.id)->interpolation = coreprefs::Soft;
        d->notifyChanged();
        d->setSelection({a.id, b.id});
        drainEvents();
        QCOMPARE(combo->currentIndex(), -1); // mixed

        // Keep within the 1400 px layout budget (object page for a bitmap).
        QCOMPARE(w->width(), 1400);
        QVERIFY(pi->contentHeight() > 0);

        coreprefs::interpolation = saved;
        // leave the tab open; later tests get a fresh doc via addDocument if needed
    }

    void lwdRoundTripsInterpolation() {
        const int saved = coreprefs::interpolation;
        coreprefs::interpolation = coreprefs::Bilinear;
        Document *d = Document::createNew(120, 100, Qt::white);
        VObject bmp = makeScaledBitmap(*d, QSize(32, 24), QRectF(8, 8, 96, 72), coreprefs::Nearest);
        d->addObject(bmp, 0, false);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("interp.lwd"));
        QString err;
        QVERIFY2(lwd::save(*d, path, &err), qPrintable(err));
        Document *loaded = lwd::load(path, &err);
        QVERIFY2(loaded, qPrintable(err));
        VObject *p = nullptr;
        for (auto &f : loaded->s.frames)
            for (auto &l : f.layers)
                for (auto &o : l)
                    if (o.type == ObjType::Bitmap) p = &o;
        QVERIFY(p);
        QCOMPARE(p->interpolation, int(coreprefs::Nearest));
        coreprefs::interpolation = coreprefs::Bicubic;
        render::clearObjectVisualCache();
        QImage fromFile = loaded->render(0, true);
        coreprefs::interpolation = coreprefs::Nearest;
        render::clearObjectVisualCache();
        QImage expect = d->render(0, true);
        QVERIFY2(samePixels(fromFile, expect), "round-tripped Nearest bitmap must keep its look");

        // Missing key → stamp live pref at load.
        Document *oldStyle = Document::createNew(120, 100, Qt::white);
        VObject ob = makeScaledBitmap(*oldStyle, QSize(32, 24), QRectF(8, 8, 96, 72), -1);
        oldStyle->addObject(ob, 0, false); // addObject stamps Soft below after we set pref… stamp first
        // Force unset and save without going through addObject again:
        oldStyle->find(ob.id)->interpolation = -1;
        // Bypass commit stamp by writing via a clone that keeps -1: serialize after clearing
        // Use save on a doc whose commit was skipped — save reads current state, so -1 omits the key.
        const QString oldPath = dir.filePath(QStringLiteral("old.lwd"));
        QVERIFY2(lwd::save(*oldStyle, oldPath, &err), qPrintable(err));
        coreprefs::interpolation = coreprefs::Soft;
        Document *reopened = lwd::load(oldPath, &err);
        QVERIFY2(reopened, qPrintable(err));
        VObject *q = nullptr;
        for (auto &f : reopened->s.frames)
            for (auto &l : f.layers)
                for (auto &o : l)
                    if (o.type == ObjType::Bitmap) q = &o;
        QVERIFY(q);
        QCOMPARE(q->interpolation, int(coreprefs::Soft));
        delete reopened;
        delete oldStyle;
        delete loaded;
        delete d;
        coreprefs::interpolation = saved;
    }

    void undoRedoRestoresInterpolation() {
        Document *d = Document::createNew(100, 80, Qt::white);
        VObject bmp = makeScaledBitmap(*d, QSize(20, 20), QRectF(10, 10, 60, 60), coreprefs::Bicubic);
        d->addObject(bmp, 0, false);
        d->commit(QStringLiteral("add"));
        quint64 id = bmp.id;
        VObject *p = d->find(id);
        p->interpolation = coreprefs::Nearest;
        d->commit(QStringLiteral("Bitmap Scaling"));
        d->undo();
        QCOMPARE(d->find(id)->interpolation, int(coreprefs::Bicubic));
        d->redo();
        QCOMPARE(d->find(id)->interpolation, int(coreprefs::Nearest));
        delete d;
    }

    void symbolEditInvalidatesInstanceCache() {
        render::setObjectVisualCacheEnabled(true);
        render::clearObjectVisualCache();
        Document *d = Document::createNew(240, 180, Qt::white);
        VObject body;
        body.type = ObjType::Rect;
        body.id = d->newId();
        body.rect = QRectF(40, 40, 80, 60);
        body.fill = QColor(200, 40, 40);
        body.strokeOn = false;
        d->addObject(body, 0, false);
        d->selection = {body.id};
        const quint64 symId = d->convertToSymbol(QStringLiteral("Mark"), SymbolKind::Graphic);
        QVERIFY(symId != 0);
        VObject *inst = nullptr;
        for (auto &f : d->s.frames)
            for (auto &l : f.layers)
                for (auto &o : l)
                    if (o.type == ObjType::Instance) inst = &o;
        QVERIFY(inst);
        LiveEffect sh = LiveEffect::make(EffectType::DropShadow);
        sh.enabled = true;
        inst->effects = {sh};
        d->notifyChanged();
        QImage before = d->render(0, true);
        Symbol *sym = nullptr;
        for (Symbol &s : d->s.symbols) if (s.id == symId) sym = &s;
        QVERIFY(sym);
        QVector<QVector<VObject>> frames = sym->frames;
        frames[0][0].fill = QColor(40, 40, 200);
        d->setSymbolFrames(symId, frames);
        QImage after = d->render(0, true);
        QVERIFY2(!samePixels(before, after),
                 "after Edit Symbol, FX instance must show the new symbol colour");
        render::setObjectVisualCacheEnabled(false);
        render::clearObjectVisualCache();
        QVERIFY(samePixels(after, d->render(0, true)));
        render::setObjectVisualCacheEnabled(true);
        delete d;
    }

    void contentKeyIncludesInterpolation() {
        VObject a, b;
        a.type = b.type = ObjType::Bitmap;
        a.image = b.image = checker(16, 16);
        a.rect = b.rect = QRectF(0, 0, 48, 48);
        a.interpolation = coreprefs::Bicubic;
        b.interpolation = coreprefs::Nearest;
        QVERIFY(lwd::objectContentKey(a) != lwd::objectContentKey(b));
    }
};

QTEST_MAIN(V046)
#include "v046_test.moc"
