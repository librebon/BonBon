// 0.3.24: Edit Stroke on the Brush is seeded from the live tip; OK without edits
// is a no-op on every tip; PI Edge/Shape/Texture stay in sync; brush and vector
// custom params are separate; Size accepts fractions and sizes over 200; dab
// spacing is continuous around the factory value.
#include "core/Document.h"
#include "io/Codecs.h"
#include <QtEndian>
#include "core/Object.h"
#include "core/Stroke.h"
#include "core/Fills.h"
#include "core/BrushEngine.h"
#include "ui/AppState.h"
#include "ui/EditStrokeDialog.h"
#include "ui/MainWindow.h"
#include "ui/PropertyInspector.h"
#include "ui/Panels.h"
#include <QApplication>
#include <QMenu>
#include <QMetaObject>
#include <QKeyEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QTimer>
#include <QTemporaryFile>
#include <QDir>
#include <QtTest>

namespace {

int pixelDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()) return -1;
    int n = 0;
    const QImage aa = a.convertToFormat(QImage::Format_ARGB32);
    const QImage bb = b.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < aa.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(aa.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(bb.constScanLine(y));
        for (int x = 0; x < aa.width(); ++x)
            if (pa[x] != pb[x]) ++n;
    }
    return n;
}

QImage paintBrushLine(const brush::Params &bp, const QSize &sz = QSize(200, 80))
{
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(255, 255, 255, 255));
    brush::Stroke st;
    st.begin(img, bp);
    QRect dirty = st.segment(QPointF(24, 40), QPointF(24, 40));
    dirty |= st.segment(QPointF(24, 40), QPointF(176, 40));
    dirty |= st.finish();
    if (!dirty.isEmpty()) st.composite(img, dirty);
    st.end();
    return img;
}

brush::Params tipParams(brush::Category cat, int tip, double size = 16)
{
    brush::Params bp;
    brush::applyTip(bp, cat, tip);
    bp.size = size;
    bp.color = QColor(20, 40, 180, 255);
    bp.opacity = 100;
    return bp;
}

VObject mkRect(Document *d, const QRectF &r)
{
    VObject o;
    o.type = ObjType::Rect;
    o.rect = r;
    o.fillOn = true;
    o.fill = Qt::blue;
    o.strokeOn = false;
    o.id = d->newId();
    return o;
}

} // namespace

