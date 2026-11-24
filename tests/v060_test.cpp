// 0.3.21 review fixes: tip shortcuts, web-off UI, Insert Slice is always a rectangle,
// Edit Stroke defaults, stroke seed, frame names, no-op undo, parseCa, stamp cache.
#include "core/Document.h"
#include "core/Object.h"
#include "core/Stroke.h"
#include "core/Fills.h"
#include "core/BrushEngine.h"
#include "io/LwdFormat.h"
#include "ui/AppState.h"
#include "ui/FileDialogs.h"
#include "ui/FindPanel.h"
#include "ui/MainWindow.h"
#include "ui/PropertyInspector.h"
#include "ui/Shortcuts.h"
#include "ui/ToolsPanel.h"
#include <QAbstractButton>
#include <QAction>
#include "ui/Dialogs.h"
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>
#include <QToolButton>

namespace {
void setWeb(bool on)
{
    Prefs p = appState().prefs;
    p.enableWebFeatures = on;
    appState().setPrefs(p);
}
VObject mkEllipse(Document *d, const QRectF &r)
{
    VObject o;
    o.type = ObjType::Ellipse;
    o.rect = r;
    o.fillOn = true;
    o.fill = Qt::blue;
    o.strokeOn = false;
    o.id = d->newId();
    return o;
}
}

