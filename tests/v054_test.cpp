// Fireworks Hue / Saturation / Color / Luminosity / Invert / Tint / Erase blend modes (0.3.20).
#include "core/Document.h"
#include "core/Object.h"
#include "core/Render.h"
#include "io/Codecs.h"
#include "io/DocIO.h"
#include "io/LwdFormat.h"
#include <QPainter>
#include <QtTest>

class V054Test : public QObject {
    Q_OBJECT
    static VObject rect(Document *d, const QRectF &r, const QColor &c, BlendMode b = BlendMode::Normal)
    {
        VObject o;
        o.type = ObjType::Rect;
        o.rect = r;
        o.fillOn = true;
        o.fill = c;
        o.strokeOn = false;
        o.antialias = false;
        o.blend = b;
        o.id = d->newId();
        return o;
    }
    static QRgb pixel(const QImage &img, int x, int y)
    {
        return qUnpremultiply(img.convertToFormat(QImage::Format_ARGB32_Premultiplied).pixel(x, y));
    }

private slots:
    void namesAndKeys()
    {
        const QStringList names = blendModeNames();
        QCOMPARE(names.size(), 19);
        QVERIFY(names.contains(QStringLiteral("Hue")));
        QVERIFY(names.contains(QStringLiteral("Saturation")));
        QVERIFY(names.contains(QStringLiteral("Color")));
        QVERIFY(names.contains(QStringLiteral("Luminosity")));
        QVERIFY(names.contains(QStringLiteral("Invert")));
        QVERIFY(names.contains(QStringLiteral("Tint")));
        QVERIFY(names.contains(QStringLiteral("Erase")));
        for (int i = 0; i < names.size(); ++i) {
            const auto m = BlendMode(i);
            QCOMPARE(int(blendModeFromKey(blendModeKey(m))), i);
        }
        QVERIFY(blendIsCustom(BlendMode::Hue));
        QVERIFY(blendIsCustom(BlendMode::Tint));
        QVERIFY(!blendIsCustom(BlendMode::Erase));
        QVERIFY(!blendIsCustom(BlendMode::Multiply));
        QCOMPARE(toComposition(BlendMode::Erase), QPainter::CompositionMode_DestinationOut);
    }

    void modesRenderDistinctPixels()
    {
        // Solid red over a green canvas, one mode at a time. The seven new modes (and Normal)
        // must each produce a different mid-rect pixel so the compositing is actually distinct.
        QMap<QRgb, QString> seen;
        const QList<BlendMode> modes = {BlendMode::Normal, BlendMode::Hue, BlendMode::Saturation,
                                        BlendMode::Color, BlendMode::Luminosity, BlendMode::Invert,
                                        BlendMode::Tint, BlendMode::Erase};
        for (BlendMode m : modes) {
            std::unique_ptr<Document> d(Document::createNew(80, 80, QColor(0, 160, 40)));
            d->addObject(rect(d.get(), QRectF(20, 20, 40, 40), QColor(220, 20, 20), m), 0, false);
            const QImage img = d->render(0, true);
            const QRgb px = pixel(img, 40, 40);
            const QString name = blendModeKey(m);
            QVERIFY2(!seen.contains(px), qPrintable(QStringLiteral("%1 collided with %2 at %3")
                                                        .arg(name, seen.value(px), QColor(px).name(QColor::HexArgb))));
            seen.insert(px, name);
        }
    }

    void hueKeepsBaseLuma()
    {
        std::unique_ptr<Document> d(Document::createNew(80, 80, QColor(0, 160, 40)));
        d->addObject(rect(d.get(), QRectF(20, 20, 40, 40), QColor(220, 20, 20), BlendMode::Hue), 0, false);
        const QRgb px = pixel(d->render(0, true), 40, 40);
        const QColor c = QColor::fromRgba(px);
        // Hue of red over mid-green: the result is reddish, but not the source red, and luma
        // stays close to the green canvas (Rec. 601 luma of (0,160,40) is ~97).
        QVERIFY(c.red() > c.green());
        QVERIFY(c.red() > c.blue());
        QVERIFY(c.red() < 220); // not the raw source red (220)
        const int luma = qRound(0.3 * c.red() + 0.59 * c.green() + 0.11 * c.blue());
        QVERIFY2(qAbs(luma - 97) <= 8, qPrintable(QString::number(luma)));
    }

    void invertFlipsBase()
    {
        std::unique_ptr<Document> d(Document::createNew(80, 80, QColor(40, 40, 40)));
        d->addObject(rect(d.get(), QRectF(20, 20, 40, 40), QColor(255, 0, 0), BlendMode::Invert), 0, false);
        const QRgb px = pixel(d->render(0, true), 40, 40);
        // Invert of 40 is 215, independent of the source colour.
        QCOMPARE(qRed(px), 215);
        QCOMPARE(qGreen(px), 215);
        QCOMPARE(qBlue(px), 215);
        QCOMPARE(qAlpha(px), 255);
    }

    void erasePunchesHole()
    {
        std::unique_ptr<Document> d(Document::createNew(80, 80, QColor(0, 160, 40)));
        d->addObject(rect(d.get(), QRectF(20, 20, 40, 40), QColor(255, 0, 0), BlendMode::Erase), 0, false);
        const QImage img = d->render(0, true);
        QCOMPARE(qAlpha(pixel(img, 40, 40)), 0);
        // Canvas outside the object stays opaque green.
        const QRgb out = pixel(img, 5, 5);
        QCOMPARE(qAlpha(out), 255);
        QCOMPARE(qGreen(out), 160);
    }

