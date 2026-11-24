// v0.3.14: Keyboard prefs command tree — only Tools expanded by default
#include "ui/AppState.h"
#include "ui/MainWindow.h"
#include "ui/Shortcuts.h"
#include "ui/KeyboardPrefs.h"
#include "ui/PreferencesDialog.h"
#include "core/Document.h"
#include "Brand.h"
#include <QApplication>
#include <QDir>
#include <functional>
#include <QSettings>
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

class V044 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;

    static QTreeWidgetItem *topNamed(QTreeWidget *tree, const QString &name) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == name) return tree->topLevelItem(i);
        return nullptr;
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
        w->addDocument(Document::createNew(120, 90, Qt::white));
        drainEvents();
    }
    void cleanupTestCase() { w.reset(); }

    void onlyToolsExpandedOnOpen() {
        PreferencesDialog dlg(w.get(), 4);
        dlg.resize(1000, 700);
        dlg.show();
        drainEvents();
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("prefShortcutTree"));
        QVERIFY(tree);

        auto *menu = topNamed(tree, QStringLiteral("Menu Commands"));
        auto *tools = topNamed(tree, QStringLiteral("Tools"));
        auto *misc = topNamed(tree, QStringLiteral("Miscellaneous"));
        auto *mods = topNamed(tree, QStringLiteral("Modifiers (not editable)"));
        QVERIFY(menu && tools && misc && mods);

        QVERIFY(!menu->isExpanded());
        QVERIFY(tools->isExpanded());
        QVERIFY(!misc->isExpanded());
        QVERIFY(!mods->isExpanded());

        // Nested Menu Commands submenus must also be collapsed.
        std::function<void(QTreeWidgetItem *)> assertCollapsed = [&](QTreeWidgetItem *it) {
            for (int i = 0; i < it->childCount(); ++i) {
                QTreeWidgetItem *c = it->child(i);
                if (c->childCount() > 0) {
                    QVERIFY2(!c->isExpanded(), qPrintable(c->text(0)));
                    assertCollapsed(c);
                }
            }
        };
        assertCollapsed(menu);

        // Selection / scroll at top of Tools
        QVERIFY(tree->currentItem());
        QVERIFY(tree->currentItem() == tools || tree->currentItem()->parent() == tools);

        const QString dir = qEnvironmentVariable("LW_SHOTS_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            dlg.grab().save(dir + QStringLiteral("/keyboard-collapsed.png"));
        }
    }

    void setSwitchKeepsDefaultUntilUserExpands() {
        PreferencesDialog dlg(w.get(), 4);
        drainEvents();
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("prefShortcutTree"));
        QVERIFY(page && tree);
        auto *menu = topNamed(tree, QStringLiteral("Menu Commands"));
        auto *tools = topNamed(tree, QStringLiteral("Tools"));
        QVERIFY(menu && tools);
        QVERIFY(!menu->isExpanded());
        QVERIFY(tools->isExpanded());

        // User expands Menu Commands — session remembers it across refill.
        tree->expandItem(menu);
        drainEvents();
        QVERIFY(menu->isExpanded());
        page->refillTree();
        drainEvents();
        menu = topNamed(tree, QStringLiteral("Menu Commands"));
        tools = topNamed(tree, QStringLiteral("Tools"));
        QVERIFY(menu && tools);
        QVERIFY(menu->isExpanded());
        QVERIFY(tools->isExpanded());
    }
};

QTEST_MAIN(V044)
#include "v044_test.moc"
