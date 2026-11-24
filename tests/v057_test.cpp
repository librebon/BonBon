// Select Behind, Vector Path and Redraw Path (0.3.20).
#include "core/Document.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/ToolsPanel.h"
#include "ui/Icons.h"
#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QToolButton>
#include <QtTest>

namespace {
void sendMouse(QWidget *w, QEvent::Type t, QPointF pos, Qt::MouseButton b, Qt::MouseButtons bs)
{
    QMouseEvent ev(t, pos, w->mapToGlobal(pos.toPoint()), b, bs, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}
void drag(Canvas *c, QPointF a, QPointF b)
{
    const QPointF va = c->toView(a), vb = c->toView(b);
    sendMouse(c, QEvent::MouseButtonPress, va, Qt::LeftButton, Qt::LeftButton);
    const int n = 8;
    for (int i = 1; i <= n; ++i) {
        const qreal t = i / qreal(n);
        sendMouse(c, QEvent::MouseMove, va + (vb - va) * t, Qt::LeftButton, Qt::LeftButton);
    }
    sendMouse(c, QEvent::MouseButtonRelease, vb, Qt::LeftButton, Qt::NoButton);
}
void clickDoc(Canvas *c, QPointF a)
{
    const QPointF v = c->toView(a);
    sendMouse(c, QEvent::MouseMove, v, Qt::NoButton, Qt::NoButton);
    sendMouse(c, QEvent::MouseButtonPress, v, Qt::LeftButton, Qt::LeftButton);
    sendMouse(c, QEvent::MouseButtonRelease, v, Qt::LeftButton, Qt::NoButton);
}
VObject box(Document *d, const QRectF &r, const QColor &c)
{
    VObject o;
    o.type = ObjType::Rect;
    o.rect = r;
    o.fillOn = true;
    o.fill = c;
    o.strokeOn = false;
    o.antialias = false;
    o.id = d->newId();
    return o;
}
}

class V057Test : public QObject {
    Q_OBJECT
private slots:
    void namesGroupsShortcuts()
    {
        QCOMPARE(toolName(Tool::SelectBehind), QStringLiteral("Select Behind"));
        QCOMPARE(toolName(Tool::VectorPath), QStringLiteral("Vector Path"));
        QCOMPARE(toolName(Tool::RedrawPath), QStringLiteral("Redraw Path"));
        QVERIFY(toolGroup(Tool::Pointer).contains(Tool::SelectBehind));
        QVERIFY(toolGroup(Tool::Pen).contains(Tool::VectorPath));
        QVERIFY(toolGroup(Tool::Pen).contains(Tool::RedrawPath));
        QCOMPARE(toolShortcut(Tool::SelectBehind), QStringLiteral("V"));
        QCOMPARE(toolShortcut(Tool::VectorPath), QStringLiteral("P"));
        QCOMPARE(toolActionId(Tool::SelectBehind), QStringLiteral("tool.Select Behind"));
        QCOMPARE(toolFromActionId(QStringLiteral("tool.Select Behind")), Tool::SelectBehind);
        QCOMPARE(int(kLastTool), int(Tool::AsHeart));
        QVERIFY(int(Tool::AsHeart) > int(Tool::RedrawPath));
        QVERIFY(int(Tool::SelectBehind) > int(Tool::PolySlice));
        QCOMPARE(toolAutoShapeKind(Tool::AsHeart), QStringLiteral("heart"));
        QCOMPARE(autoShapeTool(QStringLiteral("heart")), Tool::AsHeart);
    }

    void hoverOnlyFlyouts()
    {
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *ptr = w.findChild<FlyoutToolButton *>(QStringLiteral("tool_Pointer"));
        QVERIFY2(ptr, "Pointer should be a flyout (Select Behind) with a chevron");
        QVERIFY(!ptr->chevronOnHover());
        QVERIFY(ptr->flyout());
        QStringList items;
        for (QAction *a : ptr->flyout()->actions()) items << a->objectName();
        QVERIFY(items.contains(QStringLiteral("flyoutItem_SelectBehind")));
        auto *pen = w.findChild<FlyoutToolButton *>(QStringLiteral("tool_Pen"));
        QVERIFY(pen);
        QVERIFY(!pen->chevronOnHover());
        QStringList pitems;
        for (QAction *a : pen->flyout()->actions()) pitems << a->objectName();
        QVERIFY(pitems.contains(QStringLiteral("flyoutItem_VectorPath")));
        QVERIFY(pitems.contains(QStringLiteral("flyoutItem_RedrawPath")));
        auto *blur = w.findChild<FlyoutToolButton *>(QStringLiteral("tool_Blur"));
        QVERIFY(blur);
        QVERIFY(!blur->chevronOnHover());
    }

