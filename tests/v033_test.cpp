// v0.3.3 tests: the Layers panel with deeply nested groups. The eye / lock columns do not indent;
// the tree decoration (indentation and expand arrow) lives in the name column.
#include "core/Document.h"
#include "ui/AppState.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/Panels.h"
#include <QApplication>
#include <QHeaderView>
#include <QScrollBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QtTest>
#include <functional>
#include <memory>

static void drainEvents() {
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

static VObject shapeObj(Document *d, ObjType t, const QRectF &r, const QColor &fill, const QString &name) {
    VObject o;
    o.type = t;
    o.rect = r;
    o.fillOn = true;
    o.fill = fill;
    o.id = d->newId();
    o.name = name;
    return o;
}

struct Nested {
    QVector<quint64> groups;   // outermost first (5 levels)
    quint64 maskedGroup = 0;
};

// a layer holding 5 nested groups, each with a shape beside the next group; the 3rd level has a mask,
// plus a second layer with one object
static Nested buildNested(Document *d) {
    Nested n;
    const QColor cols[] = {QColor(220, 60, 50), QColor(40, 110, 220), QColor(250, 190, 40), QColor(60, 160, 90), QColor(150, 70, 200)};
    VObject leaf = shapeObj(d, ObjType::Ellipse, QRectF(150, 100, 40, 40), QColor(20, 20, 20), QStringLiteral("Innermost dot with a long name"));
    d->addObject(leaf, 0, false);
    quint64 inner = leaf.id;
    for (int lvl = 5; lvl >= 1; --lvl) {
        VObject s = shapeObj(d, lvl % 2 ? ObjType::Rect : ObjType::Ellipse, QRectF(20 + lvl * 20, 20 + lvl * 15, 120, 80), cols[lvl - 1],
                             QStringLiteral("Shape at level %1").arg(lvl));
        d->addObject(s, 0, false);
        d->setSelection({inner, s.id});
        const quint64 g = d->groupSelection();
        Q_ASSERT(g);
        d->find(g)->name = QStringLiteral("Group level %1").arg(lvl);
        if (lvl == 3) {
            VObject m = shapeObj(d, ObjType::Ellipse, QRectF(40, 40, 220, 160), Qt::black, QStringLiteral("mask"));
            d->setSelection({g});
            const quint64 mg = d->pasteAsMask(m);
            Q_ASSERT(mg);
            n.maskedGroup = mg;
            inner = mg;
        } else {
            inner = g;
        }
        n.groups.prepend(inner);
    }
    const int l2 = d->addLayer(QStringLiteral("Top layer"));
    d->addObject(shapeObj(d, ObjType::Rect, QRectF(250, 200, 60, 40), QColor(90, 90, 90), QStringLiteral("Button")), l2, false);
    d->clearSelection();
    d->commit(QStringLiteral("setup"));
    return n;
}

static QTreeWidgetItem *itemFor(QTreeWidget *t, quint64 id, int kind = -1) {
    for (QTreeWidgetItemIterator it(t); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toULongLong() == id && (kind < 0 || (*it)->data(0, Qt::UserRole + 1).toInt() == kind)) return *it;
    return nullptr;
}
static int depthOf(QTreeWidgetItem *it) { int d = 0; while ((it = it->parent())) ++d; return d; }
static void click(QTreeWidget *t, QPoint p) {
    QTest::mouseClick(t->viewport(), Qt::LeftButton, {}, p);
    drainEvents();
}

struct Setup {
    std::unique_ptr<MainWindow> w;
    Document *d = nullptr;
    QTreeWidget *tree = nullptr;
    LayersPanel *panel = nullptr;
    Nested n;
};
static void makeSetup(Setup &s) {
    s.w = std::make_unique<MainWindow>();
    s.w->resize(1024, 688);
    s.w->show();
    QVERIFY(QTest::qWaitForWindowExposed(s.w.get()));
    DocumentView *v = s.w->addDocument(Document::createNew(320, 240, QColor(Qt::white)));
    drainEvents();
    s.d = v->doc();
    for (const char *pn : {"Optimize", "Frames"}) if (auto *cp = s.w->panel(QLatin1String(pn))) cp->setCollapsed(true);
    appState().setTool(Tool::Pointer);
    s.n = buildNested(s.d);
    drainEvents();
    s.panel = s.w->findChild<LayersPanel *>();
    QVERIFY(s.panel);
    s.tree = s.panel->findChild<QTreeWidget *>();
    QVERIFY(s.tree);
    s.tree->expandAll();
    drainEvents();
}

class V033 : public QObject {
    Q_OBJECT
private slots:
    void screenshot() {
        const QString file = qEnvironmentVariable("LW_LAYERS_SHOT");
        if (file.isEmpty()) QSKIP("set LW_LAYERS_SHOT to write the nested-groups Layers panel screenshot");
        Setup s;
        makeSetup(s);
        s.d->setSelection({s.n.groups[4]});
        drainEvents();
        s.tree->expandAll();
        drainEvents();
        QWidget *box = s.panel->parentWidget() ? s.panel->parentWidget() : s.panel;
        QVERIFY(box->grab().save(file));
    }

    void nestedRowsGeometry() {
        Setup s;
        makeSetup(s);
        QTreeWidget *t = s.tree;
        QCOMPARE(t->treePosition(), 2);
        const QRect vp = t->viewport()->rect();
        int rows = 0, maxDepth = 0;
        for (QTreeWidgetItemIterator iter(t); *iter; ++iter) {
            QTreeWidgetItem *it = *iter;
            ++rows;
            maxDepth = qMax(maxDepth, depthOf(it));
            t->scrollToItem(it);
            drainEvents();
            const QModelIndex i0 = t->indexFromItem(it, 0), i1 = t->indexFromItem(it, 1), i2 = t->indexFromItem(it, 2);
            const int off = t->horizontalScrollBar()->value();
            for (int c : {0, 1}) {
                const QRect r = t->visualRect(c == 0 ? i0 : i1);
                const QRect sec(t->header()->sectionViewportPosition(c), r.top(), t->header()->sectionSize(c), r.height());
                QVERIFY2(r.width() >= 16, qPrintable(QStringLiteral("%1 col %2 width %3").arg(it->text(2)).arg(c).arg(r.width())));
                QVERIFY2(sec.contains(r), qPrintable(it->text(2)));
                QVERIFY2(vp.contains(r), qPrintable(it->text(2)));
                Q_UNUSED(off);
            }
            const QRect nameR = t->visualRect(i2);
            const int sec2 = t->header()->sectionViewportPosition(2);
            const QRect sec2r(sec2, nameR.top(), t->header()->sectionSize(2), nameR.height());
            QVERIFY2(sec2r.contains(nameR), qPrintable(it->text(2)));
            QVERIFY2(nameR.width() >= 60, qPrintable(QStringLiteral("%1: name width %2").arg(it->text(2)).arg(nameR.width())));
            if (it->childCount()) {
                const QRect arrow(nameR.left() - t->indentation(), nameR.top(), t->indentation(), nameR.height());
                QVERIFY2(sec2r.contains(arrow), qPrintable(it->text(2)));
                QVERIFY2(vp.contains(arrow), qPrintable(QStringLiteral("%1 arrow %2,%3 vp %4").arg(it->text(2)).arg(arrow.left()).arg(arrow.right()).arg(vp.width())));
            }
        }
        QVERIFY(maxDepth >= 6);   // layer > 5 groups > members
        QVERIFY(rows >= 14);
        // at the default panel width nothing needs horizontal scrolling
        QCOMPARE(t->horizontalScrollBar()->value(), 0);
    }

    void eyeAndLockClicks() {
        Setup s;
        makeSetup(s);
        QTreeWidget *t = s.tree;
        Document *d = s.d;
        // snapshot of every visible / locked flag (layers, all nested objects, masks)
        auto state = [&] {
            QMap<QString, bool> m;
            for (int li = 0; li < d->s.layers.size(); ++li) {
                m[QStringLiteral("L%1v").arg(li)] = d->s.layers[li].visible;
                m[QStringLiteral("L%1l").arg(li)] = d->s.layers[li].locked;
                for (const VObject &o : d->objects(li))
                    o.forEachNested([&](const VObject &x) {
                        m[QStringLiteral("%1v").arg(x.id)] = x.visible;
                        m[QStringLiteral("%1l").arg(x.id)] = x.locked;
                        if (x.type == ObjType::Group) {
                            m[QStringLiteral("%1me").arg(x.id)] = x.maskEnabled;
                            m[QStringLiteral("%1ml").arg(x.id)] = x.maskLinked;
                        }
                    });
            }
            return m;
        };
        struct Row { quint64 id; int kind; int layer; quint64 owner; };
        QVector<Row> rowsList;
        for (QTreeWidgetItemIterator iter(t); *iter; ++iter)
            rowsList.append({(*iter)->data(0, Qt::UserRole).toULongLong(), (*iter)->parent() ? (*iter)->data(0, Qt::UserRole + 1).toInt() : -1,
                             (*iter)->data(1, Qt::UserRole).toInt(), (*iter)->data(0, Qt::UserRole + 2).toULongLong()});
        int maskRows = 0, deepRows = 0;
        for (const Row &r : rowsList) {
            for (int col : {0, 1}) {
                QTreeWidgetItem *it = r.kind < 0 ? itemFor(t, r.id) : itemFor(t, r.id, r.kind);
                QVERIFY(it);
                if (depthOf(it) >= 5) ++deepRows;
                t->scrollToItem(it);
                drainEvents();
                const QRect rc = t->visualRect(t->indexFromItem(it, col));
                const auto before = state();
                click(t, rc.center());
                const auto after = state();
                QString key;
                if (r.kind < 0) key = QStringLiteral("L%1%2").arg(r.layer).arg(col == 0 ? QLatin1Char('v') : QLatin1Char('l'));
                else if (r.kind == 2) { key = QStringLiteral("%1%2").arg(r.owner).arg(col == 0 ? QStringLiteral("me") : QStringLiteral("ml")); ++maskRows; }
                else key = QStringLiteral("%1%2").arg(r.id).arg(col == 0 ? QLatin1Char('v') : QLatin1Char('l'));
                QVERIFY2(before.contains(key), qPrintable(key));
                for (auto k = before.cbegin(); k != before.cend(); ++k) {
                    if (k.key() == key) QVERIFY2(after.value(k.key()) != k.value(), qPrintable(QStringLiteral("not toggled: ") + key));
                    else QVERIFY2(after.value(k.key()) == k.value(), qPrintable(QStringLiteral("%1 changed when clicking %2").arg(k.key(), key)));
                }
                // the icon follows: eye shown iff visible / enabled
                it = r.kind < 0 ? itemFor(t, r.id) : itemFor(t, r.id, r.kind);
                QVERIFY(it);
                if (col == 0) QCOMPARE(!it->icon(0).isNull(), after.value(key));
                click(t, t->visualRect(t->indexFromItem(it, col)).center());   // and back
                QCOMPARE(state(), before);
            }
        }
        QVERIFY(maskRows >= 2);
        QVERIFY(deepRows > 0);
    }

    void arrowsAndKeyboard() {
        Setup s;
        makeSetup(s);
        QTreeWidget *t = s.tree;
        t->collapseAll();
        drainEvents();
        // open the tree level by level by clicking the arrows, down to the innermost group
        QVector<quint64> path;
        path.append(s.d->s.layers[0].id);
        for (quint64 g : s.n.groups) path.append(g);
        const auto selBefore = s.d->selection;
        for (int i = 0; i < path.size(); ++i) {
            QTreeWidgetItem *it = i == 0 ? itemFor(t, path[i]) : itemFor(t, path[i], 1);
            QVERIFY2(it, qPrintable(QString::number(i)));
            QVERIFY(it->childCount() > 0);
            QVERIFY2(!it->isExpanded(), qPrintable(it->text(2)));
            t->scrollToItem(it);
            drainEvents();
            const QRect nameR = t->visualRect(t->indexFromItem(it, 2));
            click(t, QPoint(nameR.left() - t->indentation() / 2, nameR.center().y()));
            it = i == 0 ? itemFor(t, path[i]) : itemFor(t, path[i], 1);
            QVERIFY2(it->isExpanded(), qPrintable(it->text(2)));
        }
        QCOMPARE(s.d->selection, selBefore);   // the arrows only expand
        // the expanded state survives a rebuild
        s.d->setSelection({});
        s.d->commit(QStringLiteral("x"));
        drainEvents();
        for (int i = 1; i < path.size(); ++i) QVERIFY(itemFor(t, path[i], 1)->isExpanded());
        // keyboard: Left collapses, Right expands, at depth 5
        QTreeWidgetItem *deep = itemFor(t, s.n.groups[4], 1);
        t->setFocus();
        t->setCurrentItem(deep, 2);
        drainEvents();
        deep = itemFor(t, s.n.groups[4], 1);
        t->setCurrentItem(deep, 2);
        QTest::keyClick(t, Qt::Key_Left);
        QVERIFY(!deep->isExpanded());
        QTest::keyClick(t, Qt::Key_Right);
        QVERIFY(deep->isExpanded());
        t->setCurrentItem(deep, 0);   // also from the eye column
        QTest::keyClick(t, Qt::Key_Left);
        QVERIFY(!deep->isExpanded());
        QTest::keyClick(t, Qt::Key_Plus);
        QVERIFY(deep->isExpanded());
        QTest::keyClick(t, Qt::Key_Minus);
        QVERIFY(!deep->isExpanded());
    }

    void narrowPanelScrolls() {
        Setup s;
        makeSetup(s);
        QTreeWidget *t = s.tree;
        // a very narrow tree: the name column keeps a usable width and the view scrolls sideways
        t->setFixedWidth(150);
        drainEvents();
        QTreeWidgetItem *deep = itemFor(t, s.n.groups[4], 1);
        QVERIFY(deep);
        QVERIFY(t->header()->sectionSize(2) >= t->indentation() * 7 + 60);
        QVERIFY(t->horizontalScrollBar()->maximum() > 0);
        QCOMPARE(t->textElideMode(), Qt::ElideRight);
        t->setMinimumWidth(0);
        t->setMaximumWidth(QWIDGETSIZE_MAX);
        // wide: the name column fills the rest, no scroll bar
        t->setFixedWidth(400);
        drainEvents();
        QCOMPARE(t->horizontalScrollBar()->maximum(), 0);
        QCOMPARE(t->header()->sectionSize(0) + t->header()->sectionSize(1) + t->header()->sectionSize(2), t->viewport()->width());
    }
};

QTEST_MAIN(V033)
#include "v033_test.moc"
