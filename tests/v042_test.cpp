// v0.3.12: Preferences apply path, defaults listed, auto-fork combo, tool key via MainWindow::event
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
#include <QComboBox>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QDir>
#include <QTreeWidgetItemIterator>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest>
#include <memory>

static void drainEvents() {
    for (int i = 0; i < 6; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

class V042 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;

    void sendKeyToWindow(int key, int mods = 0) {
        // Deliver through MainWindow::event (real ShortcutOverride + KeyPress path)
        QKeyEvent ov(QEvent::ShortcutOverride, key, Qt::KeyboardModifiers(mods));
        QApplication::sendEvent(w.get(), &ov);
        QKeyEvent press(QEvent::KeyPress, key, Qt::KeyboardModifiers(mods));
        QApplication::sendEvent(w.get(), &press);
        drainEvents();
    }

private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        QSettings st;
        st.beginGroup(QStringLiteral("shortcuts"));
        st.remove(QString());
        st.endGroup();
        w = std::make_unique<MainWindow>();
        w->resize(1100, 800);
        w->show();
        drainEvents();
        Document *d = Document::createNew(160, 120, Qt::white);
        w->addDocument(d);
        drainEvents();
    }
    void cleanupTestCase() { w.reset(); }


    void writeReleaseShots() {
        const QString dir = qEnvironmentVariable("LW_SHOTS_DIR");
        if (dir.isEmpty()) QSKIP("set LW_SHOTS_DIR");
        QDir().mkpath(dir);
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        for (const shortcuts::Set &s : sm.sets())
            if (!s.readOnly) sm.remove(s.id);
        sm.setCurrent(brand::nameLower());

        // Shot 1: Keyboard tab showing defaults (Pointer V, Scale Q, Rename F2)
        {
            PreferencesDialog dlg(w.get(), 4);
            dlg.resize(900, 600);
            dlg.show();
            drainEvents();
            auto *page = dlg.findChild<KeyboardPrefsPage *>();
            auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("prefShortcutTree"));
            // select Pointer
            QTreeWidgetItemIterator it(tree);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("tool.Pointer")) {
                    tree->setCurrentItem(*it); break;
                }
                ++it;
            }
            drainEvents();
            dlg.grab().save(dir + QStringLiteral("/keyboard-defaults-pointer.png"));
            // Scale
            it = QTreeWidgetItemIterator(tree);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("tool.Scale")) {
                    tree->setCurrentItem(*it); break;
                }
                ++it;
            }
            drainEvents();
            dlg.grab().save(dir + QStringLiteral("/keyboard-defaults-scale.png"));
            // Rename
            it = QTreeWidgetItemIterator(tree);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("panels.rename")) {
                    tree->setCurrentItem(*it); break;
                }
                ++it;
            }
            drainEvents();
            dlg.grab().save(dir + QStringLiteral("/keyboard-defaults-rename.png"));

            // Auto-fork: add Ctrl+Alt+Shift+F5 to Scale, grab combo without reopening
            auto *capture = page->findChild<QKeySequenceEdit *>(QStringLiteral("prefShortcutCapture"));
            auto *add = page->findChild<QPushButton *>(QStringLiteral("prefShortcutAdd"));
            auto *sets = page->findChild<QComboBox *>(QStringLiteral("prefShortcutSet"));
            it = QTreeWidgetItemIterator(tree);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("tool.Scale")) {
                    tree->setCurrentItem(*it); break;
                }
                ++it;
            }
            drainEvents();
            capture->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F5")));
            // Avoid conflict dialog: clear clash first if any
            const QString clash = sm.conflict(QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F5")), QStringLiteral("tool.Scale"));
            if (!clash.isEmpty()) {
                QList<QKeySequence> o = sm.sequences(clash); o.removeAll(QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F5")));
                sm.setSequences(clash, o);
            }
            QTest::mouseClick(add, Qt::LeftButton);
            drainEvents();
            QVERIFY(sets->currentText().contains(QStringLiteral("custom")));
            dlg.grab().save(dir + QStringLiteral("/keyboard-autofork-combo.png"));
        }

        // Rebind Scale only, apply, grab tools tip
        sm.setSequences(QStringLiteral("tool.Scale"), {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F6"))});
        sm.apply(w.get());
        appState().setTool(Tool::Scale);
        drainEvents();
        if (auto *panel = w->findChild<ToolsPanel *>()) panel->refreshShortcutLabels();
        drainEvents();
        w->grab().save(dir + QStringLiteral("/scale-rebound-window.png"));
        // Confirm key selects Scale from Brush
        appState().setTool(Tool::Brush);
        {
            const auto mods = Qt::KeyboardModifiers(int(Qt::CTRL | Qt::ALT | Qt::SHIFT));
            QKeyEvent ov(QEvent::ShortcutOverride, Qt::Key_F6, mods);
            QApplication::sendEvent(w.get(), &ov);
            QKeyEvent press(QEvent::KeyPress, Qt::Key_F6, mods);
            QApplication::sendEvent(w.get(), &press);
            drainEvents();
        }
        QCOMPARE(appState().tool(), Tool::Scale);
        w->grab().save(dir + QStringLiteral("/scale-after-key.png"));
    }

    void builtinDefaultsListedForToolsAndRename() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        QVERIFY(sm.set(sm.currentId())->readOnly);
        QCOMPARE(sm.sequences(QStringLiteral("tool.Pointer")).value(0).toString(), QStringLiteral("V"));
        QCOMPARE(sm.sequences(QStringLiteral("tool.Brush")).value(0).toString(), QStringLiteral("B"));
        QCOMPARE(sm.sequences(QStringLiteral("tool.Scale")).value(0).toString(), QStringLiteral("Q"));
        QCOMPARE(sm.sequences(QStringLiteral("panels.rename")).value(0).toString(), QStringLiteral("F2"));

        PreferencesDialog dlg(w.get(), 4);
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("prefShortcutTree"));
        auto *list = page->findChild<QListWidget *>(QStringLiteral("prefShortcutList"));
        QVERIFY(tree && list);
        QTreeWidgetItemIterator it(tree);
        bool foundScale = false;
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("tool.Scale")) {
                tree->setCurrentItem(*it);
                drainEvents();
                foundScale = true;
                break;
            }
            ++it;
        }
        QVERIFY(foundScale);
        QCOMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Q")));
    }

    void autoForkUpdatesCurrentSetComboImmediately() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        // wipe custom sets
        sm.setCurrent(brand::nameLower());
        for (const shortcuts::Set &s : sm.sets())
            if (!s.readOnly) sm.remove(s.id);
        sm.setCurrent(brand::nameLower());

        PreferencesDialog dlg(w.get(), 4);
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        auto *sets = page->findChild<QComboBox *>(QStringLiteral("prefShortcutSet"));
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("prefShortcutTree"));
        auto *list = page->findChild<QListWidget *>(QStringLiteral("prefShortcutList"));
        auto *capture = page->findChild<QKeySequenceEdit *>(QStringLiteral("prefShortcutCapture"));
        auto *add = page->findChild<QPushButton *>(QStringLiteral("prefShortcutAdd"));
        auto *ren = page->findChild<QPushButton *>(QStringLiteral("prefShortcutRename"));
        QVERIFY(sets && tree && list && capture && add && ren);

        QCOMPARE(sets->currentData().toString(), brand::nameLower());
        QVERIFY(!ren->isEnabled()); // built-in

        // select Scale
        QTreeWidgetItemIterator it(tree);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("tool.Scale")) {
                tree->setCurrentItem(*it); break;
            }
            ++it;
        }
        drainEvents();
        capture->setKeySequence(QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F5")));
        QTest::mouseClick(add, Qt::LeftButton);
        drainEvents();

        // Combo must already show the forked custom set (no OK/reopen)
        QVERIFY(sets->currentData().toString() != brand::nameLower());
        QVERIFY(sets->currentText().contains(QStringLiteral("custom")));
        QVERIFY(ren->isEnabled());
        QVERIFY(list->count() >= 1);
        const auto seqs = shortcuts::Manager::get().sequences(QStringLiteral("tool.Scale"));
        bool has = false;
        for (const QKeySequence &k : seqs)
            if (k.matches(QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F5"))) == QKeySequence::ExactMatch) has = true;
        QVERIFY(has);
    }

    void prefsApplyPathRebindsScaleAndTooltip() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        for (const shortcuts::Set &s : sm.sets())
            if (!s.readOnly) sm.remove(s.id);

        // Fork via manager (same as Preferences edit), then Preferences OK path: save + apply
        QVERIFY(sm.setSequences(QStringLiteral("tool.Scale"),
                                {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F6"))}));
        PreferencesDialog dlg(w.get(), 4); // constructs Keyboard page against live manager
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        auto *sets = page->findChild<QComboBox *>(QStringLiteral("prefShortcutSet"));
        QVERIFY(sets);
        QVERIFY(sets->currentData().toString() != brand::nameLower());

        QSettings st;
        sm.save(st);
        sm.apply(w.get()); // MainWindow::editPreferences does this on OK
        drainEvents();
        QCOMPARE(sm.firstShortcut(QStringLiteral("tool.Scale")).toString(QKeySequence::PortableText),
                 QStringLiteral("Ctrl+Alt+Shift+F6"));

        appState().setTool(Tool::Brush);
        sendKeyToWindow(Qt::Key_F6, int(Qt::CTRL | Qt::ALT | Qt::SHIFT));
        QCOMPARE(appState().tool(), Tool::Scale);

        if (QAction *a = w->action(QStringLiteral("tool.Scale")))
            QVERIFY(a->toolTip().contains(QStringLiteral("F6")));
        appState().setTool(Tool::Scale);
        drainEvents();
        if (auto *panel = w->findChild<ToolsPanel *>()) {
            panel->refreshShortcutLabels();
            bool ok = false;
            for (auto *b : panel->findChildren<QToolButton *>()) {
                if (b->toolTip().startsWith(QStringLiteral("Scale")) && b->toolTip().contains(QStringLiteral("F6")))
                    ok = true;
            }
            QVERIFY2(ok, "Tools panel tip should mention Scale (...F6)");
        }
    }

};

QTEST_MAIN(V042)
#include "v042_test.moc"
