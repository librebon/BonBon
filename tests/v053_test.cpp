// Polygon Slice tool (always in the Slice flyout when Enable web features is on),
// .lwd round-trip, and HTML image-map export. Existing polygon slices load and export
// without a preference gate.
#include "core/Document.h"
#include "io/HtmlExport.h"
#include "io/LwdFormat.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/Shortcuts.h"
#include "ui/ToolsPanel.h"
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QRegularExpression>
#include <QDir>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

namespace {
void setWebPref(bool on)
{
    Prefs p = appState().prefs;
    p.enableWebFeatures = on;
    appState().setPrefs(p);
}

void sendMouse(QWidget *w, QEvent::Type t, QPointF pos, Qt::MouseButton b, Qt::MouseButtons bs)
{
    QMouseEvent ev(t, pos, w->mapToGlobal(pos.toPoint()), b, bs, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}
void click(Canvas *c, QPointF a)
{
    const QPointF v = c->toView(a);
    sendMouse(c, QEvent::MouseMove, v, Qt::NoButton, Qt::NoButton);
    sendMouse(c, QEvent::MouseButtonPress, v, Qt::LeftButton, Qt::LeftButton);
    sendMouse(c, QEvent::MouseButtonRelease, v, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
}
VObject makePolySlice()
{
    VObject o;
    o.type = ObjType::Slice;
    o.webShape = WebShape::Polygon;
    o.points = QPolygonF{{40, 30}, {200, 40}, {180, 160}, {50, 150}};
    o.rect = o.points.boundingRect();
    o.closed = true;
    o.name = QStringLiteral("badge");
    o.link = QStringLiteral("https://example.com/poly");
    o.alt = QStringLiteral("Polygon badge");
    o.target = QStringLiteral("_blank");
    return o;
}

FlyoutToolButton *sliceFlyout(QWidget *w)
{
    return w->findChild<FlyoutToolButton *>(QStringLiteral("tool_Slice"));
}

QString exportPolyHtml(const Document &d)
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) return {};
    const QString htmlPath = tmp.filePath(QStringLiteral("page.html"));
    html::Options opt;
    opt.layout = html::Layout::Table;
    opt.imagesDir = QStringLiteral("images");
    QString err;
    QStringList written;
    if (!html::exportHtml(d, htmlPath, opt, &err, &written))
        return err.isEmpty() ? QStringLiteral("export failed") : err;
    QFile hf(htmlPath);
    if (!hf.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(hf.readAll());
}
}

class V053Test : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        setWebPref(false);
        appState().setTool(Tool::Pointer);
    }

    void toolRegistry()
    {
        QCOMPARE(toolName(Tool::PolySlice), QStringLiteral("Polygon Slice"));
        QCOMPARE(toolActionId(Tool::PolySlice), QStringLiteral("tool.Polygon Slice"));
        QCOMPARE(toolFromActionId(QStringLiteral("tool.Polygon Slice")), Tool::PolySlice);
        QCOMPARE(toolShortcut(Tool::PolySlice), QStringLiteral("K"));
        QVERIFY(toolIsSlice(Tool::PolySlice));
        QCOMPARE(int(kLastTool) + 1, toolCount());
        QVERIFY(int(Tool::PolySlice) > int(Tool::Zoom)); // appended after Zoom

        QCOMPARE(toolGroup(Tool::Slice), (QList<Tool>{Tool::Slice, Tool::PolySlice}));
        QCOMPARE(toolGroup(Tool::PolySlice), (QList<Tool>{Tool::Slice, Tool::PolySlice}));

        setWebPref(false);
        QVERIFY(!toolAvailable(Tool::PolySlice));
        QVERIFY(!toolAvailable(Tool::Slice));

        setWebPref(true);
        QVERIFY(toolAvailable(Tool::Slice));
        QVERIFY(toolAvailable(Tool::PolySlice));
        setWebPref(false);
    }

    void flyoutPresentWhenWebOnAndKeyCycles()
    {
        setWebPref(true);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        DocumentView *v = w.addDocument(Document::createNew(400, 300, QColor(240, 236, 220)));
        Canvas *c = v->canvas();
        QVERIFY(c);

        auto *fly = sliceFlyout(&w);
        QVERIFY2(fly, "Slice is a flyout (Rectangle + Polygon) whenever web features are on");
        QVERIFY(fly->isVisibleTo(&w));
        QVERIFY(fly->flyout());
        bool hasPoly = false;
        for (QAction *a : fly->flyout()->actions())
            if (a->data().toInt() == int(Tool::PolySlice)) hasPoly = true;
        QVERIFY(hasPoly);

        appState().setTool(Tool::PolySlice);
        QCOMPARE(appState().tool(), Tool::PolySlice);

        appState().setTool(Tool::Pointer);
        c->setFocus();
        QTest::keyClick(c, Qt::Key_K);
        QVERIFY(toolIsSlice(appState().tool()));
        const Tool first = appState().tool();
        QTest::keyClick(c, Qt::Key_K);
        QVERIFY(toolIsSlice(appState().tool()));
        QVERIFY(appState().tool() != first);
    }

    void existingPolygonSliceLoadsAndExports()
    {
        std::unique_ptr<Document> src(Document::createNew(400, 300, QColor(240, 236, 220)));
        VObject art;
        art.type = ObjType::Rect;
        art.rect = QRectF(20, 20, 240, 180);
        art.fillOn = true;
        art.fill = QColor(70, 140, 210);
        art.strokeOn = false;
        src->addObject(art, 0, false);
        src->addObject(makePolySlice());
        src->commit(QStringLiteral("poly"));

        std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*src)));
        QVERIFY(e);
        const int ewl = e->webLayer();
        QVERIFY(ewl >= 0);
        QCOMPARE(e->objects(ewl).size(), 1);
        const VObject &got = e->objects(ewl).first();
        QCOMPARE(got.type, ObjType::Slice);
        QCOMPARE(got.webShape, WebShape::Polygon);
        QCOMPARE(got.points.size(), 4);
        QVERIFY(got.isPolygonWeb());
        QVERIFY(got.usesPoints());
        QCOMPARE(got.link, QStringLiteral("https://example.com/poly"));
        QCOMPARE(got.alt, QStringLiteral("Polygon badge"));
        QCOMPARE(got.target, QStringLiteral("_blank"));

        const QString html = exportPolyHtml(*e);
        QVERIFY2(html.contains(QStringLiteral("shape=\"poly\"")), qPrintable(html));
        QVERIFY(html.contains(QStringLiteral("https://example.com/poly")));
        QVERIFY(html.contains(QStringLiteral("Polygon badge")));
        QVERIFY(html.contains(QStringLiteral("target=\"_blank\"")));
        QVERIFY(html.contains(QStringLiteral("<area ")));
        QVERIFY(!html.contains(QRegularExpression(QStringLiteral("<a href=\"https://example.com/poly\"[^>]*>\\s*<img"))));
    }

    void turningWebOffClampsSliceTool()
    {
        setWebPref(true);
        appState().setTool(Tool::PolySlice);
        QCOMPARE(appState().tool(), Tool::PolySlice);
        setWebPref(false);
        QCOMPARE(appState().tool(), Tool::Pointer);
        QVERIFY(!toolAvailable(Tool::PolySlice));
        QVERIFY(!toolAvailable(Tool::Slice));
    }

    void flyoutDrawSaveLoadAndHtml()
    {
        setWebPref(true);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        DocumentView *v = w.addDocument(Document::createNew(400, 300, QColor(240, 236, 220)));
        Document *d = v->doc();
        Canvas *c = v->canvas();
        QVERIFY(c);

        auto *fb = sliceFlyout(&w);
        QVERIFY(fb);
        QVERIFY(fb->isVisibleTo(&w));
        QVERIFY(fb->flyout());
        bool hasPoly = false;
        for (QAction *a : fb->flyout()->actions())
            if (a->data().toInt() == int(Tool::PolySlice)) hasPoly = true;
        QVERIFY(hasPoly);

        // Artwork so the exported cell is not blank.
        VObject art;
        art.type = ObjType::Rect;
        art.rect = QRectF(20, 20, 240, 180);
        art.fillOn = true;
        art.fill = QColor(70, 140, 210);
        art.strokeOn = false;
        d->addObject(art, 0, false);
        d->commit(QStringLiteral("art"));

        appState().webLayerVisible = true;
        appState().notifyOptions();
        appState().setTool(Tool::PolySlice);
        QCOMPARE(appState().tool(), Tool::PolySlice);
        click(c, {40, 30});
        click(c, {200, 40});
        click(c, {180, 160});
        click(c, {50, 150});
        // Esc mid-stroke cancels
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(c, &esc);
        QCoreApplication::processEvents();
        QCOMPARE(d->webLayer() >= 0 ? d->objects(d->webLayer()).size() : 0, 0);

        click(c, {40, 30});
        click(c, {200, 40});
        click(c, {180, 160});
        click(c, {50, 150});
        c->finishPen(true);
        const int wl = d->webLayer();
        QVERIFY(wl >= 0);
        QCOMPARE(d->objects(wl).size(), 1);
        const VObject sl = d->objects(wl).last();
        QCOMPARE(sl.type, ObjType::Slice);
        QCOMPARE(sl.webShape, WebShape::Polygon);
        QCOMPARE(sl.points.size(), 4);
        QVERIFY(sl.usesPoints());
        QVERIFY(sl.isPolygonWeb());

        // Apply link fields as the tool would from the inspector (AppState defaults).
        VObject *live = d->find(sl.id);
        QVERIFY(live);
        live->link = QStringLiteral("https://example.com/poly");
        live->alt = QStringLiteral("Polygon badge");
        live->target = QStringLiteral("_blank");
        d->commit(QStringLiteral("slice link"));

        // Points stay editable with the pref on (Subselection uses canEditPoints()).
        appState().setTool(Tool::Subselect);
        VObject *edit = d->find(sl.id);
        QVERIFY(edit && edit->usesPoints() && edit->points.size() == 4);
        edit->points[2] = QPointF(190, 170);
        edit->rect = edit->points.boundingRect();
        d->commit(QStringLiteral("edit point"));
        QCOMPARE(d->find(sl.id)->points[2], QPointF(190, 170));
        QVERIFY(d->find(sl.id)->bounds().contains(QPointF(190, 170)));

        // .lwd round-trip (no format bump; old rect slices still load).
        std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*d)));
        QVERIFY(e);
        const int ewl = e->webLayer();
        QVERIFY(ewl >= 0);
        QCOMPARE(e->objects(ewl).size(), 1);
        const VObject &got = e->objects(ewl).first();
        QCOMPARE(got.type, ObjType::Slice);
        QCOMPARE(got.webShape, WebShape::Polygon);
        QCOMPARE(got.points.size(), 4);
        QCOMPARE(got.points[2], QPointF(190, 170));
        QCOMPARE(got.link, QStringLiteral("https://example.com/poly"));
        QCOMPARE(got.alt, QStringLiteral("Polygon badge"));
        QCOMPARE(got.target, QStringLiteral("_blank"));

        // Old documents: a slice with no shape field stays a rectangle.
        VObject rectSl;
        rectSl.type = ObjType::Slice;
        rectSl.rect = QRectF(10, 10, 80, 40);
        rectSl.name = QStringLiteral("old");
        std::unique_ptr<Document> oldd(Document::createNew(200, 100, Qt::white));
        oldd->addObject(rectSl);
        oldd->commit(QStringLiteral("old slice"));
        const QByteArray raw = lwd::serialize(*oldd);
        QVERIFY(raw.size() > 0);
        std::unique_ptr<Document> oldBack(lwd::deserialize(raw));
        QVERIFY(oldBack);
        QCOMPARE(oldBack->objects(oldBack->webLayer()).first().webShape, WebShape::Rect);

        const QString html = exportPolyHtml(*e);
        QVERIFY2(html.contains(QStringLiteral("shape=\"poly\"")), qPrintable(html));
        QVERIFY(html.contains(QStringLiteral("https://example.com/poly")));
        QVERIFY(html.contains(QStringLiteral("Polygon badge")));
        QVERIFY(html.contains(QStringLiteral("target=\"_blank\"")));
        QVERIFY(html.contains(QStringLiteral("<area ")));
        // The whole cell is not wrapped in <a> — the poly area carries the link.
        QVERIFY(!html.contains(QRegularExpression(QStringLiteral("<a href=\"https://example.com/poly\"[^>]*>\\s*<img"))));
        // Sample snippet for the report.
        const QString snippetPath = QStringLiteral("/workspace/brushes-0.3.20/polygon-slice-export.html");
        QDir().mkpath(QStringLiteral("/workspace/brushes-0.3.20"));
        QFile sf(snippetPath);
        QVERIFY(sf.open(QIODevice::WriteOnly));
        sf.write(html.toUtf8());

        // Shortcut K cycles Slice ↔ Polygon Slice when web features are on.
        appState().setTool(Tool::Pointer);
        c->setFocus();
        QTest::keyClick(c, Qt::Key_K);
        QVERIFY(toolIsSlice(appState().tool()));
        const Tool first = appState().tool();
        QTest::keyClick(c, Qt::Key_K);
        QVERIFY(toolIsSlice(appState().tool()));
        QVERIFY(appState().tool() != first);
    }
};

