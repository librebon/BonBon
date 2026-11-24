// 0.3.23: complete commit digests, strokeSeed load, Smoke dirty bounds, Brush Edit Stroke
// (PI button beside Size; colour popups are colours only; custom params reach the dab engine),
// Insert Hotspot on groups/compounds, web-off tool keys, Cellular lock, ToolsPanel cleanup,
// stamp cache, sample-under with vector tools.
#include "core/Document.h"
#include "core/Object.h"
#include "core/Stroke.h"
#include "core/Fills.h"
#include "core/BrushEngine.h"
#include "io/LwdFormat.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/ColorPicker.h"
#include "ui/Dialogs.h"
#include "ui/DocumentView.h"
#include "ui/EditStrokeDialog.h"
#include "ui/MainWindow.h"
#include "ui/Panels.h"
#include "ui/PropertyInspector.h"
#include "ui/ToolsPanel.h"
#include <QAction>
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {

void setWeb(bool on)
{
    Prefs p = appState().prefs;
    p.enableWebFeatures = on;
    appState().setPrefs(p);
}

VObject mkRect(Document *d, const QRectF &r, const QColor &c = Qt::blue)
{
    VObject o;
    o.type = ObjType::Rect;
    o.rect = r;
    o.fillOn = true;
    o.fill = c;
    o.strokeOn = false;
    o.id = d->newId();
    return o;
}

VObject mkEllipse(Document *d, const QRectF &r, const QColor &c = Qt::red)
{
    VObject o;
    o.type = ObjType::Ellipse;
    o.rect = r;
    o.fillOn = true;
    o.fill = c;
    o.strokeOn = false;
    o.id = d->newId();
    return o;
}

void sendMouse(QWidget *w, QEvent::Type t, QPointF pos, Qt::MouseButton b, Qt::MouseButtons bs)
{
    QMouseEvent ev(t, pos, w->mapToGlobal(pos.toPoint()), b, bs, Qt::NoModifier);
    QApplication::sendEvent(w, &ev);
}

void dragStroke(Canvas *c, QPointF a, QPointF b, int steps = 8)
{
    const QPointF va = c->toView(a), vb = c->toView(b);
    sendMouse(c, QEvent::MouseMove, va, Qt::NoButton, Qt::NoButton);
    sendMouse(c, QEvent::MouseButtonPress, va, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= steps; ++i)
        sendMouse(c, QEvent::MouseMove, va + (vb - va) * (double(i) / steps), Qt::NoButton, Qt::LeftButton);
    sendMouse(c, QEvent::MouseButtonRelease, vb, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
}

QTreeWidgetItem *layerRowForId(QTreeWidget *tree, quint64 id)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *layer = tree->topLevelItem(i);
        for (int j = 0; j < layer->childCount(); ++j) {
            QTreeWidgetItem *it = layer->child(j);
            if (it->data(0, Qt::UserRole).toULongLong() == id)
                return it;
        }
    }
    return nullptr;
}

void contextDeleteRow(QTreeWidget *tree, QTreeWidgetItem *it)
{
    tree->scrollToItem(it);
    QCoreApplication::processEvents();
    const QRect r = tree->visualItemRect(it);
    const QRect c = tree->visualRect(tree->indexFromItem(it, 2));
    const QPoint vp(c.left() + 12, r.center().y());
    // Right-clicks land on the viewport; the tree emits customContextMenuRequested from there.
    QContextMenuEvent ev(QContextMenuEvent::Mouse, vp, tree->viewport()->mapToGlobal(vp));
    QApplication::sendEvent(tree->viewport(), &ev);
    QMetaObject::invokeMethod(tree, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, vp));
    QCoreApplication::processEvents();
    QAction *del = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *m = qobject_cast<QMenu *>(w)) {
            if (m->objectName() == QLatin1String("layersOptionsMenu") && m->isVisible()) {
                del = m->findChild<QAction *>(QStringLiteral("layers.delete"));
                if (!del) {
                    for (QAction *a : m->actions())
                        if (a->text() == QLatin1String("Delete")) { del = a; break; }
                }
                break;
            }
        }
    }
    if (!del) {
        // Fall back: the panel's menu may not be a top-level once parented.
        if (auto *m = tree->window()->findChild<QMenu *>(QStringLiteral("layersOptionsMenu")))
            del = m->findChild<QAction *>(QStringLiteral("layers.delete"));
    }
    QVERIFY2(del, "layers.delete action");
    del->trigger();
    if (QMenu *open = tree->window()->findChild<QMenu *>(QStringLiteral("layersOptionsMenu")))
        open->hide();
    QCoreApplication::processEvents();
}

QVector<quint64> bitmapIds(Document *d)
{
    QVector<quint64> ids;
    for (int li = 0; li < d->s.layers.size(); ++li)
        for (const VObject &o : d->objects(li))
            if (o.type == ObjType::Bitmap) ids << o.id;
    return ids;
}

QImage paintCellular(const QPainterPath &path, const stroke::Params &p, double width,
                     int cellSizeLock, quint32 seed, const QSize &sz)
{
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, false);
    stroke::PaintInput in;
    in.params = p;
    in.width = width;
    in.color = QColor(20, 180, 80);
    in.seed = seed;
    in.cellSizeLock = cellSizeLock;
    stroke::paint(painter, path, in);
    return img;
}


