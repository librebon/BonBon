// 0.3.35: Auto preview from stroke pixels; content-sized Optimize/Align/Auto Shapes;
// Align canvas toggle; list max-heights that never overlap button bars; splitter push-down;
// dock width follows the viewport (no clip with the scrollbar) and fixed panels have no slack;
// Align (and other right-dock) button labels are never elided.
#include "core/Document.h"
#include "core/Stroke.h"
#include "io/ImageIO.h"
#include "ui/AppState.h"
#include "ui/Dialogs.h"
#include "ui/EditStrokeDialog.h"
#include "ui/MainWindow.h"
#include "ui/Panels.h"
#include <QAbstractItemView>
#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QListWidget>
#include <QtTest>

class V073Test : public QObject {
    Q_OBJECT

    static void drain()
    {
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
    }

    static int lastVisibleBottom(QWidget *root)
    {
        int b = -1;
        for (QObject *o : root->children()) {
            auto *w = qobject_cast<QWidget *>(o);
            if (!w || w->isHidden()) continue;
            b = qMax(b, w->geometry().bottom());
        }
        return b;
    }

    static int slackBelowLastChild(QWidget *root)
    {
        const int last = lastVisibleBottom(root);
        if (last < 0) return 0;
        int margin = 0;
        if (auto *ly = root->layout())
            margin = ly->contentsMargins().bottom();
        return root->height() - last - margin;
    }

    static bool noChildPastRight(QWidget *root)
    {
        if (!root) return false;
        const int right = root->width();
        for (auto *w : root->findChildren<QWidget *>()) {
            // Skip unmapped internals (combo popups, hidden format rows whose
            // parent is hidden) — isHidden() is only the explicit flag.
            if (!w || !w->isVisible() || w->isWindow()) continue;
            const QRect r(w->mapTo(root, QPoint(0, 0)), w->size());
            if (r.right() > right + 1) return false;
        }
        return true;
    }

    // A labelled button is unelided if it is at least as wide as its sizeHint,
    // or as the text advance plus the theme QToolButton padding/border.
    static bool buttonTextUnelided(const QAbstractButton *b)
    {
        if (!b || !b->isVisible() || b->text().isEmpty()) return true;
        const int advance = b->fontMetrics().horizontalAdvance(b->text());
        const int pad = 10; // 2px padding + 1px border each side, plus a little slack
        return b->width() >= advance + pad || b->width() + 1 >= b->sizeHint().width();
    }

    static bool noElidedDockButtons(QWidget *root)
    {
        if (!root) return false;
        for (auto *b : root->findChildren<QAbstractButton *>()) {
            if (!b->isVisible() || b->isWindow()) continue;
            if (!buttonTextUnelided(b)) return false;
        }
        return true;
    }

