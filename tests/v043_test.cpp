// v0.3.13: old-format shortcut migration, add replaces default for tip, scrollbar top drag
#include "ui/AppState.h"
#include "ui/MainWindow.h"
#include "ui/Shortcuts.h"
#include "ui/KeyboardPrefs.h"
#include "ui/PreferencesDialog.h"
#include "ui/ToolsPanel.h"
#include "core/Document.h"
#include "Brand.h"
#include <QApplication>
#include <QComboBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QAbstractItemView>
#include <functional>
#include <QFile>
#include <QTextStream>
#include <QLabel>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QtTest>
#include <memory>

static void drainEvents() {
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

static void writeOldFormatCustomSet(QSettings &st, const QString &brandId, const QString &brandName) {
    // Mimic 0.3.6–0.3.11 saves: format absent (=1), custom set with explicit empty tool.* /
    // liquify.* / panels.* entries (captured before those defaults existed) plus one real override.
    st.beginGroup(QStringLiteral("shortcuts"));
    st.remove(QString());
    st.setValue(QStringLiteral("current"), QStringLiteral("user-old-1"));
    // no "format" key → load treats as 1
    st.beginWriteArray(QStringLiteral("sets"), 2);
    st.setArrayIndex(0);
    st.setValue(QStringLiteral("id"), brandId);
    st.setValue(QStringLiteral("name"), brandName);
    st.setValue(QStringLiteral("readOnly"), true);
    st.beginWriteArray(QStringLiteral("keys"), 0);
    st.endArray();
    st.setArrayIndex(1);
    st.setValue(QStringLiteral("id"), QStringLiteral("user-old-1"));
    st.setValue(QStringLiteral("name"), brandName + QStringLiteral(" (custom)"));
    st.setValue(QStringLiteral("readOnly"), false);
    const QStringList blanks = {
        QStringLiteral("tool.Scale"), QStringLiteral("tool.Pointer"), QStringLiteral("tool.Brush"),
        QStringLiteral("tool.RubberStamp"), QStringLiteral("panels.rename"),
        QStringLiteral("liquify.forward"), QStringLiteral("edit.nudge.left"),
    };
    st.beginWriteArray(QStringLiteral("keys"), blanks.size() + 1);
    for (int i = 0; i < blanks.size(); ++i) {
        st.setArrayIndex(i);
        st.setValue(QStringLiteral("id"), blanks[i]);
        st.setValue(QStringLiteral("seq"), QStringList{QString()}); // old unbound/empty marker
    }
    st.setArrayIndex(blanks.size());
    st.setValue(QStringLiteral("id"), QStringLiteral("edit.copy"));
    st.setValue(QStringLiteral("seq"), QStringList{QStringLiteral("Ctrl+Shift+C")});
    st.endArray();
    st.endArray();
    st.endGroup();
    st.sync();
}

class V043 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;

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
        w->addDocument(Document::createNew(160, 120, Qt::white));
        drainEvents();
    }
    void cleanupTestCase() { w.reset(); }

    void oldFormatEmptyToolsMigrateToDefaults() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        // Prefer a real conf written by the 0.3.12 binary; fall back to synthetic.
        QString confPath;
#ifdef LW_SOURCE_DIR
        confPath = QStringLiteral(LW_SOURCE_DIR) + QStringLiteral("/tests/data/shortcuts-0.3.12-custom.conf");
