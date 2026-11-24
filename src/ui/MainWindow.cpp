#include <QMap>
#include "MainWindow.h"
#include "Brand.h"
#include <QKeySequenceEdit>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTextEdit>
#include <QAbstractSpinBox>
#include <QComboBox>
#include "Shortcuts.h"
#include "PreferencesDialog.h"
#include "PopupSlider.h"
#include "ZoomField.h"
#include "io/HtmlExport.h"
#include "io/DocIO.h"
#include "FaviconDialog.h"
#include "LiquifyDialog.h"
#include "TraceDialog.h"
#include "PixelUpscaleDialog.h"
#include "core/Trace.h"
#include "BatchDialog.h"
#include "core/FxExtra.h"
#include <QProgressDialog>
#include <QVBoxLayout>
#include "Canvas.h"
#include "Dialogs.h"
#include "ClipboardImage.h"
#include "DocumentView.h"
#include "Icons.h"
#include "AppIcons.h"
#include "Panels.h"
#include "PropertyInspector.h"
#include "ProceduralDialog.h"
#include "core/Procedural.h"
#include "ToolsPanel.h"
#include "core/Paths.h"
#include "core/Fills.h"
#include "core/Filters.h"
#include "core/SelectionMask.h"
#include <QListWidget>
#include <QPushButton>
#include "core/Effects.h"
#include "core/Render.h"
#include "FindPanel.h"
#include "core/Transform.h"
#include "io/LwdFormat.h"
#include <QActionGroup>
#include <algorithm>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <memory>
#include <functional>
#include <QTextEdit>
#include <QTimer>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <cmath>
#include <QApplication>
#include <QCursor>
#include <QClipboard>
#include "ColorPicker.h"
#include <QDir>
#include <QtMath>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include "FileDialogs.h"
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QScrollArea>
#include <QSettings>
#include "PreferencesDialog.h"
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>
#include "core/PixelEdit.h"

// Ends the document's open pixel edits (PixelEdit.h) when an operation returns without committing
// (Copy, Cancel, nothing to do): bitmaps go back to their own geometry unchanged. After a commit it
// does nothing.
struct PixelEditGuard {
    QPointer<Document> d;
    explicit PixelEditGuard(Document *doc) : d(doc) {}
    ~PixelEditGuard() { if (d) d->endPixelEdits(false); }
    PixelEditGuard(const PixelEditGuard &) = delete;
    PixelEditGuard &operator=(const PixelEditGuard &) = delete;
};

namespace {
const char *kObjMime = "application/x-libreworks-objects";
QVector<VObject> g_clipboard;
QSize g_clipboardSize;      // pixel size of the last Copy (File > New sizes the canvas from it)
double g_clipboardDpi = 72; // resolution of the document it came from

using xf::transformObject;

QIcon stdIcon(QStyle::StandardPixmap sp) { return appicons::standard(sp); }
}

MainWindow::MainWindow() {
    PopupSliderHub::installGlobalAdoption();   // Qt's own dialogs (colour, input) get pop-up slider fields too
    clipimage::setInternalProvider([] {
        clipimage::Internal in;
        if (!g_clipboard.isEmpty()) { in.size = g_clipboardSize; in.dpi = g_clipboardDpi; }
        return in;
    });
    setWindowTitle(brand::name());
    setWindowIcon(icons::appIconIcon());
    setAcceptDrops(true);
    // One pop-up slider drag (any field in the main window or its panels) = one undo step.
    connect(PopupSliderHub::instance(), &PopupSliderHub::interactionStarted, this, [this](QWidget *f) {
        if (!f || !isAncestorOf(f) || m_sliderMergeDoc) return;
        if ((m_sliderMergeDoc = currentDoc())) m_sliderMergeDoc->beginMerge();
    });
    connect(PopupSliderHub::instance(), &PopupSliderHub::interactionFinished, this, [this](QWidget *) {
        if (m_sliderMergeDoc) {
            const auto how = PopupSliderHub::instance()->consumeCancelled()
                ? Document::MergeEnd::Cancel : Document::MergeEnd::KeepIfChanged;
            m_sliderMergeDoc->endMerge(how);
        }
        m_sliderMergeDoc = nullptr;
    });
    setDockNestingEnabled(false);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("docTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    // document tabs: full titles, scroll buttons only when the tabs really overflow
    m_tabs->setElideMode(Qt::ElideNone);
    m_tabs->tabBar()->setElideMode(Qt::ElideNone);
    auto *empty = new QLabel(QStringLiteral("%1 %2\n\nFile > New (Ctrl+N) or File > Open (Ctrl+O) to begin.").arg(brand::name(), QStringLiteral(LW_VERSION)));
    empty->setAlignment(Qt::AlignCenter);
    empty->setObjectName(QStringLiteral("EmptyHint"));   // themed (#EmptyHint)
    auto *central = new QWidget;
    auto *cl = new QVBoxLayout(central);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(m_tabs);
    cl->addWidget(empty);
    setCentralWidget(central);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, empty] {
        empty->setVisible(m_tabs->count() == 0);
        m_tabs->setVisible(m_tabs->count() > 0);
        currentChanged();
    });
    m_tabs->setVisible(false);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);

    buildDocks();
    buildMenus();
    {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.captureDefaults(this);
        QVector<shortcuts::ActionInfo> cat;
        auto add = [&](const QString &id, const QString &label, const QString &category, const QString &menuPath = {}) {
            shortcuts::ActionInfo a; a.id = id; a.label = label; a.category = category; a.menuPath = menuPath; cat.append(a);
        };
        for (QAction *a : findChildren<QAction *>()) {
            if (a->objectName().isEmpty() || a->text().isEmpty()) continue;
            QString text = a->text(); text.remove(QLatin1Char('&'));
            if (a->objectName().startsWith(QLatin1String("tool.")))
                add(a->objectName(), text, QStringLiteral("tools"));
            else if (a->objectName().startsWith(QLatin1String("colors.")) || a->objectName().startsWith(QLatin1String("view."))
                     || a->objectName().startsWith(QLatin1String("canvas."))
                     || a->objectName().startsWith(QLatin1String("liquify."))
                     || a->objectName().startsWith(QLatin1String("panels."))
                     || a->objectName() == QLatin1String("tool.sizedown") || a->objectName() == QLatin1String("tool.sizeup")
                     || a->objectName() == QLatin1String("tool.softdown") || a->objectName() == QLatin1String("tool.softup"))
                add(a->objectName(), text, QStringLiteral("misc"));
            else {
                QString path = text;
                if (qobject_cast<QWidget *>(a->parent())) {
                    // walk menu ancestry via associated widgets is hard; use objectName prefix as menu
                    const QString id = a->objectName();
                    const QString top = id.section(QLatin1Char('.'), 0, 0);
                    static const QHash<QString, QString> tops{{"file","File"},{"edit","Edit"},{"select","Select"},
                        {"view","View"},{"modify","Modify"},{"text","Text"},{"commands","Commands"},
                        {"filters","Filters"},{"window","Window"},{"help","Help"},{"arrange","Modify/Arrange"},
                        {"path","Modify/Path"},{"mask","Modify/Mask"},{"symbol","Modify/Symbol"},{"anim","Commands"}};
                    if (tops.contains(top)) path = tops.value(top) + QLatin1Char('/') + text;
                }
                add(a->objectName(), text, QStringLiteral("menu"), path);
            }
        }
        sm.setCatalog(cat);
        QSettings st; sm.load(st); sm.apply(this);
    }

    m_statusPos = new QLabel;
    m_statusZoom = new ZoomField;
    m_statusSize = new QLabel;
    for (QLabel *l : {m_statusSize, m_statusPos}) { l->setMinimumWidth(90); statusBar()->addPermanentWidget(l); }
    statusBar()->addPermanentWidget(m_statusZoom);
    m_statusZoom->setEnabled(false);
    // the zoom field acts on the active view: the canvas (Original) or the shared preview view
    // (so 2-Up / 4-Up panes stay in sync)
    connect(m_statusZoom, &ZoomField::zoomRequested, this, [this](double z) {
        if (auto *v = currentView()) { v->setZoom(z); v->setFocus(); }
    });
    connect(m_statusZoom, &ZoomField::fitAllRequested, this, [this] { if (auto *v = currentView()) v->fit(); });
    connect(m_statusZoom, &ZoomField::fitSelectionRequested, this, [this] { if (auto *v = currentView()) v->fitSelection(); });
    connect(&appState(), &AppState::statusMessage, this, [this](const QString &m) { statusBar()->showMessage(m, 5000); });
    connect(&appState(), &AppState::toolChanged, this, [this](Tool t) {
        if (auto *a = action(toolActionId(t))) a->setChecked(true);
        statusBar()->showMessage(toolIsStub(t) ? QStringLiteral("%1: not available yet").arg(toolName(t)) : toolName(t), 3000);
        updateActions();
    });
    auto pushEnv = [this] {
        if (m_envDoc) syncToolEnvironmentTo(m_envDoc);
    };
    connect(&appState(), &AppState::optionsChanged, this, pushEnv);
    connect(&appState(), &AppState::colorsChanged, this, pushEnv);
    connect(&appState(), &AppState::toolChanged, this, pushEnv);


    QSettings st;
    m_lastDir = st.value(QStringLiteral("lastDir")).toString();
    m_recent = st.value(QStringLiteral("recentFiles")).toStringList();
    rebuildRecent();
    connect(&appState(), &AppState::prefsChanged, this, [this] {
        rebuildRecent();
        appicons::refreshAll(this);
        syncWebFeatureUi();
        // Interpolation (and any other bake-in pref) must not leave stale object visuals or frame caches.
        render::clearObjectVisualCache();
        for (int i = 0; i < m_tabs->count(); ++i)
            if (auto *dv = qobject_cast<DocumentView *>(m_tabs->widget(i))) {
                dv->canvas()->updateCursor();
                dv->canvas()->invalidateFrameCache();
                dv->scheduleRefresh();
            }
    });
    resize(1400, 900);
    syncWebFeatureUi();
    currentChanged();
}

MainWindow::~MainWindow() {
    QSettings st;
    st.setValue(QStringLiteral("lastDir"), m_lastDir);
    st.setValue(QStringLiteral("recentFiles"), m_recent);
    if (m_panelStack) m_panelStack->saveLayout(st);
}

void MainWindow::addRecent(const QString &path) {
    const QString abs = QFileInfo(path).absoluteFilePath();
    m_recent.removeAll(abs);
    m_recent.prepend(abs);
    while (m_recent.size() > 30) m_recent.removeLast();
    rebuildRecent();
}

QStringList MainWindow::recentFiles() const { return m_recent.mid(0, appState().prefs.recentFiles); }

void MainWindow::rebuildRecent() {
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    const QStringList files = recentFiles();
    for (int i = 0; i < files.size(); ++i) {
        const QString f = files[i];
        QAction *a = m_recentMenu->addAction(QStringLiteral("&%1 %2").arg(i + 1).arg(QFileInfo(f).fileName()));
        a->setToolTip(f);
        a->setData(f);
        connect(a, &QAction::triggered, this, [this, f] { openFile(f); });
    }
    if (files.isEmpty()) m_recentMenu->addAction(QStringLiteral("(No recent files)"))->setEnabled(false);
    else {
        m_recentMenu->addSeparator();
        connect(m_recentMenu->addAction(QStringLiteral("&Clear Recent Files")), &QAction::triggered, this, [this] { m_recent.clear(); rebuildRecent(); });
    }
}

void MainWindow::refreshToolShortcutLabels() {
    auto &sm = shortcuts::Manager::get();
    for (int i = 0; i <= int(kLastTool); ++i) {
        Tool t = Tool(i);
        const QString id = toolActionId(t);
        if (id.isEmpty()) continue;
        if (QAction *a = action(id)) {
            const QString key = sm.displayShortcutText(id);
            a->setToolTip(key.isEmpty() ? toolName(t)
                                        : toolName(t) + QStringLiteral(" (") + key + QChar(')'));
        }
    }
    if (m_toolsPanel) m_toolsPanel->refreshShortcutLabels();
}


bool MainWindow::event(QEvent *e) {
    // Tool QActions use WidgetShortcut; we dispatch tool keys here so they work with canvas
    // focus and are not lost to ambiguous WindowShortcut collisions with menus/docks.
    if (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        QWidget *fw = QApplication::focusWidget();
        const bool typing = fw && (
            qobject_cast<QLineEdit *>(fw)
            || qobject_cast<QTextEdit *>(fw)
            || qobject_cast<QPlainTextEdit *>(fw)
            || qobject_cast<QAbstractSpinBox *>(fw)
            || qobject_cast<QComboBox *>(fw)
            || qobject_cast<QKeySequenceEdit *>(fw)
            || fw->property("lwInlineEdit").toBool());
        if (!typing) {
            auto &sm = shortcuts::Manager::get();
            QString matched;
            for (const shortcuts::ActionInfo &info : sm.catalog()) {
                if (!info.id.startsWith(QLatin1String("tool."))) continue;
                if (info.id == QLatin1String("tool.sizedown") || info.id == QLatin1String("tool.sizeup")
                    || info.id == QLatin1String("tool.softdown") || info.id == QLatin1String("tool.softup"))
                    continue;
                if (sm.matches(info.id, ke)) { matched = info.id; break; }
            }
            if (!matched.isEmpty()) {
                const QKeySequence primary = sm.firstShortcut(matched);
                const QStringList share = sm.toolsSharingSequence(primary);
                if (!share.isEmpty()) {
                    QString pick;
                    for (const QString &id : share) {
                        const Tool cand = toolFromActionId(id);
                        if (toolAvailable(cand)) { pick = id; break; }
                    }
                    if (pick.isEmpty())
                        matched.clear();
                    else
                        matched = pick;
                }
                if (!matched.isEmpty() && !toolAvailable(toolFromActionId(matched)))
                    matched.clear();
                if (!matched.isEmpty()) {
                    if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }
                    if (!ke->isAutoRepeat()) {
                        if (QAction *a = action(matched)) { a->trigger(); return true; }
                    }
                    return true;
                }
            }
        }
    }
    return QMainWindow::event(e);
}

void MainWindow::editPreferences() {
    PreferencesDialog dlg(this, qEnvironmentVariableIsSet("LW_PREFS_TAB") ? qEnvironmentVariableIntValue("LW_PREFS_TAB") : 0);
    if (dlg.exec() != QDialog::Accepted) return;
    appState().setPrefs(dlg.prefs());
    appState().savePrefs();
    QSettings st;
    shortcuts::Manager::get().save(st);
    shortcuts::Manager::get().apply(this);
    appicons::refreshAll(this);
}

QAction *MainWindow::act(const QString &id, const QString &text, const QKeySequence &key, std::function<void()> fn, const QString &icon) {
    auto *a = new QAction(text, this);
    if (!key.isEmpty()) a->setShortcut(key);
    if (!icon.isEmpty()) a->setIcon(icons::named(icon));
    a->setObjectName(id);
    connect(a, &QAction::triggered, this, [fn] { fn(); });
    m_actions.insert(id, a);
    addAction(a); // shortcuts work even when menus are hidden
    return a;
}

void MainWindow::buildDocks() {
    // Tools (left)
    auto *toolsDock = new QDockWidget(QStringLiteral("Tools"), this);
    toolsDock->setObjectName(QStringLiteral("ToolsDock"));
    toolsDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    m_toolsPanel = new ToolsPanel;
    auto *toolsScroll = new QScrollArea;
    toolsScroll->setWidget(m_toolsPanel);
    toolsScroll->setWidgetResizable(true);
    toolsScroll->setFrameShape(QFrame::NoFrame);
    toolsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    toolsScroll->setFixedWidth(76);
    toolsDock->setWidget(toolsScroll);
    auto *toolsTitle = new QLabel(QStringLiteral("Tools"));
    toolsTitle->setObjectName(QStringLiteral("DockTitle"));
    toolsTitle->setContentsMargins(6, 3, 2, 3);
    toolsDock->setTitleBarWidget(toolsTitle);
    addDockWidget(Qt::LeftDockWidgetArea, toolsDock);

    // Property Inspector (bottom)
    auto *piDock = new QDockWidget(QStringLiteral("Properties"), this);
    piDock->setObjectName(QStringLiteral("PropertiesDock"));
    m_pi = new PropertyInspector;
    piDock->setWidget(m_pi);
    addDockWidget(Qt::BottomDockWidgetArea, piDock);
    connect(m_pi, &PropertyInspector::canvasSizeRequested, this, &MainWindow::canvasSize);
    connect(m_pi, &PropertyInspector::imageSizeRequested, this, &MainWindow::imageSize);
    connect(m_pi, &PropertyInspector::fitCanvasRequested, this, &MainWindow::fitCanvas);
    connect(m_pi, &PropertyInspector::fitCanvasToWindowRequested, this, &MainWindow::fitCanvasToWindow);

    // Panels (right)
    auto *panelsDock = new QDockWidget(QStringLiteral("Panels"), this);
    panelsDock->setObjectName(QStringLiteral("PanelsDock"));
    m_panelStack = new PanelStack;
    m_optimize = new OptimizePanel;
    m_layers = new LayersPanel;
    m_frames = new FramesPanel;
    m_history = new HistoryPanel;
    m_align = new AlignPanel;
    m_styles = new StylesPanel;
    m_library = new LibraryPanel;
    auto *autoShapes = new AutoShapesPanel;
    connect(m_library, &LibraryPanel::editSymbol, this, [this](quint64 id) { if (Document *d = currentDoc()) editSymbol(d, id); });
    struct P { QString t; QWidget *w; int stretch; bool collapsed; };
    const QList<P> ps = {
        {QStringLiteral("Optimize"), m_optimize, 0, false},
        {QStringLiteral("Layers"), m_layers, 3, false},
        {QStringLiteral("Frames"), m_frames, 1, false},
        {QStringLiteral("History"), m_history, 1, true},
        {QStringLiteral("Align"), m_align, 0, true},
        {QStringLiteral("Styles"), m_styles, 1, true},
        {QStringLiteral("Library"), m_library, 0, true},
        {QStringLiteral("Auto Shapes"), autoShapes, 0, true},
    };
    for (const P &p : ps) {
        auto *cp = new CollapsiblePanel(p.t, p.w, m_panelStack);
        cp->setObjectName(QStringLiteral("panel_") + p.t);
        m_panelStack->addPanel(cp, p.stretch);
        cp->setCollapsed(p.collapsed);
        if (p.t == QLatin1String("Optimize") || p.t == QLatin1String("Align")
            || p.t == QLatin1String("Auto Shapes"))
            cp->setContentSized(true);
        m_panels << cp;
    }
    {
        QSettings st;
        m_panelStack->restoreLayout(st);
    }
    auto *scroll = new PanelScrollArea;
    scroll->setObjectName(QStringLiteral("PanelsScroll"));
    scroll->setWidget(m_panelStack);
    scroll->setMinimumWidth(270);
    panelsDock->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, panelsDock);
    connect(m_optimize, &OptimizePanel::optionsChanged, this, [this] {
        if (auto *dv = currentView()) dv->scheduleRefresh();
    });
    connect(m_optimize, &OptimizePanel::exportRequested, this, [this] { fileExport(); });

    // Find & Replace (v0.3.1): its own dock, hidden until Window > Find & Replace (Ctrl+F)
    auto *findDock = new QDockWidget(QStringLiteral("Find & Replace"), this);
    findDock->setObjectName(QStringLiteral("FindDock"));
    m_find = new FindPanel;
    m_find->setDocumentProvider([this] { return documents(); }, [this] { return currentDoc(); },
                                [this](Document *d) {
                                    for (int i = 0; i < m_tabs->count(); ++i)
                                        if (auto *dv = qobject_cast<DocumentView *>(m_tabs->widget(i)); dv && dv->doc() == d) m_tabs->setCurrentIndex(i);
                                });
    auto *findScroll = new QScrollArea;
    findScroll->setWidget(m_find);
    findScroll->setWidgetResizable(true);
    findScroll->setFrameShape(QFrame::NoFrame);
    findDock->setWidget(findScroll);
    addDockWidget(Qt::RightDockWidgetArea, findDock);
    findDock->hide();
}

QVector<Document *> MainWindow::documents() const {
    QVector<Document *> out;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *v = qobject_cast<DocumentView *>(m_tabs->widget(i))) out << v->doc();
    return out;
}

CollapsiblePanel *MainWindow::panel(const QString &title) const {
    for (auto *p : m_panels) if (p->title() == title) return p;
    return nullptr;
}