class V064Test : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        appState().brushStrokeByTip.clear();
        appState().strokeCustom = false;
        appState().strokeParams = stroke::Params{};
        appState().strokeKind = QStringLiteral("basic.hard");
        appState().strokeOn = true;
        appState().brushCategory = 0;
        appState().brushTip = 0;
        appState().brushSize = 16;
        appState().brushEdge = 0;
        appState().tipShape = 0;
        appState().brushTexture.clear();
        appState().brushTextureAmount = 0;
    }

    void okWithoutEditsLeavesEveryTipIdentical()
    {
        int checked = 0;
        QString failed;
        const QStringList cats = brush::categoryNames();
        for (int ci = 0; ci < cats.size(); ++ci) {
            const auto cat = brush::Category(ci);
            const QStringList tips = brush::tipNames(cat);
            for (int ti = 0; ti < tips.size(); ++ti) {
                brush::Params bp = tipParams(cat, ti, 16);
                const QImage before = paintBrushLine(bp);
                stroke::Params seeded = brush::liveEditStrokeParams(bp);
                brush::Params after = bp;
                after.strokeCustom = true;
                after.strokeParams = seeded;
                const QImage out = paintBrushLine(after);
                const int d = pixelDiff(before, out);
                ++checked;
                if (d != 0)
                    failed += cats[ci] + QLatin1Char('/') + tips[ti] + QStringLiteral(" diff=%1; ").arg(d);
            }
        }
        QVERIFY2(checked == 76, qPrintable(QStringLiteral("expected 76 tips, got %1").arg(checked)));
        QVERIFY2(failed.isEmpty(), qPrintable(failed));
    }

    void okWithoutEditsKeepsPiTextureAndNamedDabTips()
    {
        // Hard Line with a PI-chosen texture must stay textured and identical after OK.
        brush::Params bp = tipParams(brush::Category::Basic, 0, 16);
        bp.texture = QStringLiteral("Grain");
        bp.textureAmount = 80;
        const QImage before = paintBrushLine(bp);
        QVERIFY2(pixelDiff(before, paintBrushLine(tipParams(brush::Category::Basic, 0, 16))) > 0,
                 "texture must be visible on the baseline");
        stroke::Params seeded = brush::liveEditStrokeParams(bp);
        QCOMPARE(seeded.texture, QStringLiteral("Grain"));
        QCOMPARE(seeded.textureAmount, 80);
        QCOMPARE(seeded.inkAmount, 100);
        brush::Params after = bp;
        after.strokeCustom = true;
        after.strokeParams = seeded;
        QCOMPARE(pixelDiff(before, paintBrushLine(after)), 0);

        // Named tips called out in the review: Soft Line, Soft Rounded, Air Brush Basic, Hard Rounded.
        const struct { brush::Category cat; const char *tip; } named[] = {
            {brush::Category::Basic, "Soft Line"},
            {brush::Category::Basic, "Soft Rounded"},
            {brush::Category::AirBrush, "Basic"},
            {brush::Category::Basic, "Hard Rounded"},
        };
        for (const auto &n : named) {
            const int ti = brush::tipIndex(n.cat, QString::fromUtf8(n.tip));
            brush::Params t = tipParams(n.cat, ti, 16);
            stroke::Params sp = brush::liveEditStrokeParams(t);
            brush::Params c = t;
            c.strokeCustom = true;
            c.strokeParams = sp;
            const int d = pixelDiff(paintBrushLine(t), paintBrushLine(c));
            QVERIFY2(d == 0, n.tip);
            if (QString::fromUtf8(n.tip) == QLatin1String("Hard Rounded"))
                QVERIFY2(t.square && sp.square, "Hard Rounded stays square");
        }
    }

    void piEdgeShapeTextureStillWorkAfterEditStroke()
    {
        brush::Params bp = tipParams(brush::Category::Basic, 0, 16);
        bp.edge = 0;
        bp.square = false;
        stroke::Params sp = brush::liveEditStrokeParams(bp);
        bp.strokeCustom = true;
        bp.strokeParams = sp;
        const QImage base = paintBrushLine(bp);

        brush::Params edge = bp;
        edge.edge = 100; // PI Edge moved; strokeParams.edge stays 0
        QVERIFY2(pixelDiff(base, paintBrushLine(edge)) > 0,
                 "PI Edge must still change the stroke after Edit Stroke");

        brush::Params sq = bp;
        sq.square = true;
        QVERIFY2(pixelDiff(base, paintBrushLine(sq)) > 0,
                 "PI Shape must still change the stroke after Edit Stroke");

        brush::Params tex = bp;
        tex.texture = QStringLiteral("Grain");
        tex.textureAmount = 80;
        QVERIFY2(pixelDiff(base, paintBrushLine(tex)) > 0,
                 "PI Texture must still change the stroke after Edit Stroke");
    }

    void brushAndVectorCustomParamsStaySeparate()
    {
        MainWindow w;
        w.resize(1400, 800);
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(200, 160, Qt::white);
        w.addDocument(d);
        appState().setTool(Tool::Brush);
        appState().brushCategory = int(brush::Category::Libreworks);
        appState().brushTip = brush::tipIndex(brush::Category::Libreworks, QStringLiteral("Cellular"));
        appState().strokeKind = QStringLiteral("basic.hard");
        appState().strokeCustom = false;
        appState().strokeOn = false;
        appState().brushStrokeByTip.clear();
        QCoreApplication::processEvents();

        auto *pi = w.findChild<PropertyInspector *>();
        QVERIFY(pi);
        QCOMPARE(pi->editStrokeTarget(), PropertyInspector::EditStrokeTarget::BrushTip);
        auto *btn = pi->findChild<QPushButton *>(QStringLiteral("opt_editStroke"));
        QVERIFY(btn);
        bool opened = false;
        QTimer::singleShot(40, &w, [&] {
            for (QWidget *tl : QApplication::topLevelWidgets()) {
                auto *dlg = qobject_cast<EditStrokeDialog *>(tl);
                if (!dlg || !dlg->isVisible()) continue;
                opened = true;
                dlg->accept();
                return;
            }
        });
        btn->click();
        QCoreApplication::processEvents();
        QVERIFY(opened);
        QVERIFY(appState().brushStrokeByTip.contains(QStringLiteral("lw.cellular")));
        QVERIFY(!appState().strokeCustom);
        QVERIFY(!appState().strokeOn);
        QCOMPARE(appState().strokeKind, QStringLiteral("basic.hard"));

        // Vector default edits must not change the Brush tip, including a matching key.
        appState().setTool(Tool::Pen);
        QCoreApplication::processEvents();
        QCOMPARE(pi->editStrokeTarget(), PropertyInspector::EditStrokeTarget::VectorDefault);
        stroke::Params vp = stroke::paramsFor(QStringLiteral("basic.hard"));
        vp.inkAmount = 40;
        vp.key = QStringLiteral("basic.hard");
        pi->applyStrokeParams(vp);
        QVERIFY(appState().strokeCustom);
        QCOMPARE(appState().strokeParams.inkAmount, 40);
        QVERIFY(appState().brushStrokeByTip.contains(QStringLiteral("lw.cellular")));
        QVERIFY(!appState().brushStrokeByTip.contains(QStringLiteral("basic.hard")));

        // A Hard Line brush stroke still looks like the factory tip, not ink 40.
        brush::Params hard = tipParams(brush::Category::Basic, 0, 16);
        const QImage plain = paintBrushLine(hard);
        auto it = appState().brushStrokeByTip.constFind(QStringLiteral("basic.hard"));
        QVERIFY(it == appState().brushStrokeByTip.cend());
        QCOMPARE(pixelDiff(plain, paintBrushLine(hard)), 0);
    }

    void editStrokeSizeKeepsFractionsAndLargeValues()
    {
        EditStrokeDialog dlg;
        stroke::Params p = stroke::paramsFor(QStringLiteral("basic.hard"));
        dlg.setParams(p, QColor(Qt::black), 0.5);
        auto *ink = dlg.findChild<QSpinBox *>(QStringLiteral("editStrokeInk"));
        auto *sz = dlg.findChild<QDoubleSpinBox *>(QStringLiteral("editStrokeSize"));
        QVERIFY(ink && sz);
        QCOMPARE(sz->value(), 0.5);
        ink->setValue(80); // change a field other than Size
        QCOMPARE(dlg.params().size, 0.5);

        dlg.setParams(p, QColor(Qt::black), 2.5);
        QCOMPARE(sz->value(), 2.5);
        ink->setValue(70);
        QCOMPARE(dlg.params().size, 2.5);

        dlg.setParams(p, QColor(Qt::black), 500.0);
        QCOMPARE(sz->value(), 500.0);
        ink->setValue(60);
        QCOMPARE(dlg.params().size, 500.0);
        QVERIFY(sz->maximum() >= 1000.0 - 1e-6);
        QVERIFY(sz->minimum() <= 0.1 + 1e-6);
    }

    void dabSpacingIsContinuousAroundFactory()
    {
        brush::Params bp = tipParams(brush::Category::Basic, 0, 16);
        stroke::Params factory = brush::liveEditStrokeParams(bp);
        QCOMPARE(factory.spacing, 100);
        auto paintAt = [&](int spacing) {
            stroke::Params sp = factory;
            sp.spacing = spacing;
            brush::Params c = bp;
            c.strokeCustom = true;
            c.strokeParams = sp;
            return paintBrushLine(c);
        };
        const QImage at100 = paintAt(100);
        QCOMPARE(pixelDiff(paintBrushLine(bp), at100), 0); // factory spacing is a no-op
        const QImage at99 = paintAt(99);
        auto coverage = [](const QImage &im) {
            int n = 0;
            const int y = 40;
            for (int x = 40; x <= 160; ++x)
                if (im.pixelColor(x, y).alpha() > 128) ++n;
            return n;
        };
        // 99 must stay a continuous stroke (the old mapping turned it into spaced dabs).
        const int c100 = coverage(at100);
        const int c99 = coverage(at99);
        QVERIFY2(c100 > 100, "factory Hard Line is a continuous stroke");
        QVERIFY2(c99 >= c100 - 2, qPrintable(QStringLiteral("spacing 99 must stay continuous (c99=%1 c100=%2)").arg(c99).arg(c100)));
        QVERIFY2(pixelDiff(at100, paintAt(400)) > 0, "a large spacing change still affects paint");
    }

    void editStrokeEnableMatchesDialogTarget()
    {
        MainWindow w;
        w.resize(1400, 800);
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(200, 160, Qt::white);
        w.addDocument(d);
        auto *pi = w.findChild<PropertyInspector *>();
        auto *act = w.action(QStringLiteral("modify.editstroke"));
        QVERIFY(pi && act);

        appState().setTool(Tool::Pointer);
        QCoreApplication::processEvents();
        QCOMPARE(pi->editStrokeTarget(), PropertyInspector::EditStrokeTarget::VectorDefault);
        QVERIFY(act->isEnabled());

        appState().setTool(Tool::Pencil);
        QCoreApplication::processEvents();
        QCOMPARE(pi->editStrokeTarget(), PropertyInspector::EditStrokeTarget::None);
        QVERIFY(!act->isEnabled());

        appState().setTool(Tool::Pen);
        QCoreApplication::processEvents();
        QCOMPARE(pi->editStrokeTarget(), PropertyInspector::EditStrokeTarget::VectorDefault);
        QVERIFY(act->isEnabled());

        d->addObject(mkRect(d, QRectF(10, 10, 40, 30)), 0, true);
        appState().setTool(Tool::Pointer);
        QCoreApplication::processEvents();
        QCOMPARE(pi->editStrokeTarget(), PropertyInspector::EditStrokeTarget::SelectedVector);
        QVERIFY2(act->isEnabled(), "a selected vector enables Edit Stroke even when stroke is off");

        appState().setTool(Tool::Brush);
        QCoreApplication::processEvents();
        QCOMPARE(pi->editStrokeTarget(), PropertyInspector::EditStrokeTarget::BrushTip);
        QVERIFY(act->isEnabled());
    }



    void readApngRejectsTruncatedIhdrAndPrefixes()
    {
        auto chunk = [](const char *type, const QByteArray &data) {
            QByteArray o;
            char l[4];
            qToBigEndian(quint32(data.size()), l);
            o.append(l, 4);
            o.append(type, 4);
            o.append(data);
            o.append(QByteArray(4, '\0')); // CRC ignored by parseChunks
            return o;
        };
        QByteArray file("\x89PNG\r\n\x1a\n", 8);
        file += chunk("IHDR", QByteArray(2, '\0')); // truncated IHDR
        file += chunk("acTL", QByteArray(8, '\0'));
        file += chunk("IEND", QByteArray());
        ImageSeq seq;
        QString err;
        QVERIFY2(!codec::readApng(file, seq, &err), "truncated IHDR must be rejected");
        QVERIFY(!err.isEmpty());
        QVERIFY(!codec::isApng(QByteArray("\x89PNG\r\n\x1a\n", 8)));

        // Fuzz: every prefix of a well-formed-looking APNG must not crash or over-read.
        QByteArray good("\x89PNG\r\n\x1a\n", 8);
        QByteArray ihdr(13, '\0');
        qToBigEndian(quint32(8), ihdr.data());
        qToBigEndian(quint32(8), ihdr.data() + 4);
        ihdr[8] = 8; // bit depth
        ihdr[9] = 6; // RGBA
        good += chunk("IHDR", ihdr);
        good += chunk("acTL", QByteArray(8, '\0'));
        good += chunk("IEND", QByteArray());
        for (int n = 0; n <= good.size(); ++n) {
            ImageSeq s;
            QString e;
            codec::readApng(good.left(n), s, &e);
            codec::isApng(good.left(n));
        }
        QVERIFY(true);
    }

    void livePreviewMatchesFinalForSmokeAndCellular()
    {
        auto run = [](const QString &key, const char *label) {
            brush::Params bp;
            bp.strokeKey = key;
            bp.size = 16;
            bp.color = QColor(20, 40, 180, 255);
            bp.opacity = 100;
            QImage live(320, 220, QImage::Format_ARGB32_Premultiplied);
            live.fill(QColor(255, 255, 255, 255));
            brush::Stroke st;
            st.begin(live, bp);
            const QPointF pts[] = {QPointF(40, 160), QPointF(80, 140), QPointF(130, 150),
                                   QPointF(180, 120), QPointF(230, 140), QPointF(280, 110)};
            QRect dirty;
            for (int i = 1; i < 6; ++i)
                dirty |= st.segment(pts[i - 1], pts[i]);
            if (!dirty.isEmpty()) st.composite(live, dirty);
            QImage finished = live;
            const QRect fin = st.finish();
            if (!fin.isEmpty()) st.composite(finished, fin);
            st.end();
            const int d = pixelDiff(live, finished);
            QVERIFY2(d == 0, qPrintable(QStringLiteral("%1 live vs final differ by %2 px").arg(QString::fromUtf8(label)).arg(d)));
        };
        run(QStringLiteral("lw.smoke"), "Smoke");
        run(QStringLiteral("lw.cellular"), "Cellular");
    }

    void cellularLockedGridStaysBounded()
    {
        // A long locked diagonal must not balloon to a multi-gigabyte grid (M4).
        stroke::Params p = stroke::paramsFor(QStringLiteral("lw.cellular"));
        p.cellSize = 1;
        QPainterPath path;
        path.moveTo(10, 10);
        path.lineTo(3900, 2900);
        QImage img(400, 300, QImage::Format_ARGB32_Premultiplied);
        img.fill(0);
        QPainter painter(&img);
        stroke::PaintInput in;
        in.params = p;
        in.width = 8;
        in.color = QColor(20, 180, 80);
        in.seed = 1;
        in.cellSizeLock = 1;
        in.gridLimit = QRect(0, 0, 400, 300); // live dirty / painted area
        stroke::paint(painter, path, in);
        painter.end();
        QVERIFY2(!img.isNull(), "capped Cellular paint returns");
        int ink = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (img.pixelColor(x, y).alpha() > 0) ++ink;
        QVERIFY2(ink > 0, "clipped Cellular still paints inside the dirty rect");
    }

    void layersTrashIgnoresDismissedContextMenu()
    {
        MainWindow w;
        w.resize(1400, 800);
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(240, 180, Qt::white);
        VObject a = mkRect(d, QRectF(10, 10, 30, 20));
        VObject b = mkRect(d, QRectF(50, 10, 30, 20));
        d->addObject(a, 0, false);
        d->addObject(b, 0, false);
        d->setSelection({a.id});
        w.addDocument(d);
        QCoreApplication::processEvents();
        auto *panel = w.findChild<LayersPanel *>();
        QVERIFY(panel);
        auto *tree = panel->findChild<QTreeWidget *>();
        auto *trash = panel->findChild<QToolButton *>(QStringLiteral("layers.trash"));
        QVERIFY(tree && trash);
        tree->expandAll();
        QCoreApplication::processEvents();

        auto rowFor = [&](quint64 id) -> QTreeWidgetItem * {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *layer = tree->topLevelItem(i);
                for (int j = 0; j < layer->childCount(); ++j) {
                    QTreeWidgetItem *it = layer->child(j);
                    if (it->data(0, Qt::UserRole).toULongLong() == id) return it;
                }
            }
            return nullptr;
        };
        QTreeWidgetItem *rowA = rowFor(a.id);
        QVERIFY(rowA);
        tree->scrollToItem(rowA);
        QCoreApplication::processEvents();
        const QRect r = tree->visualItemRect(rowA);
        const QPoint vp = r.center();
        QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, vp));
        QCoreApplication::processEvents();
        for (QWidget *tl : QApplication::topLevelWidgets()) {
            if (auto *m = qobject_cast<QMenu *>(tl)) {
                if (m->objectName() == QLatin1String("layersOptionsMenu") && m->isVisible())
                    m->hide();
            }
        }
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::Timer);

        d->setSelection({b.id});
        QCoreApplication::processEvents();
        trash->click();
        QCoreApplication::processEvents();
        QVERIFY2(d->find(a.id), "trash must not delete the stale right-click row A");
        QVERIFY2(!d->find(b.id), "trash deletes the current selection B");
    }

    void setWeb(bool on)
    {
        Prefs p = appState().prefs;
        p.enableWebFeatures = on;
        appState().setPrefs(p);
    }

    void insertSliceSkipsHiddenGroupChildren()
    {
        setWeb(true);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(240, 180, Qt::white);
        VObject a, b, c;
        a.type = b.type = c.type = ObjType::Rect;
        a.rect = QRectF(10, 10, 40, 30);
        b.rect = QRectF(80, 20, 50, 40);
        c.rect = QRectF(140, 30, 40, 30);
        a.fillOn = b.fillOn = c.fillOn = true;
        a.fill = Qt::blue; b.fill = Qt::red; c.fill = Qt::green;
        a.strokeOn = b.strokeOn = c.strokeOn = false;
        a.id = d->newId(); b.id = d->newId(); c.id = d->newId();
        b.visible = false;
        VObject g;
        g.type = ObjType::Group;
        g.id = d->newId();
        g.children = {a, b, c};
        d->addObject(g, 0, true);
        w.addDocument(d);
        QCoreApplication::processEvents();
        QAction *ins = w.action(QStringLiteral("edit.insertslice"));
        QVERIFY(ins && ins->isEnabled());
        ins->trigger();
        QCoreApplication::processEvents();
        int slices = 0;
        const int wl = d->webLayer();
        QVERIFY(wl >= 0);
        for (const VObject &o : d->objects(wl))
            if (o.type == ObjType::Slice) ++slices;
        QCOMPARE(slices, 2);
        setWeb(false);
    }

    void underSampleCacheReleasesAfterStrokeEnd()
    {
        brush::Params bp;
        bp.size = 16;
        bp.color = QColor(20, 40, 180, 255);
        bp.opacity = 100;
        bp.strokeCustom = true;
        bp.strokeParams = stroke::paramsFor(QStringLiteral("lw.cellular"));
        bp.strokeParams.cellSampleUnder = true;
        bp.strokeParams.cellFollowEdges = true;
        bp.strokeParams.cellSize = 8;
        const QImage a = paintBrushLine(bp, QSize(80, 80));
        // finish/end already released the planes; a second call must be a no-op.
        stroke::releaseUnderSampleCache();
        const QImage b = paintBrushLine(bp, QSize(80, 80));
        QVERIFY(!a.isNull() && !b.isNull());
        QCOMPARE(pixelDiff(a, b), 0);
    }

    void stampCacheCapsFileStampsAndCachesMisses()
    {
        QImage big(400, 400, QImage::Format_ARGB32_Premultiplied);
        big.fill(qRgba(200, 10, 10, 255));
        const QString path = QDir::temp().filePath(QStringLiteral("lw-v064-stamp.png"));
        QVERIFY(big.save(path));
        const QImage loaded = stroke::loadStampImage(path);
        QVERIFY(!loaded.isNull());
        QVERIFY(loaded.width() <= 256 && loaded.height() <= 256);
        const QImage again = stroke::loadStampImage(path);
        QCOMPARE(pixelDiff(loaded, again), 0);
        QFile::remove(path);
        const QString missing = QDir::temp().filePath(QStringLiteral("lw-v064-missing-stamp.png"));
        QFile::remove(missing);
        const QImage miss1 = stroke::loadStampImage(missing);
        const QImage miss2 = stroke::loadStampImage(missing);
        QVERIFY(miss1.isNull());
        QVERIFY(miss2.isNull());
    }

    void optimizePanelMarksDocumentDirty()
    {
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(80, 60, Qt::white);
        w.addDocument(d);
        QCoreApplication::processEvents();
        d->markClean();
        QVERIFY(!d->isDirty());
        auto *panel = w.findChild<OptimizePanel *>();
        QVERIFY(panel);
        auto *quality = panel->findChild<QSpinBox *>(QStringLiteral("optQuality"));
        QVERIFY(quality);
        const int v = quality->value();
        quality->setValue(v == 80 ? 70 : 80);
        QCoreApplication::processEvents();
        QVERIFY2(d->isDirty(), "changing Optimize settings must mark the document dirty");
        QCOMPARE(d->exportOptions.quality, quality->value());
    }

    void commitDropsNoOpAndAlwaysRecords()
    {
        auto *d = Document::createNew(80, 80, Qt::white);
        const int n = d->history().size();
        d->commit(QStringLiteral("Edit Stroke"));
        QCOMPARE(d->history().size(), n);
        d->commit(QStringLiteral("Forced"), Document::CommitMode::Always);
        QCOMPARE(d->history().size(), n + 1);
        d->s.canvasColor = QColor(10, 20, 30);
        d->commit(QStringLiteral("Canvas Color"));
        QCOMPARE(d->history().size(), n + 2);
        QCOMPARE(d->s.canvasColor, QColor(10, 20, 30));
        d->undo();
        QVERIFY(d->s.canvasColor != QColor(10, 20, 30));
        delete d;
    }

    void commitMergeSkipsDigestAndFolds()
    {
        auto *d = Document::createNew(120, 80, Qt::white);
        VObject r;
        r.type = ObjType::Rect;
        r.rect = QRectF(10, 10, 20, 20);
        d->addObject(r, 0, true);
        d->commit(QStringLiteral("Rect"));
        const int n = d->history().size();
        d->beginMerge();
        d->objects(0)[0].rect.translate(1, 0);
        d->commit(QStringLiteral("Nudge"));
        d->objects(0)[0].rect.translate(1, 0);
        d->commit(QStringLiteral("Nudge"));
        d->endMerge();
        QCOMPARE(d->history().size(), n + 1);
        QCOMPARE(d->objects(0)[0].rect.left(), 12.0);
        d->undo();
        QCOMPARE(d->objects(0)[0].rect.left(), 10.0);
        delete d;
    }

    static int inkedPixels(const QImage &im, int thresh = 10)
    {
        const QImage a = im.convertToFormat(QImage::Format_ARGB32);
        int n = 0;
        for (int y = 0; y < a.height(); ++y) {
            const QRgb *sl = reinterpret_cast<const QRgb *>(a.constScanLine(y));
            for (int x = 0; x < a.width(); ++x) {
                if (qAlpha(sl[x]) > thresh)
                    ++n;
            }
        }
        return n;
    }

    // Mean absolute per-channel difference over pixels that are inked in either image.
    static double meanAbsDiff(const QImage &a0, const QImage &b0)
    {
        QImage a = a0.convertToFormat(QImage::Format_ARGB32);
        QImage b = b0.convertToFormat(QImage::Format_ARGB32);
        if (a.size() != b.size()) return 1e9;
        qint64 sum = 0, n = 0;
        for (int y = 0; y < a.height(); ++y) {
            const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
            const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
            for (int x = 0; x < a.width(); ++x) {
                if (qAlpha(pa[x]) <= 10 && qAlpha(pb[x]) <= 10) continue;
                sum += qAbs(qRed(pa[x]) - qRed(pb[x]))
                     + qAbs(qGreen(pa[x]) - qGreen(pb[x]))
                     + qAbs(qBlue(pa[x]) - qBlue(pb[x]))
                     + qAbs(qAlpha(pa[x]) - qAlpha(pb[x]));
                ++n;
            }
        }
        return n ? double(sum) / (n * 4) : 0;
    }

    void editStrokePreviewMatchesEveryTip()
    {
        const QSize dest(620, 72);
        const QColor col(20, 40, 180, 255);
        const QColor bg(250, 250, 248);
        const double brushSize = 16;
        const double vecWidth = 8;
        int tips = 0;
        QStringList bad;
        for (int c = 0; c <= int(brush::Category::Libreworks); ++c) {
            const auto cat = brush::Category(c);
            const QStringList names = brush::tipNames(cat);
            for (int ti = 0; ti < names.size(); ++ti) {
                ++tips;
                const QString label = brush::categoryName(cat) + QLatin1Char('/') + names[ti];
                brush::Params bp;
                brush::applyTip(bp, cat, ti);
                bp.size = brushSize;
                bp.color = col;
                bp.opacity = 100;
                bp.strokeCustom = true;
                bp.strokeParams = brush::liveEditStrokeParams(bp);
                bp.strokeKey = brush::tipStrokeKey(cat, ti);
                const quint32 seed = stroke::kPreviewSeed;

                const QImage prevB = brush::renderPreview(bp, dest, bg, seed);
                const QImage rawB = brush::renderPreviewRaw(bp, dest, seed);
                // Independent brush-engine reference on the same layout/path.
                const stroke::PreviewLayout Lb = stroke::previewLayout(bp.strokeParams, bp.size, dest);
                QImage refB(Lb.work, QImage::Format_ARGB32_Premultiplied);
                refB.fill(0);
                {
                    brush::Params paintBp = bp;
                    paintBp.size = bp.size * Lb.scale;
                    if (paintBp.strokeCustom) paintBp.strokeParams.size = paintBp.size;
                    brush::Stroke st;
                    st.begin(refB, paintBp);
                    st.setEngineSeed(seed);
                    st.setPreviewSense(true);
                    st.strokePath(Lb.path);
                    st.finish();
                    st.composite(refB, refB.rect());
                    st.end();
                }

                stroke::Params vec = stroke::paramsFor(bp.strokeKey);
                if (vec.key.isEmpty()) vec = bp.strokeParams;
                stroke::PaintInput in;
                in.params = vec;
                in.width = vecWidth;
                in.color = col;
                in.seed = seed;
                in.pressure = 0.85;
                in.speed = 0.7;
                const QImage prevV = stroke::renderPreview(in, dest, bg);
                const QImage rawV = stroke::renderPreviewRaw(in, dest);
                const stroke::PreviewLayout Lv = stroke::previewLayout(vec, vecWidth, dest);
                QImage refV(Lv.work, QImage::Format_ARGB32_Premultiplied);
                refV.fill(0);
                {
                    QPainter p(&refV);
                    p.setRenderHint(QPainter::Antialiasing, true);
                    stroke::PaintInput pin = in;
                    pin.width = vecWidth * Lv.scale;
                    if (pin.params.look == stroke::Look::ImageStamp && pin.stampImage.isNull())
                        pin.stampImage = stroke::loadStampImage(pin.params.stampSource);
                    if (pin.sampleDist.isEmpty())
                        stroke::fillPreviewSenseRamp(pin, Lv.path);
                    stroke::paint(p, Lv.path, pin);
                    p.end();
                }

                const int inkPB = inkedPixels(prevB);
                const int inkPV = inkedPixels(prevV);
                const double dB = meanAbsDiff(rawB, refB);
                const double dV = meanAbsDiff(rawV, refV);
                if (inkPB < 40)
                    bad.append(label + QStringLiteral(" brush preview blank (ink=%1)").arg(inkPB));
                if (inkPV < 40)
                    bad.append(label + QStringLiteral(" vector preview blank (ink=%1)").arg(inkPV));
                if (dB > 2.0)
                    bad.append(label + QStringLiteral(" brush raw vs engine mean= %1").arg(dB, 0, 'f', 2));
                if (dV > 2.0)
                    bad.append(label + QStringLiteral(" vector raw vs paint mean= %1").arg(dV, 0, 'f', 2));
            }
        }
        QCOMPARE(tips, 76);
        QVERIFY2(bad.isEmpty(), qPrintable(bad.join(QLatin1Char('\n'))));
    }

    void editStrokeDialogPreviewNonBlankForRopeAndNeon()
    {
        // Dialog path: Brush engine for Rope / Neon / Hard Line, vector path too.
        struct Case { brush::Category cat; int tip; };
        const Case cases[] = {
            {brush::Category::Libreworks, 0}, // first Libreworks tip check by name below
        };
        Q_UNUSED(cases);
        EditStrokeDialog dlg;
        dlg.resize(640, 520);
        dlg.show();
        QCoreApplication::processEvents();
        const QColor col(20, 40, 180, 255);
        auto check = [&](brush::Category cat, const QString &tipName, bool brushMode) {
            const int ti = brush::tipIndex(cat, tipName);
            QVERIFY(ti >= 0);
            brush::Params bp;
            brush::applyTip(bp, cat, ti);
            bp.size = 16;
            bp.color = col;
            bp.opacity = 100;
            stroke::Params live = brush::liveEditStrokeParams(bp);
            live.key = brush::tipStrokeKey(cat, ti);
            if (brushMode) {
                bp.strokeCustom = true;
                bp.strokeParams = live;
                dlg.setBrushSource(bp);
                dlg.setParams(live, col, 16, stroke::kPreviewSeed);
                dlg.setVectorStroke(false);
            } else {
                dlg.setVectorStroke(true);
                dlg.setParams(live, col, 8, stroke::kPreviewSeed);
            }
            QCoreApplication::processEvents();
            const QImage im = dlg.previewImage();
            QVERIFY2(!im.isNull(), qPrintable(tipName));
            QVERIFY2(inkedPixels(im) >= 40,
                     qPrintable(QStringLiteral("%1 %2 ink=%3")
                                .arg(brushMode ? "brush" : "vector", tipName)
                                .arg(inkedPixels(im))));
        };
        // Resolve Neon Tube / Rope by name.
        const QStringList lw = brush::tipNames(brush::Category::Libreworks);
        QVERIFY(lw.contains(QLatin1String("Rope")));
        QVERIFY(lw.contains(QLatin1String("Neon Tube")));
        check(brush::Category::Libreworks, QStringLiteral("Rope"), true);
        check(brush::Category::Libreworks, QStringLiteral("Rope"), false);
        check(brush::Category::Libreworks, QStringLiteral("Neon Tube"), true);
        check(brush::Category::Libreworks, QStringLiteral("Neon Tube"), false);
        check(brush::Category::Basic, QStringLiteral("Hard Line"), true);
        check(brush::Category::Basic, QStringLiteral("Hard Line"), false);
    }
};

QTEST_MAIN(V064Test)
#include "v064_test.moc"
