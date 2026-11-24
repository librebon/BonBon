// Theme switch A→B must be pixel-identical to a fresh launch in B,
// for the whole main window, a floating panel, and an open menu.
#include "ui/Theme.h"
#include "ui/AppState.h"
#include "ui/MainWindow.h"
#include "core/Document.h"
#include <QApplication>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QLabel>
#include <QToolTip>
#include <QStatusBar>
#include <QDir>
#include <QtTest>

static void drain() {
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

static void applyTheme(const QString &id) {
    Prefs p = appState().prefs;
    p.themeFollowSystem = false;
    p.theme = id;
    appState().setPrefs(p);
}

static int changedPixels(const QImage &aIn, const QImage &bIn) {
    QImage a = aIn.convertToFormat(QImage::Format_RGB32);
    QImage b = bIn.convertToFormat(QImage::Format_RGB32);
    if (a.size() != b.size())
        return -1;
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            if (pa[x] != pb[x]) ++n;
    }
    return n;
}

struct Scene {
    MainWindow *win = nullptr;
    QDockWidget *floater = nullptr;
    QMenu *menu = nullptr;
};

// Build a typical working window: document open, panels docked, one
// panel floated, first menu bar menu popped up.
static Scene openScene() {
    Scene s;
    s.win = new MainWindow;
    s.win->resize(1280, 800);
    s.win->addDocument(Document::createNew(320, 240, Qt::white));
    s.win->show();
    drain();
    QTest::qWait(40);
    drain();

    if (auto *propDock = s.win->findChild<QDockWidget *>(QStringLiteral("PropertiesDock"))) {
        propDock->show();
        propDock->raise();
    }
    if (auto *toolsDock = s.win->findChild<QDockWidget *>(QStringLiteral("ToolsDock"))) {
        toolsDock->show();
        toolsDock->raise();
    }
    if (auto *panelsDock = s.win->findChild<QDockWidget *>(QStringLiteral("PanelsDock"))) {
        panelsDock->show();
        panelsDock->setFloating(true);
        panelsDock->resize(280, 480);
        panelsDock->move(40, 40);
        panelsDock->show();
        panelsDock->raise();
        s.floater = panelsDock;
    }
    drain();
    QTest::qWait(40);
    drain();

    if (QMenuBar *mb = s.win->menuBar()) {
        for (QAction *a : mb->actions()) {
            if (!a->menu()) continue;
            a->menu()->popup(mb->mapToGlobal(mb->actionGeometry(a).bottomLeft()));
            s.menu = a->menu();
            break;
        }
    }
    drain();
    QTest::qWait(30);
    drain();
    return s;
}


static void waitForEstimate(Scene &s) {
    if (!s.floater) return;
    for (int i = 0; i < 30; ++i) {
        bool ready = false;
        for (QLabel *lab : s.floater->findChildren<QLabel *>()) {
            if (lab->isVisible() && lab->text().startsWith(QLatin1String("Estimated:"))) {
                ready = true;
                break;
            }
        }
        if (ready) break;
        QTest::qWait(20);
        drain();
    }
}

static void reopenMenu(Scene &s) {
    if (!s.menu || s.menu->isVisible() || !s.win) return;
    if (QMenuBar *mb = s.win->menuBar()) {
        for (QAction *a : mb->actions()) {
            if (a->menu() == s.menu) {
                s.menu->popup(mb->mapToGlobal(mb->actionGeometry(a).bottomLeft()));
                break;
            }
        }
    }
    drain();
    QTest::qWait(20);
    drain();
}

static void closeScene(Scene &s) {
    if (s.menu) s.menu->hide();
    if (s.floater) s.floater->hide();
    if (s.win) {
        s.win->hide();
        delete s.win;
    }
    s = Scene();
    drain();
}

struct Grab {
    QImage window, floating, menu;
};

static void hideLeftoverTips() {
    QToolTip::hideText();
    for (QWidget *x : QApplication::allWidgets()) {
        if (x && x->inherits("QTipLabel")) { x->hide(); x->close(); }
    }
}

// The Tools-panel default ("Pointer") is shown with a 3 s timeout. Under
// ASan, grabScene can take longer than that, so two stable grabs after
// the message has faded compare a blank status bar to a fresh launch
// that still had "Pointer" (the 202 px synthwave→graphite flake).
static void pinToolStatus(Scene &s)
{
    if (!s.win || !s.win->statusBar()) return;
    s.win->statusBar()->showMessage(toolName(appState().tool()));
}

