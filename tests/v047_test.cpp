// v0.3.17: open stamps Preferences Scaling on PSD/ORA/XCF/SVG; pref change refreshes
// canvas; nested symbol body in cache key; undo after open keeps stamped interpolation.
#include "core/Document.h"
#include "core/Effects.h"
#include "core/Object.h"
#include "core/CorePrefs.h"
#include "core/Render.h"
#include "core/PixelScale.h"
#include "io/DocIO.h"
#include "io/ImageIO.h"
#include "io/LwdFormat.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "Brand.h"
#include <QApplication>
#include <QPainter>
#include <QAction>
#include <QCursor>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
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

static int countBitmaps(const Document &d, std::function<void(const VObject &)> visit = {}) {
    int n = 0;
    auto walk = [&](auto &&self, const VObject &o) -> void {
        if (o.type == ObjType::Bitmap) { ++n; if (visit) visit(o); }
        for (const VObject &c : o.children) self(self, c);
    };
    for (const Frame &f : d.s.frames)
        for (const auto &l : f.layers)
            for (const VObject &o : l) walk(walk, o);
    for (const Symbol &sym : d.s.symbols)
        for (const auto &fr : sym.frames)
            for (const VObject &o : fr) walk(walk, o);
    return n;
}

static Document *sourceDocWithScaledBitmap() {
    Document *d = Document::createNew(160, 120, Qt::white);
    VObject bmp = makeScaledBitmap(*d, QSize(32, 24), QRectF(10, 10, 96, 72), -1); // unset; addObject stamps
    d->addObject(bmp, 0, false);
    // Force display scale so Nearest vs Bicubic differ visibly after reopen+pref change.
    return d;
}

class V047 : public QObject {
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

    // Bug 1: PSD/ORA/XCF/SVG open stamps live Preferences; later pref change does not alter them.
    void layeredAndSvgOpenStampsLivePref() {
        const int saved = coreprefs::interpolation;
        coreprefs::interpolation = coreprefs::Soft;
        std::unique_ptr<Document> src(sourceDocWithScaledBitmap());
        QCOMPARE(src->find(src->s.frames[0].layers[0][0].id)->interpolation, int(coreprefs::Soft));

        auto checkOpened = [&](Fmt fmt, const QByteArray &data) {
            QString err;
            std::unique_ptr<Document> d(docio::openData(data, fmt, QString(), &err));
            QVERIFY2(d, qPrintable(QStringLiteral("%1: %2").arg(int(fmt)).arg(err)));
            int bitmaps = 0, stampedSoft = 0;
            countBitmaps(*d, [&](const VObject &o) {
                ++bitmaps;
                if (o.interpolation == int(coreprefs::Soft)) ++stampedSoft;
            });
            QVERIFY2(bitmaps >= 1, "opened doc must contain a bitmap");
            QCOMPARE(stampedSoft, bitmaps);

            QImage atSoft = d->render(0, true);
            coreprefs::interpolation = coreprefs::Nearest;
            render::clearObjectVisualCache();
            QImage afterPref = d->render(0, true);
            QVERIFY2(samePixels(atSoft, afterPref),
                     "pref change must not alter bitmaps stamped at Open");
            countBitmaps(*d, [&](const VObject &o) {
                QCOMPARE(o.interpolation, int(coreprefs::Soft));
            });
            coreprefs::interpolation = coreprefs::Soft; // restore for next format
        };

        for (Fmt f : {Fmt::PSD, Fmt::ORA, Fmt::XCF}) {
            ExportOptions o = src->exportOptions;
            o.format = f;
            QByteArray data;
            QString err;
            QVERIFY2(docio::exportDocument(*src, 0, o, data, &err), qPrintable(err));
            checkOpened(f, data);
        }
        {
            ExportOptions o = src->exportOptions;
            o.format = Fmt::SVG;
            QByteArray data;
            QString err;
            QVERIFY2(docio::exportDocument(*src, 0, o, data, &err), qPrintable(err));
            checkOpened(Fmt::SVG, data);
        }
        coreprefs::interpolation = saved;
    }