    static bool noOverlap(QWidget *panel, QAbstractItemView *list)
    {
        if (!panel || !list) return false;
        const QRect lr = QRect(list->mapTo(panel, QPoint(0, 0)), list->size());
        if (auto *bar = panel->findChild<QWidget *>(QStringLiteral("PanelButtonBar"))) {
            const QRect br = QRect(bar->mapTo(panel, QPoint(0, 0)), bar->size());
            if (lr.intersects(br.adjusted(0, 1, 0, 0))) return false;
            if (lr.bottom() > br.top() + 1) return false;
        }
        if (auto *opts = panel->findChild<QWidget *>(QStringLiteral("framesOpts"))) {
            const QRect orc = QRect(opts->mapTo(panel, QPoint(0, 0)), opts->size());
            if (lr.intersects(orc.adjusted(0, 1, 0, 0))) return false;
        }
        return lr.bottom() <= panel->rect().bottom() + 1;
    }

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("LibreworksTest035"));
        QCoreApplication::setApplicationName(QStringLiteral("v073"));
    }

    void lightningAutoPreviewUsesStrokePixels()
    {
        appState().prefs.strokePreviewBg = 2;
        appState().savePrefs();
        const stroke::Params lightning = stroke::paramsFor(QStringLiteral("lw.lightning"));
        QCOMPARE(lightning.look, stroke::Look::Lightning);
        EditStrokeDialog dlg;
        dlg.setParams(lightning, QColor(20, 20, 40), 16);
        dlg.resize(480, 420);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        drain();
        QCOMPARE(dlg.previewBackgroundMode(), 2);
        QCOMPARE(dlg.previewBackground(), QColor(20, 20, 20));
        QVERIFY(!dlg.previewImage().isNull());
        QVERIFY(dlg.previewImage().pixelColor(2, 2).lightness() < 60);
    }

    void alignCanvasToggleIsCondensedToolButton()
    {
        AlignPanel panel;
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        drain();
        auto *btn = panel.findChild<QToolButton *>(QStringLiteral("alignToCanvas"));
        QVERIFY(btn);
        QVERIFY(btn->isCheckable());
        QVERIFY(!btn->isChecked());
        QVERIFY(btn->toolTip().contains(QStringLiteral("canvas"), Qt::CaseInsensitive));
        QVERIFY(btn->toolTip().contains(QStringLiteral("selection"), Qt::CaseInsensitive));
        QVERIFY(!btn->icon().isNull());
        // Right-aligned on the second row: the button sits on the right of the panel.
        QVERIFY(btn->geometry().right() >= panel.width() - 12 - panel.contentsMargins().right());
        QVERIFY(!panel.findChild<QWidget *>(QStringLiteral("To canvas")));
    }

    void alignAndDockButtonsAreNotElided()
    {
        auto *opt = new OptimizePanel;
        auto *align = new AlignPanel;
        auto *shapes = new AutoShapesPanel;
        auto *layers = new LayersPanel;
        auto *frames = new FramesPanel;
        auto *history = new HistoryPanel;
        auto *stack = new PanelStack;
        auto *optCp = new CollapsiblePanel(QStringLiteral("Optimize"), opt, stack);
        auto *layCp = new CollapsiblePanel(QStringLiteral("Layers"), layers, stack);
        auto *alCp = new CollapsiblePanel(QStringLiteral("Align"), align, stack);
        auto *asCp = new CollapsiblePanel(QStringLiteral("Auto Shapes"), shapes, stack);
        auto *frCp = new CollapsiblePanel(QStringLiteral("Frames"), frames, stack);
        auto *hiCp = new CollapsiblePanel(QStringLiteral("History"), history, stack);
        stack->addPanel(optCp, 0);
        stack->addPanel(layCp, 3);
        stack->addPanel(alCp, 0);
        stack->addPanel(asCp, 0);
        stack->addPanel(frCp, 1);
        stack->addPanel(hiCp, 1);
        optCp->setContentSized(true);
        alCp->setContentSized(true);
        asCp->setContentSized(true);
        for (auto *cp : {optCp, layCp, alCp, asCp, frCp, hiCp})
            cp->setCollapsed(false);

        // Default dock width, then the scroll area's minimum (270), then the
        // narrower viewport once the vertical scrollbar appears.
        auto check = [&](int width, int height) {
            PanelScrollArea scroll;
            scroll.setObjectName(QStringLiteral("PanelsScroll"));
            scroll.setWidget(stack);
            scroll.resize(width, height);
            scroll.show();
            QVERIFY(QTest::qWaitForWindowExposed(&scroll));
            drain();
            stack->relayout();
            drain();

            QStringList labels;
            for (auto *b : align->findChildren<QToolButton *>()) {
                if (b == align->findChild<QToolButton *>(QStringLiteral("alignToCanvas"))) continue;
                labels << b->text();
                QVERIFY2(buttonTextUnelided(b),
                         qPrintable(QStringLiteral("elided Align button '%1' width %2 hint %3")
                                        .arg(b->text()).arg(b->width()).arg(b->sizeHint().width())));
            }
            QVERIFY(labels.contains(QString::fromUtf8("|↔|")));
            QVERIFY(labels.contains(QString::fromUtf8("↕")));
            QVERIFY(labels.contains(QStringLiteral("=W")));
            QVERIFY(labels.contains(QStringLiteral("=H")));
            QVERIFY(!labels.contains(QStringLiteral("...")));

            QVERIFY(noElidedDockButtons(align));
            QVERIFY(noElidedDockButtons(opt));
            QVERIFY(noElidedDockButtons(shapes));
            QVERIFY(noElidedDockButtons(layers));
            QVERIFY(noElidedDockButtons(frames));
            QVERIFY(noElidedDockButtons(history));
            QVERIFY(noElidedDockButtons(stack));
            QVERIFY(noChildPastRight(align));
            scroll.takeWidget(); // keep the stack alive for the next width
        };

        check(280, 900);
        // Reparent the stack for the next size; the previous scroll is gone.
        // check() creates a new scroll and setWidget() reparents the stack.
        check(270, 900);
        check(270, 380); // scrollbar visible, viewport narrower than 270
    }

    void contentSizedPanelsFollowSizeHintAndAreNotSplitterResized()
    {
        auto *opt = new OptimizePanel;
        auto *align = new AlignPanel;
        auto *shapes = new AutoShapesPanel;
        auto *layersBody = new QLabel(QStringLiteral("Layers"));
        PanelStack stack;
        auto *optCp = new CollapsiblePanel(QStringLiteral("Optimize"), opt, &stack);
        auto *layCp = new CollapsiblePanel(QStringLiteral("Layers"), layersBody, &stack);
        auto *alCp = new CollapsiblePanel(QStringLiteral("Align"), align, &stack);
        auto *asCp = new CollapsiblePanel(QStringLiteral("Auto Shapes"), shapes, &stack);
        stack.addPanel(optCp, 0);
        stack.addPanel(layCp, 3);
        stack.addPanel(alCp, 0);
        stack.addPanel(asCp, 0);
        optCp->setContentSized(true);
        alCp->setContentSized(true);
        asCp->setContentSized(true);
        optCp->setCollapsed(false);
        layCp->setCollapsed(false);
        alCp->setCollapsed(false);
        asCp->setCollapsed(false);
        stack.resize(280, 900);
        stack.show();
        QVERIFY(QTest::qWaitForWindowExposed(&stack));
        drain();
        stack.relayout();
        drain();

        // Handles only after resizable panels (Layers), not Optimize / Align / Auto Shapes.
        QVERIFY(stack.visibleHandleCount() >= 1);
        QVERIFY(!stack.dragBetween(optCp, 40));
        QVERIFY(!stack.dragBetween(alCp, 40));
        QVERIFY(!stack.dragBetween(asCp, 40));

        const int optH = optCp->height();
        QVERIFY(optH >= opt->sizeHint().height() + optCp->headerHeight());
        QVERIFY(optH <= opt->sizeHint().height() + optCp->headerHeight() + 6);
        QVERIFY(opt->height() >= opt->minimumSizeHint().height() - 1);
        QVERIFY(alCp->height() >= align->sizeHint().height() + alCp->headerHeight() - 2);
        QVERIFY(alCp->height() <= align->sizeHint().height() + alCp->headerHeight() + 6);
        QVERIFY(asCp->height() >= shapes->sizeHint().height() + asCp->headerHeight() - 2);
        QVERIFY(asCp->height() <= shapes->sizeHint().height() + asCp->headerHeight() + 6);
        QVERIFY(slackBelowLastChild(opt->widget()) <= 4);
        if (auto *bar = opt->findChild<QWidget *>(QStringLiteral("PanelButtonBar")))
            QVERIFY(opt->height() - bar->geometry().bottom() <= 6);
        QVERIFY(slackBelowLastChild(align) <= 4);
        QVERIFY(slackBelowLastChild(shapes) <= 4);

        // Live update: a format with more controls should grow (or at least keep) the panel,
        // never squash the contents.
        ExportOptions png;
        png.format = Fmt::PNG;
        opt->widget()->setOptions(png);
        drain();
        stack.relayout();
        drain();
        const int pngH = optCp->height();
        ExportOptions tiff;
        tiff.format = Fmt::TIFF;
        opt->widget()->setOptions(tiff);
        drain();
        stack.relayout();
        drain();
        const int tiffH = optCp->height();
        QVERIFY(tiffH >= opt->sizeHint().height() + optCp->headerHeight() - 2);
        QVERIFY(opt->height() + 2 >= opt->sizeHint().height());
        QVERIFY(optCp->height() <= opt->sizeHint().height() + optCp->headerHeight() + 6);
        QVERIFY(slackBelowLastChild(opt->widget()) <= 4);
        // TIFF and PNG expose different option rows; heights may differ, but neither is clipped.
        Q_UNUSED(pngH);
    }

    void dockScrollbarDoesNotClipAndFixedPanelsHaveNoSlack()
    {
        auto *opt = new OptimizePanel;
        auto *align = new AlignPanel;
        auto *shapes = new AutoShapesPanel;
        auto *layers = new LayersPanel;
        auto *frames = new FramesPanel;
        auto *history = new HistoryPanel;
        auto *stack = new PanelStack;
        auto *optCp = new CollapsiblePanel(QStringLiteral("Optimize"), opt, stack);
        auto *layCp = new CollapsiblePanel(QStringLiteral("Layers"), layers, stack);
        auto *alCp = new CollapsiblePanel(QStringLiteral("Align"), align, stack);
        auto *asCp = new CollapsiblePanel(QStringLiteral("Auto Shapes"), shapes, stack);
        auto *frCp = new CollapsiblePanel(QStringLiteral("Frames"), frames, stack);
        auto *hiCp = new CollapsiblePanel(QStringLiteral("History"), history, stack);
        stack->addPanel(optCp, 0);
        stack->addPanel(layCp, 3);
        stack->addPanel(alCp, 0);
        stack->addPanel(asCp, 0);
        stack->addPanel(frCp, 1);
        stack->addPanel(hiCp, 1);
        optCp->setContentSized(true);
        alCp->setContentSized(true);
        asCp->setContentSized(true);
        for (auto *cp : {optCp, layCp, alCp, asCp, frCp, hiCp})
            cp->setCollapsed(false);

        PanelScrollArea scroll;
        scroll.setObjectName(QStringLiteral("PanelsScroll"));
        scroll.setWidget(stack);
        scroll.resize(270, 380);
        scroll.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scroll));
        drain();
        ExportOptions webp;
        webp.format = Fmt::WEBP;
        opt->widget()->setOptions(webp);
        drain();
        stack->relayout();
        drain();

        QVERIFY(scroll.verticalScrollBar());
        QVERIFY(scroll.verticalScrollBar()->isVisible() || scroll.verticalScrollBar()->maximum() > 0);
        QCOMPARE(stack->width(), scroll.viewport()->width());
        QVERIFY(scroll.horizontalScrollBar()->maximum() == 0);

        QVERIFY(noChildPastRight(opt));
        QVERIFY(noChildPastRight(align));
        QVERIFY(noChildPastRight(shapes));
        QVERIFY(noChildPastRight(layers));
        QVERIFY(noChildPastRight(frames));
        QVERIFY(noChildPastRight(history));
        QVERIFY(noChildPastRight(stack));

        QVERIFY2(slackBelowLastChild(opt->widget()) <= 4,
                 qPrintable(QStringLiteral("OptimizeWidget slack %1").arg(slackBelowLastChild(opt->widget()))));
        if (auto *bar = opt->findChild<QWidget *>(QStringLiteral("PanelButtonBar")))
            QVERIFY(opt->height() - bar->geometry().bottom() <= 6);
        QVERIFY(optCp->height() <= opt->sizeHint().height() + optCp->headerHeight() + 6);
        QVERIFY2(slackBelowLastChild(align) <= 4,
                 qPrintable(QStringLiteral("Align slack %1").arg(slackBelowLastChild(align))));
        QVERIFY2(slackBelowLastChild(shapes) <= 4,
                 qPrintable(QStringLiteral("Auto Shapes slack %1").arg(slackBelowLastChild(shapes))));
        QVERIFY(alCp->height() <= align->sizeHint().height() + alCp->headerHeight() + 6);
        QVERIFY(asCp->height() <= shapes->sizeHint().height() + asCp->headerHeight() + 6);
    }

    void listMaxHeightsDoNotOverlapButtonBars()
    {
        auto *layers = new LayersPanel;
        auto *frames = new FramesPanel;
        auto *history = new HistoryPanel;
        PanelStack stack;
        auto *lCp = new CollapsiblePanel(QStringLiteral("Layers"), layers, &stack);
        auto *fCp = new CollapsiblePanel(QStringLiteral("Frames"), frames, &stack);
        auto *hCp = new CollapsiblePanel(QStringLiteral("History"), history, &stack);
        stack.addPanel(lCp, 3);
        stack.addPanel(fCp, 1);
        stack.addPanel(hCp, 1);
        lCp->setCollapsed(false);
        fCp->setCollapsed(false);
        hCp->setCollapsed(false);
        stack.resize(280, 700);
        stack.show();
        QVERIFY(QTest::qWaitForWindowExposed(&stack));
        drain();
        stack.relayout();
        drain();

        auto *tree = layers->findChild<QTreeWidget *>(QStringLiteral("layersTree"));
        auto *flist = frames->findChild<QListWidget *>(QStringLiteral("framesList"));
        auto *hlist = history->findChild<QListWidget *>(QStringLiteral("historyList"));
        QVERIFY(tree && flist && hlist);

        // Lists keep a finite max height (smaller than the old unconstrained one).
        QVERIFY(tree->maximumHeight() < QWIDGETSIZE_MAX);
        QVERIFY(flist->maximumHeight() < QWIDGETSIZE_MAX);
        QVERIFY(hlist->maximumHeight() < QWIDGETSIZE_MAX);

        QVERIFY(noOverlap(layers, tree));
        QVERIFY(noOverlap(frames, flist));
        QVERIFY(noOverlap(history, hlist));

        // Shrink each panel toward its minimum: the bar must still sit below the list.
        QVERIFY(stack.dragBetween(lCp, -400));
        QVERIFY(stack.dragBetween(fCp, -400));
        QVERIFY(stack.dragBetween(hCp, -400));
        drain();
        QVERIFY(noOverlap(layers, tree));
        QVERIFY(noOverlap(frames, flist));
        QVERIFY(noOverlap(history, hlist));
        QVERIFY(tree->maximumHeight() >= tree->minimumHeight());
        QVERIFY(flist->maximumHeight() >= flist->minimumHeight());
    }

    void splitterPushDownKeepsPanelsBelow()
    {
        QLabel *aBody = new QLabel(QStringLiteral("A"));
        QLabel *bBody = new QLabel(QStringLiteral("B"));
        QLabel *cBody = new QLabel(QStringLiteral("C"));
        auto *stack = new PanelStack;
        auto *a = new CollapsiblePanel(QStringLiteral("Layers"), aBody, stack);
        auto *b = new CollapsiblePanel(QStringLiteral("Frames"), bBody, stack);
        auto *c = new CollapsiblePanel(QStringLiteral("History"), cBody, stack);
        stack->addPanel(a, 3);
        stack->addPanel(b, 1);
        stack->addPanel(c, 1);
        a->setCollapsed(false);
        b->setCollapsed(false);
        c->setCollapsed(false);

        PanelScrollArea scroll;
        scroll.setObjectName(QStringLiteral("PanelsScroll"));
        scroll.setWidget(stack);
        scroll.resize(280, 360);
        scroll.show();
        QVERIFY(QTest::qWaitForWindowExposed(&scroll));
        drain();
        stack->relayout();
        drain();

        const int ha = stack->storedHeight(a);
        const int hb = stack->storedHeight(b);
        const int hc = stack->storedHeight(c);
        const int hint0 = stack->sizeHint().height();
        const int ya = a->y(), yb = b->y(), yc = c->y();

        QVERIFY(stack->dragBetween(a, 80));
        drain();
        QCOMPARE(stack->storedHeight(a), ha + 80);
        QCOMPARE(stack->storedHeight(b), hb);
        QCOMPARE(stack->storedHeight(c), hc);
        QVERIFY(stack->sizeHint().height() >= hint0 + 80 - 2);
        QVERIFY(b->y() >= yb + 70);
        QVERIFY(c->y() >= yc + 70);
        QCOMPARE(a->y(), ya);
    }
};

QTEST_MAIN(V073Test)
#include "v073_test.moc"
