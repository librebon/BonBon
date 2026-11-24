#include <QtTest>
#include <QApplication>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QCheckBox>
#include <memory>
#include "Brand.h"
#include "ui/MainWindow.h"
#include "ui/AppState.h"
#include "ui/Theme.h"
#include "ui/PreferencesDialog.h"
#include "ui/Icons.h"

class V039 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;
private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        QApplication::setApplicationName(brand::name());
        QApplication::setOrganizationName(brand::org());
        w = std::make_unique<MainWindow>();
        w->show();
        QApplication::processEvents();
    }
    void brandHeaderValues() {
        QVERIFY(!QStringLiteral(LW_PRODUCT_NAME).isEmpty());
        QCOMPARE(brand::name(), QStringLiteral(LW_PRODUCT_NAME));
        QCOMPARE(brand::nameLower(), QStringLiteral(LW_PRODUCT_NAME_LOWER));
        QCOMPARE(QApplication::applicationName(), brand::name());
        QCOMPARE(QApplication::organizationName(), brand::org());
#if LW_IS_BONBON
        QCOMPARE(brand::name(), QStringLiteral("Bonbon"));
        QCOMPARE(theme::defaultId(), QStringLiteral("bonbon"));
#else
        QCOMPARE(brand::name(), QStringLiteral("Libreworks"));
        QCOMPARE(theme::defaultId(), QStringLiteral("classic"));
#endif
        // Bonbon theme exists in both builds
        QVERIFY(theme::exists(QStringLiteral("bonbon")));
        bool found = false;
        for (const theme::Info &i : theme::list())
            if (i.id == QLatin1String("bonbon") && i.name == QLatin1String("Bonbon")) found = true;
        QVERIFY(found);
    }
    void windowTitleUsesBrand() {
        QVERIFY(w->windowTitle().startsWith(brand::name()));
#if LW_IS_BONBON
        QVERIFY(!w->windowTitle().contains(QStringLiteral("Libreworks")));
#endif
    }
    void aboutUsesBrand() {
        // Trigger About via the action
        QAction *a = w->findChild<QAction *>(QStringLiteral("help.about"));
        QVERIFY(a);
        QVERIFY(a->text().contains(brand::name()));
#if LW_IS_BONBON
        QVERIFY(!a->text().contains(QStringLiteral("Libreworks")));
#endif
    }
    void prefsDefaultTheme() {
        Prefs d;
        QCOMPARE(d.theme, theme::defaultId());
#if LW_IS_BONBON
        QCOMPARE(d.theme, QStringLiteral("bonbon"));
#else
        QCOMPARE(d.theme, QStringLiteral("classic"));
#endif
    }
    void noLibreworksInBonbonUi() {
#if !LW_IS_BONBON
        QSKIP("Libreworks build — UI may contain the product name");
#endif
        // Walk widgets and actions for the forbidden product name
        const QString forbid = QStringLiteral("Libreworks");
        QStringList hits;
        auto check = [&](const QString &where, const QString &text) {
            if (text.contains(forbid, Qt::CaseInsensitive)) hits << where + ": " + text.left(80);
        };
        check("windowTitle", w->windowTitle());
        for (QAction *a : w->findChildren<QAction *>())
            check(QStringLiteral("action:%1").arg(a->objectName()), a->text() + QLatin1Char(' ') + a->toolTip() + QLatin1Char(' ') + a->statusTip());
        for (QWidget *wd : w->findChildren<QWidget *>()) {
            check(QStringLiteral("widget:%1").arg(wd->objectName()), wd->windowTitle() + QLatin1Char(' ') + wd->toolTip());
            if (auto *l = qobject_cast<QLabel *>(wd)) check(QStringLiteral("label:%1").arg(l->objectName()), l->text());
        }
        // Preferences dialog
        PreferencesDialog dlg(w.get(), 0);
        for (QAction *a : dlg.findChildren<QAction *>())
            check(QStringLiteral("prefs-action:%1").arg(a->objectName()), a->text());
        for (QWidget *wd : dlg.findChildren<QWidget *>()) {
            if (auto *l = qobject_cast<QLabel *>(wd)) check(QStringLiteral("prefs-label"), l->text());
            check(QStringLiteral("prefs-tip"), wd->toolTip());
        }
        // Theme combo should list Bonbon, not show Libreworks as a theme name
        QVERIFY(hits.isEmpty() || [&]{ qWarning() << hits; return hits.isEmpty(); }());
        QVERIFY2(hits.isEmpty(), qPrintable(hits.join(QLatin1Char('\n'))));
    }
    void appIconNonNull() {
        QVERIFY(!icons::appIcon(256).isNull());
        QVERIFY(!icons::appIcon(64).isNull());
        QVERIFY(!icons::appIcon(32).isNull());
    }
    void qsettingsAppName() {
        QCOMPARE(QSettings().applicationName(), brand::name());
        QCOMPARE(QSettings().organizationName(), brand::org());
    }
};
QTEST_MAIN(V039)
#include "v039_test.moc"
