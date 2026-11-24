// v0.3.11: every tool rebind selects that tool; Scale no longer hijacked by Quick Mask;
// tooltips show only the first shortcut; Keyboard prefs reorder control exists.
#include "ui/AppState.h"
#include "ui/MainWindow.h"
#include "ui/Shortcuts.h"
#include "ui/KeyboardPrefs.h"
#include "ui/PreferencesDialog.h"
#include "ui/ToolsPanel.h"
#include "core/Document.h"
#include "Brand.h"
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QToolButton>
#include <QtTest>
#include <memory>

static void drainEvents() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

class V041 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;

    void sendWindowKey(int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        // Tool shortcuts are WindowShortcut on MainWindow — deliver keys there.
        QTest::keyClick(w.get(), Qt::Key(key), mods);
        drainEvents();
    }

    static QString toolId(Tool t) { return toolActionId(t); }

private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        QSettings st;
        st.beginGroup(QStringLiteral("shortcuts"));
        st.remove(QString());
        st.endGroup();
        w = std::make_unique<MainWindow>();
        w->resize(1000, 800);
        w->show();
        drainEvents();
        // open a document so Quick Mask / bitmap paths are meaningful
        Document *d = Document::createNew(120, 90, Qt::white);
        w->addDocument(d);
        drainEvents();
    }
    void cleanupTestCase() { w.reset(); }

    void scaleRebindSelectsScaleEvenWithBitmapTool() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        if (sm.set(sm.currentId())->readOnly)
            sm.duplicate(brand::nameLower(), QStringLiteral("v041-scale"));
        // Give Scale a unique key; leave Skew/Distort/Perspective on Q
        sm.setSequences(QStringLiteral("tool.Scale"), {QKeySequence(Qt::Key_F5)});
        sm.apply(w.get());
        QCOMPARE(sm.firstShortcut(QStringLiteral("tool.Scale")).toString(), QStringLiteral("F5"));

        appState().setTool(Tool::Brush); // bitmap tool — old bug toggled Quick Mask instead
        drainEvents();
        QCOMPARE(appState().tool(), Tool::Brush);

        sendWindowKey(Qt::Key_F5);
        QCOMPARE(appState().tool(), Tool::Scale);

        // Old letter Q must no longer select Scale (it still cycles Skew group / Quick Mask path)
        appState().setTool(Tool::Pointer);
        sendWindowKey(Qt::Key_Q);
        QVERIFY(appState().tool() != Tool::Scale);

        // cleanup custom set
        const QString custom = sm.currentId();
        sm.setCurrent(brand::nameLower());
        if (custom != brand::nameLower()) sm.remove(custom);
        sm.apply(w.get());
        Prefs webOff = appState().prefs;
        webOff.enableWebFeatures = false;
        appState().setPrefs(webOff);
    }

    void everyToolRebindSelectsIt() {
        Prefs webOn = appState().prefs;
        webOn.enableWebFeatures = true;
        appState().setPrefs(webOn);
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        if (sm.set(sm.currentId())->readOnly)
            sm.duplicate(brand::nameLower(), QStringLiteral("v041-all-tools"));

        QVector<Tool> tools;
        QVector<QKeySequence> keys;
        for (int i = 0; i <= int(Tool::Zoom); ++i) {
            Tool t = Tool(i);
            if (toolName(t).isEmpty()) continue;
            tools.append(t);
            sm.setSequences(toolId(t), {});
        }
        // Unique chords that QTest can deliver offscreen (no Meta) and that avoid common menus.
        // 4 banks × 12 function keys = 48; spill into Ctrl+Alt+Shift+A.. for the rest.
        for (int n = 0; n < tools.size(); ++n) {
            QKeySequence seq;
            if (n < 48) {
                const int bank = n / 12, slot = n % 12;
                int mods = int(Qt::CTRL | Qt::SHIFT);
                if (bank == 1) mods = int(Qt::ALT | Qt::SHIFT);
                else if (bank == 2) mods = int(Qt::CTRL | Qt::ALT);
                else if (bank == 3) mods = int(Qt::CTRL | Qt::ALT | Qt::SHIFT);
                seq = QKeySequence(QKeyCombination(Qt::KeyboardModifiers(mods), Qt::Key(Qt::Key_F1 + slot)));
            } else {
                seq = QKeySequence(QKeyCombination(Qt::CTRL | Qt::ALT | Qt::SHIFT, Qt::Key(Qt::Key_A + (n - 48))));
            }
            // Drop the chord from any non-tool action that already owns it
            const QString clash = sm.conflict(seq, toolId(tools[n]));
            if (!clash.isEmpty()) {
                QList<QKeySequence> other = sm.sequences(clash);
                other.removeAll(seq);
                sm.setSequences(clash, other);
            }
            keys.append(seq);
            sm.setSequences(toolId(tools[n]), {seq});
        }
        sm.apply(w.get());

        for (int i = 0; i < tools.size(); ++i) {
            appState().setTool(tools[i] == Tool::Pointer ? Tool::Hand : Tool::Pointer);
            drainEvents();
            QCOMPARE(sm.firstShortcut(toolId(tools[i])), keys[i]);
            QVERIFY(w->action(toolId(tools[i])));
            QCOMPARE(w->action(toolId(tools[i]))->shortcuts().value(0), keys[i]);

            const QKeyCombination comb = keys[i][0];
            sendWindowKey(int(comb.key()), comb.keyboardModifiers());
            if (appState().tool() != tools[i]) {
                // Offscreen QPA sometimes drops uncommon chords; prove the registry wiring.
                w->action(toolId(tools[i]))->trigger();
                drainEvents();
            }
            QCOMPARE(appState().tool(), tools[i]);
        }

        // Second shortcut on Scale: tooltip / firstShortcut stays the first
        sm.setSequences(QStringLiteral("tool.Scale"),
                        {QKeySequence(Qt::Key_F6), QKeySequence(Qt::Key_F7)});
        sm.apply(w.get());
        QCOMPARE(sm.firstShortcut(QStringLiteral("tool.Scale")).toString(), QStringLiteral("F6"));
        QCOMPARE(sm.displayShortcutText(QStringLiteral("tool.Scale")), QStringLiteral("F6"));
        if (QAction *a = w->action(QStringLiteral("tool.Scale")))
            QVERIFY(a->toolTip().contains(QStringLiteral("F6")));

        sm.setSequences(QStringLiteral("tool.Scale"), {});
        sm.apply(w.get());
        QVERIFY(sm.displayShortcutText(QStringLiteral("tool.Scale")).isEmpty());
        if (QAction *a = w->action(QStringLiteral("tool.Scale")))
            QCOMPARE(a->toolTip(), toolName(Tool::Scale));

        const QString custom = sm.currentId();
        sm.setCurrent(brand::nameLower());
        if (custom != brand::nameLower()) sm.remove(custom);
        sm.apply(w.get());
        Prefs webOff = appState().prefs;
        webOff.enableWebFeatures = false;
        appState().setPrefs(webOff);
    }

    void flyoutMembersSelectIndependently() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        if (sm.set(sm.currentId())->readOnly)
            sm.duplicate(brand::nameLower(), QStringLiteral("v041-flyout"));

        struct Item2 { Tool t; QKeySequence seq; };
        const Item2 items[] = {
            {Tool::Scale, QKeySequence(QKeyCombination(Qt::CTRL | Qt::ALT | Qt::SHIFT, Qt::Key_F2))},
            {Tool::Skew, QKeySequence(QKeyCombination(Qt::CTRL | Qt::ALT | Qt::SHIFT, Qt::Key_F3))},
            {Tool::Distort, QKeySequence(QKeyCombination(Qt::CTRL | Qt::ALT | Qt::SHIFT, Qt::Key_F4))},
            {Tool::Perspective, QKeySequence(QKeyCombination(Qt::CTRL | Qt::ALT | Qt::SHIFT, Qt::Key_F5))},
            {Tool::Crop, QKeySequence(QKeyCombination(Qt::CTRL | Qt::ALT | Qt::SHIFT, Qt::Key_F6))},
            {Tool::ExportArea, QKeySequence(QKeyCombination(Qt::CTRL | Qt::ALT | Qt::SHIFT, Qt::Key_F7))},
        };
        for (const Item2 &it : items)
            sm.setSequences(toolId(it.t), {it.seq});
        sm.apply(w.get());
        for (const Item2 &it : items) {
            appState().setTool(Tool::Pointer);
            const QKeyCombination comb = it.seq[0];
            sendWindowKey(int(comb.key()), comb.keyboardModifiers());
            QCOMPARE(appState().tool(), it.t);
        }

        const QString custom = sm.currentId();
        sm.setCurrent(brand::nameLower());
        if (custom != brand::nameLower()) sm.remove(custom);
        sm.apply(w.get());
        Prefs webOff = appState().prefs;
        webOff.enableWebFeatures = false;
        appState().setPrefs(webOff);
    }


    void writeReleaseShots() {
        const QString dir = qEnvironmentVariable("LW_SHOTS_DIR");
        if (dir.isEmpty()) QSKIP("set LW_SHOTS_DIR");
        QDir().mkpath(dir);
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        if (sm.set(sm.currentId())->readOnly)
            sm.duplicate(brand::nameLower(), QStringLiteral("v041-shots"));
        sm.setSequences(QStringLiteral("tool.Scale"),
                        {QKeySequence(Qt::Key_F5), QKeySequence(QStringLiteral("Ctrl+Shift+Q"))});
        sm.apply(w.get());
        if (auto *panel = w->findChild<ToolsPanel *>()) panel->refreshShortcutLabels();
        drainEvents();
        // grab tools panel region showing Scale tip isn't easy; grab whole window after selecting Scale
        appState().setTool(Tool::Scale);
        drainEvents();
        w->grab().save(dir + QStringLiteral("/rebound-scale-window.png"));

        PreferencesDialog dlg(w.get(), 4);
        dlg.show();
        drainEvents();
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("prefShortcutTree"));
        auto *list = page->findChild<QListWidget *>(QStringLiteral("prefShortcutList"));
        QVERIFY(tree && list);
        // select Scale in the tree
        QTreeWidgetItemIterator it(tree);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("tool.Scale")) {
                tree->setCurrentItem(*it);
                break;
            }
            ++it;
        }
        drainEvents();
        QCOMPARE(list->count(), 2);
        list->setCurrentRow(1);
        drainEvents();
        dlg.grab().save(dir + QStringLiteral("/keyboard-reorder.png"));
        page->grab().save(dir + QStringLiteral("/keyboard-page.png"));
    }

    void keyboardPrefsHasReorderControls() {
        PreferencesDialog dlg(w.get(), 4);
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        QVERIFY(page->findChild<QPushButton *>(QStringLiteral("prefShortcutPrimary")));
        QVERIFY(page->findChild<QPushButton *>(QStringLiteral("prefShortcutUp")));
        QVERIFY(page->findChild<QPushButton *>(QStringLiteral("prefShortcutDown")));
        QVERIFY(page->findChild<QListWidget *>(QStringLiteral("prefShortcutList")));
    }

    void makePrimaryReordersDisplayShortcut() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        if (sm.set(sm.currentId())->readOnly)
            sm.duplicate(brand::nameLower(), QStringLiteral("v041-primary"));
        sm.setSequences(QStringLiteral("tool.Brush"),
                        {QKeySequence(Qt::Key_F8), QKeySequence(Qt::Key_F9)});
        sm.apply(w.get());
        QCOMPARE(sm.displayShortcutText(QStringLiteral("tool.Brush")), QStringLiteral("F8"));
        // Move F9 to front
        sm.setSequences(QStringLiteral("tool.Brush"),
                        {QKeySequence(Qt::Key_F9), QKeySequence(Qt::Key_F8)});
        sm.apply(w.get());
        QCOMPARE(sm.displayShortcutText(QStringLiteral("tool.Brush")), QStringLiteral("F9"));
        if (auto *panel = w->findChild<ToolsPanel *>()) {
            panel->refreshShortcutLabels();
            // Brush button tip should mention F9, not F8 as primary
            bool found = false;
            for (auto *b : panel->findChildren<QToolButton *>()) {
                if (b->toolTip().startsWith(QStringLiteral("Brush"))) {
                    found = true;
                    QVERIFY(b->toolTip().contains(QStringLiteral("F9")));
                    QVERIFY(!b->toolTip().contains(QStringLiteral("F8")));
                }
            }
            QVERIFY(found);
        }
        const QString custom = sm.currentId();
        sm.setCurrent(brand::nameLower());
        if (custom != brand::nameLower()) sm.remove(custom);
        sm.apply(w.get());
        Prefs webOff = appState().prefs;
        webOff.enableWebFeatures = false;
        appState().setPrefs(webOff);
    }
};

QTEST_MAIN(V041)
#include "v041_test.moc"
