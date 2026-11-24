// v0.3.7: every shortcut editable; editing built-in auto-forks; formerly hardcoded canvas keys rebind/remove.
#include "core/Document.h"
#include <QPainter>
#include "core/PixelEdit.h"
#include "core/Filters.h"
#include "core/Render.h"
#include "core/Effects.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/Shortcuts.h"
#include "ui/KeyboardPrefs.h"
#include "ui/PreferencesDialog.h"
#include <QAction>
#include <QLabel>
#include "ui/ToolsPanel.h"
#include <QAbstractItemView>
#include <QComboBox>
#include <QTemporaryDir>
#include "io/LwdFormat.h"
#include "ui/WideFontCombo.h"
#include <QFontComboBox>
#include <QScrollBar>
#include <QScreen>
#include <QWidget>
#include <QToolButton>
#include <QPushButton>
#include <QKeySequenceEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QTreeWidget>
#include <QtTest>
#include "Brand.h"

static void drainEvents() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

class V037 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;
    Document *d = nullptr;
    Canvas *cv = nullptr;

    void openDoc() {
        d = Document::createNew(200, 160, Qt::white);
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        cv->setFocus(Qt::OtherFocusReason);
        drainEvents();
    }
    void closeDoc() {
        if (d) d->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        drainEvents();
        d = nullptr;
        cv = nullptr;
    }
    void sendKey(int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QKeyEvent press(QEvent::KeyPress, key, mods);
        QApplication::sendEvent(cv, &press);
        QKeyEvent release(QEvent::KeyRelease, key, mods);
        QApplication::sendEvent(cv, &release);
        drainEvents();
    }

