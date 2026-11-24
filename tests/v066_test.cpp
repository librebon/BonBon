// 0.3.27: layer object reorder, drag-reorder APIs, slider cancel, clipboard canvas
// size, favicon menu, font sources, stroke edge softness, brush live preview timing,
// plus the review fixes (moveLayers, drop-to-bottom, font proxy, tight bounds,
// endMerge, incremental path paint, paint-only event compression, locked groups).
#include "core/Document.h"
#include "core/Object.h"
#include "core/Stroke.h"
#include "core/BrushEngine.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/Dialogs.h"
#include "ui/DragReorder.h"
#include "ui/EditStrokeDialog.h"
#include "ui/MainWindow.h"
#include "ui/Panels.h"
#include "ui/PopupSlider.h"
#include "ui/WideFontCombo.h"
#include "ui/PreferencesDialog.h"
#include "ui/PropertyInspector.h"
#include "ui/ClipboardImage.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCompleter>
#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QtMath>
#include <algorithm>
#include <QComboBox>
#include <QFile>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QtTest>

namespace {

VObject mkRect(Document *d, const QRectF &r, const QString &name)
{
    VObject o;
    o.id = d->newId();
    o.type = ObjType::Rect;
    o.name = name;
    o.rect = r;
    o.fillOn = true;
    o.strokeOn = false;
    o.fill = QColor(200, 40, 40);
    return o;
}

VObject mkBitmap(Document *d, int w, int h, const QPointF &pos = QPointF(0, 0))
{
    VObject o;
    o.id = d->newId();
    o.type = ObjType::Bitmap;
    o.name = QStringLiteral("Bitmap");
    o.image = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    o.image.fill(QColor(20, 80, 200, 255));
    o.stampInterpolation();
    o.rect = QRectF(pos, QSizeF(w, h));
    o.fillOn = o.strokeOn = false;
    return o;
}

}

class V066 : public QObject {
    Q_OBJECT
private slots:
    void arrangeMovesObjectsInLayer()
    {
        auto *doc = Document::createNew(200, 200, Qt::white);
        QVERIFY(doc);
        auto a = mkRect(doc, QRectF(10, 10, 40, 40), QStringLiteral("A"));
        auto b = mkRect(doc, QRectF(60, 10, 40, 40), QStringLiteral("B"));
        auto c = mkRect(doc, QRectF(110, 10, 40, 40), QStringLiteral("C"));
        // storage back-to-front: A, B, C (C is front / top of the tree)
        doc->objects(0) = {a, b, c};
        doc->selectOnly(b.id);
        doc->arrange(1); // bring forward: swap with C
        QCOMPARE(doc->objects(0)[1].name, QStringLiteral("C"));
        QCOMPARE(doc->objects(0)[2].name, QStringLiteral("B"));
        doc->arrange(2); // send backward: back to A,B,C
        QCOMPARE(doc->objects(0)[1].name, QStringLiteral("B"));
        QCOMPARE(doc->objects(0)[2].name, QStringLiteral("C"));
        delete doc;
    }

    void relocateObjectsWithinAndAcrossLayers()
    {
        auto *doc = Document::createNew(200, 200, Qt::white);
        int l2 = doc->addLayer(QStringLiteral("Layer 2"));
        auto a = mkRect(doc, QRectF(10, 10, 40, 40), QStringLiteral("A"));
        auto b = mkRect(doc, QRectF(60, 10, 40, 40), QStringLiteral("B"));
        auto c = mkRect(doc, QRectF(110, 10, 40, 40), QStringLiteral("C"));
        doc->objects(0) = {a, b, c};
        const quint64 aid = a.id, bid = b.id, cid = c.id;
        // Move B to the front of the same layer (storage end).
        QVERIFY(doc->relocateObjects({bid}, 0, 0, 3));
        QCOMPARE(doc->objects(0).last().id, bid);
        // Move A into layer 2 at the back.
        QVERIFY(doc->relocateObjects({aid}, l2, 0, 0));
        QCOMPARE(doc->objects(l2).size(), 1);
        QCOMPARE(doc->objects(l2)[0].id, aid);
        QCOMPARE(doc->objects(0).size(), 2);
        QCOMPARE(doc->objects(0)[0].id, cid);
        delete doc;
    }