    void lwdRoundTrip()
    {
        std::unique_ptr<Document> d(Document::createNew(60, 60, Qt::white));
        const QList<BlendMode> modes = {BlendMode::Hue, BlendMode::Saturation, BlendMode::Color,
                                        BlendMode::Luminosity, BlendMode::Invert, BlendMode::Tint,
                                        BlendMode::Erase};
        int x = 2;
        for (BlendMode m : modes) {
            d->addObject(rect(d.get(), QRectF(x, 2, 6, 6), QColor(200, 10, 10), m), 0, false);
            x += 8;
        }
        QString err;
        const QByteArray data = lwd::serialize(*d);
        std::unique_ptr<Document> r(lwd::deserialize(data, &err));
        QVERIFY2(r, qPrintable(err));
        QCOMPARE(r->s.frames[0].layers[0].size(), modes.size());
        for (int i = 0; i < modes.size(); ++i)
            QCOMPARE(int(r->s.frames[0].layers[0][i].blend), int(modes[i]));
        const QImage a = d->render(0, true), b = r->render(0, true);
        QCOMPARE(a.size(), b.size());
        QCOMPARE(a, b);
    }

    void psdMapsHueFamily()
    {
        codec::LayeredImage li;
        li.width = 32;
        li.height = 32;
        auto add = [&](const QString &name, QColor c, BlendMode m) {
            codec::RasterLayer L;
            L.name = name;
            L.image = QImage(32, 32, QImage::Format_ARGB32);
            L.image.fill(c);
            L.blend = int(m);
            li.layers << L;
        };
        add(QStringLiteral("base"), QColor(0, 160, 40), BlendMode::Normal);
        add(QStringLiteral("hue"), QColor(220, 20, 20, 200), BlendMode::Hue);
        add(QStringLiteral("sat"), QColor(220, 20, 20, 200), BlendMode::Saturation);
        add(QStringLiteral("col"), QColor(220, 20, 20, 200), BlendMode::Color);
        add(QStringLiteral("lum"), QColor(220, 20, 20, 200), BlendMode::Luminosity);
        QByteArray bytes;
        QString err;
        QVERIFY2(codec::writePsd(bytes, li, &err), qPrintable(err));
        codec::LayeredImage out;
        QVERIFY2(codec::readPsd(bytes, out, &err), qPrintable(err));
        QCOMPARE(out.layers.size(), 5);
        QCOMPARE(out.layers[1].blend, int(BlendMode::Hue));
        QCOMPARE(out.layers[2].blend, int(BlendMode::Saturation));
        QCOMPARE(out.layers[3].blend, int(BlendMode::Color));
        QCOMPARE(out.layers[4].blend, int(BlendMode::Luminosity));
    }

    void oraMapsHueAndErase()
    {
        codec::LayeredImage li;
        li.width = 16;
        li.height = 16;
        codec::RasterLayer base, hue, era;
        base.name = QStringLiteral("base");
        base.image = QImage(16, 16, QImage::Format_ARGB32);
        base.image.fill(QColor(0, 160, 40));
        hue.name = QStringLiteral("hue");
        hue.image = QImage(16, 16, QImage::Format_ARGB32);
        hue.image.fill(QColor(220, 20, 20, 200));
        hue.blend = int(BlendMode::Hue);
        era.name = QStringLiteral("erase");
        era.image = QImage(16, 16, QImage::Format_ARGB32);
        era.image.fill(QColor(255, 255, 255));
        era.blend = int(BlendMode::Erase);
        li.layers << base << hue << era;
        QByteArray bytes;
        QString err;
        QVERIFY2(codec::writeOra(bytes, li, &err), qPrintable(err));
        codec::LayeredImage out;
        QVERIFY2(codec::readOra(bytes, out, &err), qPrintable(err));
        QCOMPARE(out.layers.size(), 3);
        QCOMPARE(out.layers[1].blend, int(BlendMode::Hue));
        QCOMPARE(out.layers[2].blend, int(BlendMode::Erase));
    }

    void svgMapsHueFamily()
    {
        std::unique_ptr<Document> d(Document::createNew(40, 40, Qt::white));
        d->addObject(rect(d.get(), QRectF(0, 0, 40, 40), QColor(0, 160, 40)), 0, false);
        auto fg = rect(d.get(), QRectF(8, 8, 24, 24), QColor(220, 20, 20), BlendMode::Hue);
        d->addObject(fg, 0, false);
        ExportOptions o;
        o.format = Fmt::SVG;
        QByteArray data;
        QString err;
        QVERIFY2(docio::exportDocument(*d, 0, o, data, &err), qPrintable(err));
        QVERIFY2(QString::fromUtf8(data).contains(QStringLiteral("mix-blend-mode:hue")), data.constData());
        std::unique_ptr<Document> r(docio::openData(data, Fmt::SVG, QString(), &err));
        QVERIFY2(r, qPrintable(err));
        bool found = false;
        for (const VObject &obj : r->s.frames[0].layers[0])
            if (obj.blend == BlendMode::Hue) found = true;
        // SVG import may put the blend on a group wrapping the shape.
        if (!found) {
            for (const auto &L : r->s.frames[0].layers)
                for (const VObject &obj : L)
                    if (obj.blend == BlendMode::Hue) found = true;
        }
        QVERIFY2(found, "SVG re-import should recover Hue (on the object or its group)");
    }
};

QTEST_MAIN(V054Test)
#include "v054_test.moc"
