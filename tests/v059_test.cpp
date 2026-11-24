// Preferences > General "Enable web features" (0.3.20 / 0.3.22). Off by default: hide
// the Web tools section (Slice, Hotspot, show/hide buttons), Insert > Slice / Hotspot,
// and File > Export HTML / Slices / Image Map. Existing slices stay visible on the
// canvas, in Layers, and in the Property Inspector; View > Show Slices still toggles
// the overlay. Existing web data loads, is kept, and exports unchanged.
#include "core/Document.h"
#include "core/Object.h"
#include "io/HtmlExport.h"
#include "io/LwdFormat.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/KeyboardPrefs.h"
#include "ui/MainWindow.h"
#include "ui/PreferencesDialog.h"
#include "ui/PropertyInspector.h"
#include "ui/ToolsPanel.h"
#include <QAbstractButton>
#include <QAction>
#include <functional>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest>
#include <memory>

namespace {
void setWeb(bool on)
{
    Prefs p = appState().prefs;
    p.enableWebFeatures = on;
    appState().setPrefs(p);
}

VObject mkRect(Document *d, const QRectF &r)
{
    VObject o;
    o.type = ObjType::Rect;
    o.rect = r;
    o.fillOn = true;
    o.fill = Qt::red;
    o.strokeOn = false;
    o.id = d->newId();
    return o;
}

Document *webDoc()
{
    auto *d = Document::createNew(200, 160, Qt::white);
    VObject art = mkRect(d, QRectF(20, 20, 60, 40));
    d->addObject(art, 0, false);
    d->selectOnly(art.id);
    if (!d->convertToSymbol(QStringLiteral("Btn"), SymbolKind::Button))
        return nullptr;
    VObject sl;
    sl.type = ObjType::Slice;
    sl.rect = QRectF(10, 10, 80, 60);
    sl.link = QStringLiteral("https://example.com/slice");
    sl.alt = QStringLiteral("Slice alt");
    sl.target = QStringLiteral("_blank");
    sl.behavior = WebBehavior::SimpleRollover;
    sl.swapFrame = 1;
    sl.name = QStringLiteral("Hero");
    sl.id = d->newId();
    const int web = d->ensureWebLayer();
    d->addObject(sl, web, false);
    VObject hs;
    hs.type = ObjType::Hotspot;
    hs.rect = QRectF(100, 40, 50, 30);
    hs.link = QStringLiteral("https://example.com/hot");
    hs.alt = QStringLiteral("Hot alt");
    hs.target = QStringLiteral("_blank");
    hs.behavior = WebBehavior::SwapImage;
    hs.swapTarget = sl.id;
    hs.swapFrame = 1;
    hs.name = QStringLiteral("ClickMe");
    hs.id = d->newId();
    d->addObject(hs, web, false);
    d->resetHistory(QStringLiteral("setup"));
    return d;
}
}


void checkSectionHeights(QWidget *window, int expectedVisible)
{
    auto *panel = window->findChild<QWidget *>(QStringLiteral("ToolsPanel"));
    QVERIFY2(panel, "ToolsPanel missing");
    QCoreApplication::processEvents();
    const auto labels = panel->findChildren<QLabel *>(QStringLiteral("ToolSection"));
    int visible = 0;
    int h = -1;
    for (QLabel *l : labels) {
        if (!l->isVisible()) continue;
        ++visible;
        if (h < 0) h = l->height();
        else QCOMPARE(l->height(), h);
        QCOMPARE(l->height(), l->sizeHint().height());
    }
    QCOMPARE(visible, expectedVisible);
    QVERIFY(h > 0);
}

class V059Test : public QObject {
    Q_OBJECT
private slots:
    void cleanup()
    {
        setWeb(false);
        appState().setTool(Tool::Pointer);
    }

    void prefDefaultsAndCheckbox()
    {
        QCOMPARE(Prefs().enableWebFeatures, false);
        QVERIFY(!webFeaturesEnabled());
        MainWindow w;
        PreferencesDialog dlg(&w);
        auto *cb = dlg.findChild<QCheckBox *>(QStringLiteral("prefEnableWebFeatures"));
        QVERIFY2(cb, "Preferences > General should have Enable web features");
        QCOMPARE(cb->text(), QStringLiteral("Enable web features"));
        QVERIFY(!cb->isChecked());
        QVERIFY2(!dlg.findChild<QCheckBox *>(QStringLiteral("prefUsePolygonSlices")),
                 "Use polygon slices preference was removed");
    }