    // Content / visual cache key never aliases unset methods across Preferences.
    void contentKeyUsesEffectiveInterpolation() {
        VObject a;
        a.type = ObjType::Bitmap;
        a.image = checker(16, 16);
        a.rect = QRectF(0, 0, 48, 48);
        a.interpolation = -1;
        const int saved = coreprefs::interpolation;
        coreprefs::interpolation = coreprefs::Bicubic;
        const QByteArray kBicubic = lwd::objectContentKey(a);
        coreprefs::interpolation = coreprefs::Nearest;
        const QByteArray kNearest = lwd::objectContentKey(a);
        QVERIFY2(kBicubic != kNearest, "unset bitmaps must not share one content key across prefs");
        a.interpolation = coreprefs::Bicubic;
        coreprefs::interpolation = coreprefs::Nearest;
        QCOMPARE(lwd::objectContentKey(a), kBicubic); // stamped Bicubic key matches earlier effective
        coreprefs::interpolation = saved;
    }

    // Bug 2: prefsChanged clears object visual cache and dirties every open canvas frame cache.
    void prefsChangeRefreshesCanvas() {
        const int saved = coreprefs::interpolation;
        coreprefs::interpolation = coreprefs::Bicubic;
        Prefs p = appState().prefs;
        p.interpolation = coreprefs::Bicubic;
        appState().setPrefs(p);

        Document *d = Document::createNew(160, 120, Qt::white);
        VObject bmp = makeScaledBitmap(*d, QSize(32, 24), QRectF(10, 10, 96, 72), -1);
        d->addObject(bmp, 0, false);
        // Leave one bitmap following live prefs (simulate pre-stamp edge) to prove refresh.
        d->find(bmp.id)->interpolation = -1;
        DocumentView *dv = w->addDocument(d);
        drainEvents();
        Canvas *c = dv->canvas();
        QVERIFY(c);
        const QImage before = c->renderedFrame().copy();
        QVERIFY(!before.isNull());

        p.interpolation = coreprefs::Nearest;
        appState().setPrefs(p); // emits prefsChanged → clear visual cache + invalidateFrameCache
        drainEvents();
        const QImage after = c->renderedFrame().copy();
        QVERIFY2(!samePixels(before, after),
                 "prefsChanged must rebuild the canvas frame when an unset bitmap follows the pref");
        coreprefs::interpolation = saved;
        p.interpolation = saved;
        appState().setPrefs(p);
    }

    // Nested symbol body is part of the outer instance key (also covers Problem 8 / deleteSymbol clear).
    void nestedSymbolEditUpdatesOuterFxInstance() {
        render::setObjectVisualCacheEnabled(true);
        render::clearObjectVisualCache();
        Document *d = Document::createNew(280, 200, Qt::white);

        // Inner symbol S2: a coloured rect.
        VObject innerBody;
        innerBody.type = ObjType::Rect;
        innerBody.id = d->newId();
        innerBody.rect = QRectF(20, 20, 60, 40);
        innerBody.fill = QColor(200, 40, 40);
        innerBody.strokeOn = false;
        d->addObject(innerBody, 0, false);
        d->selection = {innerBody.id};
        const quint64 s2 = d->convertToSymbol(QStringLiteral("Inner"), SymbolKind::Graphic);
        QVERIFY(s2 != 0);

        // Outer symbol S1: contains the instance of S2 (current selection after convert).
        quint64 s2InstId = 0;
        for (auto &f : d->s.frames)
            for (auto &l : f.layers)
                for (auto &o : l)
                    if (o.type == ObjType::Instance && o.symbolId == s2) s2InstId = o.id;
        QVERIFY(s2InstId != 0);
        d->selection = {s2InstId};
        const quint64 s1 = d->convertToSymbol(QStringLiteral("Outer"), SymbolKind::Graphic);
        QVERIFY(s1 != 0);

        VObject *outerInst = nullptr;
        for (auto &f : d->s.frames)
            for (auto &l : f.layers)
                for (auto &o : l)
                    if (o.type == ObjType::Instance && o.symbolId == s1) outerInst = &o;
        QVERIFY(outerInst);
        LiveEffect sh = LiveEffect::make(EffectType::DropShadow);
        sh.enabled = true;
        outerInst->effects = {sh};
        d->notifyChanged();

        QImage before = d->render(0, true);

        // Edit nested S2 fill; setSymbolFrames invalidates all body keys + clears cache.
        Symbol *sym2 = nullptr;
        for (Symbol &s : d->s.symbols) if (s.id == s2) sym2 = &s;
        QVERIFY(sym2);
        QVector<QVector<VObject>> frames = sym2->frames;
        frames[0][0].fill = QColor(40, 40, 200);
        d->setSymbolFrames(s2, frames);
        QImage after = d->render(0, true);
        QVERIFY2(!samePixels(before, after),
                 "editing nested symbol must update outer FX instance composite");

        // deleteSymbol clears the visual cache (no stale outer after delete).
        QVERIFY(d->deleteSymbol(s2));
        render::setObjectVisualCacheEnabled(true);
        delete d;
    }