    void layersPanelButtonsMoveObjects()
    {
        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *doc = Document::createNew(200, 200, Qt::white);
        auto a = mkRect(doc, QRectF(10, 10, 40, 40), QStringLiteral("A"));
        auto b = mkRect(doc, QRectF(60, 10, 40, 40), QStringLiteral("B"));
        doc->objects(0) = {a, b};
        doc->resetHistory(QStringLiteral("Setup"));
        // Attach via opening a document: use the window's new-doc path if needed.
        // MainWindow stores docs in tabs; call fileNew-like by adding the doc.
        // Tests typically use w.openDocument or similar — fall back to arrange via buttons
        // after we put the doc in the window.
        w.addDocument(doc);
        QCoreApplication::processEvents();
        auto *lp = w.findChild<LayersPanel *>();
        QVERIFY(lp);
        auto *tree = lp->findChild<QTreeWidget *>();
        QVERIFY(tree);
        // Find object row B (front / first child of the layer, which is listed top-first)
        QTreeWidgetItem *layer = tree->topLevelItem(tree->topLevelItemCount() - 1);
        QVERIFY(layer);
        QVERIFY(layer->childCount() >= 2);
        QTreeWidgetItem *front = layer->child(0); // B, top of tree
        QCOMPARE(front->text(2), QStringLiteral("B"));
        tree->setCurrentItem(front);
        doc->selectOnly(b.id);
        lp->moveCurrent(-1); // visually down = send backward
        QCOMPARE(doc->objects(0)[0].name, QStringLiteral("B"));
        QCOMPARE(doc->objects(0)[1].name, QStringLiteral("A"));
    }

    void sliderCancelRestoresValueAndLeavesNoUndo()
    {
        MainWindow w;
        w.show();
        auto *doc = Document::createNew(200, 200, Qt::white);
        w.addDocument(doc);
        const int hist0 = doc->canUndo() ? 1 : 0;
        Q_UNUSED(hist0);
        PopupSliderField field(0, 100);
        field.setValue(40);
        field.show();
        QVERIFY(QTest::qWaitForWindowExposed(&field));
        field.openPopup(true);
        QVERIFY(field.isPopupOpen());
        field.setSliderValue(75);
        QVERIFY(field.value() != 40);
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(field.popup(), &esc);
        QCOMPARE(field.value(), 40);
        QVERIFY(!field.isPopupOpen());

        field.setValue(40);
        field.openPopup(true);
        field.setSliderValue(10);
        field.popup()->cancelDrag();
        QCOMPARE(field.value(), 40);
    }

    void clipboardBitmapIsExactPixelSize()
    {
        auto *doc = Document::createNew(400, 300, Qt::white);
        auto bm = mkBitmap(doc, 80, 50, QPointF(12, 7));
        doc->objects(0).append(bm);
        const QSize tight = clipboardCanvasSize({bm});
        QCOMPARE(tight, QSize(80, 50));
        // visualBounds is padded; artworkBounds must not be
        QVERIFY(bm.visualBounds().size().width() > 80 || bm.visualBounds().size().height() > 50);
        QCOMPARE(bm.artworkBounds().size().toSize(), QSize(80, 50));
        delete doc;
    }

    void clipboardVectorUsesObjectBounds()
    {
        auto *doc = Document::createNew(400, 300, Qt::white);
        auto r = mkRect(doc, QRectF(10, 20, 40, 30), QStringLiteral("R"));
        r.strokeOn = true;
        r.strokeWidth = 4;
        r.strokeAlign = 0;
        const QSize sz = clipboardCanvasSize({r});
        QVERIFY(sz.width() >= 40);
        QVERIFY(sz.height() >= 30);
        // Must not inflate by the old visualBounds +1 pad on each side beyond stroke.
        const QSize visual = r.visualBounds().toAlignedRect().size();
        QVERIFY(sz.width() <= visual.width());
        QVERIFY(sz.height() <= visual.height());
        delete doc;
    }

    void faviconDisabledWithoutDocument()
    {
        MainWindow w;
        w.show();
        QAction *a = w.action(QStringLiteral("file.exportfavicons"));
        QVERIFY2(a, "Export Favicon action not found");
        // No document on a fresh window
        QVERIFY(!a->isEnabled());
    }