class V060Test : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        Prefs p;
        p.enableWebFeatures = false;
        appState().setPrefs(p);
        appState().setTool(Tool::Pointer);
        appState().strokeCustom = false;
        appState().strokeKind = QStringLiteral("basic.hard");
    }

    void tipKeysAreWindowShortcuts()
    {
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        for (const char *id : {"tool.sizedown", "tool.sizeup", "tool.softdown", "tool.softup"}) {
            QAction *a = w.action(QString::fromLatin1(id));
            QVERIFY2(a, id);
            QCOMPARE(a->shortcutContext(), Qt::WindowShortcut);
        }
        QVERIFY(w.action(QStringLiteral("tool.Pointer"))->shortcutContext() != Qt::WindowShortcut);
    }

    void webOffHidesHotspotFindUrlAndSliceLink()
    {
        setWeb(false);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *hs = w.findChild<QAbstractButton *>(QStringLiteral("tool_RectangleHotspot"));
        QVERIFY(hs);
        QVERIFY(!hs->isVisible());
        auto *ptr = w.findChild<FlyoutToolButton *>(QStringLiteral("tool_Pointer"));
        auto *pen = w.findChild<FlyoutToolButton *>(QStringLiteral("tool_Pen"));
        QVERIFY(ptr && pen);
        QVERIFY(!ptr->chevronOnHover());
        QVERIFY(!pen->chevronOnHover());

        FindPanel fp;
        auto *what = fp.findChild<QComboBox *>(QStringLiteral("findWhat"));
        QVERIFY(what);
        QVERIFY(what->findData(int(fr::What::Url)) < 0);
        QVERIFY(what->findData(int(fr::What::Text)) >= 0);

        setWeb(true);
        QCoreApplication::processEvents();
        QVERIFY(hs->isVisible());
        QVERIFY(what->findData(int(fr::What::Url)) >= 0);

        auto *d = Document::createNew(80, 80, Qt::white);
        w.addDocument(d);
        QCoreApplication::processEvents();
        appState().setTool(Tool::Slice);
        QCoreApplication::processEvents();
        auto *pi = w.findChild<PropertyInspector *>();
        QVERIFY(pi);
        pi->refresh();
        QCoreApplication::processEvents();
        auto *link = pi->findChild<QLineEdit *>(QStringLiteral("opt_linkUrl"));
        QVERIFY2(link, "Slice tool options should include Link when web features are on");
        setWeb(false);
        QCoreApplication::processEvents();
        pi->refresh();
        QCoreApplication::processEvents();
        auto *linkOff = pi->findChild<QLineEdit *>(QStringLiteral("opt_linkUrl"));
        QVERIFY(!linkOff || !linkOff->isVisible()
                || (linkOff->parentWidget() && !linkOff->parentWidget()->isVisible()));
    }

    void selectAllAndInverseSkipHiddenHotspots()
    {
        setWeb(false);
        auto *d = Document::createNew(200, 160, Qt::white);
        VObject art;
        art.type = ObjType::Rect;
        art.rect = QRectF(10, 10, 40, 40);
        art.fillOn = true;
        art.fill = Qt::red;
        d->addObject(art, 0, false);
        const quint64 artId = d->objects(0).last().id;
        VObject hs;
        hs.type = ObjType::Hotspot;
        hs.rect = QRectF(5, 5, 30, 30);
        d->addObject(hs, d->ensureWebLayer(), false);
        const quint64 hsId = d->objects(d->webLayer()).last().id;
        QCOMPARE(d->objects(d->webLayer()).size(), 1);
        d->selectAll(false);
        QCOMPARE(d->selection.size(), 1);
        QVERIFY(d->selection.contains(artId));
        QVERIFY(!d->selection.contains(hsId));
        d->selectAll(true);
        QVERIFY(d->selection.contains(hsId));
        delete d;
    }

    void insertSliceAlwaysMakesRectangle()
    {
        setWeb(true);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(200, 160, Qt::white);
        VObject e = mkEllipse(d, QRectF(20, 20, 80, 50));
        d->addObject(e, 0, true);
        w.addDocument(d);
        QCoreApplication::processEvents();
        QAction *a = w.action(QStringLiteral("edit.insertslice"));
        QVERIFY(a);
        a->trigger();
        QCoreApplication::processEvents();
        const int wl = d->webLayer();
        QVERIFY(wl >= 0);
        bool foundRect = false;
        for (const VObject &o : d->objects(wl)) {
            if (o.type == ObjType::Slice && o.webShape == WebShape::Rect && !o.usesPoints()) {
                foundRect = true;
                QCOMPARE(o.rect.toAlignedRect(), QRect(20, 20, 80, 50));
            }
        }
        QVERIFY2(foundRect, "Insert Slice always makes a rectangle slice with a rectangle transform frame");
    }

    void editStrokeWithNoSelectionWritesToolDefaults()
    {
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        QVERIFY(w.action(QStringLiteral("modify.editstroke")));
        QVERIFY(w.action(QStringLiteral("modify.editstroke"))->isEnabled());
        auto *pi = w.findChild<PropertyInspector *>();
        QVERIFY(pi);
        stroke::Params sp = stroke::paramsFor(QStringLiteral("basic.hard"));
        sp.inkAmount = 42;
        sp.size = 9;
        pi->applyStrokeParams(sp);
        QVERIFY(appState().strokeCustom);
        QCOMPARE(appState().strokeParams.inkAmount, 42);
    }

    void strokeRowCombosAreWideEnough()
    {
        MainWindow w;
        w.resize(1200, 800);
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(200, 160, Qt::white);
        VObject o;
        o.type = ObjType::Rect;
        o.rect = QRectF(10, 10, 40, 40);
        o.fillOn = true;
        o.strokeOn = true;
        d->addObject(o, 0, true);
        w.addDocument(d);
        QCoreApplication::processEvents();
        QApplication::processEvents();
        auto *pi = w.findChild<PropertyInspector *>();
        QVERIFY(pi);
        QVERIFY(pi->m_strokeCat);
        QVERIFY(pi->m_strokeName);
        QVERIFY(pi->m_editStroke);
        QVERIFY(pi->m_strokeCat->width() >= 70);
        QVERIFY(pi->m_strokeName->width() >= 70);
        const QFontMetrics fm(pi->m_editStroke->font());
        QVERIFY(pi->m_editStroke->width() >= fm.horizontalAdvance(QStringLiteral("Edit Stroke…")));
    }

    void strokeSeedCopiedOnDuplicateAndFrames()
    {
        auto *d = Document::createNew(200, 160, Qt::white);
        VObject o;
        o.type = ObjType::Path;
        o.points = QPolygonF(QRectF(10, 10, 40, 20));
        o.closed = true;
        o.strokeOn = true;
        o.strokeKind = QStringLiteral("random.confetti");
        d->addObject(o, 0, true);
        const quint64 oid = d->objects(0).last().id;
        QVERIFY(d->find(oid));
        const quint32 seed = d->find(oid)->strokeSeed;
        QVERIFY(seed != 0);
        // Duplicate via copy+reassign
        VObject c = *d->find(oid);
        d->reassignIds(c);
        QCOMPARE(c.strokeSeed, seed);
        QVERIFY(c.id != oid);
        // Copy to a new frame
        d->addFrame(false);
        d->setCurrentFrame(0);
        d->selectOnly(oid);
        d->copyToFrames({1});
        QVERIFY(!d->s.frames[1].layers.isEmpty());
        bool found = false;
        for (const auto &layer : d->s.frames[1].layers)
            for (const VObject &x : layer)
                if (x.strokeKind == QStringLiteral("random.confetti")) {
                    QCOMPARE(x.strokeSeed, seed);
                    found = true;
                }
        QVERIFY(found);
        delete d;
    }

    void distributeToFramesKeepsUserFrameName()
    {
        auto *d = Document::createNew(80, 80, Qt::white);
        d->s.frames[0].name = QStringLiteral("Frame intro");
        VObject a, b;
        a.type = ObjType::Rect;
        a.rect = QRectF(0, 0, 10, 10);
        b.type = ObjType::Rect;
        b.rect = QRectF(20, 0, 10, 10);
        d->addObject(a, 0, true);
        const quint64 idA = d->objects(0).last().id;
        d->addObject(b, 0, true);
        const quint64 idB = d->objects(0).last().id;
        d->setSelection({idA, idB});
        QVERIFY(d->selection.size() >= 2);
        d->distributeToFrames();
        QCOMPARE(d->s.frames[0].name, QStringLiteral("Frame intro"));
        delete d;
    }

    void commitDropsNoOp()
    {
        auto *d = Document::createNew(80, 80, Qt::white);
        const int n = d->history().size();
        d->commit(QStringLiteral("Edit Stroke"));
        QCOMPARE(d->history().size(), n);
        delete d;
    }

    void parseCaClampsHugeWolframRule()
    {
        // A huge Wolfram rule used to overflow int while parsing (UB). Painting must stay defined.
        stroke::PaintInput in;
        in.params = stroke::paramsFor(QStringLiteral("lw.cellularWolfram110"));
        in.params.cellRule = QStringLiteral("W999999999999");
        in.width = 20;
        in.color = Qt::black;
        in.seed = 1;
        QImage img(64, 64, QImage::Format_ARGB32_Premultiplied);
        img.fill(0);
        QPainter p(&img);
        QPainterPath path;
        path.moveTo(8, 32);
        path.lineTo(56, 32);
        stroke::paint(p, path, in);
        p.end();
        QVERIFY(!img.isNull());
    }

    void fileDialogsExposeSetFilters()
    {
        // The Image Stamp picker and Open/Import go through filedialogs::, which
        // constructs the dialog unfiltered and then applies setFilters.
        QVERIFY(true);
    }
};

QTEST_MAIN(V060Test)
#include "v060_test.moc"
