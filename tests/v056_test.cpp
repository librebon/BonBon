// Optimize to Size: binary-search export settings to a target byte size (0.3.20).
#include "core/Document.h"
#include "ui/Dialogs.h"
#include "ui/DocumentView.h"
#include <QPainter>
#include <QToolButton>
#include <QtTest>

class V056Test : public QObject {
    Q_OBJECT
    static Document *noisyDoc(int w = 96, int h = 96)
    {
        Document *d = Document::createNew(w, h, Qt::white);
        VObject o;
        o.type = ObjType::Bitmap;
        o.rect = QRectF(0, 0, w, h);
        o.image = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y) {
            QRgb *p = reinterpret_cast<QRgb *>(o.image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const int n = (x * 37 + y * 91 + (x ^ y) * 13) & 255;
                p[x] = qRgb(n, (n * 3) & 255, (x * y) & 255);
            }
        }
        o.id = d->newId();
        d->addObject(o, 0, false);
        return d;
    }

private slots:
    void jpegMeetsGenerousTarget()
    {
        std::unique_ptr<Document> d(noisyDoc());
        ExportOptions o;
        o.format = Fmt::JPEG;
        o.quality = 50;
        const EncodeResult mid = encodePreview(*d, o, 0);
        QVERIFY(mid.error.isEmpty());
        QVERIFY(mid.bytes > 0);
        auto r = optimizeToSize(*d, o, 0, mid.bytes * 4);
        QVERIFY(r.encoded.error.isEmpty());
        QVERIFY(r.metTarget);
        QVERIFY(!r.cancelled);
        QVERIFY(r.encoded.bytes <= mid.bytes * 4);
        QVERIFY(r.options.quality >= 50); // can only stay or rise when the target is generous
    }

    void jpegLowersQualityToFit()
    {
        std::unique_ptr<Document> d(noisyDoc());
        ExportOptions o;
        o.format = Fmt::JPEG;
        o.quality = 95;
        const EncodeResult hi = encodePreview(*d, o, 0);
        QVERIFY(hi.bytes > 0);
        const qint64 target = qMax<qint64>(512, hi.bytes / 3);
        auto r = optimizeToSize(*d, o, 0, target);
        QVERIFY(r.encoded.error.isEmpty());
        if (r.metTarget) {
            QVERIFY(r.encoded.bytes <= target);
            QVERIFY(r.options.quality < 95);
        } else {
            QCOMPARE(r.options.quality, 1); // smallest settings applied
            QVERIFY(r.encoded.bytes > target);
        }
    }

    void gifReducesColors()
    {
        std::unique_ptr<Document> d(noisyDoc(64, 64));
        ExportOptions o;
        o.format = Fmt::GIF;
        o.colors = 256;
        o.dither = true;
        const EncodeResult hi = encodePreview(*d, o, 0);
        QVERIFY(hi.error.isEmpty());
        QVERIFY(hi.bytes > 0);
        const qint64 target = qMax<qint64>(200, hi.bytes / 3);
        auto r = optimizeToSize(*d, o, 0, target);
        QVERIFY(r.encoded.error.isEmpty());
        if (r.metTarget) {
            QVERIFY(r.encoded.bytes <= target);
            QVERIFY(r.options.colors < 256);
        } else {
            QVERIFY(r.options.colors <= 256);
            QVERIFY(r.encoded.bytes > 0);
        }
    }

    void cancelStops()
    {
        std::unique_ptr<Document> d(noisyDoc());
        ExportOptions o;
        o.format = Fmt::JPEG;
        o.quality = 80;
        std::atomic<bool> cancel{true};
        auto r = optimizeToSize(*d, o, 0, 100, &cancel);
        QVERIFY(r.cancelled);
    }

    void buttonOnOptimizeWidget()
    {
        OptimizeWidget w;
        auto *b = w.optimizeToSizeButton();
        QVERIFY(b);
        QCOMPARE(b->objectName(), QStringLiteral("optimizeToSize"));
        QVERIFY(b->isVisible() || !w.isVisible()); // created; shown with the widget
        QVERIFY(!b->icon().isNull());
        QVERIFY(b->text().contains(QStringLiteral("Optimize to Size")));
    }
};

QTEST_MAIN(V056Test)
#include "v056_test.moc"