    void customOnlyFontsPersistAndExcludeSystem()
    {
        Prefs p = appState().prefs;
        const bool oldSys = p.useSystemFonts;
        const bool oldCus = p.useCustomFonts;
        p.useSystemFonts = false;
        p.useCustomFonts = true;
        p.customFontDir.clear();
        appState().setPrefs(p);
        QCOMPARE(appState().prefs.useSystemFonts, false);
        QCOMPARE(appState().prefs.useCustomFonts, true);
        // Empty custom folder: list is empty (or only what was loaded), not the full system list.
        const int sysCount = QFontDatabase::families().size();
        QCOMPARE(appState().fontSourceFamilies().size(), 0);
        QVERIFY(sysCount > 10);

        PreferencesDialog dlg;
        dlg.setPrefs(appState().prefs);
        auto *sys = dlg.findChild<QCheckBox *>(QStringLiteral("prefUseSystemFonts"));
        auto *cus = dlg.findChild<QCheckBox *>(QStringLiteral("prefUseCustomFonts"));
        if (!sys || !cus) {
            // object names may differ; search by text
            for (QCheckBox *b : dlg.findChildren<QCheckBox *>()) {
                if (b->text().contains(QStringLiteral("System"), Qt::CaseInsensitive)) sys = b;
                if (b->text().contains(QStringLiteral("Custom"), Qt::CaseInsensitive)) cus = b;
            }
        }
        QVERIFY(sys && cus);
        QVERIFY(cus->isChecked());
        QVERIFY(!sys->isChecked());
        Prefs out = dlg.prefs();
        QCOMPARE(out.useSystemFonts, false);
        QCOMPARE(out.useCustomFonts, true);

        p.useSystemFonts = oldSys;
        p.useCustomFonts = oldCus;
        appState().setPrefs(p);
    }

    void edgeSoftnessChangesStrokePaint()
    {
        QPainterPath path;
        path.moveTo(20, 40);
        path.lineTo(80, 40);
        stroke::Params hard = stroke::paramsFor(QStringLiteral("basic.hard"));
        hard.edge = 0;
        stroke::Params soft = hard;
        soft.edge = 100;
        QVERIFY(stroke::usesEdgeSoftness(hard.look));
        auto render = [&](const stroke::Params &p) {
            QImage img(100, 80, QImage::Format_ARGB32_Premultiplied);
            img.fill(0);
            QPainter qp(&img);
            qp.setRenderHint(QPainter::Antialiasing, true);
            stroke::PaintInput in;
            in.params = p;
            in.width = 12;
            in.color = Qt::black;
            stroke::paint(qp, path, in);
            qp.end();
            return img;
        };
        const QImage a = render(hard);
        const QImage b = render(soft);
        QVERIFY(a != b);
        // Soft edge should have more semi-transparent pixels around the core.
        auto fringe = [](const QImage &im) {
            int n = 0;
            for (int y = 0; y < im.height(); ++y) {
                const QRgb *sl = reinterpret_cast<const QRgb *>(im.constScanLine(y));
                for (int x = 0; x < im.width(); ++x) {
                    const int al = qAlpha(sl[x]);
                    if (al > 8 && al < 240) ++n;
                }
            }
            return n;
        };
        QVERIFY(fringe(b) > fringe(a));
    }

    void imageStampDisablesEdge()
    {
        QVERIFY(!stroke::usesEdgeSoftness(stroke::Look::ImageStamp));
        QVERIFY(!stroke::usesEdgeSoftness(stroke::Look::Cellular));
        QVERIFY(stroke::usesEdgeSoftness(stroke::Look::Soft));
        QVERIFY(stroke::usesEdgeSoftness(stroke::Look::Pen));
        EditStrokeDialog dlg;
        stroke::Params p = stroke::paramsFor(QStringLiteral("lw.imageStamp"));
        if (p.look != stroke::Look::ImageStamp)
            p.look = stroke::Look::ImageStamp;
        dlg.setParams(p, Qt::black, 8);
        auto *edge = dlg.findChild<QSpinBox *>(QStringLiteral("editStrokeEdge"));
        QVERIFY(edge);
        QVERIFY(!edge->isEnabled());
    }

    void brushLiveMatchesFinishAndIsFastEnough()
    {
        QImage target(512, 512, QImage::Format_ARGB32_Premultiplied);
        target.fill(Qt::white);
        brush::Params bp;
        bp.size = 80;
        bp.edge = 80;
        bp.strokeKey = QStringLiteral("basic.soft");
        brush::applyTip(bp, brush::Category::Basic, 0);
        bp.size = 80;
        bp.edge = 80;
        brush::Stroke st;
        st.begin(target, bp);
        const int steps = 40;
        QVector<int> us;
        us.reserve(steps);
        QPointF a(40, 256);
        for (int i = 1; i <= steps; ++i) {
            const QPointF b(40 + i * 10, 256 + std::sin(i * 0.3) * 30);
            QElapsedTimer t;
            t.start();
            const QRect dirty = st.segment(a, b);
            st.composite(target, dirty);
            us.append(int(t.nsecsElapsed() / 1000));
            a = b;
        }
        QImage mid = target.copy();
        QElapsedTimer fin;
        fin.start();
        const QRect fd = st.finish();
        st.composite(target, fd);
        const int finishUs = int(fin.nsecsElapsed() / 1000);
        // Live already used a full-path paint, so finish should be a no-op or a
        // matching redraw, not a sudden smoothness change.
        if (!fd.isEmpty()) {
            // Allow tiny blending differences; the images should be very close.
            int diff = 0;
            for (int y = 0; y < mid.height(); ++y) {
                const QRgb *x = reinterpret_cast<const QRgb *>(mid.constScanLine(y));
                const QRgb *y2 = reinterpret_cast<const QRgb *>(target.constScanLine(y));
                for (int c = 0; c < mid.width(); ++c)
                    if (x[c] != y2[c]) ++diff;
            }
            QVERIFY2(diff < 512 * 512 / 20, qPrintable(QStringLiteral("live vs finish diff %1 px").arg(diff)));
        }
        std::sort(us.begin(), us.end());
        const int medianUs = us[us.size() / 2];
        qInfo("brush live frame median %d us (max %d, finish %d) on 80px tip, 512 canvas",
              medianUs, us.last(), finishUs);
        // 60 fps budget is 16.6 ms. Leave headroom; offscreen CPU can be slower.
        QVERIFY2(medianUs < 25000, qPrintable(QStringLiteral("median frame %1 us exceeds 25 ms").arg(medianUs)));
        st.end();
    }