// Theme::apply() unpolishes and re-polishes synchronously, but a switch can
// still post a follow-up StyleChange (icon refresh), a 0-timer panel rebuild,
// or a tooltip/popup palette tweak. Grab repeatedly until two passes match
// so a deferred paint is not compared against a fresh launch.
static Grab grabScene(Scene &s) {
    if (s.win) {
        s.win->activateWindow();
        s.win->setFocus(Qt::OtherFocusReason);
    }
    pinToolStatus(s);
    if (s.floater) {
        s.floater->show();
        s.floater->raise();
    }
    hideLeftoverTips();
    drain();
    waitForEstimate(s);
    reopenMenu(s);
    drain();
    Grab prev, g;
    for (int i = 0; i < 20; ++i) {
        hideLeftoverTips();
        reopenMenu(s);
        waitForEstimate(s);
        if (s.win) g.window = s.win->grab().toImage();
        if (s.floater && s.floater->isVisible())
            g.floating = s.floater->grab().toImage();
        if (s.menu && s.menu->isVisible())
            g.menu = s.menu->grab().toImage();
        if (i > 0
            && changedPixels(prev.window, g.window) == 0
            && changedPixels(prev.floating, g.floating) == 0
            && changedPixels(prev.menu, g.menu) == 0)
            return g;
        prev = g;
        QTest::qWait(30);
        drain();
    }
    return g;
}

class V065 : public QObject {
    Q_OBJECT
private slots:
    void switchMatchesFresh() {
        auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(app);

        QStringList ids;
        for (const theme::Info &info : theme::list()) ids << info.id;
        QVERIFY(ids.size() >= 2);

        // Fresh launch: apply B before the window is created so the first
        // polish resolves palette() against B. The old chrome-only test
        // applied B to a window that had already been polished in the
        // default theme, so a broken switch matched a equally-broken "fresh".
        QMap<QString, Grab> fresh;
        for (const QString &id : ids) {
            applyTheme(id);
            drain();
            Scene s = openScene();
            fresh.insert(id, grabScene(s));
            QVERIFY2(!fresh[id].window.isNull(), qPrintable("fresh window missing for " + id));
            QVERIFY2(!fresh[id].floating.isNull(), qPrintable("fresh floating panel missing for " + id));
            QVERIFY2(!fresh[id].menu.isNull(), qPrintable("fresh menu missing for " + id));
            closeScene(s);
        }

        int failed = 0;
        QString report;
        const QString failDir = QStringLiteral("/workspace/review-0.3.29/v065-fail");
        auto check = [&](const QString &pair, const char *name, const QImage &got, const QImage &ref) {
            const int n = changedPixels(got, ref);
            if (n != 0) {
                ++failed;
                report += QStringLiteral("  %1 %2 %3\n")
                              .arg(pair, QString::fromLatin1(name),
                                   n < 0 ? QStringLiteral("SIZE") : QString::number(n) + QStringLiteral(" px"));
                QDir().mkpath(failDir);
                const QString stem = failDir + QLatin1Char('/') + pair + QLatin1Char('-') + QString::fromLatin1(name);
                got.save(stem + QStringLiteral("-got.png"));
                ref.save(stem + QStringLiteral("-ref.png"));
            }
        };

        for (const QString &a : ids) {
            for (const QString &b : ids) {
                if (a == b) continue;
                // Window is first polished in A (like a launch in A), then
                // switched to B at runtime, and compared to a fresh B launch.
                applyTheme(a);
                drain();
                Scene s = openScene();
                applyTheme(b);
                drain();
                QTest::qWait(40);
                drain();
                const Grab got = grabScene(s);
                const Grab &ref = fresh[b];
                const QString pair = a + QStringLiteral("->") + b;
                check(pair, "window", got.window, ref.window);
                check(pair, "floating", got.floating, ref.floating);
                check(pair, "menu", got.menu, ref.menu);
                closeScene(s);
            }
        }
        QVERIFY2(failed == 0, qPrintable(QStringLiteral("%1 mismatches:\n%2").arg(failed).arg(report)));
    }
};


// QtTest's default watchdog is 5 minutes; the full-window pair loop
// can exceed that under ASan, so raise it before QTEST_MAIN starts.
static const bool kRaiseTestTimeout = [] {
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT"))
        qputenv("QTEST_FUNCTION_TIMEOUT", "900000");
    return true;
}();

QTEST_MAIN(V065)
#include "v065_test.moc"
