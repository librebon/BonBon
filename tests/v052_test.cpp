#include <QtTest>
#include <QElapsedTimer>
#include "core/Document.h"
#include "core/Render.h"
#include "io/ImageIO.h"
#include "ui/Dialogs.h"
#include <QApplication>
#include <QFile>

class V052Test : public QObject {
    Q_OBJECT
    std::unique_ptr<Document> gridDoc(int n, int cols)
    {
        std::unique_ptr<Document> d(Document::createNew(900, 900, Qt::white));
        VObject a;
        a.id = d->newId();
        a.type = ObjType::Rect;
        a.rect = QRectF(0, 0, 40, 24);
        a.fill = QColor(200, 40, 40);
        a.fillOn = true;
        a.strokeOn = true;
        a.stroke = QColor(20, 20, 40);
        a.strokeWidth = 2;
        a.strokeKind = QStringLiteral("basic.hard");
        d->addObject(a, 0, false);
        VObject b;
        b.id = d->newId();
        b.type = ObjType::Ellipse;
        b.rect = QRectF(16, 4, 20, 20);
        b.fill = QColor(40, 80, 200);
        b.fillOn = true;
        b.strokeOn = false;
        d->addObject(b, 0, false);
        d->commit(QStringLiteral("art"));
        d->setSelection({a.id, b.id});
        const quint64 sid = d->convertToSymbol(QStringLiteral("Mark"), SymbolKind::Graphic);
        Q_ASSERT(sid);
        d->commit(QStringLiteral("Convert to Symbol"));
        // First instance is already on the canvas; add n-1 more on a grid (same size).
        const VObject *first = nullptr;
        for (const VObject &o : d->objects(0)) if (o.isInstance()) { first = &o; break; }
        Q_ASSERT(first);
        const QSizeF sz = first->rect.size();
        int made = 1;
        for (int i = 1; i < n; ++i) {
            const int col = i % cols, row = i / cols;
            auto ids = d->insertInstance(sid, QPointF(20 + col * 44 + sz.width() / 2,
                                                      20 + row * 28 + sz.height() / 2));
            Q_ASSERT(ids.size() == 1);
            ++made;
        }
        d->commit(QStringLiteral("instances"));
        Q_UNUSED(made);
        return d;
    }
private slots:
    void symbolBodyBlitMatchesUncached() {
        auto d = gridDoc(12, 4);
        render::setObjectVisualCacheEnabled(false);
        render::setSymbolBodyBlitCacheEnabled(false);
        render::clearObjectVisualCache();
        const QImage off = d->render(0, true);
        render::setSymbolBodyBlitCacheEnabled(true);
        render::clearSymbolBodyBlitCache();
        const QImage on = d->render(0, true);
        QCOMPARE(on.size(), off.size());
        QVERIFY2(on == off, "symbol body blit cache must be pixel-identical to drawing children");
        QVERIFY(render::symbolBodyBlitCacheEntries() >= 1);
        render::setObjectVisualCacheEnabled(true);
    }

    void symbolBodyBlitWarm200() {
        auto d = gridDoc(200, 16);
        render::setObjectVisualCacheEnabled(false);
        render::setSymbolBodyBlitCacheEnabled(true);
        render::clearObjectVisualCache();
        // cold
        QElapsedTimer t;
        t.start();
        const QImage cold = d->render(0, true);
        const qint64 coldMs = t.elapsed();
        // warm frames
        qint64 warm1 = 0, warm2 = 0;
        t.restart();
        const QImage w1 = d->render(0, true);
        warm1 = t.elapsed();
        t.restart();
        const QImage w2 = d->render(0, true);
        warm2 = t.elapsed();
        QVERIFY(cold == w1 && w1 == w2);
        QVERIFY(render::symbolBodyBlitCacheEntries() >= 1);
        qWarning("symbol-body blit cache: 200 instances cold=%lld ms warm=%lld / %lld ms entries=%d",
                 coldMs, warm1, warm2, render::symbolBodyBlitCacheEntries());
        render::setObjectVisualCacheEnabled(true);
        // store a small note for the report
        QFile f(QStringLiteral("/workspace/brushes-0.3.20/symbol-cache-perf.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QStringLiteral("200 instances: cold %1 ms, warm %2 ms and %3 ms, cache entries %4\n")
                        .arg(coldMs).arg(warm1).arg(warm2).arg(render::symbolBodyBlitCacheEntries()).toUtf8());
    }

    void encoderControlsShownForFormat() {
        OptimizeWidget w;
        ExportOptions o;
        o.format = Fmt::WEBP;
        o.lossless = false;
        w.setOptions(o);
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optWebpEffort"))->isVisibleTo(&w));
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optWebpAlphaQuality"))->isVisibleTo(&w));
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optWebpSharpYuv"))->isVisibleTo(&w));
        QVERIFY(!w.findChild<QWidget *>(QStringLiteral("optWebpExact"))->isVisibleTo(&w)); // lossless only
        QVERIFY(!w.findChild<QWidget *>(QStringLiteral("optAvifSpeed"))->isVisibleTo(&w));
        o.lossless = true;
        w.setOptions(o);
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optWebpExact"))->isVisibleTo(&w));
        o.format = Fmt::AVIF;
        w.setOptions(o);
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optAvifSpeed"))->isVisibleTo(&w));
        QVERIFY(!w.findChild<QWidget *>(QStringLiteral("optJxlEffort"))->isVisibleTo(&w));
        o.lossless = false;
        w.setOptions(o);
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optAvifChroma"))->isVisibleTo(&w));
        o.format = Fmt::JXL;
        w.setOptions(o);
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optJxlEffort"))->isVisibleTo(&w));
        QVERIFY(!w.findChild<QWidget *>(QStringLiteral("optAvifSpeed"))->isVisibleTo(&w));
        o.format = Fmt::HEIC;
        w.setOptions(o);
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optHeifPreset"))->isVisibleTo(&w));
        o.format = Fmt::PNG;
        o.tinyPng = true;
        w.setOptions(o);
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optTinyDither"))->isVisibleTo(&w));
        o.format = Fmt::KTX2;
        o.ktxZstd = true;
        w.setOptions(o);
        QVERIFY(w.findChild<QWidget *>(QStringLiteral("optKtxZstdLevel"))->isVisibleTo(&w));
        // round-trip a non-default option
        o.format = Fmt::AVIF;
        o.avifSpeed = 4;
        o.avifDepth = 10;
        w.setOptions(o);
        QCOMPARE(w.options().avifSpeed, 4);
        QCOMPARE(w.options().avifDepth, 10);
    }
};

QTEST_MAIN(V052Test)
#include "v052_test.moc"
