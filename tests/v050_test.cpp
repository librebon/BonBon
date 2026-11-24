// v0.3.20: WebP export Effort (libwebp method 0–6, default 3).
#include "io/Codecs.h"
#include "io/ImageIO.h"
#include "io/LwdFormat.h"
#include "ui/Dialogs.h"
#include "Brand.h"
#include <QComboBox>
#include <QJsonObject>
#include <QLabel>
#include <QSpinBox>
#include <QtTest>

static ImageSeq solidNoise(int w, int h) {
    QImage im(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        auto *p = reinterpret_cast<QRgb *>(im.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int n = ((x * 73 + y * 47) ^ (x * y * 13)) & 255;
            p[x] = qRgba(n, (n * 3) & 255, (x + y) & 255, 255);
        }
    }
    ImageSeq seq;
    seq.frames.append({im, 100});
    seq.width = w;
    seq.height = h;
    return seq;
}

class V050 : public QObject {
    Q_OBJECT
private slots:
    void webpEffortDefaultsToThree() {
        QCOMPARE(ExportOptions{}.webpEffort, 3);
        const ExportOptions e = lwd::exportOptionsFromJson(QJsonObject{{QStringLiteral("format"), QStringLiteral("webp")},
                                                                       {QStringLiteral("quality"), 80}});
        QCOMPARE(e.format, Fmt::WEBP);
        QCOMPARE(e.webpEffort, 3);
        QVERIFY(!lwd::exportOptionsToJson(ExportOptions()).contains(QStringLiteral("webpEffort")));
    }

    void webpEffortRoundTripsAndClamps() {
        ExportOptions o;
        o.format = Fmt::WEBP;
        o.webpEffort = 6;
        const QJsonObject j = lwd::exportOptionsToJson(o);
        QCOMPARE(j.value(QStringLiteral("webpEffort")).toInt(), 6);
        const ExportOptions back = lwd::exportOptionsFromJson(j);
        QCOMPARE(back.webpEffort, 6);
        QVERIFY(back == o);

        const ExportOptions lo = lwd::exportOptionsFromJson(QJsonObject{{QStringLiteral("webpEffort"), -4}});
        QCOMPARE(lo.webpEffort, 0);
        const ExportOptions hi = lwd::exportOptionsFromJson(QJsonObject{{QStringLiteral("webpEffort"), 99}});
        QCOMPARE(hi.webpEffort, 6);
    }

    void writeWebpHonoursEffort() {
        if (!ImageIO::canWrite(Fmt::WEBP))
            QSKIP("WebP writer not built");
        const ImageSeq seq = solidNoise(256, 192);
        ExportOptions o;
        o.format = Fmt::WEBP;
        o.quality = 80;
        o.lossless = false;
        o.webpEffort = 0;
        QByteArray fast, slow;
        QString err;
        QVERIFY2(codec::writeWebp(fast, seq, o, &err), qPrintable(err));
        o.webpEffort = 6;
        err.clear();
        QVERIFY2(codec::writeWebp(slow, seq, o, &err), qPrintable(err));
        QVERIFY(!fast.isEmpty());
        QVERIFY(!slow.isEmpty());
        // Higher method is allowed to match but should not grow the file.
        QVERIFY2(slow.size() <= fast.size(),
                 qPrintable(QStringLiteral("effort 6 (%1 bytes) should be <= effort 0 (%2 bytes)")
                                .arg(slow.size()).arg(fast.size())));
    }

    void optimizeWidgetShowsWebpEffort() {
        OptimizeWidget w;
        ExportOptions png;
        png.format = Fmt::PNG;
        w.setOptions(png);
        auto *effort = w.findChild<QSpinBox *>(QStringLiteral("optWebpEffort"));
        QVERIFY2(effort, "expected optWebpEffort spin box");
        QVERIFY(!effort->isVisibleTo(&w));

        ExportOptions webp;
        webp.format = Fmt::WEBP;
        webp.quality = 75;
        webp.webpEffort = 4;
        w.setOptions(webp);
        QVERIFY(effort->isVisibleTo(&w));
        QCOMPARE(effort->value(), 4);
        QCOMPARE(effort->minimum(), 0);
        QCOMPARE(effort->maximum(), 6);
        effort->setValue(5);
        QCOMPARE(w.options().webpEffort, 5);
        QCOMPARE(w.options().quality, 75);
    }
};

QTEST_MAIN(V050)
#include "v050_test.moc"