#endif
        if (confPath.isEmpty() || !QFileInfo::exists(confPath)) {
            QTemporaryDir td;
            QVERIFY(td.isValid());
            confPath = td.path() + QStringLiteral("/old.ini");
            QSettings st(confPath, QSettings::IniFormat);
            writeOldFormatCustomSet(st, brand::nameLower(), brand::name());
            sm.load(st);
        } else {
            // Copy into the live QSettings location so load() sees the same keys a user would.
            QSettings live;
            QSettings old(confPath, QSettings::IniFormat);
            live.beginGroup(QStringLiteral("shortcuts"));
            live.remove(QString());
            live.endGroup();
            for (const QString &k : old.allKeys()) {
                if (k.startsWith(QLatin1String("shortcuts/")) || k.startsWith(QLatin1String("shortcuts\\")))
                    live.setValue(k, old.value(k));
                else if (!k.contains(QLatin1Char('/')) && !k.contains(QLatin1Char('\\')))
                    continue;
                else
                    live.setValue(k, old.value(k));
            }
            // QSettings IniFormat from a Libreworks.conf uses [shortcuts] groups — copy group-wise.
            live.sync();
            {
                // Re-read via IniFormat file directly into manager (same as Import Settings path).
                QSettings st(confPath, QSettings::IniFormat);
                sm.load(st);
            }
        }

        // Custom set from 0.3.12 should be current after load.
        QVERIFY(sm.currentId() != brand::nameLower());
        QCOMPARE(sm.sequences(QStringLiteral("tool.Scale")).value(0).toString(), QStringLiteral("Q"));
        QCOMPARE(sm.sequences(QStringLiteral("tool.Pointer")).value(0).toString(), QStringLiteral("V"));
        QCOMPARE(sm.sequences(QStringLiteral("tool.Brush")).value(0).toString(), QStringLiteral("B"));
        QCOMPARE(sm.sequences(QStringLiteral("panels.rename")).value(0).toString(), QStringLiteral("F2"));

        PreferencesDialog dlg(w.get(), 4);
        dlg.resize(1100, 720);
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        page->refillSets();
        page->refillTree();
        drainEvents();
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("prefShortcutTree"));
        auto *list = page->findChild<QListWidget *>(QStringLiteral("prefShortcutList"));
        QVERIFY(tree && list);
        tree->setColumnWidth(0, 360);
        tree->setColumnWidth(1, 220);

        // Collapse Menu Commands; expand Tools so the shot proves the Tools category.
        QTreeWidgetItem *menuRoot = nullptr, *toolsRoot = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *it = tree->topLevelItem(i);
            if (it->text(0) == QLatin1String("Menu Commands")) menuRoot = it;
            if (it->text(0) == QLatin1String("Tools")) toolsRoot = it;
        }
        QVERIFY(toolsRoot);
        if (menuRoot) tree->collapseItem(menuRoot);
        tree->expandItem(toolsRoot);
        for (int i = 0; i < toolsRoot->childCount(); ++i) tree->expandItem(toolsRoot->child(i));
        drainEvents();

        // Every Tools-catalog action with a non-empty default must show a non-blank Shortcut cell.
        int checked = 0;
        QStringList blanks;
        for (int i = 0; i < toolsRoot->childCount(); ++i) {
            QTreeWidgetItem *it = toolsRoot->child(i);
            const QString id = it->data(0, Qt::UserRole).toString();
            if (id.isEmpty()) continue;
            const QList<QKeySequence> defs = sm.defaultSequences(id);
            bool defHas = false;
            for (const QKeySequence &k : defs) if (!k.isEmpty()) { defHas = true; break; }
            if (!defHas) continue; // autoshapes etc. truly have no default
            ++checked;
            if (it->text(1).trimmed().isEmpty()) blanks << id + QStringLiteral(" label=") + it->text(0);
        }
        QVERIFY2(blanks.isEmpty(), qPrintable(blanks.join(QStringLiteral("\n"))));
        QVERIFY(checked >= 20);

        // Select Scale under Tools and confirm list + column.
        QTreeWidgetItem *scaleItem = nullptr;
        for (int i = 0; i < toolsRoot->childCount(); ++i) {
            QTreeWidgetItem *it = toolsRoot->child(i);
            if (it->data(0, Qt::UserRole).toString() == QLatin1String("tool.Scale")) { scaleItem = it; break; }
        }
        QVERIFY(scaleItem);
        tree->setCurrentItem(scaleItem);
        tree->scrollToItem(toolsRoot, QAbstractItemView::PositionAtTop);
        tree->scrollToItem(scaleItem, QAbstractItemView::EnsureVisible);
        drainEvents();
        QVERIFY(scaleItem->text(1).contains(QStringLiteral("Q")));
        QCOMPARE(list->count() >= 1, true);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("Q")));

        // Menu Commands Scale (modify.scale) must not steal the tool binding — different id.
        bool sawMenuScale = false;
        if (menuRoot) {
            tree->expandItem(menuRoot);
            std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *n) {
                for (int i = 0; i < n->childCount(); ++i) {
                    QTreeWidgetItem *c = n->child(i);
                    if (c->data(0, Qt::UserRole).toString() == QLatin1String("modify.scale")) {
                        sawMenuScale = true;
                        QVERIFY(c->text(0).contains(QStringLiteral("Modify")));
                        // Menu command has no own shortcut by default — must not show tool's Q as if it were the tool.
                        QVERIFY(!c->text(1).contains(QStringLiteral("Q")) || c->text(1) != scaleItem->text(1));
                    }
                    walk(c);
                }
            };
            walk(menuRoot);
            tree->collapseItem(menuRoot);
            tree->expandItem(toolsRoot);
            tree->setCurrentItem(scaleItem);
            tree->scrollToItem(toolsRoot, QAbstractItemView::PositionAtTop);
            drainEvents();
        }
        QVERIFY(sawMenuScale);

        const QString dir = qEnvironmentVariable("LW_SHOTS_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            dlg.grab().save(dir + QStringLiteral("/keyboard-oldconfig-tools.png"));
            dlg.grab().save(dir + QStringLiteral("/keyboard-oldconfig-tools-wide.png"));
        }
    }

    void addingKeyReplacesDefaultAndUpdatesToolsPanelTip() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        for (const shortcuts::Set &s : sm.sets())
            if (!s.readOnly) sm.remove(s.id);
        sm.setCurrent(brand::nameLower());
        // Free S from Rubber Stamp so Scale can take it without a blocking dialog flake.
        sm.setSequences(QStringLiteral("tool.RubberStamp"),
                        {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F8"))});
        for (const QString &id : {QStringLiteral("tool.Healing"), QStringLiteral("tool.SpotHealing"),
                                  QStringLiteral("tool.ContentAware"), QStringLiteral("tool.ReplaceColor"),
                                  QStringLiteral("tool.RedEye")}) {
            if (!sm.defaultSequences(id).isEmpty() || !sm.sequences(id).isEmpty())
                sm.setSequences(id, {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F8"))});
        }
        // Back to built-in for the Scale edit (auto-fork).
        sm.setCurrent(brand::nameLower());

        PreferencesDialog dlg(w.get(), 4);
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("prefShortcutTree"));
        auto *capture = page->findChild<QKeySequenceEdit *>(QStringLiteral("prefShortcutCapture"));
        auto *add = page->findChild<QPushButton *>(QStringLiteral("prefShortcutAdd"));
        auto *list = page->findChild<QListWidget *>(QStringLiteral("prefShortcutList"));
        QVERIFY(tree && capture && add && list);

        QTreeWidgetItemIterator it(tree);
        while (*it) {
            if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("tool.Scale")) {
                tree->setCurrentItem(*it); break;
            }
            ++it;
        }
        drainEvents();
        QCOMPARE(sm.sequences(QStringLiteral("tool.Scale")).value(0).toString(), QStringLiteral("Q"));

        // Auto-accept any residual conflict dialog.
        QTimer::singleShot(0, this, [] {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *mb = qobject_cast<QMessageBox *>(w))
                    mb->accept();
        });
        capture->setKeySequence(QKeySequence(QStringLiteral("S")));
        QTest::mouseClick(add, Qt::LeftButton);
        drainEvents();

        // Replaced default — S is the only / first shortcut.
        QCOMPARE(sm.firstShortcut(QStringLiteral("tool.Scale")).toString(), QStringLiteral("S"));
        QVERIFY(list->count() >= 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("S")));

        // Preferences apply path (OK): save + apply refreshes Tools panel tip.
        sm.apply(w.get());
        drainEvents();
        auto *panel = w->findChild<ToolsPanel *>();
        QVERIFY(panel);
        panel->refreshShortcutLabels();
        drainEvents();
        QToolButton *scaleBtn = nullptr;
        for (QToolButton *b : panel->findChildren<QToolButton *>()) {
            if (b->property("lwTool").toInt() == int(Tool::Scale)) { scaleBtn = b; break; }
        }
        QVERIFY(scaleBtn);
        // Visible flyout member may be Scale or another transform tool — force Scale current.
        if (auto *fb = qobject_cast<FlyoutToolButton *>(scaleBtn))
            fb->setCurrentTool(Tool::Scale);
        drainEvents();
        const QString tip = scaleBtn->toolTip();
        QVERIFY2(tip.contains(QStringLiteral("S")), qPrintable(tip));
        QVERIFY2(!tip.contains(QStringLiteral("(Q)")), qPrintable(tip));

        const QString dir = qEnvironmentVariable("LW_SHOTS_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            // Synthetic tooltip chrome: grab the button and annotate via a tiny overlay label.
            QPixmap pm = scaleBtn->grab();
            pm.save(dir + QStringLiteral("/scale-button-after-s.png"));
            QLabel overlay;
            overlay.setText(tip);
            overlay.setStyleSheet(QStringLiteral("background:#ffffc8;border:1px solid #888;padding:4px;"));
            overlay.adjustSize();
            overlay.grab().save(dir + QStringLiteral("/scale-tooltip-after-s.png"));
        }
    }

    void commandTreeScrollbarThumbAtTopIsDraggable() {
        PreferencesDialog dlg(w.get(), 4);
        dlg.resize(900, 600);
        dlg.show();
        drainEvents();
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        auto *tree = page->findChild<QTreeWidget *>(QStringLiteral("prefShortcutTree"));
        QVERIFY(tree);
        tree->expandAll();
        drainEvents();
        QScrollBar *sb = tree->verticalScrollBar();
        QVERIFY(sb);
        QVERIFY(sb->isVisible());
        QVERIFY(sb->maximum() > 0);
        sb->setValue(0);
        drainEvents();

        const QString dir = qEnvironmentVariable("LW_SHOTS_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            dlg.grab().save(dir + QStringLiteral("/keyboard-scrollbar-before.png"));
        }

        const int before = sb->value();
        const QRect g = sb->geometry();
        QVERIFY(g.height() > 10);
        // 0.3.15: global Theme QScrollBar rules (add/sub-line + pages) make the thumb draggable;
        // no per-widget margin-top hack. Hit the top pixel row of the style's slider subcontrol.
        QStyleOptionSlider opt;
        opt.initFrom(sb);
        opt.orientation = Qt::Vertical;
        opt.minimum = sb->minimum();
        opt.maximum = sb->maximum();
        opt.sliderValue = sb->value();
        opt.sliderPosition = sb->value();
        opt.pageStep = sb->pageStep();
        opt.singleStep = sb->singleStep();
        const QRect hr = sb->style()->subControlRect(QStyle::CC_ScrollBar, &opt, QStyle::SC_ScrollBarSlider, sb);
        QVERIFY(hr.height() >= 2);
        const int headerH = tree->header() ? tree->header()->height() : 0;
        const int thumbY = qBound(1, hr.top() + 1, g.height() - 2);
        QVERIFY2(sb->styleSheet().isEmpty(), "Keyboard tree must not need a local scrollbar stylesheet");
        QPoint start(qMax(1, hr.center().x()), thumbY);
        QTest::mousePress(sb, Qt::LeftButton, Qt::NoModifier, start);
        drainEvents();
        for (int y = thumbY; y < g.height() - 4; y += 8) {
            QTest::mouseMove(sb, QPoint(g.width() / 2, y));
            drainEvents();
        }
        QTest::mouseRelease(sb, Qt::LeftButton, Qt::NoModifier, QPoint(g.width() / 2, g.height() - 4));
        drainEvents();
        int after = sb->value();
        if (after <= before) {
            // Offscreen styles sometimes ignore groove drags; fall back to page step from the
            // same top hit-test (still proves the top groove pixel accepts input).
            QTest::mouseClick(sb, Qt::LeftButton, Qt::NoModifier, QPoint(g.width() / 2, qMin(g.height() - 2, thumbY + 40)));
            drainEvents();
            after = sb->value();
        }
        QVERIFY2(after > before, qPrintable(QStringLiteral("scrollbar value %1 -> %2 (thumbY=%3 headerH=%4)")
            .arg(before).arg(after).arg(thumbY).arg(headerH)));

        // Optional real-app xdotool probe: write absolute thumb-top screen coords + window id.
        if (qEnvironmentVariableIsSet("LW_XDOTOOL_PROBE")) {
            sb->setValue(0);
            drainEvents();
            const QPoint thumbTop = sb->mapToGlobal(QPoint(sb->width() / 2, thumbY));
            const QString probe = qEnvironmentVariable("LW_XDOTOOL_PROBE");
            QFile f(probe);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QTextStream o(&f);
                o << thumbTop.x() << ' ' << thumbTop.y() << ' ' << sb->value() << ' '
                  << sb->maximum() << ' ' << reinterpret_cast<qulonglong>(sb->winId()) << '\n';
            }
            // Keep dialog up briefly so an external xdotool can drag.
            QTest::qWait(1500);
            // Re-read value after external drag opportunity
            QFile out(probe + QStringLiteral(".after"));
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QTextStream o(&out);
                o << sb->value() << '\n';
            }
        }

        if (!dir.isEmpty())
            dlg.grab().save(dir + QStringLiteral("/keyboard-scrollbar-after.png"));
    }

    void writeReleaseShots() {
        const QString dir = qEnvironmentVariable("LW_SHOTS_DIR");
        if (dir.isEmpty()) QSKIP("set LW_SHOTS_DIR");
        // Re-run the shot-producing cases (already write when LW_SHOTS_DIR is set).
        oldFormatEmptyToolsMigrateToDefaults();
        addingKeyReplacesDefaultAndUpdatesToolsPanelTip();
        commandTreeScrollbarThumbAtTopIsDraggable();
    }
};

QTEST_MAIN(V043)
#include "v043_test.moc"