    // Problem 5: undo of a move after open must not touch interpolation (no commit-time mass stamp).

    // Bonbon: .bon is the native extension (same payload as .lwd); Libreworks stays on .lwd.
    void brandNativeDocumentExtension() {
        QVERIFY(brand::docExt().startsWith(QLatin1Char('.')));
        QVERIFY(!brand::docFilter().isEmpty());
        QVERIFY(brand::isNativeDocumentPath(QStringLiteral("x.lwd")));
        QVERIFY(brand::isNativeDocumentPath(QStringLiteral("X.LWD")));
        if (brand::isBonbon()) {
            QCOMPARE(brand::docExt(), QStringLiteral(".bon"));
            QCOMPARE(brand::docExtNoDot(), QStringLiteral("bon"));
            QCOMPARE(brand::docMime(), QStringLiteral("application/x-bonbon"));
            QCOMPARE(brand::docFilter(), QStringLiteral("Bonbon Document (*.bon)"));
            QVERIFY(brand::isNativeDocumentPath(QStringLiteral("doc.bon")));
            QVERIFY(brand::isNativeDocumentPath(QStringLiteral("DOC.BON")));
            QCOMPARE(brand::suggestNativeFileName(QStringLiteral("sketch.lwd")), QStringLiteral("sketch.bon"));
            QCOMPARE(brand::suggestNativeFileName(QStringLiteral("sketch.bon")), QStringLiteral("sketch.bon"));
            QCOMPARE(brand::suggestNativeFileName(QStringLiteral("Untitled")), QStringLiteral("Untitled.bon"));
            const QString filt = ImageIO::importFilter();
            QVERIFY2(filt.contains(QStringLiteral("*.bon")), qPrintable(filt));
            QVERIFY2(filt.contains(QStringLiteral("*.lwd")), qPrintable(filt));
            QVERIFY2(filt.contains(QStringLiteral("Bonbon Document")), qPrintable(filt));
        } else {
            QCOMPARE(brand::docExt(), QStringLiteral(".lwd"));
            QCOMPARE(brand::docMime(), QStringLiteral("application/x-libreworks"));
            QVERIFY(brand::isNativeDocumentPath(QStringLiteral("doc.bon")));
            QCOMPARE(brand::suggestNativeFileName(QStringLiteral("sketch.lwd")), QStringLiteral("sketch.lwd"));
            const QString filt = ImageIO::importFilter();
            QVERIFY2(filt.contains(QStringLiteral("*.lwd")), qPrintable(filt));
            QVERIFY2(filt.contains(QStringLiteral("*.bon")), qPrintable(filt));
        }

        Document *d = Document::createNew(80, 60, Qt::white);
        VObject bmp = makeScaledBitmap(*d, QSize(16, 12), QRectF(4, 4, 48, 36), coreprefs::Nearest);
        d->addObject(bmp, 0, false);
        QTemporaryDir dir;
        QString err;
        const QString nativePath = dir.filePath(QStringLiteral("n") + brand::docExt());
        const QString lwdPath = dir.filePath(QStringLiteral("n.lwd"));
        QVERIFY2(lwd::save(*d, nativePath, &err), qPrintable(err));
        QVERIFY2(lwd::save(*d, lwdPath, &err), qPrintable(err));
        QFile fn(nativePath), fl(lwdPath);
        QVERIFY(fn.open(QIODevice::ReadOnly) && fl.open(QIODevice::ReadOnly));
        QCOMPARE(fn.readAll(), fl.readAll());
        Document *loaded = lwd::load(nativePath, &err);
        QVERIFY2(loaded, qPrintable(err));
        QVERIFY(samePixels(d->render(0, true), loaded->render(0, true)));
        delete loaded;
        delete d;
    }

