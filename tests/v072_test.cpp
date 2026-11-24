// 0.3.34: Text PI font combo sync after load, Edit Stroke preview background
// toggle, Heart auto shape, right-dock panel splitters, Healing Brush audit.
#include "core/AutoShape.h"
#include "core/Document.h"
#include "core/Object.h"
#include "core/Render.h"
#include "core/Retouch.h"
#include "core/Stroke.h"
#include "io/DocIO.h"
#include "io/LwdFormat.h"
#include "io/Zip.h"
#include "ui/AppState.h"
#include "ui/EditStrokeDialog.h"
#include "ui/MainWindow.h"
#include "ui/Panels.h"
#include "ui/PropertyInspector.h"
#include "ui/WideFontCombo.h"
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

class V072Test : public QObject {
    Q_OBJECT

    static void drain() { QCoreApplication::processEvents(); }

    static QString proofDir()
    {
        const QString d = QDir(QStringLiteral("/workspace/review-0.3.34")).exists()
            ? QStringLiteral("/workspace/review-0.3.34")
            : QDir::temp().filePath(QStringLiteral("lw-heal-proof"));
        QDir().mkpath(d);
        return d;
    }

    static VObject makeText(Document *d, const QString &text, const QRectF &r,
                            const QString &family, double size = 24, bool bold = false)
    {
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Text;
        o.text = text;
        o.rect = r;
        o.fontFamily = family;
        o.fontSize = size;
        o.bold = bold;
        return o;
    }

    static int lwdVersion(const QByteArray &data)
    {
        QMap<QString, QByteArray> files;
        if (!zip::read(data, files, nullptr)) return -1;
        const QJsonObject root = QJsonDocument::fromJson(files.value(QStringLiteral("document.json"))).object();
        return root.value(QStringLiteral("version")).toInt(-1);
    }

    static QString itemLabelForFamily(const WideFontComboBox *box, const QString &family)
    {
        for (int i = 0; i < box->count(); ++i) {
            const QString role = box->itemData(i, Qt::UserRole).toString();
            const QString text = box->itemText(i);
            if (role.compare(family, Qt::CaseInsensitive) == 0
                || text.compare(family, Qt::CaseInsensitive) == 0
                || text.startsWith(family + QStringLiteral(" (missing)"), Qt::CaseInsensitive))
                return text;
        }
        return {};
    }

private slots:
    void cleanup()
    {
        Prefs p = appState().prefs;
        p.strokePreviewBg = 2;
        p.useSystemFonts = true;
        p.useCustomFonts = false;
        p.customFontDir.clear();
        appState().setPrefs(p);
        appState().savePrefs();
    }