private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        // isolate shortcut sets from the developer's QSettings
        QSettings st;
        st.beginGroup(QStringLiteral("shortcuts"));
        st.remove(QString());
        st.endGroup();
        w = std::make_unique<MainWindow>();
        w->resize(1000, 800);
        w->show();
        drainEvents();
    }
    void cleanupTestCase() { w.reset(); }

    void everyCatalogActionEditable() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        QVERIFY(!sm.catalog().isEmpty());
        sm.setCurrent(brand::nameLower());
        QVERIFY(sm.set(sm.currentId())->readOnly);
        for (const shortcuts::ActionInfo &a : sm.catalog()) {
            const QList<QKeySequence> before = sm.sequences(a.id);
            // set to a unique throwaway, then restore — must succeed (auto-fork on first edit)
            const bool forked = sm.setSequences(a.id, {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+F12"))});
            Q_UNUSED(forked);
            QCOMPARE(sm.sequences(a.id).value(0).toString(), QStringLiteral("Ctrl+Alt+Shift+F12"));
            QVERIFY(!sm.set(sm.currentId())->readOnly);
            sm.setSequences(a.id, before);
        }
        // tear down custom set so later tests start clean
        const QString custom = sm.currentId();
        sm.setCurrent(brand::nameLower());
        if (custom != brand::nameLower()) sm.remove(custom);
    }

    void editingBuiltinAutoForks() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        QVERIFY(sm.set(brand::nameLower())->readOnly);
        QCOMPARE(sm.currentId(), brand::nameLower());
        const bool forked = sm.setSequences(QStringLiteral("file.new"), {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+N"))});
        QVERIFY(forked);
        QCOMPARE(sm.set(sm.currentId())->name, brand::name() + QStringLiteral(" (custom)"));
        QVERIFY(!sm.set(sm.currentId())->readOnly);
        QCOMPARE(sm.sequences(QStringLiteral("file.new")).value(0).toString(), QStringLiteral("Ctrl+Alt+Shift+N"));
        // second fork while built-in active again → custom 2
        const QString firstCustom = sm.currentId();
        sm.setCurrent(brand::nameLower());
        QVERIFY(sm.setSequences(QStringLiteral("file.open"), {QKeySequence(QStringLiteral("Ctrl+Alt+Shift+O"))}));
        QCOMPARE(sm.set(sm.currentId())->name, brand::name() + QStringLiteral(" (custom 2)"));
        const QString secondCustom = sm.currentId();
        // built-in unchanged
        sm.setCurrent(brand::nameLower());
        QCOMPARE(sm.sequences(QStringLiteral("file.new")).value(0).toString(), QStringLiteral("Ctrl+N"));
        // capture / + / − always enabled on the built-in set; rename stays off
        PreferencesDialog dlg(w.get(), 4);
        auto *page = dlg.findChild<KeyboardPrefsPage *>();
        QVERIFY(page);
        auto *capture = page->findChild<QKeySequenceEdit *>(QStringLiteral("prefShortcutCapture"));
        auto *add = page->findChild<QPushButton *>(QStringLiteral("prefShortcutAdd"));
        auto *rem = page->findChild<QPushButton *>(QStringLiteral("prefShortcutRemove"));
        auto *ren = page->findChild<QPushButton *>(QStringLiteral("prefShortcutRename"));
        auto *warn = page->findChild<QLabel *>(QStringLiteral("prefShortcutWarn"));
        QVERIFY(capture && add && rem && ren && warn);
        QVERIFY(capture->isEnabled());
        QVERIFY(add->isEnabled());
        QVERIFY(rem->isEnabled());
        QVERIFY(!ren->isEnabled()); // built-in not renameable
        QVERIFY(warn->text().contains(QStringLiteral("custom")));
        sm.remove(firstCustom);
        sm.remove(secondCustom);
        sm.setCurrent(brand::nameLower());
    }

    void formerlyHardcodedReboundAndRemoved() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        // ensure a writable set
        if (sm.set(sm.currentId())->readOnly)
            sm.duplicate(brand::nameLower(), QStringLiteral("v037-rebind"));
        openDoc();
        VObject o; o.type = ObjType::Rect; o.rect = QRectF(40, 40, 60, 40); o.id = d->newId(); o.fillOn = true;
        d->addObject(o, 0, true);
        d->commit(QStringLiteral("add"));
        drainEvents();
        QCOMPARE(d->find(o.id)->rect.topLeft(), QPointF(40, 40));

        // default Left nudges 1 px
        sendKey(Qt::Key_Left);
        QCOMPARE(d->find(o.id)->rect.topLeft(), QPointF(39, 40));

        // rebind nudgeLeft to N, remove Left
        sm.setSequences(QStringLiteral("canvas.nudgeLeft"), {QKeySequence(Qt::Key_F9)});
        sm.apply(w.get());
        sendKey(Qt::Key_Left); // should no longer nudge
        QCOMPARE(d->find(o.id)->rect.topLeft(), QPointF(39, 40));
        sendKey(Qt::Key_F9);
        QCOMPARE(d->find(o.id)->rect.topLeft(), QPointF(38, 40));

        // remove binding entirely
        sm.setSequences(QStringLiteral("canvas.nudgeLeft"), {});
        sm.apply(w.get());
        sendKey(Qt::Key_F9);
        QCOMPARE(d->find(o.id)->rect.topLeft(), QPointF(38, 40));

        // Escape cancel clears selection (canvas.cancel)
        QVERIFY(!d->selection.isEmpty());
        sendKey(Qt::Key_Escape);
        QVERIFY(d->selection.isEmpty());

        // rebind cancel to C
        d->selectOnly(o.id);
        sm.setSequences(QStringLiteral("canvas.cancel"), {QKeySequence(Qt::Key_F10)});
        sm.apply(w.get());
        sendKey(Qt::Key_Escape);
        QVERIFY(!d->selection.isEmpty()); // Escape unbound
        sendKey(Qt::Key_F10);
        QVERIFY(d->selection.isEmpty());

        // colors.swap / tempPan / confirm registered
        for (const char *id : {"canvas.tempPan", "canvas.confirm", "canvas.nudgeUp10", "colors.swap",
                               "canvas.removeLastPoint", "view.zoomin", "tool.Pointer"}) {
            bool found = false;
            for (const shortcuts::ActionInfo &a : sm.catalog())
                if (a.id == QLatin1String(id)) { found = true; break; }
            QVERIFY2(found, id);
        }

        // HTML export includes Miscellaneous + Modifiers
        const QString html = sm.toHtml();
        QVERIFY(html.contains(QStringLiteral("Miscellaneous")));
        QVERIFY(html.contains(QStringLiteral("Modifiers")));
        QVERIFY(html.contains(QStringLiteral("Nudge Left")) || html.contains(QStringLiteral("canvas.nudgeLeft"))
                || html.contains(QStringLiteral("Temporary Pan")));

        closeDoc();
        // restore defaults via built-in
        sm.setCurrent(brand::nameLower());
        for (const shortcuts::Set &s : sm.sets())
            if (!s.readOnly) sm.remove(s.id);
        sm.apply(w.get());
    }

    void toolTipsMatchRegistry() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.setCurrent(brand::nameLower());
        // Blur (and every R-group tool) must list R in the Tools catalog
        for (const char *id : {"tool.Blur", "tool.Sharpen", "tool.Dodge", "tool.Burn", "tool.Smudge"}) {
            bool found = false;
            for (const shortcuts::ActionInfo &a : sm.catalog())
                if (a.id == QLatin1String(id) && a.category == QLatin1String("tools")) { found = true; break; }
            QVERIFY2(found, id);
            const QList<QKeySequence> seqs = sm.sequences(QLatin1String(id));
            QVERIFY2(!seqs.isEmpty() && seqs.first().toString() == QStringLiteral("R"), id);
        }
        // rebind Blur's key → tips / flyout text follow
        if (sm.set(sm.currentId())->readOnly)
            sm.duplicate(brand::nameLower(), QStringLiteral("v037-tips"));
        sm.setSequences(QStringLiteral("tool.Blur"), {QKeySequence(QStringLiteral("F8"))});
        sm.apply(w.get());
        // 0.3.11+: flyout mates keep their own sequences after a rebind (Blur alone becomes F8)
        QCOMPARE(sm.sequences(QStringLiteral("tool.Blur")).value(0).toString(), QStringLiteral("F8"));
        QCOMPARE(sm.sequences(QStringLiteral("tool.Sharpen")).value(0).toString(), QStringLiteral("R"));
        auto *blurBtn = w->findChild<QToolButton *>(QStringLiteral("tool_Blur"));
        QVERIFY(blurBtn);
        QVERIFY2(blurBtn->toolTip().contains(QStringLiteral("F8")), qPrintable(blurBtn->toolTip()));
        if (auto *fb = qobject_cast<FlyoutToolButton *>(blurBtn)) {
            bool saw = false;
            for (QAction *a : fb->flyout()->actions())
                if (a->text().contains(QStringLiteral("Blur")) && a->text().contains(QStringLiteral("F8"))) saw = true;
            QVERIFY(saw);
        }
        sm.setCurrent(brand::nameLower());
        for (const shortcuts::Set &s : sm.sets()) if (!s.readOnly) sm.remove(s.id);
        sm.apply(w.get());
    }

    void modifiersListedNotInCatalog() {
        const auto notes = shortcuts::Manager::modifierNotes();
        QVERIFY(notes.size() >= 5);
        for (const shortcuts::ActionInfo &a : shortcuts::Manager::get().catalog())
            QVERIFY(!a.label.contains(QStringLiteral("not editable")));
    }

    void fontPopupFitsWidestName() {
        QFontComboBox *font = w->findChild<WideFontComboBox *>();
        if (!font) font = w->findChild<QFontComboBox *>();
        QVERIFY2(font, "expected a font combo on the PI / tool options");
        // Ensure the Text tool options (with a font combo) are visible
        appState().setTool(Tool::Text);
        drainEvents();
        font = w->findChild<WideFontComboBox *>();
        if (!font) font = w->findChild<QFontComboBox *>();
        QVERIFY(font);
        font->show();
        drainEvents();
        const int comboW = qMax(font->width(), font->sizeHint().width());
        QVERIFY(comboW > 40);
        font->resize(comboW, font->height()); // stable width for the factor check
        const int lockedW = font->width();
        font->showPopup();
        drainEvents();
        QAbstractItemView *view = font->view();
        QVERIFY(view);
        QWidget *container = view->window();
        QVERIFY(container);
        const int got = qMax(view->width(), container->width());
        const int minFactor = qRound(lockedW * kFontPopupMinFactor);
        const int want = widenFontPopup(font);
        QVERIFY2(want >= minFactor,
                 qPrintable(QStringLiteral("widenFontPopup returned %1 < %2 (combo %3, view %4)").arg(want).arg(minFactor).arg(lockedW).arg(got)));
        QCOMPARE(font->width(), lockedW); // combo itself unchanged
        font->hidePopup();
    }

    void toolEnvRoundTripAndPerDocument() {
        Prefs prefs = appState().prefs;
        prefs.carryToolSettings = false;
        appState().setPrefs(prefs);

        // Doc A: change brush size
        d = Document::createNew(120, 80, Qt::white);
        w->addDocument(d);
        drainEvents();
        appState().setTool(Tool::Brush);
        appState().brushSize = 42;
        appState().stroke = QColor(12, 34, 56);
        appState().notifyOptions();
        drainEvents();
        QVERIFY(!d->toolEnv.isEmpty());
        QCOMPARE(d->toolEnv.value(QStringLiteral("options")).toObject().value(QStringLiteral("brushSize")).toInt(), 42);

        // round-trip through .lwd
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("env.lwd"));
        QString err;
        d->toolEnv = appState().captureEnvironment();
        QVERIFY(lwd::save(*d, path, &err));
        Document *loaded = lwd::load(path, &err);
        QVERIFY2(loaded, qPrintable(err));
        QVERIFY(!loaded->toolEnv.isEmpty());
        QCOMPARE(loaded->toolEnv.value(QStringLiteral("options")).toObject().value(QStringLiteral("brushSize")).toInt(), 42);
        QCOMPARE(QColor(loaded->toolEnv.value(QStringLiteral("stroke")).toString()), QColor(12, 34, 56));
        delete loaded;

        // Doc B (new, carry OFF): must get defaults, not A's brush size
        Document *b = Document::createNew(100, 60, Qt::white);
        QVERIFY(b->toolEnv.isEmpty());
        w->addDocument(b);
        drainEvents();
        QCOMPARE(appState().brushSize, AppState::defaultSlot(Tool::Brush).value(QStringLiteral("brushSize")).toInt());

        // switch back to A: restore 42
        b->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        drainEvents();
        QVERIFY(w->currentDoc() == d);
        QCOMPARE(appState().brushSize, 42);

        d->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        drainEvents();
        d = nullptr;
    }

    void carryToolSettingsBetweenDocuments() {
        Prefs prefs = appState().prefs;
        prefs.carryToolSettings = true;
        appState().setPrefs(prefs);
        appState().savePrefs();

        d = Document::createNew(80, 60, Qt::white);
        w->addDocument(d);
        drainEvents();
        appState().setTool(Tool::Brush);
        appState().brushSize = 55;
        appState().notifyOptions();
        drainEvents();
        QCOMPARE(appState().brushSize, 55);

        // New doc with carry ON: inherits 55
        Document *b = Document::createNew(90, 70, Qt::white);
        w->addDocument(b);
        drainEvents();
        QCOMPARE(appState().brushSize, 55);
        QVERIFY(!b->toolEnv.isEmpty());
        QCOMPARE(b->toolEnv.value(QStringLiteral("options")).toObject().value(QStringLiteral("brushSize")).toInt(), 55);

        // .lwd with its own env still wins
        Document *fromFile = Document::createNew(50, 50, Qt::white);
        fromFile->toolEnv = AppState::defaultEnvironment();
        QJsonObject opts = fromFile->toolEnv.value(QStringLiteral("options")).toObject();
        opts.insert(QStringLiteral("brushSize"), 11);
        fromFile->toolEnv.insert(QStringLiteral("options"), opts);
        fromFile->toolEnv.insert(QStringLiteral("tool"), int(Tool::Brush));
        // also put 11 in Brush slot
        QJsonObject slotMap = fromFile->toolEnv.value(QStringLiteral("slots")).toObject();
        QJsonObject brush = slotMap.value(QStringLiteral("Brush")).toObject();
        brush.insert(QStringLiteral("brushSize"), 11);
        slotMap.insert(QStringLiteral("Brush"), brush);
        fromFile->toolEnv.insert(QStringLiteral("slots"), slotMap);
        w->addDocument(fromFile);
        drainEvents();
        QCOMPARE(appState().brushSize, 11);

        // carried QSettings updated to the loaded file's state
        appState().saveCarriedEnvironment();
        Prefs off = appState().prefs;
        off.carryToolSettings = false;
        appState().setPrefs(off);

        fromFile->markClean(); b->markClean(); d->markClean();
        while (w->currentDoc()) {
            w->currentDoc()->markClean();
            w->action(QStringLiteral("file.close"))->trigger();
            drainEvents();
        }
        d = nullptr;

        // restore prefs for later tests
        prefs.carryToolSettings = false;
        appState().setPrefs(prefs);
        appState().applyEnvironment({});
    }


    void magicWandIgnoresOverlappingText() {
        // Bitmap under black text: wand must sample the bitmap only, so the text outline is not
        // cut into the selection (the bug that looked like a Glow "leak" after Copy).
        closeDoc();
        d = Document::createNew(200, 120, Qt::white);
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));

        VObject bm;
        bm.type = ObjType::Bitmap;
        bm.id = d->newId();
        bm.rect = QRectF(0, 0, 200, 120);
        bm.image = QImage(200, 120, QImage::Format_ARGB32_Premultiplied);
        bm.image.fill(qRgb(40, 80, 200));   // solid blue
        d->addObject(bm, 0, false);

        VObject tx;
        tx.type = ObjType::Text;
        tx.id = d->newId();
        tx.rect = QRectF(40, 40, 120, 40);
        tx.text = QStringLiteral("HELLO");
        tx.fontFamily = QStringLiteral("DejaVu Sans");
        tx.fontSize = 28;
        tx.bold = true;
        tx.fillOn = true;
        tx.fill = Qt::black;
        d->addObject(tx, 0, false);
        d->commit(QStringLiteral("setup"));
        d->setSelection({bm.id});

        // Sanity: composite under a glyph is not pure blue (text covers it)
        QImage composite = d->render(0, true);
        bool textCovers = false;
        for (int y = 45; y < 75 && !textCovers; ++y)
            for (int x = 50; x < 150; ++x) {
                const QColor c = composite.pixelColor(x, y);
                if (c.red() < 30 && c.green() < 30 && c.blue() < 30) { textCovers = true; break; }
            }
        QVERIFY2(textCovers, "test setup: text should cover some bitmap pixels in the composite");

        appState().tolerance = 32;
        appState().contiguous = true;
        appState().wandSampleAllLayers = false;
        appState().wandEdge = 0;
        appState().setTool(Tool::MagicWand);
        drainEvents();

        // Click on a blue pixel that the composite shows as text-black (through the glyph)
        QPointF throughGlyph;
        bool found = false;
        for (int y = 45; y < 75 && !found; ++y)
            for (int x = 50; x < 150; ++x) {
                const QColor c = composite.pixelColor(x, y);
                if (c.red() < 30 && c.green() < 30 && c.blue() < 30) {
                    throughGlyph = QPointF(x + 0.5, y + 0.5);
                    found = true;
                    break;
                }
            }
        QVERIFY(found);

        auto sendClick = [&](QPointF docPt) {
            const QPointF vp = cv->toView(docPt);
            auto send = [&](QEvent::Type ty, Qt::MouseButton b, Qt::MouseButtons bs) {
                QMouseEvent ev(ty, vp, cv->mapToGlobal(vp.toPoint()), b, bs, Qt::NoModifier);
                QApplication::sendEvent(cv, &ev);
            };
            send(QEvent::MouseMove, Qt::NoButton, Qt::NoButton);
            send(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
            drainEvents();
        };
        sendClick(throughGlyph);

        QVERIFY(!d->pixelSelection.isEmpty());
        const QImage mask = d->canvasSelectionMask(QRect(0, 0, 200, 120));
        // Every blue bitmap pixel should be selected; none of the mask should be "text-shaped holes"
        // Under the glyph, the bitmap is still blue, so those mask pixels must be on.
        int hole = 0, on = 0;
        for (int y = 0; y < 120; ++y) {
            const uchar *m = mask.constScanLine(y);
            for (int x = 0; x < 200; ++x) {
                if (m[x] >= 128) ++on;
                const QColor c = composite.pixelColor(x, y);
                if (c.red() < 30 && c.green() < 30 && c.blue() < 30 && m[x] < 128) ++hole;
            }
        }
        QVERIFY2(hole == 0, qPrintable(QStringLiteral("text-shaped holes in wand mask: %1").arg(hole)));
        QVERIFY2(on >= 200 * 120 - 10, qPrintable(QStringLiteral("expected nearly full bitmap selected, got %1").arg(on)));

        // Sample-all-layers ON reproduces the old composite behaviour (holes under text)
        d->clearPixelSelection();
        appState().wandSampleAllLayers = true;
        sendClick(QPointF(10.5, 10.5));   // pure blue in composite (away from text)
        const QImage bad = d->canvasSelectionMask(QRect(0, 0, 200, 120));
        int holes2 = 0;
        for (int y = 45; y < 75; ++y) {
            const uchar *m = bad.constScanLine(y);
            for (int x = 50; x < 150; ++x) {
                const QColor c = composite.pixelColor(x, y);
                if (c.red() < 30 && c.green() < 30 && c.blue() < 30 && m[x] < 128) ++holes2;
            }
        }
        QVERIFY2(holes2 > 10, "sample-all-layers should cut text out of a contiguous flood from blue");
        appState().wandSampleAllLayers = false;
        appState().setTool(Tool::Pointer);
        d->markClean();
        closeDoc();
    }

    void glowOnBitmapDoesNotAlterDistantText() {
        // Independent check of the original "glow leak" hypothesis: Glow on a bitmap must not
        // put effects on a separate text object, and must not change that text when rendered alone.
        closeDoc();
        std::unique_ptr<Document> doc(Document::createNew(300, 200, Qt::white));
        VObject bm;
        bm.type = ObjType::Bitmap; bm.id = doc->newId(); bm.rect = QRectF(10, 10, 50, 50);
        bm.image = QImage(50, 50, QImage::Format_ARGB32_Premultiplied);
        bm.image.fill(qRgba(0, 0, 200, 255));
        doc->addObject(bm, 0, false);
        VObject tx;
        tx.type = ObjType::Text; tx.id = doc->newId(); tx.rect = QRectF(180, 80, 100, 40);
        tx.text = QStringLiteral("Hello"); tx.fontFamily = QStringLiteral("DejaVu Sans"); tx.fontSize = 24;
        tx.fillOn = true; tx.fill = Qt::black;
        doc->addObject(tx, 0, false);
        doc->commit(QStringLiteral("setup"));
        const quint64 bmId = bm.id, txId = tx.id;

        QImage beforeAlone;
        {
            SymbolScope scope(&doc->s.symbols);
            beforeAlone = render::renderObject(*doc->find(txId), nullptr, QRect(-1000, -1000, 4000, 4000));
        }
        LiveEffect glow = LiveEffect::make(EffectType::OuterGlow);
        glow.width = 12; glow.softness = 10; glow.opacity = 100; glow.color = Qt::red;
        doc->find(bmId)->effects.append(glow);
        doc->commit(QStringLiteral("glow"));

        QCOMPARE(doc->find(txId)->effects.size(), 0);
        QCOMPARE(doc->find(bmId)->effects.size(), 1);
        QImage afterAlone;
        {
            SymbolScope scope(&doc->s.symbols);
            afterAlone = render::renderObject(*doc->find(txId), nullptr, QRect(-1000, -1000, 4000, 4000));
        }
        QCOMPARE(afterAlone.size(), beforeAlone.size());
        QCOMPARE(afterAlone, beforeAlone);

        // undo / redo keep text effect-free
        doc->undo();
        QCOMPARE(doc->find(bmId)->effects.size(), 0);
        QCOMPARE(doc->find(txId)->effects.size(), 0);
        doc->redo();
        QCOMPARE(doc->find(bmId)->effects.size(), 1);
        QCOMPARE(doc->find(txId)->effects.size(), 0);

        // save / load
        QByteArray bytes = lwd::serialize(*doc);
        std::unique_ptr<Document> loaded(lwd::deserialize(bytes));
        QVERIFY(loaded);
        quint64 lBm = 0, lTx = 0;
        for (const VObject &o : loaded->objects(0)) {
            if (o.type == ObjType::Bitmap) lBm = o.id;
            if (o.type == ObjType::Text) lTx = o.id;
        }
        QVERIFY(lBm && lTx);
        QCOMPARE(loaded->find(lBm)->effects.size(), 1);
        QCOMPARE(loaded->find(lTx)->effects.size(), 0);
    }

    void fontPopupScrollbarOnScreen() {
        QScreen *sc = QApplication::primaryScreen();
        QVERIFY(sc);
        const QRect avail = sc->availableGeometry();

        auto checkPopup = [&](QComboBox *combo, const char *tag) {
            combo->showPopup();
            drainEvents();
            QTest::qWait(30);
            drainEvents();
            QAbstractItemView *view = combo->view();
            QVERIFY2(view, tag);
            QWidget *container = view->window();
            QVERIFY2(container && container != combo->window(), tag);
            const QRect cGeom = container->frameGeometry();
            QVERIFY2(avail.contains(cGeom),
                     qPrintable(QStringLiteral("%1 container %2,%3 %4x%5 not in avail %6,%7 %8x%9")
                                    .arg(QString::fromLatin1(tag))
                                    .arg(cGeom.x()).arg(cGeom.y()).arg(cGeom.width()).arg(cGeom.height())
                                    .arg(avail.x()).arg(avail.y()).arg(avail.width()).arg(avail.height())));
            QScrollBar *sb = view->verticalScrollBar();
            QVERIFY2(sb, tag);
            // A long list must reserve a scrollbar whose global rect stays on-screen and inside
            // the container. Offscreen backends sometimes report isVisible()==false even when
            // the range is active, so the range + geometry are the source of truth.
            if (combo->count() >= 40)
                QVERIFY2(sb->maximum() > 0 || sb->size().height() > 0,
                         qPrintable(QStringLiteral("%1 expected a scrollbar range on a long list").arg(QString::fromLatin1(tag))));
            if (sb->width() > 0 && sb->height() > 0) {
                const QRect sbGlobal(sb->mapToGlobal(QPoint(0, 0)), sb->size());
                QVERIFY2(container->frameGeometry().adjusted(-2, -2, 2, 2).contains(sbGlobal),
                         qPrintable(QStringLiteral("%1 scrollbar %2,%3 %4x%5 outside container")
                                        .arg(QString::fromLatin1(tag))
                                        .arg(sbGlobal.x()).arg(sbGlobal.y()).arg(sbGlobal.width()).arg(sbGlobal.height())));
                QVERIFY2(avail.contains(sbGlobal) || avail.intersects(sbGlobal),
                         qPrintable(QStringLiteral("%1 scrollbar off screen").arg(QString::fromLatin1(tag))));
                QVERIFY2(avail.right() >= sbGlobal.right() && avail.left() <= sbGlobal.left()
                         && avail.top() <= sbGlobal.top() && avail.bottom() >= sbGlobal.bottom(),
                         qPrintable(QStringLiteral("%1 scrollbar %2,%3 %4x%5 not inside avail")
                                        .arg(QString::fromLatin1(tag))
                                        .arg(sbGlobal.x()).arg(sbGlobal.y()).arg(sbGlobal.width()).arg(sbGlobal.height())));
            }
            QCOMPARE(combo->width(), 150);
            combo->hidePopup();
            drainEvents();
        };

        QWidget host;
        host.resize(200, 80);
        auto *font = new WideFontComboBox(&host);
        font->setFixedWidth(150);
        font->move(0, 0);
        host.move(avail.right() - host.width(), avail.top() + 40);
        host.show();
        drainEvents();
        QVERIFY(font->count() > 0);
        checkPopup(font, "WideFontComboBox right edge");

        host.move(avail.right() - host.width(), avail.bottom() - host.height());
        drainEvents();
        checkPopup(font, "WideFontComboBox bottom-right");

        auto *plain = new WidePopupComboBox(&host);
        for (int i = 0; i < 80; ++i)
            plain->addItem(QStringLiteral("Item %1 with a fairly long name for the popup").arg(i));
        plain->setFixedWidth(150);
        plain->move(0, 30);
        host.move(avail.right() - host.width(), avail.top() + 40);
        drainEvents();
        checkPopup(plain, "WidePopupComboBox right edge");
    }

};

QTEST_MAIN(V037)
#include "v037_test.moc"