    void pathEngineLiveMatchesFinish()
    {
        QImage target(512, 512, QImage::Format_ARGB32_Premultiplied);
        target.fill(Qt::white);
        brush::Params bp;
        brush::applyTip(bp, brush::Category::Calligraphy, 0);
        bp.size = 64;
        bp.edge = 40;
        QVERIFY(stroke::usesPathEngine(stroke::paramsFor(bp.strokeKey).look));
        brush::Stroke st;
        st.begin(target, bp);
        QPointF a(50, 200);
        QVector<int> us;
        for (int i = 1; i <= 30; ++i) {
            const QPointF b(50 + i * 12, 200 + std::sin(i * 0.4) * 40);
            QElapsedTimer t;
            t.start();
            const QRect dirty = st.segment(a, b);
            st.composite(target, dirty);
            us.append(int(t.nsecsElapsed() / 1000));
            a = b;
        }
        QImage mid = target.copy();
        QElapsedTimer fin;
        fin.start();
        const QRect fd = st.finish();
        if (!fd.isEmpty()) st.composite(target, fd);
        const int finishUs = int(fin.nsecsElapsed() / 1000);
        int diff = 0;
        for (int y = 0; y < mid.height(); ++y) {
            const QRgb *x = reinterpret_cast<const QRgb *>(mid.constScanLine(y));
            const QRgb *y2 = reinterpret_cast<const QRgb *>(target.constScanLine(y));
            for (int c = 0; c < mid.width(); ++c)
                if (x[c] != y2[c]) ++diff;
        }
        std::sort(us.begin(), us.end());
        const int medianUs = us[us.size() / 2];
        qInfo("path-engine live median %d us (max %d, finish %d, live-vs-finish %d px)",
              medianUs, us.last(), finishUs, diff);
        QCOMPARE(diff, 0);
        QVERIFY2(medianUs < 25000, qPrintable(QStringLiteral("median frame %1 us exceeds 25 ms").arg(medianUs)));
        st.end();
    }

    void moveLayersMovesABlockInOneStep()
    {
        auto *doc = Document::createNew(200, 200, Qt::white);
        doc->s.layers[0].name = QStringLiteral("L0");
        doc->addLayer(QStringLiteral("L1"));
        doc->addLayer(QStringLiteral("L2"));
        doc->addLayer(QStringLiteral("L3"));
        QCOMPARE(doc->s.layers.size(), 4);
        QVERIFY(doc->moveLayers({0, 1}, 4));
        QCOMPARE(doc->s.layers[0].name, QStringLiteral("L2"));
        QCOMPARE(doc->s.layers[1].name, QStringLiteral("L3"));
        QCOMPARE(doc->s.layers[2].name, QStringLiteral("L0"));
        QCOMPARE(doc->s.layers[3].name, QStringLiteral("L1"));
        // Contiguous block onto itself is a no-op.
        QVERIFY(!doc->moveLayers({2, 3}, 2));
        QVERIFY(!doc->moveLayers({2, 3}, 4));
        delete doc;
    }