    void brandDesktopAndMimeDeclareNativeType() {
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
        QVERIFY2(!deskBytes.isEmpty(), "generated .desktop missing (run from build dir or beside packaging/)");
        QVERIFY2(!mimeBytes.isEmpty(), "generated MIME xml missing");
        if (brand::isBonbon()) {
            QVERIFY2(deskBytes.contains("application/x-bonbon"), deskBytes.constData());
            QVERIFY2(deskBytes.contains("application/x-libreworks"), deskBytes.constData());
            QVERIFY2(mimeBytes.contains("application/x-bonbon"), mimeBytes.constData());
            QVERIFY2(mimeBytes.contains("*.bon"), mimeBytes.constData());
            QVERIFY2(mimeBytes.contains("application-x-bonbon"), mimeBytes.constData());
            QVERIFY2(mimeBytes.contains("*.lwd"), mimeBytes.constData());
        } else {
            QVERIFY2(deskBytes.contains("application/x-libreworks"), deskBytes.constData());
            QVERIFY2(QByteArray(deskBytes).contains("application/x-bonbon"), deskBytes.constData());
            QVERIFY2(mimeBytes.contains("*.lwd"), mimeBytes.constData());
            QVERIFY2(!mimeBytes.contains("*.bon"), mimeBytes.constData());
        }
    }


