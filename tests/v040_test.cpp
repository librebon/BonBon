// v0.3.10: Bonbon (and Libreworks) application icon from brand resources / painted mark
#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include "Brand.h"
#include "ui/Icons.h"
#include "ui/MainWindow.h"

class V040 : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        QApplication::setApplicationName(brand::name());
        QApplication::setOrganizationName(brand::org());
        QApplication::setDesktopFileName(brand::desktopId());
        QApplication::setWindowIcon(icons::appIconIcon());
    }

    void brandResourceLinked() {
#if LW_IS_BONBON
        QVERIFY2(QFile::exists(QStringLiteral(":/brand/bonbon-256.png")),
                 "brand.qrc not registered — need qInitResources_brand()");
        QPixmap pm(QStringLiteral(":/brand/bonbon-256.png"));
        QVERIFY(!pm.isNull());
#else
        QSKIP("Libreworks build — brand PNGs unused");
#endif
    }

    void appIconNotLwWhenBonbon() {
        QImage icon = icons::appIcon(256).toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!icon.isNull());
#if LW_IS_BONBON
        // Reference brand artwork
        QImage ref(QStringLiteral(":/brand/bonbon-256.png"));
        QVERIFY(!ref.isNull());
        ref = ref.convertToFormat(QImage::Format_ARGB32);
        // Must match brand artwork (not orange Lw). Sample centre and a pink corner.
        QCOMPARE(icon.size(), ref.size());
        // Mean abs channel diff should be tiny (identical decode)
        qint64 diff = 0; int n = 0;
        for (int y = 0; y < icon.height(); y += 4)
            for (int x = 0; x < icon.width(); x += 4) {
                const QRgb a = icon.pixel(x, y), b = ref.pixel(x, y);
                diff += qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b)) + qAbs(qBlue(a) - qBlue(b));
                ++n;
            }
        QVERIFY2(diff / qMax(1, n) < 5, qPrintable(QStringLiteral("appIcon diverges from brand art (mean abs=%1)").arg(diff / qMax(1,n))));
        // Lw icon is orange at centre-ish; brand is pink/magenta — hue check on a saturated pixel
        bool foundPink = false;
        for (int y = 40; y < 80; ++y)
            for (int x = 40; x < 80; ++x) {
                QColor c = icon.pixelColor(x, y);
                if (c.alpha() < 200 || c.hslSaturationF() < 0.2) continue;
                const double h = c.hslHueF() * 360.0;
                // pink/magenta ~300–360 / 0–20; orange Lw ~20–45
                if (h > 280 || h < 20) foundPink = true;
                QVERIFY2(!(h > 20 && h < 50 && c.hslSaturationF() > 0.5),
                         "appIcon looks orange (Libreworks Lw) in a Bonbon build");
            }
        QVERIFY(foundPink);
#else
        // Libreworks: centre letter area is bright (white Lw stroke)
        QVERIFY(!icon.isNull());
#endif
    }

    void windowIconHasBrandSizes() {
        const QIcon ic = QApplication::windowIcon();
        QVERIFY(!ic.isNull());
        const QList<QSize> sizes = ic.availableSizes();
        QVERIFY2(!sizes.isEmpty(), "windowIcon has no availableSizes");
#if LW_IS_BONBON
        // Expect several of the embedded sizes
        auto has = [&](int s) {
            for (const QSize &z : sizes) if (z.width() == s) return true;
            return false;
        };
        QVERIFY(has(16) || has(32));
        QVERIFY(has(64) || has(128));
        QVERIFY(has(256) || has(512));
#endif
        QCOMPARE(QApplication::desktopFileName(), brand::desktopId());
    }

    void writeIconMatchesBrand() {
#if !LW_IS_BONBON
        QSKIP("Bonbon-only");
#endif
#ifndef LW_EXE
        QSKIP("LW_EXE not defined");
#else
        QTemporaryDir td;
        const QString out = td.filePath(QStringLiteral("icon.png"));
        QProcess p;
        p.start(QStringLiteral(LW_EXE), {QStringLiteral("--write-icon"), out});
        QVERIFY2(p.waitForFinished(15000), "write-icon timed out");
        QCOMPARE(p.exitCode(), 0);
        QVERIFY(QFile::exists(out));
        QImage written(out), ref(QStringLiteral(":/brand/bonbon-256.png"));
        QVERIFY(!written.isNull() && !ref.isNull());
        written = written.convertToFormat(QImage::Format_ARGB32);
        ref = ref.convertToFormat(QImage::Format_ARGB32).scaled(written.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        qint64 diff = 0; int n = 0;
        for (int y = 0; y < written.height(); y += 2)
            for (int x = 0; x < written.width(); x += 2) {
                const QRgb a = written.pixel(x, y), b = ref.pixel(x, y);
                diff += qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b)) + qAbs(qBlue(a) - qBlue(b));
                ++n;
            }
        QVERIFY2(diff / qMax(1, n) < 8, qPrintable(QStringLiteral("write-icon mean abs diff=%1").arg(diff / qMax(1,n))));
#endif
    }

    void mainWindowUsesBrandIcon() {
        MainWindow w;
        QVERIFY(!w.windowIcon().isNull());
#if LW_IS_BONBON
        QImage a = w.windowIcon().pixmap(64, 64).toImage();
        QImage b = icons::appIcon(64).toImage();
        QVERIFY(!a.isNull() && !b.isNull());
        // Not the Lw orange: check no strong orange dominance
        QColor c = a.pixelColor(32, 20);
        if (c.alpha() > 200 && c.hslSaturationF() > 0.3) {
            const double h = c.hslHueF() * 360.0;
            QVERIFY2(!(h > 20 && h < 50), "MainWindow icon looks like orange Lw");
        }
#endif
    }
};
QTEST_MAIN(V040)
#include "v040_test.moc"