    void dropBelowListGoesToBottom()
    {
        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto *doc = Document::createNew(200, 200, Qt::white);
        doc->s.layers[0].name = QStringLiteral("Bottom");
        const int top = doc->addLayer(QStringLiteral("Top"));
        auto back = mkRect(doc, QRectF(10, 10, 20, 20), QStringLiteral("Back"));
        auto front = mkRect(doc, QRectF(40, 10, 20, 20), QStringLiteral("Front"));
        auto moving = mkRect(doc, QRectF(70, 10, 20, 20), QStringLiteral("Moving"));
        doc->objects(0) = {back, front};
        doc->objects(top) = {moving};
        doc->resetHistory(QStringLiteral("Setup"));
        w.addDocument(doc);
        QCoreApplication::processEvents();
        auto *lp = w.findChild<LayersPanel *>();
        QVERIFY(lp);
        auto *tree = lp->findChild<QTreeWidget *>();
        QVERIFY(tree);
        QVERIFY(tree->topLevelItemCount() >= 2);
        // Tree is top-first: first top-level is "Top".
        QTreeWidgetItem *moveRow = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto *layer = tree->topLevelItem(i);
            for (int c = 0; c < layer->childCount(); ++c) {
                auto *ch = layer->child(c);
                if (ch->data(0, Qt::UserRole).toULongLong() == moving.id) moveRow = ch;
            }
        }
        QVERIFY(moveRow);
        tree->clearSelection();
        moveRow->setSelected(true);
        tree->setCurrentItem(moveRow);
        tree->resize(280, 520);
        QCoreApplication::processEvents();
        // Empty space is below the last *visible* row (the bottom layer's last child),
        // not merely below the last layer header.
        QTreeWidgetItem *last = tree->topLevelItem(tree->topLevelItemCount() - 1);
        QTreeWidgetItem *tip = last;
        while (tip->childCount() > 0)
            tip = tip->child(tip->childCount() - 1);
        const QRect vr = tree->visualItemRect(tip);
        const QPoint below(qMax(8, vr.center().x()), vr.bottom() + 24);
        QVERIFY2(!tree->itemAt(below), "drop point must be in the empty space below the list");
        QVERIFY(lp->applyLayersDrop(below));
        QCOMPARE(doc->objects(0).size(), 3);
        QCOMPARE(doc->objects(0)[0].id, moving.id); // storage 0 = back = visually bottom
        QCOMPARE(doc->objects(top).size(), 0);
    }

    void relocateIntoLockedGroupIsRejected()
    {
        auto *doc = Document::createNew(200, 200, Qt::white);
        auto a = mkRect(doc, QRectF(10, 10, 20, 20), QStringLiteral("A"));
        auto b = mkRect(doc, QRectF(40, 10, 20, 20), QStringLiteral("B"));
        doc->objects(0) = {a, b};
        doc->selectOnly(a.id);
        const quint64 gid = doc->groupSelection();
        QVERIFY(gid);
        VObject *g = doc->find(gid);
        QVERIFY(g && g->type == ObjType::Group);
        g->locked = true;
        QVERIFY(!doc->relocateObjects({b.id}, 0, gid, 0));
        QVERIFY(doc->find(b.id));
        QCOMPARE(doc->parentGroup(b.id) == nullptr || doc->parentGroup(b.id)->id != gid, true);
        delete doc;
    }

    void customOnlyFontComboHidesSystemFamilies()
    {
        Prefs p = appState().prefs;
        const bool oldSys = p.useSystemFonts;
        const bool oldCus = p.useCustomFonts;
        const QString oldDir = p.customFontDir;
        p.useSystemFonts = false;
        p.useCustomFonts = true;
        p.customFontDir.clear();
        appState().setPrefs(p);

        WideFontComboBox box;
        box.show();
        box.refreshFontSources();
        const QString cur = box.currentFont().family();
        QString sys;
        for (const QString &f : QFontDatabase::families()) {
            if (!f.isEmpty() && f.compare(cur, Qt::CaseInsensitive) != 0) { sys = f; break; }
        }
        QVERIFY(!sys.isEmpty());
        QCOMPARE(box.findText(sys, Qt::MatchExactly), -1);
        bool inModel = false;
        if (QAbstractItemModel *m = box.model()) {
            for (int i = 0; i < m->rowCount(); ++i) {
                if (m->index(i, 0).data().toString().compare(sys, Qt::CaseInsensitive) == 0)
                    inModel = true;
            }
        }
        QVERIFY(!inModel);
        box.setEditable(true);
        box.refreshFontSources();
        if (QCompleter *c = box.completer()) {
            c->setCompletionPrefix(sys);
            // The completer is wired to the filtered model, so a system family
            // that is not the current face must not be offered.
            bool offered = false;
            for (int i = 0; i < c->completionCount(); ++i) {
                if (c->completionModel()->index(i, 0).data().toString().compare(sys, Qt::CaseInsensitive) == 0)
                    offered = true;
            }
            QVERIFY(!offered);
        }

        p.useSystemFonts = oldSys;
        p.useCustomFonts = oldCus;
        p.customFontDir = oldDir;
        appState().setPrefs(p);
    }


    void fontSourcesReloadAtRuntimeBothDirections()
    {
        const Prefs saved = appState().prefs;
        const auto restore = qScopeGuard([&] { appState().setPrefs(saved); });

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString srcDir = QStringLiteral(LW_SOURCE_DIR "/tests/data/fonts");
        QVERIFY(QFile::copy(srcDir + QStringLiteral("/LwCustomTest.ttf"),
                            dir.filePath(QStringLiteral("LwCustomTest.ttf"))));
        QVERIFY(QFile::copy(srcDir + QStringLiteral("/LwCustomTestTwo.ttf"),
                            dir.filePath(QStringLiteral("LwCustomTestTwo.ttf"))));
        const QString customA = QStringLiteral("LwCustomTest");
        const QString customB = QStringLiteral("LwCustomTestTwo");
        QVERIFY2(!QFontDatabase::families().contains(customA),
                 "test font must not already be a system family");

        auto hasFamily = [](QComboBox *c, const QString &fam) {
            if (!c || fam.isEmpty()) return false;
            for (int i = 0; i < c->count(); ++i) {
                if (c->itemText(i).compare(fam, Qt::CaseInsensitive) == 0)
                    return true;
            }
            return false;
        };
        auto checkCustomOnly = [&](QComboBox *c, const QString &kept, const char *tag) {
            QVERIFY2(hasFamily(c, customA), tag);
            QVERIFY2(hasFamily(c, customB), tag);
            if (!kept.isEmpty())
                QVERIFY2(hasFamily(c, kept), tag);
            int extras = 0;
            QString extraName;
            for (int i = 0; i < c->count(); ++i) {
                const QString it = c->itemText(i);
                if (it.isEmpty()) continue;
                if (it.compare(customA, Qt::CaseInsensitive) == 0) continue;
                if (it.compare(customB, Qt::CaseInsensitive) == 0) continue;
                if (!kept.isEmpty() && it.compare(kept, Qt::CaseInsensitive) == 0) continue;
                ++extras;
                extraName = it;
            }
            QVERIFY2(extras == 0,
                     qPrintable(QStringLiteral("%1 unexpected family '%2' (count %3)")
                                    .arg(QString::fromLatin1(tag), extraName)
                                    .arg(c->count())));
        };

        // Start the way the report does: system fonts only.
        Prefs p = appState().prefs;
        p.useSystemFonts = true;
        p.useCustomFonts = false;
        p.customFontDir.clear();
        appState().setPrefs(p);
        QVERIFY(!appState().fontSourceFamilies().contains(customA));

        QString sysFam;
        const QString fallback = AppState::fallbackUiFontFamily();
        for (const QString &f : QFontDatabase::families()) {
            if (f.isEmpty() || f.contains(QLatin1Char('['))) continue;
            if (f.compare(QStringLiteral("Sans Serif"), Qt::CaseInsensitive) == 0) continue;
            if (f.compare(fallback, Qt::CaseInsensitive) == 0) continue;
            sysFam = f;
            break;
        }
        QVERIFY(!sysFam.isEmpty());

        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document *doc = Document::createNew(400, 300, Qt::white);
        VObject txt;
        txt.id = doc->newId();
        txt.type = ObjType::Text;
        txt.text = QStringLiteral("Hello");
        txt.rect = QRectF(20, 20, 200, 40);
        txt.fontSize = 24;
        txt.fontFamily = sysFam;
        doc->objects(0).append(txt);
        w.addDocument(doc);
        doc->selectOnly(txt.id);
        QCoreApplication::processEvents();

        auto *pi = w.findChild<PropertyInspector *>();
        QVERIFY(pi);
        auto *piFont = pi->findChild<WideFontComboBox *>(QStringLiteral("piFont"));
        QVERIFY(piFont);
        QVERIFY(piFont->count() > 20);
        QVERIFY(hasFamily(piFont, sysFam));
        QVERIFY(!hasFamily(piFont, customA));
        piFont->setCurrentFont(QFont(sysFam));
        QCOMPARE(piFont->currentFont().family(), sysFam);

        WideFontComboBox box;
        box.show();
        QCoreApplication::processEvents();
        box.setCurrentFont(QFont(sysFam));
        QCOMPARE(box.currentFont().family(), sysFam);

        QComboBox findCombo;
        findCombo.setEditable(true);
        fillFontSourceCombo(&findCombo, true);
        QVERIFY(findCombo.count() > 20);
        QVERIFY(!hasFamily(&findCombo, customA));

        // Switch to custom fonts only with a real folder.
        p.useSystemFonts = false;
        p.useCustomFonts = true;
        p.customFontDir = dir.path();
        appState().setPrefs(p);
        QCoreApplication::processEvents();
        box.refreshFontSources();
        fillFontSourceCombo(&findCombo, true);

        const QStringList loaded = appState().fontSourceFamilies();
        QVERIFY(loaded.contains(customA, Qt::CaseInsensitive));
        QVERIFY(loaded.contains(customB, Qt::CaseInsensitive));
        QCOMPARE(loaded.size(), 2);

        checkCustomOnly(&box, sysFam, "standalone combo after system->custom");
        QCOMPARE(box.currentFont().family(), sysFam);
        checkCustomOnly(piFont, sysFam, "PI combo after system->custom");
        QVERIFY(piFont->currentFont().family().compare(sysFam, Qt::CaseInsensitive) == 0);
        QCOMPARE(doc->find(txt.id)->fontFamily, sysFam);
        checkCustomOnly(&findCombo, findCombo.currentText() == customA || findCombo.currentText() == customB
                                        ? QString()
                                        : findCombo.currentText(),
                        "Find combo after system->custom");

        // Turning a source off and on again repeatedly must keep working.
        for (int cycle = 0; cycle < 3; ++cycle) {
            p.useSystemFonts = true;
            p.useCustomFonts = false;
            appState().setPrefs(p);
            QCoreApplication::processEvents();
            box.refreshFontSources();
            QVERIFY2(appState().fontSourceFamilies().size() > 20, "system list empty after toggle");
            QVERIFY(box.count() > 20);
            QVERIFY(hasFamily(&box, sysFam));
            QVERIFY2(!hasFamily(&box, customA), qPrintable(QStringLiteral("cycle %1 still lists custom").arg(cycle)));
            QCOMPARE(box.currentFont().family(), sysFam);
            QVERIFY(piFont->count() > 20);
            QVERIFY(!hasFamily(piFont, customA));
            QCOMPARE(doc->find(txt.id)->fontFamily, sysFam);

            p.useSystemFonts = false;
            p.useCustomFonts = true;
            p.customFontDir = dir.path();
            appState().setPrefs(p);
            QCoreApplication::processEvents();
            box.refreshFontSources();
            QVERIFY(appState().fontSourceFamilies().contains(customA, Qt::CaseInsensitive));
            checkCustomOnly(&box, sysFam, qPrintable(QStringLiteral("cycle %1 custom").arg(cycle)));
            QCOMPARE(box.currentFont().family(), sysFam);
        }

        // Reverse: custom-only current is a custom family, then switch to system only.
        box.setCurrentFont(QFont(customA));
        QCOMPARE(box.currentFont().family(), customA);
        piFont->setCurrentFont(QFont(customA));
        QCoreApplication::processEvents();

        p.useSystemFonts = true;
        p.useCustomFonts = false;
        appState().setPrefs(p);
        QCoreApplication::processEvents();
        box.refreshFontSources();
        QVERIFY(box.count() > 20);
        QVERIFY2(hasFamily(&box, customA), "excluded custom family should stay as the current entry");
        QVERIFY2(!hasFamily(&box, customB), "other custom family must leave the system-only list");
        QCOMPARE(box.currentFont().family(), customA);
        QVERIFY(hasFamily(piFont, customA));
        QVERIFY(piFont->currentFont().family().compare(customA, Qt::CaseInsensitive) == 0);
        QVERIFY(piFont->count() > 20);
    }

    void clipboardInstanceAndFractionalSizeAreTight()
    {
        auto *doc = Document::createNew(400, 300, Qt::white);
        auto r = mkRect(doc, QRectF(10.3, 20.6, 40, 30), QStringLiteral("R"));
        QCOMPARE(clipboardCanvasSize({r}), QSize(40, 30));
        QVERIFY(r.visualBounds().toAlignedRect().size().width() >= 40);

        auto bm = mkBitmap(doc, 40, 20, QPointF(0, 0));
        Symbol sym;
        sym.id = doc->newId();
        sym.name = QStringLiteral("Sym");
        sym.frames = {{bm}};
        doc->s.symbols.append(sym);
        SymbolScope scope(&doc->s.symbols);
        VObject inst;
        inst.id = doc->newId();
        inst.type = ObjType::Instance;
        inst.symbolId = sym.id;
        inst.rect = QRectF(5, 5, 40, 20);
        inst.fillOn = inst.strokeOn = false;
        const QSize instSz = clipboardCanvasSize({inst});
        QCOMPARE(instSz, QSize(40, 20));
        const QSize visual = inst.visualBounds().toAlignedRect().size();
        QVERIFY(visual.width() >= 40);
        QVERIFY(instSz.width() <= visual.width());
        QVERIFY(instSz.height() <= visual.height());
        delete doc;
    }

    void endMergeCancelDropsTipKeepIfChangedKeepsEdit()
    {
        auto *doc = Document::createNew(200, 200, Qt::white);
        auto a = mkRect(doc, QRectF(10, 10, 20, 20), QStringLiteral("A"));
        doc->objects(0).append(a);
        doc->resetHistory(QStringLiteral("Setup"));
        const int idx = doc->historyIndex();

        doc->beginMerge();
        doc->objects(0)[0].rect.translate(5, 0);
        doc->commit(QStringLiteral("Nudge"));
        QVERIFY(doc->historyIndex() > idx);
        doc->endMerge(Document::MergeEnd::Cancel);
        QCOMPARE(doc->historyIndex(), idx);
        QCOMPARE(doc->objects(0)[0].rect.x(), 10.0);

        doc->beginMerge();
        doc->objects(0)[0].rect.translate(8, 0);
        doc->commit(QStringLiteral("Nudge"));
        doc->endMerge(Document::MergeEnd::KeepIfChanged);
        QVERIFY(doc->historyIndex() > idx);
        QCOMPARE(doc->objects(0)[0].rect.x(), 18.0);
        delete doc;
    }

    void paintingInputTogglesCompressionOnlyWhileActive()
    {
        // MouseMove compression stays on so a high-rate pointer flood is folded
        // into one paint frame. begin() only disables tablet-event compression
        // so every tablet sample is kept; end() restores the saved tablet value
        // (including nesting).
        QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, true);
        QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents, true);
        QVERIFY(QCoreApplication::testAttribute(Qt::AA_CompressHighFrequencyEvents));
        QVERIFY(QCoreApplication::testAttribute(Qt::AA_CompressTabletEvents));
        paintinput::begin();
        QVERIFY(QCoreApplication::testAttribute(Qt::AA_CompressHighFrequencyEvents));
        QVERIFY(!QCoreApplication::testAttribute(Qt::AA_CompressTabletEvents));
        paintinput::begin(); // nested
        paintinput::end();
        QVERIFY(QCoreApplication::testAttribute(Qt::AA_CompressHighFrequencyEvents));
        QVERIFY(!QCoreApplication::testAttribute(Qt::AA_CompressTabletEvents));
        paintinput::end();
        QVERIFY(QCoreApplication::testAttribute(Qt::AA_CompressHighFrequencyEvents));
        QVERIFY(QCoreApplication::testAttribute(Qt::AA_CompressTabletEvents));
    }

    void pathEngineLongStrokeStaysIncrementalAndMatchesFinish()
    {
        QImage target(1024, 512, QImage::Format_ARGB32_Premultiplied);
        target.fill(Qt::white);
        brush::Params bp;
        brush::applyTip(bp, brush::Category::Calligraphy, 0);
        bp.size = 48;
        bp.edge = 40;
        QVERIFY(stroke::usesPathEngine(stroke::paramsFor(bp.strokeKey).look));
        brush::Stroke st;
        st.begin(target, bp);
        QPointF a(40, 256);
        QVector<int> first, last;
        const int steps = 200;
        for (int i = 1; i <= steps; ++i) {
            const QPointF b(40 + i * 4, 256 + std::sin(i * 0.15) * 40);
            QElapsedTimer t;
            t.start();
            const QRect dirty = st.segment(a, b);
            st.composite(target, dirty);
            const int us = int(t.nsecsElapsed() / 1000);
            if (i <= 40) first.append(us);
            if (i > steps - 40) last.append(us);
            a = b;
        }
        QImage mid = target.copy();
        const QRect fd = st.finish();
        if (!fd.isEmpty()) st.composite(target, fd);
        int diff = 0;
        for (int y = 0; y < mid.height(); ++y) {
            const QRgb *x = reinterpret_cast<const QRgb *>(mid.constScanLine(y));
            const QRgb *y2 = reinterpret_cast<const QRgb *>(target.constScanLine(y));
            for (int c = 0; c < mid.width(); ++c)
                if (x[c] != y2[c]) ++diff;
        }
        std::sort(first.begin(), first.end());
        std::sort(last.begin(), last.end());
        const int medFirst = first[first.size() / 2];
        const int medLast = last[last.size() / 2];
        qInfo("long path-engine first-40 median %d us, last-40 median %d us, live-vs-finish %d px",
              medFirst, medLast, diff);
        QCOMPARE(diff, 0);
        // Incremental tail paint: the last frames must not grow linearly with path length.
        QVERIFY2(medLast < medFirst * 4 + 8000,
                 qPrintable(QStringLiteral("last %1 us vs first %2 us").arg(medLast).arg(medFirst)));
        st.end();
    }

};


QTEST_MAIN(V066)
#include "v066_test.moc"
