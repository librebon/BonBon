#include "Brand.h"
#include <unistd.h>
#include <QMenuBar>
#include <QDialog>
#include "io/Codecs.h"
#include "core/Procedural.h"
#include "core/Document.h"
#include "core/BrushEngine.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/Icons.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include "ui/AppState.h"
#include "ui/Panels.h"
#include "ui/ToolsPanel.h"
#include "ui/PopupSlider.h"
#include <QMenu>
#include <QPointer>
#include <QPainter>
#include <QFileInfo>
#include <QScreen>
#include <QTimer>
#include <cstdio>
#include "core/SelectionMask.h"
#include <QPainterPath>
#include "io/Batch.h"
#include <QGuiApplication>
#include <cstring>

// v0.3.0: `libreworks --batch script.json [files...]` runs a Batch Process script headless.
static int runBatchCli(int argc, char **argv) {
    // no display needed: fall back to the offscreen platform (text still renders)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(brand::name());
    QGuiApplication::setOrganizationName(brand::org());
    QGuiApplication::setApplicationVersion(QStringLiteral(LW_VERSION));
    QGuiApplication::setDesktopFileName(brand::desktopId());
    QGuiApplication::setWindowIcon(icons::appIconIcon());
    codec::heifInitPlugins();
    QCommandLineParser cli;
    cli.setApplicationDescription(brand::name() + QStringLiteral(" batch processing"));
    cli.addHelpOption();
    QCommandLineOption batchOpt(QStringLiteral("batch"), QStringLiteral("Run the Batch Process script <script.json> on [files...] (or its own inputs)."), QStringLiteral("script.json"));
    QCommandLineOption outOpt(QStringLiteral("out"), QStringLiteral("Override the output folder."), QStringLiteral("dir"));
    QCommandLineOption quietOpt(QStringLiteral("quiet"), QStringLiteral("Only print errors."));
    cli.addOptions({batchOpt, outOpt, quietOpt});
    cli.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Files to process."), QStringLiteral("[files...]"));
    cli.process(app);
    QString err;
    bool ok = false;
    batch::Script s = batch::Script::load(cli.value(batchOpt), &err, &ok);
    if (!ok) { fprintf(stderr, "batch: %s: %s\n", qPrintable(cli.value(batchOpt)), qPrintable(err)); return 2; }
    if (!err.isEmpty()) fprintf(stderr, "batch: %s\n", qPrintable(err));
    if (cli.isSet(outOpt)) { s.output = batch::Script::ToFolder; s.outDir = QFileInfo(cli.value(outOpt)).absoluteFilePath(); }
    QStringList files;
    for (const QString &f : cli.positionalArguments()) files << QFileInfo(f).absoluteFilePath();
    if (files.isEmpty()) files = batch::inputFiles(s);
    if (files.isEmpty()) { fprintf(stderr, "batch: no input files\n"); return 2; }
    const bool quiet = cli.isSet(quietOpt);
    const batch::Result r = batch::run(s, files, [quiet](int i, int n, const QString &f) {
        if (!quiet && !f.isEmpty()) fprintf(stderr, "[%d/%d] %s\n", i + 1, n, qPrintable(f));
        return true;
    });
    if (!quiet) for (const QString &l : r.log) printf("%s\n", qPrintable(l));
    for (const QString &e : r.errors) fprintf(stderr, "error: %s\n", qPrintable(e));
    if (!quiet) printf("batch: %d processed, %d failed, %lld files written\n", r.processed, r.failed, qlonglong(r.written.size()));
    return r.failed ? 1 : 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--batch") == 0 || std::strncmp(argv[i], "--batch=", 8) == 0) return runBatchCli(argc, argv);
    QApplication app(argc, argv);
    QApplication::setApplicationName(brand::name());
    QApplication::setOrganizationName(brand::org());
    QApplication::setApplicationVersion(QStringLiteral(LW_VERSION));
    QApplication::setDesktopFileName(brand::desktopId());
    // Multi-size brand icon for Alt-Tab / taskbar / every window (before any widget is shown).
    QApplication::setWindowIcon(icons::appIconIcon());
    codec::heifInitPlugins();   // HEIC: libheif plugins (bundled ones first in the AppImage / tarball)
    theme::apply(app);
    appState().loadPrefs();
    appState().loadToolOptions();
    if (appState().prefs.carryToolSettings) appState().loadCarriedEnvironment();

    QCommandLineParser cli;
    cli.setApplicationDescription(brand::name() + QStringLiteral(" - a Fireworks MX 2004 style image editor for Linux"));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption sample(QStringLiteral("sample"), QStringLiteral("Open the built-in sample document."));
    QCommandLineOption shot(QStringLiteral("screenshot"), QStringLiteral("Save a screenshot of the main window to <path> and quit."), QStringLiteral("path"));
    QCommandLineOption size(QStringLiteral("size"), QStringLiteral("Window size WxH (default 1400x900)."), QStringLiteral("WxH"));
    QCommandLineOption mode(QStringLiteral("view"), QStringLiteral("Initial view tab: original, preview, 2up or 4up."), QStringLiteral("mode"));
    QCommandLineOption writeIcon(QStringLiteral("write-icon"), QStringLiteral("Write the application icon PNG to <path> and quit."), QStringLiteral("path"));
    QCommandLineOption selectOpt(QStringLiteral("select"), QStringLiteral("Select the object named <name> (screenshots)."), QStringLiteral("name"));
    QCommandLineOption expandOpt(QStringLiteral("expand"), QStringLiteral("Comma-separated panels to expand, e.g. Styles,Align (screenshots)."), QStringLiteral("panels"));
    QCommandLineOption collapseOpt(QStringLiteral("collapse"), QStringLiteral("Comma-separated panels to collapse (screenshots)."), QStringLiteral("panels"));
    QCommandLineOption flyoutOpt(QStringLiteral("flyout"), QStringLiteral("Open the Tools panel flyout of <tool> (e.g. Blur) before the screenshot."), QStringLiteral("tool"));
    QCommandLineOption sliderOpt(QStringLiteral("slider"), QStringLiteral("Open the pop-up slider of the field named <name> before the screenshot."), QStringLiteral("name"));
    QCommandLineOption toolOpt(QStringLiteral("tool"), QStringLiteral("Select the tool named <tool> (e.g. Brush, Dodge)."), QStringLiteral("tool"));
    QCommandLineOption newOpt(QStringLiteral("new"), QStringLiteral("Open a new empty document of size WxH (pixels)."), QStringLiteral("WxH"));
    QCommandLineOption zoomOpt(QStringLiteral("zoom"), QStringLiteral("Initial canvas zoom (e.g. 0.5, 1, 2 or 50%, 100%, 200%)."), QStringLiteral("zoom"));
    QCommandLineOption tipOpt(QStringLiteral("tip"), QStringLiteral("Select a Brush tip by stroke key (e.g. unnatural.alienPaint)."), QStringLiteral("key"));
    QCommandLineOption brushSizeOpt(QStringLiteral("brush-size"), QStringLiteral("Brush size in pixels (default 6)."), QStringLiteral("px"));
    QCommandLineOption colorOpt(QStringLiteral("color"), QStringLiteral("Stroke colour, e.g. #FF6699."), QStringLiteral("color"));
    QCommandLineOption texSheet(QStringLiteral("texture-sheet"), QStringLiteral("Write a contact sheet of the procedural textures to <path> and quit."), QStringLiteral("path"));
    cli.addOption(texSheet);
    QCommandLineOption qmOpt(QStringLiteral("quickmask"), QStringLiteral("Make a feathered elliptical selection and enter Quick Mask before the screenshot."));
    cli.addOption(qmOpt);
    QCommandLineOption themeOpt(QStringLiteral("theme"), QStringLiteral("Use the interface theme <id> for this session (not saved), e.g. dark, nord."), QStringLiteral("id"));
    cli.addOption(themeOpt);
    QCommandLineOption batchHelp(QStringLiteral("batch"), QStringLiteral("Run the Batch Process script <script.json> headless on [files...] and quit."), QStringLiteral("script.json"));
    cli.addOption(batchHelp);
    QCommandLineOption actionOpt(QStringLiteral("action"), QStringLiteral("Trigger the menu command <Menu/Sub/Item> before the screenshot; an opened dialog is captured too."), QStringLiteral("path"));
    cli.addOption(actionOpt);
    QCommandLineOption menuOpt(QStringLiteral("menu"), QStringLiteral("Open the menu <Menu[/Sub]> before the screenshot."), QStringLiteral("path"));
    cli.addOption(menuOpt);
    cli.addOptions({sample, shot, size, mode, writeIcon, selectOpt, expandOpt, collapseOpt, flyoutOpt, sliderOpt, toolOpt, newOpt, zoomOpt, tipOpt, brushSizeOpt, colorOpt});
    cli.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Documents or images to open."), QStringLiteral("[files...]"));
    cli.process(app);

    if (cli.isSet(texSheet)) return proc::contactSheet().save(cli.value(texSheet)) ? 0 : 1;
    if (cli.isSet(writeIcon)) return icons::appIcon(256).save(cli.value(writeIcon)) ? 0 : 1;

    if (cli.isSet(themeOpt)) {
        const QString id = cli.value(themeOpt);
        if (!theme::exists(id)) {
            QStringList ids;
            for (const theme::Info &i : theme::list()) ids << i.id;
            fprintf(stderr, "unknown theme '%s'; available: %s\n", qPrintable(id), qPrintable(ids.join(QStringLiteral(", "))));
            return 2;
        }
        Prefs p = appState().prefs;
        p.theme = id;
        p.themeFollowSystem = false;
        appState().setPrefs(p);
    }

    MainWindow w;
    // 1400 x 900, or less on a smaller screen (e.g. 1366 x 768): the canvas gives up the height
    QSize sz(1400, 900);
    if (const QScreen *scr = QGuiApplication::primaryScreen()) sz = sz.boundedTo(scr->availableSize() - QSize(0, 40));
    if (cli.isSet(size)) {
        const QStringList p = cli.value(size).split(QLatin1Char('x'));
        if (p.size() == 2) sz = QSize(p[0].toInt(), p[1].toInt());
    }
    w.resize(sz);
    for (const QString &f : cli.positionalArguments()) w.openFile(QFileInfo(f).absoluteFilePath());
    if (cli.isSet(sample)) w.addDocument(Document::createSample());
    if (cli.isSet(newOpt)) {
        const QStringList p = cli.value(newOpt).split(QLatin1Char('x'));
        if (p.size() == 2) {
            const int nw = p[0].toInt(), nh = p[1].toInt();
            if (nw > 0 && nh > 0)
                w.addDocument(Document::createNew(nw, nh, Qt::white));
        }
    }
    if (cli.isSet(toolOpt)) {
        const QString tn = cli.value(toolOpt);
        for (int t = 0; t <= int(kLastTool); ++t)
            if (toolName(Tool(t)).compare(tn, Qt::CaseInsensitive) == 0) {
                appState().setTool(Tool(t));
                break;
            }
    }
    if (cli.isSet(tipOpt)) {
        const QString want = cli.value(tipOpt);
        bool found = false;
        for (int c = 0; c <= int(brush::Category::Libreworks) && !found; ++c) {
            const auto cat = brush::Category(c);
            const QStringList names = brush::tipNames(cat);
            for (int i = 0; i < names.size(); ++i) {
                const QString key = brush::tipStrokeKey(cat, i);
                if (key.compare(want, Qt::CaseInsensitive) == 0
                    || names[i].compare(want, Qt::CaseInsensitive) == 0) {
                    appState().brushCategory = c;
                    appState().brushTip = i;
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            fprintf(stderr, "unknown --tip %s\n", qPrintable(want));
    }
    if (cli.isSet(brushSizeOpt)) {
        bool ok = false;
        const int s = cli.value(brushSizeOpt).toInt(&ok);
        if (ok && s > 0)
            appState().brushSize = qBound(1, s, 800);
    }
    if (cli.isSet(colorOpt)) {
        const QColor c(cli.value(colorOpt));
        if (c.isValid())
            appState().setStroke(c);
    }
    w.show();
    if (cli.isSet(zoomOpt) && w.currentView()) {
        QString z = cli.value(zoomOpt);
        if (z.endsWith(QLatin1Char('%'))) z.chop(1);
        const double zv = z.toDouble() > 4.0 ? z.toDouble() / 100.0 : z.toDouble();
        if (zv > 0)
            w.currentView()->setZoom(zv);
    }
    for (const QString &p : cli.value(expandOpt).split(QLatin1Char(','), Qt::SkipEmptyParts)) if (auto *cp = w.panel(p)) cp->setCollapsed(false);
    for (const QString &p : cli.value(collapseOpt).split(QLatin1Char(','), Qt::SkipEmptyParts)) if (auto *cp = w.panel(p)) cp->setCollapsed(true);

    if (cli.isSet(mode) && w.currentView()) {
        const QString m = cli.value(mode).toLower();
        int idx = m == QLatin1String("preview") ? 1 : m == QLatin1String("2up") ? 2 : m == QLatin1String("4up") ? 3 : 0;
        QTimer::singleShot(100, &w, [&w, idx] { if (w.currentView()) w.currentView()->setMode(idx); });
    }
    if (cli.isSet(shot)) {
        const QString path = cli.value(shot);
        QTimer::singleShot(1500, &w, [&w, &cli, path] {
            const QString selName = cli.value(QStringLiteral("select"));
            if (auto *v = w.currentView(); v && !selName.isEmpty()) {
                Document *d = v->doc();
                for (int li = 0; li < d->s.layers.size(); ++li) for (const VObject &o : d->objects(li)) if (o.name == selName) d->selectOnly(o.id);
            } else if (auto *view = w.currentView()) { view->doc()->selectOnly(view->doc()->s.frames[view->doc()->curFrame].layers.last().isEmpty() ? 0 : view->doc()->s.frames[view->doc()->curFrame].layers.last().last().id); }
            if (cli.isSet(QStringLiteral("quickmask")) && w.currentView()) {
                Document *d = w.currentView()->doc();
                const QSize cs(d->s.width, d->s.height);
                QPainterPath e;
                e.addEllipse(QRectF(cs.width() * 0.08, cs.height() * 0.12, cs.width() * 0.5, cs.height() * 0.62));
                d->setSelectionMask(selmask::feather(selmask::fromPath(e, cs, QPoint(), true), 24), QPoint());
                w.toggleQuickMask();
            }
            if (cli.isSet(QStringLiteral("tool"))) {
                const QString tn = cli.value(QStringLiteral("tool"));
                for (int t = 0; t <= int(kLastTool); ++t)
                    if (toolName(Tool(t)).compare(tn, Qt::CaseInsensitive) == 0) appState().setTool(Tool(t));
            }
            FlyoutToolButton *fly = nullptr;
            QPointer<QWidget> popup;
            QCoreApplication::processEvents();
            if (cli.isSet(QStringLiteral("slider"))) {
                const QString sn = cli.value(QStringLiteral("slider"));
                for (QAbstractSpinBox *sb : w.findChildren<QAbstractSpinBox *>(sn)) {
                    auto *t = dynamic_cast<SliderTarget *>(sb);
                    if (t && sb->isVisible()) { t->openPopup(false); popup = t->popup(); break; }
                }
            }
            if (cli.isSet(QStringLiteral("flyout"))) {
                fly = w.findChild<FlyoutToolButton *>(QStringLiteral("tool_") + cli.value(QStringLiteral("flyout")));
                if (fly) fly->showFlyout();
            }
            auto clean = [](QString t) { return t.remove(QLatin1Char('&')).remove(QStringLiteral("...")).trimmed(); };
            auto findMenu = [&w, clean](const QStringList &parts, int n) -> QMenu * {
                QMenu *m = nullptr;
                QList<QAction *> acts = w.menuBar()->actions();
                for (int i = 0; i < n; ++i) {
                    QMenu *next = nullptr;
                    for (QAction *a : acts) if (a->menu() && clean(a->text()).compare(clean(parts[i]), Qt::CaseInsensitive) == 0) { next = a->menu(); break; }
                    if (!next) return nullptr;
                    m = next;
                    acts = m->actions();
                }
                return m;
            };
            QList<QPointer<QMenu>> menus;
            if (cli.isSet(QStringLiteral("menu"))) {
                const QStringList parts = cli.value(QStringLiteral("menu")).split(QLatin1Char('/'));
                QPoint at;
                for (int i = 1; i <= parts.size(); ++i) {
                    QMenu *m = findMenu(parts, i);
                    if (!m) break;
                    if (i == 1) {
                        for (QAction *a : w.menuBar()->actions()) if (a->menu() == m) at = w.menuBar()->mapToGlobal(w.menuBar()->actionGeometry(a).bottomLeft());
                    } else if (!menus.isEmpty()) {
                        QMenu *parent = menus.last();
                        for (QAction *a : parent->actions()) if (a->menu() == m) { parent->setActiveAction(a); at = parent->mapToGlobal(parent->actionGeometry(a).topRight()); }
                    }
                    m->popup(at);
                    QCoreApplication::processEvents();
                    menus << m;
                }
            }
            if (cli.isSet(QStringLiteral("action"))) {
                const QStringList parts = cli.value(QStringLiteral("action")).split(QLatin1Char('/'));
                QMenu *m = parts.size() > 1 ? findMenu(parts, parts.size() - 1) : nullptr;
                QAction *act = nullptr;
                const QList<QAction *> cands = m ? m->actions() : w.findChildren<QAction *>();
                for (QAction *a : cands) if (!a->menu() && clean(a->text()).compare(clean(parts.last()), Qt::CaseInsensitive) == 0) { act = a; break; }
                if (!act) { fprintf(stderr, "action not found: %s\n", qPrintable(cli.value(QStringLiteral("action")))); QCoreApplication::exit(1); return; }
                QTimer::singleShot(900, &w, [&w, path] {
                    QWidget *dlg = QApplication::activeModalWidget();
                    if (!dlg) for (QWidget *tw : QApplication::topLevelWidgets()) if (tw != &w && tw->isVisible() && qobject_cast<QDialog *>(tw)) dlg = tw;
                    QDir().mkpath(QFileInfo(path).absolutePath());
                    QPixmap pm = w.grab();
                    if (dlg && dlg != &w) {
                        QPixmap dp = dlg->grab();
                        QPainter p(&pm);
                        QPoint at((pm.width() - dp.width()) / 2, qMax(0, (pm.height() - dp.height()) / 2));
                        p.fillRect(QRect(at + QPoint(4, 4), dp.size()), QColor(0, 0, 0, 70));
                        p.drawPixmap(at, dp);
                        p.setPen(QColor(90, 90, 90));
                        p.drawRect(QRect(at, dp.size()).adjusted(0, 0, -1, -1));
                    }
                    bool ok = pm.save(path);
                    fprintf(stderr, "screenshot %s: %s (%dx%d)%s\n", ok ? "saved" : "FAILED", qPrintable(path), pm.width(), pm.height(), dlg ? " +dialog" : "");
                    fflush(stderr);
                    ::_exit(ok ? 0 : 1);
                });
                act->trigger();
                return;
            }
            QTimer::singleShot(500, &w, [&w, path, fly, popup, menus] {
                QDir().mkpath(QFileInfo(path).absolutePath());
                QPixmap pm = w.grab();
                if (fly && fly->flyout()->isVisible()) {
                    // popups are separate windows: composite the open flyout onto the grab
                    QPainter p(&pm);
                    QMenu *m = fly->flyout();
                    p.drawPixmap(w.mapFromGlobal(m->pos()), m->grab());
                }
                if (popup && popup->isVisible()) {
                    QPainter p(&pm);
                    p.drawPixmap(w.mapFromGlobal(popup->pos()), popup->grab());
                }
                for (const QPointer<QMenu> &m : menus) if (m && m->isVisible()) {
                    QPainter p(&pm);
                    p.drawPixmap(w.mapFromGlobal(m->pos()), m->grab());
                }
                bool ok = pm.save(path);
                fprintf(stderr, "screenshot %s: %s (%dx%d)\n", ok ? "saved" : "FAILED", qPrintable(path), pm.width(), pm.height());
                QCoreApplication::exit(ok ? 0 : 1);
            });
        });
    }
    return app.exec();
}