    void selectBehindCyclesStack()
    {
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        Document *d = Document::createNew(160, 160, Qt::white);
        auto red = box(d, QRectF(10, 10, 80, 80), Qt::red);
        auto green = box(d, QRectF(20, 20, 80, 80), Qt::green);
        auto blue = box(d, QRectF(30, 30, 80, 80), Qt::blue);
        d->addObject(red, 0, false);
        d->addObject(green, 0, false);
        d->addObject(blue, 0, false);
        d->resetHistory(QStringLiteral("setup"));
        DocumentView *v = w.addDocument(d);
        Canvas *c = v->canvas();
        c->setZoom(1);
        QCoreApplication::processEvents();
        QCOMPARE(c->objectsUnder(QPointF(50, 50)).size(), 3);
        QCOMPARE(c->objectsUnder(QPointF(50, 50)).first(), blue.id); // top

        appState().setTool(Tool::SelectBehind);
        QCOMPARE(appState().tool(), Tool::SelectBehind);
        clickDoc(c, QPointF(50, 50));
        QCOMPARE(d->singleSelected() ? d->singleSelected()->id : 0, blue.id);
        clickDoc(c, QPointF(50, 50));
        QCOMPARE(d->singleSelected() ? d->singleSelected()->id : 0, green.id);
        clickDoc(c, QPointF(50, 50));
        QCOMPARE(d->singleSelected() ? d->singleSelected()->id : 0, red.id);
        clickDoc(c, QPointF(50, 50));
        QCOMPARE(d->singleSelected() ? d->singleSelected()->id : 0, blue.id);
    }

    void vectorPathDrawsAndRedraws()
    {
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        Document *d = Document::createNew(200, 160, Qt::white);
        DocumentView *v = w.addDocument(d);
        Canvas *c = v->canvas();
        c->setZoom(1);
        appState().strokeOn = true;
        appState().stroke = Qt::black;
        appState().strokeWidth = 2;
        appState().pathPrecision = 8;
        appState().setTool(Tool::VectorPath);
        drag(c, QPointF(20, 40), QPointF(160, 80));
        QCOMPARE(d->s.frames[0].layers[0].size(), 1);
        const VObject &path = d->s.frames[0].layers[0][0];
        QCOMPARE(path.type, ObjType::Path);
        QVERIFY(path.points.size() >= 2);
        QVERIFY(path.strokeOn);
        const int n0 = path.points.size();
        const QPolygonF orig = path.points;
        d->selectOnly(path.id);
        appState().setTool(Tool::RedrawPath);
        // Redraw the middle of the path upward.
        const QPointF mid = orig[orig.size() / 2];
        drag(c, orig.first(), mid + QPointF(0, -30));
        const VObject *got = d->find(path.id);
        QVERIFY(got);
        QCOMPARE(got->type, ObjType::Path);
        QVERIFY(got->points.size() >= 2);
        // The path should have changed (not an identical point list).
        QVERIFY(got->points != orig || got->points.size() != n0);
        QVERIFY(d->canUndo());
        d->undo();
        QCOMPARE(d->find(path.id)->points, orig);
    }

    void distinctPenFamilyIcons()
    {
        const QSize s(22, 22);
        const QImage pen = icons::tool(Tool::Pen).pixmap(s, 2.0).toImage().convertToFormat(QImage::Format_ARGB32);
        const QImage vec = icons::tool(Tool::VectorPath).pixmap(s, 2.0).toImage().convertToFormat(QImage::Format_ARGB32);
        const QImage red = icons::tool(Tool::RedrawPath).pixmap(s, 2.0).toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(pen != vec);
        QVERIFY(pen != red);
        QVERIFY(vec != red);
    }
};

QTEST_MAIN(V057Test)
#include "v057_test.moc"