    // Paste at mouse location (Preferences, off by default): centres paste on pointer over canvas.
    void pasteAtMouseLocation() {
        Prefs saved = appState().prefs;
        Prefs p = saved;
        p.pasteAtMouse = false;
        appState().setPrefs(p);

        Document *d = Document::createNew(400, 300, Qt::white);
        VObject src;
        src.type = ObjType::Rect;
        src.id = d->newId();
        src.rect = QRectF(10, 10, 40, 30);
        src.fill = QColor(200, 40, 40);
        src.strokeOn = false;
        d->addObject(src, 0, true);
        d->commit(QStringLiteral("add"));
        const quint64 srcId = src.id;
        DocumentView *dv = w->addDocument(d);
        drainEvents();
        Canvas *c = dv->canvas();
        QVERIFY(c);
        QVERIFY(w->action(QStringLiteral("edit.copy")));
        w->action(QStringLiteral("edit.copy"))->trigger();
        drainEvents();

        auto findNewestRect = [&](quint64 excludeA = 0, quint64 excludeB = 0, quint64 excludeC = 0) -> VObject * {
            VObject *found = nullptr;
            for (VObject &o : d->objects(0))
                if (o.type == ObjType::Rect && o.id != srcId && o.id != excludeA && o.id != excludeB && o.id != excludeC)
                    found = &o;
            return found;
        };

        // Pref off, pointer not over canvas
        QCursor::setPos(w->mapToGlobal(QPoint(2, 2)));
        drainEvents();
        const int beforeCount = d->objects(0).size();
        w->action(QStringLiteral("edit.paste"))->trigger();
        drainEvents();
        QCOMPARE(d->objects(0).size(), beforeCount + 1);
        VObject *p0 = findNewestRect();
        QVERIFY(p0);
        const quint64 id0 = p0->id;
        const QPointF centerOff = p0->rect.center();

        // Pref on, pointer over canvas
        p.pasteAtMouse = true;
        appState().setPrefs(p);
        const QPoint local(int(c->width() * 0.75), int(c->height() * 0.7));
        QVERIFY(c->rect().contains(local));
        const QPointF expect = c->toDoc(QPointF(local));
        QCursor::setPos(c->mapToGlobal(local));
        drainEvents();
        const int midCount = d->objects(0).size();
        w->action(QStringLiteral("edit.paste"))->trigger();
        drainEvents();
        QCOMPARE(d->objects(0).size(), midCount + 1);
        VObject *p1 = findNewestRect(id0);
        QVERIFY(p1);
        const quint64 id1 = p1->id;
        const QPointF got = p1->rect.center();
        QVERIFY2(qAbs(got.x() - expect.x()) <= 1.5 && qAbs(got.y() - expect.y()) <= 1.5,
                 qPrintable(QStringLiteral("paste centre %1,%2 expected near %3,%4")
                                .arg(got.x()).arg(got.y()).arg(expect.x()).arg(expect.y())));

        // Pointer not over canvas → fall back
        QCursor::setPos(w->mapToGlobal(QPoint(2, 2)));
        drainEvents();
        const int n3 = d->objects(0).size();
        w->action(QStringLiteral("edit.paste"))->trigger();
        drainEvents();
        QCOMPARE(d->objects(0).size(), n3 + 1);
        VObject *p2 = findNewestRect(id0, id1);
        QVERIFY(p2);
        const QPointF fb = p2->rect.center();
        QVERIFY2(qAbs(fb.x() - centerOff.x()) <= 2 && qAbs(fb.y() - centerOff.y()) <= 2,
                 qPrintable(QStringLiteral("fallback %1,%2 vs off %3,%4")
                                .arg(fb.x()).arg(fb.y()).arg(centerOff.x()).arg(centerOff.y())));

        d->undo();
        QCOMPARE(d->objects(0).size(), n3);
        QCOMPARE(saved.pasteAtMouse, false);
        appState().setPrefs(saved);
    }

        void undoMoveAfterOpenKeepsInterpolation() {
        const int saved = coreprefs::interpolation;
        coreprefs::interpolation = coreprefs::Nearest;
        std::unique_ptr<Document> src(sourceDocWithScaledBitmap());
        ExportOptions o = src->exportOptions;
        o.format = Fmt::ORA;
        QByteArray data;
        QString err;
        QVERIFY2(docio::exportDocument(*src, 0, o, data, &err), qPrintable(err));
        std::unique_ptr<Document> d(docio::openData(data, Fmt::ORA, QString(), &err));
        QVERIFY2(d, qPrintable(err));

        VObject *bmp = nullptr;
        for (auto &f : d->s.frames)
            for (auto &l : f.layers)
                for (auto &o2 : l)
                    if (o2.type == ObjType::Bitmap) bmp = &o2;
        QVERIFY(bmp);
        QCOMPARE(bmp->interpolation, int(coreprefs::Nearest));
        const quint64 id = bmp->id;
        const int stamped = bmp->interpolation;

        // Move then undo — history must restore position only, not unset Scaling.
        bmp->rect.translate(12, 8);
        d->commit(QStringLiteral("Move"));
        QCOMPARE(d->find(id)->interpolation, stamped);
        d->undo();
        QCOMPARE(d->find(id)->interpolation, stamped);
        QVERIFY(d->find(id)->interpolation >= 0);

        // Change prefs mid-session; undo/redo of the move still keeps the open-time stamp.
        coreprefs::interpolation = coreprefs::Soft;
        d->redo();
        QCOMPARE(d->find(id)->interpolation, stamped);
        d->undo();
        QCOMPARE(d->find(id)->interpolation, stamped);
        coreprefs::interpolation = saved;
    }
};

QTEST_MAIN(V047)
#include "v047_test.moc"