int pixelDiff(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()) return -1;
    int n = 0;
    const QImage aa = a.convertToFormat(QImage::Format_ARGB32);
    const QImage bb = b.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < aa.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(aa.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(bb.constScanLine(y));
        for (int x = 0; x < aa.width(); ++x)
            if (pa[x] != pb[x]) ++n;
    }
    return n;
}

QImage paintBrushLine(const brush::Params &bp, const QSize &sz = QSize(200, 80))
{
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(255, 255, 255, 255));
    brush::Stroke st;
    st.begin(img, bp);
    // First segment only stamps its end point; the second interpolates the line.
    QRect dirty = st.segment(QPointF(24, 40), QPointF(24, 40));
    dirty |= st.segment(QPointF(24, 40), QPointF(176, 40));
    dirty |= st.finish();
    if (!dirty.isEmpty()) st.composite(img, dirty);
    st.end();
    return img;
}

brush::Params hardTip(double size = 16)
{
    brush::Params bp;
    brush::applyTip(bp, brush::Category::Basic, 0);
    bp.size = size;
    bp.color = QColor(20, 40, 180, 255);
    bp.opacity = 100;
    return bp;
}

brush::Params cellularTip(double size = 16)
{
    const QStringList names = brush::tipNames(brush::Category::Libreworks);
    const int ti = qMax(0, names.indexOf(QStringLiteral("Cellular")));
    brush::Params bp;
    brush::applyTip(bp, brush::Category::Libreworks, ti);
    bp.size = size;
    bp.color = QColor(20, 160, 70, 255);
    bp.opacity = 100;
    return bp;
}

stroke::Params customFrom(const brush::Params &bp)
{
    stroke::Params sp = bp.strokeCustom ? bp.strokeParams : stroke::paramsFor(bp.strokeKey);
    sp.key = stroke::canonicalKey(bp.strokeKey);
    return sp;
}

QImage withCustom(brush::Params bp, const stroke::Params &sp)
{
    bp.strokeCustom = true;
    bp.strokeParams = sp;
    return paintBrushLine(bp);
}

VObject strokeLineObj(const QString &kind, double width)
{
    VObject o;
    o.type = ObjType::Path;
    o.strokeOn = true;
    o.fillOn = false;
    o.stroke = QColor(20, 40, 180, 255);
    o.strokeWidth = width;
    o.strokeKind = kind;
    o.strokeCustom = false;
    return o;
}

QImage paintVectorLine(const VObject &o, const QSize &sz = QSize(200, 80))
{
    QImage img(sz, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(255, 255, 255, 255));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.moveTo(24, 40);
    path.lineTo(176, 40);
    fills::strokeShape(p, o, path, false);
    return img;
}

}