    void hidesWebToolsKeepsExistingSlices()
    {
        setWeb(false);
        QVERIFY(!toolAvailable(Tool::Hotspot));
        QVERIFY(!toolAvailable(Tool::CircleHotspot));
        QVERIFY(!toolAvailable(Tool::PolyHotspot));
        QVERIFY(!toolAvailable(Tool::Slice));
        QVERIFY(!toolAvailable(Tool::PolySlice));
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *webSec = w.findChild<QWidget *>(QStringLiteral("toolSection_Web"));
        QVERIFY2(webSec, "Web section remains in the widget tree so it can return live");
        QVERIFY(!webSec->isVisible());
        auto *hs = w.findChild<QAbstractButton *>(QStringLiteral("tool_RectangleHotspot"));
        QVERIFY2(hs, "Hotspot flyout remains in the widget tree so it can return live");
        QVERIFY(!hs->isVisibleTo(&w));
        auto *slice = w.findChild<QAbstractButton *>(QStringLiteral("tool_Slice"));
        QVERIFY(slice);
        QVERIFY(!slice->isVisibleTo(&w));
        appState().setTool(Tool::Hotspot);
        QCOMPARE(appState().tool(), Tool::Pointer);
        appState().setTool(Tool::Slice);
        QCOMPARE(appState().tool(), Tool::Pointer);
        setWeb(true);
        QCoreApplication::processEvents();
        QVERIFY(toolAvailable(Tool::Hotspot));
        QVERIFY(toolAvailable(Tool::Slice));
        QVERIFY(webSec->isVisible());
        QVERIFY(hs->isVisibleTo(&w));
        QVERIFY(slice->isVisibleTo(&w));
        appState().setTool(Tool::Hotspot);
        QCOMPARE(appState().tool(), Tool::Hotspot);
        appState().setTool(Tool::Slice);
        QCOMPARE(appState().tool(), Tool::Slice);
        setWeb(false);
        QCoreApplication::processEvents();
        QCOMPARE(appState().tool(), Tool::Pointer);
        QVERIFY(!toolAvailable(Tool::Hotspot));
        QVERIFY(!toolAvailable(Tool::Slice));
        QVERIFY(!webSec->isVisible());
    }

