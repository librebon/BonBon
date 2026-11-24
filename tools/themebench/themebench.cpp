// Theme-switch benchmark: main window with the sample document and every right-hand panel expanded,
// then Preferences-style theme changes (appState().setPrefs) timed until the event loop is idle.
#include "ui/AppState.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Panels.h"
#include "core/Document.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QProxyStyle>
#include <QStyle>
#include <QTimer>
#include <QWidget>
#include <QComboBox>
#include <QListView>
#include <QAbstractItemView>
#include <algorithm>
#include <cstdio>

static int styleDepth(QStyle *s) {
    int n = 0;
    while (auto *p = qobject_cast<QProxyStyle *>(s)) { ++n; s = p->baseStyle(); }
    return n;
}
static void idle() {
    // flush posted events (polish, LayoutRequest, UpdateRequest, deferred deletes) and repaint
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    for (QWidget *w : QApplication::topLevelWidgets()) if (w->isVisible()) w->repaint();
    QCoreApplication::processEvents();
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int rounds = argc > 1 ? atoi(argv[1]) : 3;
    Prefs p0 = appState().prefs;
    p0.theme = QStringLiteral("classic");
    p0.themeFollowSystem = false;
    p0.accentOverride = false;
    appState().setPrefs(p0);
    MainWindow w;
    w.resize(1400, 900);
    w.addDocument(Document::createSample());
    w.show();
    for (const char *t : {"Optimize", "Layers", "Frames", "History", "Align", "Styles", "Library", "Auto Shapes"})
        if (auto *cp = w.panel(QString::fromLatin1(t))) cp->setCollapsed(false);
    if (qEnvironmentVariableIsSet("LW_BENCH_EMPTY_FONTBOXES"))   // experiment: the Find & Replace font lists
        for (QComboBox *c : w.findChildren<QComboBox *>()) if (c->count() > 200) {
                qInfo("%s %s (%d items, view %s)", qgetenv("LW_BENCH_EMPTY_FONTBOXES").constData(), qPrintable(c->objectName()), c->count(), c->view()->metaObject()->className());
                if (qgetenv("LW_BENCH_EMPTY_FONTBOXES") == "clear") c->clear();
                else if (auto *lv = qobject_cast<QListView *>(c->view())) lv->setUniformItemSizes(true);
            }
    idle();
    const QStringList ids = {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("nord"), QStringLiteral("midnight"), QStringLiteral("classic")};
    QVector<double> ms;
    int widgets = QApplication::allWidgets().size();
    for (int r = 0; r < rounds; ++r)
        for (const QString &id : ids) {
            Prefs p = appState().prefs;
            p.theme = id;
            QElapsedTimer t;
            t.start();
            appState().setPrefs(p);
            idle();
            ms << t.nsecsElapsed() / 1e6;
        }
    QVector<double> s = ms;
    std::sort(s.begin(), s.end());
    double sum = 0; for (double v : ms) sum += v;
    printf("switches=%d widgets=%d median=%.1f mean=%.1f min=%.1f max=%.1f proxyDepth=%d style=%s\n", int(ms.size()), widgets,
           s[s.size() / 2], sum / ms.size(), s.first(), s.last(), styleDepth(app.style()), app.style()->metaObject()->className());
    printf("styles alive=%d\n", int(app.findChildren<QStyle *>().size()));
    printf("all:"); for (double v : ms) printf(" %.0f", v); printf("\n");
    return 0;
}