    void fontComboSyncsAfterLoadAndSelection()
    {
        QString sysA, sysB;
        for (const QString &f : QFontDatabase::families()) {
            if (f.isEmpty() || f.contains(QLatin1Char('['))) continue;
            if (f.compare(QStringLiteral("Sans Serif"), Qt::CaseInsensitive) == 0) continue;
            if (sysA.isEmpty()) sysA = f;
            else if (f.compare(sysA, Qt::CaseInsensitive) != 0) { sysB = f; break; }
        }
        QVERIFY(!sysA.isEmpty());
        QVERIFY(!sysB.isEmpty());

        std::unique_ptr<Document> src(Document::createNew(400, 300, Qt::white));
        VObject a = makeText(src.get(), QStringLiteral("Alpha"), QRectF(20, 20, 200, 40), sysA, 18, false);
        VObject b = makeText(src.get(), QStringLiteral("Beta"), QRectF(20, 80, 200, 40), sysB, 32, true);
        const quint64 idA = a.id, idB = b.id;
        src->addObject(a);
        src->addObject(b);
        src->commit(QStringLiteral("text"));

        QTemporaryDir tmp;
        const QString lwdPath = tmp.filePath(QStringLiteral("fonts.lwd"));
        const QString bonPath = tmp.filePath(QStringLiteral("fonts.bon"));
        QString err;
        QVERIFY2(lwd::save(*src, lwdPath, &err), qPrintable(err));
        QVERIFY2(lwd::save(*src, bonPath, &err), qPrintable(err));
        {
            QFile f(lwdPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(lwdVersion(f.readAll()), 7);
        }

        auto checkFile = [&](const QString &path, const char *tag) {
            MainWindow w;
            w.show();
            QVERIFY(QTest::qWaitForWindowExposed(&w));
            QVERIFY2(w.openFile(path), tag);
            drain();
            Document *doc = w.currentDoc();
            QVERIFY2(doc, tag);
            auto *piFont = w.findChild<WideFontComboBox *>(QStringLiteral("piFont"));
            QVERIFY2(piFont, tag);
            auto *size = w.findChild<QDoubleSpinBox *>(QStringLiteral("fontSize"));
            QVERIFY2(size, tag);

            doc->selectOnly(idA);
            drain();
            QCOMPARE(piFont->currentFamily(), sysA);
            QCOMPARE(piFont->currentFont().family(), sysA);
            QCOMPARE(size->value(), 18.0);

            doc->selectOnly(idB);
            drain();
            QCOMPARE(piFont->currentFamily(), sysB);
            QCOMPARE(piFont->currentFont().family(), sysB);
            QCOMPARE(size->value(), 32.0);

            doc->selectOnly(idA);
            drain();
            QCOMPARE(piFont->currentFamily(), sysA);
        };
        checkFile(lwdPath, "lwd");
        checkFile(bonPath, "bon");
    }

    void fontComboCustomOnlyAndMissing()
    {
        QTemporaryDir fontdir;
        const QString srcDir = QStringLiteral(LW_SOURCE_DIR "/tests/data/fonts");
        QVERIFY(QFile::copy(srcDir + QStringLiteral("/LwCustomTest.ttf"),
                            fontdir.filePath(QStringLiteral("LwCustomTest.ttf"))));
        const QString custom = QStringLiteral("LwCustomTest");
        const QString missing = QStringLiteral("DefinitelyMissingFontXYZ");

        Prefs p = appState().prefs;
        p.useSystemFonts = false;
        p.useCustomFonts = true;
        p.customFontDir = fontdir.path();
        appState().setPrefs(p);
        drain();
        QVERIFY(appState().fontSourceFamilies().contains(custom, Qt::CaseInsensitive));

        std::unique_ptr<Document> src(Document::createNew(400, 300, Qt::white));
        VObject ok = makeText(src.get(), QStringLiteral("Custom"), QRectF(20, 20, 200, 40), custom, 22);
        VObject miss = makeText(src.get(), QStringLiteral("Missing"), QRectF(20, 80, 200, 40), missing, 20);
        const quint64 idOk = ok.id, idMiss = miss.id;
        src->addObject(ok);
        src->addObject(miss);
        src->commit(QStringLiteral("text"));
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("custom.lwd"));
        QString err;
        QVERIFY2(lwd::save(*src, path, &err), qPrintable(err));

        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openFile(path));
        drain();
        Document *doc = w.currentDoc();
        QVERIFY(doc);
        auto *piFont = w.findChild<WideFontComboBox *>(QStringLiteral("piFont"));
        QVERIFY(piFont);

        doc->selectOnly(idOk);
        drain();
        QCOMPARE(piFont->currentFamily(), custom);
        QVERIFY(!itemLabelForFamily(piFont, custom).contains(QStringLiteral("(missing)")));