class V063Test : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        QCoreApplication::instance()->setProperty("lw_testing", true);
    }

    void commitRecordsAnimationLoops()
    {
        std::unique_ptr<Document> d(Document::createNew(80, 60, Qt::white));
        d->markClean();
        QCOMPARE(d->s.loops, 0);
        d->s.loops = 4;
        d->commit(QStringLiteral("Animation Loops"));
        QVERIFY(d->isDirty());
        QVERIFY(d->canUndo());
        QCOMPARE(d->s.loops, 4);
        d->undo();
        QCOMPARE(d->s.loops, 0);
        d->redo();
        QCOMPARE(d->s.loops, 4);
    }

    void commitRecordsSymbolNameAndKind()
    {
        std::unique_ptr<Document> d(Document::createNew(80, 60, Qt::white));
        d->addObject(mkRect(d.get(), QRectF(8, 8, 20, 16)), 0, true);
        const quint64 sid = d->convertToSymbol(QStringLiteral("Original"), SymbolKind::Graphic);
        QVERIFY(sid != 0);
        d->commit(QStringLiteral("Convert to Symbol"));
        d->markClean();
        QVERIFY(!d->s.symbols.isEmpty());
        Symbol *sy = nullptr;
        for (Symbol &s : d->s.symbols) if (s.id == sid) { sy = &s; break; }
        QVERIFY(sy);
        sy->name = QStringLiteral("Renamed");
        sy->kind = SymbolKind::Button;
        d->commit(QStringLiteral("Symbol Properties"));
        QVERIFY(d->isDirty());
        QVERIFY(d->canUndo());
        bool found = false;
        for (const Symbol &s : d->s.symbols) if (s.id == sid) {
            QCOMPARE(s.name, QStringLiteral("Renamed"));
            QCOMPARE(s.kind, SymbolKind::Button);
            found = true;
        }
        QVERIFY(found);
        d->undo();
        found = false;
        for (const Symbol &s : d->s.symbols) if (s.id == sid) {
            QCOMPARE(s.name, QStringLiteral("Original"));
            QCOMPARE(s.kind, SymbolKind::Graphic);
            found = true;
        }
        QVERIFY(found);
    }

    void strokeSeedRoundTripsInLwd()
    {
        std::unique_ptr<Document> d(Document::createNew(80, 60, Qt::white));
        VObject o;
        o.type = ObjType::Path;
        o.points = QPolygonF(QRectF(10, 10, 40, 20));
        o.closed = true;
        o.strokeOn = true;
        o.strokeKind = QStringLiteral("lw.cellular");
        o.strokeSeed = 0xC0FFEEu;
        o.id = d->newId();
        d->addObject(o, 0, true);
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("seed.lwd"));
        QString err;
        QVERIFY2(lwd::save(*d, path, &err), qPrintable(err));
        std::unique_ptr<Document> loaded(lwd::load(path, &err));
        QVERIFY2(loaded, qPrintable(err));
        QVERIFY(!loaded->objects(0).isEmpty());
        QCOMPARE(loaded->objects(0).last().strokeSeed, quint32(0xC0FFEEu));
    }

    void smokePaintedBoundsIncludesUpwardDrift()
    {
        stroke::Params p = stroke::paramsFor(QStringLiteral("lw.smoke"));
        QCOMPARE(p.look, stroke::Look::Smoke);
        QPainterPath path;
        path.moveTo(40, 80);
        path.lineTo(120, 80);
        const double w = 8;
        const QRectF bounds = stroke::paintedBounds(path, p, w);
        const double r = stroke::paintedRadius(p, w);
        const QRectF tight = path.boundingRect().adjusted(-r, -r, r, r);
        QVERIFY2(bounds.top() < tight.top() - 1.0,
                 "Smoke drift must extend the dirty rect upward past the painted radius");
        QCOMPARE(bounds.left(), tight.left());
        QCOMPARE(bounds.right(), tight.right());
        QCOMPARE(bounds.bottom(), tight.bottom());
    }

    void brushEditStrokeButtonSitsBesideSizeAndDrivesTip()
    {
        MainWindow w;
        w.resize(1400, 800);
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(200, 160, Qt::white);
        // A selected bitmap must not steal Edit Stroke from the Brush tool.
        VObject bm;
        bm.type = ObjType::Bitmap;
        bm.rect = QRectF(0, 0, 200, 160);
        bm.image = QImage(200, 160, QImage::Format_ARGB32_Premultiplied);
        bm.image.fill(Qt::white);
        bm.id = d->newId();
        d->addObject(bm, 0, true);
        w.addDocument(d);
        appState().setTool(Tool::Brush);
        {
            const QStringList cats = brush::categoryNames();
            int ci = cats.indexOf(QStringLiteral("Libreworks"));
            if (ci < 0) ci = 0;
            appState().brushCategory = ci;
            const QStringList tips = brush::tipNames(brush::Category(ci));
            int ti = tips.indexOf(QStringLiteral("Cellular"));
            if (ti < 0) ti = 0;
            appState().brushTip = ti;
            appState().strokeCustom = false;
            appState().strokeKind = QStringLiteral("basic.hard");
            appState().strokeOn = false;
            appState().brushStrokeByTip.clear();
        }
        QCoreApplication::processEvents();
        QApplication::processEvents();

        auto *editAct = w.action(QStringLiteral("modify.editstroke"));
        QVERIFY(editAct);
        QVERIFY2(editAct->isEnabled(), "Modify > Edit Stroke stays enabled with the Brush tool, even if a bitmap is selected");

        auto *pi = w.findChild<PropertyInspector *>();
        QVERIFY(pi);
        auto *size = pi->findChild<QSpinBox *>(QStringLiteral("opt_size"));
        auto *btn = pi->findChild<QPushButton *>(QStringLiteral("opt_editStroke"));
        QVERIFY2(size, "Brush PI Size control");
        QVERIFY2(btn, "Edit Stroke… must sit on the Brush PI, to the right of Size");
        QCOMPARE(btn->text(), QStringLiteral("Edit Stroke…"));
        QVERIFY(btn->isVisible());
        // Same row: the button is to the right of the size field.
        QVERIFY(btn->parentWidget() == size->parentWidget());
        QVERIFY(btn->x() > size->x() + size->width() / 2);

        // Colour popups are colours only — no Stroke Options block.
        ColorPopup pop(QColor(Qt::black), true, nullptr);
        QVERIFY(!pop.findChild<QWidget *>(QStringLiteral("colorPopupStrokeBox")));
        QVERIFY(!pop.findChild<QWidget *>(QStringLiteral("colorPopupStrokeAdvanced")));

        // Opening the dialog from the Brush PI enables Cellular sample-under and writes the tip.
        bool sawCellular = false;
        QTimer::singleShot(40, &w, [&] {
            for (QWidget *tl : QApplication::topLevelWidgets()) {
                auto *dlg = qobject_cast<EditStrokeDialog *>(tl);
                if (!dlg || !dlg->isVisible()) continue;
                QCheckBox *sampleBox = nullptr, *followBox = nullptr;
                for (QCheckBox *b : dlg->findChildren<QCheckBox *>()) {
                    if (b->text().contains(QLatin1String("underneath"))) sampleBox = b;
                    if (b->text().contains(QLatin1String("edges"))) followBox = b;
                }
                QVERIFY(sampleBox && followBox);
                QVERIFY2(sampleBox->isEnabled() && followBox->isEnabled(),
                         "Cellular sample-under options must be enabled for the Brush tool");
                sampleBox->setChecked(true);
                followBox->setChecked(true);
                sawCellular = true;
                dlg->accept();
                return;
            }
        });
        btn->click();
        QCoreApplication::processEvents();
        QVERIFY2(sawCellular, "Edit Stroke dialog opened from the Brush PI button");
        QVERIFY(!appState().strokeCustom);
        QVERIFY(!appState().strokeOn);
        QCOMPARE(appState().strokeKind, QStringLiteral("basic.hard"));
        const auto it = appState().brushStrokeByTip.constFind(QStringLiteral("lw.cellular"));
        QVERIFY2(it != appState().brushStrokeByTip.cend(), "custom params are stored on the Brush tip, not the vector default");
        QVERIFY(it->cellSampleUnder);
        QVERIFY(it->cellFollowEdges);
    }

    void editStrokeWithVectorToolDisablesSampleUnder()
    {
        appState().setTool(Tool::Pen);
        EditStrokeDialog dlg;
        stroke::Params p = stroke::paramsFor(QStringLiteral("lw.cellular"));
        dlg.setParams(p, QColor(20, 80, 200), 12);
        dlg.setVectorStroke(true);
        QCheckBox *sampleBox = nullptr, *followBox = nullptr;
        for (QCheckBox *b : dlg.findChildren<QCheckBox *>()) {
            if (b->text().contains(QLatin1String("underneath"))) sampleBox = b;
            if (b->text().contains(QLatin1String("edges"))) followBox = b;
        }
        QVERIFY(sampleBox && followBox);
        QVERIFY(!sampleBox->isEnabled());
        QVERIFY(!followBox->isEnabled());
    }

    void insertHotspotKeepsGroupChildrenAndCompoundPolygons()
    {
        setWeb(true);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(240, 180, Qt::white);
        VObject a = mkEllipse(d, QRectF(10, 10, 40, 30));
        VObject b = mkEllipse(d, QRectF(80, 20, 50, 40));
        VObject g;
        g.type = ObjType::Group;
        g.id = d->newId();
        g.children = {a, b};
        d->addObject(g, 0, true);
        w.addDocument(d);
        QCoreApplication::processEvents();
        QAction *ins = w.action(QStringLiteral("edit.inserthotspot"));
        QVERIFY(ins && ins->isEnabled());
        ins->trigger();
        QCoreApplication::processEvents();
        int hs = 0;
        const int wl = d->webLayer();
        QVERIFY(wl >= 0);
        for (const VObject &o : d->objects(wl))
            if (o.type == ObjType::Hotspot) ++hs;
        QCOMPARE(hs, 2);

        // Compound path: two disjoint fill polygons become two hotspots.
        auto *d2 = Document::createNew(240, 180, Qt::white);
        VObject path;
        path.type = ObjType::Path;
        path.points = QPolygonF{{10, 10}, {50, 10}, {50, 50}, {10, 50}, {80, 20}, {130, 20}, {130, 70}, {80, 70}};
        path.contours = {4};
        path.closed = true;
        path.fillOn = true;
        path.strokeOn = false;
        path.id = d2->newId();
        d2->addObject(path, 0, true);
        w.addDocument(d2);
        QCoreApplication::processEvents();
        ins->trigger();
        QCoreApplication::processEvents();
        int hs2 = 0;
        const int wl2 = d2->webLayer();
        QVERIFY(wl2 >= 0);
        for (const VObject &o : d2->objects(wl2))
            if (o.type == ObjType::Hotspot) ++hs2;
        QCOMPARE(hs2, 2);
    }

    void webOffDoesNotSwallowSliceOrHotspotKeys()
    {
        setWeb(false);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(80, 60, Qt::white);
        w.addDocument(d);
        appState().setTool(Tool::Pointer);
        QCoreApplication::processEvents();

        QKeyEvent ovK(QEvent::ShortcutOverride, Qt::Key_K, Qt::NoModifier);
        QVERIFY(!QApplication::sendEvent(&w, &ovK) || !ovK.isAccepted());
        QKeyEvent ovJ(QEvent::ShortcutOverride, Qt::Key_J, Qt::NoModifier);
        QVERIFY(!QApplication::sendEvent(&w, &ovJ) || !ovJ.isAccepted());
        QCOMPARE(appState().tool(), Tool::Pointer);

        setWeb(true);
        QCoreApplication::processEvents();
        QKeyEvent ovK2(QEvent::ShortcutOverride, Qt::Key_K, Qt::NoModifier);
        QApplication::sendEvent(&w, &ovK2);
        QVERIFY(ovK2.isAccepted());
        setWeb(false);
    }

    void cellularLockKeepsCellSizeAndStableOrigins()
    {
        stroke::Params p = stroke::paramsFor(QStringLiteral("lw.cellular"));
        p.cellSize = 3;
        p.cellSteps = 1;
        p.seedDensity = 70;
        // A coverage so large that 0.3.21 coarsens well past 3 px (256×256 cap).
        QPainterPath longPath;
        longPath.moveTo(4, 4);
        longPath.lineTo(900, 4);
        longPath.lineTo(900, 900);
        const QSize sz(920, 920);
        const QImage coarsened = paintCellular(longPath, p, 8, 0, 42, sz);
        const QImage locked = paintCellular(longPath, p, 8, 3, 42, sz);
        QVERIFY(!coarsened.isNull() && !locked.isNull());
        QVERIFY2(coarsened != locked, "Unlocked (0.3.21) coarsening must differ from a locked 3 px grid");

        // Live stroke: growing the path must not jump cells already covered.
        QPainterPath shortPath;
        shortPath.moveTo(40, 80);
        shortPath.lineTo(160, 80);
        QPainterPath grown = shortPath;
        grown.lineTo(280, 90);
        const QImage a = paintCellular(shortPath, p, 10, 3, 99, QSize(320, 160));
        const QImage b = paintCellular(grown, p, 10, 3, 99, QSize(320, 160));
        int diffs = 0;
        const QRect overlap(0, 0, 180, 160);
        for (int y = overlap.top(); y < overlap.bottom(); ++y) {
            const QRgb *sa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
            const QRgb *sb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
            for (int x = overlap.left(); x < overlap.right(); ++x) {
                if (sa[x] == 0) continue;          // only pixels the short stroke already painted
                if (sa[x] != sb[x]) ++diffs;
            }
        }
        QCOMPARE(diffs, 0);
    }

    void toolsPanelHasNoDeadHotspotStack()
    {
        setWeb(true);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *tools = w.findChild<ToolsPanel *>();
        QVERIFY(tools);
        QVERIFY(!tools->findChild<QWidget *>(QStringLiteral("tool_HotspotStack")));
        QVERIFY(!tools->findChild<QWidget *>(QStringLiteral("tool_HotspotSpacer")));
        setWeb(false);
        QCoreApplication::processEvents();
        QVERIFY(tools->findChild<QWidget *>(QStringLiteral("toolSection_Web")) == nullptr
                || !tools->findChild<QWidget *>(QStringLiteral("toolSection_Web"))->isVisible());
    }

    void layersContextDeleteAfterBrushDeselectBrush()
    {
        MainWindow w;
        w.resize(1100, 800);
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(240, 180, Qt::white);
        w.addDocument(d);
        appState().setTool(Tool::Brush);
        appState().brushSize = 12;
        appState().strokeTarget = int(AppState::NewBitmapPerStroke);
        QCoreApplication::processEvents();
        Canvas *c = w.currentView()->canvas();
        dragStroke(c, QPointF(30, 40), QPointF(80, 50));
        const QVector<quint64> afterA = bitmapIds(d);
        QCOMPARE(afterA.size(), 1);
        const quint64 idA = afterA.first();
        d->clearSelection();
        QCoreApplication::processEvents();
        dragStroke(c, QPointF(140, 50), QPointF(190, 70));
        const QVector<quint64> afterB = bitmapIds(d);
        QCOMPARE(afterB.size(), 2);
        quint64 idB = afterB[0] == idA ? afterB[1] : afterB[0];
        QVERIFY(d->selection.contains(idB));
        QVERIFY(!d->selection.contains(idA));

        auto *tree = w.findChild<LayersPanel *>()->findChild<QTreeWidget *>();
        QVERIFY(tree);
        tree->expandAll();
        QCoreApplication::processEvents();
        QTreeWidgetItem *rowA = layerRowForId(tree, idA);
        QVERIFY(rowA);
        contextDeleteRow(tree, rowA);
        QVERIFY2(!d->find(idA), "right-click Delete on A must delete A even when B is selected");
        QVERIFY2(d->find(idB), "bitmap B must be left alone");
    }

    void layersContextDeleteAfterBrushRectBrush()
    {
        MainWindow w;
        w.resize(1100, 800);
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(240, 180, Qt::white);
        w.addDocument(d);
        appState().setTool(Tool::Brush);
        appState().brushSize = 12;
        appState().strokeTarget = int(AppState::NewBitmapPerStroke);
        QCoreApplication::processEvents();
        Canvas *c = w.currentView()->canvas();
        dragStroke(c, QPointF(30, 40), QPointF(80, 50));
        const QVector<quint64> afterA = bitmapIds(d);
        QCOMPARE(afterA.size(), 1);
        const quint64 idA = afterA.first();

        appState().setTool(Tool::Rect);
        QCoreApplication::processEvents();
        dragStroke(c, QPointF(20, 100), QPointF(70, 140));
        quint64 idRect = 0;
        for (int li = 0; li < d->s.layers.size(); ++li)
            for (const VObject &o : d->objects(li))
                if (o.type == ObjType::Rect) idRect = o.id;
        QVERIFY(idRect);

        appState().setTool(Tool::Brush);
        QCoreApplication::processEvents();
        dragStroke(c, QPointF(140, 50), QPointF(190, 70));
        const QVector<quint64> bms = bitmapIds(d);
        QCOMPARE(bms.size(), 2);
        const quint64 idB = bms[0] == idA ? bms[1] : bms[0];

        auto *tree = w.findChild<LayersPanel *>()->findChild<QTreeWidget *>();
        QVERIFY(tree);
        tree->expandAll();
        QCoreApplication::processEvents();
        QVERIFY(layerRowForId(tree, idA));
        contextDeleteRow(tree, layerRowForId(tree, idA));
        QVERIFY2(!d->find(idA), "right-click Delete on A after brush/rect/brush");
        QVERIFY(d->find(idB));
        QVERIFY(d->find(idRect));

        contextDeleteRow(tree, layerRowForId(tree, idB));
        QVERIFY2(!d->find(idB), "right-click Delete on B must also work");
        QVERIFY(d->find(idRect));
    }

    void layersContextDeleteRemovesWholeSelectionWhenClickedRowIsSelected()
    {
        MainWindow w;
        w.resize(1100, 800);
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(200, 160, Qt::white);
        VObject a = mkRect(d, QRectF(10, 10, 30, 20), Qt::red);
        VObject b = mkRect(d, QRectF(50, 10, 30, 20), Qt::blue);
        VObject c = mkRect(d, QRectF(90, 10, 30, 20), Qt::green);
        d->addObject(a, 0, false);
        d->addObject(b, 0, false);
        d->addObject(c, 0, false);
        d->setSelection({a.id, b.id});
        w.addDocument(d);
        QCoreApplication::processEvents();
        auto *tree = w.findChild<LayersPanel *>()->findChild<QTreeWidget *>();
        tree->expandAll();
        QCoreApplication::processEvents();
        contextDeleteRow(tree, layerRowForId(tree, a.id));
        QVERIFY(!d->find(a.id) && !d->find(b.id));
        QVERIFY2(d->find(c.id), "unselected objects stay when the clicked row is part of the selection");
    }


    void brushEditStrokeParamsChangePaintedPixels()
    {
        // Path (a): default Basic / Hard Line uses the dab engine. Edit Stroke
        // params must change the next painted stroke, and Accept with no edits
        // must keep the factory appearance.
        const brush::Params base = hardTip(16);
        const QImage before = paintBrushLine(base);
        QCOMPARE(pixelDiff(before, paintBrushLine(base)), 0);

        stroke::Params factory = customFrom(base);
        QVERIFY(pixelDiff(before, withCustom(base, factory)) == 0);

        auto expectChange = [&](const char *what, const stroke::Params &sp) {
            const int d = pixelDiff(before, withCustom(base, sp));
            QVERIFY2(d > 0, what);
        };

        {
            auto sp = factory; sp.inkAmount = 20;
            expectChange("ink amount", sp);
        }
        {
            auto sp = factory; sp.spacing = 200;
            expectChange("spacing", sp);
        }
        {
            auto sp = factory; sp.flowRate = 80;
            expectChange("flow rate", sp);
        }
        {
            auto sp = factory; sp.texture = QStringLiteral("Grain"); sp.textureAmount = 80;
            brush::Params tex = base;
            tex.texture = sp.texture;
            tex.textureAmount = sp.textureAmount;
            QVERIFY2(pixelDiff(before, withCustom(tex, sp)) > 0, "texture");
        }
        {
            auto sp = factory;
            brush::Params sized = base;
            sized.size = 28;
            sp.size = 28;
            QVERIFY2(pixelDiff(before, withCustom(sized, sp)) > 0, "tip size");
        }
        {
            auto sp = factory; sp.edge = 80;
            brush::Params ed = base;
            ed.edge = 80;
            QVERIFY2(pixelDiff(before, withCustom(ed, sp)) > 0, "edge softness");
        }
        {
            auto sp = factory; sp.square = true;
            brush::Params sq = base;
            sq.square = true;
            QVERIFY2(pixelDiff(before, withCustom(sq, sp)) > 0, "square tip");
        }
        {
            auto sp = factory; sp.aspect = 0.25; sp.angle = 45;
            expectChange("aspect / angle", sp);
        }
        {
            auto sp = factory; sp.sizeSens.random = 80;
            expectChange("size sensitivity (random)", sp);
        }

        // Cellular (path engine): spacing and cell fields already reached paint;
        // keep them covered, plus ink (via dab-equivalent custom copy on width).
        const brush::Params cell = cellularTip(16);
        const QImage cellBefore = paintBrushLine(cell);
        const stroke::Params cellFactory = customFrom(cell);
        QCOMPARE(pixelDiff(cellBefore, withCustom(cell, cellFactory)), 0);
        {
            auto sp = cellFactory; sp.spacing = 10;
            QVERIFY2(pixelDiff(cellBefore, withCustom(cell, sp)) > 0, "cellular spacing");
        }
        {
            auto sp = cellFactory; sp.cellSize = 8; sp.cellSteps = 6; sp.seedDensity = 80;
            QVERIFY2(pixelDiff(cellBefore, withCustom(cell, sp)) > 0, "cellular size/steps/density");
        }
        if (qEnvironmentVariableIsSet("LW_EDITSTROKE_SHOTS")) {
            const QString dir = qEnvironmentVariable("LW_EDITSTROKE_SHOTS");
            QDir().mkpath(dir);
            auto sheet = [&](const QString &name, const QImage &a, const QImage &b) {
                const int s = 3;
                const QImage A = a.scaled(a.size() * s, Qt::IgnoreAspectRatio, Qt::FastTransformation);
                const QImage B = b.scaled(b.size() * s, Qt::IgnoreAspectRatio, Qt::FastTransformation);
                QImage out(A.width() * 2 + 36, A.height() + 36, QImage::Format_ARGB32);
                out.fill(QColor(245, 245, 245));
                QPainter p(&out);
                p.setPen(Qt::black);
                p.drawText(8, 16, name + QStringLiteral(" — before"));
                p.drawText(A.width() + 24, 16, QStringLiteral("after Edit Stroke"));
                p.drawImage(6, 24, A);
                p.drawImage(A.width() + 24, 24, B);
                out.save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
                a.save(dir + QLatin1Char('/') + name + QStringLiteral("-before.png"));
                b.save(dir + QLatin1Char('/') + name + QStringLiteral("-after.png"));
            };
            sheet(QStringLiteral("brush-hard-ink"), before, withCustom(base, [&]{ auto sp=factory; sp.inkAmount=20; return sp; }()));
            sheet(QStringLiteral("brush-hard-spacing"), before, withCustom(base, [&]{ auto sp=factory; sp.spacing=200; return sp; }()));
            sheet(QStringLiteral("brush-hard-texture"), before, withCustom(base, [&]{ auto sp=factory; sp.texture=QStringLiteral("Grain"); sp.textureAmount=80; return sp; }()));
            sheet(QStringLiteral("brush-hard-edge"), before, withCustom(base, [&]{ auto sp=factory; sp.edge=80; return sp; }()));
            sheet(QStringLiteral("brush-hard-aspect"), before, withCustom(base, [&]{ auto sp=factory; sp.aspect=0.25; sp.angle=45; return sp; }()));
            {
                auto sp = factory; sp.size = 28;
                brush::Params sized = base; sized.size = 28;
                sheet(QStringLiteral("brush-hard-size"), before, withCustom(sized, sp));
            }
            {
                auto sp = cellFactory; sp.spacing = 10;
                sheet(QStringLiteral("brush-cellular-spacing"), cellBefore, withCustom(cell, sp));
            }
        }


    }

    void brushEditStrokePiButtonAppliesAndPaints()
    {
        // Full GUI path: Brush PI Edit Stroke…, change ink and size, Accept, paint.
        MainWindow w;
        w.resize(1400, 800);
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(200, 80, Qt::white);
        DocumentView *view = w.addDocument(d);
        QVERIFY(view);
        Canvas *cv = view->canvas();
        QVERIFY(cv);
        appState().setTool(Tool::Brush);
        appState().strokeTarget = AppState::ExpandSelected;
        appState().brushCategory = 0;
        appState().brushTip = 0;
        appState().brushSize = 16;
        appState().brushEdge = 0;
        appState().tipShape = 0;
        appState().brushTexture.clear();
        appState().strokeCustom = false;
        appState().stroke = QColor(20, 40, 180, 255);
        appState().toolOpacity = 100;
        VObject bm;
        bm.type = ObjType::Bitmap;
        bm.rect = QRectF(0, 0, 200, 80);
        bm.image = QImage(200, 80, QImage::Format_ARGB32_Premultiplied);
        bm.image.fill(Qt::white);
        bm.id = d->newId();
        d->addObject(bm);
        d->setSelection({bm.id});
        d->commit(QStringLiteral("bitmap"));
        QCoreApplication::processEvents();
        QApplication::processEvents();

        dragStroke(cv, QPointF(24, 40), QPointF(176, 40), 12);
        QCoreApplication::processEvents();
        auto bitmapOn = [&]() -> VObject * {
            for (int li = 0; li < d->s.layers.size(); ++li)
                for (VObject &o : d->objects(li))
                    if (o.type == ObjType::Bitmap) return &o;
            return nullptr;
        };
        VObject *painted = bitmapOn();
        QVERIFY(painted && !painted->image.isNull());
        const QImage before = painted->image.copy();

        // Fresh white target for the post-edit stroke (do not paint over the baseline).
        painted->image.fill(Qt::white);
        d->markClean();

        auto *pi = w.findChild<PropertyInspector *>();
        QVERIFY(pi);
        auto *btn = pi->findChild<QPushButton *>(QStringLiteral("opt_editStroke"));
        QVERIFY(btn);

        bool applied = false;
        QTimer::singleShot(40, &w, [&] {
            for (QWidget *tl : QApplication::topLevelWidgets()) {
                auto *dlg = qobject_cast<EditStrokeDialog *>(tl);
                if (!dlg || !dlg->isVisible()) continue;
                auto *ink = dlg->findChild<QSpinBox *>(QStringLiteral("editStrokeInk"));
                auto *sz = dlg->findChild<QDoubleSpinBox *>(QStringLiteral("editStrokeSize"));
                QVERIFY(ink && sz);
                QCOMPARE(sz->value(), 16.0);   // Size is seeded from brushSize, not factory 8
                ink->setValue(25);
                sz->setValue(28);
                applied = true;
                dlg->accept();
                return;
            }
        });
        btn->click();
        QCoreApplication::processEvents();
        QVERIFY2(applied, "Edit Stroke dialog from the Brush PI button");
        QVERIFY(!appState().strokeCustom);
        const auto it = appState().brushStrokeByTip.constFind(QStringLiteral("basic.hard"));
        QVERIFY(it != appState().brushStrokeByTip.cend());
        QCOMPARE(it->inkAmount, 25);
        QCOMPARE(appState().brushSize, 28);

        dragStroke(cv, QPointF(24, 40), QPointF(176, 40), 12);
        QCoreApplication::processEvents();
        painted = bitmapOn();
        QVERIFY(painted);
        const int dpx = pixelDiff(before, painted->image);
        QVERIFY2(dpx > 0, "painting after Edit Stroke must differ from the unedited Hard Line");

        // Switching away from Brush and back must not reset the tip or drop custom params.
        const int tip = appState().brushTip;
        const int cat = appState().brushCategory;
        appState().setTool(Tool::Pointer);
        QCoreApplication::processEvents();
        appState().setTool(Tool::Brush);
        QCoreApplication::processEvents();
        QCOMPARE(appState().brushCategory, cat);
        QCOMPARE(appState().brushTip, tip);
        QVERIFY(appState().brushStrokeByTip.contains(QStringLiteral("basic.hard")));
        QCOMPARE(appState().brushStrokeByTip.value(QStringLiteral("basic.hard")).inkAmount, 25);
    }

    void vectorEditStrokeParamsChangePixels()
    {
        // Path (b), secondary: a selected vector's Edit Stroke params change the stroke.
        VObject o = strokeLineObj(QStringLiteral("basic.hard"), 10);
        const QImage before = paintVectorLine(o);
        stroke::Params factory = stroke::paramsFor(o.strokeKind);

        auto apply = [&](stroke::Params sp, bool syncWidth = false) {
            VObject e = o;
            e.strokeCustom = true;
            e.strokeParams = sp;
            if (syncWidth && sp.size > 0) e.strokeWidth = sp.size;
            return paintVectorLine(e);
        };
        QCOMPARE(pixelDiff(before, apply(factory)), 0);

        {
            auto sp = factory; sp.inkAmount = 20;
            QVERIFY2(pixelDiff(before, apply(sp)) > 0, "vector ink amount");
        }
        {
            auto sp = factory; sp.size = 22;
            QVERIFY2(pixelDiff(before, apply(sp, true)) > 0, "vector tip size / strokeWidth");
        }
        {
            auto sp = factory; sp.texture = QStringLiteral("Grain"); sp.textureAmount = 80;
            QVERIFY2(pixelDiff(before, apply(sp)) > 0, "vector texture");
        }

        // Cellular vector stroke
        VObject c = strokeLineObj(QStringLiteral("lw.cellular"), 16);
        const QImage cellBefore = paintVectorLine(c);
        stroke::Params cf = stroke::paramsFor(c.strokeKind);
        {
            auto sp = cf; sp.spacing = 10;
            VObject e = c; e.strokeCustom = true; e.strokeParams = sp;
            QVERIFY2(pixelDiff(cellBefore, paintVectorLine(e)) > 0, "vector cellular spacing");
        }

        // Save / reload keeps custom params and the pixel difference.
        std::unique_ptr<Document> doc(Document::createNew(200, 80, Qt::white));
        VObject saved = o;
        saved.strokeCustom = true;
        saved.strokeParams = factory;
        saved.strokeParams.inkAmount = 20;
        saved.strokeParams.key = QStringLiteral("basic.hard");
        saved.id = doc->newId();
        saved.points = QPolygonF{QPointF(24, 40), QPointF(176, 40)};
        doc->addObject(saved, 0, false);
        QTemporaryDir tmp;
        const QString path = tmp.filePath(QStringLiteral("edit-stroke.lwd"));
        QString err;
        QVERIFY(lwd::save(*doc, path, &err));
        std::unique_ptr<Document> loaded(lwd::load(path, &err));
        QVERIFY(loaded);
        QVERIFY(!loaded->objects(0).isEmpty());
        const VObject *lo = nullptr;
        for (const VObject &x : loaded->objects(0))
            if (x.strokeCustom) { lo = &x; break; }
        QVERIFY(lo);
        QVERIFY(lo->strokeCustom);
        QCOMPARE(lo->strokeParams.inkAmount, 20);
        QVERIFY2(pixelDiff(before, paintVectorLine(*lo)) > 0, "reloaded custom stroke still differs");
    }

    void stampCacheHoldsMultipleImages()
    {
        QTemporaryDir tmp;
        auto write = [&](const QString &name, const QColor &c) {
            QImage im(16, 16, QImage::Format_ARGB32_Premultiplied);
            im.fill(c);
            const QString path = tmp.filePath(name);
            if (!im.save(path, "PNG"))
                return QString();
            return path;
        };
        const QString a = write(QStringLiteral("a.png"), QColor(255, 0, 0, 255));
        const QString b = write(QStringLiteral("b.png"), QColor(0, 0, 255, 255));
        const QImage ia = stroke::loadStampImage(a);
        const QImage ib = stroke::loadStampImage(b);
        const QImage ia2 = stroke::loadStampImage(a);
        QVERIFY(!ia.isNull() && !ib.isNull() && !ia2.isNull());
        QVERIFY(ia != ib);
        QCOMPARE(ia.pixelColor(0, 0).red(), 255);
        QCOMPARE(ib.pixelColor(0, 0).blue(), 255);
        QCOMPARE(ia2.pixelColor(0, 0).red(), 255);
    }
};

QTEST_MAIN(V063Test)
#include "v063_test.moc"