static bool writeShot(const QString &path)
{
    setWebPref(true);
    MainWindow w;
    w.resize(900, 640);
    w.show();
    QCoreApplication::processEvents();
    DocumentView *v = w.addDocument(Document::createNew(480, 320, QColor(248, 247, 244)));
    Document *d = v->doc();
    VObject art;
    art.type = ObjType::Rect;
    art.rect = QRectF(30, 24, 400, 260);
    art.fillOn = true;
    art.fill = QColor(90, 150, 210);
    art.strokeOn = true;
    art.stroke = QColor(30, 50, 90);
    art.strokeWidth = 2;
    d->addObject(art, 0, false);
    VObject sl = makePolySlice();
    sl.points = QPolygonF{{80, 50}, {300, 70}, {340, 230}, {70, 210}};
    sl.rect = sl.points.boundingRect();
    sl.name = QStringLiteral("badge");
    sl.link = QStringLiteral("https://example.com/poly");
    d->addObject(sl);
    d->commit(QStringLiteral("shot"));
    d->selectOnly(sl.id);
    appState().webLayerVisible = true;
    appState().notifyOptions();
    appState().setTool(Tool::Pointer);
    for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
    Canvas *c = v->canvas();
    const QPixmap pm = c->grab();
    QDir().mkpath(QFileInfo(path).absolutePath());
    return pm.save(path, "PNG");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--shot") && i + 1 < argc)
            return writeShot(QString::fromLocal8Bit(argv[++i])) ? 0 : 1;
    }
    V053Test t;
    return QTest::qExec(&t, argc, argv);
}

#include "v053_test.moc"