void MainWindow::buildMenus() {
    auto *mb = menuBar();
    auto withDoc = [this](std::function<void(Document *)> f) { return [this, f] { if (Document *d = currentDoc()) f(d); }; };

    // ---- File
    QMenu *m = mb->addMenu(QStringLiteral("&File"));
    m->addAction(act(QStringLiteral("file.new"), QStringLiteral("&New..."), QKeySequence::New, [this] { fileNew(); }));
    m_actions[QStringLiteral("file.new")]->setIcon(stdIcon(QStyle::SP_FileIcon));
    m->addAction(act(QStringLiteral("file.open"), QStringLiteral("&Open..."), QKeySequence::Open, [this] { fileOpen(); }));
    m_actions[QStringLiteral("file.open")]->setIcon(stdIcon(QStyle::SP_DialogOpenButton));
    m_recentMenu = m->addMenu(QStringLiteral("Open &Recent"));
    m_recentMenu->setObjectName(QStringLiteral("recentMenu"));
    m->addAction(act(QStringLiteral("file.close"), QStringLiteral("&Close"), QKeySequence(QStringLiteral("Ctrl+W")), [this] { if (m_tabs->count()) closeTab(m_tabs->currentIndex()); }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("file.save"), QStringLiteral("&Save"), QKeySequence::Save, withDoc([this](Document *d) { saveDocument(d, false); })));
    m_actions[QStringLiteral("file.save")]->setIcon(stdIcon(QStyle::SP_DialogSaveButton));
    m->addAction(act(QStringLiteral("file.saveas"), QStringLiteral("Save &As..."), QKeySequence(QStringLiteral("Ctrl+Shift+S")), withDoc([this](Document *d) { saveDocument(d, true); })));
    m->addAction(act(QStringLiteral("file.revert"), QStringLiteral("Re&vert"), {}, withDoc([this](Document *d) {
        if (d->filePath.isEmpty()) return;
        if (QMessageBox::question(this, QStringLiteral("Revert"), QStringLiteral("Discard all changes to %1?").arg(d->title())) != QMessageBox::Yes) return;
        while (d->canUndo()) d->undo();
        d->markClean();
    })));
    m->addSeparator();
    m->addAction(act(QStringLiteral("file.import"), QStringLiteral("&Import..."), QKeySequence(QStringLiteral("Ctrl+R")), [this] { fileImport(); }));
    m->addAction(act(QStringLiteral("file.export"), QStringLiteral("&Export..."), QKeySequence(QStringLiteral("Ctrl+Shift+R")), [this] { fileExport(); }));
    m->addAction(act(QStringLiteral("file.exportpreview"), QStringLiteral("Export Pre&view..."), QKeySequence(QStringLiteral("Ctrl+Shift+X")), [this] { fileExportPreview(); }));
    m->addAction(act(QStringLiteral("file.exportselarea"), QStringLiteral("Export Selection &Area..."), {}, [this] { fileExportSelectionArea(); }));
    m->addAction(act(QStringLiteral("file.exportselected"), QStringLiteral("Export Selecte&d..."), {}, [this] { fileExportSelected(); }));
    connect(m, &QMenu::aboutToShow, this, [this] {
        Document *d = currentDoc();
        if (auto *a = action(QStringLiteral("file.exportselarea"))) a->setEnabled(d && !d->pixelSelection.isEmpty());
    });
    // Export Area tool: Enter / double-click inside the area opens Export Preview for it
    connect(&appState(), &AppState::exportAreaRequested, this, [this](const QRect &r) { fileExportPreview(r); });
    m->addAction(act(QStringLiteral("file.exporthtml"), QStringLiteral("Export &HTML and Images..."), QKeySequence() /* Ctrl+Alt+Shift+R is Text > Align Right, as in FW */, [this] { fileExportHtml(); }));
    m->addAction(act(QStringLiteral("file.exportfavicons"), QStringLiteral("Export Fa&vicon / App Icons..."), {}, [this] { fileExportFavicons(); }));
    m->addAction(act(QStringLiteral("file.exportslices"), QStringLiteral("Export &Slices..."), {}, [this] { exportSlices(); }));
    m->addAction(act(QStringLiteral("file.exportimagemap"), QStringLiteral("Export Image &Map..."), {}, [this] { exportImageMap(); }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("file.batch"), QStringLiteral("&Batch Process..."), {}, [this] { batchProcess(); }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("file.quit"), QStringLiteral("E&xit"), QKeySequence(QStringLiteral("Ctrl+Q")), [this] { close(); }));

    // ---- Edit
    m = mb->addMenu(QStringLiteral("&Edit"));
    m->addAction(act(QStringLiteral("edit.undo"), QStringLiteral("&Undo"), QKeySequence::Undo, withDoc([](Document *d) { d->undo(); })));
    m->addAction(act(QStringLiteral("edit.redo"), QStringLiteral("&Redo"), QKeySequence(QStringLiteral("Ctrl+Y")), withDoc([](Document *d) { d->redo(); })));
    m_actions[QStringLiteral("edit.redo")]->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Y")), QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
    m_actions[QStringLiteral("edit.undo")]->setIcon(stdIcon(QStyle::SP_ArrowBack));
    m_actions[QStringLiteral("edit.redo")]->setIcon(stdIcon(QStyle::SP_ArrowForward));
    m->addSeparator();
    m->addAction(act(QStringLiteral("edit.cut"), QStringLiteral("Cu&t"), QKeySequence::Cut, [this] { editCopy(true); }));
    m->addAction(act(QStringLiteral("edit.copy"), QStringLiteral("&Copy"), QKeySequence::Copy, [this] { editCopy(false); }));
    m->addAction(act(QStringLiteral("edit.copymerged"), QStringLiteral("Copy &Merged"), QKeySequence(QStringLiteral("Ctrl+Shift+C")), withDoc([this](Document *d) {
        VObject piece;
        if (!copyPixels(d, true, &piece)) { statusBar()->showMessage(QStringLiteral("Copy Merged needs a pixel selection over visible pixels"), 4000); return; }
        g_clipboard = {piece};
        g_clipboardSize = piece.image.size();
        g_clipboardDpi = d->s.dpi;
        auto *mime = new QMimeData;
        mime->setImageData(piece.image);
        mime->setData(QString::fromLatin1(kObjMime), QByteArray::number(1));
        QApplication::clipboard()->setMimeData(mime);
    })));
    m->addAction(act(QStringLiteral("edit.paste"), QStringLiteral("&Paste"), QKeySequence::Paste, [this] { editPaste(); }));
    m->addAction(act(QStringLiteral("edit.pasteinplace"), QStringLiteral("Paste in Pl&ace"), QKeySequence(QStringLiteral("Ctrl+Alt+V")), [this] { editPaste(true); }));
    m->addAction(act(QStringLiteral("edit.pasteinside"), QStringLiteral("Paste &Inside"), {}, [this] { groupCommand(GroupCmd::PasteInside); }));
    m->addAction(act(QStringLiteral("edit.pasteattributes"), QStringLiteral("Paste A&ttributes"), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+V")), [this] { pasteAttributes(); }));
    m->addAction(act(QStringLiteral("edit.clear"), QStringLiteral("C&lear"), QKeySequence(Qt::Key_Delete), withDoc([this](Document *d) {
        QVector<VObject *> bitmaps;
        PixelEditGuard pxGuard(d);
        if (!d->pixelSelection.isEmpty())
            for (VObject *o : d->selectedEditable())
                if (o->type == ObjType::Bitmap && !o->rect.isEmpty()) {
                    if (d->beginPixelEdit(*o)) bitmaps << o;
                    else statusBar()->showMessage(pxedit::blockedMessage(*o, QStringLiteral("Clear")), 6000);
                }
        if (!d->pixelSelection.isEmpty() && !bitmaps.isEmpty()) {
            clearPixels(d, bitmaps);
            d->commit(QStringLiteral("Clear Pixels"));
            return;
        }
        if (d->selection.isEmpty()) return;
        d->deleteSelection();
        d->commit(QStringLiteral("Clear"));
    })));
    m_actions[QStringLiteral("edit.clear")]->setShortcuts({QKeySequence(Qt::Key_Delete)}); // Backspace is canvas.removeLastPoint (poly); both editable
    m->addSeparator();
    m->addAction(act(QStringLiteral("edit.duplicate"), QStringLiteral("&Duplicate"), QKeySequence(QStringLiteral("Ctrl+Alt+D")), [this] { editDuplicate(false); }));
    m->addAction(act(QStringLiteral("edit.clone"), QStringLiteral("Clo&ne"), QKeySequence(QStringLiteral("Ctrl+Shift+D")), [this] { editDuplicate(true); }));
    m->addSeparator();
    {
        QMenu *ins = m->addMenu(QStringLiteral("&Insert"));
        ins->addAction(act(QStringLiteral("edit.insertslice"), QStringLiteral("&Slice"), QKeySequence(QStringLiteral("Alt+Shift+U")), [this] { insertWebObjects(ObjType::Slice); }));
        ins->addAction(act(QStringLiteral("edit.inserthotspot"), QStringLiteral("&Hotspot"), QKeySequence(QStringLiteral("Ctrl+Shift+U")), [this] { insertWebObjects(ObjType::Hotspot); }));
        ins->addSeparator();
        ins->addAction(act(QStringLiteral("edit.insertframe"), QStringLiteral("Empty &Frame"), {}, withDoc([](Document *d) { d->addFrame(false); d->commit(QStringLiteral("Add Frame")); })));
    }
    m->addSeparator();
    m->addAction(act(QStringLiteral("edit.edittext"), QStringLiteral("Edit Te&xt..."), {}, [this] {
        if (auto *dv = currentView()) if (VObject *o = dv->doc()->singleSelected()) if (o->type == ObjType::Text) dv->canvas()->editText(o);
    }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("edit.preferences"), QStringLiteral("Pr&eferences..."), QKeySequence(QStringLiteral("Ctrl+U")), [this] { editPreferences(); }));

    // ---- View
    m = mb->addMenu(QStringLiteral("&View"));
    m->addAction(act(QStringLiteral("view.zoomin"), QStringLiteral("Zoom &In"), QKeySequence(QStringLiteral("Ctrl+=")), [this] { if (auto *v = currentView()) v->zoomIn(); }));
    m_actions[QStringLiteral("view.zoomin")]->setShortcuts({QKeySequence(QStringLiteral("Ctrl+=")), QKeySequence(QStringLiteral("Ctrl++"))});
    m->addAction(act(QStringLiteral("view.zoomout"), QStringLiteral("Zoom &Out"), QKeySequence(QStringLiteral("Ctrl+-")), [this] { if (auto *v = currentView()) v->zoomOut(); }));
    QMenu *mag = m->addMenu(QStringLiteral("&Magnification"));
    for (int pct : {25, 50, 100, 200, 400, 800, 1600, 3200}) {
        mag->addAction(act(QStringLiteral("view.zoom%1").arg(pct), QStringLiteral("%1%").arg(pct), {}, [this, pct] { if (auto *v = currentView()) v->setZoom(pct / 100.0); }));
    }
    m->addAction(act(QStringLiteral("view.fit"), QStringLiteral("&Fit All"), QKeySequence(QStringLiteral("Ctrl+0")), [this] { if (auto *v = currentView()) v->fit(); }));
    m->addAction(act(QStringLiteral("view.fitsel"), QStringLiteral("Fit &Selection"), QKeySequence(QStringLiteral("Ctrl+Alt+0")), [this] { if (auto *v = currentView()) v->fitSelection(); }));
    m->addAction(act(QStringLiteral("view.actual"), QStringLiteral("&Actual Size"), QKeySequence(QStringLiteral("Ctrl+1")), [this] { if (auto *v = currentView()) v->actualSize(); }));
    m->addSeparator();
    m_viewModes = new QActionGroup(this);
    const char *modes[] = {"&Original", "&Preview", "&2-Up", "&4-Up"};
    for (int i = 0; i < 4; ++i) {
        QAction *a = act(QStringLiteral("view.mode%1").arg(i), QString::fromLatin1(modes[i]), QKeySequence(QStringLiteral("Ctrl+Alt+%1").arg(i + 7)), [this, i] { if (auto *v = currentView()) v->setMode(i); });
        a->setCheckable(true);
        a->setChecked(i == 0);
        m_viewModes->addAction(a);
        m->addAction(a);
    }
    m->addAction(act(QStringLiteral("view.resetpanes"), QStringLiteral("Reset 4-Up Pane Settings"), {}, [this] {
        if (auto *v = currentView()) v->resetPaneOptions();
    }));
    m->addSeparator();
    // Fireworks View > Hide Selection / Show All (Ctrl+L is Filters > Levels here, so Hide has no key)
    m->addAction(act(QStringLiteral("view.hide"), QStringLiteral("&Hide Selection"), {}, withDoc([](Document *d) {
        bool any = false;
        for (VObject *o : d->selectedEditable()) { o->visible = false; any = true; }
        if (!any) return;
        d->clearSelection();
        d->commit(QStringLiteral("Hide Selection"));
    })));
    m->addAction(act(QStringLiteral("view.showall"), QStringLiteral("Sho&w All"), QKeySequence(QStringLiteral("Ctrl+Shift+L")), withDoc([](Document *d) {
        bool any = false;
        for (int li = 0; li < d->s.layers.size(); ++li)
            for (VObject &o : d->objects(li)) if (!o.visible) { o.visible = true; any = true; }
        if (any) d->commit(QStringLiteral("Show All"));
    })));
    m->addSeparator();
    // Grid, guides and rulers. Grid and guides belong to the document (saved in .lwd).
    auto docToggle = [this](const QString &id, auto setter) {
        return [this, id, setter] {
            Document *d = currentDoc();
            if (!d) return;
            setter(d, action(id)->isChecked());
        };
    };
    QAction *rulers = act(QStringLiteral("view.rulers"), QStringLiteral("&Rulers"), QKeySequence(QStringLiteral("Ctrl+Alt+R")), [this] {
        appState().showRulers = action(QStringLiteral("view.rulers"))->isChecked();
        appState().notifyOptions();
    });
    rulers->setCheckable(true);
    rulers->setChecked(appState().showRulers);
    m->addAction(rulers);
    QMenu *gm = m->addMenu(QStringLiteral("&Grid"));
    gm->setObjectName(QStringLiteral("menuGrid"));
    QAction *grid = act(QStringLiteral("view.grid"), QStringLiteral("Show &Grid"), QKeySequence(QStringLiteral("Ctrl+Alt+G")),
                        docToggle(QStringLiteral("view.grid"), [](Document *d, bool on) { GridSettings g = d->grid; g.show = on; d->setGrid(g); }));
    grid->setCheckable(true);
    gm->addAction(grid);
    QAction *gsnap = act(QStringLiteral("view.gridsnap"), QStringLiteral("&Snap to Grid"), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+G")),
                         docToggle(QStringLiteral("view.gridsnap"), [](Document *d, bool on) { GridSettings g = d->grid; g.snap = on; d->setGrid(g); }));
    gsnap->setCheckable(true);
    gm->addAction(gsnap);
    gm->addAction(act(QStringLiteral("view.gridedit"), QStringLiteral("&Edit Grid..."), {}, [this] {
        if (Document *d = currentDoc()) { EditGridDialog dlg(d, this); dlg.exec(); }
    }));
    QMenu *gdm = m->addMenu(QStringLiteral("G&uides"));
    gdm->setObjectName(QStringLiteral("menuGuides"));
    QAction *gshow = act(QStringLiteral("view.guides"), QStringLiteral("&Show Guides"), QKeySequence(QStringLiteral("Ctrl+;")),
                         docToggle(QStringLiteral("view.guides"), [](Document *d, bool on) { GuideSettings g = d->guides; g.show = on; d->setGuides(g); }));
    QAction *glock = act(QStringLiteral("view.guideslock"), QStringLiteral("&Lock Guides"), QKeySequence(QStringLiteral("Ctrl+Alt+;")),
                         docToggle(QStringLiteral("view.guideslock"), [](Document *d, bool on) { GuideSettings g = d->guides; g.lock = on; d->setGuides(g); }));
    QAction *gsn = act(QStringLiteral("view.guidessnap"), QStringLiteral("S&nap to Guides"), QKeySequence(QStringLiteral("Ctrl+Shift+;")),
                       docToggle(QStringLiteral("view.guidessnap"), [](Document *d, bool on) { GuideSettings g = d->guides; g.snap = on; d->setGuides(g); }));
    for (QAction *a : {gshow, glock, gsn}) { a->setCheckable(true); gdm->addAction(a); }
    gdm->addAction(act(QStringLiteral("view.guidesedit"), QStringLiteral("&Edit Guides..."), {}, [this] {
        if (Document *d = currentDoc()) { EditGuidesDialog dlg(d, this); dlg.exec(); }
    }));
    // smart snapping (application preferences, saved with Preferences)
    {
        auto prefToggle = [this](const char *id, const QString &text, bool Prefs::*field) {
            QAction *a = act(QString::fromLatin1(id), text, {}, [this, id, field] {
                Prefs p = appState().prefs;
                p.*field = action(QString::fromLatin1(id))->isChecked();
                appState().setPrefs(p);
                appState().savePrefs();
            });
            a->setCheckable(true);
            a->setChecked(appState().prefs.*field);
            connect(&appState(), &AppState::prefsChanged, a, [a, field] { QSignalBlocker b(a); a->setChecked(appState().prefs.*field); });
            return a;
        };
        m->addAction(prefToggle("view.snapcanvas", QStringLiteral("Snap to &Canvas Edges"), &Prefs::snapToCanvas));
        m->addAction(prefToggle("view.snapobjects", QStringLiteral("Snap to &Objects"), &Prefs::snapToObjects));
    }
    QAction *web = act(QStringLiteral("view.web"), QStringLiteral("Show Slices and &Hotspots"), QKeySequence(QStringLiteral("2")), [this] {
        appState().webLayerVisible = action(QStringLiteral("view.web"))->isChecked();
        appState().notifyOptions();
    });
    web->setCheckable(true);
    web->setChecked(true);
    m->addAction(web);

    // ---- Select
    m = mb->addMenu(QStringLiteral("&Select"));
    m->addAction(act(QStringLiteral("select.all"), QStringLiteral("Select &All"), QKeySequence::SelectAll, withDoc([](Document *d) {
        // with a bitmap selection tool: the whole canvas as a pixel selection (Fireworks), else every object
        const Tool t = appState().tool();
        if (t == Tool::Marquee || t == Tool::OvalMarquee || t == Tool::Lasso || t == Tool::PolyLasso || t == Tool::MagicWand) {
            QPainterPath all;
            all.addRect(0, 0, d->s.width, d->s.height);
            d->combinePixelSelection(all, Qt::NoModifier);
            d->notifyChanged();
            return;
        }
        d->selectAll(webFeaturesEnabled());
    })));
    m->addAction(act(QStringLiteral("select.none"), QStringLiteral("&Deselect"), QKeySequence(QStringLiteral("Ctrl+D")), withDoc([](Document *d) {
        d->clearSelection();
        d->clearPixelSelection();
        d->notifyChanged();
    })));
    m->addAction(act(QStringLiteral("select.inverse"), QStringLiteral("Select &Inverse"), QKeySequence(QStringLiteral("Ctrl+Shift+I")), [this] { selectInverse(); }));
    // v0.3.2: leave the group entered with a double-click or the Subselection tool (Esc on the canvas)
    m->addAction(act(QStringLiteral("select.exitgroup"), QStringLiteral("E&xit Group"), {}, withDoc([](Document *d) {
        if (!d->enteredGroup) return;
        const quint64 g = d->enteredGroup;
        d->exitGroup();
        d->selectOnly(g);
        d->notifyChanged();
    })));
    m->addSeparator();
    m->addAction(act(QStringLiteral("select.feather"), QStringLiteral("&Feather..."), {}, [this] { modifySelection(SelFeather); }));
    m->addAction(act(QStringLiteral("select.expand"), QStringLiteral("&Expand Marquee..."), {}, [this] { modifySelection(SelExpand); }));
    m->addAction(act(QStringLiteral("select.contract"), QStringLiteral("&Contract Marquee..."), {}, [this] { modifySelection(SelContract); }));
    m->addAction(act(QStringLiteral("select.border"), QStringLiteral("&Border Marquee..."), {}, [this] { modifySelection(SelBorder); }));
    m->addAction(act(QStringLiteral("select.smooth"), QStringLiteral("S&mooth Marquee..."), {}, [this] { modifySelection(SelSmooth); }));
    m->addAction(act(QStringLiteral("select.similar"), QStringLiteral("Select Si&milar"), {}, [this] { selectSimilar(); }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("select.save"), QStringLiteral("&Save Bitmap Selection..."), {}, [this] { saveSelection(); }));
    m->addAction(act(QStringLiteral("select.restore"), QStringLiteral("&Restore Bitmap Selection..."), {}, [this] { manageSelections(); }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("select.topath"), QStringLiteral("Convert Marquee to &Path"), {}, [this] { convertMarqueeToPath(); }));
    QAction *qm = act(QStringLiteral("select.quickmask"), QStringLiteral("&Quick Mask"), {}, [this] { toggleQuickMask(); });
    qm->setCheckable(true);
    qm->setToolTip(QStringLiteral("Quick Mask: paint the selection as a mask; white = selected"));
    m->addAction(qm);

    // ---- Modify
    m = mb->addMenu(QStringLiteral("&Modify"));
    QMenu *canvas = m->addMenu(QStringLiteral("&Canvas"));
    canvas->addAction(act(QStringLiteral("modify.imagesize"), QStringLiteral("&Image Size..."), {}, [this] { imageSize(); }));
    canvas->addAction(act(QStringLiteral("modify.canvassize"), QStringLiteral("&Canvas Size..."), {}, [this] { canvasSize(); }));
    canvas->addAction(act(QStringLiteral("modify.canvascolor"), QStringLiteral("Canvas C&olor..."), {}, withDoc([this](Document *d) {
        QColor c = colorpick::getColor(d->s.canvasColor, this, QStringLiteral("Canvas Color"), true);
        if (c.isValid()) { d->s.canvasColor = c; d->commit(QStringLiteral("Canvas Color")); }
    })));
    canvas->addSeparator();
    canvas->addAction(act(QStringLiteral("modify.trim"), QStringLiteral("&Trim Canvas"), QKeySequence(QStringLiteral("Ctrl+Alt+T")), [this] { trimCanvas(); }));
    canvas->addAction(act(QStringLiteral("modify.fit"), QStringLiteral("&Fit Canvas"), QKeySequence(QStringLiteral("Ctrl+Alt+F")), [this] { fitCanvas(); }));
    canvas->addAction(act(QStringLiteral("modify.fitToWindow"), QStringLiteral("Fit Canvas to &Window"), {}, [this] { fitCanvasToWindow(); }));
    canvas->addSeparator();
    canvas->addAction(act(QStringLiteral("modify.canvas180"), QStringLiteral("Rotate 180°"), {}, [this] { transform(10); }));
    canvas->addAction(act(QStringLiteral("modify.canvas90cw"), QStringLiteral("Rotate 90° CW"), {}, [this] { transform(11); }));
    canvas->addAction(act(QStringLiteral("modify.canvas90ccw"), QStringLiteral("Rotate 90° CCW"), {}, [this] { transform(12); }));
    canvas->addAction(act(QStringLiteral("modify.canvasrotate"), QStringLiteral("Rotate Canvas..."), {}, [this] {
        Document *d = currentDoc();
        if (!d) return;
        bool ok = false;
        const double deg = QInputDialog::getDouble(this, QStringLiteral("Rotate Canvas"), QStringLiteral("Angle (degrees, clockwise):"), 15, -360, 360, 2, &ok);
        if (ok) rotateCanvas(deg);
    }));
    QMenu *bmp = m->addMenu(QStringLiteral("&Bitmap"));
    bmp->addAction(act(QStringLiteral("modify.bitmapviacopy"), QStringLiteral("Copy to New &Bitmap (Bitmap via Copy)"),
                       QKeySequence(QStringLiteral("Ctrl+Alt+J")), [this] { bitmapVia(false); }));
    bmp->addAction(act(QStringLiteral("modify.bitmapviacut"), QStringLiteral("Cut to New Bitmap (Bitmap via C&ut)"),
                       QKeySequence(QStringLiteral("Ctrl+Alt+Shift+J")), [this] { bitmapVia(true); }));
    m->addAction(act(QStringLiteral("modify.tracebitmap"), QStringLiteral("T&race Bitmap..."), {}, [this] { traceBitmap(); }));
    QMenu *idx = m->addMenu(QStringLiteral("&Indexed Color"));
    idx->setObjectName(QStringLiteral("indexedColorMenu"));
    idx->addAction(act(QStringLiteral("modify.toindexed"), QStringLiteral("&Convert to Indexed..."), {}, [this] { convertToIndexed(); }));
    idx->addAction(act(QStringLiteral("modify.editpalette"), QStringLiteral("&Edit Palette..."), {}, [this] { editPalette(); }));
    idx->addAction(act(QStringLiteral("modify.paletteswap"), QStringLiteral("Palette &Swap..."), {}, [this] { effectFilter(int(EffectType::PaletteSwap)); }));
    QMenu *anim = m->addMenu(QStringLiteral("&Animation"));
    anim->addAction(act(QStringLiteral("anim.addframe"), QStringLiteral("&Add Frame"), {}, withDoc([](Document *d) { d->addFrame(false); d->commit(QStringLiteral("Add Frame")); })));
    anim->addAction(act(QStringLiteral("anim.dupframe"), QStringLiteral("&Duplicate Frame"), {}, withDoc([](Document *d) { d->addFrame(true); d->commit(QStringLiteral("Duplicate Frame")); })));
    anim->addAction(act(QStringLiteral("anim.delframe"), QStringLiteral("De&lete Frame"), {}, withDoc([](Document *d) {
        if (d->s.frames.size() > 1) { d->deleteFrame(d->curFrame); d->commit(QStringLiteral("Delete Frame")); }
    })));
    anim->addSeparator();
    anim->addAction(act(QStringLiteral("anim.play"), QStringLiteral("&Play / Stop"), QKeySequence(QStringLiteral("Ctrl+Alt+P")), [this] { m_frames->play(!m_frames->isPlaying()); }));
    anim->addAction(act(QStringLiteral("anim.next"), QStringLiteral("&Next Frame"), QKeySequence(QStringLiteral("Ctrl+PgDown")), withDoc([](Document *d) { d->setCurrentFrame((d->curFrame + 1) % d->s.frames.size()); })));
    anim->addAction(act(QStringLiteral("anim.prev"), QStringLiteral("P&revious Frame"), QKeySequence(QStringLiteral("Ctrl+PgUp")), withDoc([](Document *d) { d->setCurrentFrame((d->curFrame + d->s.frames.size() - 1) % d->s.frames.size()); })));
    anim->addAction(act(QStringLiteral("anim.copytoframes"), QStringLiteral("&Copy to Frames..."), {}, [this] {
        Document *d = currentDoc();
        if (!d || d->selection.isEmpty()) return;
        CopyToFramesDialog dlg(d->s.frames.size(), d->curFrame, this);
        if (dlg.exec() != QDialog::Accepted) return;
        const QVector<int> t = dlg.targets();
        if (t.isEmpty()) return;
        d->copyToFrames(t);
        d->commit(QStringLiteral("Copy to Frames"));
    }));
    anim->addAction(act(QStringLiteral("anim.distributetoframes"), QStringLiteral("D&istribute to Frames"), {}, withDoc([](Document *d) {
        if (d->selection.isEmpty()) return;
        d->distributeToFrames();
        d->commit(QStringLiteral("Distribute to Frames"));
    })));
    anim->addAction(act(QStringLiteral("anim.animate"), QStringLiteral("Animate &Selection..."), QKeySequence(QStringLiteral("Alt+Shift+F8")), [this] { animateSelectionDialog(); }));
    QMenu *symm = m->addMenu(QStringLiteral("S&ymbol"));
    symm->addAction(act(QStringLiteral("symbol.convert"), QStringLiteral("&Convert to Symbol..."), QKeySequence(QStringLiteral("F8")), [this] { convertToSymbol(); }));
    symm->addAction(act(QStringLiteral("symbol.edit"), QStringLiteral("&Edit Symbol"), {}, withDoc([this](Document *d) {
        VObject *o = d->singleSelected();
        if (o && o->isInstance()) editSymbol(d, o->symbolId);
    })));
    symm->addAction(act(QStringLiteral("symbol.tween"), QStringLiteral("&Tween Instances..."), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+T")), [this] { tweenInstancesDialog(); }));
    symm->addAction(act(QStringLiteral("symbol.breakapart"), QStringLiteral("&Break Apart"), {}, withDoc([](Document *d) {
        if (!d->breakApart().isEmpty()) d->commit(QStringLiteral("Break Apart"));
    })));
    QMenu *cp = m->addMenu(QStringLiteral("Combine &Path"));
    auto combineAct = [this, withDoc](const char *id, const QString &label, paths::Combine op, const QString &hist, const QKeySequence &ks = {}) {
        return act(QString::fromLatin1(id), label, ks, withDoc([this, op, hist](Document *d) {
            QString err;
            if (paths::combine(*d, op, &err)) d->commit(hist);
            else statusBar()->showMessage(err, 4000);
        }));
    };
    cp->addAction(combineAct("path.join", QStringLiteral("&Join"), paths::Combine::Join, QStringLiteral("Join"), QKeySequence(QStringLiteral("Ctrl+J"))));
    cp->addAction(act(QStringLiteral("path.split"), QStringLiteral("&Split"), QKeySequence(QStringLiteral("Ctrl+Shift+J")), withDoc([](Document *d) {
        if (paths::split(*d)) d->commit(QStringLiteral("Split"));
    })));
    cp->addSeparator();
    cp->addAction(combineAct("path.union", QStringLiteral("&Union"), paths::Combine::Union, QStringLiteral("Union")));
    cp->addAction(combineAct("path.intersect", QStringLiteral("&Intersect"), paths::Combine::Intersect, QStringLiteral("Intersect")));
    cp->addAction(combineAct("path.punch", QStringLiteral("&Punch"), paths::Combine::Punch, QStringLiteral("Punch")));
    cp->addAction(combineAct("path.crop", QStringLiteral("&Crop"), paths::Combine::Crop, QStringLiteral("Crop")));
    QMenu *ap = m->addMenu(QStringLiteral("Alter Pat&h"));
    ap->addAction(act(QStringLiteral("path.simplify"), QStringLiteral("&Simplify..."), {}, [this] { alterPathDialog(0); }));
    ap->addAction(act(QStringLiteral("path.expand"), QStringLiteral("&Expand Stroke..."), {}, [this] { alterPathDialog(1); }));
    ap->addAction(act(QStringLiteral("path.inset"), QStringLiteral("&Inset Path..."), {}, [this] { alterPathDialog(2); }));
    ap->addSeparator();
    ap->addAction(act(QStringLiteral("path.convert"), QStringLiteral("&Convert to Paths"), QKeySequence(QStringLiteral("Ctrl+Shift+P")), withDoc([](Document *d) {
        if (paths::convertToPaths(*d)) d->commit(QStringLiteral("Convert to Paths"));
    })));
    m->addSeparator();
    // ---- groups and masks (v0.3.2, Fireworks MX 2004)
    m->addAction(act(QStringLiteral("modify.group"), QStringLiteral("&Group"), QKeySequence(QStringLiteral("Ctrl+G")), [this] { groupCommand(GroupCmd::Group); }));
    m->addAction(act(QStringLiteral("modify.ungroup"), QStringLiteral("&Ungroup"), QKeySequence(QStringLiteral("Ctrl+Shift+G")), [this] { groupCommand(GroupCmd::Ungroup); }));
    QMenu *mk = m->addMenu(QStringLiteral("Mas&k"));
    mk->setObjectName(QStringLiteral("modifyMaskMenu"));   // the canvas context menu and Layers panel menu mirror it
    mk->addAction(act(QStringLiteral("mask.pasteasmask"), QStringLiteral("&Paste as Mask"), {}, [this] { groupCommand(GroupCmd::PasteAsMask); }));
    mk->addAction(act(QStringLiteral("mask.pasteinside"), QStringLiteral("Paste &Inside"), QKeySequence(QStringLiteral("Ctrl+Shift+V")), [this] { groupCommand(GroupCmd::PasteInside); }));
    mk->addAction(act(QStringLiteral("mask.groupasmask"), QStringLiteral("&Group as Mask"), {}, [this] { groupCommand(GroupCmd::GroupAsMask); }));
    mk->addSeparator();
    mk->addAction(act(QStringLiteral("mask.revealall"), QStringLiteral("&Reveal All"), {}, [this] { groupCommand(GroupCmd::RevealAll); }));
    mk->addAction(act(QStringLiteral("mask.hideall"), QStringLiteral("&Hide All"), {}, [this] { groupCommand(GroupCmd::HideAll); }));
    mk->addSeparator();
    mk->addAction(act(QStringLiteral("mask.disable"), QStringLiteral("&Disable Mask"), {}, [this] { groupCommand(GroupCmd::DisableMask); }));
    mk->addAction(act(QStringLiteral("mask.enable"), QStringLiteral("&Enable Mask"), {}, [this] { groupCommand(GroupCmd::EnableMask); }));
    {
        QAction *lk = act(QStringLiteral("mask.link"), QStringLiteral("&Link Mask"), {}, [this] { groupCommand(GroupCmd::ToggleLink); });
        lk->setCheckable(true);
        lk->setToolTip(QStringLiteral("Linked: the mask moves and transforms with the masked objects"));
        mk->addAction(lk);
    }
    mk->addSeparator();
    mk->addAction(act(QStringLiteral("mask.delete"), QStringLiteral("Dele&te Mask"), {}, [this] { groupCommand(GroupCmd::DeleteMask); }));
    m->addAction(act(QStringLiteral("modify.converttobitmap"), QStringLiteral("Con&vert to Bitmap"), {}, [this] { groupCommand(GroupCmd::ConvertToBitmap); }));
    m->addAction(act(QStringLiteral("modify.flattensel"), QStringLiteral("Flatten &Selection"), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+Z")), [this] { convertToBitmap(); }));
    m->addAction(act(QStringLiteral("modify.mergedown"), QStringLiteral("&Merge Down"), QKeySequence(QStringLiteral("Ctrl+E")), [this] { mergeDown(); }));
    m->addAction(act(QStringLiteral("modify.flattenlayers"), QStringLiteral("Flatten &Layers"), {}, [this] { flattenLayers(); }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("modify.lock"), QStringLiteral("Lock Selectio&n"), QKeySequence(QStringLiteral("Ctrl+Alt+L")), withDoc([](Document *d) {
        bool any = false;
        for (VObject *o : d->selectedEditable()) { o->locked = true; any = true; }
        if (!any) return;
        d->clearSelection();
        d->commit(QStringLiteral("Lock Selection"));
    })));
    m->addAction(act(QStringLiteral("modify.unlockall"), QStringLiteral("Unloc&k All"), {}, withDoc([](Document *d) {
        bool any = false;
        for (int li = 0; li < d->s.layers.size(); ++li)
            for (VObject &o : d->objects(li)) if (o.locked) { o.locked = false; any = true; }
        if (any) d->commit(QStringLiteral("Unlock All"));
    })));
    m->addSeparator();
    m->addAction(act(QStringLiteral("modify.savestyle"), QStringLiteral("Save as St&yle..."), {}, [this] { if (m_styles) m_styles->newStyleDialog(); }));
    m->addAction(act(QStringLiteral("modify.editgradient"), QStringLiteral("Edit &Gradient..."), {}, [this] { m_pi->editGradient(); }));
    m->addAction(act(QStringLiteral("modify.editstroke"), QStringLiteral("Edit St&roke..."), {}, [this] { m_pi->editStroke(); }));
    m->addSeparator();
    QMenu *tr = m->addMenu(QStringLiteral("&Transform"));
    tr->setObjectName(QStringLiteral("modifyTransformMenu"));   // the canvas context menu mirrors it
    auto pickTool = [this](Tool t) {
        return [this, t] {
            appState().setTool(t);
            if (auto *v = currentView()) v->canvas()->setFocus();
        };
    };
    tr->addAction(act(QStringLiteral("modify.freetransform"), QStringLiteral("&Free Transform"), QKeySequence(QStringLiteral("Ctrl+T")), pickTool(Tool::Scale)));
    tr->addAction(act(QStringLiteral("modify.scale"), QStringLiteral("&Scale"), {}, pickTool(Tool::Scale)));
    tr->addAction(act(QStringLiteral("modify.skew"), QStringLiteral("S&kew"), {}, pickTool(Tool::Skew)));
    tr->addAction(act(QStringLiteral("modify.distort"), QStringLiteral("&Distort"), {}, pickTool(Tool::Distort)));
    tr->addAction(act(QStringLiteral("modify.perspective"), QStringLiteral("&Perspective"), {}, pickTool(Tool::Perspective)));
    tr->addAction(act(QStringLiteral("modify.numeric"), QStringLiteral("&Numeric Transform..."), QKeySequence(QStringLiteral("Ctrl+Shift+T")), [this] { numericTransform(); }));
    tr->addAction(act(QStringLiteral("modify.removexform"), QStringLiteral("&Remove Transformations"), {}, [this] { removeTransformations(); }));
    tr->addSeparator();
    tr->addAction(act(QStringLiteral("modify.rot180"), QStringLiteral("Rotate 180°"), {}, [this] { transform(0); }));
    tr->addAction(act(QStringLiteral("modify.rot90cw"), QStringLiteral("Rotate 90° CW"), QKeySequence(QStringLiteral("Ctrl+Shift+9")), [this] { transform(1); }));
    tr->addAction(act(QStringLiteral("modify.rot90ccw"), QStringLiteral("Rotate 90° CCW"), QKeySequence(QStringLiteral("Ctrl+Shift+7")), [this] { transform(2); }));
    tr->addSeparator();
    tr->addAction(act(QStringLiteral("modify.fliph"), QStringLiteral("Flip &Horizontal"), {}, [this] { transform(3); }));
    tr->addAction(act(QStringLiteral("modify.flipv"), QStringLiteral("Flip &Vertical"), {}, [this] { transform(4); }));
    QMenu *ar = m->addMenu(QStringLiteral("A&rrange"));
    ar->addAction(act(QStringLiteral("arrange.front"), QStringLiteral("Bring to &Front"), QKeySequence(QStringLiteral("Ctrl+Shift+Up")), withDoc([](Document *d) { d->arrange(0); d->commit(QStringLiteral("Bring to Front")); })));
    ar->addAction(act(QStringLiteral("arrange.forward"), QStringLiteral("Bring F&orward"), QKeySequence(QStringLiteral("Ctrl+Up")), withDoc([](Document *d) { d->arrange(1); d->commit(QStringLiteral("Bring Forward")); })));
    ar->addAction(act(QStringLiteral("arrange.backward"), QStringLiteral("Send Back&ward"), QKeySequence(QStringLiteral("Ctrl+Down")), withDoc([](Document *d) { d->arrange(2); d->commit(QStringLiteral("Send Backward")); })));
    ar->addAction(act(QStringLiteral("arrange.back"), QStringLiteral("Send to &Back"), QKeySequence(QStringLiteral("Ctrl+Shift+Down")), withDoc([](Document *d) { d->arrange(3); d->commit(QStringLiteral("Send to Back")); })));
    QMenu *al = m->addMenu(QStringLiteral("A&lign"));
    const char *alNames[] = {"&Left", "Center &Vertical", "&Right", "&Top", "Center &Horizontal", "&Bottom", "Distribute &Widths", "Distribute H&eights", "Same Width", "Same Height"};
    for (int i = 0; i < 10; ++i)
        al->addAction(act(QStringLiteral("align.%1").arg(i), QString::fromLatin1(alNames[i]), i < 6 ? QKeySequence(QStringLiteral("Ctrl+Alt+%1").arg(i + 1)) : QKeySequence(),
                          withDoc([i](Document *d) { align::apply(d, i, false); })));

    // ---- Text
    m = mb->addMenu(QStringLiteral("&Text"));
    m->addAction(act(QStringLiteral("text.bold"), QStringLiteral("&Bold"), QKeySequence(QStringLiteral("Ctrl+B")), [this] { textStyle(0); }));
    m->addAction(act(QStringLiteral("text.italic"), QStringLiteral("&Italic"), QKeySequence(QStringLiteral("Ctrl+I")), [this] { textStyle(1); }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("text.left"), QStringLiteral("Align &Left"), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+L")), [this] { textStyle(2); }));
    m->addAction(act(QStringLiteral("text.center"), QStringLiteral("Align &Center"), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+C")), [this] { textStyle(3); }));
    m->addAction(act(QStringLiteral("text.right"), QStringLiteral("Align &Right"), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+R")), [this] { textStyle(4); }));
    m->addAction(act(QStringLiteral("text.justify"), QStringLiteral("Align &Justify"), {}, [this] { textStyle(7); }));
    m->addSeparator();
    QMenu *orient = m->addMenu(QStringLiteral("&Orientation"));
    orient->setObjectName(QStringLiteral("textOrientationMenu"));
    orient->addAction(act(QStringLiteral("text.horizontal"), QStringLiteral("&Horizontal"), {}, [this] { textStyle(8); }));
    orient->addAction(act(QStringLiteral("text.vertical"), QStringLiteral("&Vertical"), {}, [this] { textStyle(9); }));
    QMenu *dir = m->addMenu(QStringLiteral("&Direction"));
    dir->setObjectName(QStringLiteral("textDirectionMenu"));
    dir->addAction(act(QStringLiteral("text.ltr"), QStringLiteral("&Left to Right"), {}, [this] { textStyle(10); }));
    dir->addAction(act(QStringLiteral("text.rtl"), QStringLiteral("&Right to Left"), {}, [this] { textStyle(11); }));
    m->addAction(act(QStringLiteral("text.reverse"), QStringLiteral("R&everse Direction"), {}, [this] { textStyle(12); }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("text.larger"), QStringLiteral("Size &Larger"), QKeySequence(QStringLiteral("Ctrl+Shift+.")), [this] { textStyle(5); }));
    m->addAction(act(QStringLiteral("text.smaller"), QStringLiteral("Size &Smaller"), QKeySequence(QStringLiteral("Ctrl+Shift+,")), [this] { textStyle(6); }));
    m->addSeparator();
    m->addAction(m_actions[QStringLiteral("edit.edittext")]);
    m->addSeparator();
    m->addAction(act(QStringLiteral("text.attach"), QStringLiteral("&Attach to Path"), QKeySequence(QStringLiteral("Ctrl+Shift+Y")), withDoc([this](Document *d) {
        QString err;
        if (paths::attachTextToPath(*d, &err)) d->commit(QStringLiteral("Attach to Path"));
        else statusBar()->showMessage(err, 4000);
    })));
    m->addAction(act(QStringLiteral("text.detach"), QStringLiteral("&Detach from Path"), {}, withDoc([](Document *d) {
        if (paths::detachTextFromPath(*d)) d->commit(QStringLiteral("Detach from Path"));
    })));
    m->addAction(m_actions[QStringLiteral("path.convert")]);

    // ---- Commands
    m = mb->addMenu(QStringLiteral("&Commands"));
    m->addAction(act(QStringLiteral("cmd.sample"), QStringLiteral("Create &Sample Document"), {}, [this] { addDocument(Document::createSample()); }));
    m->addAction(act(QStringLiteral("cmd.exportframes"), QStringLiteral("Export &Frames as Files..."), {}, [this] { exportFrames(); }));
    m->addAction(act(QStringLiteral("cmd.shadow"), QStringLiteral("Add &Drop Shadow to Selection"), {}, withDoc([](Document *d) {
        bool any = false;
        for (VObject *o : d->selectedEditable()) { o->effects.append(LiveEffect::make(EffectType::DropShadow)); any = true; }
        if (any) d->commit(QStringLiteral("Add Drop Shadow"));
    })));
    {
        // Live Effects (editable, non-destructive) by category -- same list as the Property Inspector's "+" menu
        QMenu *live = m->addMenu(QStringLiteral("Add &Live Effect"));
        live->setObjectName(QStringLiteral("liveEffectsMenu"));
        QMap<QString, QMenu *> subs;
        for (EffectType t : allEffectTypes()) subs[effectCategory(t)] = nullptr;
        for (auto it = subs.begin(); it != subs.end(); ++it) it.value() = live->addMenu(it.key());
        for (EffectType t : allEffectTypes())
            subs[effectCategory(t)]->addAction(act(QStringLiteral("fx.") + effectKey(t), effectName(t), {}, withDoc([this, t](Document *d) {
                // same path as the Property Inspector's "+": a settings window first, added only on OK
                if (m_pi && m_pi->document() == d) { m_pi->addEffect(t); return; }
                bool any = false;
                for (VObject *o : d->selectedEditable()) { o->effects.append(LiveEffect::make(t)); any = true; }
                if (any) d->commit(QStringLiteral("Add ") + effectName(t));
            })));
    }
    m->addAction(act(QStringLiteral("cmd.seamless"), QStringLiteral("Make Seamless &Tile..."), {}, [this] { makeSeamlessTile(); }));
    m->addAction(act(QStringLiteral("cmd.removefx"), QStringLiteral("&Remove Live Effects from Selection"), {}, withDoc([](Document *d) {
        bool any = false;
        for (VObject *o : d->selectedEditable()) if (!o->effects.isEmpty()) { o->effects.clear(); any = true; }
        if (any) d->commit(QStringLiteral("Remove Effects"));
    })));

    // ---- Filters
    m = mb->addMenu(QStringLiteral("F&ilters"));
    // Fireworks MX 2004 Filters menu (+ modern extras). Every entry is the destructive form of a Live Effect.
    auto flt = [this](QMenu *menu, const char *id, const QString &text, EffectType t, const QKeySequence &ks = {}) {
        menu->addAction(act(QString::fromLatin1(id), text, ks, [this, t] { effectFilter(int(t)); }));
    };
    QMenu *adj = m->addMenu(QStringLiteral("&Adjust Color"));
    flt(adj, "filter.autocontrast", QStringLiteral("Auto Co&ntrast"), EffectType::AutoContrast);
    flt(adj, "filter.autolevels", QStringLiteral("&Auto Levels"), EffectType::AutoLevels);
    flt(adj, "filter.bc", QStringLiteral("&Brightness/Contrast..."), EffectType::BrightnessContrast);
    flt(adj, "filter.colorbalance", QStringLiteral("Color &Balance..."), EffectType::ColorBalance, QKeySequence(QStringLiteral("Ctrl+Alt+B")));
    flt(adj, "filter.colorfill", QStringLiteral("Color &Fill..."), EffectType::ColorFill);
    flt(adj, "filter.curves", QStringLiteral("&Curves..."), EffectType::Curves, QKeySequence(QStringLiteral("Ctrl+Alt+M")));
    flt(adj, "filter.desat", QStringLiteral("&Desaturate"), EffectType::Desaturate);
    flt(adj, "filter.gradientmap", QStringLiteral("&Gradient Map..."), EffectType::GradientMap);
    flt(adj, "filter.huesat", QStringLiteral("&Hue/Saturation..."), EffectType::HueSaturation, QKeySequence(QStringLiteral("Ctrl+Alt+U")));
    // (Ctrl+U is Edit > Preferences, as in Fireworks MX 2004.)
    flt(adj, "filter.invert", QStringLiteral("&Invert"), EffectType::Invert, QKeySequence(QStringLiteral("Ctrl+Alt+Shift+I")));
    flt(adj, "filter.levels", QStringLiteral("&Levels..."), EffectType::Levels, QKeySequence(QStringLiteral("Ctrl+L")));
    flt(adj, "filter.photofilter", QStringLiteral("&Photo Filter..."), EffectType::PhotoFilter);
    flt(adj, "filter.posterize", QStringLiteral("P&osterize..."), EffectType::Posterize);
    flt(adj, "filter.replacecolor", QStringLiteral("&Replace Color..."), EffectType::ReplaceColor);
    flt(adj, "filter.threshold", QStringLiteral("&Threshold..."), EffectType::Threshold);
    QMenu *blur = m->addMenu(QStringLiteral("&Blur"));
    flt(blur, "filter.blur", QStringLiteral("&Blur"), EffectType::Blur);
    flt(blur, "filter.blurmore", QStringLiteral("Blur &More"), EffectType::BlurMore);
    flt(blur, "filter.gauss", QStringLiteral("&Gaussian Blur..."), EffectType::GaussianBlur);
    flt(blur, "filter.motionblur", QStringLiteral("M&otion Blur..."), EffectType::MotionBlur);
    flt(blur, "filter.radialblur", QStringLiteral("&Radial Blur..."), EffectType::RadialBlur);
    flt(blur, "filter.surfaceblur", QStringLiteral("&Surface Blur..."), EffectType::SurfaceBlur);
    flt(blur, "filter.zoomblur", QStringLiteral("&Zoom Blur..."), EffectType::ZoomBlur);
    QMenu *distort = m->addMenu(QStringLiteral("&Distort"));
    flt(distort, "filter.lens", QStringLiteral("&Lens Correction..."), EffectType::LensCorrection);
    distort->addAction(act(QStringLiteral("filter.liquify"), QStringLiteral("L&iquify..."), {}, [this] { liquifyFilter(); }));
    QMenu *noise = m->addMenu(QStringLiteral("&Noise"));
    flt(noise, "filter.noise", QStringLiteral("&Add Noise..."), EffectType::Noise);
    flt(noise, "filter.denoise", QStringLiteral("&Reduce Noise (Bilateral)..."), EffectType::BilateralDenoise);
    QMenu *other = m->addMenu(QStringLiteral("&Other"));
    flt(other, "filter.toalpha", QStringLiteral("Convert to &Alpha"), EffectType::ConvertToAlpha);
    flt(other, "filter.findedges", QStringLiteral("&Find Edges"), EffectType::FindEdges);
    flt(other, "filter.highpass", QStringLiteral("&High Pass..."), EffectType::HighPass);
    flt(other, "filter.offset", QStringLiteral("&Offset..."), EffectType::Offset);
    flt(other, "filter.feather", QStringLiteral("F&eather Edges..."), EffectType::FeatherEdges);
    other->addAction(act(QStringLiteral("filter.contentaware"), QStringLiteral("&Content-Aware Fill"), QKeySequence(QStringLiteral("Shift+Backspace")), [this] { contentAwareFill(); }));
    QMenu *pix = m->addMenu(QStringLiteral("&Pixel Art"));
    flt(pix, "filter.dither", QStringLiteral("&Dither..."), EffectType::Dither);
    flt(pix, "filter.outline", QStringLiteral("&Outline..."), EffectType::Outline);
    flt(pix, "filter.paletteswap", QStringLiteral("&Palette Swap..."), EffectType::PaletteSwap);
    pix->addAction(act(QStringLiteral("filter.pixelupscale"), QStringLiteral("&Upscale..."), {}, [this] { pixelUpscale(); }));
    QMenu *render = m->addMenu(QStringLiteral("&Render"));
    render->addAction(act(QStringLiteral("filter.texture"), QStringLiteral("&Texture..."), {}, [this] { renderTexture(); }));
    QMenu *sharp = m->addMenu(QStringLiteral("&Sharpen"));
    flt(sharp, "filter.sharpen", QStringLiteral("&Sharpen"), EffectType::Sharpen);
    flt(sharp, "filter.sharpenmore", QStringLiteral("Sharpen &More"), EffectType::SharpenMore);
    flt(sharp, "filter.smartsharpen", QStringLiteral("Smart S&harpen..."), EffectType::SmartSharpen);
    flt(sharp, "filter.unsharp", QStringLiteral("&Unsharp Mask..."), EffectType::UnsharpMask);
    QMenu *tex = m->addMenu(QStringLiteral("&Texture"));
    flt(tex, "filter.normalmap", QStringLiteral("&Normal Map..."), EffectType::NormalMap);
    flt(tex, "filter.ao", QStringLiteral("&Ambient Occlusion..."), EffectType::AmbientOcclusion);
    flt(tex, "filter.makeseamless", QStringLiteral("Make &Seamless..."), EffectType::MakeSeamless);

    // ---- Window
    m = mb->addMenu(QStringLiteral("&Window"));
    for (QDockWidget *d : findChildren<QDockWidget *>()) {
        QAction *a = d->toggleViewAction();
        if (d->objectName() == QLatin1String("ToolsDock")) {
            a->setObjectName(QStringLiteral("window.tools"));
            a->setShortcut(QKeySequence(QStringLiteral("Ctrl+F2")));
            m_actions[a->objectName()] = a;
        }
        if (d->objectName() == QLatin1String("PropertiesDock")) {
            a->setObjectName(QStringLiteral("window.propertiesdock"));
            a->setShortcut(QKeySequence(QStringLiteral("Ctrl+F3")));
            m_actions[a->objectName()] = a;
        }
        if (d->objectName() == QLatin1String("FindDock")) {
            a->setText(QStringLiteral("&Find && Replace"));
            a->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
            a->setObjectName(QStringLiteral("window.find"));
            m_actions[QStringLiteral("window.find")] = a;
            connect(a, &QAction::toggled, this, [this, d](bool on) { if (on) { d->raise(); m_find->setFocus(); } });
        }
        m->addAction(a);
    }
    // the context menu's "Properties...": shows the Property Inspector and gives it focus
    act(QStringLiteral("view.properties"), QStringLiteral("&Properties..."), {}, [this] {
        if (auto *dock = findChild<QDockWidget *>(QStringLiteral("PropertiesDock"))) { dock->show(); dock->raise(); }
        m_pi->setFocus(Qt::OtherFocusReason);
    });
    m->addSeparator();
    const QHash<QString, QString> keys = {{QStringLiteral("Optimize"), QStringLiteral("F6")}, {QStringLiteral("Layers"), QStringLiteral("F2")},
                                          {QStringLiteral("Frames"), QStringLiteral("Shift+F2")}, {QStringLiteral("History"), QStringLiteral("Shift+F10")},
                                          {QStringLiteral("Align"), QStringLiteral("Shift+F7")}, {QStringLiteral("Styles"), QStringLiteral("Shift+F11")},
                                          {QStringLiteral("Library"), QStringLiteral("F11")}};
    for (CollapsiblePanel *p : m_panels) {
        QAction *a = act(QStringLiteral("window.") + p->title().toLower(), p->title(), QKeySequence(keys.value(p->title())), [p] {
            if (!p->isVisible() || p->isCollapsed()) { p->show(); p->setCollapsed(false); } else p->setCollapsed(true);
        });
        m->addAction(a);
    }

    // ---- Help
    m = mb->addMenu(QStringLiteral("&Help"));
    m->addAction(act(QStringLiteral("help.shortcuts"), QStringLiteral("&Keyboard Shortcuts"), QKeySequence(QStringLiteral("F1")), [this] { dialogs::showShortcuts(this); }));
    m->addAction(act(QStringLiteral("help.formats"), QStringLiteral("Supported &Formats"), {}, [this] {
        QMessageBox::information(this, QStringLiteral("Supported Formats"), ImageIO::codecSummary());
    }));
    m->addSeparator();
    m->addAction(act(QStringLiteral("help.about"), QStringLiteral("&About %1").arg(brand::name()), {}, [this] {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("About %1").arg(brand::name()));
        box.setIconPixmap(icons::appIcon(64));
        box.setText(QStringLiteral("<h3>%1 %2</h3><p>%3</p>"
                                   "<p>Toolbar icons are from <b>GNOME-Wine</b> / GNOME-Colors (GPLv2+), hue-shifted to each interface theme. "
                                   "Preferences &gt; General &gt; Use system icon theme restores the desktop theme.</p>"
                                   "<p>%4</p>")
                           .arg(brand::name(), QStringLiteral(LW_VERSION),
                                QStringLiteral(LW_ABOUT_TAGLINE), QStringLiteral(LW_ABOUT_CREDIT)));
        box.exec();
    }));
    m->addAction(act(QStringLiteral("help.aboutqt"), QStringLiteral("About &Qt"), {}, [] { QApplication::aboutQt(); }));

    // ---- Tool shortcuts and colour keys (0.3.11)
    // One QAction per tool. Defaults still share Fireworks letters within a flyout; after the
    // user rebinds, each tool keeps its own sequences (Manager no longer force-syncs the group).
    // Manager::apply gives WindowShortcut only to the catalog-first owner of each sequence so Qt
    // fires once; the handler below cycles among tools that currently share that key.
    // Quick Mask: only when the fired tool still has Q bound AND a bitmap tool / Quick Mask is
    // active — never based on the hardcoded toolShortcut() letter alone (that broke Scale rebinds).
    auto *toolGroup = new QActionGroup(this);
    auto selectOrCycle = [this](Tool t) {
        auto &sm = shortcuts::Manager::get();
        const QString id = toolActionId(t);
        const QKeySequence primary = sm.firstShortcut(id);
        const bool stillHasQ = sm.sequences(id).contains(QKeySequence(QStringLiteral("Q")));
        if (stillHasQ && toolIsTransform(t) && currentDoc() &&
            (toolIsBitmap(appState().tool()) || quickMaskActive())) {
            toggleQuickMask();
            return;
        }
        QList<Tool> same;
        if (!primary.isEmpty()) {
            for (const QString &sid : sm.toolsSharingSequence(primary)) {
                if (!sid.startsWith(QLatin1String("tool."))) continue;
                const Tool cand = toolFromActionId(sid);
                // toolFromActionId returns Pointer for unknown — only accept when id matches
                if (toolActionId(cand) == sid && toolAvailable(cand)) same.append(cand);
            }
        }
        // Prefer the flyout/toolGroup order when it covers the same set (Fireworks cycle order).
        if (same.size() > 1) {
            const QList<Tool> grp = ::toolGroup(same.first());
            if (grp.size() == same.size()
                && std::all_of(same.begin(), same.end(), [&](Tool x) { return grp.contains(x); }))
                same = QList<Tool>(grp.begin(), grp.end());
        }
        if (same.size() <= 1) { appState().setTool(t); return; }
        int k = same.indexOf(appState().tool());
        appState().setTool(k < 0 ? t : same[(k + 1) % same.size()]);
    };
    for (int i = 0; i <= int(kLastTool); ++i) {
        Tool t = Tool(i);
        QList<Tool> same;
        const QString keyStr = toolShortcut(t);
        for (int j = 0; j <= int(kLastTool); ++j)
            if (!keyStr.isEmpty() && toolShortcut(Tool(j)) == keyStr) same << Tool(j);
        if (same.isEmpty()) same << t;
        if (same.size() > 1) {
            const QList<Tool> grp = ::toolGroup(same.first());
            if (grp.size() == same.size() && std::all_of(same.begin(), same.end(), [&](Tool x) { return grp.contains(x); }))
                same = QList<Tool>(grp.begin(), grp.end());
        }
        const bool owner = same.first() == t;
        const QKeySequence key = keyStr.isEmpty() ? QKeySequence() : QKeySequence(keyStr);
        QAction *a = act(toolActionId(t), toolName(t), key, [selectOrCycle, t] {
            selectOrCycle(t);
        });
        a->setCheckable(true);
        a->setChecked(t == appState().tool());
        if (!owner && !key.isEmpty()) a->setShortcutContext(Qt::WidgetShortcut);
        a->setProperty("lwToolShortcutOwner", owner);
        QStringList shareIds;
        for (Tool x : same) shareIds << toolActionId(x);
        a->setProperty("lwToolShareGroup", QVariant::fromValue(shareIds));
        toolGroup->addAction(a);
    }
    // Liquify dialog tools (defaults; dialog reads the registry so rebinds apply)
    {
        static const char *ids[] = {"warp", "pinch", "bloat", "twirlcw", "twirlccw", "reconstruct"};
        static const char *labels[] = {"Warp", "Pinch", "Bloat", "Twirl CW", "Twirl CCW", "Reconstruct"};
        static const char *liquifyKeys[] = {"W", "S", "B", "C", "Alt+C", "R"};
        for (int i = 0; i < 6; ++i) {
            QAction *a = act(QStringLiteral("liquify.") + QLatin1String(ids[i]),
                             QStringLiteral("Liquify: ") + QLatin1String(labels[i]),
                             QKeySequence(QLatin1String(liquifyKeys[i])), [] {});
            a->setShortcutContext(Qt::WidgetShortcut); // only the Liquify dialog buttons fire them
        }
    }
    // Panel rename (default F2): editable; Layers/Frames/Styles eventFilters match via the registry.
    // Shares F2 with window.layers (toggle) — focused panel list wins for rename.
    act(QStringLiteral("panels.rename"), QStringLiteral("Rename Panel Item"), QKeySequence(Qt::Key_F2), [] {})
        ->setShortcutContext(Qt::WidgetShortcut);

    act(QStringLiteral("colors.default"), QStringLiteral("Default Colors"), QKeySequence(QStringLiteral("D")), [] { appState().strokeOn = appState().fillOn = true; appState().defaultColors(); });
    act(QStringLiteral("colors.swap"), QStringLiteral("Swap Colors"), QKeySequence(QStringLiteral("X")), [] {
        // X swaps the Path Scrubber's Increase / Decrease mode while that tool is active
        if (appState().tool() == Tool::PathScrubber) {
            appState().psMode = appState().psMode ? 0 : 1;
            appState().notifyOptions();
            emit appState().statusMessage(appState().psMode ? QStringLiteral("Path Scrubber: Decrease (\u2212)") : QStringLiteral("Path Scrubber: Increase (+)"));
            return;
        }
        appState().swapColors();
    });
    act(QStringLiteral("tool.sizedown"), QStringLiteral("Decrease Tip Size"), QKeySequence(QStringLiteral("[")), [this] { adjustTipSize(-1); });
    act(QStringLiteral("tool.sizeup"), QStringLiteral("Increase Tip Size"), QKeySequence(QStringLiteral("]")), [this] { adjustTipSize(1); });
    // Shift+[ arrives as { on most layouts; accept both spellings
    act(QStringLiteral("tool.softdown"), QStringLiteral("Decrease Tip Softness"), {}, [this] { adjustTipSize(-1, true); })
        ->setShortcuts({QKeySequence(QStringLiteral("Shift+[")), QKeySequence(QStringLiteral("{")), QKeySequence(QStringLiteral("Shift+{"))});
    act(QStringLiteral("tool.softup"), QStringLiteral("Increase Tip Softness"), {}, [this] { adjustTipSize(1, true); })
        ->setShortcuts({QKeySequence(QStringLiteral("Shift+]")), QKeySequence(QStringLiteral("}")), QKeySequence(QStringLiteral("Shift+}"))});

    // ---- Canvas / view keys that used to be hardcoded in Canvas::keyPressEvent (0.3.7)
    // These are QActions so the Keyboard prefs can edit them; Canvas matches via shortcuts::Manager.
    act(QStringLiteral("canvas.nudgeLeft"), QStringLiteral("Nudge Left"), QKeySequence(Qt::Key_Left), [] {});
    act(QStringLiteral("canvas.nudgeRight"), QStringLiteral("Nudge Right"), QKeySequence(Qt::Key_Right), [] {});
    act(QStringLiteral("canvas.nudgeUp"), QStringLiteral("Nudge Up"), QKeySequence(Qt::Key_Up), [] {});
    act(QStringLiteral("canvas.nudgeDown"), QStringLiteral("Nudge Down"), QKeySequence(Qt::Key_Down), [] {});
    act(QStringLiteral("canvas.nudgeLeft10"), QStringLiteral("Nudge Left 10 px"), QKeySequence(Qt::SHIFT | Qt::Key_Left), [] {});
    act(QStringLiteral("canvas.nudgeRight10"), QStringLiteral("Nudge Right 10 px"), QKeySequence(Qt::SHIFT | Qt::Key_Right), [] {});
    act(QStringLiteral("canvas.nudgeUp10"), QStringLiteral("Nudge Up 10 px"), QKeySequence(Qt::SHIFT | Qt::Key_Up), [] {});
    act(QStringLiteral("canvas.nudgeDown10"), QStringLiteral("Nudge Down 10 px"), QKeySequence(Qt::SHIFT | Qt::Key_Down), [] {});
    act(QStringLiteral("canvas.confirm"), QStringLiteral("Confirm / Finish (Enter)"), {}, [] {})
        ->setShortcuts({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    act(QStringLiteral("canvas.cancel"), QStringLiteral("Cancel / Deselect (Escape)"), QKeySequence(Qt::Key_Escape), [] {});
    act(QStringLiteral("canvas.tempPan"), QStringLiteral("Temporary Pan (hold Space)"), QKeySequence(Qt::Key_Space), [] {});
    act(QStringLiteral("canvas.removeLastPoint"), QStringLiteral("Remove Last Polygon Point"), QKeySequence(Qt::Key_Backspace), [] {});
    // canvas.* keys are handled in Canvas::keyPressEvent via shortcuts::Manager::matches — keep them as
    // WidgetShortcut so Left/Space/Escape do not steal focus from text fields and spin boxes
    for (const char *cid : {"canvas.nudgeLeft", "canvas.nudgeRight", "canvas.nudgeUp", "canvas.nudgeDown",
                            "canvas.nudgeLeft10", "canvas.nudgeRight10", "canvas.nudgeUp10", "canvas.nudgeDown10",
                            "canvas.confirm", "canvas.cancel", "canvas.tempPan", "canvas.removeLastPoint"}) {
        if (QAction *a = m_actions.value(QString::fromLatin1(cid))) a->setShortcutContext(Qt::WidgetShortcut);
    }

    // ---- Main toolbar
    QToolBar *tb = addToolBar(QStringLiteral("Main"));
    tb->setObjectName(QStringLiteral("MainToolbar"));
    tb->setIconSize(QSize(18, 18));
    tb->setMovable(false);
    for (const char *id : std::initializer_list<const char *>{"file.new", "file.open", "file.save", nullptr, "file.import", "file.export", nullptr, "edit.undo", "edit.redo"}) {
        if (!id) { tb->addSeparator(); continue; }
        tb->addAction(m_actions[QString::fromLatin1(id)]);
    }
    m_actions[QStringLiteral("file.import")]->setIcon(stdIcon(QStyle::SP_ArrowDown));
    m_actions[QStringLiteral("file.export")]->setIcon(stdIcon(QStyle::SP_ArrowUp));
}

// ---------------------------------------------------------------- [ / ] tip size
double MainWindow::stepTipSize(double v, int dir) {
    // 1 px steps below 10, otherwise ~10%, always at least 1 px; clamped to 1..1000
    double n;
    if (dir > 0) n = v < 10 ? std::floor(v) + 1 : std::max(v + 1, std::round(v * 1.1));
    else n = v <= 10 ? std::ceil(v) - 1 : std::min(v - 1, std::round(v / 1.1));
    return std::clamp(n, 1.0, 1000.0);
}

bool MainWindow::adjustTipSize(int dir, bool softness) {
    QWidget *f = QApplication::focusWidget();
    if (f && (qobject_cast<QLineEdit *>(f) || qobject_cast<QAbstractSpinBox *>(f) || qobject_cast<QTextEdit *>(f) ||
              qobject_cast<QPlainTextEdit *>(f) || (f->parentWidget() && qobject_cast<QAbstractSpinBox *>(f->parentWidget()))))
        return false;   // typing [ or ] into a field
    AppState &as = appState();
    const Tool t = as.tool();
    static const QList<Tool> tipTools = {Tool::Brush, Tool::Pencil, Tool::Eraser, Tool::Blur, Tool::Sharpen, Tool::Dodge,
                                         Tool::Burn, Tool::Smudge, Tool::RubberStamp, Tool::Healing, Tool::SpotHealing,
                                         Tool::ReplaceColor, Tool::RedEye};
    static const QList<Tool> strokeTools = {Tool::Line, Tool::Pen, Tool::Rect, Tool::Ellipse, Tool::Polygon};
    if (softness) {
        if (!tipTools.contains(t) || t == Tool::Pencil) return false;   // the pencil is always hard
        const int e = std::clamp(as.brushEdge + dir * 10, 0, 100);
        if (e == as.brushEdge) return true;
        as.brushEdge = e;
        as.notifyOptions();
        emit as.statusMessage(QStringLiteral("Edge: %1%").arg(e));
        return true;
    }
    if (tipTools.contains(t)) {
        as.brushSize = int(stepTipSize(as.brushSize, dir));
        as.notifyOptions();
        emit as.statusMessage(QStringLiteral("Tip size: %1 px").arg(as.brushSize));
        return true;
    }
    if (t == Tool::PathScrubber) {
        as.psSize = int(stepTipSize(as.psSize, dir));
        as.notifyOptions();
        emit as.statusMessage(QStringLiteral("Size: %1 px").arg(as.psSize));
        return true;
    }
    if (t == Tool::Freeform || t == Tool::ReshapeArea) {
        as.pathToolSize = int(stepTipSize(as.pathToolSize, dir));
        as.notifyOptions();
        emit as.statusMessage(QStringLiteral("Size: %1 px").arg(as.pathToolSize));
        return true;
    }
    if (strokeTools.contains(t)) {
        as.strokeWidth = stepTipSize(as.strokeWidth, dir);
        as.notifyOptions();
        emit as.statusMessage(QStringLiteral("Stroke: %1 px").arg(as.strokeWidth));
        return true;
    }
    if (t == Tool::Pointer || t == Tool::Subselect) {
        Document *d = currentDoc();
        if (!d) return false;
        QVector<VObject *> objs;
        for (VObject *o : d->selectedEditable())
            if (o->strokeOn && o->type != ObjType::Bitmap && o->type != ObjType::Slice && o->type != ObjType::Hotspot &&
                o->type != ObjType::Instance) objs << o;
        if (objs.isEmpty()) return false;
        // a held key autorepeats: fold the whole run into one undo step
        static QPointer<Document> mergeDoc;
        static QTimer *mergeEnd = nullptr;
        if (!mergeEnd) {
            mergeEnd = new QTimer(this);
            mergeEnd->setSingleShot(true);
            connect(mergeEnd, &QTimer::timeout, this, [] { if (mergeDoc) mergeDoc->endMerge(); mergeDoc = nullptr; });
        }
        if (mergeDoc != d) {
            if (mergeDoc) mergeDoc->endMerge();
            mergeDoc = d;
            d->beginMerge();
        }
        mergeEnd->start(600);
        for (VObject *o : objs) {
            o->strokeWidth = stepTipSize(o->strokeWidth, dir);
        }
        d->commit(QStringLiteral("Stroke Size"));
        emit as.statusMessage(QStringLiteral("Stroke: %1 px").arg(objs.first()->strokeWidth));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- documents
DocumentView *MainWindow::addDocument(Document *doc) {
    if (doc->filePath.isEmpty() && doc->untitledName.isEmpty()) doc->untitledName = QStringLiteral("Untitled-%1").arg(++m_untitled);
    auto *view = new DocumentView(doc);
    doc->setParent(view);
    int idx = m_tabs->addTab(view, doc->title());
    connect(doc, &Document::historyChanged, this, [this] { updateTitle(); updateActions(); });
    connect(doc, &Document::selectionChanged, this, &MainWindow::updateActions);
    connect(view->canvas(), &Canvas::openDocumentRequested, this, [this](const QString &path) { openFile(path); }, Qt::QueuedConnection);
    connect(view->canvas(), &Canvas::editSymbolRequested, this, [this, doc](quint64 id) { editSymbol(doc, id); });
    connect(view->canvas(), &Canvas::contextMenuRequested, this, [this](QPoint global) { showCanvasContextMenu(global); });
    connect(view->canvas(), &Canvas::cursorMoved, this, [this](QPointF p) {
        m_statusPos->setText(p.x() < -1e8 ? QString() : QStringLiteral("%1, %2").arg(qFloor(p.x())).arg(qFloor(p.y())));
    });
    connect(view, &DocumentView::zoomChanged, this, [this, view](double z) {
        if (view == currentView()) m_statusZoom->setZoom(z);
    });
    connect(view, &DocumentView::modeChanged, this, [this](int mode) {
        if (auto *a = action(QStringLiteral("view.mode%1").arg(mode))) a->setChecked(true);
        updateOptimizeTitle();
    });
    connect(view, &DocumentView::paneSelected, this, [this, doc](int) {
        // the Optimize panel (and File > Export) now follow the selected pane's own settings
        if (doc == currentDoc()) m_optimize->setDocument(doc);
        updateOptimizeTitle();
        statusBar()->showMessage(QStringLiteral("Optimize: %1").arg(describeOptions(doc->exportOptions)), 4000);
    });
    m_tabs->setCurrentIndex(idx);
    view->canvas()->setFocus();
    return view;
}

DocumentView *MainWindow::currentView() const { return qobject_cast<DocumentView *>(m_tabs->currentWidget()); }
Document *MainWindow::currentDoc() const { auto *v = currentView(); return v ? v->doc() : nullptr; }

// "Optimize: pane N" while 2-Up / 4-Up is showing (the Original is pane 1, as the captions read)
void MainWindow::updateOptimizeTitle() {
    for (CollapsiblePanel *cp : m_panels) {
        if (cp->title() != QLatin1String("Optimize")) continue;
        DocumentView *v = currentView();
        QString t = QStringLiteral("Optimize");
        if (v && v->mode() == 2) t = QStringLiteral("Optimize: pane 2");
        else if (v && v->mode() == 3) t = QStringLiteral("Optimize: pane %1").arg(v->doc()->selectedPane + 2);
        cp->setHeaderText(t);
    }
}

void MainWindow::syncToolEnvironmentTo(Document *d) {
    if (!d) return;
    d->toolEnv = appState().captureEnvironment();
    // not dirty: tool options are working state, as in Fireworks
    if (appState().prefs.carryToolSettings) appState().saveCarriedEnvironment();
}

void MainWindow::applyToolEnvironmentFrom(Document *d) {
    // Point m_envDoc first so optionsChanged → pushEnv writes onto the document we are
    // applying, not the one we just left.
    m_envDoc = d;
    if (!d) {
        if (!appState().prefs.carryToolSettings) appState().applyEnvironment({});
        return;
    }
    if (!d->toolEnv.isEmpty()) {
        // .lwd (or previously stamped) settings win
        appState().applyEnvironment(d->toolEnv);
        if (appState().prefs.carryToolSettings) appState().saveCarriedEnvironment();
        return;
    }
    // empty toolEnv: new canvas / older file / opened image
    if (appState().prefs.carryToolSettings) {
        // First document this session: restore the persisted carried state if any
        if (m_tabs && m_tabs->count() <= 1)
            appState().loadCarriedEnvironment();
        // Keep last-used AppState and stamp it onto the new document
        d->toolEnv = appState().captureEnvironment();
        return;
    }
    appState().applyEnvironment({}); // built-in defaults
}

void MainWindow::currentChanged() {
    updateOptimizeTitle();
    Document *d = currentDoc();
    if (m_envDoc && m_envDoc != d) syncToolEnvironmentTo(m_envDoc);
    if (d != m_envDoc) applyToolEnvironmentFrom(d);
    m_pi->setDocument(d);
    m_layers->setDocument(d);
    m_frames->setDocument(d);
    m_history->setDocument(d);
    m_optimize->setDocument(d);
    m_align->setDocument(d);
    m_styles->setDocument(d);
    m_library->setDocument(d);
    if (auto *v = currentView()) {
        m_statusZoom->setEnabled(true);
        m_statusZoom->setZoom(v->zoom());
        if (auto *a = action(QStringLiteral("view.mode%1").arg(v->mode()))) a->setChecked(true);
    } else {
        m_statusZoom->setEnabled(false);
        m_statusZoom->setZoom(1);
        m_statusPos->clear();
    }
    updateTitle();
    updateActions();
}

void MainWindow::updateTitle() {
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *v = qobject_cast<DocumentView *>(m_tabs->widget(i));
        if (!v) continue;
        m_tabs->setTabText(i, v->doc()->title() + (v->doc()->isDirty() ? QStringLiteral("*") : QString()));
        m_tabs->setTabToolTip(i, v->doc()->filePath);
    }
    Document *d = currentDoc();
    if (d) {
        setWindowTitle(QStringLiteral("%1 - [%2%3 @ %4%]").arg(brand::name(), d->title(), d->isDirty() ? QStringLiteral("*") : QString())
                           .arg(qRound(currentView()->zoom() * 100)));
        m_statusSize->setText(QStringLiteral("%1 x %2").arg(d->s.width).arg(d->s.height));
    } else {
        setWindowTitle(brand::name());
        m_statusSize->clear();
    }
}

void MainWindow::updateActions() {
    Document *d = currentDoc();
    const bool has = d != nullptr;
    const bool sel = has && !d->selection.isEmpty();
    static const QStringList needDoc = {
        QStringLiteral("file.close"), QStringLiteral("file.save"), QStringLiteral("file.saveas"), QStringLiteral("file.export"),
        QStringLiteral("file.exportpreview"), QStringLiteral("file.exporthtml"), QStringLiteral("file.exportfavicons"), QStringLiteral("edit.insertframe"), QStringLiteral("edit.paste"), QStringLiteral("select.all"), QStringLiteral("select.none"),
        QStringLiteral("select.inverse"), QStringLiteral("modify.imagesize"), QStringLiteral("modify.canvassize"), QStringLiteral("modify.trim"),
        QStringLiteral("modify.fit"), QStringLiteral("modify.canvasrotate"), QStringLiteral("modify.flattenlayers"), QStringLiteral("cmd.exportframes"), QStringLiteral("view.fit"),
        QStringLiteral("view.actual"), QStringLiteral("view.resetpanes"), QStringLiteral("view.zoomin"), QStringLiteral("view.zoomout")};
    for (const QString &id : needDoc) if (auto *a = action(id)) a->setEnabled(has);
    static const QStringList needSel = {
        QStringLiteral("edit.cut"), QStringLiteral("edit.copy"), QStringLiteral("edit.duplicate"), QStringLiteral("edit.clone"),
        QStringLiteral("edit.insertslice"), QStringLiteral("edit.inserthotspot"),
        QStringLiteral("modify.flattensel"), QStringLiteral("arrange.front"), QStringLiteral("arrange.forward"), QStringLiteral("arrange.backward"),
        QStringLiteral("arrange.back"), QStringLiteral("modify.rot180"), QStringLiteral("modify.rot90cw"), QStringLiteral("modify.rot90ccw"),
        QStringLiteral("modify.fliph"), QStringLiteral("modify.flipv"), QStringLiteral("modify.numeric"), QStringLiteral("modify.removexform"), QStringLiteral("cmd.shadow"), QStringLiteral("cmd.removefx"),
        QStringLiteral("symbol.convert"), QStringLiteral("symbol.tween"), QStringLiteral("anim.animate"),
        QStringLiteral("path.join"), QStringLiteral("path.split"), QStringLiteral("path.union"), QStringLiteral("path.intersect"),
        QStringLiteral("path.punch"), QStringLiteral("path.crop"), QStringLiteral("path.simplify"), QStringLiteral("path.expand"),
        QStringLiteral("path.inset"), QStringLiteral("path.convert"), QStringLiteral("modify.tracebitmap"),
        QStringLiteral("view.hide"), QStringLiteral("modify.lock"), QStringLiteral("modify.savestyle"), QStringLiteral("file.exportselected")};
    for (const QString &id : needSel) if (auto *a = action(id)) a->setEnabled(sel);
    for (const QString &id : {QStringLiteral("view.showall"), QStringLiteral("modify.unlockall")}) if (auto *a = action(id)) a->setEnabled(has);
    if (auto *a = action(QStringLiteral("edit.pasteattributes"))) a->setEnabled(sel && !g_clipboard.isEmpty());
    {   // groups and masks
        int n = 0, groups = 0;
        if (sel) for (VObject *o : d->selectedOutermost()) { ++n; if (o->type == ObjType::Group) ++groups; }
        const bool clip = !g_clipboard.isEmpty() || (QApplication::clipboard()->mimeData() && QApplication::clipboard()->mimeData()->hasImage());
        VObject *owner = has ? d->find(d->maskOwnerOfSelection()) : nullptr;
        const bool masked = owner && owner->hasMask();
        auto en = [this](const char *id, bool on) { if (auto *a = action(QString::fromLatin1(id))) a->setEnabled(on); };
        en("modify.group", n >= 1);
        en("modify.ungroup", groups > 0);
        en("mask.pasteasmask", n >= 1 && clip);
        en("mask.pasteinside", n == 1 && clip);
        en("edit.pasteinside", n == 1 && clip);
        en("mask.groupasmask", n >= 2);
        en("mask.revealall", n >= 1);
        en("mask.hideall", n >= 1);
        en("mask.disable", masked && owner->maskEnabled);
        en("mask.enable", masked && !owner->maskEnabled);
        en("mask.link", masked);
        en("mask.delete", masked);
        en("modify.converttobitmap", n >= 1);
        en("select.exitgroup", has && d->enteredGroup != 0);
        if (auto *a = action(QStringLiteral("mask.link"))) { QSignalBlocker b(a); a->setChecked(masked && owner->maskLinked); }
    }
    if (auto *a = action(QStringLiteral("modify.editgradient"))) {
        bool grad = false;
        if (sel) for (VObject *o : d->selectedEditable()) if (o->isVector() && o->fillOn && fills::isGradient(o->fillKind)) grad = true;
        a->setEnabled(grad);
    }
    if (auto *a = action(QStringLiteral("modify.editstroke"))) {
        a->setEnabled(m_pi && m_pi->editStrokeTarget() != PropertyInspector::EditStrokeTarget::None);
    }
    // per-document view aids
    for (const QString &id : {QStringLiteral("view.grid"), QStringLiteral("view.gridsnap"), QStringLiteral("view.gridedit"), QStringLiteral("view.guides"),
                              QStringLiteral("view.guideslock"), QStringLiteral("view.guidessnap"), QStringLiteral("view.guidesedit")})
        if (auto *a = action(id)) a->setEnabled(has);
    auto check = [this](const QString &id, bool on) { if (auto *a = action(id)) { QSignalBlocker b(a); a->setChecked(on); } };
    check(QStringLiteral("view.grid"), has && d->grid.show);
    check(QStringLiteral("view.gridsnap"), has && d->grid.snap);
    check(QStringLiteral("view.guides"), has && d->guides.show);
    check(QStringLiteral("view.guideslock"), has && d->guides.lock);
    check(QStringLiteral("view.guidessnap"), has && d->guides.snap);
    check(QStringLiteral("view.rulers"), appState().showRulers);
    if (auto *a = action(QStringLiteral("edit.undo"))) {
        a->setEnabled(has && d->canUndo());
        a->setText(has && d->canUndo() ? QStringLiteral("&Undo %1").arg(d->history()[d->historyIndex()].label) : QStringLiteral("&Undo"));
    }
    if (auto *a = action(QStringLiteral("edit.redo"))) {
        a->setEnabled(has && d->canRedo());
        a->setText(has && d->canRedo() ? QStringLiteral("&Redo %1").arg(d->history()[d->historyIndex() + 1].label) : QStringLiteral("&Redo"));
    }
    syncWebFeatureUi();
}

bool MainWindow::maybeSave(Document *doc) {
    if (m_symbolEdits.contains(doc)) { pushSymbolEdit(doc); return true; } // edits are already in the parent
    if (!doc->isDirty()) return true;
    auto r = QMessageBox::question(this, brand::name(), QStringLiteral("Save changes to \"%1\" before closing?").arg(doc->title()),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return saveDocument(doc, false);
    return true;
}

bool MainWindow::closeTab(int index) {
    auto *v = qobject_cast<DocumentView *>(m_tabs->widget(index));
    if (!v) return true;
    if (!maybeSave(v->doc())) return false;
    if (m_frames->isPlaying() && v == currentView()) m_frames->play(false);
    Document *closing = v->doc();
    m_symbolEdits.remove(closing);
    m_tabs->removeTab(index);
    v->deleteLater();
    render::clearObjectVisualCache();
    // symbol editors of a closed document go with it
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        auto *ev = qobject_cast<DocumentView *>(m_tabs->widget(i));
        if (!ev) continue;
        auto it = m_symbolEdits.find(ev->doc());
        if (it == m_symbolEdits.end() || (it->parent && it->parent != closing)) continue;
        m_symbolEdits.erase(it);
        m_tabs->removeTab(i);
        ev->deleteLater();
    }
    if (m_tabs->count() == 0) currentChanged();
    return true;
}

// ---------------------------------------------------------------- symbols
Document *MainWindow::editSymbol(Document *parent, quint64 symId) {
    if (!parent) return nullptr;
    for (auto it = m_symbolEdits.begin(); it != m_symbolEdits.end(); ++it) {
        if (it->parent != parent || it->symId != symId) continue;
        for (int i = 0; i < m_tabs->count(); ++i)
            if (auto *v = qobject_cast<DocumentView *>(m_tabs->widget(i)); v && v->doc() == it.key()) m_tabs->setCurrentIndex(i);
        return it.key();
    }
    const Symbol *sym = parent->symbol(symId);
    if (!sym) return nullptr;
    auto *ed = new Document;
    QRectF vb;
    {
        SymbolScope scope(&parent->s.symbols);
        vb = sym->visualBounds();
    }
    ed->s.width = qMax(parent->s.width, vb.isEmpty() ? 0 : int(std::ceil(vb.right())));
    ed->s.height = qMax(parent->s.height, vb.isEmpty() ? 0 : int(std::ceil(vb.bottom())));
    ed->s.canvasColor = parent->s.canvasColor;
    ed->s.symbols = parent->s.symbols; // nested instances
    LayerInfo li;
    li.id = parent->newId();
    li.name = QStringLiteral("Layer 1");
    ed->s.layers = {li};
    const QStringList states = buttonStateNames();
    for (int i = 0; i < sym->frameCount(); ++i) {
        Frame f;
        f.name = sym->kind == SymbolKind::Button ? states.value(i) : QStringLiteral("Frame %1").arg(i + 1);
        f.layers = {i < sym->frames.size() ? sym->frames[i] : QVector<VObject>()};
        ed->s.frames.append(f);
    }
    // ids made in the editor must not collide with the parent's
    ed->reserveIds(parent->lastId() + (quint64(1) << 24));
    ed->untitledName = QStringLiteral("Symbol: %1").arg(sym->name);
    ed->resetHistory(QStringLiteral("Edit Symbol"));
    m_symbolEdits.insert(ed, SymbolEdit{parent, symId, ed->historyIndex()});
    connect(ed, &Document::historyChanged, this, [this, ed] { pushSymbolEdit(ed); });
    addDocument(ed);
    statusBar()->showMessage(QStringLiteral("Editing symbol \"%1\": changes update every instance").arg(sym->name), 5000);
    return ed;
}

void MainWindow::pushSymbolEdit(Document *ed) {
    auto it = m_symbolEdits.find(ed);
    if (it == m_symbolEdits.end() || ed->historyIndex() == it->pushedIndex) return;
    it->pushedIndex = ed->historyIndex();
    Document *p = it->parent;
    if (!p || !p->symbol(it->symId)) return;
    QVector<QVector<VObject>> frames;
    for (const Frame &f : ed->s.frames) {
        QVector<VObject> objs;
        for (int li = 0; li < f.layers.size() && li < ed->s.layers.size(); ++li) {
            if (ed->s.layers[li].web || !ed->s.layers[li].visible) continue;
            for (const VObject &o : f.layers[li]) if (!o.isWeb()) objs << o;
        }
        frames << objs;
    }
    p->reserveIds(ed->lastId());
    p->setSymbolFrames(it->symId, frames);
    p->commit(QStringLiteral("Edit Symbol"));
    ed->markClean();
}

void MainWindow::convertToSymbol() {
    Document *d = currentDoc();
    if (!d || d->selection.isEmpty()) return;
    QString name = d->uniqueSymbolName(QStringLiteral("Symbol"));
    SymbolKind kind = SymbolKind::Graphic;
    if (!symbolPropertiesDialog(this, QStringLiteral("Convert to Symbol"), name, kind)) return;
    if (d->convertToSymbol(name, kind)) d->commit(QStringLiteral("Convert to Symbol"));
}

void MainWindow::tweenInstancesDialog() {
    Document *d = currentDoc();
    if (!d) return;
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("tweenDialog"));
    dlg.setWindowTitle(QStringLiteral("Tween Instances"));
    auto *f = new QFormLayout(&dlg);
    auto *steps = new PopupSliderField;
    steps->setObjectName(QStringLiteral("tweenSteps"));
    steps->setRange(1, 250);
    steps->setValue(5);
    auto *dist = new QCheckBox(QStringLiteral("Distribute to frames"));
    dist->setObjectName(QStringLiteral("tweenDistribute"));
    f->addRow(QStringLiteral("Steps:"), steps);
    f->addRow(dist);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    f->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    if (d->tweenInstances(steps->value(), dist->isChecked())) d->commit(QStringLiteral("Tween Instances"));
    else if (!qApp->property("lw_testing").toBool())
        QMessageBox::information(this, QStringLiteral("Tween Instances"), QStringLiteral("Select two or more instances of the same symbol."));
}

void MainWindow::animateSelectionDialog() {
    Document *d = currentDoc();
    if (!d || d->selection.isEmpty()) return;
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("animateDialog"));
    dlg.setWindowTitle(QStringLiteral("Animate"));
    auto *f = new QFormLayout(&dlg);
    auto *frames = new PopupSliderField; frames->setObjectName(QStringLiteral("animFrames")); frames->setRange(1, 250); frames->setValue(5);
    auto *mx = new PopupSliderField; mx->setSliderRange(-500, 500); mx->setObjectName(QStringLiteral("animMoveX")); mx->setRange(-5000, 5000); mx->setValue(72); mx->setSuffix(QStringLiteral(" px"));
    auto *my = new PopupSliderField; my->setSliderRange(-500, 500); my->setObjectName(QStringLiteral("animMoveY")); my->setRange(-5000, 5000); my->setSuffix(QStringLiteral(" px"));
    auto *sc = new PopupSliderField; sc->setSliderRange(1, 400); sc->setObjectName(QStringLiteral("animScale")); sc->setRange(1, 1000); sc->setValue(100); sc->setSuffix(QStringLiteral(" %"));
    auto *o1 = new PopupSliderField; o1->setObjectName(QStringLiteral("animOpacityFrom")); o1->setRange(0, 100); o1->setValue(100);
    auto *o2 = new PopupSliderField; o2->setObjectName(QStringLiteral("animOpacityTo")); o2->setRange(0, 100); o2->setValue(100);
    f->addRow(QStringLiteral("Frames:"), frames);
    f->addRow(QStringLiteral("Move X:"), mx);
    f->addRow(QStringLiteral("Move Y:"), my);
    f->addRow(QStringLiteral("Scale to:"), sc);
    f->addRow(QStringLiteral("Opacity from:"), o1);
    f->addRow(QStringLiteral("Opacity to:"), o2);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    f->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    if (d->animateSelection(frames->value(), QPointF(mx->value(), my->value()), sc->value(), o1->value(), o2->value()))
        d->commit(QStringLiteral("Animate Selection"));
}

void MainWindow::showNotes(const QString &title, const QString &intro, const QStringList &notes) {
    if (notes.isEmpty()) return;
    m_lastNotes = notes;
    // modeless and non-blocking, so drag-and-drop of several files is not held up and the
    // canvas keeps keyboard focus; only the newest note box is kept
    for (QMessageBox *old : findChildren<QMessageBox *>(QStringLiteral("formatNotes"), Qt::FindDirectChildrenOnly)) old->close();
    auto *box = new QMessageBox(QMessageBox::Information, title, intro, QMessageBox::Ok, this);
    box->setObjectName(QStringLiteral("formatNotes"));
    box->setInformativeText(QStringLiteral("\u2022 ") + notes.join(QStringLiteral("\n\u2022 ")));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setAttribute(Qt::WA_ShowWithoutActivating);
    box->show();
}

void MainWindow::closeEvent(QCloseEvent *e) {
    for (int i = m_tabs->count() - 1; i >= 0; --i) {
        m_tabs->setCurrentIndex(i);
        auto *v = qobject_cast<DocumentView *>(m_tabs->widget(i));
        if (v && !maybeSave(v->doc())) { e->ignore(); return; }
    }
    if (m_panelStack) {
        QSettings st;
        m_panelStack->saveLayout(st);
    }
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e) {
    // drops that land on the canvas are handled there (import into the document); everything else
    // (empty workspace, tab bar, docks) opens each file as a new document
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) openFile(u.toLocalFile());
}

bool MainWindow::openFile(const QString &path) {
    QString err;
    Document *doc = nullptr;
    if (brand::isNativeDocumentPath(path)) {
        doc = lwd::load(path, &err);   // .lwd and Bonbon .bon share the same payload
        if (doc) doc->filePath = path;
    } else {
        // layered (PSD / ORA / XCF), vector (SVG / SVGZ), editable PNG and every flat format
        QStringList notes;
        doc = docio::open(path, &err, &notes);
        if (doc) {
            QFileInfo fi(path);
            QFile pf(path);
            QByteArray head;
            if (pf.open(QIODevice::ReadOnly)) head = pf.readAll();
            const Fmt f = ImageIO::detect(head) != Fmt::Unknown ? ImageIO::detect(head) : ImageIO::fromExtension(path);
            if (docio::isEditablePng(head)) {
                // an editable PNG re-opens with every layer and object; Save writes it back as an editable PNG
                doc->untitledName = fi.completeBaseName() + brand::docExt();
                doc->exportOptions.format = Fmt::PNG;
                doc->exportOptions.pngEditable = true;
                doc->setProperty("editablePng", path);
            } else {
                doc->untitledName = fi.completeBaseName() + brand::docExt();
                if (f != Fmt::Unknown && f != Fmt::PBM && ImageIO::available(f) && ImageIO::canWrite(f)) doc->exportOptions.format = f;
                doc->exportOptions.animated = doc->s.frames.size() > 1;
            }
            doc->setProperty("importedFrom", path);
            showNotes(QStringLiteral("Open"), QStringLiteral("%1 was opened with these approximations:").arg(fi.fileName()), notes);
        }
    }
    if (!doc) {
        QMessageBox::warning(this, QStringLiteral("Open"), QStringLiteral("Could not open %1:\n%2").arg(path, err));
        return false;
    }
    m_lastDir = QFileInfo(path).absolutePath();
    addRecent(path);
    addDocument(doc);
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(path), 4000);
    return true;
}

void MainWindow::fileNew() {
    NewDocumentDialog dlg(this);   // sizes itself from an image on the clipboard (never pastes it)
    if (dlg.exec() != QDialog::Accepted) return;
    addDocument(Document::createNew(dlg.w(), dlg.h(), dlg.canvasColor(), nullptr, dlg.resolution()));
}

void MainWindow::fileOpen() {
    const QStringList files = filedialogs::getOpenFileNames(this, QStringLiteral("Open"), m_lastDir, ImageIO::importFilter());
    for (const QString &f : files) openFile(f);
}

bool MainWindow::saveDocument(Document *doc, bool saveAs) {
    if (auto it = m_symbolEdits.find(doc); it != m_symbolEdits.end()) {
        pushSymbolEdit(doc);
        if (it->parent) return saveDocument(it->parent, saveAs); // saving a symbol saves its document
        return true;
    }
    QString path = doc->filePath;
    // Libreworks: Save of an opened .bon opens Save As and suggests .lwd (does not overwrite .bon).
        if (saveAs || path.isEmpty() || brand::saveRequiresNativeExt(path)) {
        // Save As always suggests this brand's native extension (Bonbon: .bon even if opened from .lwd).
        const QString suggestion = QDir(m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir).filePath(brand::suggestNativeFileName(doc->title()));
        QString chosen;
        const QString docFilter = brand::docFilter(), pngFilter = QStringLiteral("Editable PNG - layers and objects kept (*.png)");
        path = filedialogs::getSaveFileName(this, QStringLiteral("Save As"), suggestion, docFilter + QStringLiteral(";;") + pngFilter, &chosen);
        if (path.isEmpty()) return false;
        const bool png = chosen == pngFilter || path.endsWith(QLatin1String(".png"), Qt::CaseInsensitive);
        if (png && !path.endsWith(QLatin1String(".png"), Qt::CaseInsensitive)) path += QStringLiteral(".png");
        if (!png && !path.endsWith(brand::docExt(), Qt::CaseInsensitive)) path += brand::docExt();
        doc->setProperty("editablePng", png ? path : QString());
    } else if (!doc->property("editablePng").toString().isEmpty()) {
        path = doc->property("editablePng").toString();
    }
    QString err;
    if (path.endsWith(QLatin1String(".png"), Qt::CaseInsensitive)) {
        ExportOptions o = doc->exportOptions;
        o.format = Fmt::PNG;
        o.pngEditable = true;
        o.animated = false;
        if (!docio::exportDocumentFile(*doc, doc->curFrame, o, path, &err)) {
            QMessageBox::warning(this, QStringLiteral("Save"), QStringLiteral("Could not save %1:\n%2").arg(path, err));
            return false;
        }
        doc->setProperty("editablePng", path);
        m_lastDir = QFileInfo(path).absolutePath();
        addRecent(path);
        doc->markClean();
        updateTitle();
        statusBar()->showMessage(QStringLiteral("Saved %1 (editable PNG)").arg(path), 4000);
        return true;
    }
    if (doc == m_envDoc) syncToolEnvironmentTo(doc);
    if (!lwd::save(*doc, path, &err)) {
        QMessageBox::warning(this, QStringLiteral("Save"), QStringLiteral("Could not save %1:\n%2").arg(path, err));
        return false;
    }
    doc->filePath = path;
    m_lastDir = QFileInfo(path).absolutePath();
    addRecent(path);
    doc->markClean();
    updateTitle();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 4000);
    return true;
}

void MainWindow::fileImport() {
    const QString path = filedialogs::getOpenFileName(this, QStringLiteral("Import"), m_lastDir, ImageIO::importFilter());
    if (path.isEmpty()) return;
    m_lastDir = QFileInfo(path).absolutePath();
    Document *d = currentDoc();
    if (!d || brand::isNativeDocumentPath(path)) { openFile(path); return; }
    QString err;
    {
        QFile pf(path);
        QByteArray head;
        if (pf.open(QIODevice::ReadOnly)) head = pf.readAll();
        Fmt f = ImageIO::detect(head);
        if (f == Fmt::Unknown) f = ImageIO::fromExtension(path);
        if (docio::opensAsDocument(f) || docio::isEditablePng(head)) {
            // layered / vector files import their objects (one per layer for PSD / ORA / XCF) into the current layer
            QStringList notes;
            std::unique_ptr<Document> src(docio::openData(head, f, QFileInfo(path).absolutePath(), &err, &notes));
            if (!src) { QMessageBox::warning(this, QStringLiteral("Import"), err); return; }
            QSet<quint64> sel;
            const Frame &sf = src->s.frames.value(qBound(0, src->curFrame, qMax(0, int(src->s.frames.size()) - 1)));
            const QPointF off((d->s.width - src->s.width) / 2.0, (d->s.height - src->s.height) / 2.0);
            for (int L = 0; L < sf.layers.size(); ++L) {
                if (L < src->s.layers.size() && (src->s.layers[L].web || !src->s.layers[L].visible)) continue;
                for (VObject o : sf.layers[L]) {
                    d->reassignIds(o);   // nested ids from the other document too
                    o.moveBy(off);
                    d->frame().layers[d->drawingLayer()].append(o);
                    sel.insert(o.id);
                }
            }
            d->commit(QStringLiteral("Import"));
            d->setSelection(sel);
            statusBar()->showMessage(QStringLiteral("Imported %1 (%2 object(s))").arg(QFileInfo(path).fileName()).arg(sel.size()), 4000);
            showNotes(QStringLiteral("Import"), QStringLiteral("%1 was imported with these approximations:").arg(QFileInfo(path).fileName()), notes);
            return;
        }
    }
    ImageSeq seq;
    if (!ImageIO::read(path, seq, &err)) { QMessageBox::warning(this, QStringLiteral("Import"), err); return; }
    // place each imported frame into the matching document frame, adding frames as needed
    const int start = d->curFrame;
    QSet<quint64> sel;
    for (int i = 0; i < seq.frames.size(); ++i) {
        while (start + i >= d->s.frames.size()) {
            Frame f;
            f.layers.resize(d->s.layers.size());
            f.name = QStringLiteral("Frame %1").arg(d->s.frames.size() + 1);
            d->s.frames.append(f);
        }
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Bitmap;
        o.name = QFileInfo(path).fileName();
        o.image = seq.frames[i].image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QSizeF sz = o.image.size();
        o.rect = QRectF(QPointF((d->s.width - sz.width()) / 2.0, (d->s.height - sz.height()) / 2.0), sz).toAlignedRect();
        o.fillOn = o.strokeOn = false;
        o.stampInterpolation();
        pxedit::trimToAlpha(o);
        if (seq.frames.size() > 1) d->s.frames[start + i].delayMs = seq.frames[i].delayMs;
        d->s.frames[start + i].layers[d->drawingLayer()].append(o);
        if (i == 0) sel.insert(o.id);
    }
    if (seq.frames.size() > 1 && d->s.loops == 0) d->s.loops = seq.loops;
    d->commit(QStringLiteral("Import"));
    d->setSelection(sel);
    statusBar()->showMessage(QStringLiteral("Imported %1 (%2 frame(s))").arg(QFileInfo(path).fileName()).arg(seq.frames.size()), 4000);
}

bool MainWindow::exportTo(Document *doc, const QString &path, const ExportOptions &o) {
    if (o.format == Fmt::PSD || o.format == Fmt::ORA || o.format == Fmt::XCF || o.format == Fmt::SVG || o.format == Fmt::SVGZ
        || o.format == Fmt::PDF || (o.format == Fmt::PNG && o.pngEditable)) {
        // document-level formats keep layers / vector objects / pages
        QString err;
        QStringList notes;
        if (!docio::exportDocumentFile(*doc, doc->curFrame, o, path, &err, &notes)) {
            QMessageBox::warning(this, QStringLiteral("Export"), QStringLiteral("Could not export %1:\n%2").arg(path, err));
            return false;
        }
        statusBar()->showMessage(QStringLiteral("Exported %1 (%2)").arg(path, describeOptions(o)), 6000);
        showNotes(QStringLiteral("Export"), QStringLiteral("%1 was exported with these approximations:").arg(QFileInfo(path).fileName()), notes);
        return true;
    }
    const bool anim = o.animated && ImageIO::canAnimate(o.format) && doc->s.frames.size() > 1;
    ImageSeq seq = doc->toImageSeq(anim);
    if (!anim) seq.frames[0] = {doc->render(doc->curFrame, true), doc->frame().delayMs};
    QString err;
    ExportOptions oo = o;
    if (oo.dpi <= 0 && doc->s.dpi != 72) oo.dpi = doc->s.dpi;
    if (oo.sourceName.isEmpty()) oo.sourceName = doc->filePath.isEmpty() ? doc->title() : QFileInfo(doc->filePath).fileName();
    if (!ImageIO::write(path, seq, oo, &err)) {
        QMessageBox::warning(this, QStringLiteral("Export"), QStringLiteral("Could not export %1:\n%2").arg(path, err));
        return false;
    }
    statusBar()->showMessage(QStringLiteral("Exported %1 (%2, %3 frame(s)%4)").arg(path, describeOptions(o)).arg(seq.frames.size())
                                 .arg(o.area.isEmpty() ? QString() : QStringLiteral(", area %1 x %2").arg(o.area.width()).arg(o.area.height())), 6000);
    return true;
}

void MainWindow::fileExport(const QRect &area) {
    Document *d = currentDoc();
    if (!d) return;
    ExportOptions o = d->exportOptions;
    o.area = area.normalized() & QRect(0, 0, d->s.width, d->s.height);
    if (!o.area.isEmpty()) appState().lastExportArea = o.area;
    QString base = d->filePath.isEmpty() ? d->title() : QFileInfo(d->filePath).completeBaseName();
    brand::stripNativeExt(base);
    QString suggestion = QDir(m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir).filePath(base + QLatin1Char('.') + ImageIO::extension(o.format));
    QString path = filedialogs::getSaveFileName(this, QStringLiteral("Export"), suggestion, ImageIO::exportFilter(o.format));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QLatin1Char('.') + ImageIO::extension(o.format);
    m_lastDir = QFileInfo(path).absolutePath();
    exportTo(d, path, o);
}

void MainWindow::syncWebFeatureUi()
{
    const bool on = webFeaturesEnabled();
    Document *d = currentDoc();
    const bool has = d != nullptr;
    const bool sel = has && !d->selection.isEmpty();
    for (auto it = m_actions.begin(); it != m_actions.end(); ++it) {
        if (!isWebFeatureAction(it.key()) || !it.value()) continue;
        it.value()->setVisible(on);
        bool en = on;
        if (it.key() == QLatin1String("file.exporthtml") || it.key() == QLatin1String("file.exportimagemap")
            || it.key() == QLatin1String("file.exportslices"))
            en = on && has;
        else if (it.key() == QLatin1String("edit.inserthotspot") || it.key() == QLatin1String("edit.insertslice"))
            en = on && sel;
        it.value()->setEnabled(en);
    }
    if (QAction *web = action(QStringLiteral("view.web"))) {
        web->setVisible(true);
        web->setText(on ? QStringLiteral("Show Slices and &Hotspots")
                        : QStringLiteral("Show &Slices"));
    }
    if (!on && (toolIsHotspot(appState().tool()) || toolIsSlice(appState().tool())))
        appState().setTool(Tool::Pointer);
    if (!on) {
        for (Document *doc : documents()) {
            QSet<quint64> keep;
            for (quint64 id : std::as_const(doc->selection)) {
                const VObject *o = doc->find(id);
                if (o && o->type != ObjType::Hotspot) keep.insert(id);
            }
            if (keep != doc->selection) doc->setSelection(keep);
        }
    }
}

void MainWindow::insertWebObjects(ObjType type) {
    Document *d = currentDoc();
    if (!d) return;
    if (!webFeaturesEnabled()) return;
    QVector<VObject> src;
    // Groups: one hotspot/slice per visible child (Fireworks). Recurse so nested groups flatten.
    std::function<void(const VObject &)> collect = [&](const VObject &o) {
        if (!o.visible || o.isWeb()) return;
        if (o.type == ObjType::Group) {
            for (const VObject &c : o.children) collect(c);
            return;
        }
        src.append(o);
    };
    for (VObject *o : d->selectedObjects()) if (o) collect(*o);
    if (src.isEmpty()) return;
    auto area = [](const QPolygonF &q) {
        double a = 0;
        for (int i = 0; i < q.size(); ++i) {
            const QPointF p = q[i], nxt = q[(i + 1) % q.size()];
            a += p.x() * nxt.y() - nxt.x() * p.y();
        }
        return std::abs(a) / 2;
    };
    QSet<quint64> sel;
    int n = 0;
    auto emitObj = [&](VObject o) {
        o.name = objTypeLabel(type);
        o.id = d->newId();
        sel.insert(o.id);
        d->addObject(o, -1, false);
        ++n;
    };
    for (const VObject &s : src) {
        VObject o;
        o.type = type;
        QRectF b = s.bounds();
        b = b.intersected(QRectF(0, 0, d->s.width, d->s.height));
        if (b.isEmpty()) continue;
        o.rect = b.toAlignedRect();
        const bool polyOk = type == ObjType::Hotspot;
        if (type == ObjType::Hotspot && s.type == ObjType::Ellipse) {
            o.webShape = WebShape::Circle;
            emitObj(o);
            continue;
        }
        if (polyOk && s.type != ObjType::Bitmap) {
            // Insert > Hotspot: keep every fill polygon (compounds / disjoint paths), not
            // only the largest. Insert > Slice always makes a rectangle slice.
            const QPainterPath sp = s.shapePath().simplified();
            const QList<QPolygonF> polys = sp.toFillPolygons();
            // A huge compound (e.g. text outlines) would spawn hundreds of tiny hotspots.
            if (polys.size() > 16) {
                emitObj(o);
                continue;
            }
            int emitted = 0;
            for (QPolygonF poly : polys) {
                poly = paths::simplifyPolyline(poly, 0.75, true);
                if (poly.size() < 3 || area(poly) < 0.5) continue;
                const QRectF br = poly.boundingRect();
                const bool axisRect = !br.isEmpty() && area(poly) >= 0.995 * br.width() * br.height();
                VObject h = o;
                if (!axisRect) {
                    h.webShape = WebShape::Polygon;
                    h.points = poly;
                    h.closed = true;
                    if (h.points.size() > 2 && h.points.first() == h.points.last()) h.points.removeLast();
                    if (!h.points.isEmpty()) h.rect = h.points.boundingRect().toAlignedRect();
                } else {
                    h.webShape = WebShape::Rect;
                    h.rect = br.toAlignedRect();
                }
                QRectF hb = QRectF(h.rect).intersected(QRectF(0, 0, d->s.width, d->s.height));
                if (hb.isEmpty()) continue;
                h.rect = hb.toAlignedRect();
                emitObj(h);
                ++emitted;
            }
            if (emitted) continue;
        }
        emitObj(o);
    }
    if (!n) return;
    appState().webLayerVisible = true;
    appState().notifyOptions();
    d->commit(type == ObjType::Slice ? QStringLiteral("Insert Slice") : QStringLiteral("Insert Hotspot"));
    d->setSelection(sel);
}

void MainWindow::fileExportHtml() {
    Document *d = currentDoc();
    if (!d || !webFeaturesEnabled()) return;
    QString base = d->filePath.isEmpty() ? d->title() : QFileInfo(d->filePath).completeBaseName();
    brand::stripNativeExt(base);
    HtmlExportDialog dlg(QDir(m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir).filePath(base + QStringLiteral(".html")), this);
    if (dlg.exec() != QDialog::Accepted) return;
    QString path = dlg.path();
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".html");
    m_lastDir = QFileInfo(path).absolutePath();
    QString err;
    QStringList written;
    if (!html::exportHtml(*d, path, dlg.options(), &err, &written)) {
        QMessageBox::warning(this, QStringLiteral("Export HTML"), QStringLiteral("Could not export %1:\n%2").arg(path, err));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Exported %1 (%2 file(s))").arg(path).arg(written.size()), 6000);
}

void MainWindow::fileExportFavicons() {
    Document *d = currentDoc();
    if (!d) return;
    FaviconDialog dlg(*d, m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString dir = dlg.folder();
    if (dir.isEmpty()) return;
    docio::FaviconResult r;
    QString err;
    if (!docio::exportFaviconBundle(d->s, d->curFrame, dir, dlg.options(), &r, &err)) {
        QMessageBox::warning(this, QStringLiteral("Export Favicon / App Icons"), QStringLiteral("Could not export to %1:\n%2").arg(dir, err));
        return;
    }
    m_lastDir = dir;
    statusBar()->showMessage(QStringLiteral("Exported %1 icon file(s) to %2").arg(r.files.size()).arg(dir), 6000);
    auto *box = new QMessageBox(QMessageBox::Information, QStringLiteral("Export Favicon / App Icons"),
                                QStringLiteral("Wrote %1 to %2.\n\nAdd this to your page's <head> (also saved as favicon-snippet.html):").arg(r.files.join(QStringLiteral(", ")), dir),
                                QMessageBox::Ok, this);
    box->setObjectName(QStringLiteral("faviconDone"));
    box->setDetailedText(r.snippet);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
}

void MainWindow::fileExportPreview(const QRect &area) {
    Document *d = currentDoc();
    if (!d) return;
    ExportDialog dlg(*d, this, area);
    dlg.setObjectName(QStringLiteral("exportPreviewDialog"));
    if (dlg.exec() != QDialog::Accepted) return;
    ExportOptions o = dlg.options();
    const QRect a = o.area;
    o.area = QRect();          // the area is per export, never part of the document's optimise settings
    d->exportOptions = o;
    m_optimize->setDocument(d);
    fileExport(a);
}


void MainWindow::fileExportSelected() {
    Document *d = currentDoc();
    if (!d || d->selection.isEmpty()) return;
    QVector<VObject> objs;
    QRectF pad;
    for (VObject *o : d->selectedOutermost()) {
        if (!o->visible || o->isWeb()) continue;
        objs << *o;
        pad = pad.isNull() ? o->visualBounds() : pad.united(o->visualBounds());
    }
    if (objs.isEmpty() || pad.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Export Selected needs a visible selection"), 4000);
        return;
    }
    QRect r = pad.toAlignedRect().intersected(QRect(-4096, -4096, d->s.width + 8192, d->s.height + 8192));
    if (r.isEmpty()) return;
    QImage img = render::renderObjects(objs, r);
    const QRect ink = filters::opaqueBounds(img);
    if (ink.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Export Selected: the selection has no visible pixels"), 4000);
        return;
    }
    if (ink != img.rect()) img = img.copy(ink);
    // a temporary document that holds only the selection (like Export Area, but selection-only)
    std::unique_ptr<Document> tmp(Document::createNew(img.width(), img.height(), Qt::transparent));
    VObject b;
    b.id = tmp->newId();
    b.type = ObjType::Bitmap;
    b.name = QStringLiteral("Selection");
    b.image = img;
    b.rect = QRectF(0, 0, img.width(), img.height());
    b.fillOn = b.strokeOn = false;
    b.stampInterpolation();
    tmp->objects(0).append(b);
    tmp->exportOptions = d->exportOptions;
    tmp->exportOptions.area = QRect();
    ExportDialog dlg(*tmp, this);
    dlg.setObjectName(QStringLiteral("exportSelectedDialog"));
    dlg.setWindowTitle(QStringLiteral("Export Selected"));
    if (dlg.exec() != QDialog::Accepted) return;
    ExportOptions o = dlg.options();
    o.area = QRect();
    QString base = d->filePath.isEmpty() ? d->title() : QFileInfo(d->filePath).completeBaseName();
    brand::stripNativeExt(base);
    base += QStringLiteral("-selection");
    QString suggestion = QDir(m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir).filePath(base + QLatin1Char('.') + ImageIO::extension(o.format));
    QString path = filedialogs::getSaveFileName(this, QStringLiteral("Export Selected"), suggestion, ImageIO::exportFilter(o.format));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += QLatin1Char('.') + ImageIO::extension(o.format);
    m_lastDir = QFileInfo(path).absolutePath();
    exportTo(tmp.get(), path, o);
}

void MainWindow::fileExportSelectionArea() {
    Document *d = currentDoc();
    if (!d) return;
    const QRect r = d->pixelSelectionBounds() & QRect(0, 0, d->s.width, d->s.height);
    if (d->pixelSelection.isEmpty() || r.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Export Selection Area needs a marquee selection on the canvas"), 4000);
        return;
    }
    fileExportPreview(r);
}

void MainWindow::exportFrames() {
    Document *d = currentDoc();
    if (!d) return;
    QString dir = filedialogs::getExistingDirectory(this, QStringLiteral("Export Frames To"), m_lastDir);
    if (dir.isEmpty()) return;
    ExportOptions o = d->exportOptions;
    o.animated = false;
    o.chName.clear();   // C Header: one identifier per file name
    if (o.dpi <= 0 && d->s.dpi != 72) o.dpi = d->s.dpi;
    QString base = d->title();
    brand::stripNativeExt(base);
    int ok = 0;
    for (int i = 0; i < d->s.frames.size(); ++i) {
        ImageSeq seq;
        seq.width = d->s.width;
        seq.height = d->s.height;
        seq.frames.append({d->render(i, true), d->s.frames[i].delayMs});
        QString path = QDir(dir).filePath(QStringLiteral("%1_%2.%3").arg(base).arg(i + 1, 2, 10, QLatin1Char('0')).arg(ImageIO::extension(o.format)));
        if (ImageIO::write(path, seq, o)) ++ok;
    }
    statusBar()->showMessage(QStringLiteral("Exported %1 of %2 frames to %3").arg(ok).arg(d->s.frames.size()).arg(dir), 6000);
}

// ---------------------------------------------------------------- edit
static QTransform docToImage(const VObject &o) { return pxedit::docToImage(o); }

// The bitmaps a pixel operation works on: the selected editable bitmaps, or else the visible
// bitmaps of the current layer under the selection.
static QVector<VObject *> pixelTargets(Document *d) {
    QVector<VObject *> t;
    for (VObject *o : d->selectedEditable()) if (o->type == ObjType::Bitmap && !o->rect.isEmpty() && !o->image.isNull() && d->beginPixelEdit(*o)) t << o;
    if (!t.isEmpty() || d->pixelSelection.isEmpty()) return t;
    const QRectF b = d->pixelSelectionBounds();
    const int li = d->drawingLayer();
    if (li < 0 || li >= d->s.layers.size() || d->s.layers[li].locked) return t;
    for (VObject &o : d->objects(li))
        if (o.type == ObjType::Bitmap && o.visible && !o.locked && !o.image.isNull() && o.bounds().intersects(b) && d->beginPixelEdit(o)) t << &o;
    return t;
}

bool MainWindow::copyPixels(Document *d, bool merged, VObject *out, QVector<VObject *> *sources) {
    if (!d || d->pixelSelection.isEmpty()) return false;
    // a feathered selection reaches past its outline by about the feather radius
    const QRect R = d->pixelSelectionBounds();
    if (R.isEmpty()) return false;
    auto masked = [&](const QImage &img, const QTransform &t, QRect *imgRect) -> QImage {
        const QRect ir = t.mapRect(QRectF(R)).toAlignedRect() & img.rect();
        *imgRect = ir;
        if (ir.isEmpty()) return {};
        QImage piece = img.copy(ir).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage mask = d->selectionMask(QTransform::fromTranslate(-ir.x(), -ir.y()) * QTransform(t), ir.size());
        QPainter p(&piece);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(0, 0, mask);
        return piece;
    };
    VObject o;
    o.type = ObjType::Bitmap;
    if (merged) {
        QRect ir;
        o.image = masked(d->render(d->curFrame, false), QTransform(), &ir);
        if (o.image.isNull()) return false;
        o.rect = QRectF(ir);
        o.name = QStringLiteral("Merged Bitmap");
    } else {
        const QVector<VObject *> targets = pixelTargets(d);
        if (sources) *sources = targets;
        QVector<VObject> pieces;
        for (VObject *src : targets) {
            const QTransform t = docToImage(*src);
            QRect ir;
            QImage img = masked(src->image, t, &ir);
            if (img.isNull()) continue;
            VObject p = *src;
            p.image = img;
            p.rect = t.inverted().mapRect(QRectF(ir));
            p.name = QStringLiteral("Bitmap");
            pieces << p;
        }
        if (pieces.isEmpty()) return false;
        if (pieces.size() == 1) {
            o = pieces.first();
        } else {   // several bitmaps: flatten what they show under the selection
            QRectF u;
            for (const VObject &p : pieces) u = u.isNull() ? p.rect : u.united(p.rect);
            const QRect ur = u.toAlignedRect();
            o.image = render::renderObjects(pieces, ur);
            o.rect = QRectF(ur);
            o.name = QStringLiteral("Bitmap");
        }
    }
    o.id = 0;
    o.locked = false;
    o.visible = true;
    o.stampInterpolation();
    *out = o;
    return true;
}

void MainWindow::clearPixels(Document *d, const QVector<VObject *> &targets) {
    for (VObject *o : targets) {
        if (o->image.format() != QImage::Format_ARGB32_Premultiplied) o->image = o->image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QImage mask = d->selectionMask(docToImage(*o), o->image.size());
        if (mask.isNull()) continue;
        QPainter p(&o->image);
        p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        p.drawImage(0, 0, mask);
    }
}

void MainWindow::bitmapVia(bool cut) {
    Document *d = currentDoc();
    if (!d) return;
    VObject piece;
    QVector<VObject *> sources;
    PixelEditGuard pxGuard(d);
    if (!copyPixels(d, false, &piece, &sources)) {
        statusBar()->showMessage(QStringLiteral("Bitmap via Copy/Cut: make a pixel selection on a bitmap first"), 4000);
        return;
    }
    int layer = d->drawingLayer(), index = -1;
    if (!sources.isEmpty()) d->find(sources.last()->id, &layer, &index);
    if (cut) clearPixels(d, sources);
    piece.id = d->newId();
    auto &objs = d->objects(layer);
    objs.insert(index >= 0 ? index + 1 : objs.size(), piece);
    d->clearPixelSelection();
    d->commit(cut ? QStringLiteral("Bitmap via Cut") : QStringLiteral("Bitmap via Copy"));
    d->selectOnly(piece.id);
}

void MainWindow::editCopy(bool cut) {
    Document *d = currentDoc();
    if (!d) return;
    if (!d->pixelSelection.isEmpty()) {
        VObject piece;
        QVector<VObject *> sources;
        PixelEditGuard pxGuard(d);
        if (copyPixels(d, false, &piece, &sources)) {
            g_clipboard = {piece};
            g_clipboardSize = piece.image.size();
            g_clipboardDpi = d->s.dpi;
            auto *mime = new QMimeData;
            mime->setImageData(piece.image);   // other apps get the pixels
            mime->setData(QString::fromLatin1(kObjMime), QByteArray::number(1));
            QApplication::clipboard()->setMimeData(mime);
            if (cut) { clearPixels(d, sources); d->commit(QStringLiteral("Cut Pixels")); }
            return;
        }
    }
    if (d->selection.isEmpty()) return;
    g_clipboard.clear();
    SymbolScope symScope(&d->s.symbols);
    QRectF area;
    // the outermost selected objects (a selected group carries its members and mask; members picked
    // inside an entered group copy on their own)
    for (VObject *o : d->selectedOutermost()) { g_clipboard.append(*o); area = area.isNull() ? o->artworkBounds() : area.united(o->artworkBounds()); }
    auto *mime = new QMimeData;
    g_clipboardSize = artworkPixelSize(area);
    g_clipboardDpi = d->s.dpi;
    // Render into a rect of the tight size, origin floored so a fractional
    // position does not inflate the clipboard image.
    const QRect r(int(std::floor(area.x() + 1e-9)), int(std::floor(area.y() + 1e-9)),
                  g_clipboardSize.width(), g_clipboardSize.height());
    if (!r.isEmpty() && r.width() * qint64(r.height()) < 64ll * 1024 * 1024) mime->setImageData(render::renderObjects(g_clipboard, r));
    mime->setData(QString::fromLatin1(kObjMime), QByteArray::number(g_clipboard.size()));
    QApplication::clipboard()->setMimeData(mime);
    if (cut) { d->deleteSelection(); d->commit(QStringLiteral("Cut")); }
}

void MainWindow::editPaste(bool inPlace) {
    Document *d = currentDoc();
    if (!d) return;
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    QSet<quint64> sel;
    // the part of the document the view shows (to centre pastes that would land off-screen)
    QRectF visible(0, 0, d->s.width, d->s.height);
    QPointF viewCentre = visible.center();
    Canvas *canvas = nullptr;
    if (DocumentView *v = currentView(); v && v->canvas() && v->canvas()->width() > 0) {
        canvas = v->canvas();
        visible = canvas->visibleDocRect();
        viewCentre = canvas->docCenter();
    }
    // Preferences > Paste at mouse location: centre on the pointer when it is over this canvas.
    // Paste Inside / Paste as Mask keep their own placement and ignore this preference.
    QPointF pasteCentre = viewCentre;
    bool atMouse = false;
    if (!inPlace && appState().prefs.pasteAtMouse && canvas) {
        const QPoint local = canvas->mapFromGlobal(QCursor::pos());
        if (canvas->rect().contains(local)) {
            pasteCentre = canvas->toDoc(QPointF(local));
            atMouse = true;
        }
    }
    if (mime && mime->hasFormat(QString::fromLatin1(kObjMime)) && !g_clipboard.isEmpty()) {
        QRectF area;
        for (const VObject &o : g_clipboard) area = area.isNull() ? o.visualBounds() : area.united(o.visualBounds());
        QPointF shift;
        if (!inPlace) {
            if (atMouse) shift = pasteCentre - area.center();
            else if (!area.intersects(visible)) shift = viewCentre - area.center();
        }
        shift = QPointF(qRound(shift.x()), qRound(shift.y()));
        if (d->pixelSelection.isEmpty() == false) { d->pixelSelection = QPainterPath(); emit d->selectionChanged(); }
        // inside an entered group the paste joins its members
        VObject *into = d->enteredGroup ? d->find(d->enteredGroup) : nullptr;
        for (VObject o : g_clipboard) {
            d->reassignIds(o);   // fresh ids for nested members and masks too
            o.locked = false;
            if (!shift.isNull()) o.moveBy(shift);
            if (into) into->children.append(o);
            else d->objects(d->drawingLayer()).append(o);
            sel.insert(o.id);
        }
    } else if (mime && mime->hasImage()) {
        QImage img = qvariant_cast<QImage>(mime->imageData());
        if (img.isNull()) return;
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Bitmap;
        o.name = QStringLiteral("Pasted Bitmap");
        o.image = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const QPointF c = atMouse ? pasteCentre : viewCentre;
        o.rect = QRectF(qRound(c.x() - img.width() / 2.0), qRound(c.y() - img.height() / 2.0), img.width(), img.height());
        o.fillOn = o.strokeOn = false;
        o.stampInterpolation();
        pxedit::trimToAlpha(o);
        d->objects(d->drawingLayer()).append(o);
        sel.insert(o.id);
    } else {
        return;
    }
    d->commit(QStringLiteral("Paste"));
    d->setSelection(sel);
}

void MainWindow::editDuplicate(bool clone) {
    Document *d = currentDoc();
    if (!d || d->selection.isEmpty()) return;
    QSet<quint64> sel;
    // copies go on top of the container they come from (a layer, or the group the object is in)
    QVector<quint64> ids;
    for (VObject *o : d->selectedOutermost()) if (!d->isMaskObject(o->id)) ids << o->id;
    if (ids.isEmpty()) return;
    for (quint64 id : ids) {
        QVector<VObject> *cont = d->containerOf(id);
        VObject *o = d->find(id);
        if (!cont || !o) continue;
        VObject c = *o;
        d->reassignIds(c);
        c.locked = false;
        if (!clone) c.moveBy({10, 10});
        cont->append(c);
        sel.insert(c.id);
    }
    d->commit(clone ? QStringLiteral("Clone") : QStringLiteral("Duplicate"));
    d->setSelection(sel);
}

void MainWindow::selectInverse() {
    Document *d = currentDoc();
    if (!d) return;
    if (!d->pixelSelection.isEmpty()) {
        const QRect canvas(0, 0, d->s.width, d->s.height);
        if (d->hasMaskSelection() || d->selectionEdge != Document::SelHard) {
            // soft selections invert as masks so partial pixels stay partial
            d->setSelectionMask(selmask::invert(d->canvasSelectionMask(canvas)), canvas.topLeft());
        } else {
            QPainterPath all;
            all.addRect(canvas);
            d->pixelSelection = all.subtracted(d->pixelSelection);
            emit d->selectionChanged();
        }
        d->notifyChanged();
        return;
    }
    QSet<quint64> sel;
    for (int li = 0; li < d->s.layers.size(); ++li) {
        if (!d->s.layers[li].visible || d->s.layers[li].locked) continue;
        for (const VObject &o : d->objects(li))
            if (o.visible && !o.locked && (webFeaturesEnabled() || o.type != ObjType::Hotspot)
                && !d->selection.contains(o.id)) sel.insert(o.id);
    }
    d->setSelection(sel);
}

// ---------------------------------------------------------------- bitmap selection commands
static QImage applySelOp(int op, const QImage &m, double v) {
    switch (op) {
    case MainWindow::SelFeather: return selmask::feather(m, v);
    case MainWindow::SelExpand: return selmask::expand(m, v);
    case MainWindow::SelContract: return selmask::contract(m, v);
    case MainWindow::SelBorder: return selmask::border(m, v);
    default: return selmask::smooth(m, v);
    }
}

bool MainWindow::modifySelection(int op, double amount) {
    Document *d = currentDoc();
    if (!d || d->pixelSelection.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Make a bitmap selection first (Marquee, Lasso or Magic Wand)"), 4000);
        return false;
    }
    // work on the canvas area around the selection, with room to grow
    const QRect canvas(0, 0, d->s.width, d->s.height);
    const QRect area = d->pixelSelectionBounds().adjusted(-260, -260, 260, 260) & canvas;
    if (area.isEmpty()) return false;
    const QImage orig = d->canvasSelectionMask(area);
    const QImage origMask = d->selMask;
    const QPoint origOrigin = d->selMaskOrigin;
    const QPainterPath origPath = d->pixelSelection, origOutline = d->selMaskOutline;
    const int origEdge = d->selectionEdge;
    const double origFeather = d->selectionFeather;
    auto preview = [d, area, orig, op](double v) {
        d->setSelectionMask(applySelOp(op, orig, v), area.topLeft());
        d->notifyChanged();
    };
    if (amount >= 0) { preview(amount); return true; }
    static const char *titles[] = {"Feather Selection", "Expand Marquee", "Contract Marquee", "Border Marquee", "Smooth Marquee"};
    static const char *labels[] = {"Feather radius:", "Expand by:", "Contract by:", "Border width:", "Sample radius:"};
    static double last[] = {10, 5, 5, 10, 5};
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("selectionModifyDialog"));
    dlg.setWindowTitle(QString::fromLatin1(titles[op]));
    auto *f = new QFormLayout(&dlg);
    auto *sp = new PopupSliderDoubleField;
    sp->setObjectName(QStringLiteral("selAmount"));
    sp->setRange(op == SelFeather ? 0 : 1, 250);
    sp->setSliderRange(op == SelFeather ? 0 : 1, 100);
    sp->setDecimals(op == SelFeather ? 1 : 0);
    sp->setSuffix(QStringLiteral(" px"));
    sp->setKeyboardTracking(false);
    sp->setValue(last[op]);
    f->addRow(QString::fromLatin1(labels[op]), sp);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(sp, &PopupSliderDoubleField::valueChanged, &dlg, [preview](double v) { preview(v); });
    preview(sp->value());   // live: the marching ants follow the setting
    if (dlg.exec() == QDialog::Accepted) {
        last[op] = sp->value();
        preview(sp->value());
        return true;
    }
    // cancel: back to the selection as it was
    d->selMask = origMask;
    d->selMaskOrigin = origOrigin;
    d->selMaskOutline = origOutline;
    d->pixelSelection = origPath;
    d->selectionEdge = origEdge;
    d->selectionFeather = origFeather;
    emit d->selectionChanged();
    d->notifyChanged();
    return false;
}

// Select Similar: add every pixel of the bitmap whose colour is close (Magic Wand tolerance) to a
// colour already selected. Samples the active bitmap's own pixels (Fireworks); with
// wandSampleAllLayers, samples the flattened canvas instead.
bool MainWindow::selectSimilar() {
    Document *d = currentDoc();
    if (!d || d->pixelSelection.isEmpty()) return false;
    const AppState &as = appState();
    if (as.wandSampleAllLayers) {
        const QRect area(0, 0, d->s.width, d->s.height);
        const QImage img = d->render(d->curFrame, false);
        const QImage m = d->canvasSelectionMask(area);
        d->setSelectionMask(selmask::similar(img, m, as.tolerance), area.topLeft());
        d->notifyChanged();
        return true;
    }
    VObject *src = nullptr;
    for (VObject *o : d->selectedEditable())
        if (o->type == ObjType::Bitmap && !o->image.isNull()) { xf::bakeBitmap(*o); src = o; break; }
    if (!src) {
        statusBar()->showMessage(QStringLiteral("Select Similar: select a bitmap object"), 4000);
        return false;
    }
    const QTransform toImg = pxedit::docToImage(*src);
    const QImage m = d->selectionMask(toImg, src->image.size());
    const QImage r = selmask::similar(src->image, m, as.tolerance);
    // place the image-space mask back onto the canvas via imageToDoc
    QImage canvas = selmask::blank(QSize(d->s.width, d->s.height));
    {
        QPainter p(&canvas);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.setTransform(pxedit::imageToDoc(*src));
        p.drawImage(0, 0, r);
    }
    d->setSelectionMask(canvas, QPoint(0, 0));
    d->notifyChanged();
    return true;
}

bool MainWindow::saveSelection(const QString &nameIn) {
    Document *d = currentDoc();
    if (!d || d->pixelSelection.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Save Bitmap Selection: there is no bitmap selection"), 4000);
        return false;
    }
    QString name = nameIn;
    if (name.isEmpty()) {
        bool ok = false;
        name = QInputDialog::getText(this, QStringLiteral("Save Bitmap Selection"), QStringLiteral("Name:"), QLineEdit::Normal,
                                     QStringLiteral("Selection %1").arg(d->savedSelections.size() + 1), &ok);
        if (!ok || name.trimmed().isEmpty()) return false;
    }
    const QRect b = d->pixelSelectionBounds();
    // same name replaces, as in Fireworks
    for (auto &ss : d->savedSelections)
        if (ss.name == name.trimmed()) { ss.mask = d->canvasSelectionMask(b); ss.origin = b.topLeft(); d->markViewDirty(); return true; }
    d->savedSelections.append({name.trimmed(), d->canvasSelectionMask(b), b.topLeft()});
    d->markViewDirty();
    return true;
}

bool MainWindow::restoreSelection(int index, Qt::KeyboardModifiers mods) {
    Document *d = currentDoc();
    if (!d || index < 0 || index >= d->savedSelections.size()) return false;
    const auto ss = d->savedSelections[index];
    const bool add = mods & Qt::ShiftModifier, sub = mods & Qt::AltModifier;
    if ((add || sub) && !d->pixelSelection.isEmpty()) {
        const QRect area = d->pixelSelectionBounds().united(QRect(ss.origin, ss.mask.size()));
        const QImage a = d->canvasSelectionMask(area);
        QImage b = selmask::blank(area.size());
        { QPainter p(&b); p.setCompositionMode(QPainter::CompositionMode_Source); p.drawImage(ss.origin - area.topLeft(), ss.mask); }
        d->setSelectionMask(add && sub ? selmask::intersect(a, b) : add ? selmask::unite(a, b) : selmask::subtract(a, b), area.topLeft());
    } else {
        d->setSelectionMask(ss.mask, ss.origin);
    }
    d->notifyChanged();
    return true;
}

void MainWindow::manageSelections() {
    Document *d = currentDoc();
    if (!d) return;
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("savedSelectionsDialog"));
    dlg.setWindowTitle(QStringLiteral("Restore Bitmap Selection"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *list = new QListWidget;
    list->setObjectName(QStringLiteral("savedSelections"));
    auto fill = [list, d] {
        const int row = list->currentRow();
        list->clear();
        for (const auto &ss : d->savedSelections) {
            // a small thumbnail of the mask
            QImage t = selmask::toGrey(ss.mask).scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            list->addItem(new QListWidgetItem(QIcon(QPixmap::fromImage(t)), ss.name));
        }
        list->setCurrentRow(qBound(-1, row < 0 ? 0 : row, list->count() - 1));
    };
    fill();
    lay->addWidget(list);
    auto *op = new QComboBox;
    op->setObjectName(QStringLiteral("restoreOp"));
    op->addItems({QStringLiteral("Replace selection"), QStringLiteral("Add to selection"), QStringLiteral("Subtract from selection"),
                  QStringLiteral("Intersect with selection")});
    lay->addWidget(op);
    auto *row = new QHBoxLayout;
    auto *restore = new QPushButton(QStringLiteral("&Restore"));
    restore->setObjectName(QStringLiteral("selRestore"));
    auto *rename = new QPushButton(QStringLiteral("Re&name..."));
    rename->setObjectName(QStringLiteral("selRename"));
    auto *del = new QPushButton(QStringLiteral("&Delete"));
    del->setObjectName(QStringLiteral("selDelete"));
    auto *close = new QPushButton(QStringLiteral("Close"));
    row->addWidget(restore); row->addWidget(rename); row->addWidget(del); row->addStretch(); row->addWidget(close);
    lay->addLayout(row);
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(restore, &QPushButton::clicked, &dlg, [this, list, op, &dlg] {
        static const Qt::KeyboardModifiers m[] = {Qt::NoModifier, Qt::ShiftModifier, Qt::AltModifier, Qt::ShiftModifier | Qt::AltModifier};
        if (restoreSelection(list->currentRow(), m[op->currentIndex()])) dlg.accept();
    });
    connect(list, &QListWidget::itemDoubleClicked, restore, &QPushButton::click);
    connect(rename, &QPushButton::clicked, &dlg, [&dlg, list, d, fill] {
        const int i = list->currentRow();
        if (i < 0 || i >= d->savedSelections.size()) return;
        bool ok = false;
        const QString n = QInputDialog::getText(&dlg, QStringLiteral("Rename Selection"), QStringLiteral("Name:"), QLineEdit::Normal, d->savedSelections[i].name, &ok);
        if (!ok || n.trimmed().isEmpty()) return;
        d->savedSelections[i].name = n.trimmed();
        d->markViewDirty();
        fill();
    });
    connect(del, &QPushButton::clicked, &dlg, [list, d, fill] {
        const int i = list->currentRow();
        if (i < 0 || i >= d->savedSelections.size()) return;
        d->savedSelections.remove(i);
        d->markViewDirty();
        fill();
    });
    auto sync = [=] { const bool any = list->currentRow() >= 0; restore->setEnabled(any); rename->setEnabled(any); del->setEnabled(any); };
    connect(list, &QListWidget::currentRowChanged, &dlg, sync);
    sync();
    dlg.exec();
}

int MainWindow::convertMarqueeToPath() {
    Document *d = currentDoc();
    if (!d || d->pixelSelection.isEmpty()) return 0;
    VObject o;
    o.type = ObjType::Path;
    o.name = QStringLiteral("Path");
    paths::fromPainterPath(o, d->pixelSelection.simplified());
    if (o.points.isEmpty()) return 0;
    o.closed = true;
    o.strokeOn = true;
    o.stroke = appState().stroke;
    o.strokeWidth = 1;
    o.fillOn = false;
    o.id = d->newId();
    d->objects(d->drawingLayer()).append(o);
    d->clearPixelSelection();
    d->commit(QStringLiteral("Convert Marquee to Path"));
    d->selectOnly(o.id);
    return 1;
}

static VObject *quickMaskObject(Document *d) {
    if (!d) return nullptr;
    for (int li = 0; li < d->s.layers.size(); ++li)
        for (VObject &o : d->objects(li)) if (o.quickMask) return &o;
    return nullptr;
}

bool MainWindow::quickMaskActive(Document *d) const {
    if (!d) d = const_cast<MainWindow *>(this)->currentDoc();
    return quickMaskObject(d) != nullptr;
}

// Quick Mask: the selection becomes a greyscale bitmap (white = selected) shown as a red overlay;
// brushes, the eraser, gradients and filters edit it; toggling back turns it into the selection.
void MainWindow::toggleQuickMask() {
    Document *d = currentDoc();
    if (!d) return;
    if (VObject *q = quickMaskObject(d)) {
        const QImage m = selmask::fromGrey(q->image);
        const QPoint at = q->rect.topLeft().toPoint();
        QImage mask = m;
        if (q->rect.size().toSize() != m.size()) mask = selmask::fromGrey(q->image.scaled(q->rect.size().toSize()));
        quint64 id = q->id;
        int index = -1;
        if (QVector<VObject> *c = d->containerOf(id, nullptr, &index)) c->removeAt(index);
        d->commit(QStringLiteral("Exit Quick Mask"));
        d->setSelectionMask(mask, at);
        d->clearSelection();
    } else {
        const QRect canvas(0, 0, d->s.width, d->s.height);
        VObject o;
        o.type = ObjType::Bitmap;
        o.quickMask = true;
        o.name = QStringLiteral("Quick Mask");
        o.rect = QRectF(canvas);
        o.image = selmask::toGrey(d->pixelSelection.isEmpty() ? selmask::blank(canvas.size()) : d->canvasSelectionMask(canvas));
        o.id = d->newId();
        o.stampInterpolation();
        d->clearPixelSelection();
        d->objects(d->drawingLayer()).append(o);
        d->commit(QStringLiteral("Quick Mask"));
        d->selectOnly(o.id);
        if (!toolIsBitmap(appState().tool())) appState().setTool(Tool::Brush);
        statusBar()->showMessage(QStringLiteral("Quick Mask: paint white to select, black to deselect; toggle Quick Mask (Q) to return"), 6000);
    }
    if (QAction *a = action(QStringLiteral("select.quickmask"))) a->setChecked(quickMaskActive(d));
    d->notifyChanged();
}

// ---------------------------------------------------------------- modify
void MainWindow::canvasSize() {
    Document *d = currentDoc();
    if (!d) return;
    SizeDialog dlg(true, d->s.width, d->s.height, this);
    if (dlg.exec() != QDialog::Accepted) return;
    d->resizeCanvas(dlg.w(), dlg.h(), dlg.offsetFor(d->s.width, d->s.height));
    d->commit(QStringLiteral("Canvas Size"));
}

void MainWindow::imageSize() {
    Document *d = currentDoc();
    if (!d) return;
    SizeDialog dlg(false, d->s.width, d->s.height, this);
    if (dlg.exec() != QDialog::Accepted) return;
    d->scaleImage(dlg.w(), dlg.h(), dlg.resample());
    d->commit(QStringLiteral("Image Size"));
}

void MainWindow::fitCanvas() {
    Document *d = currentDoc();
    if (!d) return;
    // exactly the content's pixels (the padded visual bounds left a 1 px margin)
    QRect r = d->contentPixelBounds();
    if (r.isEmpty() || r == QRect(0, 0, d->s.width, d->s.height)) return;
    d->resizeCanvas(r.width(), r.height(), -r.topLeft());
    d->commit(QStringLiteral("Fit Canvas"));
}

void MainWindow::fitCanvasToWindow() {
    DocumentView *v = currentView();
    if (!v || !v->canvas() || !v->doc()) return;
    Canvas *c = v->canvas();
    Document *d = v->doc();
    const double z = qMax(1e-6, c->zoom());
    const int nw = qBound(1, int(std::lround(double(c->width()) / z)), 16000);
    const int nh = qBound(1, int(std::lround(double(c->height()) / z)), 16000);
    const QPointF tl = c->toDoc(QPointF(0, 0));
    const QPoint offset(-qRound(tl.x()), -qRound(tl.y()));
    if (nw == d->s.width && nh == d->s.height && offset.isNull()) {
        c->pinToOrigin();
        return;
    }
    d->resizeCanvas(nw, nh, offset);
    d->commit(QStringLiteral("Fit Canvas to Window"));
    c->pinToOrigin();
}

void MainWindow::trimCanvas() {
    Document *d = currentDoc();
    if (!d) return;
    QRect r;
    for (int i = 0; i < d->s.frames.size(); ++i) r = r.united(filters::opaqueBounds(d->render(i, false)));
    if (r.isEmpty() || r == QRect(0, 0, d->s.width, d->s.height)) return;
    d->resizeCanvas(r.width(), r.height(), -r.topLeft());
    d->commit(QStringLiteral("Trim Canvas"));
}

void MainWindow::transform(int how) {
    Document *d = currentDoc();
    if (!d) return;
    if (how >= 10) {
        // whole canvas, all frames
        const double W = d->s.width, H = d->s.height;
        QTransform t;
        if (how == 10) t = QTransform(-1, 0, 0, -1, W, H);
        else if (how == 11) t = QTransform(0, 1, -1, 0, H, 0);   // (x,y) -> (H - y, x)
        else t = QTransform(0, -1, 1, 0, 0, W);                    // (x,y) -> (y, W - x)
        for (auto &f : d->s.frames) for (auto &l : f.layers) for (auto &o : l) transformObject(o, t);
        if (how != 10) std::swap(d->s.width, d->s.height);
        d->commit(how == 10 ? QStringLiteral("Rotate Canvas 180°") : how == 11 ? QStringLiteral("Rotate Canvas 90° CW") : QStringLiteral("Rotate Canvas 90° CCW"));
        return;
    }
    auto objs = d->selectedEditable();
    if (objs.isEmpty()) return;
    QPointF c = d->selectionBounds().center();
    QTransform op;
    switch (how) {
    case 0: op = QTransform(-1, 0, 0, -1, 0, 0); break;
    case 1: op = QTransform(0, 1, -1, 0, 0, 0); break;
    case 2: op = QTransform(0, -1, 1, 0, 0, 0); break;
    case 3: op = QTransform(-1, 0, 0, 1, 0, 0); break;
    case 4: op = QTransform(1, 0, 0, -1, 0, 0); break;
    }
    QTransform t = QTransform::fromTranslate(-c.x(), -c.y()) * op * QTransform::fromTranslate(c.x(), c.y());
    for (VObject *o : objs) transformObject(*o, t);
    static const char *labels[] = {"Rotate 180°", "Rotate 90° CW", "Rotate 90° CCW", "Flip Horizontal", "Flip Vertical"};
    d->commit(QString::fromUtf8(labels[how]));
}

bool MainWindow::applyNumericTransform(int mode, double a, double b, bool scaleAttrs) {
    Document *d = currentDoc();
    if (!d) return false;
    auto objs = d->selectedEditable();
    if (objs.isEmpty()) return false;
    const QRectF sb = d->selectionBounds();
    const QPointF c = sb.center();
    QTransform op;
    QString label;
    double attrScale = 1;
    switch (mode) {
    case 0: case 1: {
        double sx = a / 100.0, sy = b / 100.0;
        if (mode == 1) { sx = sb.width() > 0 ? a / sb.width() : 1; sy = sb.height() > 0 ? b / sb.height() : 1; }
        if (sx <= 0 || sy <= 0) return false;
        op = QTransform::fromScale(sx, sy);
        attrScale = std::sqrt(sx * sy);
        label = mode == 0 ? QStringLiteral("Scale") : QStringLiteral("Resize");
        break;
    }
    case 2:
        op = xf::rotationAbout(QPointF(), a);
        label = QStringLiteral("Rotate");
        break;
    case 3: {
        if (qAbs(a) >= 89.9 || qAbs(b) >= 89.9) return false;
        op = QTransform(1, std::tan(qDegreesToRadians(b)), std::tan(qDegreesToRadians(a)), 1, 0, 0);
        label = QStringLiteral("Skew");
        break;
    }
    default: return false;
    }
    if (op.isIdentity()) return false;
    const QTransform t = QTransform::fromTranslate(-c.x(), -c.y()) * op * QTransform::fromTranslate(c.x(), c.y());
    for (VObject *o : objs) {
        transformObject(*o, t);
        if (scaleAttrs && attrScale != 1 && o->isVector() && o->type != ObjType::Text) {
            o->strokeWidth = qBound(0.0, o->strokeWidth * attrScale, 1000.0);
            for (auto &fx : o->effects) {
                fx.distance *= attrScale;
                fx.softness *= attrScale;
                fx.width *= attrScale;
            }
        }
    }
    d->commit(label);
    return true;
}

void MainWindow::numericTransform() {
    Document *d = currentDoc();
    if (!d || d->selectedEditable().isEmpty()) return;
    const QRectF sb = d->selectionBounds();
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("numericTransformDialog"));
    dlg.setWindowTitle(QStringLiteral("Numeric Transform"));
    auto *form = new QFormLayout(&dlg);
    auto *mode = new QComboBox(&dlg);
    mode->setObjectName(QStringLiteral("ntMode"));
    mode->addItems({QStringLiteral("Scale"), QStringLiteral("Resize"), QStringLiteral("Rotate"), QStringLiteral("Skew")});
    auto spin = [&](const QString &name, double lo, double hi, const QString &suffix) {
        auto *sp = new PopupSliderDoubleField(&dlg);
        sp->setObjectName(name);
        sp->setRange(lo, hi);
        sp->setDecimals(2);
        sp->setSuffix(suffix);
        return sp;
    };
    auto *x = spin(QStringLiteral("ntX"), -100000, 100000, QString());
    auto *y = spin(QStringLiteral("ntY"), -100000, 100000, QString());
    auto *ang = spin(QStringLiteral("ntAngle"), -360, 360, QStringLiteral("°"));
    auto *constrain = new QCheckBox(QStringLiteral("Constrain proportions"), &dlg);
    constrain->setObjectName(QStringLiteral("ntConstrain"));
    constrain->setChecked(true);
    auto *attrs = new QCheckBox(QStringLiteral("Scale attributes"), &dlg);
    attrs->setObjectName(QStringLiteral("ntScaleAttrs"));
    attrs->setChecked(true);
    auto *xl = new QLabel(&dlg), *yl = new QLabel(&dlg);
    form->addRow(QStringLiteral("Transform:"), mode);
    form->addRow(xl, x);
    form->addRow(yl, y);
    form->addRow(QStringLiteral("Angle:"), ang);
    form->addRow(constrain);
    form->addRow(attrs);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    bool syncing = false;
    auto setup = [&](int m) {
        syncing = true;
        const bool twoFields = m != 2;
        x->setEnabled(twoFields); y->setEnabled(twoFields);
        ang->setEnabled(m == 2);
        constrain->setEnabled(m <= 1);
        attrs->setEnabled(m <= 1);
        if (m == 0) { xl->setText(QStringLiteral("Width (%):")); yl->setText(QStringLiteral("Height (%):")); x->setValue(100); y->setValue(100); }
        else if (m == 1) { xl->setText(QStringLiteral("Width (px):")); yl->setText(QStringLiteral("Height (px):")); x->setValue(sb.width()); y->setValue(sb.height()); }
        else if (m == 2) { xl->setText(QStringLiteral("Width:")); yl->setText(QStringLiteral("Height:")); }
        else { xl->setText(QStringLiteral("Horizontal (°):")); yl->setText(QStringLiteral("Vertical (°):")); x->setValue(0); y->setValue(0); }
        syncing = false;
    };
    connect(mode, &QComboBox::currentIndexChanged, &dlg, setup);
    auto link = [&](QDoubleSpinBox *from, QDoubleSpinBox *to, bool fromIsX) {
        connect(from, &QDoubleSpinBox::valueChanged, &dlg, [&, from, to, fromIsX](double v) {
            if (syncing || !constrain->isChecked()) return;
            const int m = mode->currentIndex();
            if (m > 1) return;
            syncing = true;
            if (m == 0) to->setValue(v);
            else if (fromIsX && sb.width() > 0) to->setValue(v * sb.height() / sb.width());
            else if (!fromIsX && sb.height() > 0) to->setValue(v * sb.width() / sb.height());
            syncing = false;
            (void)from;
        });
    };
    link(x, y, true);
    link(y, x, false);
    setup(0);
    if (dlg.exec() != QDialog::Accepted) return;
    const int m = mode->currentIndex();
    if (m == 2) applyNumericTransform(2, ang->value());
    else applyNumericTransform(m, x->value(), y->value(), attrs->isChecked());
}

bool MainWindow::removeTransformations() {
    Document *d = currentDoc();
    if (!d) return false;
    bool any = false;
    for (VObject *o : d->selectedEditable()) any |= xf::removeTransform(*o);
    if (any) d->commit(QStringLiteral("Remove Transformations"));
    else statusBar()->showMessage(QStringLiteral("No transformations to remove"), 3000);
    return any;
}

bool MainWindow::rotateCanvas(double degrees) {
    Document *d = currentDoc();
    if (!d || qAbs(std::fmod(degrees, 360.0)) < 1e-9) return false;
    const QRectF canvas(0, 0, d->s.width, d->s.height);
    const QTransform r = xf::rotationAbout(canvas.center(), degrees);
    const QRectF nb = r.mapRect(canvas);
    const int W = qMax(1, qRound(nb.width())), H = qMax(1, qRound(nb.height()));
    // centre the rotated canvas in the new one
    const QTransform t = r * QTransform::fromTranslate(W / 2.0 - canvas.center().x(), H / 2.0 - canvas.center().y());
    for (auto &f : d->s.frames) for (auto &l : f.layers) for (auto &o : l) transformObject(o, t);
    d->s.width = W;
    d->s.height = H;
    d->commit(QStringLiteral("Rotate Canvas %1°").arg(degrees));
    return true;
}

// The clipboard's objects (or a bitmap of another application's image) as fresh copies.
QVector<VObject> MainWindow::clipboardObjects(Document *d) {
    QVector<VObject> out;
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (mime && mime->hasFormat(QString::fromLatin1(kObjMime)) && !g_clipboard.isEmpty()) {
        for (VObject o : g_clipboard) { d->reassignIds(o); o.locked = false; out << o; }
    } else if (mime && mime->hasImage()) {
        const QImage img = qvariant_cast<QImage>(mime->imageData());
        if (img.isNull()) return out;
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Bitmap;
        o.name = QStringLiteral("Pasted Bitmap");
        o.image = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QRectF area;
        for (VObject *s : d->selectedOutermost()) area = area.isNull() ? s->bounds() : area.united(s->bounds());
        const QPointF c = area.isNull() ? QPointF(d->s.width / 2.0, d->s.height / 2.0) : area.center();
        o.rect = QRectF(qRound(c.x() - img.width() / 2.0), qRound(c.y() - img.height() / 2.0), img.width(), img.height());
        o.stampInterpolation();
        out << o;
    }
    return out;
}

bool MainWindow::groupCommand(GroupCmd cmd) {
    Document *d = currentDoc();
    if (!d) return false;
    QString label;
    QSet<quint64> sel;
    switch (cmd) {
    case GroupCmd::Group:
        if (quint64 g = d->groupSelection()) { label = QStringLiteral("Group"); sel = {g}; }
        break;
    case GroupCmd::Ungroup: {
        const QVector<quint64> ids = d->ungroupSelection();
        if (!ids.isEmpty()) { label = QStringLiteral("Ungroup"); sel = QSet<quint64>(ids.begin(), ids.end()); }
        break;
    }
    // Paste Inside / Paste as Mask deliberately preserve placement, ignoring paste-at-mouse.
    case GroupCmd::PasteAsMask: {
        QVector<VObject> objs = clipboardObjects(d);
        if (objs.isEmpty() || d->selection.isEmpty()) break;
        VObject m = objs.first();
        if (objs.size() > 1) {   // several copied objects mask as one group
            m = VObject();
            m.type = ObjType::Group;
            m.id = d->newId();
            m.name = QStringLiteral("Mask");
            m.children = objs;
        }
        if (quint64 g = d->pasteAsMask(m)) { label = QStringLiteral("Paste as Mask"); sel = {g}; }
        break;
    }
    case GroupCmd::PasteInside: {
        const QVector<VObject> objs = clipboardObjects(d);
        if (objs.isEmpty()) break;
        if (quint64 g = d->pasteInside(objs)) { label = QStringLiteral("Paste Inside"); sel = {g}; }
        break;
    }
    case GroupCmd::GroupAsMask:
        if (quint64 g = d->groupAsMask()) { label = QStringLiteral("Group as Mask"); sel = {g}; }
        break;
    case GroupCmd::RevealAll:
    case GroupCmd::HideAll:
        if (quint64 g = d->revealHideAll(cmd == GroupCmd::RevealAll)) {
            label = cmd == GroupCmd::RevealAll ? QStringLiteral("Reveal All") : QStringLiteral("Hide All");
            sel = {g};
        }
        break;
    case GroupCmd::DisableMask:
    case GroupCmd::EnableMask:
        if (d->setMaskEnabled(cmd == GroupCmd::EnableMask)) label = cmd == GroupCmd::EnableMask ? QStringLiteral("Enable Mask") : QStringLiteral("Disable Mask");
        break;
    case GroupCmd::ToggleLink: {
        VObject *g = d->find(d->maskOwnerOfSelection());
        if (g && g->hasMask() && d->setMaskLinked(!g->maskLinked)) label = g->maskLinked ? QStringLiteral("Link Mask") : QStringLiteral("Unlink Mask");
        break;
    }
    case GroupCmd::DeleteMask:
        if (d->deleteMask()) label = QStringLiteral("Delete Mask");
        break;
    case GroupCmd::ConvertToBitmap: {
        const QVector<quint64> ids = d->convertToBitmap();
        if (!ids.isEmpty()) { label = QStringLiteral("Convert to Bitmap"); sel = QSet<quint64>(ids.begin(), ids.end()); }
        break;
    }
    }
    if (label.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Nothing to do: check the selection (and the clipboard for paste commands)"), 3000);
        return false;
    }
    d->commit(label);
    if (!sel.isEmpty()) d->setSelection(sel);
    else d->notifyChanged();
    updateActions();
    return true;
}

void MainWindow::convertToBitmap() {
    Document *d = currentDoc();
    if (!d || d->selection.isEmpty()) return;
    QVector<VObject> objs;
    int layer = -1;
    SymbolScope symScope(&d->s.symbols);
    QRectF area;
    int topIdx = -1, below = 0;   // stack slot of the topmost flattened object (and how many flattened sit under it)
    for (int li = 0; li < d->s.layers.size(); ++li)
        for (int oi = 0; oi < d->objects(li).size(); ++oi)
            if (const VObject &o = d->objects(li)[oi]; d->selection.contains(o.id) && o.visible && !o.locked) {
                objs.append(o);
                if (layer != li) below = 0; else ++below;
                layer = li;
                topIdx = oi;
                area = area.isNull() ? o.visualBounds() : area.united(o.visualBounds());
            }
    QRect r = area.toAlignedRect().intersected(QRect(-4096, -4096, d->s.width + 8192, d->s.height + 8192));
    if (objs.isEmpty() || r.isEmpty()) return;
    VObject b;
    b.id = d->newId();
    b.type = ObjType::Bitmap;
    b.name = QStringLiteral("Bitmap");
    b.stampInterpolation();
    b.image = render::renderObjects(objs, r);
    // tight bounds: drop the transparent safety margin visualBounds() adds, so the bitmap (and its
    // selection box) hugs the art instead of carrying an empty ring that showed as a border
    const QRect ink = filters::opaqueBounds(b.image);
    if (ink.isEmpty()) return;
    if (ink != b.image.rect()) { b.image = b.image.copy(ink); r = ink.translated(r.topLeft()); }
    b.rect = r;
    // remove the selection (deleteSelection also drops selected hidden objects, as before), then put the
    // bitmap where the topmost flattened object was so objects above it stay above
    int slot = topIdx - below;
    for (int oi = 0; oi < topIdx && oi < d->objects(layer).size(); ++oi) {
        const VObject &o = d->objects(layer)[oi];
        if (d->selection.contains(o.id) && !o.locked && !o.visible) --slot;   // deleted, not flattened
    }
    d->deleteSelection();
    QVector<VObject> &dst = d->objects(layer);
    dst.insert(qBound(0, slot, int(dst.size())), b);
    d->commit(QStringLiteral("Flatten Selection"));
    d->selectOnly(b.id);
}

void MainWindow::mergeDown() {
    Document *d = currentDoc();
    if (!d || d->selection.isEmpty()) return;
    // include the object right below the lowest selected object when it is a bitmap
    int li = -1, idx = -1;
    for (quint64 id : d->selection) {
        int l, i;
        VObject *parent = nullptr;
        if (d->containerOf(id, &l, &i, &parent) && !parent && (li < 0 || l < li || (l == li && i < idx))) { li = l; idx = i; }
    }
    if (li >= 0 && idx > 0) {
        const VObject &below = d->objects(li)[idx - 1];
        if (below.type == ObjType::Bitmap && !below.locked) {
            xf::bakeBitmap(const_cast<VObject &>(below));
            QSet<quint64> sel = d->selection;
            sel.insert(below.id);
            d->selection = sel;
        }
    }
    convertToBitmap();
}

void MainWindow::flattenLayers() {
    Document *d = currentDoc();
    if (!d || d->s.layers.size() < 2) return;
    for (auto &f : d->s.frames) {
        QVector<VObject> all;
        for (int li = 0; li < f.layers.size(); ++li)
            if (d->s.layers[li].visible) all += f.layers[li];
        f.layers = {all};
    }
    LayerInfo keep = d->s.layers.first();
    keep.visible = true;
    keep.locked = false;
    d->s.layers = {keep};
    d->curLayer = 0;
    d->commit(QStringLiteral("Flatten Layers"));
}

// Bitmaps targeted by a filter: the selected editable bitmaps.
QVector<VObject *> MainWindow::filterTargets(Document *d) {
    QVector<VObject *> targets;
    QString blocked;
    for (VObject *o : d->selectedEditable())
        if (o->type == ObjType::Bitmap) {
            if (d->beginPixelEdit(*o)) targets << o;
            else blocked = pxedit::blockedMessage(*o, QStringLiteral("Filters"));
        }
    if (targets.isEmpty() && !blocked.isEmpty()) { statusBar()->showMessage(blocked, 8000); return targets; }
    if (!blocked.isEmpty()) statusBar()->showMessage(blocked, 8000);
    if (targets.isEmpty() && !QCoreApplication::instance()->property("lw_testing").toBool())
        QMessageBox::information(this, QStringLiteral("Filters"), QStringLiteral("Select a bitmap object first. Vector objects can use Live Effects (Property Inspector > Effects)."));
    return targets;
}

// Applies an effect destructively to the targets, honouring the pixel selection; returns the processed image.
// The selection's bounding box in the pixels of bitmap `o` (empty: no selection / no overlap).
static QPainterPath selectionInImage(const Document *d, const VObject *o, const QImage &img) {
    if (d->pixelSelection.isEmpty() || o->rect.isEmpty()) return {};
    const QTransform t = QTransform::fromTranslate(-o->rect.x(), -o->rect.y()) *
                         QTransform::fromScale(img.width() / o->rect.width(), img.height() / o->rect.height());
    return t.map(d->pixelSelection);
}

// Run `fn` over the bitmap, limited to the pixel selection when there is one. Offset-like
// operations (`withinBounds`) work on the selection's bounding box, as in Fireworks/Photoshop,
// so "Half" and wrapping refer to the selected area rather than the whole bitmap.
static QImage filteredWith(const Document *d, const VObject *o, const QImage &orig,
                           const std::function<QImage(const QImage &)> &fn, bool withinBounds) {
    const QPainterPath sel = selectionInImage(d, o, orig);
    if (sel.isEmpty()) return fn(orig);
    QImage merged = orig.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage out;
    QPoint at;
    if (withinBounds) {
        const QRect r = sel.boundingRect().toAlignedRect() & orig.rect();
        if (r.isEmpty()) return orig;
        out = fn(merged.copy(r));
        at = r.topLeft();
    } else {
        out = fn(orig);
    }
    // blend through the selection mask so anti-aliased and feathered edges fade the effect in
    QImage layer = merged.copy();
    {
        QPainter q(&layer);
        q.setCompositionMode(QPainter::CompositionMode_Source);
        q.drawImage(at, out);
    }
    const QImage mask = d->selectionMask(QTransform::fromTranslate(-o->rect.x(), -o->rect.y()) *
                                             QTransform::fromScale(orig.width() / o->rect.width(), orig.height() / o->rect.height()),
                                         orig.size());
    for (int y = 0; y < merged.height(); ++y) {
        QRgb *m = reinterpret_cast<QRgb *>(merged.scanLine(y));
        const QRgb *l = reinterpret_cast<const QRgb *>(layer.constScanLine(y));
        const uchar *k = mask.constScanLine(y);
        for (int x = 0; x < merged.width(); ++x) {
            const int a = k[x];
            if (!a) continue;
            if (a == 255) { m[x] = l[x]; continue; }
            auto mix = [a](int u, int v) { return (u * (255 - a) + v * a + 127) / 255; };
            m[x] = qRgba(mix(qRed(m[x]), qRed(l[x])), mix(qGreen(m[x]), qGreen(l[x])), mix(qBlue(m[x]), qBlue(l[x])), mix(qAlpha(m[x]), qAlpha(l[x])));
        }
    }
    return merged;
}

static QImage filtered(const Document *d, const VObject *o, const QImage &orig, const LiveEffect &e) {
    return filteredWith(d, o, orig, [&e](const QImage &in) { QImage x = in; fx::adjust(x, e); return x; },
                        e.type == EffectType::Offset);
}

bool MainWindow::applyEffectFilter(const LiveEffect &e) {
    Document *d = currentDoc();
    if (!d) return false;
    PixelEditGuard pxGuard(d);
    const QVector<VObject *> targets = filterTargets(d);
    if (targets.isEmpty()) return false;
    for (VObject *o : targets) o->image = filtered(d, o, o->image, e);
    d->commit(effectName(e.type));
    return true;
}

void MainWindow::effectFilter(int type) {
    LiveEffect e = LiveEffect::make(EffectType(type));
    if (e.type == EffectType::ColorFill) e.opacity = 100;
    if (e.type == EffectType::GaussianBlur) e.softness = 3;
    if (e.type == EffectType::PaletteSwap && e.palette.isEmpty()) {
        // start with the image's own colours in both rows (up to 64; otherwise a 16-colour adaptive palette)
        if (Document *d = currentDoc()) {
            PixelEditGuard pxGuard(d);
            const QVector<VObject *> targets = filterTargets(d);
            if (!targets.isEmpty()) {
                const QImage &img = targets.first()->image;
                QVector<QRgb> pal = fx2::uniqueColors(img, 65);
                if (pal.size() > 64) pal = fx2::uniqueColors(fx2::dither(img, fx2::DitherNone, fx2::PalAdaptive, 16, {}, 100, false), 16);
                e.palette = e.palette2 = pal;
            }
        }
    }
    effectFilterWith(e);
}

bool MainWindow::effectFilterWith(const LiveEffect &e, const QString &title) {
    Document *d = currentDoc();
    if (!d) return false;
    PixelEditGuard pxGuard(d);
    const QVector<VObject *> targets = filterTargets(d);
    if (targets.isEmpty()) return false;
    if (!effectHasSettings(e.type)) return applyEffectFilter(e);
    // live preview while the dialog is open; revert on cancel
    QHash<quint64, QImage> originals;
    for (VObject *o : targets) originals[o->id] = o->image;
    QImage sample = targets.first()->image;
    if (e.type == EffectType::Offset) {
        // the sliders span ±width/height of what is being offset: the selection's box when there is one
        const QPainterPath sel = selectionInImage(d, targets.first(), sample);
        if (!sel.isEmpty()) sample = sample.copy(sel.boundingRect().toAlignedRect() & sample.rect());
    }
    EffectDialog dlg(e, this, sample);
    const QString label = title.isEmpty() ? effectName(e.type) : title;
    dlg.setWindowTitle(label);
    auto preview = [d, originals](const LiveEffect &x) {
        for (auto it = originals.begin(); it != originals.end(); ++it)
            if (VObject *o = d->find(it.key())) o->image = filtered(d, o, it.value(), x);
        d->notifyChanged();
    };
    connect(&dlg, &EffectDialog::changed, this, preview);
    preview(dlg.effect()); // show the default settings straight away (e.g. Gaussian Blur 3, Color Fill), as Fireworks does
    if (dlg.exec() == QDialog::Accepted) {
        preview(dlg.effect());
        d->commit(label);
        return true;
    }
    d->revert();
    return false;
}

// ---- v0.3.0 filters and commands -----------------------------------------------------------

bool MainWindow::liquifyFilter(const std::function<void(LiquifyDialog &)> &script) {
    Document *d = currentDoc();
    if (!d) return false;
    PixelEditGuard pxGuard(d);
    const QVector<VObject *> targets = filterTargets(d);
    if (targets.isEmpty()) return false;
    // keep the id, not the pointer: the dialog runs an event loop and the document's object storage
    // may change under it (timers, autosave); look the object up again afterwards
    const quint64 id = targets.first()->id;
    const QImage orig = targets.first()->image;
    if (orig.isNull() || orig.width() <= 0 || orig.height() <= 0) return false;
    QPointer<Document> guard(d);
    LiquifyDialog dlg(orig, this);
    if (script) script(dlg);
    else if (dlg.exec() != QDialog::Accepted) return false;
    if (!guard || dlg.liquify().isIdentity()) return false;
    VObject *o = d->find(id);
    if (!o || o->image.cacheKey() != orig.cacheKey()) return false;   // gone or changed meanwhile
    const QImage res = dlg.liquified().convertToFormat(orig.format());
    if (res.size() != orig.size()) return false;
    o->image = filteredWith(d, o, orig, [res](const QImage &) { return res; }, false);
    d->commit(QStringLiteral("Liquify"));
    return true;
}

bool MainWindow::pixelUpscale(const std::function<bool(PixelUpscaleDialog &)> &script) {
    Document *d = currentDoc();
    if (!d) return false;
    PixelEditGuard pxGuard(d);
    const QVector<VObject *> targets = filterTargets(d);
    if (targets.isEmpty()) return false;
    static int lastMethod = int(pixelscale::Method::Xbrz), lastFactor = 2;
    static bool lastEnlarge = true;
    QVector<quint64> ids;
    for (VObject *o : targets) ids << o->id;
    QPointer<Document> guard(d);
    PixelUpscaleDialog dlg(targets.first()->image, this);
    dlg.setMethod(pixelscale::Method(lastMethod));
    dlg.setFactor(lastFactor);
    dlg.setEnlargeObject(lastEnlarge);
    if (script) { if (!script(dlg)) return false; }
    else if (dlg.exec() != QDialog::Accepted) return false;
    lastMethod = int(dlg.method());
    lastFactor = dlg.factor();
    lastEnlarge = dlg.enlargeObject();
    if (!guard) return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    int done = 0;
    for (quint64 id : ids) {
        VObject *o = d->find(id);
        if (!o || o->image.isNull()) continue;
        const qint64 px = qint64(o->image.width()) * o->image.height() * lastFactor * lastFactor;
        if (px > 256LL * 1024 * 1024) continue;   // over 256 Mpx: skip rather than exhaust memory
        o->image = pixelscale::upscale(o->image, dlg.method(), lastFactor).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (lastEnlarge) {
            const QRectF r = o->rect.normalized();
            o->rect = QRectF(r.topLeft(), r.size() * lastFactor);
        }
        ++done;
    }
    QApplication::restoreOverrideCursor();
    if (!done) return false;
    d->commit(QStringLiteral("Upscale (%1 \u00D7%2)").arg(pixelscale::name(dlg.method())).arg(lastFactor));
    statusBar()->showMessage(QStringLiteral("Upscaled %1 bitmap(s) %2\u00D7 with %3").arg(done).arg(lastFactor).arg(pixelscale::name(dlg.method())), 4000);
    return true;
}

bool MainWindow::traceBitmap(const std::function<bool(TraceDialog &)> &script) {
    Document *d = currentDoc();
    if (!d) return false;
    VObject *b = d->singleSelected();
    if (!b || b->type != ObjType::Bitmap || b->image.isNull()) {
        statusBar()->showMessage(QStringLiteral("Trace Bitmap: select a single bitmap object."), 4000);
        return false;
    }
    if (b->locked) {
        statusBar()->showMessage(QStringLiteral("Trace Bitmap: the bitmap is locked."), 4000);
        return false;
    }
    // last used settings for this session
    static trace::Options lastOpts;
    static trace::Insert lastIns;
    static int lastPal = TraceDialog::PalAdaptive;
    const quint64 id = b->id;
    const qint64 key = b->image.cacheKey();
    QPointer<Document> guard(d);
    TraceDialog dlg(b->image, this);
    dlg.setOptions(lastOpts, lastPal);
    dlg.setInsertOptions(lastIns);
    if (script) { if (!script(dlg)) return false; }
    else if (dlg.exec() != QDialog::Accepted) return false;
    lastOpts = dlg.options();
    lastOpts.palette.clear();
    lastIns = dlg.insertOptions();
    lastPal = dlg.paletteChoice();
    if (!guard) return false;
    VObject *o = d->find(id);
    if (!o || o->image.cacheKey() != key) return false;   // gone or changed meanwhile
    const trace::Result r = dlg.result();
    if (r.shapes.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Trace Bitmap: nothing to trace with these settings."), 4000);
        return false;
    }
    if (trace::insert(*d, id, r, dlg.insertOptions()).isEmpty()) return false;
    d->commit(QStringLiteral("Trace Bitmap"));
    statusBar()->showMessage(QStringLiteral("Traced: %1 paths, %2 nodes").arg(r.shapes.size()).arg(r.nodes), 4000);
    return true;
}

bool MainWindow::contentAwareFill(quint32 seed) {
    static bool running = false;   // the progress loop below must not start a second fill
    if (running) return false;
    Document *d = currentDoc();
    if (!d) return false;
    running = true;
    struct Reset { ~Reset() { running = false; } } reset;
    const bool testing = qApp->property("lw_testing").toBool();
    if (d->pixelSelection.isEmpty()) {
        if (!testing)
            QMessageBox::information(this, QStringLiteral("Content-Aware Fill"),
                                     QStringLiteral("Select the area to fill first (marquee, lasso, magic wand or Quick Mask), "
                                                    "or paint over it with the Content-Aware Remove tool (Rubber Stamp flyout)."));
        return false;
    }
    PixelEditGuard pxGuard(d);
    const QVector<VObject *> targets = filterTargets(d);
    if (targets.isEmpty()) return false;
    // Build every job up front (ids + copies of the pixels and masks); nothing below holds a VObject*
    // across the event loop, so the document can change without leaving dangling pointers.
    struct Job { quint64 id; QImage src, mask; qint64 key; };
    QVector<Job> jobs;
    for (VObject *o : targets) {
        if (o->type != ObjType::Bitmap || o->image.isNull() || o->image.width() <= 0 || o->image.height() <= 0) continue;
        QImage mask = d->selectionMask(pxedit::docToImage(*o), o->image.size());
        if (mask.isNull()) continue;
        mask = mask.convertToFormat(QImage::Format_Alpha8);
        bool hit = false;
        for (int y = 0; y < mask.height() && !hit; ++y) {
            const uchar *m = mask.constScanLine(y);
            for (int x = 0; x < mask.width(); ++x) if (m[x] >= 128) { hit = true; break; }
        }
        if (hit) jobs.push_back({o->id, o->image, mask, o->image.cacheKey()});
    }
    if (jobs.isEmpty()) return false;
    // The fill runs on a worker thread; the GUI keeps painting and the progress dialog (application
    // modal, so no edits can reach the document meanwhile) can cancel it.
    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto progress = std::make_shared<std::atomic_int>(0);
    QProgressDialog *pd = nullptr;
    if (!testing) {
        pd = new QProgressDialog(QStringLiteral("Filling the selection..."), QStringLiteral("Cancel"), 0, 100, this);
        pd->setWindowTitle(QStringLiteral("Content-Aware Fill"));
        pd->setWindowModality(Qt::ApplicationModal);
        pd->setMinimumDuration(150);
        pd->setValue(0);
        connect(pd, &QProgressDialog::canceled, pd, [cancel] { cancel->store(true); });
    }
    QPointer<Document> guard(d);
    const int nJobs = jobs.size();
    QFuture<QVector<QImage>> fut = QtConcurrent::run([jobs, seed, cancel, progress, nJobs] {
        QVector<QImage> out;
        for (int i = 0; i < nJobs; ++i) {
            if (cancel->load()) break;
            out << fx2::contentAwareFill(jobs[i].src, jobs[i].mask, seed, [&](int pct) {
                progress->store((i * 100 + qBound(0, pct, 100)) / nJobs);
                return !cancel->load();
            });
        }
        return out;
    });
    QFutureWatcher<QVector<QImage>> watcher;
    QEventLoop loop;
    QTimer tick;
    connect(&watcher, &QFutureWatcher<QVector<QImage>>::finished, &loop, &QEventLoop::quit);
    connect(&tick, &QTimer::timeout, &loop, [pd, progress] { if (pd) pd->setValue(qBound(0, progress->load(), 99)); });
    watcher.setFuture(fut);
    if (!fut.isFinished()) {
        tick.start(50);
        loop.exec();
        tick.stop();
    }
    fut.waitForFinished();
    delete pd;
    const bool cancelled = cancel->load();
    if (!guard) return false;   // the document went away meanwhile
    const QVector<QImage> results = fut.result();
    bool any = false;
    if (!cancelled) {
        for (int i = 0; i < results.size() && i < jobs.size(); ++i) {
            VObject *o = d->find(jobs[i].id);
            if (!o || o->image.cacheKey() != jobs[i].key || results[i].isNull() || results[i].size() != o->image.size()) continue;
            o->image = results[i].convertToFormat(QImage::Format_ARGB32_Premultiplied);
            any = true;
        }
    }
    if (cancelled || !any) { d->revert(); return false; }
    d->commit(QStringLiteral("Content-Aware Fill"));
    return true;
}

bool MainWindow::convertToIndexed() {
    LiveEffect e = LiveEffect::make(EffectType::Dither);
    e.mode = fx2::DitherNone;
    e.mode2 = fx2::PalAdaptive;
    e.posterize = 256;
    return effectFilterWith(e, QStringLiteral("Convert to Indexed Color"));
}

// The colours of an image as a palette: its own colours when there are at most 256, else an adaptive 256.
static QVector<QRgb> imagePalette(const QImage &img, QImage *indexed) {
    QVector<QRgb> pal = fx2::uniqueColors(img, 257);
    *indexed = img;
    if (pal.size() > 256) {
        *indexed = fx2::dither(img, fx2::DitherNone, fx2::PalAdaptive, 256, {}, 100, false);
        pal = fx2::uniqueColors(*indexed, 256);
    }
    return pal;
}

bool MainWindow::editPalette(const QVector<QRgb> *palette) {
    Document *d = currentDoc();
    if (!d) return false;
    PixelEditGuard pxGuard(d);
    const QVector<VObject *> targets = filterTargets(d);
    if (targets.isEmpty()) return false;
    QHash<quint64, QImage> originals;   // indexed (<= 256 colours) versions of the targets
    QVector<QRgb> pal;
    for (VObject *o : targets) {
        QImage ix;
        const QVector<QRgb> p = imagePalette(o->image, &ix);
        originals[o->id] = ix;
        for (QRgb c : p) if (!pal.contains(c) && pal.size() < 256) pal << c;
    }
    const QVector<QRgb> base = pal;
    auto apply = [d, originals, base](const QVector<QRgb> &edited) {
        for (auto it = originals.begin(); it != originals.end(); ++it) {
            VObject *o = d->find(it.key());
            if (!o) continue;
            QImage out;
            if (edited.size() == base.size()) out = fx2::paletteSwap(it.value(), base, edited, false);   // index-wise remap
            else if (edited.isEmpty()) out = it.value();
            else out = fx2::dither(it.value(), fx2::DitherNone, fx2::PalCustom, int(edited.size()), edited, 100, false);
            o->image = filteredWith(d, o, it.value(), [out](const QImage &) { return out; }, false).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
        d->notifyChanged();
    };
    if (palette) {
        apply(*palette);
        d->commit(QStringLiteral("Edit Palette"));
        return true;
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("editPaletteDialog"));
    dlg.setWindowTitle(QStringLiteral("Edit Palette"));
    auto *v = new QVBoxLayout(&dlg);
    auto *note = new QLabel(QStringLiteral("Double-click a swatch to change it. Same number of colours: every pixel of a colour follows it "
                                           "(palette edit); a different number: the image is mapped to the new palette."));
    note->setWordWrap(true);
    v->addWidget(note);
    auto *ed = new PaletteEditor(&dlg, originals.value(targets.first()->id));
    ed->setColors(base);
    v->addWidget(ed, 1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    connect(ed, &PaletteEditor::changed, this, [ed, apply] { apply(ed->colors()); });
    apply(base);
    if (dlg.exec() != QDialog::Accepted) { d->revert(); return false; }
    apply(ed->colors());
    d->commit(QStringLiteral("Edit Palette"));
    return true;
}

bool MainWindow::exportSlices(const QString &dirIn) {
    Document *d = currentDoc();
    if (!d) return false;
    QString dir = dirIn;
    if (dir.isEmpty()) {
        dir = filedialogs::getExistingDirectory(this, QStringLiteral("Export Slices to Folder"), m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir);
        if (dir.isEmpty()) return false;
    }
    QString err;
    QStringList written;
    if (!html::exportSlices(*d, dir, &err, &written)) {
        if (!qApp->property("lw_testing").toBool())
            QMessageBox::warning(this, QStringLiteral("Export Slices"), QStringLiteral("Could not export slices:\n%1").arg(err));
        return false;
    }
    m_lastDir = dir;
    statusBar()->showMessage(QStringLiteral("Exported %1 slice image(s) to %2").arg(written.size()).arg(dir), 6000);
    return true;
}

bool MainWindow::exportImageMap(const QString &pathIn) {
    Document *d = currentDoc();
    if (!d) return false;
    QString path = pathIn;
    if (path.isEmpty()) {
        QString base = d->filePath.isEmpty() ? d->title() : QFileInfo(d->filePath).completeBaseName();
        brand::stripNativeExt(base);
        path = filedialogs::getSaveFileName(this, QStringLiteral("Export Image Map"),
                                            QDir(m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir).filePath(base + QStringLiteral(".html")),
                                            QStringLiteral("HTML (*.html *.htm)"));
        if (path.isEmpty()) return false;
    }
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".html");
    html::Options o;
    o.title = d->title();
    QString err;
    QStringList written;
    if (!html::exportImageMap(*d, path, o, &err, &written)) {
        if (!qApp->property("lw_testing").toBool())
            QMessageBox::warning(this, QStringLiteral("Export Image Map"), QStringLiteral("Could not export %1:\n%2").arg(path, err));
        return false;
    }
    m_lastDir = QFileInfo(path).absolutePath();
    statusBar()->showMessage(QStringLiteral("Exported %1 (%2 file(s))").arg(path).arg(written.size()), 6000);
    return true;
}

void MainWindow::batchProcess() {
    QVector<QPair<QString, Document *>> open;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *v = qobject_cast<DocumentView *>(m_tabs->widget(i))) {
            Document *d = v->doc();
            if (isSymbolEditor(d)) continue;
            QString name = d->filePath.isEmpty() ? d->title() : d->filePath;
            open << qMakePair(name, d);
        }
    BatchDialog dlg(open, m_lastDir.isEmpty() ? QDir::homePath() : m_lastDir, this);
    dlg.exec();
}

bool MainWindow::renderTexture(const QString &paramsJson) {
    Document *d = currentDoc();
    if (!d) return false;
    QVector<VObject *> targets;
    PixelEditGuard pxGuard(d);
    for (VObject *o : d->selectedEditable()) if (o->type == ObjType::Bitmap && !o->rect.isEmpty() && !o->image.isNull() && d->beginPixelEdit(*o)) targets << o;
    const QRect canvas(0, 0, d->s.width, d->s.height);
    // no bitmap selected: a new bitmap covering the pixel selection (or the canvas)
    const QRect newRect = d->pixelSelection.isEmpty() ? canvas : (d->pixelSelectionBounds() & canvas);
    if (targets.isEmpty() && newRect.isEmpty()) return false;
    QSize size = newRect.size();
    if (!targets.isEmpty()) {
        const VObject *o = targets.first();
        const QPainterPath sel = selectionInImage(d, o, o->image);
        size = sel.isEmpty() ? o->image.size() : (sel.boundingRect().toAlignedRect() & o->image.rect()).size();
    }
    QSettings st;
    proc::Params p;
    if (!paramsJson.isEmpty()) {
        p = proc::fromString(paramsJson);
    } else {
        const QString last = st.value(QStringLiteral("procedural/lastRender")).toString();
        if (!last.isEmpty()) p = proc::fromString(last);
        else { p.gen = QStringLiteral("clouds"); p.ramp = fills::gradientPreset(fills::gradientPresetNames().value(0)); }
        ProceduralDialog dlg(p, ProceduralDialog::Mode::Render, size.isValid() ? size : QSize(256, 256), this);
        // live canvas preview in the selected bitmaps: the (capped) preview scaled up; Cancel restores
        QHash<quint64, QImage> originals;
        for (VObject *o : targets) originals[o->id] = o->image;
        auto restore = [d, &originals] {
            for (auto it = originals.cbegin(); it != originals.cend(); ++it) if (VObject *o = d->find(it.key())) o->image = it.value();
            d->notifyChanged();
        };
        auto show = [d, &dlg, &originals, restore] {
            if (originals.isEmpty()) return;
            const QImage pv = dlg.previewImage();
            if (!dlg.canvasPreview() || pv.isNull()) { restore(); return; }
            for (auto it = originals.cbegin(); it != originals.cend(); ++it)
                if (VObject *o = d->find(it.key()))
                    o->image = filteredWith(d, o, it.value(), [&pv](const QImage &in) {
                        return pv.scaled(in.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation); }, true);
            d->notifyChanged();
        };
        connect(&dlg, &ProceduralDialog::previewUpdated, &dlg, show);
        connect(&dlg, &ProceduralDialog::canvasPreviewToggled, &dlg, show);
        show();
        const bool ok = dlg.exec() == QDialog::Accepted;
        restore();
        if (!ok) return false;
        p = dlg.params();
        st.setValue(QStringLiteral("procedural/lastRender"), proc::toString(p));
    }
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QHash<quint64, QImage> cache;   // by w<<32|h
    auto tex = [&](QSize s) {
        const quint64 key = (quint64(s.width()) << 32) | quint64(s.height());
        if (!cache.contains(key)) {
            proc::Params q = p;
            q.width = s.width(); q.height = s.height();
            cache[key] = proc::render(q, s.width(), s.height());
        }
        return cache[key];
    };
    if (!targets.isEmpty()) {
        for (VObject *o : targets)
            o->image = filteredWith(d, o, o->image, [&](const QImage &in) { return tex(in.size()); }, true);
    } else {
        VObject o;
        o.type = ObjType::Bitmap;
        o.rect = newRect;
        QImage img = tex(newRect.size());
        if (!d->pixelSelection.isEmpty()) {   // clip to the selection, keeping its soft edges
            const QImage mask = d->selectionMask(QTransform::fromTranslate(-newRect.x(), -newRect.y()), newRect.size());
            img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < img.height(); ++y) {
                QRgb *l = reinterpret_cast<QRgb *>(img.scanLine(y));
                const uchar *k = mask.constScanLine(y);
                for (int x = 0; x < img.width(); ++x) {
                    const int a = k[x];
                    l[x] = qRgba(qRed(l[x]) * a / 255, qGreen(l[x]) * a / 255, qBlue(l[x]) * a / 255, qAlpha(l[x]) * a / 255);
                }
            }
        }
        o.image = img;
        o.name = proc::generatorLabel(p.gen);
        o.id = d->newId();
        d->addObject(o, -1, true);
    }
    QGuiApplication::restoreOverrideCursor();
    d->commit(QStringLiteral("Render Texture"));
    return true;
}

// Commands > Make Seamless Tile: offset by half (wrapping) and cross-fade the seams, with a live preview.
void MainWindow::makeSeamlessTile(double blendWidth) {
    Document *d = currentDoc();
    if (!d) return;
    PixelEditGuard pxGuard(d);
    const QVector<VObject *> targets = filterTargets(d);
    if (targets.isEmpty()) return;
    QHash<quint64, QImage> originals;
    for (VObject *o : targets) originals[o->id] = o->image;
    auto preview = [d, originals](double bw) {
        for (auto it = originals.begin(); it != originals.end(); ++it)
            if (VObject *o = d->find(it.key()))
                o->image = filteredWith(d, o, it.value(), [bw](const QImage &in) { return fx::makeSeamless(in, bw); }, true);
        d->notifyChanged();
    };
    if (blendWidth >= 0) { // scripted / tests: no dialog
        preview(blendWidth);
        d->commit(QStringLiteral("Make Seamless Tile"));
        return;
    }
    const QImage &img = targets.first()->image;
    const int maxBlend = qMax(1, qMin(img.width(), img.height()) / 2);
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("seamlessDialog"));
    dlg.setWindowTitle(QStringLiteral("Make Seamless Tile"));
    auto *f = new QFormLayout(&dlg);
    auto *bw = new PopupSliderDoubleField;
    bw->setObjectName(QStringLiteral("seamlessBlend"));
    bw->setRange(0, maxBlend * 2);
    bw->setSliderRange(0, maxBlend);
    bw->setDecimals(0);
    bw->setSuffix(QStringLiteral(" px"));
    bw->setKeyboardTracking(false);
    bw->setValue(qMin(32, maxBlend / 2 > 0 ? maxBlend / 2 : 1));
    bw->setToolTip(QStringLiteral("Width of the cross-fade over the seams that the half offset brings to the middle"));
    f->addRow(QStringLiteral("Blend width:"), bw);
    auto *pv = new QCheckBox(QStringLiteral("Preview"));
    pv->setObjectName(QStringLiteral("seamlessPreview"));
    pv->setChecked(true);
    f->addRow(QString(), pv);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto update = [=] {
        if (pv->isChecked()) preview(bw->value());
        else { for (auto it = originals.begin(); it != originals.end(); ++it) if (VObject *o = d->find(it.key())) o->image = it.value(); d->notifyChanged(); }
    };
    connect(bw, &QDoubleSpinBox::valueChanged, &dlg, update);
    connect(pv, &QCheckBox::toggled, &dlg, update);
    update();
    if (dlg.exec() == QDialog::Accepted) {
        preview(bw->value());
        d->commit(QStringLiteral("Make Seamless Tile"));
    } else {
        d->revert();
    }
}

void MainWindow::textStyle(int what) {
    Document *d = currentDoc();
    if (!d) return;
    bool any = false;
    auto objs = d->selectedEditable();
    bool allBold = true, allItalic = true;
    for (VObject *o : objs) if (o->type == ObjType::Text) { allBold &= o->bold; allItalic &= o->italic; }
    for (VObject *o : objs) {
        if (o->type != ObjType::Text) continue;
        switch (what) {
        case 0: o->bold = !allBold; break;
        case 1: o->italic = !allItalic; break;
        case 2: case 3: case 4: o->textAlign = what - 2; break;
        case 5: o->fontSize = qMin(2000.0, o->fontSize + 2); break;
        case 6: o->fontSize = qMax(1.0, o->fontSize - 2); break;
        case 7: o->textAlign = 3; break;
        case 8: o->textOrientation = 0; break;
        case 9: o->textOrientation = 1; break;
        case 10: o->textDirection = 0; break;
        case 11: o->textDirection = 1; break;
        case 12:
            if (o->textOnPath) o->textPathReverse = !o->textPathReverse;
            else continue;
            break;
        }
        if (what != 12) o->fitTextBox();
        any = true;
    }
    if (!any) {
        // no text selected: change the defaults of the Text tool
        if (what == 0) appState().bold = !appState().bold;
        else if (what == 1) appState().italic = !appState().italic;
        else if (what == 2 || what == 3 || what == 4) appState().textAlign = what - 2;
        else if (what == 7) appState().textAlign = 3;
        else if (what == 8) appState().textOrientation = 0;
        else if (what == 9) appState().textOrientation = 1;
        else if (what == 10) appState().textDirection = 0;
        else if (what == 11) appState().textDirection = 1;
        appState().notifyOptions();
        return;
    }
    static const char *labels[] = {"Bold", "Italic", "Align Left", "Align Center", "Align Right", "Font Size", "Font Size",
                                   "Align Justify", "Horizontal Text", "Vertical Text", "Left to Right", "Right to Left", "Reverse Direction"};
    d->commit(QString::fromLatin1(labels[what]));
}

void MainWindow::alterPathDialog(int which) {
    Document *d = currentDoc();
    if (!d || d->selectedEditable().isEmpty()) return;
    static const char *titles[] = {"Simplify", "Expand Stroke", "Inset Path"};
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("alterPathDialog"));
    dlg.setWindowTitle(QString::fromLatin1(titles[which]));
    auto *form = new QFormLayout(&dlg);
    auto *amount = new PopupSliderDoubleField;
    amount->setObjectName(QStringLiteral("pathAmount"));
    amount->setDecimals(1);
    QComboBox *corner = nullptr, *ends = nullptr, *dir = nullptr;
    if (which == 0) {
        amount->setRange(0.1, 100);
        amount->setValue(1);
        form->addRow(QStringLiteral("Amount:"), amount);
    } else {
        amount->setRange(0.1, 500);
        amount->setValue(which == 1 ? qMax(1.0, d->selectedEditable().first()->strokeWidth) : 4);
        amount->setSuffix(QStringLiteral(" px"));
        form->addRow(QStringLiteral("Width:"), amount);
    }
    if (which == 1) {
        corner = new QComboBox;
        corner->setObjectName(QStringLiteral("pathCorner"));
        corner->addItems({QStringLiteral("Miter"), QStringLiteral("Round"), QStringLiteral("Bevel")});
        form->addRow(QStringLiteral("Corners:"), corner);
        ends = new QComboBox;
        ends->setObjectName(QStringLiteral("pathEnds"));
        ends->addItems({QStringLiteral("Butt"), QStringLiteral("Square"), QStringLiteral("Round")});
        form->addRow(QStringLiteral("Ends:"), ends);
    }
    if (which == 2) {
        dir = new QComboBox;
        dir->setObjectName(QStringLiteral("pathDirection"));
        dir->addItems({QStringLiteral("Inside"), QStringLiteral("Outside")});
        form->addRow(QStringLiteral("Direction:"), dir);
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    int n = 0;
    if (which == 0) n = paths::simplify(*d, amount->value());
    else if (which == 1) {
        static const Qt::PenJoinStyle joins[] = {Qt::MiterJoin, Qt::RoundJoin, Qt::BevelJoin};
        static const Qt::PenCapStyle caps[] = {Qt::FlatCap, Qt::SquareCap, Qt::RoundCap};
        n = paths::expandStroke(*d, amount->value(), joins[corner->currentIndex()], caps[ends->currentIndex()]);
    } else n = paths::insetPath(*d, amount->value(), dir->currentIndex() == 0);
    if (n) d->commit(QString::fromLatin1(titles[which]));
}

// ---------------------------------------------------------------- canvas context menu
// Edit > Paste Attributes (Fireworks): the fill, stroke, Live Effects, opacity and blend mode of
// the first copied object (and its text attributes, between text objects) onto the selection.
bool MainWindow::pasteAttributes() {
    Document *d = currentDoc();
    if (!d || g_clipboard.isEmpty()) return false;
    const VObject &src = g_clipboard.first();
    Style st = Style::fromObject(src, QString());
    bool any = false;
    for (VObject *o : d->selectedEditable()) {
        Style s = st;
        s.hasText = src.type == ObjType::Text && o->type == ObjType::Text;
        bool ch = s.applyTo(*o);
        if (o->opacity != src.opacity || o->blend != src.blend) { o->opacity = src.opacity; o->blend = src.blend; ch = true; }
        any |= ch;
    }
    if (any) d->commit(QStringLiteral("Paste Attributes"));
    return any;
}

void MainWindow::showCanvasContextMenu(const QPoint &global) {
    Document *d = currentDoc();
    if (!d) return;
    QMenu *menu = buildCanvasContextMenu(d);
    menu->popup(global);
}

// Fireworks MX 2004's canvas pop-up menu: context sensitive (objects, pixel selection or the empty
// canvas), built from the application's own actions so shortcuts and enabled states match the menus.
QMenu *MainWindow::buildCanvasContextMenu(Document *d) {
    if (m_ctxMenu) m_ctxMenu->deleteLater();
    updateActions();
    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("canvasContextMenu"));
    menu->setToolTipsVisible(true);
    m_ctxMenu = menu;
    auto add = [this](QMenu *m, const char *id) {
        if (QAction *a = action(QString::fromLatin1(id)); a && a->isVisible()) m->addAction(a);
    };
    auto sub = [menu](const QString &title, const char *name) {
        QMenu *m = menu->addMenu(title);
        m->setObjectName(QString::fromLatin1(name));
        return m;
    };
    auto sep = [menu] { menu->addSeparator(); };

    const QVector<VObject *> sel = d ? d->selectedEditable() : QVector<VObject *>();
    int vec = 0, text = 0, bmp = 0, inst = 0, grad = 0;
    for (VObject *o : sel) {
        if (o->type == ObjType::Text) ++text;
        else if (o->type == ObjType::Bitmap) ++bmp;
        else if (o->isInstance()) ++inst;
        else if (o->isVector()) ++vec;
        if (o->isVector() && o->fillOn && fills::isGradient(o->fillKind)) ++grad;
    }
    const bool pixelSel = d && !d->pixelSelection.isEmpty();
    QString kind;

    if (!sel.isEmpty()) {
        kind = bmp == sel.size() ? QStringLiteral("bitmap") : text == sel.size() ? QStringLiteral("text")
             : sel.size() > 1 ? QStringLiteral("multiple") : inst ? QStringLiteral("symbol") : QStringLiteral("vector");
        for (const char *id : {"edit.cut", "edit.copy", "edit.paste", "edit.pasteattributes"}) add(menu, id);
        if (pixelSel && bmp) add(menu, "edit.copymerged");
        for (const char *id : {"edit.duplicate", "edit.clone", "edit.clear"}) add(menu, id);
        sep();
        QMenu *ar = sub(QStringLiteral("&Arrange"), "ctxArrange");
        for (const char *id : {"arrange.front", "arrange.forward", "arrange.backward", "arrange.back"}) add(ar, id);
        QMenu *al = sub(QStringLiteral("A&lign"), "ctxAlign");
        for (int i = 0; QAction *a = action(QStringLiteral("align.%1").arg(i)); ++i) {
            if (i == 3 || i == 6) al->addSeparator();
            al->addAction(a);
        }
        // v0.3.2: the same actions, order and separators as Modify > Transform (Perspective was missing)
        QMenu *tr = sub(QStringLiteral("&Transform"), "ctxTransform");
        if (QMenu *mt = findChild<QMenu *>(QStringLiteral("modifyTransformMenu")))
            for (QAction *a : mt->actions()) { if (a->isSeparator()) tr->addSeparator(); else tr->addAction(a); }
        if (vec || text) {
            QMenu *cp = sub(QStringLiteral("Combine &Path"), "ctxCombinePath");
            for (const char *id : {"path.join", "path.split"}) add(cp, id);
            cp->addSeparator();
            for (const char *id : {"path.union", "path.intersect", "path.punch", "path.crop"}) add(cp, id);
            QMenu *ap = sub(QStringLiteral("Alter Pat&h"), "ctxAlterPath");
            for (const char *id : {"path.simplify", "path.expand", "path.inset"}) add(ap, id);
            ap->addSeparator();
            add(ap, "path.convert");
        }
        if (text) {
            QMenu *tm = sub(QStringLiteral("Te&xt"), "ctxText");
            for (const char *id : {"edit.edittext", "text.attach", "text.detach", "path.convert"}) add(tm, id);
        }
        QMenu *sy = sub(QStringLiteral("S&ymbol"), "ctxSymbol");
        add(sy, "symbol.convert");
        if (inst) { add(sy, "symbol.edit"); add(sy, "symbol.breakapart"); }
        if (sel.size() > 1) add(sy, "symbol.tween");
        sep();
        // v0.3.2 groups and masks, as in Modify
        add(menu, "modify.group");
        add(menu, "modify.ungroup");
        if (QMenu *mm = findChild<QMenu *>(QStringLiteral("modifyMaskMenu"))) {
            QMenu *mk = sub(QStringLiteral("Mas&k"), "ctxMask");
            for (QAction *a : mm->actions()) { if (a->isSeparator()) mk->addSeparator(); else mk->addAction(a); }
        }
        add(menu, "modify.converttobitmap");
        add(menu, "modify.flattensel");
        if (bmp) add(menu, "modify.tracebitmap");
        if (bmp || pixelSel) {
            QMenu *px = sub(QStringLiteral("&Marquee"), "ctxMarquee");
            for (const char *id : {"select.all", "select.none", "select.inverse", "select.feather"}) add(px, id);
            px->addSeparator();
            add(px, "select.quickmask");   // edit the selection as a mask
            if (pixelSel) { add(px, "modify.bitmapviacopy"); add(px, "modify.bitmapviacut"); }
        }
        sep();
        add(menu, "edit.inserthotspot");
        add(menu, "edit.insertslice");
        sep();
        add(menu, "modify.lock");
        add(menu, "view.hide");
        sep();
        if (QMenu *live = findChild<QMenu *>(QStringLiteral("liveEffectsMenu"))) menu->addMenu(live);
        add(menu, "cmd.removefx");
        add(menu, "modify.savestyle");
        if (grad) add(menu, "modify.editgradient");
    } else if (pixelSel) {
        kind = QStringLiteral("pixels");
        for (const char *id : {"edit.cut", "edit.copy", "edit.copymerged", "edit.paste", "edit.clear"}) add(menu, id);
        sep();
        for (const char *id : {"select.all", "select.none", "select.inverse", "select.feather", "select.expand", "select.contract",
                               "select.border", "select.smooth", "select.similar"}) add(menu, id);
        sep();
        for (const char *id : {"select.quickmask", "select.topath", "select.save", "select.restore"}) add(menu, id);
        sep();
        for (const char *id : {"modify.bitmapviacopy", "modify.bitmapviacut"}) add(menu, id);
    } else {
        kind = QStringLiteral("canvas");
        add(menu, "edit.paste");
        add(menu, "edit.pasteinplace");
        if (d && d->enteredGroup) add(menu, "select.exitgroup");
        add(menu, "select.all");
        sep();
        for (const char *id : {"modify.imagesize", "modify.canvassize", "modify.canvascolor", "modify.trim", "modify.fit"}) add(menu, id);
        sep();
        QMenu *gm = sub(QStringLiteral("&Grid"), "ctxGrid");
        for (const char *id : {"view.grid", "view.gridsnap", "view.gridedit"}) add(gm, id);
        QMenu *gd = sub(QStringLiteral("G&uides"), "ctxGuides");
        for (const char *id : {"view.guides", "view.guideslock", "view.guidessnap", "view.guidesedit"}) add(gd, id);
        add(menu, "view.rulers");
        QMenu *zm = sub(QStringLiteral("&Zoom"), "ctxZoom");
        for (const char *id : {"view.zoomin", "view.zoomout", "view.actual", "view.fit"}) add(zm, id);
        zm->addSeparator();
        QList<QAction *> presets;
        for (const QString &id : actionIds())
            if (id.startsWith(QLatin1String("view.zoom")) && id.size() > 9 && id.at(9).isDigit()) presets << action(id);
        std::sort(presets.begin(), presets.end(), [](QAction *a, QAction *b) { return a->objectName().mid(9).toInt() < b->objectName().mid(9).toInt(); });
        for (QAction *a : presets) zm->addAction(a);
        sep();
        add(menu, "view.showall");
        add(menu, "modify.unlockall");
    }
    sep();
    add(menu, "view.properties");
    menu->setProperty("context", kind);
    return menu;
}
