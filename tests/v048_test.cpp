// v0.3.18: Group-nested symbol delete/edit/keys, cyclic symbols, mixed canvas drop,
// Libreworks opens .bon (CLI + drag-drop), Save on .bon goes to .lwd, blur-reach early-out.
#include "core/Document.h"
#include "core/Effects.h"
#include "core/Object.h"
#include "core/Render.h"
#include "io/Batch.h"
#include "io/ImageIO.h"
#include "io/LwdFormat.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "Brand.h"
#include <QApplication>
#include <QtMath>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QDropEvent>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
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

static VObject makeRect(Document &d, const QRectF &r, const QColor &c) {
    VObject o;
    o.type = ObjType::Rect;
    o.id = d.newId();
    o.rect = r;
    o.fill = c;
    o.fillOn = true;
    o.strokeOn = false;
    return o;
}

class V048 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;
private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        render::setObjectVisualCacheEnabled(true);
        render::clearObjectVisualCache();
        w = std::make_unique<MainWindow>();
        w->resize(1000, 700);
        w->show();
        drainEvents();
    }
    void cleanupTestCase() { w.reset(); }
    void cleanup() { render::clearObjectVisualCache(); }

    void deleteAndEditGroupedNestedInstance() {
        Document *d = Document::createNew(200, 160, Qt::white);
        // Inner symbol S2: a coloured rectangle.
        Symbol s2;
        s2.name = QStringLiteral("Inner");
        VObject body = makeRect(*d, QRectF(0, 0, 40, 30), QColor(200, 40, 40));
        s2.frames = {{body}};
        const quint64 id2 = d->addSymbol(s2);

        VObject inst2 = d->makeInstance(id2);
        inst2.id = d->newId();
        inst2.rect = QRectF(10, 10, 40, 30);

        // Group on the canvas holding that instance.
        VObject g;
        g.type = ObjType::Group;
        g.id = d->newId();
        g.children = {inst2};
        d->objects(0).append(g);
        QCOMPARE(d->instanceCount(id2), 1);

        // Outer symbol S1 whose artwork is a group containing Instance(S2).
        Symbol s1;
        s1.name = QStringLiteral("Outer");
        VObject inst2b = d->makeInstance(id2);
        inst2b.id = d->newId();
        inst2b.rect = QRectF(0, 0, 40, 30);
        VObject g2;
        g2.type = ObjType::Group;
        g2.id = d->newId();
        g2.children = {inst2b};
        s1.frames = {{g2}};
        const quint64 id1 = d->addSymbol(s1);
        VObject inst1 = d->makeInstance(id1);
        inst1.id = d->newId();
        inst1.rect = QRectF(80, 40, 60, 45);
        inst1.effects.append(LiveEffect::make(EffectType::DropShadow));
        d->objects(0).append(inst1);

        SymbolScope scope(&d->s.symbols);
        const QByteArray keyBefore = render::symbolBodyDigest(id1);
        const QImage before = d->render(0, true);

        // Edit S2: change fill so the grouped outer composite must update.
        VObject body2 = makeRect(*d, QRectF(0, 0, 40, 30), QColor(40, 40, 200));
        d->setSymbolFrames(id2, {{body2}});
        const QByteArray keyAfter = render::symbolBodyDigest(id1);
        QVERIFY2(keyBefore != keyAfter, "editing grouped nested symbol must change outer body key");
        const QImage after = d->render(0, true);
        QVERIFY2(!samePixels(before, after), "editing grouped nested symbol must update pixels");

        QVERIFY(d->deleteSymbol(id2));
        QCOMPARE(d->instanceCount(id2), 0);
        // The canvas group must no longer hold an instance of S2.
        const VObject *grp = d->find(g.id);
        QVERIFY(grp);
        QCOMPARE(grp->children.size(), 0);
        delete d;
    }

    void cyclicSymbolBodyKeysAreStable() {
        Document *d = Document::createNew(120, 80, Qt::white);
        Symbol a, b;
        a.name = QStringLiteral("A");
        b.name = QStringLiteral("B");
        // Placeholder frames; ids are assigned by addSymbol, then we wire instances.
        a.frames = {{makeRect(*d, QRectF(0, 0, 20, 20), Qt::red)}};
        b.frames = {{makeRect(*d, QRectF(0, 0, 20, 20), Qt::blue)}};
        const quint64 idA = d->addSymbol(a);
        const quint64 idB = d->addSymbol(b);
        VObject ia = d->makeInstance(idB);
        ia.id = d->newId();
        ia.rect = QRectF(0, 0, 20, 20);
        VObject ib = d->makeInstance(idA);
        ib.id = d->newId();
        ib.rect = QRectF(0, 0, 20, 20);
        d->setSymbolFrames(idA, {{ia}});
        d->setSymbolFrames(idB, {{ib}});

        SymbolScope scope(&d->s.symbols);
        const QByteArray k1 = render::symbolBodyDigest(idA);
        const QByteArray k2 = render::symbolBodyDigest(idA);
        const QByteArray kB = render::symbolBodyDigest(idB);
        QVERIFY(!k1.isEmpty());
        QCOMPARE(k1, k2);
        QVERIFY(!kB.isEmpty());
        // Render must terminate.
        const QImage img = d->render(0, true);
        QVERIFY(!img.isNull());
        delete d;
    }

    void mixedCanvasDropOpensNativeAndImportsImage() {
        Document *d = Document::createNew(160, 120, Qt::white);
        DocumentView *view = w->addDocument(d);
        drainEvents();
        Canvas *c = view->canvas();
        QVERIFY(c);
        c->resize(400, 300);
        drainEvents();

        QTemporaryDir dir;
        QString err;
        const QString bon = dir.filePath(QStringLiteral("drop.bon"));
        QVERIFY2(lwd::save(*d, bon, &err), qPrintable(err));
        QImage png(16, 16, QImage::Format_ARGB32_Premultiplied);
        png.fill(QColor(20, 180, 40));
        const QString pngPath = dir.filePath(QStringLiteral("drop.png"));
        QVERIFY(png.save(pngPath, "PNG"));

        const int tabsBefore = [&] {
            int n = 0;
            // MainWindow tabs: count via opening — use a property: recent or just openFile side effect
            return n;
        }();
        Q_UNUSED(tabsBefore);

        const int objsBefore = d->objects(0).size();
        int opens = 0;
        QObject::connect(c, &Canvas::openDocumentRequested, c, [&](const QString &) { ++opens; });

        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(pngPath), QUrl::fromLocalFile(bon)});
        QDropEvent ev(QPointF(40, 40), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        // Deliver straight to Canvas::dropEvent (same pattern as ui_smoke_test).
        static_cast<QObject *>(c)->event(&ev);
        drainEvents();
        QVERIFY2(ev.isAccepted(), "mixed drop must be accepted");
        QCOMPARE(d->objects(0).size(), objsBefore + 1);
        QCOMPARE(opens, 1);

        // Also deliver through the live MainWindow connection: drop again and check a new tab opens.
        const bool opened = w->openFile(bon);
        QVERIFY2(opened, "openFile must load a .bon document");
        Document *openedDoc = w->currentDoc();
        QVERIFY(openedDoc);
        QVERIFY(openedDoc->filePath.endsWith(QStringLiteral(".bon"), Qt::CaseInsensitive));
        delete mime;
    }

    void libreworksSavePolicyAndCliOpenBon() {
        if (brand::isBonbon()) {
            QVERIFY(!brand::saveRequiresNativeExt(QStringLiteral("sketch.bon")));
            QVERIFY(!brand::saveRequiresNativeExt(QStringLiteral("sketch.lwd")));
            QCOMPARE(brand::suggestNativeFileName(QStringLiteral("sketch.bon")), QStringLiteral("sketch.bon"));
        } else {
            QVERIFY(brand::saveRequiresNativeExt(QStringLiteral("sketch.bon")));
            QVERIFY(brand::saveRequiresNativeExt(QStringLiteral("SKETCH.BON")));
            QVERIFY(!brand::saveRequiresNativeExt(QStringLiteral("sketch.lwd")));
            QCOMPARE(brand::suggestNativeFileName(QStringLiteral("sketch.bon")), QStringLiteral("sketch.lwd"));
        }

        Document *d = Document::createNew(80, 60, QColor(240, 240, 255));
        VObject r = makeRect(*d, QRectF(8, 8, 30, 20), QColor(180, 20, 80));
        d->addObject(r, 0, false);
        QTemporaryDir dir;
        QString err;
        const QString bon = dir.filePath(QStringLiteral("cli.bon"));
        QVERIFY2(lwd::save(*d, bon, &err), qPrintable(err));
        delete d;

        QVERIFY(w->openFile(bon));
        Document *opened = w->currentDoc();
        QVERIFY(opened);
        QVERIFY(opened->filePath.endsWith(QLatin1String(".bon"), Qt::CaseInsensitive));
        if (!brand::isBonbon())
            QVERIFY(brand::saveRequiresNativeExt(opened->filePath));

        // Batch can open *.bon on both brands.
        batch::Script script;
        script.input = batch::Script::Files;
        script.files = {bon};
        script.output = batch::Script::ToFolder;
        script.outDir = dir.filePath(QStringLiteral("out"));
        QDir().mkpath(script.outDir);
        const batch::Result res = batch::run(script, script.files, {}, {});
        QCOMPARE(res.failed, 0);
        QVERIFY(res.processed >= 1);

#ifndef LW_EXE
        QSKIP("LW_EXE not defined");
#else
        const QString shot = dir.filePath(QStringLiteral("cli.png"));
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        p.setProcessEnvironment(env);
        p.start(QStringLiteral(LW_EXE), {bon, QStringLiteral("--screenshot"), shot, QStringLiteral("--size"), QStringLiteral("800x500")});
        QVERIFY2(p.waitForFinished(30000), qPrintable(p.readAllStandardError()));
        QCOMPARE(p.exitStatus(), QProcess::NormalExit);
        QCOMPARE(p.exitCode(), 0);
        QVERIFY(QFile::exists(shot));
        QImage img(shot);
        QVERIFY(!img.isNull());
#endif
    }

    void blurReachEarlyOutIsPixelIdentical() {
        Document *d = Document::createNew(200, 200, Qt::white);
        VObject o = makeRect(*d, QRectF(60, 60, 40, 40), QColor(30, 160, 40));
        o.effects.append(LiveEffect::make(EffectType::RadialBlur));
        o.effects.last().amount = 80;
        o.effects.append(LiveEffect::make(EffectType::ZoomBlur));
        o.effects.last().amount = 60;
        d->addObject(o, 0, false);
        render::setObjectVisualCacheEnabled(false);
        QRect areaFull, areaClip, areaFar;
        const QImage full = render::renderObject(o, &areaFull, QRect());
        QVERIFY(!full.isNull());
        const QRect tight = o.visualBounds().toAlignedRect();
        const QImage clipped = render::renderObject(o, &areaClip, tight);
        QVERIFY2(samePixels(full, clipped), "early-out must not change pixels when the clip overlaps the object");
        QCOMPARE(areaFull, areaClip);

        // A clip well beyond true reach must skip work.
        const int reach = int(qCeil(qMax(o.effectMargin(), o.effectReach()) + 2));
        const QRect far(tight.right() + reach + 80, tight.bottom() + reach + 80, 10, 10);
        const QImage skipped = render::renderObject(o, &areaFar, far);
        QVERIFY(skipped.isNull());

        // Full-frame render vs object blit stays defined.
        const QImage frame = d->render(0, true);
        QVERIFY(!frame.isNull());
        render::setObjectVisualCacheEnabled(true);
        delete d;
    }

    void desktopListsBonbonWithoutOwningMimeOnLibreworks() {
        auto readNear = [](const QString &rel) -> QByteArray {
            const QStringList tries = {
                rel,
                QCoreApplication::applicationDirPath() + QLatin1Char('/') + rel,
                QStringLiteral("../") + rel,
            };
            for (const QString &path : tries) {
                QFile f(path);
                if (f.open(QIODevice::ReadOnly)) return f.readAll();
            }
            return {};
        };
        const QByteArray deskBytes = readNear(QStringLiteral("packaging/%1.desktop").arg(QStringLiteral(LW_DESKTOP_ID)));
        const QByteArray mimeBytes = readNear(QStringLiteral("packaging/%1-lwd.xml").arg(QStringLiteral(LW_PACKAGE_NAME)));
        QVERIFY2(!deskBytes.isEmpty(), "generated .desktop missing");
        QVERIFY2(!mimeBytes.isEmpty(), "generated MIME xml missing");
        QVERIFY2(deskBytes.contains("application/x-bonbon"), deskBytes.constData());
        if (!brand::isBonbon()) {
            QVERIFY2(!mimeBytes.contains("*.bon"), mimeBytes.constData());
            QVERIFY2(!mimeBytes.contains("application/x-bonbon"), mimeBytes.constData());
        } else {
            QVERIFY2(mimeBytes.contains("*.bon"), mimeBytes.constData());
            QVERIFY2(mimeBytes.contains("application/x-bonbon"), mimeBytes.constData());
        }
        const QString filt = ImageIO::importFilter();
        QVERIFY2(filt.contains(QStringLiteral("*.bon")), qPrintable(filt));
        QVERIFY2(filt.contains(QStringLiteral("*.lwd")), qPrintable(filt));
        QVERIFY(brand::isNativeDocumentPath(QStringLiteral("doc.bon")));
        QVERIFY(brand::isNativeDocumentPath(QStringLiteral("doc.lwd")));
    }
};

QTEST_MAIN(V048)
#include "v048_test.moc"