        doc->selectOnly(idMiss);
        drain();
        QCOMPARE(piFont->currentFamily(), missing);
        const QString label = itemLabelForFamily(piFont, missing);
        QCOMPARE(label, missing); // item text stays the real family so findText still works
        QVERIFY2(piFont->itemData(piFont->currentIndex(), Qt::ToolTipRole).toString().contains(QStringLiteral("missing")),
                 qPrintable(piFont->itemData(piFont->currentIndex(), Qt::ToolTipRole).toString()));
        const QFont itemFont = piFont->itemData(piFont->currentIndex(), Qt::FontRole).value<QFont>();
        QVERIFY(itemFont.italic());
        QVERIFY(!piFont->currentFont().italic()); // must not leak the missing mark into the object
        // the stored family on the object must stay the real name, not the label
        QCOMPARE(doc->find(idMiss)->fontFamily, missing);
    }

    void genericSansSerifSyncsAfterLoad()
    {
        std::unique_ptr<Document> src(Document::createNew(300, 200, Qt::white));
        VObject t = makeText(src.get(), QStringLiteral("Hello"), QRectF(10, 10, 200, 40),
                             QStringLiteral("Sans Serif"), 24);
        const quint64 id = t.id;
        src->addObject(t);
        src->commit(QStringLiteral("text"));
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("sans.lwd"));
        QString err;
        QVERIFY2(lwd::save(*src, path, &err), qPrintable(err));

        MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openFile(path));
        drain();
        w.currentDoc()->selectOnly(id);
        drain();
        auto *piFont = w.findChild<WideFontComboBox *>(QStringLiteral("piFont"));
        QVERIFY(piFont);
        QCOMPARE(piFont->currentFamily(), QStringLiteral("Sans Serif"));
    }

    void editStrokePreviewBackgroundToggle()
    {
        QCOMPARE(Prefs().strokePreviewBg, 2);
        QCOMPARE(EditStrokeDialog::previewBackgroundFor(0, Qt::black), QColor(250, 250, 248));
        QCOMPARE(EditStrokeDialog::previewBackgroundFor(1, Qt::white), QColor(20, 20, 20));
        QCOMPARE(EditStrokeDialog::previewBackgroundFor(2, QColor(250, 250, 250)), QColor(20, 20, 20));
        QCOMPARE(EditStrokeDialog::previewBackgroundFor(2, QColor(20, 20, 20)), QColor(250, 250, 248));

        // Default is Auto: light strokes get a black preview, dark strokes get white.
        appState().prefs.strokePreviewBg = 2;
        appState().savePrefs();
        EditStrokeDialog dlg;
        stroke::Params sp;
        dlg.setParams(sp, QColor(255, 255, 240), 8);
        dlg.resize(480, 420);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        drain();
        QCOMPARE(dlg.previewBackgroundMode(), 2);
        QCOMPARE(dlg.previewBackground(), QColor(20, 20, 20));
        auto *btn = dlg.findChild<QToolButton *>(QStringLiteral("editStrokePreviewBg"));
        QVERIFY(btn);

        dlg.setParams(sp, QColor(10, 10, 40), 8);
        drain();
        QCOMPARE(dlg.previewBackgroundMode(), 2);
        QCOMPARE(dlg.previewBackground(), QColor(250, 250, 248));

        // Click cycles Auto -> White -> Black -> Auto and persists the choice.
        QTest::mouseClick(btn, Qt::LeftButton);
        drain();
        QCOMPARE(dlg.previewBackgroundMode(), 0);
        QCOMPARE(dlg.previewBackground(), QColor(250, 250, 248));
        QCOMPARE(appState().prefs.strokePreviewBg, 0);

        QTest::mouseClick(btn, Qt::LeftButton);
        drain();
        QCOMPARE(dlg.previewBackgroundMode(), 1);
        QCOMPARE(dlg.previewBackground(), QColor(20, 20, 20));
        QCOMPARE(appState().prefs.strokePreviewBg, 1);

        // A newly opened dialog follows the persisted manual choice (Black).
        EditStrokeDialog blackDlg;
        blackDlg.setParams(sp, QColor(255, 255, 240), 8);
        blackDlg.show();
        drain();
        QCOMPARE(blackDlg.previewBackgroundMode(), 1);
        QCOMPARE(blackDlg.previewBackground(), QColor(20, 20, 20));

        QTest::mouseClick(btn, Qt::LeftButton);
        drain();
        QCOMPARE(dlg.previewBackgroundMode(), 2);
        QCOMPARE(appState().prefs.strokePreviewBg, 2);

        EditStrokeDialog again;
        again.setParams(sp, QColor(255, 255, 240), 8);
        again.show();
        drain();
        QCOMPARE(again.previewBackgroundMode(), 2);
        QCOMPARE(again.previewBackground(), QColor(20, 20, 20));

        // Lightning is mostly white even when the palette colour is dark: Auto
        // must choose a black background from the rendered pixels, not the palette.
        appState().prefs.strokePreviewBg = 2;
        appState().savePrefs();
        const stroke::Params lightning = stroke::paramsFor(QStringLiteral("lw.lightning"));
        QVERIFY(lightning.look == stroke::Look::Lightning);
        EditStrokeDialog bolt;
        bolt.setParams(lightning, QColor(20, 20, 40), 16);
        bolt.resize(480, 420);
        bolt.show();
        QVERIFY(QTest::qWaitForWindowExposed(&bolt));
        drain();
        QCOMPARE(bolt.previewBackgroundMode(), 2);
        QCOMPARE(bolt.previewBackground(), QColor(20, 20, 20));
        const QImage preview = bolt.previewImage();
        QVERIFY(!preview.isNull());
        auto corner = preview.pixelColor(2, 2);
        QVERIFY2(corner.lightness() < 60,
                 qPrintable(QStringLiteral("Lightning Auto preview should be on black, got lightness %1")
                                .arg(corner.lightness())));
    }

    void heartAutoShapeLwdAndSvg()
    {
        QVERIFY(autoshape::isKind(QStringLiteral("heart")));
        QCOMPARE(autoshape::label(QStringLiteral("heart")), QStringLiteral("Heart"));
        QVERIFY(autoshape::kinds().contains(QStringLiteral("heart")));
        QCOMPARE(toolAutoShapeKind(Tool::AsHeart), QStringLiteral("heart"));
        QCOMPARE(autoShapeTool(QStringLiteral("heart")), Tool::AsHeart);
        QVERIFY(toolIsAutoShape(Tool::AsHeart));
        QVERIFY(toolGroup(Tool::Rect).contains(Tool::AsHeart));

        VObject o;
        autoshape::init(o, QStringLiteral("heart"));
        o.rect = QRectF(20, 20, 120, 110);
        o.fillOn = true;
        o.fill = QColor(200, 30, 50);
        o.strokeOn = false;
        QCOMPARE(o.shapeParams.value(QStringLiteral("cleft")), 0.35);
        QCOMPARE(o.shapeParams.value(QStringLiteral("point")), 0.85);
        const QPainterPath defPath = autoshape::outline(o);
        QVERIFY(!defPath.isEmpty());
        QVERIFY(defPath.boundingRect().isValid());

        const auto hs = autoshape::handles(o);
        QCOMPARE(hs.size(), 2);
        autoshape::dragHandle(o, 0, hs[0].pos + QPointF(0, 20), false);
        QVERIFY(o.shapeParams.value(QStringLiteral("cleft")) > 0.35);
        autoshape::dragHandle(o, 1, QPointF(o.rect.center().x() + 10, o.rect.bottom() - 10), false);
        const QPainterPath after = autoshape::outline(o);
        QVERIFY(after != defPath);

        std::unique_ptr<Document> d(Document::createNew(240, 200, Qt::white));
        o.id = d->newId();
        d->addObject(o);
        d->commit(QStringLiteral("heart"));
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("heart.lwd"));
        QString err;
        QVERIFY2(lwd::save(*d, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray data = f.readAll();
        QCOMPARE(lwdVersion(data), 7);
        QMap<QString, QByteArray> files;
        QVERIFY(zip::read(data, files, nullptr));
        const QJsonObject root = QJsonDocument::fromJson(files.value(QStringLiteral("document.json"))).object();
        QCOMPARE(root.value(QStringLiteral("version")).toInt(), 7);
        const QString json = QString::fromUtf8(files.value(QStringLiteral("document.json")));
        QVERIFY(json.contains(QStringLiteral("\"shape\": \"heart\"")) || json.contains(QStringLiteral("\"shape\":\"heart\"")));
        QVERIFY(json.contains(QStringLiteral("cleft")));
        QVERIFY(json.contains(QStringLiteral("point")));

        QString lerr;
        std::unique_ptr<Document> loaded(lwd::load(path, &lerr));
        QVERIFY2(loaded, qPrintable(lerr));
        const VObject *got = nullptr;
        for (VObject &obj : loaded->objects(0))
            if (obj.type == ObjType::AutoShape && obj.shapeKind == QLatin1String("heart")) got = &obj;
        QVERIFY(got);
        QCOMPARE(got->shapeParams.value(QStringLiteral("cleft")), o.shapeParams.value(QStringLiteral("cleft")));
        QCOMPARE(got->shapeParams.value(QStringLiteral("point")), o.shapeParams.value(QStringLiteral("point")));

        const QByteArray svg = docio::stateToSvg(d->s, 0);
        const QString sv = QString::fromUtf8(svg);
        QVERIFY(sv.contains(QStringLiteral("lw:shape=\"heart\"")));
        QVERIFY(sv.contains(QStringLiteral("lw:cleft=")));
        QVERIFY(sv.contains(QStringLiteral("lw:point=")));
        DocState st;
        quint64 next = 1;
        QString serr;
        QVERIFY2(docio::svgToState(svg, st, [&] { return next++; }, QString(), nullptr, &serr), qPrintable(serr));
        QVector<VObject> all;
        for (const auto &L : st.frames[0].layers) all += L;
        QCOMPARE(all.size(), 1);
        QCOMPARE(all[0].shapeKind, QStringLiteral("heart"));

        // Default heart is left-right symmetric and a classic silhouette (lobes, dip, tip).
        {
            VObject dflt;
            autoshape::init(dflt, QStringLiteral("heart"));
            dflt.rect = QRectF(0, 0, 200, 200);
            const QPainterPath path = autoshape::outline(dflt);
            QVERIFY(!path.isEmpty());
            const QPainterPath mirrored = QTransform(-1, 0, 0, 1, 200, 0).map(path);
            QImage a(200, 200, QImage::Format_ARGB32);
            a.fill(0);
            QImage b = a;
            { QPainter pa(&a); pa.setRenderHint(QPainter::Antialiasing); pa.setPen(Qt::NoPen); pa.setBrush(Qt::black); pa.drawPath(path); }
            { QPainter pb(&b); pb.setRenderHint(QPainter::Antialiasing); pb.setPen(Qt::NoPen); pb.setBrush(Qt::black); pb.drawPath(mirrored); }
            int diff = 0;
            for (int y = 0; y < 200; ++y)
                for (int x = 0; x < 200; ++x)
                    if (qAbs(int(qAlpha(a.pixel(x, y))) - int(qAlpha(b.pixel(x, y)))) > 30) ++diff;
            QVERIFY2(diff < 250, qPrintable(QStringLiteral("heart not symmetric: %1 pixels").arg(diff)));
            QVERIFY(path.contains(QPointF(44, 48)));   // left lobe
            QVERIFY(path.contains(QPointF(156, 48)));  // right lobe
            QVERIFY(!path.contains(QPointF(100, 20))); // in the cleft, above the dip
            QVERIFY(!path.contains(QPointF(100, 48))); // still in the dip; lobes bulge above it
            QVERIFY(path.contains(QPointF(100, 170))); // lower body toward the tip
            // Mid-join is G1: last control of the first cubic, the join, and the
            // first control of the second cubic are collinear (classic 2-cubic heart).
            QVERIFY(path.elementCount() >= 7);
            const auto linEl = path.elementAt(2);
            const auto joinEl = path.elementAt(3);
            const auto loutEl = path.elementAt(4);
            const QPointF vIn(joinEl.x - linEl.x, joinEl.y - linEl.y);
            const QPointF vOut(loutEl.x - joinEl.x, loutEl.y - joinEl.y);
            const double cross = vIn.x() * vOut.y() - vIn.y() * vOut.x();
            const double scale = vIn.manhattanLength() * vOut.manhattanLength();
            QVERIFY2(qAbs(cross) <= 1e-6 * qMax(1.0, scale),
                     qPrintable(QStringLiteral("heart mid-join not G1, cross=%1").arg(cross)));
            // Sides must curve, not run vertically down the box edge (the previous
            // waist-with-vertical-tangent outline looked like a tooth).
            int vrun = 0;
            for (int y = 0; y < 200; ++y) {
                int left = 200;
                for (int x = 0; x < 200; ++x)
                    if (qAlpha(a.pixel(x, y)) > 128) { left = x; break; }
                if (left <= 2) ++vrun;
            }
            QVERIFY2(vrun < 12, qPrintable(QStringLiteral("heart has a vertical side run: %1 rows").arg(vrun)));
            // Handle extremes stay a symmetric heart and keep a cleft gap at the top.
            for (const auto &pr : {QPair<double, double>(0.0, 0.0), {0.0, 1.0}, {1.0, 0.0}, {1.0, 1.0}}) {
                VObject ext;
                autoshape::init(ext, QStringLiteral("heart"));
                ext.rect = QRectF(0, 0, 200, 200);
                ext.shapeParams[QStringLiteral("cleft")] = pr.first;
                ext.shapeParams[QStringLiteral("point")] = pr.second;
                const QPainterPath ep = autoshape::outline(ext);
                QVERIFY(!ep.isEmpty());
                QVERIFY(!ep.contains(QPointF(100, 20)));
                const QPainterPath em = QTransform(-1, 0, 0, 1, 200, 0).map(ep);
                QImage ea(200, 200, QImage::Format_ARGB32);
                ea.fill(0);
                QImage eb = ea;
                { QPainter pa(&ea); pa.setRenderHint(QPainter::Antialiasing); pa.setPen(Qt::NoPen); pa.setBrush(Qt::black); pa.drawPath(ep); }
                { QPainter pb(&eb); pb.setRenderHint(QPainter::Antialiasing); pb.setPen(Qt::NoPen); pb.setBrush(Qt::black); pb.drawPath(em); }
                int ediff = 0;
                for (int y = 0; y < 200; ++y)
                    for (int x = 0; x < 200; ++x)
                        if (qAbs(int(qAlpha(ea.pixel(x, y))) - int(qAlpha(eb.pixel(x, y)))) > 30) ++ediff;
                QVERIFY2(ediff < 250, qPrintable(QStringLiteral("heart extreme %1,%2 not symmetric: %3")
                                                     .arg(pr.first).arg(pr.second).arg(ediff)));
            }
        }
    }

    void rightPanelSplittersResizeAndPersist()
    {
        QLabel *aBody = new QLabel(QStringLiteral("A"));
        QLabel *bBody = new QLabel(QStringLiteral("B"));
        QLabel *cBody = new QLabel(QStringLiteral("C"));
        PanelStack stack;
        auto *a = new CollapsiblePanel(QStringLiteral("Optimize"), aBody, &stack);
        auto *b = new CollapsiblePanel(QStringLiteral("Layers"), bBody, &stack);
        auto *c = new CollapsiblePanel(QStringLiteral("History"), cBody, &stack);
        stack.addPanel(a, 0);
        stack.addPanel(b, 3);
        stack.addPanel(c, 1);
        a->setContentSized(true);
        a->setCollapsed(false);
        b->setCollapsed(false);
        c->setCollapsed(true);
        stack.resize(280, 700);
        stack.show();
        QVERIFY(QTest::qWaitForWindowExposed(&stack));
        drain();

        // Optimize is content-sized: no handle resizes it. Layers still has a handle.
        QCOMPARE(stack.visibleHandleCount(), 1);
        QVERIFY(!stack.dragBetween(a, 40));

        const int hb = stack.storedHeight(b);
        const int hc = stack.storedHeight(c);
        QVERIFY(stack.dragBetween(b, 40));
        QCOMPARE(stack.storedHeight(b), hb + 40);
        QCOMPARE(stack.storedHeight(c), hc); // push-down keeps panels below at their own height

        // min-height clamp: shrinking past the minimum is rejected or pinned
        QVERIFY(stack.dragBetween(b, -4000) == false || stack.storedHeight(b) >= b->headerHeight() + 48);

        // collapsing Layers hides its handle
        b->setCollapsed(true);
        drain();
        QCOMPARE(stack.visibleHandleCount(), 0);

        QTemporaryDir tmp;
        QSettings st(tmp.filePath(QStringLiteral("ws.ini")), QSettings::IniFormat);
        a->setCollapsed(false);
        b->setCollapsed(false);
        c->setCollapsed(true);
        drain();
        QCOMPARE(stack.visibleHandleCount(), 1);
        const int beforeB = stack.storedHeight(b);
        QVERIFY(stack.dragBetween(b, 30));
        QCOMPARE(stack.storedHeight(b), beforeB + 30);
        const int sa = stack.storedHeight(a), sb = stack.storedHeight(b);
        stack.saveLayout(st);
        st.sync();

        PanelStack stack2;
        auto *a2 = new CollapsiblePanel(QStringLiteral("Optimize"), new QLabel(QStringLiteral("A")), &stack2);
        auto *b2 = new CollapsiblePanel(QStringLiteral("Layers"), new QLabel(QStringLiteral("B")), &stack2);
        auto *c2 = new CollapsiblePanel(QStringLiteral("History"), new QLabel(QStringLiteral("C")), &stack2);
        stack2.addPanel(a2, 0);
        stack2.addPanel(b2, 3);
        stack2.addPanel(c2, 1);
        a2->setContentSized(true);
        stack2.restoreLayout(st);
        QCOMPARE(stack2.storedHeight(b2), sb);
        QVERIFY(!a2->isCollapsed());
        QVERIFY(!b2->isCollapsed());
        QVERIFY(c2->isCollapsed());
        Q_UNUSED(sa);
    }

    void healingMatchesDestinationLightingNotClone()
    {
        // Destination: a left-to-right lighting ramp with a dark blemish in the middle.
        // Source: a high-frequency checker (texture) on a mid-grey field, same size.
        QImage dest(160, 80, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < dest.height(); ++y)
            for (int x = 0; x < dest.width(); ++x) {
                const int v = 40 + x * 180 / (dest.width() - 1);
                dest.setPixel(x, y, qRgb(v, v / 2, v / 3));
            }
        const QRect blot(68, 28, 24, 24);
        QPainter pd(&dest);
        pd.fillRect(blot, QColor(20, 10, 8));
        pd.end();
        const QImage before = dest.copy();

        QImage src(160, 80, QImage::Format_ARGB32_Premultiplied);
        src.fill(QColor(180, 40, 40)); // strongly red — a clone would stay red
        QPainter ps(&src);
        ps.setPen(Qt::NoPen);
        for (int y = 0; y < src.height(); y += 4)
            for (int x = 0; x < src.width(); x += 4)
                if (((x / 4) + (y / 4)) & 1)
                    ps.fillRect(x, y, 2, 2, QColor(80, 20, 20));
        ps.end();

        QImage healed = dest;
        const QPointF c(80, 40);
        retouch::healDab(healed, src, c, QPointF(0, 0), 28, 40, retouch::HealMode::Normal);

        // Mean colour in the blot should stay close to the destination lighting (amber ramp),
        // not the red source.
        auto mean = [](const QImage &im, const QRect &r) {
            qint64 R = 0, G = 0, B = 0, n = 0;
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) {
                    const QRgb p = im.pixel(x, y);
                    R += qRed(p); G += qGreen(p); B += qBlue(p); ++n;
                }
            return n ? QColor(int(R / n), int(G / n), int(B / n)) : QColor();
        };
        const QRect core(74, 34, 12, 12);
        const QColor mh = mean(healed, core);
        const QColor md = mean(before, QRect(74, 8, 12, 12)); // dest lighting above the blot, same x
        QImage cloned = dest;
        retouch::stampDab(cloned, src, c, QPointF(0, 0), 28, 100, nullptr);
        const QColor mc = mean(cloned, core);
        QVERIFY2(std::abs(mh.red() - md.red()) < 40 && std::abs(mh.green() - md.green()) < 40,
                 qPrintable(QStringLiteral("lighting mismatch healed=%1 dest=%2").arg(mh.name(), md.name())));
        QVERIFY2(mh.red() < mc.red() - 30,
                 qPrintable(QStringLiteral("heal looks like a clone: healed=%1 clone=%2").arg(mh.name(), mc.name())));
        // A per-channel RGB Poisson turns dark-on-red dots green on brown skin.
        int greenish = 0;
        for (int y = core.top(); y <= core.bottom(); ++y)
            for (int x = core.left(); x <= core.right(); ++x) {
                const QRgb p = healed.pixel(x, y);
                if (qGreen(p) > qRed(p) + 8) ++greenish;
            }
        QVERIFY2(greenish == 0, qPrintable(QStringLiteral("healed dots went green (%1 pixels), mean %2")
                                               .arg(greenish).arg(mh.name())));
        // And it must actually change the blot (not a no-op / blur-only)
        int changed = 0;
        for (int y = blot.top(); y < blot.bottom(); ++y)
            for (int x = blot.left(); x < blot.right(); ++x)
                if (qAbs(qGray(healed.pixel(x, y)) - qGray(before.pixel(x, y))) > 8) ++changed;
        QVERIFY(changed > 40);

        const QString dir = proofDir();
        QVERIFY(before.save(dir + QStringLiteral("/heal-test-before.png")));
        QVERIFY(healed.save(dir + QStringLiteral("/heal-test-after.png")));
        QVERIFY(cloned.save(dir + QStringLiteral("/heal-test-clone.png")));
    }

    void spotHealingRemovesBlemishAndWorksAtEdges()
    {
        QImage img(120, 80, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(200, 180, 160));
        QPainter p(&img);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(30, 20, 15));
        p.drawEllipse(QPoint(60, 40), 6, 6);
        p.end();
        const QImage before = img.copy();
        retouch::spotHealDab(img, QPointF(60, 40), 22, 50, retouch::HealMode::Normal);
        // the blot should be gone, matching the surrounding beige
        const QRgb mid = img.pixel(60, 40);
        QVERIFY2(qRed(mid) > 140 && qGreen(mid) > 120,
                 qPrintable(QStringLiteral("spot left a dark pixel: %1").arg(QColor(mid).name())));

        // corner: must pick an in-image source (the old default offset could miss)
        QImage edge(80, 80, QImage::Format_ARGB32_Premultiplied);
        edge.fill(QColor(40, 80, 160));
        QPainter pe(&edge);
        pe.fillRect(0, 0, 10, 10, QColor(255, 0, 0));
        pe.end();
        const QPointF off = retouch::spotHealOffset(edge, QPointF(6, 6), 14);
        const QPointF src = QPointF(6, 6) + off;
        QVERIFY2(src.x() >= 0 && src.y() >= 0 && src.x() < edge.width() && src.y() < edge.height(),
                 qPrintable(QStringLiteral("spot offset off-image: %1,%2").arg(src.x()).arg(src.y())));
        retouch::spotHealDab(edge, QPointF(6, 6), 14, 40, retouch::HealMode::Normal);
        const QRgb c0 = edge.pixel(4, 4);
        QVERIFY2(qBlue(c0) > qRed(c0), qPrintable(QStringLiteral("corner not healed: %1").arg(QColor(c0).name())));

        const QString dir = proofDir();
        QVERIFY(before.save(dir + QStringLiteral("/spot-heal-test-before.png")));
        QVERIFY(img.save(dir + QStringLiteral("/spot-heal-test-after.png")));
    }

    void healingHandlesOffsetAlphaAndSrcOrigin()
    {
        // Source snapshot whose pixel (0,0) lives at document (40, 20) — srcOrigin.
        QImage canvas(100, 80, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(QColor(30, 80, 30));
        QPainter p(&canvas);
        p.fillRect(10, 10, 30, 30, QColor(30, 80, 30));
        p.end();
        QImage src = canvas.copy(QRect(40, 20, 40, 40));
        src.fill(QColor(200, 40, 40)); // red texture in the snapshot
        QImage dest(100, 80, QImage::Format_ARGB32_Premultiplied);
        dest.fill(QColor(30, 80, 30));
        QPainter pd(&dest);
        pd.fillRect(50, 30, 16, 16, QColor(0, 0, 0));
        pd.end();
        QImage healed = dest;
        // heal at (58, 38) from source whose origin is (40, 20) with offset (0,0) would be wrong;
        // pass srcOrigin so the source lines up with the dest.
        retouch::healDab(healed, src, QPointF(58, 38), QPointF(0, 0), 20, 30, retouch::HealMode::Normal, nullptr, QPoint(40, 20));
        // Without srcOrigin the red source would be sampled out of bounds (fallback to dest) or
        // misaligned; with it, lighting stays green and the blot changes.
        const QRgb mid = healed.pixel(58, 38);
        QVERIFY2(qGreen(mid) > qRed(mid), qPrintable(QStringLiteral("srcOrigin misaligned: %1").arg(QColor(mid).name())));

        // Alpha: opaque stain on a transparent canvas; heal from an opaque neighbour.
        QImage a(64, 64, QImage::Format_ARGB32_Premultiplied);
        a.fill(Qt::transparent);
        QPainter pa(&a);
        pa.fillRect(8, 8, 20, 20, QColor(200, 180, 160));
        pa.fillRect(30, 18, 10, 10, QColor(20, 10, 8));
        pa.end();
        retouch::healDab(a, a, QPointF(35, 23), QPointF(-16, 0), 14, 40, retouch::HealMode::Normal);
        const QRgb hp = a.pixel(35, 23);
        QVERIFY(qAlpha(hp) > 80);
        QVERIFY(qRed(hp) > 100);
    }
};

QTEST_MAIN(V072Test)
#include "v072_test.moc"
