// v0.3.8: bundled GNOME-Wine icons, hue-shift per theme, Use system icon theme pref
#include "ui/AppIcons.h"
#include "ui/AppState.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/PreferencesDialog.h"
#include <QAction>
#include <QCheckBox>
#include <QImage>
#include <QElapsedTimer>
#include <QtTest>

class V038 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;
private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        w = std::make_unique<MainWindow>();
        w->show();
        QCoreApplication::processEvents();
    }
    void cleanupTestCase() { w.reset(); }

    void defaultNeverUsesSystem() {
        Prefs p = appState().prefs;
        p.useSystemIcons = false;
        appState().setPrefs(p);
        QVERIFY(!appState().prefs.useSystemIcons);
        for (const QString &n : appicons::bundledNames()) {
            QVERIFY2(appicons::isBundled(n), qPrintable(n));
            QIcon ic = appicons::icon(n);
            QVERIFY2(!ic.isNull(), qPrintable(n));
            QVERIFY2(!ic.pixmap(16, 16).isNull(), qPrintable(n + " pixmap"));
        }
    }

    void everyToolbarActionHasBundledIcon() {
        Prefs p = appState().prefs;
        p.useSystemIcons = false;
        appState().setPrefs(p);
        appicons::refreshAll(w.get());
        for (const char *id : {"file.new", "file.open", "file.save", "edit.undo", "edit.redo", "file.import", "file.export"}) {
            QAction *a = w->action(QString::fromLatin1(id));
            QVERIFY2(a, id);
            QVERIFY2(!a->icon().isNull(), id);
            QVERIFY2(!a->icon().pixmap(22, 22).isNull(), id);
        }
    }

    void hueShiftChangesColourKeepsGrey() {
        // Build a test image: red square + grey square
        QImage src(32, 16, QImage::Format_ARGB32_Premultiplied);
        src.fill(0);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                src.setPixel(x, y, qRgba(0xC2, 0x2F, 0x2F, 255));
        for (int y = 0; y < 16; ++y)
            for (int x = 16; x < 32; ++x)
                src.setPixel(x, y, qRgba(0x66, 0x66, 0x66, 255));
        QImage blue = appicons::hueShift(src, 210.0, false); // blue-ish
        QColor c = blue.pixelColor(8, 8);
        QVERIFY2(c.blue() > c.red(), qPrintable(c.name()));
        QColor g = blue.pixelColor(24, 8);
        QCOMPARE(g.red(), g.green());
        QCOMPARE(g.green(), g.blue());
    }

    void prefTogglesLive() {
        Prefs p = appState().prefs;
        p.useSystemIcons = false;
        appState().setPrefs(p);
        appicons::refreshAll(w.get());
        QVERIFY(appicons::isBundled(QStringLiteral("document-open")));
        p.useSystemIcons = true;
        appState().setPrefs(p);
        appicons::refreshAll(w.get());
        QVERIFY(!appicons::isBundled(QStringLiteral("document-open")));
        // icon still non-null (system or fallback)
        QVERIFY(!appicons::icon(QStringLiteral("document-open")).pixmap(16,16).isNull()
                || !appicons::standard(QStyle::SP_DialogOpenButton).pixmap(16,16).isNull());
        p.useSystemIcons = false;
        appState().setPrefs(p);
        appicons::refreshAll(w.get());
        QVERIFY(appicons::isBundled(QStringLiteral("document-open")));
    }

    void themeSwitchTiming() {
        Prefs p = appState().prefs;
        p.useSystemIcons = false;
        p.theme = QStringLiteral("classic");
        appState().setPrefs(p);
        appicons::refreshAll(w.get());
        QElapsedTimer t; t.start();
        p.theme = QStringLiteral("midnight");
        appState().setPrefs(p);
        const int ms = appicons::refreshAll(w.get());
        const qint64 total = t.elapsed();
        qInfo("theme switch + icon refresh: prefs=%lld ms, refreshAll=%d ms", total - ms, ms);
        // Release baseline ~266 ms; ASan/UBSan inflate prefs apply ~3×. Icon refresh itself stays tiny.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
        QVERIFY2(total < 2500, qPrintable(QString::number(total)));
#else
        QVERIFY2(total < 800, qPrintable(QString::number(total)));
#endif
        QVERIFY2(ms < 100, qPrintable(QStringLiteral("refreshAll=%1").arg(ms)));
        // icons look different hue
        QImage a = appicons::icon(QStringLiteral("go-next")).pixmap(32,32).toImage();
        p.theme = QStringLiteral("synthwave");
        appState().setPrefs(p);
        appicons::refreshAll(w.get());
        QImage b = appicons::icon(QStringLiteral("go-next")).pixmap(32,32).toImage();
        QVERIFY(a != b);
    }

    void prefsCheckboxExists() {
        PreferencesDialog dlg(w.get(), 0);
        QVERIFY(dlg.findChild<QCheckBox *>(QStringLiteral("prefUseSystemIcons")));
        QCOMPARE(dlg.findChild<QCheckBox *>(QStringLiteral("prefUseSystemIcons"))->isChecked(), false);
    }
};
QTEST_MAIN(V038)
#include "v038_test.moc"