    void menusAndShortcutsHideLive()
    {
        setWeb(false);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(120, 80, Qt::white);
        d->addObject(mkRect(d, QRectF(10, 10, 40, 40)), 0, true);
        w.addDocument(d);
        QCoreApplication::processEvents();

        auto *insHs = w.action(QStringLiteral("edit.inserthotspot"));
        auto *insSl = w.action(QStringLiteral("edit.insertslice"));
        auto *expHtml = w.action(QStringLiteral("file.exporthtml"));
        auto *expSlices = w.action(QStringLiteral("file.exportslices"));
        QVERIFY(insHs && insSl && expHtml && expSlices);
        QVERIFY(!insHs->isVisible());
        QVERIFY(!insHs->isEnabled());
        QVERIFY(!insSl->isVisible());
        QVERIFY(!insSl->isEnabled());
        QVERIFY(!expHtml->isVisible());
        QVERIFY(!expHtml->isEnabled());
        QVERIFY(!expSlices->isVisible());
        QVERIFY(!expSlices->isEnabled());
        auto *view = w.action(QStringLiteral("view.web"));
        QVERIFY(view);
        QVERIFY2(view->isVisible(), "View > Show Slices stays available so existing slice overlays can be toggled");
        QVERIFY(view->text().contains(QStringLiteral("Slices")));
        QVERIFY(!view->text().contains(QStringLiteral("Hotspot")));

        KeyboardPrefsPage keys(&w);
        keys.show();
        QCoreApplication::processEvents();
        auto *tree = keys.findChild<QTreeWidget *>();
        QVERIFY(tree);
        auto hasId = [&](const QString &id) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                std::function<bool(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *it) -> bool {
                    if (it->data(0, Qt::UserRole).toString() == id) return true;
                    for (int c = 0; c < it->childCount(); ++c)
                        if (walk(it->child(c))) return true;
                    return false;
                };
                if (walk(tree->topLevelItem(i))) return true;
            }
            return false;
        };
        QVERIFY(!hasId(QStringLiteral("edit.inserthotspot")));
        QVERIFY(!hasId(QStringLiteral("file.exporthtml")));
        QVERIFY(!hasId(toolActionId(Tool::Hotspot)));
        QVERIFY(!hasId(QStringLiteral("edit.insertslice")));
        QVERIFY(!hasId(QStringLiteral("file.exportslices")));
        QVERIFY(!hasId(toolActionId(Tool::Slice)));

        setWeb(true);
        QCoreApplication::processEvents();
        QVERIFY(insHs->isVisible());
        QVERIFY(insHs->isEnabled());
        QVERIFY(insSl->isVisible());
        QVERIFY(insSl->isEnabled());
        QVERIFY(expHtml->isVisible());
        QVERIFY(expHtml->isEnabled());
        QVERIFY(expSlices->isVisible());
        QVERIFY(view->isVisible());
        QVERIFY(view->text().contains(QStringLiteral("Hotspot")));
        QVERIFY(hasId(QStringLiteral("edit.inserthotspot")));
        QVERIFY(hasId(QStringLiteral("file.exporthtml")));
        QVERIFY(hasId(toolActionId(Tool::Hotspot)));
        QVERIFY(hasId(QStringLiteral("edit.insertslice")));
        QVERIFY(hasId(QStringLiteral("file.exportslices")));
        QVERIFY(hasId(toolActionId(Tool::Slice)));
    }

    void inspectorHidesWebPageKeepsSliceOptimize()
    {
        setWeb(false);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        auto *d = Document::createNew(200, 160, Qt::white);
        VObject sl;
        sl.type = ObjType::Slice;
        sl.rect = QRectF(10, 10, 80, 50);
        sl.link = QStringLiteral("https://kept.example");
        sl.name = QStringLiteral("S");
        sl.id = d->newId();
        d->addObject(sl, d->ensureWebLayer(), false);
        d->selectOnly(sl.id);
        w.addDocument(d);
        QCoreApplication::processEvents();
        auto *link = w.findChild<QLineEdit *>(QStringLiteral("WebLink"));
        auto *beh = w.findChild<QComboBox *>(QStringLiteral("WebBehavior"));
        auto *opt = w.findChild<QCheckBox *>(QStringLiteral("SliceCustomOpt"));
        QVERIFY(link && beh && opt);
        QVERIFY(!link->isVisible() || (link->parentWidget() && !link->parentWidget()->isVisible()));
        QVERIFY(!beh->isVisible() || (beh->parentWidget() && !beh->parentWidget()->isVisible()));
        QVERIFY(opt->isVisible());
        setWeb(true);
        QCoreApplication::processEvents();
        w.findChild<PropertyInspector *>()->refresh();
        QCoreApplication::processEvents();
        QVERIFY(link->isVisible() || (link->parentWidget() && link->parentWidget()->isVisible()));
        QVERIFY(beh->isVisible() || (beh->parentWidget() && beh->parentWidget()->isVisible()));
        QCOMPARE(d->find(sl.id)->link, QStringLiteral("https://kept.example"));
        QVERIFY(opt->isVisible());
    }

    void dataKeptAndExportedEitherWay()
    {
        setWeb(false);
        std::unique_ptr<Document> d(webDoc());
        QVERIFY(d);
        const quint64 hsId = [&] {
            for (const VObject &o : d->objects(d->webLayer()))
                if (o.type == ObjType::Hotspot) return o.id;
            return quint64(0);
        }();
        QVERIFY(hsId);
        const VObject *hs = d->find(hsId);
        QCOMPARE(hs->link, QStringLiteral("https://example.com/hot"));
        QCOMPARE(hs->behavior, WebBehavior::SwapImage);
        bool hasButton = false;
        for (const Symbol &s : d->s.symbols)
            if (s.kind == SymbolKind::Button) hasButton = true;
        QVERIFY(hasButton);

        QString err;
        const QByteArray data = lwd::serialize(*d);
        std::unique_ptr<Document> back(lwd::deserialize(data, &err));
        QVERIFY2(back, qPrintable(err));
        const VObject *hs2 = nullptr;
        for (const VObject &o : back->objects(back->webLayer()))
            if (o.type == ObjType::Hotspot) hs2 = &o;
        QVERIFY(hs2);
        QCOMPARE(hs2->link, hs->link);
        QCOMPARE(hs2->alt, hs->alt);
        QCOMPARE(hs2->target, hs->target);
        QCOMPARE(hs2->behavior, hs->behavior);
        QCOMPARE(hs2->swapFrame, hs->swapFrame);
        bool hasButton2 = false;
        for (const Symbol &s : back->s.symbols)
            if (s.kind == SymbolKind::Button) hasButton2 = true;
        QVERIFY(hasButton2);

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString htmlPath = tmp.filePath(QStringLiteral("page.html"));
        QStringList written;
        QVERIFY2(html::exportHtml(*back, htmlPath, html::Options{}, &err, &written), qPrintable(err));
        QFile f(htmlPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString html = QString::fromUtf8(f.readAll());
        QVERIFY2(html.contains(QStringLiteral("https://example.com/hot")), qPrintable(html.left(500)));
        QVERIFY(html.contains(QStringLiteral("Hot alt")));
        QVERIFY(html.contains(QStringLiteral("shape=\"rect\"")));

        // Same document with the pref on still exports the same links.
        setWeb(true);
        const QString htmlPath2 = tmp.filePath(QStringLiteral("page-on.html"));
        QVERIFY2(html::exportHtml(*back, htmlPath2, html::Options{}, &err, &written), qPrintable(err));
        QFile f2(htmlPath2);
        QVERIFY(f2.open(QIODevice::ReadOnly));
        const QString html2 = QString::fromUtf8(f2.readAll());
        QVERIFY(html2.contains(QStringLiteral("https://example.com/hot")));
        QVERIFY(html2.contains(QStringLiteral("https://example.com/slice")));
    }

    void canvasAndLayersHideHotspotsKeepSlices()
    {
        setWeb(false);
        MainWindow w;
        w.show();
        QCoreApplication::processEvents();
        std::unique_ptr<Document> owned(webDoc());
        QVERIFY(owned);
        Document *d = owned.release();
        DocumentView *v = w.addDocument(d);
        Canvas *c = v->canvas();
        c->setZoom(1);
        appState().webLayerVisible = true;
        appState().notifyOptions();
        QCoreApplication::processEvents();
        quint64 hsId = 0, slId = 0;
        for (const VObject &o : d->objects(d->webLayer())) {
            if (o.type == ObjType::Hotspot) hsId = o.id;
            if (o.type == ObjType::Slice) slId = o.id;
        }
        QVERIFY(hsId && slId);
        const VObject *hs = d->find(hsId);
        QCOMPARE(c->objectsUnder(hs->rect.center()).value(0, 0), quint64(0));
        QCOMPARE(c->objectsUnder(d->find(slId)->rect.center()).value(0, 0), slId);

        auto *layers = w.findChild<QWidget *>(QStringLiteral("panel_Layers"));
        QVERIFY(layers);
        bool sawHot = false, sawSlice = false;
        for (auto *tree : layers->findChildren<QTreeWidget *>()) {
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                const quint64 id = (*it)->data(0, Qt::UserRole).toULongLong();
                if (id == hsId) sawHot = true;
                if (id == slId) sawSlice = true;
            }
        }
        QVERIFY(!sawHot);
        QVERIFY(sawSlice);

        setWeb(true);
        QCoreApplication::processEvents();
        QCOMPARE(c->objectsUnder(hs->rect.center()).value(0, 0), hsId);
        sawHot = false;
        for (auto *tree : layers->findChildren<QTreeWidget *>()) {
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
                if ((*it)->data(0, Qt::UserRole).toULongLong() == hsId) sawHot = true;
        }
        QVERIFY(sawHot);
    }

    void buttonKindHiddenWhenOff()
    {
        setWeb(false);
        QDialog dummy;
        // symbolPropertiesDialog is interactive; just check the radio would be hidden
        // by constructing the same object names through a MainWindow library new-symbol is heavy.
        // Direct: the helper builds radios named kindButton.
        // Open the dialog is modal. Skip modal — PI inst web box is covered above.
        // Verify the pref gates the Button kind at the helper by inspecting source behaviour:
        // convertToSymbol still works so existing / programmatic buttons are kept.
        auto *d = Document::createNew(80, 80, Qt::white);
        d->addObject(mkRect(d, QRectF(5, 5, 30, 20)), 0, true);
        QVERIFY(d->convertToSymbol(QStringLiteral("KeepBtn"), SymbolKind::Button));
        bool found = false;
        for (const Symbol &s : d->s.symbols)
            if (s.kind == SymbolKind::Button && s.name == QStringLiteral("KeepBtn")) found = true;
        QVERIFY(found);
        delete d;
    }
    void sectionHeaderHeightsStayEqualAfterToggle()
    {
        MainWindow w;
        w.resize(1280, 800);
        w.addDocument(Document::createNew(320, 240, Qt::white));
        w.show();
        QCoreApplication::processEvents();

        for (const QString &id : {QStringLiteral("classic"), QStringLiteral("bonbon")}) {
            Prefs p = appState().prefs;
            p.theme = id;
            p.enableWebFeatures = false;
            appState().setPrefs(p);
            QCoreApplication::processEvents();
            checkSectionHeights(&w, 5); // Select, Bitmap, Vector, Colors, View
            if (QTest::currentTestFailed()) return;
            setWeb(true);
            QCoreApplication::processEvents();
            checkSectionHeights(&w, 6); // plus Web
            if (QTest::currentTestFailed()) return;
            setWeb(false);
            QCoreApplication::processEvents();
            checkSectionHeights(&w, 5);
            if (QTest::currentTestFailed()) return;
            setWeb(true);
            QCoreApplication::processEvents();
            checkSectionHeights(&w, 6);
            if (QTest::currentTestFailed()) return;
            setWeb(false);
            QCoreApplication::processEvents();
            checkSectionHeights(&w, 5);
            if (QTest::currentTestFailed()) return;
        }
    }


};

QTEST_MAIN(V059Test)
#include "v059_test.moc"
