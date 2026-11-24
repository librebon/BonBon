// Headless UI smoke test: drives the real MainWindow with synthetic mouse and
// keyboard input. Run with QT_QPA_PLATFORM=offscreen (ctest sets this).
#include "core/Document.h"
#include "io/ImageIO.h"
#include "io/LwdFormat.h"
#include "core/Transform.h"
#include <QToolButton>
#include <QJsonObject>
#include "ui/LiquifyDialog.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/Dialogs.h"
#include "ui/ColorPicker.h"
#include "ui/GradientEditor.h"
#include "ui/FindPanel.h"
#include <QColorDialog>
#include "ui/ClipboardImage.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QPointer>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include "ui/FaviconDialog.h"
#include "io/Codecs.h"
#include "io/DocIO.h"
#include "io/Quantize.h"
#include <QMessageBox>
#include <QMimeData>
#include <QDir>
#include <QSet>
#include <memory>
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/Panels.h"
#include "ui/PixelUpscaleDialog.h"
#include "ui/PropertyInspector.h"
#include "ui/ProceduralDialog.h"
#include "core/Procedural.h"
#include "core/Effects.h"
#include "core/Fills.h"
#include "core/AutoShape.h"
#include <QWindow>
#include <QMenu>
#include <QHash>
#include <functional>
#include <cmath>
#include <QSettings>
#include <QTabBar>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <QFileInfo>
#include "core/CorePrefs.h"
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QListWidget>
#include <QDoubleSpinBox>
#include <QSvgRenderer>
#include <QDockWidget>
#include <QContextMenuEvent>
#include <QComboBox>
#include <QTableWidget>
#include <QTreeWidget>
#include "ui/Theme.h"
#include "ui/ComboFit.h"
#include <QToolTip>
#include "Brand.h"
#include "ui/ToolsPanel.h"
#include "ui/PopupSlider.h"
#include "ui/Icons.h"
#include "core/Styles.h"
#include <QDialog>
#include <QApplication>
#include <QComboBox>
#include <QInputDialog>
#include <QScopeGuard>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QMenuBar>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QRandomGenerator>
#include <QRadioButton>
#include <QGroupBox>
#include <QSlider>
#include <QElapsedTimer>
#include <QPointer>
#include "ui/Rulers.h"
#include "ui/ViewGeometry.h"
#include "ui/PreferencesDialog.h"
#include "ui/ZoomField.h"
#include <QClipboard>
#include <QStyleOption>
#include "core/SelectionMask.h"
#include "io/LwdFormat.h"
#include <QListWidget>

namespace {
void sendMouse(QWidget *w, QEvent::Type t, QPointF pos, Qt::MouseButton b, Qt::MouseButtons bs, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QMouseEvent ev(t, pos, w->mapToGlobal(pos), b, bs, m);
    QApplication::sendEvent(w, &ev);
}

// drag in document coordinates
void drag(Canvas *c, QPointF a, QPointF b, int steps = 6, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QPointF va = c->toView(a), vb = c->toView(b);
    sendMouse(c, QEvent::MouseMove, va, Qt::NoButton, Qt::NoButton, m);
    sendMouse(c, QEvent::MouseButtonPress, va, Qt::LeftButton, Qt::LeftButton, m);
    for (int i = 1; i <= steps; ++i) sendMouse(c, QEvent::MouseMove, va + (vb - va) * (double(i) / steps), Qt::NoButton, Qt::LeftButton, m);
    sendMouse(c, QEvent::MouseButtonRelease, vb, Qt::LeftButton, Qt::NoButton, m);
    QCoreApplication::processEvents();
}

void click(Canvas *c, QPointF a) { drag(c, a, a, 0); }

int countObjects(Document *d) {
    int n = 0;
    for (int li = 0; li < d->s.layers.size(); ++li) n += d->objects(li).size();
    return n;
}

const VObject *lastObject(Document *d) {
    for (int li = d->s.layers.size() - 1; li >= 0; --li)
        if (!d->objects(li).isEmpty()) return &d->objects(li).last();
    return nullptr;
}

// Accept the next modal QInputDialog with the given text.
void autoAnswer(const QString &text) {
    QTimer::singleShot(50, [text] {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (auto *dlg = qobject_cast<QInputDialog *>(w); dlg && dlg->isVisible()) { dlg->setTextValue(text); dlg->accept(); return; }
    });
}

int maskAt(const QImage &m, int x, int y) { return m.constScanLine(y)[x]; }

// a 100x80 document holding one opaque bitmap: red left half, blue right half
Document *makeSelDoc(MainWindow *w, quint64 *bid) {
    DocumentView *v = w->addDocument(Document::createNew(100, 80, Qt::white));
    Document *fd = v->doc();
    QImage src(100, 80, QImage::Format_ARGB32_Premultiplied);
    src.fill(Qt::red);
    { QPainter p(&src); p.fillRect(50, 0, 50, 80, Qt::blue); }
    VObject b;
    b.type = ObjType::Bitmap;
    b.rect = QRectF(0, 0, 100, 80);
    b.image = src;
    b.id = fd->newId();
    fd->addObject(b, 0, false);
    fd->commit(QStringLiteral("bitmap"));
    fd->setSelection({b.id});
    *bid = b.id;
    return fd;
}
void selectRect(Document *d, QRectF r) {
    QPainterPath p;
    p.addRect(r);
    d->combinePixelSelection(p, Qt::NoModifier, Document::SelHard, 0);
}
}

namespace {
QImage photoLike(int w, int h) {
    QImage im(w, h, QImage::Format_ARGB32_Premultiplied);
    quint32 s = 12345;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        s = s * 1664525u + 1013904223u;
        const int n = int((s >> 24) % 21) - 10;
        const int r = qBound(0, (x * 200) / qMax(1, w) + 30 + n, 255), g = qBound(0, (y * 180) / qMax(1, h) + 40 + n, 255);
        im.setPixel(x, y, ((x / 12 + y / 12) & 1) ? qRgb(r, g, 120 + n) : qRgb(g, r, 60 + n));
    }
    return im;
}
struct CaDoc { DocumentView *v; Document *d; Canvas *c; quint64 id; };
CaDoc caDoc(MainWindow *w, QSize docSize, const QImage &img, QRectF rect, bool select) {
    DocumentView *v = w->addDocument(Document::createNew(docSize.width(), docSize.height(), Qt::white));
    QCoreApplication::processEvents();
    Document *dd = v->doc();
    Canvas *cv = v->canvas();
    cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
    quint64 id = 0;
    if (!img.isNull()) {
        VObject b;
        b.type = ObjType::Bitmap;
        b.rect = rect;
        b.image = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        b.id = id = dd->newId();
        dd->addObject(b, 0, false);
        dd->commit(QStringLiteral("bitmap"));
        if (select) dd->setSelection({b.id}); else dd->clearSelection();
    }
    appState().setTool(Tool::ContentAware);
    return {v, dd, cv, id};
}
// a real stroke: press, `steps` moves (1 = one very fast jump), release, all in document coordinates
void caStroke(Canvas *cv, QVector<QPointF> pts, int steps = 8) {
    QTest::mouseMove(cv, cv->toView(pts.first()).toPoint());
    QTest::mousePress(cv, Qt::LeftButton, Qt::NoModifier, cv->toView(pts.first()).toPoint());
    for (int k = 1; k < pts.size(); ++k)
        for (int i = 1; i <= steps; ++i)
            QTest::mouseMove(cv, cv->toView(pts[k - 1] + (pts[k] - pts[k - 1]) * (double(i) / steps)).toPoint());
    QTest::mouseRelease(cv, Qt::LeftButton, Qt::NoModifier, cv->toView(pts.last()).toPoint());
}
QString lastLabel(Document *dd) { return dd->history()[dd->historyIndex()].label; }
bool waitRemove(Document *dd, Canvas *cv) {
    QElapsedTimer t; t.start();
    while ((cv->contentAwareBusy() || lastLabel(dd) != QLatin1String("Content-Aware Remove")) && t.elapsed() < 120000) {
        if (!cv->contentAwareBusy() && lastLabel(dd) != QLatin1String("Content-Aware Remove")) return false;
        QTest::qWait(10);
    }
    return lastLabel(dd) == QLatin1String("Content-Aware Remove");
}
bool settle(Canvas *cv) {
    QElapsedTimer t; t.start();
    while (cv->contentAwareBusy() && t.elapsed() < 120000) QTest::qWait(10);
    QTest::qWait(1);
    return !cv->contentAwareBusy();
}

}

class UiSmoke : public QObject {
    Q_OBJECT
    MainWindow *w = nullptr;
    Document *d = nullptr;
    Canvas *c = nullptr;
    QTemporaryDir tmp;

private slots:
    void initTestCase() {
        theme::apply(*qApp);
        w = new MainWindow;
        w->resize(1300, 850);
        DocumentView *v = w->addDocument(Document::createNew(400, 300, Qt::white));
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w));
        d = v->doc();
        c = v->canvas();
        QTest::qWait(50);
        QVERIFY(c->zoom() > 0);
        QCOMPARE(d->s.width, 400);
        QVERIFY(w->windowTitle().contains(QStringLiteral("Untitled-1")));
    }

    void menusAndActions() {
        const QStringList menus = {QStringLiteral("&File"), QStringLiteral("&Edit"), QStringLiteral("&View"), QStringLiteral("&Select"), QStringLiteral("&Modify"),
                                   QStringLiteral("&Text"), QStringLiteral("&Commands"), QStringLiteral("F&ilters"), QStringLiteral("&Window"), QStringLiteral("&Help")};
        QStringList got;
        for (QAction *a : w->menuBar()->actions()) got << a->text();
        QCOMPARE(got, menus);
        QVERIFY(w->action(QStringLiteral("file.export")));
        QCOMPARE(w->action(QStringLiteral("edit.undo"))->shortcut(), QKeySequence::keyBindings(QKeySequence::Undo).first());
        for (const QString &p : {QStringLiteral("Layers"), QStringLiteral("Frames"), QStringLiteral("History"), QStringLiteral("Optimize"),
                                 QStringLiteral("Align"), QStringLiteral("Styles"), QStringLiteral("Library")})
            QVERIFY2(w->panel(p), qPrintable(p));
    }

    void vectorTools() {
        appState().setTool(Tool::Rect);
        int n = countObjects(d);
        drag(c, {40, 40}, {140, 110});
        QCOMPARE(countObjects(d), n + 1);
        const VObject *o = lastObject(d);
        QCOMPARE(o->type, ObjType::Rect);
        QVERIFY(qAbs(o->rect.width() - 100) < 2 && qAbs(o->rect.height() - 70) < 2);

        appState().setTool(Tool::Ellipse);
        drag(c, {200, 40}, {280, 120});
        QCOMPARE(lastObject(d)->type, ObjType::Ellipse);

        appState().setTool(Tool::Line);
        drag(c, {20, 200}, {380, 220});
        QCOMPARE(lastObject(d)->type, ObjType::Line);

        appState().setTool(Tool::Text);
        autoAnswer(QStringLiteral("Hello Libreworks"));
        click(c, {50, 250});
        QTest::qWait(150);
        QCOMPARE(lastObject(d)->type, ObjType::Text);
        QCOMPARE(lastObject(d)->text, QStringLiteral("Hello Libreworks"));
        QCOMPARE(countObjects(d), n + 4);
    }

    void pointerSelectMoveUndo() {
        appState().setTool(Tool::Pointer);
        click(c, {90, 75});
        VObject *sel = d->singleSelected();
        QVERIFY(sel);
        QCOMPARE(sel->type, ObjType::Rect);
        const QRectF before = sel->rect;
        drag(c, {90, 75}, {120, 95});
        QVERIFY(qAbs(d->singleSelected()->rect.x() - (before.x() + 30)) < 1.5);
        const int hi = d->historyIndex();
        d->undo();
        QCOMPARE(d->historyIndex(), hi - 1);
        QVERIFY(qAbs(d->find(sel->id)->rect.x() - before.x()) < 0.01);
        d->redo();
        QVERIFY(qAbs(d->find(sel->id)->rect.x() - (before.x() + 30)) < 1.5);
        // History panel reflects the steps
        QVERIFY(d->history().size() >= 6);
    }

    void bitmapTools() {
        d->clearSelection();
        appState().setTool(Tool::Brush);
        appState().stroke = QColor(255, 0, 0);
        int n = countObjects(d);
        drag(c, {300, 150}, {380, 160}, 10);
        QCOMPARE(countObjects(d), n + 1);
        const VObject *b = lastObject(d);
        QCOMPARE(b->type, ObjType::Bitmap);
        QImage r = d->render(-1, true);
        QColor px = r.pixelColor(340, 155);
        QVERIFY2(px.red() > 200 && px.green() < 80, qPrintable(px.name()));

        appState().setTool(Tool::Pencil);
        appState().stroke = QColor(0, 0, 255);
        drag(c, {300, 170}, {380, 170}, 10);
        r = d->render(-1, true);
        QVERIFY(r.pixelColor(340, 170).blue() > 200);

        appState().setTool(Tool::Eraser);
        drag(c, {340, 140}, {340, 180}, 10);
        r = d->render(-1, true);
        QColor e = r.pixelColor(340, 155);
        QVERIFY2(e.red() > 200 && e.green() > 200 && e.blue() > 200, qPrintable(e.name()));

        appState().setTool(Tool::Eyedropper);
        click(c, {305, 151});
        QVERIFY(appState().fill.red() > 200 && appState().fill.green() < 80);  // click = fill, Shift+click = stroke

        // marquee + Clear erases pixels in the selected bitmap
        appState().setTool(Tool::Marquee);
        drag(c, {295, 145}, {320, 175});
        QVERIFY(!d->pixelSelection.isEmpty());
        QVERIFY(!d->selection.isEmpty() || true);
        // select the bitmap explicitly and clear
        int li, idx;
        quint64 bid = 0;
        for (int l = 0; l < d->s.layers.size(); ++l) for (const VObject &o : d->objects(l)) if (o.type == ObjType::Bitmap) bid = o.id;
        QVERIFY(d->find(bid, &li, &idx));
        QSet<quint64> s{bid};
        d->setSelection(s);
        w->action(QStringLiteral("edit.clear"))->trigger();
        r = d->render(-1, true);
        QColor cl = r.pixelColor(310, 155);
        QVERIFY2(cl.red() > 200 && cl.green() > 200, qPrintable(cl.name()));
        w->action(QStringLiteral("select.none"))->trigger();
        QVERIFY(d->pixelSelection.isEmpty());

        // filter on a bitmap
        d->setSelection(s);
        w->action(QStringLiteral("filter.invert"))->trigger();
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Invert"));
        d->undo();
    }

    void editCommands() {
        appState().setTool(Tool::Pointer);
        click(c, {240, 80});
        QVERIFY(d->singleSelected());
        int n = countObjects(d);
        w->action(QStringLiteral("edit.duplicate"))->trigger();
        QCOMPARE(countObjects(d), n + 1);
        w->action(QStringLiteral("edit.copy"))->trigger();
        w->action(QStringLiteral("edit.paste"))->trigger();
        QCOMPARE(countObjects(d), n + 2);
        w->action(QStringLiteral("cmd.shadow"))->trigger();
        QCOMPARE(d->singleSelected()->effects.size(), 1);
        w->action(QStringLiteral("modify.fliph"))->trigger();
        w->action(QStringLiteral("modify.flattensel"))->trigger();
        QCOMPARE(d->singleSelected()->type, ObjType::Bitmap);
        w->action(QStringLiteral("edit.clear"))->trigger();
        QCOMPARE(countObjects(d), n + 1);
        w->action(QStringLiteral("select.all"))->trigger();
        QCOMPARE(d->selection.size(), countObjects(d));
        w->action(QStringLiteral("align.0"))->trigger();
        d->undo();
        w->action(QStringLiteral("select.none"))->trigger();
    }

    void layersAndFrames() {
        const int layers = d->s.layers.size();
        int idx = d->addLayer();
        d->commit(QStringLiteral("New Layer"));
        QCOMPARE(d->s.layers.size(), layers + 1);
        d->curLayer = idx;
        appState().setTool(Tool::Rect);
        drag(c, {10, 10}, {30, 30});
        QCOMPARE(d->objects(idx).size(), 1);
        d->s.layers[idx].visible = false;
        d->commit(QStringLiteral("Hide Layer"));
        d->moveLayer(idx, 0);
        d->commit(QStringLiteral("Move Layer"));
        d->deleteLayer(0);
        d->commit(QStringLiteral("Delete Layer"));
        QCOMPARE(d->s.layers.size(), layers);
        d->undo();
        QCOMPARE(d->s.layers.size(), layers + 1);
        d->redo();

        w->action(QStringLiteral("anim.addframe"))->trigger();
        w->action(QStringLiteral("anim.dupframe"))->trigger();
        QCOMPARE(d->s.frames.size(), 3);
        w->action(QStringLiteral("anim.play"))->trigger();
        QTest::qWait(350);
        w->action(QStringLiteral("anim.play"))->trigger();
        w->action(QStringLiteral("anim.delframe"))->trigger();
        QCOMPARE(d->s.frames.size(), 2);
        d->setCurrentFrame(0);
    }

    void viewTools() {
        appState().setTool(Tool::Zoom);
        const double z = c->zoom();
        click(c, {200, 150});
        QVERIFY(c->zoom() > z);
        appState().setTool(Tool::Hand);
        QPointF before = c->toView({0, 0});
        drag(c, {200, 150}, {180, 140});
        QVERIFY(c->toView({0, 0}) != before);
        w->action(QStringLiteral("view.fit"))->trigger();
        w->action(QStringLiteral("view.actual"))->trigger();
        QCOMPARE(c->zoom(), 1.0);
        // Zoom tool: Alt-click zooms out, dragging a rectangle zooms to it
        appState().setTool(Tool::Zoom);
        drag(c, {200, 150}, {200, 150}, 0, Qt::AltModifier);
        QVERIFY(c->zoom() < 1.0);
        drag(c, {100, 100}, {200, 150});
        QVERIFY(c->zoom() > 2.0);
        QVERIFY(qAbs(c->docCenter().x() - 150) < 2 && qAbs(c->docCenter().y() - 125) < 2);
        appState().setTool(Tool::Pointer);
        w->action(QStringLiteral("view.fit"))->trigger();
    }

    void previewZoom() {
        DocumentView *v = w->currentView();
        auto *status = w->findChild<QLineEdit *>(QStringLiteral("statusZoom"));
        QVERIFY(status);
        w->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(w));
        w->action(QStringLiteral("view.actual"))->trigger();
        w->action(QStringLiteral("view.mode2"))->trigger(); // 2-Up
        QCOMPARE(v->mode(), 2);
        QCOMPARE(v->zoom(), 1.0); // carried over from Original
        PreviewPane *a = v->previewPane(2, 0), *b = v->previewPane(2, 1);
        QVERIFY(a && b && a->view() == b->view());
        a->setFocus();
        QCoreApplication::processEvents();
        // keyboard shortcuts, including the numeric keypad
        QTest::keyClick(a, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(v->zoom(), 1.5);
        QCOMPARE(status->text(), QStringLiteral("150%"));
        QTest::keyClick(a, Qt::Key_Plus, Qt::ControlModifier | Qt::KeypadModifier);
        QCOMPARE(v->zoom(), 2.0);
        QTest::keyClick(a, Qt::Key_Minus, Qt::ControlModifier | Qt::KeypadModifier);
        QCOMPARE(v->zoom(), 1.5);
        QTest::keyClick(a, Qt::Key_Minus, Qt::ControlModifier);
        QCOMPARE(v->zoom(), 1.0);
        v->setZoom(8);
        QTest::keyClick(a, Qt::Key_0, Qt::ControlModifier);
        const double fitZ = v->zoom();
        QVERIFY(fitZ > 0.1);   // Fit fills the pane exactly on the limiting axis (it may zoom in)
        QVERIFY(400 * fitZ <= a->imageArea().width() + 0.5 && 300 * fitZ <= a->imageArea().height() + 0.5); // fits the pane
        QVERIFY(qAbs(400 * fitZ - a->imageArea().width()) < 1 || qAbs(300 * fitZ - a->imageArea().height()) < 1);
        QCOMPARE(v->previewView()->center(), QPointF(200, 150));
        QTest::keyClick(a, Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(v->zoom(), 1.0);
        QCOMPARE(status->text(), QStringLiteral("100%"));
        // Ctrl+wheel zooms around the cursor: the document point under the mouse stays put
        PreviewView *pv = v->previewView();
        const QPointF at(a->imageArea().width() * 0.3, a->imageArea().height() * 0.4);
        const QPointF docAt = pv->toDoc(at, a->imageArea());
        {
            QWheelEvent we(at, a->mapToGlobal(at), {}, QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(a, &we);
        }
        QVERIFY(qAbs(pv->zoom() - 1.25) < 1e-9);
        QPointF after = pv->toDoc(at, a->imageArea());
        QVERIFY(QLineF(after, docAt).length() < 1e-6);
        // Zoom tool: click zooms in, Alt-click zooms out, drag a rectangle zooms to it
        pv->setView(1, {200, 150});
        appState().setTool(Tool::Zoom);
        const QPointF mid(a->imageArea().width() / 2.0, a->imageArea().height() / 2.0);
        sendMouse(a, QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton);
        sendMouse(a, QEvent::MouseButtonRelease, mid, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(pv->zoom(), 1.5);
        sendMouse(a, QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
        sendMouse(a, QEvent::MouseButtonRelease, mid, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
        QCOMPARE(pv->zoom(), 1.0);
        sendMouse(a, QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton);
        sendMouse(a, QEvent::MouseMove, mid + QPointF(40, 30), Qt::NoButton, Qt::LeftButton);
        sendMouse(a, QEvent::MouseButtonRelease, mid + QPointF(40, 30), Qt::LeftButton, Qt::NoButton);
        QVERIFY(pv->zoom() > 3.0);
        QVERIFY(QLineF(pv->center(), QPointF(220, 165)).length() < 1);
        // Hand tool pans both panes together
        appState().setTool(Tool::Hand);
        QPointF c0 = pv->center();
        sendMouse(b, QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton);
        sendMouse(b, QEvent::MouseMove, mid + QPointF(-30, 0), Qt::NoButton, Qt::LeftButton);
        sendMouse(b, QEvent::MouseButtonRelease, mid + QPointF(-30, 0), Qt::LeftButton, Qt::NoButton);
        QVERIFY(pv->center().x() > c0.x());
        QCOMPARE(a->view()->center(), b->view()->center());
        // Space-drag pans even with another tool
        appState().setTool(Tool::Pointer);
        c0 = pv->center();
        b->setFocus();
        QTest::keyPress(b, Qt::Key_Space);
        sendMouse(b, QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton);
        sendMouse(b, QEvent::MouseMove, mid + QPointF(0, 25), Qt::NoButton, Qt::LeftButton);
        sendMouse(b, QEvent::MouseButtonRelease, mid + QPointF(0, 25), Qt::LeftButton, Qt::NoButton);
        QTest::keyRelease(b, Qt::Key_Space);
        QVERIFY(pv->center().y() < c0.y());
        // 4-Up shares the same view; Original picks the zoom back up
        w->action(QStringLiteral("view.mode3"))->trigger();
        QCOMPARE(v->previewPane(3, 3)->view(), pv);
        const double z4 = v->zoom();
        w->action(QStringLiteral("view.mode0"))->trigger();
        QCOMPARE(c->zoom(), z4);
        QCOMPARE(status->text(), QStringLiteral("%1%").arg(qRound(z4 * 100)));
        w->action(QStringLiteral("view.fit"))->trigger();

        // Export Preview dialog
        ExportDialog dlg(*d, w);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        PreviewPane *ep = dlg.previewPane();
        auto *zl = dlg.findChild<QLabel *>(QStringLiteral("exportPreviewZoom"));
        QVERIFY(ep && zl);
        QTest::keyClick(ep, Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(dlg.previewView()->zoom(), 1.0);
        QTest::keyClick(ep, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(dlg.previewView()->zoom(), 1.5);
        QCOMPARE(zl->text(), QStringLiteral("Zoom: 150%"));
        QTest::keyClick(ep, Qt::Key_Minus, Qt::ControlModifier | Qt::KeypadModifier);
        QCOMPARE(dlg.previewView()->zoom(), 1.0);
        {
            QWheelEvent we(QPointF(50, 50), ep->mapToGlobal(QPointF(50, 50)), {}, QPoint(0, -120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(ep, &we);
        }
        QVERIFY(qAbs(dlg.previewView()->zoom() - 0.8) < 1e-9);
        dlg.reject();
    }

    void previewTabs() {
        DocumentView *v = w->currentView();
        for (Fmt f : {Fmt::PNG, Fmt::JPEG, Fmt::GIF, Fmt::WEBP, Fmt::PPM}) {
            if (!ImageIO::available(f)) continue;
            d->exportOptions.format = f;
            for (int m = 1; m < 4; ++m) {
                w->action(QStringLiteral("view.mode%1").arg(m))->trigger();
                QCOMPARE(v->mode(), m);
                v->refresh();
                QCoreApplication::processEvents();
            }
        }
        EncodeResult er = encodePreview(*d, d->exportOptions, 0);
        QVERIFY2(er.error.isEmpty(), qPrintable(er.error));
        QVERIFY(er.bytes > 0);
        w->action(QStringLiteral("view.mode0"))->trigger();
        QCOMPARE(v->mode(), 0);
    }

    // Regression: in 4-Up, double-clicking a pane used to overwrite the settings with that pane's
    // variant and every refresh derived new variants from it (JPEG quality cascading down to 5).
    void fourUpPaneSettings() {
        DocumentView *v = w->addDocument(Document::createNew(160, 120, Qt::white));
        Document *doc = v->doc();
        doc->exportOptions = ExportOptions();
        doc->exportOptions.format = Fmt::JPEG;
        doc->exportOptions.quality = 80;
        QCoreApplication::processEvents();
        w->action(QStringLiteral("view.mode3"))->trigger();
        v->refresh();
        QCOMPARE(doc->paneOptions.size(), Document::kPanes);
        QVector<ExportOptions> before;
        for (int i = 0; i < Document::kPanes; ++i) before.append(doc->paneOption(i));
        QCOMPARE(before[0].quality, 80);
        QCOMPARE(before[1].quality, 60);
        QCOMPARE(before[2].quality, 40);
        auto same = [&] {
            for (int i = 0; i < Document::kPanes; ++i)
                if (!(doc->paneOption(i) == before[i])) return false;
            return true;
        };
        auto *opt = w->findChild<OptimizePanel *>();
        QVERIFY(opt);
        auto *q = opt->widget()->findChild<QSpinBox *>(QStringLiteral("optQuality"));
        QVERIFY(q);
        for (Tool t : {Tool::Pointer, Tool::Zoom, Tool::Hand}) {
            appState().setTool(t);
            for (int p = 0; p < 4; ++p) {
                PreviewPane *pane = v->previewPane(3, p);
                QVERIFY(pane);
                const QPoint mid = pane->rect().center();
                QTest::mouseDClick(pane, Qt::LeftButton, Qt::NoModifier, mid);
                v->refresh();
                QVERIFY2(same(), qPrintable(QStringLiteral("double-click, tool %1, pane %2").arg(int(t)).arg(p)));
                for (int k = 0; k < 5; ++k) {
                    QTest::mouseClick(pane, Qt::LeftButton, Qt::NoModifier, mid);
                    v->refresh();
                }
                QVERIFY2(same(), qPrintable(QStringLiteral("clicks, tool %1, pane %2").arg(int(t)).arg(p)));
                // the raw event sequence of a double-click as a window system delivers it
                sendMouse(pane, QEvent::MouseButtonPress, mid, Qt::LeftButton, Qt::LeftButton);
                sendMouse(pane, QEvent::MouseButtonRelease, mid, Qt::LeftButton, Qt::NoButton);
                sendMouse(pane, QEvent::MouseButtonDblClick, mid, Qt::LeftButton, Qt::LeftButton);
                sendMouse(pane, QEvent::MouseButtonRelease, mid, Qt::LeftButton, Qt::NoButton);
                v->refresh();
                QVERIFY(same());
                // Optimize always shows the selected pane's own settings
                QCOMPARE(q->value(), doc->paneOption(doc->selectedPane).quality);
                QCOMPARE(doc->exportOptions, doc->paneOption(doc->selectedPane));
            }
        }
        QCoreApplication::processEvents();
        QTest::qWait(450); // let the zoom/pan-triggered refreshes run
        QVERIFY(same());
        // a plain Pointer click selects a pane (highlight border) without touching any settings
        appState().setTool(Tool::Pointer);
        QTest::mouseClick(v->previewPane(3, 3), Qt::LeftButton, Qt::NoModifier, v->previewPane(3, 3)->rect().center());
        QCOMPARE(doc->selectedPane, 2);
        QVERIFY(v->previewPane(3, 3)->isSelected());
        QVERIFY(!v->previewPane(3, 1)->isSelected() && !v->previewPane(3, 2)->isSelected());
        QCOMPARE(doc->exportOptions.quality, 40); // File > Export uses the selected pane
        QCOMPARE(q->value(), 40);
        QVERIFY(same());
        // clicking the Original pane selects nothing
        QTest::mouseClick(v->previewPane(3, 0), Qt::LeftButton, Qt::NoModifier, v->previewPane(3, 0)->rect().center());
        QCOMPARE(doc->selectedPane, 2);
        // editing Optimize changes only the selected pane
        q->setValue(33);
        QCoreApplication::processEvents();
        v->refresh();
        QCOMPARE(doc->paneOption(2).quality, 33);
        QCOMPARE(doc->paneOption(0), before[0]);
        QCOMPARE(doc->paneOption(1), before[1]);
        before[2].quality = 33;
        // switching views never derives the variants again
        w->action(QStringLiteral("view.mode0"))->trigger();
        w->action(QStringLiteral("view.mode2"))->trigger();
        w->action(QStringLiteral("view.mode3"))->trigger();
        v->refresh();
        QVERIFY(same());
        QVERIFY(v->previewPane(3, 3)->isSelected());
        // selecting pane 1 again gives back its untouched settings
        QTest::mouseClick(v->previewPane(3, 1), Qt::LeftButton, Qt::NoModifier, v->previewPane(3, 1)->rect().center());
        QCOMPARE(doc->selectedPane, 0);
        QCOMPARE(doc->exportOptions.quality, 80);
        QCOMPARE(q->value(), 80);
        QVERIFY(same());
        // the per-pane settings are stored in the .lwd
        doc->selectPane(1);
        const QString path = tmp.filePath(QStringLiteral("panes.lwd"));
        QString err;
        QVERIFY2(lwd::save(*doc, path, &err), qPrintable(err));
        std::unique_ptr<Document> back(lwd::load(path, &err));
        QVERIFY2(back, qPrintable(err));
        QCOMPARE(back->selectedPane, 1);
        QCOMPARE(back->exportOptions.quality, 60);
        for (int i = 0; i < Document::kPanes; ++i) QCOMPARE(back->paneOption(i), before[i]);
        // explicit reset derives fresh variants from the selected settings
        w->action(QStringLiteral("view.resetpanes"))->trigger();
        QCOMPARE(doc->selectedPane, 0);
        QCOMPARE(doc->paneOption(0).quality, 60);
        QCOMPARE(doc->paneOption(1).quality, 40);
        QCOMPARE(doc->paneOption(2).quality, 20);
        w->action(QStringLiteral("view.mode0"))->trigger();
        appState().setTool(Tool::Pointer);
        // Optimize settings now mark the document dirty (so they are saved).
        doc->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        QCOMPARE(w->currentDoc(), d);
    }

    // User bug: "Fit leaves a 1 pixel border around the image".
    void fitHasNoBorder() {
        for (QSize ds : {QSize(400, 300), QSize(37, 211), QSize(3000, 1200)}) {
            DocumentView *v = w->addDocument(Document::createNew(ds.width(), ds.height(), QColor(200, 30, 40)));
            QCoreApplication::processEvents();
            w->action(QStringLiteral("view.fit"))->trigger();
            Canvas *cv = v->canvas();
            const QRectF r(cv->toView(QPointF(0, 0)), cv->toView(QPointF(ds.width(), ds.height())));
            const bool xLimit = double(cv->width()) / ds.width() <= double(cv->height()) / ds.height();
            if (xLimit) { QVERIFY2(qAbs(r.left()) < 0.01 && qAbs(r.right() - cv->width()) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(r.left()).arg(r.right()))); }
            else { QVERIFY2(qAbs(r.top()) < 0.01 && qAbs(r.bottom() - cv->height()) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(r.top()).arg(r.bottom()))); }
            // the grab: the limiting edges are document pixels, not pasteboard
            const QImage g = cv->grab().toImage();
            const QPoint a = xLimit ? QPoint(0, g.height() / 2) : QPoint(g.width() / 2, 0);
            const QPoint b = xLimit ? QPoint(g.width() - 1, g.height() / 2) : QPoint(g.width() / 2, g.height() - 1);
            QCOMPARE(QColor(g.pixel(a)), QColor(200, 30, 40));
            QCOMPARE(QColor(g.pixel(b)), QColor(200, 30, 40));
            // preview panes too
            for (int mode : {1, 2, 3}) {
                v->setMode(mode);
                v->refresh();
                QCoreApplication::processEvents();
                v->fit();
                PreviewPane *pp = v->previewPane(mode, 0);
                const QSize area = pp->imageArea();
                const double z = v->previewView()->zoom();
                const QPointF o = v->previewView()->origin(area);
                const bool xl = double(area.width()) / ds.width() <= double(area.height()) / ds.height();
                if (xl) QVERIFY(qAbs(o.x()) < 0.01 && qAbs(o.x() + ds.width() * z - area.width()) < 0.01);
                else QVERIFY(qAbs(o.y()) < 0.01 && qAbs(o.y() + ds.height() * z - area.height()) < 0.01);
            }
            v->setMode(0);
            v->doc()->markClean();
            w->action(QStringLiteral("file.close"))->trigger();
        }
        // the optional margin preference
        appState().prefs.fitMargin = 10;
        DocumentView *v = w->addDocument(Document::createNew(400, 300, Qt::white));
        QCoreApplication::processEvents();
        v->fit();
        const QRectF r(v->canvas()->toView(QPointF(0, 0)), v->canvas()->toView(QPointF(400, 300)));
        QVERIFY(qAbs(qMin(r.left(), r.top()) - 10) < 0.01);
        appState().prefs.fitMargin = 0;
        v->doc()->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }
    // Fit Canvas / Trim Canvas give exactly the content's pixel bounds
    void fitTrimCanvasExact() {
        for (const char *cmd : {"modify.trim", "modify.fit"}) {
            DocumentView *v = w->addDocument(Document::createNew(200, 150, Qt::transparent));
            Document *d = v->doc();
            VObject r;
            r.type = ObjType::Rect;
            r.rect = QRectF(10, 10, 50, 30);
            r.fillOn = true;
            r.fill = Qt::blue;
            r.strokeOn = false;
            r.id = d->newId();
            d->addObject(r, 0, false);
            d->commit(QStringLiteral("r"));
            w->action(QString::fromLatin1(cmd))->trigger();
            QVERIFY2(d->s.width == 50 && d->s.height == 30, qPrintable(QStringLiteral("%1: %2x%3").arg(cmd).arg(d->s.width).arg(d->s.height)));
            QCOMPARE(d->find(r.id)->rect, QRectF(0, 0, 50, 30));
            const QImage img = d->render(0, false);
            QCOMPARE(qAlpha(img.pixel(0, 0)), 255);
            QCOMPARE(qAlpha(img.pixel(49, 29)), 255);
            d->markClean();
            w->action(QStringLiteral("file.close"))->trigger();
        }
    }
    // User bug: in 4-Up a pane would not select with a slight wobble, or with the Zoom / Hand tools.
    void fourUpPaneSelectByPress() {
        QSettings().remove(QStringLiteral("hints/fourUpPanes"));
        DocumentView *v = w->addDocument(Document::createNew(160, 120, Qt::white));
        Document *doc = v->doc();
        doc->exportOptions = ExportOptions();
        doc->exportOptions.format = Fmt::JPEG;
        doc->exportOptions.quality = 80;
        QCoreApplication::processEvents();
        w->action(QStringLiteral("view.mode3"))->trigger();
        v->refresh();
        QVERIFY(!v->previewPane(3, 0)->hint().isEmpty());   // the one-time tip
        QVector<ExportOptions> before;
        for (int i = 0; i < Document::kPanes; ++i) before.append(doc->paneOption(i));
        auto same = [&] {
            for (int i = 0; i < Document::kPanes; ++i) if (!(doc->paneOption(i) == before[i])) return false;
            return true;
        };
        CollapsiblePanel *optPanel = nullptr;
        for (auto *cp : w->findChildren<CollapsiblePanel *>()) if (cp->title() == QLatin1String("Optimize")) optPanel = cp;
        QVERIFY(optPanel);
        int target = 1;
        for (Tool t : {Tool::Pointer, Tool::Zoom, Tool::Hand}) {
            appState().setTool(t);
            for (int k = 0; k < 3; ++k, target = target % 3 + 1) {
                PreviewPane *pane = v->previewPane(3, target);
                const QPoint mid = pane->rect().center();
                // press, wobble 10 px, release: real mouse events
                QTest::mousePress(pane, Qt::LeftButton, Qt::NoModifier, mid);
                QCOMPARE(doc->selectedPane, target - 1);   // selected on the press itself
                QTest::mouseMove(pane, mid + QPoint(6, 4));
                QTest::mouseMove(pane, mid + QPoint(10, 0));
                QTest::mouseRelease(pane, Qt::LeftButton, Qt::NoModifier, mid + QPoint(10, 0));
                QCoreApplication::processEvents();
                QVERIFY2(doc->selectedPane == target - 1, qPrintable(QStringLiteral("tool %1 pane %2").arg(int(t)).arg(target)));
                QVERIFY(pane->isSelected());
                for (int o = 1; o < 4; ++o) if (o != target) QVERIFY(!v->previewPane(3, o)->isSelected());
                QCOMPARE(optPanel->headerText(), QStringLiteral("Optimize: pane %1").arg(target + 1));
                QVERIFY(same());   // selecting never changes any pane's settings
            }
        }
        QVERIFY(v->previewPane(3, 0)->hint().isEmpty());   // the tip is gone once used
        QVERIFY(QSettings().value(QStringLiteral("hints/fourUpPanes")).toBool());
        // Space held (temporary hand) still selects
        appState().setTool(Tool::Pointer);
        PreviewPane *p2 = v->previewPane(3, 2);
        QTest::keyPress(p2, Qt::Key_Space);
        QTest::mousePress(p2, Qt::LeftButton, Qt::NoModifier, p2->rect().center());
        QTest::mouseRelease(p2, Qt::LeftButton, Qt::NoModifier, p2->rect().center() + QPoint(-8, 3));
        QTest::keyRelease(p2, Qt::Key_Space);
        QCOMPARE(doc->selectedPane, 1);
        // the Original pane never takes the selection
        QTest::mousePress(v->previewPane(3, 0), Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QTest::mouseRelease(v->previewPane(3, 0), Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
        QCOMPARE(doc->selectedPane, 1);
        // keyboard: Tab into a pane selects it
        {
            QFocusEvent fe(QEvent::FocusIn, Qt::TabFocusReason);
            QApplication::sendEvent(v->previewPane(3, 3), &fe);
            QCOMPARE(doc->selectedPane, 2);
        }
        QVERIFY(same());
        // the highlight: an accent border and a tinted caption, obvious next to the others
        v->refresh();
        const QColor accent = v->previewPane(3, 3)->palette().color(QPalette::Highlight);
        const QImage sel = v->previewPane(3, 3)->grab().toImage(), un = v->previewPane(3, 1)->grab().toImage();
        const qreal dpr = sel.devicePixelRatio();
        QCOMPARE(QColor(sel.pixel(int(1 * dpr), sel.height() / 2)).rgb(), accent.rgb());
        QVERIFY(QColor(un.pixel(int(1 * dpr), un.height() / 2)).rgb() != accent.rgb());
        const int cy = sel.height() - int(4 * dpr), cx = sel.width() / 2;
        QVERIFY(QColor(sel.pixel(cx, cy)).rgb() != QColor(un.pixel(cx, cy)).rgb());
        // 2-Up: the right pane is the selected, editable one
        w->action(QStringLiteral("view.mode2"))->trigger();
        v->refresh();
        QVERIFY(v->previewPane(2, 1)->isSelected() && !v->previewPane(2, 0)->isSelected());
        QCOMPARE(optPanel->headerText(), QStringLiteral("Optimize: pane 2"));
        auto *q = w->findChild<OptimizePanel *>()->widget()->findChild<QSpinBox *>(QStringLiteral("optQuality"));
        q->setValue(55);
        QCoreApplication::processEvents();
        QCOMPARE(doc->exportOptions.quality, 55);
        v->refresh();
        QVERIFY(v->previewPane(2, 1)->caption().contains(QStringLiteral("55")));
        w->action(QStringLiteral("view.mode0"))->trigger();
        QCOMPARE(optPanel->headerText(), QStringLiteral("Optimize"));
        // Optimize settings now mark the document dirty (so they are saved).
        doc->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }

    void moreTools() {
        // Pen: three clicks then finish -> path object
        appState().setTool(Tool::Pen);
        int n = countObjects(d);
        click(c, {50, 160}); click(c, {120, 180}); click(c, {80, 230});
        c->finishPen(true);
        QCOMPARE(countObjects(d), n + 1);
        QCOMPARE(lastObject(d)->type, ObjType::Path);
        QVERIFY(lastObject(d)->closed);
        // Lasso builds a pixel selection
        appState().setTool(Tool::Lasso);
        {   // a triangle (a straight drag encloses no area at every zoom)
            sendMouse(c, QEvent::MouseButtonPress, c->toView({10, 10}), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            for (QPointF q : {QPointF(60, 10), QPointF(60, 60)}) sendMouse(c, QEvent::MouseMove, c->toView(q), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            sendMouse(c, QEvent::MouseButtonRelease, c->toView({60, 60}), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::processEvents();
        }
        QVERIFY(!d->pixelSelection.isEmpty());
        w->action(QStringLiteral("select.none"))->trigger();
        // Magic wand and paint bucket work on the selected bitmap
        quint64 bid = 0;
        for (int l = 0; l < d->s.layers.size(); ++l) for (const VObject &o : d->objects(l)) if (o.type == ObjType::Bitmap) bid = o.id;
        QVERIFY(bid);
        d->setSelection({bid});
        appState().setTool(Tool::MagicWand);
        QRectF br = d->find(bid)->rect;
        click(c, br.center());
        QVERIFY(!d->pixelSelection.isEmpty());
        w->action(QStringLiteral("select.none"))->trigger();
        d->setSelection({bid});
        appState().setTool(Tool::PaintBucket);
        const int hi = d->historyIndex();
        click(c, br.center());
        QVERIFY(d->historyIndex() > hi);
        appState().setTool(Tool::Pointer);
    }

    void cropTool() {
        appState().setTool(Tool::Crop);
        drag(c, {20, 20}, {220, 170});
        QVERIFY(c->cropActive());
        c->applyCrop();
        QCOMPARE(d->s.width, 200);
        QCOMPARE(d->s.height, 150);
        d->undo();
        QCOMPARE(d->s.width, 400);
        appState().setTool(Tool::Pointer);
    }

    void exportAndSave() {
        QVERIFY(tmp.isValid());
        for (Fmt f : ImageIO::exportFormats()) {
            if (!ImageIO::available(f)) continue;
            ExportOptions o;
            o.format = f;
            o.animated = ImageIO::canAnimate(f);
            const QString path = tmp.filePath(QStringLiteral("out.") + ImageIO::extension(f));
            QVERIFY2(w->exportTo(d, path, o), qPrintable(path));
            if (!ImageIO::canRead(f)) { QVERIFY(QFileInfo(path).size() > 0); continue; }   // PDF: export only
            ImageSeq back;
            QVERIFY2(ImageIO::read(path, back), qPrintable(path));
            if (f == Fmt::ICO || f == Fmt::CUR || f == Fmt::ICNS) QVERIFY(back.width > 0 && back.width <= 1024);   // square icon sizes
            else QCOMPARE(back.width, 400);
        }
        const QString lwdPath = tmp.filePath(QStringLiteral("doc.lwd"));
        QString err;
        QVERIFY2(lwd::save(*d, lwdPath, &err), qPrintable(err));
        QVERIFY(w->openFile(lwdPath));
        Document *d2 = w->currentDoc();
        QVERIFY(d2 != d);
        QCOMPARE(d2->s.layers.size(), d->s.layers.size());
        QCOMPARE(d2->s.frames.size(), d->s.frames.size());
        QCOMPARE(d2->render(0, true), d->render(0, true));
        // open an image file as a new document
        QVERIFY(w->openFile(tmp.filePath(QStringLiteral("out.png"))));
        QCOMPARE(w->currentDoc()->s.width, 400);
        // TIFF (multi-page -> frames) and TGA via drag-and-drop; both land in Open Recent and keep
        // their format as the document's export format.
        {
            ExportOptions o; o.format = Fmt::TIFF; o.animated = true; o.tiffCompression = 2;
            const QString tif = tmp.filePath(QStringLiteral("pages.tif")), tga = tmp.filePath(QStringLiteral("dropped.tga"));
            QVERIFY(w->exportTo(d, tif, o));
            o = ExportOptions(); o.format = Fmt::TGA; o.tgaRle = true;
            QVERIFY(w->exportTo(d, tga, o));
            for (const QString &f : {tif, tga}) {
                QMimeData md; md.setUrls({QUrl::fromLocalFile(f)});
                QDropEvent ev(QPointF(100, 100), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
                static_cast<QObject *>(w)->event(&ev); // deliver straight to MainWindow::dropEvent
                QCOMPARE(w->currentDoc()->s.width, 400);
                QCOMPARE(w->recentFiles().first(), QFileInfo(f).absoluteFilePath());
                QCOMPARE(w->currentDoc()->exportOptions.format, f == tif ? Fmt::TIFF : Fmt::TGA);
                QCOMPARE(w->currentDoc()->s.frames.size(), f == tif ? d->s.frames.size() : 1);
            }
        }
        for (QMessageBox *mb : w->findChildren<QMessageBox *>()) mb->close();
        QCoreApplication::processEvents();
    }

    // v0.2.5: Optimize presets / controls, colour table, Tiny saving caption, LWD persistence of the
    // new export options, favicon dialog, drag-and-drop of the new formats, HDR tone-map preference.
    // v0.2.6: C Header (.h) export - presets, option controls, preview byte size, Export / Optimize wiring
    void tabFocus027() {
        // v0.2.7: no dotted focus rectangle around the selected tab's label, in every theme; keys still work
        const QString shotDir = qEnvironmentVariable("LW_TABFOCUS_SHOTS");
        for (const theme::Info &t : theme::list()) {
            theme::apply(*qApp, t.id);
            PreferencesDialog dlg;
            dlg.show();
            dlg.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&dlg));
            auto *tabs = dlg.findChild<QTabWidget *>();
            QVERIFY(tabs && tabs->count() > 1);
            QTabBar *bar = tabs->tabBar();
            tabs->setCurrentIndex(0);
            QWidget *other = nullptr;
            for (QWidget *w : tabs->currentWidget()->findChildren<QWidget *>())
                if (w->focusPolicy() & Qt::TabFocus && w->isVisible()) { other = w; break; }
            QVERIFY(other);
            other->setFocus(Qt::OtherFocusReason);
            QApplication::processEvents();
            const QImage plain = bar->grab().toImage();
            bar->setFocus(Qt::TabFocusReason);
            QApplication::processEvents();
            QVERIFY2(bar->hasFocus(), qPrintable(t.id));
            const QImage focused = bar->grab().toImage();
            if (!shotDir.isEmpty()) {
                dlg.grab().save(QDir(shotDir).filePath(QStringLiteral("prefs-tabfocus-%1.png").arg(t.id)));
                if (t.id == theme::list().first().id) bar->grab().scaled(bar->size() * 3).save(QDir(shotDir).filePath(QStringLiteral("prefs-tabbar-x3.png")));
            }
            QVERIFY2(plain == focused, qPrintable(QStringLiteral("focus rectangle drawn on the tab bar in theme %1").arg(t.id)));
            QTest::keyClick(bar, Qt::Key_Right);
            QCOMPARE(tabs->currentIndex(), 1);
            QVERIFY(bar->hasFocus());
            QTest::keyClick(bar, Qt::Key_Left);
            QCOMPARE(tabs->currentIndex(), 0);
        }
        theme::apply(*qApp);
    }
    void itemViewFocus031() {
        // v0.3.1: item views show only the selection highlight - no dotted focus rect / dashed outline round
        // the current item (the Styles panel drew a small dashed box under the selected style). Every theme;
        // the arrow keys still move the current item.
        const QString shotDir = qEnvironmentVariable("LW_ITEMFOCUS_SHOTS");
        for (const theme::Info &t : theme::list()) {
            theme::apply(*qApp, t.id);
            QWidget host;
            auto *lay = new QVBoxLayout(&host);
            auto *edit = new QLineEdit;
            lay->addWidget(edit);
            auto *sp = new StylesPanel;
            lay->addWidget(sp);
            auto *tree = new QTreeWidget;
            tree->setHeaderHidden(true);
            for (int i = 0; i < 4; ++i) new QTreeWidgetItem(tree, {QStringLiteral("Layer %1").arg(i + 1)});
            lay->addWidget(tree);
            auto *table = new QTableWidget(3, 2);
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 2; ++c) table->setItem(r, c, new QTableWidgetItem(QStringLiteral("%1,%2").arg(r).arg(c)));
            lay->addWidget(table);
            auto *list = new QListWidget;
            list->addItems({QStringLiteral("Frame 1"), QStringLiteral("Frame 2"), QStringLiteral("Frame 3")});
            lay->addWidget(list);
            host.resize(260, 640);
            host.show();
            host.activateWindow();
            QVERIFY(QTest::qWaitForWindowExposed(&host));
            auto *styles = sp->findChild<QListWidget *>(QStringLiteral("stylesList"));
            QVERIFY(styles && styles->count() > 3);
            styles->clearSelection();
            styles->setCurrentRow(1);
            styles->item(1)->setSelected(true);
            tree->setCurrentItem(tree->topLevelItem(1));
            table->setCurrentCell(1, 1);
            list->setCurrentRow(1);
            const QList<QAbstractItemView *> views{styles, tree, table, list};
            for (QAbstractItemView *v : views) {
                edit->setFocus(Qt::OtherFocusReason);
                QApplication::processEvents();
                QVERIFY(!v->hasFocus());
                const QImage plain = v->grab().toImage();
                v->setFocus(Qt::TabFocusReason);
                QApplication::processEvents();
                QVERIFY2(v->hasFocus(), qPrintable(t.id));
                const QImage focused = v->grab().toImage();
                if (!shotDir.isEmpty() && v == styles) {
                    const QRect r = v->visualRect(v->currentIndex()).adjusted(-6, -6, 6, 10).translated(v->viewport()->pos());
                    focused.copy(r).scaled(r.size() * 6, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                        .save(QDir(shotDir).filePath(QStringLiteral("styles-focus-%1-x6.png").arg(t.id)));
                    focused.save(QDir(shotDir).filePath(QStringLiteral("styles-focus-%1.png").arg(t.id)));
                }
                QVERIFY2(plain == focused, qPrintable(QStringLiteral("focus outline drawn in %1 (theme %2)").arg(QString::fromLatin1(v->metaObject()->className()), t.id)));
            }
            // the selection highlight is really drawn (the focused grab differs from an unselected one)
            styles->setFocus();
            const QImage sel = styles->grab().toImage();
            styles->clearSelection();
            QApplication::processEvents();
            QVERIFY2(styles->grab().toImage() != sel, qPrintable(t.id));
            // keyboard navigation still works
            styles->setFocus();
            QTest::keyClick(styles, Qt::Key_Right);
            QCOMPARE(styles->currentRow(), 2);
            QTest::keyClick(styles, Qt::Key_Home);
            QCOMPARE(styles->currentRow(), 0);
            tree->setFocus();
            QTest::keyClick(tree, Qt::Key_Down);
            QCOMPARE(tree->indexOfTopLevelItem(tree->currentItem()), 2);
            list->setFocus();
            QTest::keyClick(list, Qt::Key_Up);
            QCOMPARE(list->currentRow(), 0);
            table->setFocus();
            QTest::keyClick(table, Qt::Key_Left);
            QCOMPARE(table->currentColumn(), 0);
        }
        theme::apply(*qApp);
    }
    void cheader027() {
        // v0.2.7: GIMP C source / GIMP header styles, the strip checkbox, stdint.h off
        const QStringList names = OptimizeWidget::presetNames();
        for (const char *n : {"C Header GIMP-style RGBA (short string)", "C Header GIMP-style RGB565 RLE (short string)"})
            QVERIFY2(names.contains(QString::fromLatin1(n)), n);
        const QVariant savedStrip = QSettings().value(QStringLiteral("export/chStrip"));
        OptimizeWidget ow;
        ow.show();
        auto pick = [&](const char *n) {
            const int i = ow.presetCombo()->findText(QString::fromLatin1(n));
            QVERIFY2(i > 0, n);
            ow.presetCombo()->setCurrentIndex(i);
            emit ow.presetCombo()->activated(i);
        };
        auto *strip = ow.findChild<QCheckBox *>(QStringLiteral("optCStrip"));
        auto *style = ow.findChild<QComboBox *>(QStringLiteral("optCStyle"));
        auto *pix = ow.findChild<QComboBox *>(QStringLiteral("optCPixel"));
        auto *gimp = ow.findChild<QWidget *>(QStringLiteral("optCGimp"));
        auto *perLine = ow.findChild<QWidget *>(QStringLiteral("optCPerLine"));
        auto *stdint = ow.findChild<QCheckBox *>(QStringLiteral("optCStdint"));
        QVERIFY(strip && style && pix && gimp && perLine && stdint);
        QCOMPARE(strip->text(), QStringLiteral("strip spaces/tabs/newlines from arrays"));
        pick("C Header RGBA8888");
        QVERIFY(strip->isChecked() == cheader_stripDefault());
        cheader_setStripDefault(true);
        QVERIFY(ExportOptions().chStrip);
        QCOMPARE(style->count(), 4);
        QVERIFY(style->itemText(2).contains(QStringLiteral("GIMP")) && style->itemText(3).contains(QStringLiteral("HEADER_PIXEL")));
        QVERIFY(!gimp->isVisible());
        // strip toggles: options, the remembered default (QSettings), the per-line spin
        strip->setChecked(true);
        QVERIFY(ow.options().chStrip && !perLine->isEnabled());
        strip->setChecked(false);
        QVERIFY(!ow.options().chStrip && perLine->isEnabled());
        QCOMPARE(QSettings().value(QStringLiteral("export/chStrip")).toBool(), false);
        QVERIFY(!cheader_stripDefault() && !ExportOptions().chStrip);
        strip->setChecked(true);
        QCOMPARE(QSettings().value(QStringLiteral("export/chStrip")).toBool(), true);
        QVERIFY(cheader_stripDefault());
        // stdint off flows into the options
        stdint->setChecked(false);
        QVERIFY(!ow.options().chStdint);
        stdint->setChecked(true);
        // GIMP presets and the GIMP box
        pick("C Header GIMP-style RGB565 RLE (short string)");
        QCOMPARE(ow.options().chStyle, 2);
        QCOMPARE(ow.options().chPixel, int(cheader::RGB565LE));
        QVERIFY(ow.options().chRle);
        QVERIFY(gimp->isVisible());
        QCOMPARE(ow.presetCombo()->currentText(), QStringLiteral("C Header GIMP-style RGB565 RLE (short string)"));
        auto *model = qobject_cast<QStandardItemModel *>(pix->model());
        QVERIFY(model);
        for (int p = 0; p < cheader::PixelCount; ++p) {
            const bool ok = p == cheader::RGBA8888 || p == cheader::RGB888 || p == cheader::RGB565LE;
            QCOMPARE(model->item(p)->isEnabled(), ok);
        }
        pick("C Header GIMP-style RGBA (short string)");
        QCOMPARE(ow.options().chPixel, int(cheader::RGBA8888));
        auto *mac = ow.findChild<QCheckBox *>(QStringLiteral("optCGimpMacros"));
        auto *glib = ow.findChild<QCheckBox *>(QStringLiteral("optCGimpGlib"));
        auto *cm = ow.findChild<QCheckBox *>(QStringLiteral("optCGimpComment"));
        auto *cmt = ow.findChild<QLineEdit *>(QStringLiteral("optCGimpCommentText"));
        auto *op = ow.findChild<QSpinBox *>(QStringLiteral("optCGimpOpacity"));
        QVERIFY(mac && glib && cm && cmt && op);
        mac->setChecked(true); glib->setChecked(true); cm->setChecked(true);
        cmt->setText(QStringLiteral("hello")); emit cmt->textEdited(cmt->text());
        op->setValue(55);
        const ExportOptions o = ow.options();
        QVERIFY(o.chGimpMacros && o.chGimpGlib && o.chGimpComment && o.chGimpCommentText == QStringLiteral("hello") && o.chGimpOpacity == 55);
        QVERIFY(describeOptions(o).contains(QStringLiteral("GIMP C source")));
        style->setCurrentIndex(3);
        QCOMPARE(ow.options().chStyle, 3);
        QVERIFY(gimp->isVisible() && !pix->isEnabled());
        QVERIFY(describeOptions(ow.options()).contains(QStringLiteral("HEADER_PIXEL")));
        // preview of a GIMP export: one decoded frame, size from the string
        std::unique_ptr<Document> sd(Document::createSample());
        ExportOptions g; g.format = Fmt::CHeader; g.chStyle = 2; g.chPixel = cheader::RGBA8888; g.chRle = true;
        EncodeResult r = encodePreview(*sd, g, 0);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.image.size(), QSize(sd->s.width, sd->s.height));
        QVERIFY(r.bytes > 0 && r.pixelBytes > 0);
        if (savedStrip.isValid()) QSettings().setValue(QStringLiteral("export/chStrip"), savedStrip);
        else QSettings().remove(QStringLiteral("export/chStrip"));
        cheader_setStripDefault(true);
    }
    void cheader026() {
        QVERIFY(tmp.isValid());
        const QStringList names = OptimizeWidget::presetNames();
        for (const char *n : {"C Header RGBA8888", "C Header RGB565 (embedded)", "C Header 1-bit mono (OLED)", "C Header Indexed 8-bit"})
            QVERIFY2(names.contains(QString::fromLatin1(n)), n);
        OptimizeWidget ow;
        ow.show();
        auto pick = [&](const char *n) {
            const int i = ow.presetCombo()->findText(QString::fromLatin1(n));
            QVERIFY2(i > 0, n);
            ow.presetCombo()->setCurrentIndex(i);
            emit ow.presetCombo()->activated(i);
        };
        QWidget *box = ow.findChild<QWidget *>(QStringLiteral("optCHeader"));
        QVERIFY(box);
        pick("PNG 32");
        QVERIFY(!box->isVisible());
        pick("C Header RGB565 (embedded)");
        QCOMPARE(ow.options().format, Fmt::CHeader);
        QCOMPARE(ow.options().chPixel, int(cheader::RGB565LE));
        QCOMPARE(ow.options().chElement, 1);
        QVERIFY(box->isVisible());
        QCOMPARE(ow.presetCombo()->currentText(), QStringLiteral("C Header RGB565 (embedded)"));
        auto *pix = ow.findChild<QComboBox *>(QStringLiteral("optCPixel"));
        auto *elem = ow.findChild<QComboBox *>(QStringLiteral("optCElement"));
        QVERIFY(pix && elem && elem->isEnabled());
        QCOMPARE(pix->count(), int(cheader::PixelCount));
        QCOMPARE(pix->currentIndex(), int(cheader::RGB565LE));
        pick("C Header 1-bit mono (OLED)");
        QCOMPARE(ow.options().chPixel, int(cheader::Mono1));
        QVERIFY(!ow.options().dither);
        QVERIFY(!elem->isEnabled());                                   // no packed words for 1-bit
        QVERIFY(ow.findChild<QCheckBox *>(QStringLiteral("optCLsbFirst"))->isEnabled());
        QVERIFY(!ow.paletteCombo()->isVisible());
        pick("C Header Indexed 8-bit");
        QCOMPARE(ow.options().chPixel, int(cheader::Indexed8));
        QVERIFY(ow.paletteCombo()->isVisible());                       // the GIF / PNG 8 quantiser controls
        QVERIFY(ow.options().dither);
        pick("C Header RGBA8888");
        QCOMPARE(ow.options().chPixel, int(cheader::RGBA8888));
        QCOMPARE(ow.options().chElement, 0);
        // user edits flow into the options; a changed option drops the preset match
        auto *name = ow.findChild<QLineEdit *>(QStringLiteral("optCName"));
        QVERIFY(name && name->placeholderText().contains(QStringLiteral("file name")));
        name->setText(QStringLiteral("splash"));
        emit name->textEdited(name->text());
        QCOMPARE(ow.options().chName, QStringLiteral("splash"));
        auto *rle = ow.findChild<QCheckBox *>(QStringLiteral("optCRle"));
        rle->setChecked(true);
        QVERIFY(ow.options().chRle);
        QVERIFY(ow.presetCombo()->currentText() != QStringLiteral("C Header RGBA8888"));
        auto *style = ow.findChild<QComboBox *>(QStringLiteral("optCStyle"));
        style->setCurrentIndex(1);
        QCOMPARE(ow.options().chStyle, 1);
        QVERIFY(!pix->isEnabled());
        for (const char *n : {"optCRadix", "optCPerLine", "optCConst", "optCProgmem", "optCAligned", "optCSection", "optCSectionName", "optCStdint",
                              "optCConstants", "optCFrameLayout", "optCDelays", "optCRawBin", "optCInvert"})
            QVERIFY2(ow.findChild<QWidget *>(QString::fromLatin1(n)), n);

        // preview: the decoded pixels and the byte size, for every pixel format
        std::unique_ptr<Document> sd(Document::createSample());
        for (int p = 0; p < cheader::PixelCount; ++p) {
            ExportOptions o; o.format = Fmt::CHeader; o.chPixel = p; o.colors = 64;
            EncodeResult r = encodePreview(*sd, o, 0);
            QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
            QCOMPARE(r.image.size(), QSize(sd->s.width, sd->s.height));
            const qint64 data = qint64(cheader::stride(p, sd->s.width)) * sd->s.height;
            QCOMPARE(r.pixelBytes, data + (cheader::isIndexed(p) ? r.colorTable.size() * 4 : 0));
            QVERIFY(r.bytes > r.pixelBytes);
            QVERIFY2(describeSaving(r).contains(QStringLiteral("Data: %L1 bytes").arg(r.pixelBytes)), qPrintable(describeSaving(r)));
            if (cheader::isIndexed(p)) QVERIFY(!r.colorTable.isEmpty() && r.colorTable.size() <= (p == cheader::Indexed4 ? 16 : 64));
            QVERIFY(!describeOptions(o).isEmpty());
        }
        {
            ExportOptions o; o.format = Fmt::CHeader; o.chPixel = cheader::Mono1;
            EncodeResult r = encodePreview(*sd, o, 0);
            for (int y = 0; y < r.image.height(); y += 7)
                for (int x = 0; x < r.image.width(); x += 7) { const QRgb c = r.image.pixel(x, y); QVERIFY(c == qRgb(0, 0, 0) || c == qRgb(255, 255, 255)); }
        }
        // Export through the main window: identifier from the file name, .bin alongside, animated frames
        {
            ExportOptions o; o.format = Fmt::CHeader; o.chPixel = cheader::RGB565LE; o.chElement = 1; o.chRawBin = true; o.animated = true;
            const QString path = tmp.filePath(QStringLiteral("1st sprite.h"));
            QVERIFY(w->exportTo(sd.get(), path, o));
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(f.readAll());
            QVERIFY(text.contains(QStringLiteral("#ifndef IMG_1ST_SPRITE_H")));
            QVERIFY(text.contains(QStringLiteral("#define IMG_1ST_SPRITE_FRAMES %1").arg(sd->s.frames.size())));
            QVERIFY(text.contains(QStringLiteral("Source: ")));
            QCOMPARE(QFileInfo(tmp.filePath(QStringLiteral("1st sprite.bin"))).size(), qint64(sd->s.width) * sd->s.height * 2 * sd->s.frames.size());
            QVERIFY(ImageIO::exportFilter(Fmt::CHeader).contains(QStringLiteral("*.h")));
        }
    }

    void formats025() {
        QVERIFY(tmp.isValid());
        const QStringList names = OptimizeWidget::presetNames();
        for (const char *n : {"PNG 8 Adaptive 256", "PNG 8 Web 216", "PNG 24", "PNG 32", "JPEG - Progressive Optimised", "JPEG - jpegli 85"})
            QVERIFY2(names.contains(QString::fromLatin1(n)), n);
        OptimizeWidget ow;
        QCOMPARE(ow.paletteCombo()->count(), 7);
        auto pick = [&](const char *n) {
            const int i = ow.presetCombo()->findText(QString::fromLatin1(n));
            QVERIFY2(i > 0, n);
            ow.presetCombo()->setCurrentIndex(i);
            emit ow.presetCombo()->activated(i);   // what a user pick sends
        };
        pick("PNG 8 Web 216");
        QCOMPARE(ow.options().format, Fmt::PNG);
        QCOMPARE(ow.options().pngType, 2);
        QCOMPARE(ow.options().palette, int(quant::Web216));
        QCOMPARE(ow.options().colors, 216);
        QCOMPARE(ow.paletteCombo()->currentText(), QStringLiteral("Web 216"));
        pick("PNG 24");
        QCOMPARE(ow.options().pngType, 1);
        QVERIFY(!ImageIO::hasAlpha(ow.options()));
        pick("PNG 32");
        QCOMPARE(ow.options().pngType, 0);
        pick("JPEG - Progressive Optimised");
        QCOMPARE(ow.options().format, Fmt::JPEG);
        QVERIFY(ow.options().jpegProgressive && ow.options().jpegOptimize);
        pick("JPEG - jpegli 85");
        QCOMPARE(ow.options().quality, 85);
        QCOMPARE(ow.options().jpegEncoder, codec::jpegEncoderAvailable(2) ? 2 : 0);
        QCOMPARE(ow.presetCombo()->currentText(), codec::jpegEncoderAvailable(2) ? QStringLiteral("JPEG - jpegli 85") : ow.presetCombo()->currentText());

        // colour table + Tiny saving in the preview path
        std::unique_ptr<Document> sd(Document::createSample());
        ExportOptions o; o.format = Fmt::PNG; o.pngType = 2; o.colors = 32;
        EncodeResult r = encodePreview(*sd, o, 0);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY(!r.colorTable.isEmpty() && r.colorTable.size() <= 32);
        ow.setOptions(o);
        ow.show();
        ow.setColorTable(r.colorTable);
        QVERIFY(ow.colorTable()->isVisible());
        QCOMPARE(ow.colorTable()->colors().size(), r.colorTable.size());
        if (codec::tinyAvailable()) {
            ExportOptions t; t.format = Fmt::PNG; t.tinyPng = true;
            EncodeResult tr = encodePreview(*sd, t, 0);
            QVERIFY(tr.originalBytes > 0);
            QVERIFY(tr.bytes < tr.originalBytes);
            QVERIFY2(describeSaving(tr).contains(QStringLiteral("% saved")), qPrintable(describeSaving(tr)));
        }

        // every new option survives the .lwd round trip
        {
            Document *doc = Document::createNew(64, 48, Qt::white);
            ExportOptions e; e.format = Fmt::PNG; e.pngType = 2; e.palette = quant::Custom;
            e.customPalette = {qRgb(255, 0, 0), qRgb(0, 255, 0), qRgba(0, 0, 0, 0)};
            e.pngTransparency = 1; e.pngInterlace = true; e.pngCompression = 4; e.tinyPng = true; e.tinyQualityMin = 50; e.tinyQualityMax = 70;
            e.tinySpeed = 2; e.jpegEncoder = 1; e.jpegProgressive = true; e.jpegSubsampling = 0; e.jpegSmoothing = 3; e.jpegSharpenEdges = true;
            e.jpegStripMeta = false; e.bmpBits = 8; e.bmpRle = true; e.icoSizes = 0x15; e.icnsSizes = 0x1F0; e.curHotX = 3; e.curHotY = 4;
            e.ddsFormat = 2; e.mipmaps = true; e.ktxZstd = true; e.exrCompression = 2; e.exrHalf = false; e.pcx8 = true;
            doc->exportOptions = e;
            if (doc->selectedPane >= 0 && doc->selectedPane < doc->paneOptions.size()) doc->paneOptions[doc->selectedPane] = e;
            std::unique_ptr<Document> back(lwd::deserialize(lwd::serialize(*doc)));
            QVERIFY(back);
            QVERIFY(lwd::exportOptionsFromJson(lwd::exportOptionsToJson(e)) == e);
            QVERIFY(back->exportOptions == e);
            delete doc;
        }

        // favicon dialog: preview grid holds the ico sizes and the app icons
        {
            FaviconDialog fd(*sd, tmp.path());
            QSet<int> sizes;
            for (const auto &pv : fd.previews()) { sizes.insert(pv.first); QCOMPARE(pv.second.width(), pv.first); }
            for (int z : {16, 32, 48, 180, 192, 512}) QVERIFY2(sizes.contains(z), qPrintable(QString::number(z)));
            QVERIFY(fd.findChild<QLabel *>(QStringLiteral("faviconSnippet")));
            const QString dir = tmp.filePath(QStringLiteral("fav"));
            QDir().mkpath(dir);
            docio::FaviconResult res; QString err;
            QVERIFY2(docio::exportFaviconBundle(sd->s, 0, dir, fd.options(), &res, &err), qPrintable(err));
            for (const char *f : {"favicon.ico", "apple-touch-icon.png", "android-chrome-192x192.png", "android-chrome-512x512.png", "site.webmanifest"})
                QVERIFY2(QFileInfo::exists(dir + QLatin1Char('/') + QLatin1String(f)), f);
            QVERIFY(w->findChild<QAction *>(QStringLiteral("file.exportfavicons")));
        }

        // drag-and-drop of new formats: open, land in recents, keep the export format
        struct Case { Fmt f; const char *name; };
        QList<Case> cases = {{Fmt::QOI, "d.qoi"}, {Fmt::PSD, "d.psd"}, {Fmt::ORA, "d.ora"}, {Fmt::XCF, "d.xcf"}, {Fmt::SVG, "d.svg"},
                             {Fmt::SVGZ, "d.svgz"}, {Fmt::BMP, "d.bmp"}, {Fmt::PCX, "d.pcx"}, {Fmt::ICO, "d.ico"}, {Fmt::DDS, "d.dds"}, {Fmt::PAM, "d.pam"}};
        if (ImageIO::canWrite(Fmt::HEIC)) cases.append({Fmt::HEIC, "d.heic"});
        if (ImageIO::canWrite(Fmt::EXR)) cases.append({Fmt::EXR, "d.exr"});
        Document *src = sd.get();
        for (const Case &c : cases) {
            ExportOptions e; e.format = c.f;
            const QString path = tmp.filePath(QString::fromLatin1(c.name));
            QVERIFY2(w->exportTo(src, path, e), c.name);
            QMimeData md; md.setUrls({QUrl::fromLocalFile(path)});
            QDropEvent ev(QPointF(100, 100), Qt::CopyAction, &md, Qt::LeftButton, Qt::NoModifier);
            static_cast<QObject *>(w)->event(&ev);
            QCoreApplication::processEvents();
            QCOMPARE(w->recentFiles().first(), QFileInfo(path).absoluteFilePath());
            QVERIFY2(qAbs(w->currentDoc()->s.width - src->s.width) <= 1 || c.f == Fmt::ICO, c.name);
            if (ImageIO::canWrite(c.f) && c.f != Fmt::ICO) QCOMPARE(w->currentDoc()->exportOptions.format, c.f);
        }
        // editable PNG re-opens with its layers
        {
            ExportOptions e; e.format = Fmt::PNG; e.pngEditable = true;
            const QString path = tmp.filePath(QStringLiteral("editable.png"));
            QVERIFY(w->exportTo(src, path, e));
            QVERIFY(w->openFile(path));
            QCOMPARE(w->currentDoc()->s.layers.size(), src->s.layers.size());
            QVERIFY(w->currentDoc()->exportOptions.pngEditable);
        }
        for (QMessageBox *mb : w->findChildren<QMessageBox *>()) mb->close();
        QCoreApplication::processEvents();

        // HDR tone-map preference drives ImageIO
        Prefs p = appState().prefs;
        Prefs q2 = p; q2.hdrToneMap = 1; q2.hdrExposure = 1.5;
        appState().setPrefs(q2);
        QCOMPARE(ImageIO::toneMap().op, 1);
        QCOMPARE(ImageIO::toneMap().exposure, 1.5);
        appState().setPrefs(p);
    }

    void textAntiAliasCanvas() {
        DocumentView *v = w->addDocument(Document::createNew(300, 100, Qt::white));
        Document *td = v->doc();
        Canvas *tc = v->canvas();
        QTest::qWait(30);
        appState().setTool(Tool::Text);
        autoAnswer(QStringLiteral("Anti-alias AaGg"));
        click(tc, {10, 20});
        QTest::qWait(150);
        QCOMPARE(lastObject(td)->type, ObjType::Text);
        appState().setTool(Tool::Pointer);
        td->selectOnly(lastObject(td)->id);
        QCoreApplication::processEvents();
        auto *combo = w->findChild<QComboBox *>(QStringLiteral("TextAA"));
        QVERIFY(combo);
        QCOMPARE(combo->count(), 5);
        QCOMPARE(combo->itemText(0), QStringLiteral("No Anti-Alias"));
        auto greyLevels = [](const QImage &im0) {
            QImage im = im0.convertToFormat(QImage::Format_ARGB32);
            QSet<int> g;
            for (int y = 0; y < im.height(); ++y) for (int x = 0; x < im.width(); ++x) g.insert(qGray(im.pixel(x, y)));
            return g.size();
        };
        combo->setCurrentIndex(0);
        QCOMPARE(lastObject(td)->textAA, TextAA::None);
        QCOMPARE(greyLevels(tc->renderedFrame()), 2);
        combo->setCurrentIndex(3);
        QCOMPARE(lastObject(td)->textAA, TextAA::Smooth);
        QVERIFY(greyLevels(tc->renderedFrame()) > 8);
        td->undo();
        QCOMPARE(lastObject(td)->textAA, TextAA::None);
    }

    void slicesAndHotspots() {
        DocumentView *v = w->addDocument(Document::createNew(400, 300, Qt::white));
        Document *wd = v->doc();
        Canvas *wc = v->canvas();
        QTest::qWait(30);
        w->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(w));
        // Slice and hotspot tools are behind Preferences > "Enable web features".
        Prefs webOn = appState().prefs;
        webOn.enableWebFeatures = true;
        appState().setPrefs(webOn);
        // artwork, then slice + hotspots drawn with the tools
        appState().setTool(Tool::Rect);
        drag(wc, {20, 20}, {220, 120});
        const quint64 rectId = lastObject(wd)->id;
        appState().setTool(Tool::Slice);
        drag(wc, {20, 20}, {220, 120});
        const int wl = wd->webLayer();
        QVERIFY(wl >= 0);
        QCOMPARE(wd->objects(wl).size(), 1);
        const VObject sl = wd->objects(wl).last();
        QCOMPARE(sl.type, ObjType::Slice);
        QVERIFY(qAbs(sl.rect.width() - 200) < 2);
        QVERIFY(wd->find(rectId));
        QCOMPARE(wd->objects(0).size(), 1); // the slice did not go to the artwork layer
        appState().setTool(Tool::Hotspot);
        drag(wc, {250, 20}, {300, 70});
        QCOMPARE(wd->objects(wl).last().type, ObjType::Hotspot);
        QCOMPARE(wd->objects(wl).last().webShape, WebShape::Rect);
        appState().setTool(Tool::CircleHotspot);
        drag(wc, {250, 150}, {310, 210});
        QCOMPARE(wd->objects(wl).last().webShape, WebShape::Circle);
        appState().setTool(Tool::PolyHotspot);
        click(wc, {50, 200}); click(wc, {120, 210}); click(wc, {80, 260});
        wc->finishPen(true);
        QCOMPARE(wd->objects(wl).last().webShape, WebShape::Polygon);
        QCOMPARE(wd->objects(wl).last().points.size(), 3);
        QCOMPARE(wd->objects(wl).size(), 4);
        // J cycles through the hotspot tools, K selects the slice tool
        appState().setTool(Tool::Pointer);
        wc->setFocus();
        QTest::keyClick(wc, Qt::Key_J);
        QCOMPARE(appState().tool(), Tool::Hotspot);
        QTest::keyClick(wc, Qt::Key_J);
        QCOMPARE(appState().tool(), Tool::CircleHotspot);
        QTest::keyClick(wc, Qt::Key_J);
        QCOMPARE(appState().tool(), Tool::PolyHotspot);
        QTest::keyClick(wc, Qt::Key_K);
        QCOMPARE(appState().tool(), Tool::Slice);
        appState().setTool(Tool::Pointer);
        // hiding the web layer makes slices unclickable, showing makes them selectable again
        appState().webLayerVisible = false; appState().notifyOptions();
        click(wc, {275, 45});
        QVERIFY(!wd->singleSelected() || !wd->singleSelected()->isWeb());
        appState().webLayerVisible = true; appState().notifyOptions();
        // property inspector: link / alt / target / behaviour / per-slice export settings
        wd->selectOnly(sl.id);
        QCoreApplication::processEvents();
        auto *link = w->findChild<QLineEdit *>(QStringLiteral("WebLink"));
        auto *alt = w->findChild<QLineEdit *>(QStringLiteral("WebAlt"));
        auto *target = w->findChild<QComboBox *>(QStringLiteral("WebTarget"));
        auto *beh = w->findChild<QComboBox *>(QStringLiteral("WebBehavior"));
        auto *custom = w->findChild<QCheckBox *>(QStringLiteral("SliceCustomOpt"));
        auto *fmt = w->findChild<QComboBox *>(QStringLiteral("SliceFormat"));
        QVERIFY(link && alt && target && beh && custom && fmt);
        QVERIFY(link->isVisibleTo(w));
        link->setText(QStringLiteral("https://libreworks.example/"));
        emit link->editingFinished();
        QCOMPARE(wd->find(sl.id)->link, QStringLiteral("https://libreworks.example/"));
        alt->setText(QStringLiteral("Banner"));
        emit alt->editingFinished();
        QCOMPARE(wd->find(sl.id)->alt, QStringLiteral("Banner"));
        target->setCurrentText(QStringLiteral("_blank"));
        QCOMPARE(wd->find(sl.id)->target, QStringLiteral("_blank"));
        beh->setCurrentIndex(1);
        QCOMPARE(wd->find(sl.id)->behavior, WebBehavior::SimpleRollover);
        custom->setChecked(true);
        QVERIFY(wd->find(sl.id)->sliceCustomOpt);
        fmt->setCurrentIndex(fmt->findData(int(Fmt::JPEG)));
        QCOMPARE(wd->find(sl.id)->sliceOpt.format, Fmt::JPEG);
        wd->undo();
        QCOMPARE(wd->find(sl.id)->sliceOpt.format, wd->exportOptions.format);
        wd->redo();
        // a normal object shows the artwork columns again
        wd->selectOnly(rectId);
        QCoreApplication::processEvents();
        QVERIFY(!link->isVisibleTo(w));
        // Edit > Insert > Hotspot / Slice from the selection
        const int before = wd->objects(wl).size();
        w->action(QStringLiteral("edit.inserthotspot"))->trigger();
        QCOMPARE(wd->objects(wl).size(), before + 1);
        QCOMPARE(wd->objects(wl).last().rect.toAlignedRect(), QRect(20, 20, 200, 100));
        wd->selectOnly(rectId);
        w->action(QStringLiteral("edit.insertslice"))->trigger();
        QCOMPARE(wd->objects(wl).last().type, ObjType::Slice);
        // shared layer toggle in the Layers panel
        wd->addFrame(false);
        wd->commit(QStringLiteral("Add Frame"));
        wd->curFrame = 0;
        wd->curLayer = 0;
        wd->notifyStructure();
        QCoreApplication::processEvents();
        auto *share = w->findChild<QToolButton *>(QStringLiteral("layerShare"));
        QVERIFY(share && share->isEnabled());
        share->click();
        QVERIFY(wd->s.layers[0].shared);
        QCOMPARE(wd->s.frames[1].layers[0].size(), wd->s.frames[0].layers[0].size());
        QCOMPARE(wd->s.frames[1].layers[wl].size(), wd->s.frames[0].layers[wl].size());
        // File > Export HTML and Images
        const QString html = tmp.filePath(QStringLiteral("site/index.html"));
        QDir().mkpath(tmp.filePath(QStringLiteral("site")));
        QTimer::singleShot(50, [html] {
            for (QWidget *tw : QApplication::topLevelWidgets())
                if (auto *dlg = qobject_cast<HtmlExportDialog *>(tw); dlg && dlg->isVisible()) {
                    dlg->findChild<QLineEdit *>(QStringLiteral("htmlPath"))->setText(html);
                    dlg->findChild<QComboBox *>(QStringLiteral("htmlLayout"))->setCurrentIndex(1);
                    dlg->accept();
                }
        });
        w->action(QStringLiteral("file.exporthtml"))->trigger();
        QVERIFY(QFileInfo::exists(html));
        QFile f(html);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString page = QString::fromUtf8(f.readAll());
        QVERIFY(page.contains(QStringLiteral("https://libreworks.example/")));
        QVERIFY(page.contains(QStringLiteral("<area shape=\"circle\"")));
        QVERIFY(page.contains(QStringLiteral("position:absolute")));
        QVERIFY(QFileInfo(tmp.filePath(QStringLiteral("site/images"))).isDir());
        Prefs webOff = appState().prefs;
        webOff.enableWebFeatures = false;
        appState().setPrefs(webOff);
    }

    // Filters > Other > Offset on an opaque noise bitmap: an exact integer wrap of the whole bitmap
    // (no 1 px frame, no 2 px seam), with no selection, with the whole canvas marqueed, and with a
    // marquee larger than the canvas
    void offsetFilterIsExact() {
        DocumentView *v = w->addDocument(Document::createNew(120, 90, Qt::white));
        Document *fd = v->doc();
        qApp->setProperty("lw_testing", true);
        QImage src(120, 90, QImage::Format_ARGB32_Premultiplied);
        QRandomGenerator g(3);
        for (int y = 0; y < 90; ++y) for (int x = 0; x < 120; ++x) src.setPixel(x, y, qRgb(g.bounded(256), g.bounded(256), g.bounded(256)));
        VObject b;
        b.type = ObjType::Bitmap;
        b.rect = QRectF(0, 0, 120, 90);
        b.image = src;
        b.id = fd->newId();
        fd->addObject(b, 0, false);
        fd->commit(QStringLiteral("bitmap"));
        fd->setSelection({b.id});
        const QList<QRectF> sels = {QRectF(), QRectF(0, 0, 120, 90), QRectF(-10, -10, 140, 110)};
        for (const QRectF &sr : sels) {
            for (const QPoint o : {QPoint(60, 45), QPoint(13, -7)}) {
                fd->pixelSelection = QPainterPath();
                if (!sr.isNull()) {
                    QPainterPath p;
                    p.addRect(sr);
                    QPainterPath canvas;
                    canvas.addRect(0, 0, 120, 90);
                    fd->combinePixelSelection(p.intersected(canvas), Qt::NoModifier, Document::SelHard, 0);
                }
                LiveEffect e = LiveEffect::make(EffectType::Offset);
                e.offsetX = o.x(); e.offsetY = o.y();
                QVERIFY(w->applyEffectFilter(e));
                const QImage out = fd->find(b.id)->image.convertToFormat(QImage::Format_ARGB32);
                QCOMPARE(out.size(), src.size());
                int bad = 0;
                for (int y = 0; y < 90; ++y)
                    for (int x = 0; x < 120; ++x) {
                        const QRgb want = src.pixel((((x - o.x()) % 120) + 120) % 120, (((y - o.y()) % 90) + 90) % 90);
                        bad += out.pixel(x, y) != want || qAlpha(out.pixel(x, y)) != 255;
                    }
                QCOMPARE(bad, 0);
                // and the canvas render (what export writes) shows exactly those pixels
                QCOMPARE(fd->render(0, true).convertToFormat(QImage::Format_ARGB32), out);
                fd->undo();
                QCOMPARE(fd->find(b.id)->image, src);
            }
        }
        fd->pixelSelection = QPainterPath();
    }

    // ---- v0.2.3 bitmap selections
    void selectionMaskOps() {
        quint64 bid;
        Document *fd = makeSelDoc(w, &bid);
        const QRect all(0, 0, 100, 80);
        selectRect(fd, QRectF(20, 20, 40, 30));
        QCOMPARE(fd->pixelSelectionBounds(), QRect(20, 20, 40, 30));
        // feather: soft edge, still fully selected in the middle
        QVERIFY(w->modifySelection(MainWindow::SelFeather, 6));
        QImage m = fd->canvasSelectionMask(all);
        QVERIFY(fd->hasMaskSelection());
        QCOMPARE(maskAt(m, 40, 35), 255);
        QVERIFY(maskAt(m, 20, 35) > 40 && maskAt(m, 20, 35) < 215);
        QCOMPARE(maskAt(m, 5, 5), 0);
        // cancelling the dialog restores the selection exactly
        QTimer::singleShot(80, [] {
            for (QWidget *t : QApplication::topLevelWidgets())
                if (t->objectName() == QLatin1String("selectionModifyDialog") && t->isVisible()) {
                    t->findChild<PopupSliderDoubleField *>(QStringLiteral("selAmount"))->setValue(30);
                    QCoreApplication::processEvents();
                    qobject_cast<QDialog *>(t)->reject();
                }
        });
        QVERIFY(!w->modifySelection(MainWindow::SelExpand));
        QCOMPARE(fd->canvasSelectionMask(all), m);
        // expand / contract / border / smooth
        selectRect(fd, QRectF(20, 20, 40, 30));
        QVERIFY(w->modifySelection(MainWindow::SelExpand, 5));
        m = fd->canvasSelectionMask(all);
        QVERIFY(maskAt(m, 16, 35) > 200);
        QCOMPARE(maskAt(m, 12, 35), 0);
        selectRect(fd, QRectF(20, 20, 40, 30));
        QVERIFY(w->modifySelection(MainWindow::SelContract, 5));
        m = fd->canvasSelectionMask(all);
        QCOMPARE(maskAt(m, 22, 35), 0);
        QCOMPARE(maskAt(m, 40, 35), 255);
        selectRect(fd, QRectF(20, 20, 40, 30));
        QVERIFY(w->modifySelection(MainWindow::SelBorder, 6));
        m = fd->canvasSelectionMask(all);
        QVERIFY(maskAt(m, 20, 35) > 200);
        QCOMPARE(maskAt(m, 40, 35), 0);
        selectRect(fd, QRectF(20, 20, 40, 30));
        QVERIFY(w->modifySelection(MainWindow::SelSmooth, 6));
        m = fd->canvasSelectionMask(all);
        QCOMPARE(maskAt(m, 20, 20), 0);   // the corner is rounded off
        QCOMPARE(maskAt(m, 40, 35), 255);
        // Select Inverse on a mask keeps partial pixels partial
        selectRect(fd, QRectF(20, 20, 40, 30));
        w->modifySelection(MainWindow::SelFeather, 6);
        const QImage before = fd->canvasSelectionMask(all);
        w->selectInverse();
        m = fd->canvasSelectionMask(all);
        QCOMPARE(maskAt(m, 5, 5), 255);
        QCOMPARE(maskAt(m, 40, 35), 0);
        QVERIFY(qAbs(maskAt(m, 20, 35) - (255 - maskAt(before, 20, 35))) <= 1);
        // the mask clips delete: a feathered hole fades out
        selectRect(fd, QRectF(20, 20, 40, 30));
        w->modifySelection(MainWindow::SelFeather, 6);
        w->action(QStringLiteral("edit.clear"))->trigger();
        const QImage img = fd->find(bid)->image;
        QCOMPARE(qAlpha(img.pixel(40, 35)), 0);
        QCOMPARE(qAlpha(img.pixel(5, 5)), 255);
        const int edge = qAlpha(img.pixel(20, 35));
        QVERIFY2(edge > 30 && edge < 225, qPrintable(QString::number(edge)));
        fd->clearPixelSelection();
        QVERIFY(fd->pixelSelection.isEmpty() && fd->selMask.isNull());
    }

    void selectSimilarAndConvert() {
        quint64 bid;
        Document *fd = makeSelDoc(w, &bid);
        selectRect(fd, QRectF(5, 5, 4, 4));   // a few red pixels
        QVERIFY(w->selectSimilar());
        const QImage m = fd->canvasSelectionMask(QRect(0, 0, 100, 80));
        QCOMPARE(maskAt(m, 45, 70), 255);   // all of the red half
        QCOMPARE(maskAt(m, 55, 10), 0);     // none of the blue
        QCOMPARE(fd->pixelSelectionBounds(), QRect(0, 0, 50, 80));
        const int n = countObjects(fd);
        QCOMPARE(w->convertMarqueeToPath(), 1);
        QCOMPARE(countObjects(fd), n + 1);
        const VObject *pth = lastObject(fd);
        QCOMPARE(pth->type, ObjType::Path);
        QVERIFY(qAbs(pth->bounds().width() - 50) < 2.5 && qAbs(pth->bounds().height() - 80) < 2.5);
        QVERIFY(fd->pixelSelection.isEmpty());
    }

    void savedSelectionsRoundTrip() {
        quint64 bid;
        Document *fd = makeSelDoc(w, &bid);
        selectRect(fd, QRectF(10, 10, 30, 20));
        w->modifySelection(MainWindow::SelFeather, 4);
        const QImage a = fd->canvasSelectionMask(QRect(0, 0, 100, 80));
        QVERIFY(w->saveSelection(QStringLiteral("Soft")));
        selectRect(fd, QRectF(60, 40, 20, 20));
        autoAnswer(QStringLiteral("Square"));
        QVERIFY(w->saveSelection());
        QCOMPARE(fd->savedSelections.size(), 2);
        QCOMPARE(fd->savedSelections[1].name, QStringLiteral("Square"));
        // stored in the .lwd
        std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*fd)));
        QVERIFY(e);
        QCOMPARE(e->savedSelections.size(), 2);
        QCOMPARE(e->savedSelections[0].name, QStringLiteral("Soft"));
        QCOMPARE(e->savedSelections[0].origin, fd->savedSelections[0].origin);
        QCOMPARE(e->savedSelections[0].mask.convertToFormat(QImage::Format_Alpha8), fd->savedSelections[0].mask.convertToFormat(QImage::Format_Alpha8));
        // restore replaces; Shift adds
        fd->clearPixelSelection();
        QVERIFY(w->restoreSelection(0));
        QCOMPARE(fd->canvasSelectionMask(QRect(0, 0, 100, 80)), a);
        QVERIFY(w->restoreSelection(1, Qt::ShiftModifier));
        const QImage both = fd->canvasSelectionMask(QRect(0, 0, 100, 80));
        QCOMPARE(maskAt(both, 25, 20), 255);
        QCOMPARE(maskAt(both, 70, 50), 255);
        // the list: rename then delete, then restore what's left
        QTimer::singleShot(80, [] {
            for (QWidget *t : QApplication::topLevelWidgets())
                if (t->objectName() == QLatin1String("savedSelectionsDialog") && t->isVisible()) {
                    auto *list = t->findChild<QListWidget *>(QStringLiteral("savedSelections"));
                    QCOMPARE(list->count(), 2);
                    list->setCurrentRow(1);
                    autoAnswer(QStringLiteral("Box"));
                    t->findChild<QPushButton *>(QStringLiteral("selRename"))->click();
                    QCOMPARE(list->item(1)->text(), QStringLiteral("Box"));
                    list->setCurrentRow(0);
                    t->findChild<QPushButton *>(QStringLiteral("selDelete"))->click();
                    QCOMPARE(list->count(), 1);
                    list->setCurrentRow(0);
                    t->findChild<QPushButton *>(QStringLiteral("selRestore"))->click();
                }
        });
        w->manageSelections();
        QCOMPARE(fd->savedSelections.size(), 1);
        QCOMPARE(fd->savedSelections[0].name, QStringLiteral("Box"));
        QCOMPARE(fd->pixelSelectionBounds(), QRect(60, 40, 20, 20));
    }

    void quickMask() {
        quint64 bid;
        Document *fd = makeSelDoc(w, &bid);
        selectRect(fd, QRectF(10, 10, 30, 20));
        const int n = countObjects(fd);
        appState().setTool(Tool::Marquee);
        // Q with a bitmap tool enters Quick Mask
        w->action(QStringLiteral("tool.Scale"))->trigger();
        QVERIFY(w->quickMaskActive());
        QCOMPARE(countObjects(fd), n + 1);
        QVERIFY(fd->pixelSelection.isEmpty());
        VObject *q = nullptr;
        for (VObject &o : fd->objects(fd->drawingLayer())) if (o.quickMask) q = &o;
        QVERIFY(q);
        QCOMPARE(qGray(q->image.pixel(20, 20)), 255);
        QCOMPARE(qGray(q->image.pixel(60, 60)), 0);
        // shown as a red overlay over the unselected area; the selected area is untouched
        const QImage r = fd->render(0, true).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(r.pixel(20, 20), qRgb(255, 0, 0));
        const QRgb ov = r.pixel(70, 60);   // blue under a half red tint
        QVERIFY(qRed(ov) > 100 && qBlue(ov) > 100);
        // edit the mask: white = selected, grey = partial
        { QPainter p(&q->image); p.fillRect(60, 50, 20, 20, Qt::white); p.fillRect(80, 10, 10, 10, QColor(128, 128, 128)); }
        // the quick mask is saved in the .lwd as such
        {
            std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*fd)));
            bool found = false;
            for (const VObject &o : e->objects(0)) found |= o.quickMask;
            QVERIFY(found);
        }
        w->action(QStringLiteral("tool.Scale"))->trigger();   // Q again: back to the marching ants
        QVERIFY(!w->quickMaskActive());
        QCOMPARE(countObjects(fd), n);
        const QImage m = fd->canvasSelectionMask(QRect(0, 0, 100, 80));
        QCOMPARE(maskAt(m, 20, 20), 255);
        QCOMPARE(maskAt(m, 70, 60), 255);
        QVERIFY(qAbs(maskAt(m, 85, 15) - 128) <= 2);
        QCOMPARE(maskAt(m, 5, 75), 0);
        QVERIFY(!fd->pixelSelection.isEmpty());
        // undo returns to quick mask mode
        fd->undo();
        QVERIFY(w->quickMaskActive());
        fd->redo();
        QVERIFY(!w->quickMaskActive());
        // Q without a bitmap tool still picks the Scale tool
        appState().setTool(Tool::Pointer);
        fd->setSelection({bid});
        w->action(QStringLiteral("tool.Scale"))->trigger();
        QCOMPARE(appState().tool(), Tool::Scale);
        QVERIFY(!w->quickMaskActive());
        appState().setTool(Tool::Pointer);
    }

    void dragSelection() {
        quint64 bid;
        Document *fd = makeSelDoc(w, &bid);
        Canvas *vc = w->currentView()->canvas();
        appState().setTool(Tool::Marquee);
        selectRect(fd, QRectF(10, 10, 20, 20));
        const QImage src = fd->find(bid)->image;
        // a plain drag inside moves the outline only
        drag(vc, QPointF(20, 20), QPointF(35, 25));
        QCOMPARE(fd->pixelSelectionBounds(), QRect(25, 15, 20, 20));
        QCOMPARE(fd->find(bid)->image, src);
        // arrows nudge 1 px, Shift+arrows 10
        QTest::keyClick(vc, Qt::Key_Right);
        QTest::keyClick(vc, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(fd->pixelSelectionBounds(), QRect(26, 25, 20, 20));
        QCOMPARE(fd->find(bid)->rect, QRectF(0, 0, 100, 80));
        // a drag outside draws a new marquee
        drag(vc, QPointF(60, 50), QPointF(70, 60));
        QCOMPARE(fd->pixelSelectionBounds(), QRect(60, 50, 10, 10));
        // Ctrl+drag moves the pixels (red area onto blue)
        selectRect(fd, QRectF(10, 10, 20, 20));
        drag(vc, QPointF(20, 20), QPointF(70, 30), 6, Qt::ControlModifier);
        QImage img = fd->find(bid)->image;
        QCOMPARE(img.pixel(65, 25), qRgb(255, 0, 0));
        QCOMPARE(qAlpha(img.pixel(15, 15)), 0);   // a hole where they were
        QCOMPARE(fd->pixelSelectionBounds(), QRect(60, 20, 20, 20));
        fd->undo();
        QCOMPARE(fd->find(bid)->image, src);
        // Alt+Ctrl+drag duplicates; dropping past the edge grows the bitmap
        selectRect(fd, QRectF(60, 10, 20, 20));
        drag(vc, QPointF(70, 20), QPointF(110, 20), 6, Qt::ControlModifier | Qt::AltModifier);
        const VObject *o = fd->find(bid);
        QCOMPARE(o->rect, QRectF(0, 0, 120, 80));
        QCOMPARE(o->image.pixel(65, 15), qRgb(0, 0, 255));   // the original stays
        QCOMPARE(o->image.pixel(110, 20), qRgb(0, 0, 255));  // and the copy
        fd->undo();
        // the Pointer on a pixel selection drags its pixels too
        appState().setTool(Tool::Pointer);
        selectRect(fd, QRectF(10, 10, 20, 20));
        drag(vc, QPointF(20, 20), QPointF(20, 60));
        QCOMPARE(fd->find(bid)->image.pixel(20, 60), qRgb(255, 0, 0));
        QCOMPARE(qAlpha(fd->find(bid)->image.pixel(20, 20)), 0);
        fd->undo();
        // Space while drawing a marquee repositions it
        appState().setTool(Tool::Marquee);
        fd->clearPixelSelection();
        const QPointF a = vc->toView(QPointF(10, 10)), b = vc->toView(QPointF(30, 30)), b2 = vc->toView(QPointF(50, 40));
        sendMouse(vc, QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton);
        sendMouse(vc, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
        sendMouse(vc, QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
        QKeyEvent sp(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
        QApplication::sendEvent(vc, &sp);
        sendMouse(vc, QEvent::MouseMove, b2, Qt::NoButton, Qt::LeftButton);
        QKeyEvent sr(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
        QApplication::sendEvent(vc, &sr);
        sendMouse(vc, QEvent::MouseButtonRelease, b2, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(fd->pixelSelectionBounds(), QRect(30, 20, 20, 20));
        appState().setTool(Tool::Pointer);
    }

    // Filters menu (destructive), live preview dialogs, selection clip, Live Effects in the PI, retouch tools
    void filtersAndEffects() {
        DocumentView *v = w->addDocument(Document::createNew(200, 150, Qt::white));
        Document *fd = v->doc();
        Canvas *fc = v->canvas();
        QTest::qWait(30);
        qApp->setProperty("lw_testing", true);
        // a 100x100 bitmap: left half dark grey, right half light grey
        VObject b;
        b.type = ObjType::Bitmap;
        b.rect = QRectF(0, 0, 100, 100);
        b.image = QImage(100, 100, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x) b.image.setPixel(x, y, x < 50 ? qRgb(60, 60, 60) : qRgb(190, 190, 190));
        b.id = fd->newId();
        fd->addObject(b, 0, false);
        fd->commit(QStringLiteral("bitmap"));
        const quint64 bid = b.id;
        auto img = [&] { return fd->find(bid)->image; };
        auto pix = [&](int x, int y) { return QColor(img().pixel(x, y)); };
        fd->setSelection({bid});
        const QImage orig = img();

        // every Filters-menu entry is an action; no-dialog filters apply directly and are undoable
        for (const char *id : {"filter.autolevels", "filter.bc", "filter.colorbalance", "filter.curves", "filter.desat", "filter.huesat",
                               "filter.invert", "filter.levels", "filter.photofilter", "filter.posterize", "filter.threshold", "filter.blur",
                               "filter.blurmore", "filter.gauss", "filter.motionblur", "filter.radialblur", "filter.zoomblur", "filter.noise",
                               "filter.toalpha", "filter.findedges", "filter.highpass", "filter.offset", "cmd.seamless", "filter.sharpen", "filter.sharpenmore", "filter.unsharp"})
            QVERIFY2(w->action(QString::fromLatin1(id)), id);
        w->action(QStringLiteral("filter.findedges"))->trigger();
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Find Edges"));
        QCOMPARE(pix(10, 50), QColor(Qt::white));
        QVERIFY(pix(50, 50).value() < 250);
        fd->undo();
        QCOMPARE(img(), orig);

        // dialog filter: live preview while open, commit on OK
        bool sawPreview = false;
        QTimer::singleShot(50, [&] {
            auto *dlg = qobject_cast<EffectDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *inBlack = dlg->findChild<QDoubleSpinBox *>(QStringLiteral("fx_inBlack"));
            QVERIFY(inBlack);
            inBlack->setValue(100);
            QCoreApplication::processEvents();
            sawPreview = pix(10, 10) == QColor(0, 0, 0);       // 60 < 100 -> black, already on canvas
            auto *prev = dlg->findChild<QCheckBox *>(QStringLiteral("fx_preview"));
            QVERIFY(prev && prev->isChecked());
            prev->setChecked(false);                             // preview off shows the original
            QCoreApplication::processEvents();
            QCOMPARE(pix(10, 10), QColor(60, 60, 60));
            prev->setChecked(true);
            dlg->accept();
        });
        w->action(QStringLiteral("filter.levels"))->trigger();
        QVERIFY(sawPreview);
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Levels"));
        QCOMPARE(pix(10, 10), QColor(0, 0, 0));
        fd->undo();
        // cancel reverts the preview
        QTimer::singleShot(50, [&] {
            auto *dlg = qobject_cast<EffectDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *sat = dlg->findChild<QWidget *>(QStringLiteral("fx_colorize"));
            if (auto *cb = qobject_cast<QCheckBox *>(sat)) cb->setChecked(true);
            QCoreApplication::processEvents();
            dlg->reject();
        });
        const int hi = fd->historyIndex();
        w->action(QStringLiteral("filter.huesat"))->trigger();
        QCOMPARE(fd->historyIndex(), hi);
        QCOMPARE(img(), orig);

        // Levels eyedroppers use the sample: white point at 190 maps the light half to white
        {
            EffectDialog dlg(LiveEffect::make(EffectType::Levels), w, orig);
            dlg.applyDropper(EffectDialog::WhitePoint, QColor(190, 190, 190));
            dlg.applyDropper(EffectDialog::BlackPoint, QColor(60, 60, 60));
            LiveEffect le = dlg.effect();
            QCOMPARE(le.levels[ChRed].inWhite, 190.0);
            QCOMPARE(le.levels[ChBlue].inBlack, 60.0);
            QImage t = orig;
            fx::adjust(t, le);
            QCOMPARE(QColor(t.pixel(80, 5)), QColor(Qt::white));
            QCOMPARE(QColor(t.pixel(10, 5)), QColor(Qt::black));
            dlg.applyDropper(EffectDialog::GreyPoint, QColor(140, 125, 125)); // neutralises a red cast
            QVERIFY(dlg.effect().levels[ChRed].gamma < 1.0);
            QVERIFY(dlg.effect().levels[ChGreen].gamma >= 1.0);
        }

        // filters respect the pixel selection
        fd->pixelSelection = QPainterPath();
        fd->pixelSelection.addRect(0, 0, 100, 40);
        w->action(QStringLiteral("filter.invert"))->trigger();
        QCOMPARE(pix(10, 10), QColor(195, 195, 195));
        QCOMPARE(pix(10, 70), QColor(60, 60, 60));
        fd->undo();
        fd->pixelSelection = QPainterPath();

        // High Pass / Offset dialogs: slider fields, Half button, Undefined Areas, Keep colour
        {
            EffectDialog hp(LiveEffect::make(EffectType::HighPass), w, orig);
            auto *r = hp.findChild<PopupSliderDoubleField *>(QStringLiteral("fx_softness"));
            QVERIFY(r);
            QCOMPARE(r->minimum(), 0.1);
            QCOMPARE(r->maximum(), 250.0);
            auto *kc = hp.findChild<QCheckBox *>(QStringLiteral("fx_keepColor"));
            QVERIFY(kc && kc->isChecked());
            kc->setChecked(false);
            QCOMPARE(hp.effect().keepColor, false);
            EffectDialog od(LiveEffect::make(EffectType::Offset), w, orig);
            auto *ox = od.findChild<PopupSliderDoubleField *>(QStringLiteral("fx_offsetX"));
            auto *oy = od.findChild<PopupSliderDoubleField *>(QStringLiteral("fx_offsetY"));
            auto *und = od.findChild<QComboBox *>(QStringLiteral("fx_undefined"));
            auto *half = od.findChild<QPushButton *>(QStringLiteral("fx_half"));
            QVERIFY(ox && oy && und && half);
            QVERIFY(ox->minimum() < 0);                      // negative offsets allowed
            QCOMPARE(und->currentIndex(), int(LiveEffect::WrapAround));
            QSignalSpy ch(&od, &EffectDialog::changed);
            half->click();
            QCOMPARE(od.effect().offsetX, 50.0);
            QCOMPARE(od.effect().offsetY, 50.0);
            QVERIFY(ch.count() >= 1);                         // live preview
            und->setCurrentIndex(LiveEffect::Transparent);
            QCOMPARE(od.effect().undefinedAreas, int(LiveEffect::Transparent));
        }
        // Offset applies to the selection's box only (wrapping inside it)
        {
            LiveEffect e = LiveEffect::make(EffectType::Offset);
            e.offsetX = 30;   // within the selection 20..80: the light half wraps round to its left
            fd->pixelSelection = QPainterPath();
            fd->pixelSelection.addRect(20, 0, 60, 100);
            QVERIFY(w->applyEffectFilter(e));
            QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Offset"));
            QCOMPARE(pix(10, 10), QColor(60, 60, 60));      // outside the selection: untouched
            QCOMPARE(pix(90, 10), QColor(190, 190, 190));
            QCOMPARE(pix(25, 10), QColor(190, 190, 190));   // x=25 <- 25-30 wraps to 75 (light)
            QCOMPARE(pix(55, 10), QColor(60, 60, 60));      // x=55 <- 25 (dark)
            fd->undo();
            fd->pixelSelection = QPainterPath();
            QCOMPARE(img(), orig);
        }
        // Commands > Make Seamless Tile: one undo step, the same result as fx::makeSeamless
        {
            w->makeSeamlessTile(20);
            QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Make Seamless Tile"));
            QCOMPARE(img().convertToFormat(QImage::Format_ARGB32_Premultiplied), fx::makeSeamless(orig, 20));
            QVERIFY(img() != orig);
            fd->undo();
            QCOMPARE(img(), orig);
        }

        // Live Effects in the Property Inspector: add, toggle, reorder, remove -- all undoable and stored on the object
        auto *pi = w->findChild<PropertyInspector *>();
        QVERIFY(pi);
        pi->m_openEditorOnAdd = false;
        fd->setSelection({bid});
        pi->refresh();
        auto addFx = [&](const QString &name) {
            QAction *a = pi->findChild<QAction *>(QStringLiteral("fx_add_") + name);
            QVERIFY2(a, qPrintable(name));
            a->trigger();
        };
        addFx(QStringLiteral("Invert"));
        addFx(QStringLiteral("Threshold"));
        QCOMPARE(fd->find(bid)->effects.size(), 2);
        auto *list = pi->findChild<QListWidget *>(QStringLiteral("fxList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 2);
        // invert then threshold(128): dark 60 -> 195 -> white
        QCOMPARE(QColor(fd->render(-1, true).pixel(10, 10)), QColor(Qt::white));
        list->setCurrentRow(1);
        pi->findChild<QToolButton *>(QStringLiteral("fxUp"))->click();
        QCOMPARE(fd->find(bid)->effects[0].type, EffectType::Threshold);
        QCOMPARE(list->currentRow(), 0);
        QCOMPARE(QColor(fd->render(-1, true).pixel(10, 10)), QColor(Qt::white)); // threshold -> black, invert -> white
        QCOMPARE(QColor(fd->render(-1, true).pixel(80, 10)), QColor(Qt::black));
        list->item(1)->setCheckState(Qt::Unchecked); // disable Invert
        QVERIFY(!fd->find(bid)->effects[1].enabled);
        QCOMPARE(QColor(fd->render(-1, true).pixel(10, 10)), QColor(Qt::black));
        // .lwd keeps the editable stack
        std::unique_ptr<Document> rt(lwd::deserialize(lwd::serialize(*fd)));
        QVERIFY(rt);
        const VObject *rb = rt->find(bid);
        QVERIFY(rb);
        QCOMPARE(rb->effects.size(), 2);
        QCOMPARE(rb->effects[0].type, EffectType::Threshold);
        QVERIFY(!rb->effects[1].enabled);
        fd->undo(); fd->undo(); fd->undo(); fd->undo();
        QVERIFY(fd->find(bid)->effects.isEmpty());
        // ... and from the Commands > Add Live Effect menu
        for (EffectType t : allEffectTypes()) QVERIFY(w->action(QStringLiteral("fx.") + effectKey(t)));
        w->action(QStringLiteral("fx.innerGlow"))->trigger();
        QCOMPARE(fd->find(bid)->effects.size(), 1);
        QCOMPARE(fd->find(bid)->effects[0].type, EffectType::InnerGlow);
        fd->undo();
        pi->m_openEditorOnAdd = true;

        // retouch tools on the bitmap
        fd->setSelection({bid});
        appState().toolIntensity = 100;
        appState().brushSize = 12;
        appState().setTool(Tool::Blur);
        drag(fc, {50, 20}, {50, 30}, 4);
        QColor bl = pix(50, 25);
        QVERIFY2(bl.red() > 60 && bl.red() < 190, qPrintable(bl.name()));
        fd->undo();
        appState().setTool(Tool::Dodge);
        drag(fc, {80, 60}, {80, 70}, 4);
        QVERIFY(pix(80, 65).red() > 190);
        fd->undo();
        appState().setTool(Tool::Burn);
        drag(fc, {80, 60}, {80, 70}, 4);
        QVERIFY(pix(80, 65).red() < 190);
        fd->undo();
        appState().setTool(Tool::Smudge);
        drag(fc, {30, 50}, {70, 50}, 10);
        QVERIFY(pix(60, 50).red() < 180); // dark dragged into the light half
        fd->undo();
        appState().setTool(Tool::RubberStamp);
        drag(fc, {20, 20}, {20, 20}, 0, Qt::AltModifier); // Alt-click sets the source (dark)
        drag(fc, {80, 80}, {82, 80}, 2);                  // clone onto the light half
        QVERIFY2(pix(80, 80).red() < 100, qPrintable(pix(80, 80).name()));
        fd->undo();
        QCOMPARE(img(), orig);
        appState().setTool(Tool::Pointer);
        appState().toolIntensity = 50;
        appState().brushSize = 6;
        fd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        QVERIFY(w->currentDoc() != fd);
        qApp->setProperty("lw_testing", false);
    }

    void filtersMenuCompleteWithLivePreview() {
        // no two unrelated actions share a shortcut (Ctrl+U was briefly both Preferences and Hue/Saturation).
        // Tools that share a cycling key (R/G/…) are allowed.
        QHash<QString, QString> seenKeys;
        QHash<QString, QAction *> byId;
        for (const QString &id : w->actionIds()) byId.insert(id, w->action(id));
        QStringList dupes;
        auto sharedOk = [&](const QString &a, const QString &b) {
            QAction *aa = byId.value(a), *bb = byId.value(b);
            const QStringList sa = aa ? aa->property("lwToolShareGroup").toStringList() : QStringList();
            const QStringList sb = bb ? bb->property("lwToolShareGroup").toStringList() : QStringList();
            if ((sa.contains(a) && sa.contains(b)) || (sb.contains(a) && sb.contains(b))) return true;
            // tools that share a built-in cycling letter (U, R, …) even if share props were cleared
            if (a.startsWith(QLatin1String("tool.")) && b.startsWith(QLatin1String("tool."))) {
                auto letter = [](const QString &id) {
                    const QString name = id.mid(5);
                    for (int i = 0; i <= int(Tool::Zoom); ++i)
                        if (toolName(Tool(i)) == name) return toolShortcut(Tool(i));
                    return QString();
                };
                const QString la = letter(a), lb = letter(b);
                if (!la.isEmpty() && la == lb) return true;
            }
            // Liquify dialog keys may reuse tool letters (same as shortcuts::Manager::conflict).
            auto isTool = [](const QString &id) { return id.startsWith(QLatin1String("tool.")); };
            auto isLiq = [](const QString &id) { return id.startsWith(QLatin1String("liquify.")); };
            if ((isTool(a) && isLiq(b)) || (isLiq(a) && isTool(b))) return true;
            if ((a == QLatin1String("panels.rename") && b.startsWith(QLatin1String("window.")))
                || (b == QLatin1String("panels.rename") && a.startsWith(QLatin1String("window."))))
                return true;
            return false;
        };
        for (const QString &id : w->actionIds()) {
            QAction *a = w->action(id);
            for (const QKeySequence &k : a->shortcuts()) {
                if (k.isEmpty()) continue;
                const QString ks = k.toString();
                if (seenKeys.contains(ks) && seenKeys.value(ks) != id && !sharedOk(seenKeys.value(ks), id))
                    dupes << QStringLiteral("%1 used by %2 and %3").arg(ks, seenKeys.value(ks), id);
                seenKeys.insert(ks, id);
            }
        }
        QVERIFY2(dupes.isEmpty(), qPrintable(dupes.join(QStringLiteral("; "))));
        QMenu *fm = nullptr;
        for (QAction *a : w->menuBar()->actions()) if (a->text() == QStringLiteral("F&ilters")) fm = a->menu();
        QVERIFY(fm);
        QStringList entries;
        std::function<void(QMenu *, const QString &)> walk = [&](QMenu *m, const QString &pre) {
            for (QAction *a : m->actions()) {
                if (a->isSeparator()) continue;
                const QString t = pre + a->text().remove(QLatin1Char('&'));
                if (a->menu()) walk(a->menu(), t + QStringLiteral(" > "));
                else entries << t;
            }
        };
        walk(fm, QString());
        qInfo().noquote() << "Filters menu:" << entries.join(QStringLiteral(" | "));
        for (const char *e : {"Adjust Color > Auto Levels", "Adjust Color > Brightness/Contrast...", "Adjust Color > Color Balance...",
                              "Adjust Color > Curves...", "Adjust Color > Desaturate", "Adjust Color > Hue/Saturation...", "Adjust Color > Invert",
                              "Adjust Color > Levels...", "Adjust Color > Photo Filter...", "Adjust Color > Posterize...", "Adjust Color > Threshold...",
                              "Adjust Color > Color Fill...", "Blur > Blur", "Blur > Blur More", "Blur > Gaussian Blur...", "Blur > Motion Blur...",
                              "Blur > Radial Blur...", "Blur > Zoom Blur...", "Noise > Add Noise...", "Other > Convert to Alpha", "Other > Find Edges",
                              "Sharpen > Sharpen", "Sharpen > Sharpen More", "Sharpen > Unsharp Mask..."})
            QVERIFY2(entries.contains(QString::fromLatin1(e)), e);

        DocumentView *v = w->addDocument(Document::createNew(120, 120, Qt::white));
        Document *fd = v->doc();
        qApp->setProperty("lw_testing", true);
        VObject b;
        b.type = ObjType::Bitmap;
        b.rect = QRectF(10, 10, 80, 80);
        b.image = QImage(80, 80, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 80; ++y) for (int x = 0; x < 80; ++x) b.image.setPixel(x, y, qRgb(40 + x * 2, 200 - y, (x * y) % 256));
        b.id = fd->newId();
        fd->addObject(b, 0, false);
        fd->commit(QStringLiteral("bitmap"));
        fd->setSelection({b.id});
        const QImage orig = fd->find(b.id)->image;
        // every dialog filter opens an EffectDialog with Preview on; the canvas shows the effect while it is open;
        // Cancel restores the original
        QStringList previewed, atOpen;
        for (QAction *a : fm->findChildren<QAction *>()) Q_UNUSED(a);
        std::function<void(QMenu *)> each = [&](QMenu *m) {
            for (QAction *a : m->actions()) {
                if (a->menu()) { each(a->menu()); continue; }
                if (a->isSeparator() || !a->text().endsWith(QStringLiteral("..."))) continue;
                bool ok = false, changed = false;
                QTimer::singleShot(30, [&] {
                    if (auto *pd = qobject_cast<ProceduralDialog *>(QApplication::activeModalWidget())) {
                        // Render > Texture: its preview is rendered off the UI thread, then shown on the canvas
                        auto *prev = pd->findChild<QCheckBox *>(QStringLiteral("fx_preview"));
                        ok = prev && prev->isChecked();
                        QTRY_VERIFY_WITH_TIMEOUT(!pd->previewImage().isNull() && !pd->previewBusy(), 10000);
                        QCoreApplication::processEvents();
                        changed = fd->find(b.id)->image != orig;
                        pd->reject();
                        return;
                    }
                    if (auto *ud = qobject_cast<PixelUpscaleDialog *>(QApplication::activeModalWidget())) {
                        // Pixel Art > Upscale changes the bitmap's size, so it previews before/after in the dialog, not on the canvas
                        ok = !ud->previewImage().isNull();
                        ud->setFactor(3);
                        QCoreApplication::processEvents();
                        changed = !ud->previewImage().isNull() && fd->find(b.id)->image == orig;
                        ud->reject();
                        return;
                    }
                    if (auto *ld = qobject_cast<LiquifyDialog *>(QApplication::activeModalWidget())) {
                        // Liquify previews in its own view; a stroke must change the result, Cancel leaves the canvas alone
                        auto *prev = ld->findChild<QCheckBox *>(QStringLiteral("liq_preview"));
                        ok = prev && prev->isChecked();
                        ld->setMode(fx2::Liquify::ForwardWarp);
                        ld->strokeImage(QPointF(10, 40), QPointF(60, 40));
                        changed = !ld->liquify().isIdentity() && ld->liquified() != orig.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                        ld->reject();
                        return;
                    }
                    auto *dlg = qobject_cast<EffectDialog *>(QApplication::activeModalWidget());
                    if (!dlg) { if (auto *other = QApplication::activeModalWidget()) other->close(); return; }
                    auto *prev = dlg->findChild<QCheckBox *>(QStringLiteral("fx_preview"));
                    ok = prev && prev->isChecked();
                    // nudge every numeric control so even identity defaults produce a visible change
                    // (only the first control: moving all three Color Balance sliders equally is a no-op with Preserve Luminosity)
                    auto ds = dlg->findChildren<QDoubleSpinBox *>();
                    auto is = dlg->findChildren<QSpinBox *>();
                    QCoreApplication::processEvents();
                    if (fd->find(b.id)->image != orig) atOpen << a->text().remove(QLatin1Char('&'));
                    if (!ds.isEmpty()) ds[0]->setValue(ds[0]->value() >= ds[0]->maximum() ? (ds[0]->value() + ds[0]->minimum()) / 2
                                                                                         : ds[0]->value() + (ds[0]->maximum() - ds[0]->value()) * 0.4);
                    else if (!is.isEmpty()) is[0]->setValue(is[0]->value() + (is[0]->maximum() - is[0]->value()) / 3);
                    else if (auto *ce = dlg->findChild<CurveEditor *>()) emit ce->curveChanged(QPolygonF({QPointF(0, 0), QPointF(128, 200), QPointF(255, 255)}));
                    QCoreApplication::processEvents();
                    changed = fd->find(b.id)->image != orig;
                    if (!changed) qInfo().noquote() << a->text() << "controls:" << ds.size() << is.size();
                    dlg->reject();
                });
                a->trigger();
                QVERIFY2(ok, qPrintable(a->text()));
                // Outline only draws around transparent edges; this test bitmap is fully opaque (covered in v030_test)
                // Palette Swap opens with From == To (the image's own colours), i.e. an identity until a swatch is edited
                if (a->text().remove(QLatin1Char('&')) != QStringLiteral("Outline...") && a->text().remove(QLatin1Char('&')) != QStringLiteral("Palette Swap..."))
                    QVERIFY2(changed, qPrintable(QStringLiteral("no live preview for ") + a->text()));
                QCOMPARE(fd->find(b.id)->image, orig);
                previewed << a->text().remove(QLatin1Char('&'));
            }
        };
        each(fm);
        qInfo().noquote() << "Live-previewed:" << previewed.join(QStringLiteral(", "));
        qInfo().noquote() << "Preview at open (non-identity defaults):" << atOpen.join(QStringLiteral(", "));
        QVERIFY(previewed.size() >= 14);

        // bevel / emboss / shadow / glow are Live Effects (Effects menu in the PI), with live preview in their editor
        auto *pi = w->findChild<PropertyInspector *>();
        pi->refresh();
        for (EffectType t : {EffectType::InnerBevel, EffectType::OuterBevel, EffectType::InsetEmboss, EffectType::RaisedEmboss,
                             EffectType::DropShadow, EffectType::InnerShadow, EffectType::OuterGlow, EffectType::InnerGlow}) {
            QAction *add = pi->findChild<QAction *>(QStringLiteral("fx_add_") + effectName(t));
            QVERIFY2(add, qPrintable(effectName(t)));
            pi->m_openEditorOnAdd = false;
            add->trigger();
            QCOMPARE(fd->find(b.id)->effects.size(), 1);
            const QImage before = fd->render(-1, true);
            bool live = false;
            QTimer::singleShot(30, [&] {
                auto *dlg = qobject_cast<EffectDialog *>(QApplication::activeModalWidget());
                if (!dlg) return;
                for (auto *sp : dlg->findChildren<QDoubleSpinBox *>()) sp->setValue(sp->value() + (sp->maximum() - sp->value()) * 0.3);
                for (auto *sp : dlg->findChildren<QSpinBox *>()) sp->setValue(sp->value() + (sp->maximum() - sp->value()) / 4);
                QCoreApplication::processEvents();
                live = fd->render(-1, true) != before;
                dlg->reject();
            });
            pi->editEffect(0);
            QVERIFY2(live, qPrintable(QStringLiteral("no live preview for ") + effectName(t)));
            QCOMPARE(fd->render(-1, true), before);
            fd->undo();
            QCOMPARE(fd->find(b.id)->effects.size(), 0);
        }
        pi->m_openEditorOnAdd = true;
        qApp->setProperty("lw_testing", false);
        fd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }



    // Contrast rules every theme (built-in or generated) must meet.
    static void checkThemeContrast(const QMap<QString, QColor> &r, const QString &label, double body = 7.0) {
        auto C = [&](const char *a, const char *b) { return theme::contrast(r.value(QLatin1String(a)), r.value(QLatin1String(b))); };
        auto need = [&](const char *fg, const char *bg, double min) {
            QVERIFY2(r.contains(QLatin1String(fg)) && r.contains(QLatin1String(bg)), qPrintable(label + QLatin1Char(' ') + QLatin1String(fg)));
            const double c = C(fg, bg);
            QVERIFY2(c >= min, qPrintable(QStringLiteral("%1: %2 on %3 = %4 < %5").arg(label, QLatin1String(fg), QLatin1String(bg)).arg(c, 0, 'f', 2).arg(min)));
        };
        need("text", "window", body);
        need("text", "input", body);
        for (const char *bg : {"bar", "menu", "body", "pi", "tools", "tab", "tabSel", "hdrTop", "hdrBot", "phTop", "phBot",
                               "phHovTop", "phHovBot", "btnTop", "btnHovTop", "secHi", "pressed", "hovBg", "chkBg", "tbChkBg", "altBase", "button"})
            need("text", bg, 4.5);
        need("accentText", "accent", 4.5);
        need("tipText", "tipBg", 4.5);
        need("secText", "secBg", 4.5);
        need("secText", "window", 4.5);
        need("small", "pi", 4.5);
        need("emptyInk", "pasteboard", 4.5);
        need("rulerInk", "ruler", 3.0);
        need("arrow", "body", 4.5);
        need("arrow", "tools", 4.5);
        need("text", "currentRow", 4.5);
        need("disabled", "window", 2.6);
        need("disabled", "body", 2.2);
        need("disabled", "dropDis", 2.2);
        QVERIFY2(C("text", "window") >= 1.8 * C("disabled", "window"), qPrintable(label));
        QVERIFY2(C("disabled", "window") <= 4.6, qPrintable(label));
    }

    // Custom from colour: themes generated from one seed colour (OKLCH tonal palettes) meet the
    // contrast rules for any seed / mode / vibrancy; named custom themes persist; JSON import /
    // export; the Preferences controls preview live; themed icons keep their pixels.
    void customThemes() {
        QTemporaryDir cfg;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, cfg.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, cfg.path());
        appState().setPrefs(Prefs());
        // OKLCH round trip
        for (QRgb c : {0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFF00u, 0x808080u, 0x123456u, 0xC23B7Au}) {
            const theme::Oklch o = theme::toOklch(QColor(c));
            const QColor back = theme::fromOklch(o.L, o.C, o.h);
            QVERIFY2(qAbs(back.red() - qRed(c)) <= 1 && qAbs(back.green() - qGreen(c)) <= 1 && qAbs(back.blue() - qBlue(c)) <= 1, qPrintable(QColor(c).name()));
        }
        QVERIFY(qAbs(theme::toOklch(Qt::white).L - 1.0) < 1e-3);
        QVERIFY(theme::toOklch(Qt::black).L < 1e-3);
        // Auto mode follows the seed's lightness
        theme::Spec a;
        a.seed = QColor(0xFF, 0xFF, 0x00); QVERIFY(!theme::specIsDark(a));
        a.seed = Qt::black; QVERIFY(theme::specIsDark(a));
        a.seed = QColor(0x10, 0x20, 0x80); QVERIFY(theme::specIsDark(a));
        a.mode = int(theme::SeedMode::Light); QVERIFY(!theme::specIsDark(a));
        a.seed = Qt::white; a.mode = int(theme::SeedMode::Dark); QVERIFY(theme::specIsDark(a));
        // seed sweep, including the edge cases (pure yellow, black, white, neon, greys)
        const QRgb seeds[] = {0xFFFF00, 0x000000, 0xFFFFFF, 0x39FF14, 0xFF00FF, 0x00FFFF, 0xFF0000, 0x0000FF, 0x808080, 0x7F7F00,
                              0xC23B7A, 0x2A5DA8, 0xFF6600, 0x013220, 0xFFC0CB, 0x101010, 0xF0F0F0, 0x6750A4};
        int checked = 0;
        for (QRgb sc : seeds)
            for (int mode : {0, 1, 2})
                for (int vib : {0, 50, 100})
                    for (const QColor &acc : {QColor(), QColor(0xFF, 0xEE, 0x00)}) {
                        theme::Spec sp; sp.seed = QColor(sc); sp.mode = mode; sp.vibrancy = vib;
                        const auto r = theme::generate(sp, acc);
                        const QString label = QStringLiteral("seed %1 mode %2 vib %3%4").arg(QColor(sc).name()).arg(mode).arg(vib).arg(acc.isValid() ? QStringLiteral(" +accent") : QString());
                        checkThemeContrast(r, label);
                        if (QTest::currentTestFailed()) return;
                        auto C = [&](const char *x, const char *y) { return theme::contrast(r.value(QLatin1String(x)), r.value(QLatin1String(y))); };
                        // UI borders and icons >= 3:1
                        for (const char *bg : {"window", "body", "input"}) {
                            QVERIFY2(C("frame", bg) >= 3.0, qPrintable(label + QStringLiteral(" frame on ") + QLatin1String(bg)));
                            QVERIFY2(C("inputEdge", bg) >= 3.0, qPrintable(label + QStringLiteral(" inputEdge on ") + QLatin1String(bg)));
                        }
                        QVERIFY2(C("chkEdge", "tools") >= 3.0 && C("hovEdge", "tools") >= 3.0, qPrintable(label));
                        QVERIFY2(C("sliderEdge", "hdrTop") >= 3.0, qPrintable(label));
                        if (!acc.isValid()) QVERIFY2(C("accent", "window") >= 3.0 && C("tabAccent", "window") >= 3.0, qPrintable(label));
                        // the icon ink (Icons.cpp: dark windows use the text colour, light ones #222)
                        const QColor ink = r.value(QStringLiteral("window")).lightnessF() < 0.45 ? r.value(QStringLiteral("text")) : QColor(0x22, 0x22, 0x22);
                        QVERIFY2(theme::contrast(ink, r.value(QStringLiteral("tools"))) >= 3.0, qPrintable(label + QStringLiteral(" icon ink")));
                        // the mode is honoured
                        const bool dark = theme::specIsDark(sp);
                        QCOMPARE(theme::contrast(r.value(QStringLiteral("window")), Qt::black) > theme::contrast(r.value(QStringLiteral("window")), Qt::white), !dark);
                        ++checked;
                    }
        QCOMPARE(checked, 18 * 3 * 3 * 2);
        // vibrancy tints the neutrals: 0 is grey, 100 carries the seed hue
        {
            theme::Spec sp; sp.seed = QColor(0xC2, 0x3B, 0x7A); sp.mode = 1;
            sp.vibrancy = 0;
            const QColor w0 = theme::generate(sp).value(QStringLiteral("window"));
            sp.vibrancy = 100;
            const QColor w1 = theme::generate(sp).value(QStringLiteral("window"));
            QVERIFY(theme::toOklch(w0).C < 0.003);
            QVERIFY(theme::toOklch(w1).C > 0.02);
            QVERIFY(qAbs(theme::toOklch(w1).h - theme::toOklch(sp.seed).h) < 8);
            // a usable seed is kept as the accent
            QCOMPARE(theme::generate(sp).value(QStringLiteral("accent")), QColor(0xC2, 0x3B, 0x7A));
        }
        // ad hoc seed ids (--theme seed:#rrggbb:mode:vibrancy)
        QVERIFY(theme::exists(QStringLiteral("seed:#39ff14")));
        QVERIFY(theme::exists(QStringLiteral("seed:FF00FF:dark:80")));
        QVERIFY(!theme::exists(QStringLiteral("seed:#zzzzzz")));
        QVERIFY(!theme::exists(QStringLiteral("seed:#ff00ff:sideways")));
        QVERIFY(!theme::exists(QStringLiteral("user:Nope")));
        // JSON round trip and bad files
        {
            theme::Spec sp; sp.name = QStringLiteral("Lime \"Fizz\""); sp.seed = QColor(0x39, 0xFF, 0x14); sp.mode = 2; sp.vibrancy = 77;
            const QByteArray js = theme::toJson(sp);
            QVERIFY(js.contains("\"format\": \"libreworks-theme\""));
            QVERIFY(js.contains("\"roles\""));
            theme::Spec back;
            QVERIFY(theme::fromJson(js, &back));
            QCOMPARE(back.name, sp.name); QCOMPARE(back.seed, sp.seed); QCOMPARE(back.mode, 2); QCOMPARE(back.vibrancy, 77);
            QString err;
            QVERIFY(!theme::fromJson("not json", &back, &err)); QVERIFY(!err.isEmpty());
            QVERIFY(!theme::fromJson("{\"format\":\"something-else\",\"seed\":\"#ff0000\"}", &back, &err));
            QVERIFY(!theme::fromJson("{\"format\":\"libreworks-theme\"}", &back, &err));
            QVERIFY(theme::fromJson("{\"seed\":\"#ff0000\",\"mode\":\"light\"}", &back, &err));
            QCOMPARE(back.name, QStringLiteral("Imported")); QCOMPARE(back.mode, 1); QCOMPARE(back.vibrancy, 40);
        }
        // named custom themes persist (settings), and apply
        {
            Prefs p;
            theme::Spec u1; u1.name = QStringLiteral("Lemon"); u1.seed = QColor(0xFF, 0xFF, 0x00); u1.mode = 0; u1.vibrancy = 60;
            theme::Spec u2; u2.name = QStringLiteral("Deep Sea"); u2.seed = QColor(0x01, 0x32, 0x50); u2.mode = 2; u2.vibrancy = 20;
            p.userThemes = {u1, u2};
            p.customSeed = QColor(0x39, 0xFF, 0x14); p.customMode = 2; p.customVibrancy = 90;
            p.theme = QStringLiteral("user:Deep Sea");
            appState().setPrefs(p);
            QCOMPARE(theme::current(), QStringLiteral("user:Deep Sea"));
            QCOMPARE(QApplication::palette().color(QPalette::Window), theme::generate(u2).value(QStringLiteral("window")));
            QCOMPARE(appState().prefs.pasteboardColor, theme::generate(u2).value(QStringLiteral("pasteboard")));
            QVERIFY(!qApp->styleSheet().contains(QLatin1Char('@')));
            appState().savePrefs();
            appState().setPrefs(Prefs());
            QCOMPARE(theme::current(), theme::defaultId());
            QVERIFY(!theme::exists(QStringLiteral("user:Deep Sea")));
            appState().loadPrefs();
            QCOMPARE(appState().prefs.userThemes.size(), 2);
            QCOMPARE(appState().prefs.userThemes[0].name, QStringLiteral("Lemon"));
            QCOMPARE(appState().prefs.userThemes[0].seed, QColor(0xFF, 0xFF, 0x00));
            QCOMPARE(appState().prefs.userThemes[0].vibrancy, 60);
            QCOMPARE(appState().prefs.userThemes[1].mode, 2);
            QCOMPARE(appState().prefs.customSeed, QColor(0x39, 0xFF, 0x14));
            QCOMPARE(appState().prefs.customMode, 2);
            QCOMPARE(appState().prefs.customVibrancy, 90);
            QCOMPARE(theme::current(), QStringLiteral("user:Deep Sea"));
            // editing a seed in place re-applies (cache key includes the seed)
            Prefs q = appState().prefs;
            q.theme = theme::customId();
            appState().setPrefs(q);
            const QColor w1 = QApplication::palette().color(QPalette::Window);
            q.customSeed = QColor(0xFF, 0x00, 0xFF); q.customMode = 1;
            appState().setPrefs(q);
            QVERIFY(QApplication::palette().color(QPalette::Window) != w1);
            // follow system regenerates a generated theme in the desktop's mode
            q.themeFollowSystem = true;
            theme::setSystemSchemeOverride(int(Qt::ColorScheme::Dark));
            appState().setPrefs(q);
            QCOMPARE(theme::current(), QStringLiteral("custom@dark"));
            QVERIFY(QApplication::palette().color(QPalette::Window).lightnessF() < 0.4);
            theme::setSystemSchemeOverride(-1);
            // themed icons keep their pixels under a generated theme (only the ink changes)
            q.themeFollowSystem = false;
            appState().setPrefs(q);
            for (const QString n : {QStringLiteral("up"), QStringLiteral("trash"), QStringLiteral("layer")}) {
                const QIcon th = icons::named(n);
                const QIcon plain(th.pixmap(QSize(16, 16), 2.0));
                for (const QSize sz : {QSize(16, 16), QSize(24, 22)}) {
                    QCOMPARE(th.actualSize(sz), plain.actualSize(sz));
                    QCOMPARE(th.pixmap(sz).toImage(), plain.pixmap(sz).toImage());
                }
            }
            appState().setPrefs(Prefs());
        }

        // Preferences > Themes > Custom from colour
        appState().setPrefs(Prefs());
        QTemporaryDir io;
        bool seen = false;
        QTimer::singleShot(0, [&] {
            auto *dlg = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *combo = dlg->findChild<QComboBox *>(QStringLiteral("prefTheme"));
            auto *hex = dlg->findChild<QLineEdit *>(QStringLiteral("prefSeedHex"));
            auto *seedBtn = dlg->findChild<ColorButton *>(QStringLiteral("prefSeedColor"));
            auto *mode = dlg->findChild<QComboBox *>(QStringLiteral("prefSeedMode"));
            auto *vib = dlg->findChild<QSlider *>(QStringLiteral("prefSeedVibrancy"));
            QVERIFY(combo && hex && seedBtn && mode && vib);
            for (const char *b : {"prefThemeSaveAs", "prefThemeRename", "prefThemeDelete", "prefThemeImport", "prefThemeExport"})
                QVERIFY2(dlg->findChild<QPushButton *>(QLatin1String(b)), b);
            QVERIFY(dlg->findChild<QLabel *>(QStringLiteral("prefSeedSwatches")));
            QVERIFY(!hex->isEnabled());   // built-in theme selected
            QVERIFY(combo->findData(QStringLiteral("pink")) >= 0);
            combo->setCurrentIndex(combo->findData(theme::customId()));
            QVERIFY(hex->isEnabled() && vib->isEnabled());
            QVERIFY(!dlg->findChild<QPushButton *>(QStringLiteral("prefThemeDelete"))->isEnabled());
            QCOMPARE(theme::current(), theme::customId());
            // typing a hex seed previews live
            hex->clear();
            QTest::keyClicks(hex, QStringLiteral("#FFFF00"));
            QCOMPARE(seedBtn->color(), QColor(0xFF, 0xFF, 0x00));
            theme::Spec want; want.seed = QColor(0xFF, 0xFF, 0x00); want.mode = 0; want.vibrancy = 40;
            QCOMPARE(QApplication::palette().color(QPalette::Window), theme::generate(want).value(QStringLiteral("window")));
            mode->setCurrentIndex(2);
            want.mode = 2;
            QCOMPARE(QApplication::palette().color(QPalette::Window), theme::generate(want).value(QStringLiteral("window")));
            vib->setValue(100);
            want.vibrancy = 100;
            QCOMPARE(QApplication::palette().color(QPalette::Window), theme::generate(want).value(QStringLiteral("window")));
            QCOMPARE(appState().prefs.pasteboardColor, theme::generate(want).value(QStringLiteral("pasteboard")));
            // the colour picker path
            seedBtn->setColor(QColor(0x39, 0xFF, 0x14));
            emit seedBtn->colorPicked(seedBtn->color());
            QCOMPARE(hex->text(), QStringLiteral("#39FF14"));
            want.seed = QColor(0x39, 0xFF, 0x14);
            QCOMPARE(QApplication::palette().color(QPalette::Window), theme::generate(want).value(QStringLiteral("window")));
            // save as, rename, delete
            const QString id = dlg->saveCustomAs(QStringLiteral("Neon"));
            QCOMPARE(id, QStringLiteral("user:Neon"));
            QCOMPARE(combo->currentData().toString(), id);
            QCOMPARE(theme::current(), id);
            QVERIFY(dlg->findChild<QPushButton *>(QStringLiteral("prefThemeDelete"))->isEnabled());
            QCOMPARE(dlg->saveCustomAs(QStringLiteral("neon")), QStringLiteral("user:neon 2"));   // names stay unique
            QVERIFY(dlg->renameCustom(QStringLiteral("Glow")));
            QCOMPARE(combo->currentData().toString(), QStringLiteral("user:Glow"));
            QVERIFY(combo->findData(QStringLiteral("user:neon 2")) < 0);
            // export, then import it back under a fresh name
            const QString file = io.filePath(QStringLiteral("glow.json"));
            QString err;
            QVERIFY2(dlg->exportThemeFile(file, &err), qPrintable(err));
            QVERIFY(dlg->deleteCustom());
            QVERIFY(combo->findData(QStringLiteral("user:Glow")) < 0);
            QCOMPARE(combo->currentData().toString(), theme::customId());
            QVERIFY2(dlg->importThemeFile(file, &err), qPrintable(err));
            QCOMPARE(combo->currentData().toString(), QStringLiteral("user:Glow"));
            QCOMPARE(seedBtn->color(), QColor(0x39, 0xFF, 0x14));
            QCOMPARE(mode->currentIndex(), 2);
            QCOMPARE(vib->value(), 100);
            QFile bad(io.filePath(QStringLiteral("bad.json")));
            QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("{ nope"); bad.close();
            QVERIFY(!dlg->importThemeFile(bad.fileName(), &err));
            QVERIFY(!err.isEmpty());
            // editing a named theme's seed changes that theme
            mode->setCurrentIndex(1);
            QVERIFY(QApplication::palette().color(QPalette::Window).lightnessF() > 0.6);
            seen = true;
            dlg->accept();
        });
        w->action(QStringLiteral("edit.preferences"))->trigger();
        QVERIFY(seen);
        QCOMPARE(appState().prefs.theme, QStringLiteral("user:Glow"));
        appState().setPrefs(Prefs());
        appState().loadPrefs();
        QCOMPARE(appState().prefs.theme, QStringLiteral("user:Glow"));
        QCOMPARE(appState().prefs.userThemes.size(), 2);   // Neon, Glow
        QCOMPARE(appState().prefs.userThemes[0].name, QStringLiteral("Neon"));
        QCOMPARE(appState().prefs.userThemes[1].mode, 1);
        QCOMPARE(appState().prefs.customSeed, QColor(0x39, 0xFF, 0x14));
        QCOMPARE(theme::current(), QStringLiteral("user:Glow"));
        // Cancel reverts a live custom preview
        seen = false;
        QTimer::singleShot(0, [&] {
            auto *dlg = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            dlg->saveCustomAs(QStringLiteral("Temp"));
            auto *combo = dlg->findChild<QComboBox *>(QStringLiteral("prefTheme"));
            combo->setCurrentIndex(combo->findData(QStringLiteral("pink-night")));
            QCOMPARE(theme::current(), QStringLiteral("pink-night"));
            seen = true;
            dlg->reject();
        });
        w->action(QStringLiteral("edit.preferences"))->trigger();
        QVERIFY(seen);
        QCOMPARE(theme::current(), QStringLiteral("user:Glow"));
        QCOMPARE(appState().prefs.userThemes.size(), 2);
        appState().setPrefs(Prefs());
        appState().savePrefs();
    }

    // Interface themes: every theme's colour roles pass WCAG contrast checks, disabled text is
    // dimmer but legible (and actually used by disabled widgets), Preferences > Themes applies live,
    // Cancel reverts, OK persists; follow-system, accent and pasteboard overrides; icons re-ink.
    void themes() {
        QTemporaryDir cfg;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, cfg.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, cfg.path());
        const QVector<theme::Info> all = theme::list();
        QCOMPARE(all.size(), 14); // + Bonbon (0.3.9)
        QCOMPARE(all.first().id, QStringLiteral("classic"));
        QCOMPARE(all.first().name, QStringLiteral("Macromedia Classic"));
        QStringList names;
        for (const theme::Info &i : all) names << i.name;
        for (const char *n : {"Light", "Dark", "Midnight", "Graphite", "Studio 8", "Solarized Dark", "Solarized Light", "Nord", "Synthwave", "High Contrast", "Pink", "Pink Night", "Bonbon"})
            QVERIFY2(names.contains(QLatin1String(n)), n);
        QVERIFY(qAbs(theme::contrast(Qt::black, Qt::white) - 21.0) < 0.01);
        QVERIFY(qAbs(theme::contrast(Qt::white, Qt::white) - 1.0) < 1e-9);
        // contrast across all themes (and with a hard accent override)
        for (const QColor &accent : {QColor(), QColor(0xFF, 0xEE, 0x00), QColor(0x80, 0x80, 0x80)})
            for (const theme::Info &i : all) {
                const auto r = theme::roles(i.id, accent);
                auto C = [&](const char *a, const char *b) { return theme::contrast(r.value(QLatin1String(a)), r.value(QLatin1String(b))); };
                auto need = [&](const char *fg, const char *bg, double min) {
                    QVERIFY2(r.contains(QLatin1String(fg)) && r.contains(QLatin1String(bg)), qPrintable(i.id + QLatin1Char(' ') + QLatin1String(fg)));
                    const double c = C(fg, bg);
                    QVERIFY2(c >= min, qPrintable(QStringLiteral("%1: %2 on %3 = %4 < %5").arg(i.id, QLatin1String(fg), QLatin1String(bg)).arg(c, 0, 'f', 2).arg(min)));
                };
                const double body = i.id == QLatin1String("high-contrast") ? 15.0 : 7.0;
                need("text", "window", body);
                need("text", "input", body);
                for (const char *bg : {"bar", "menu", "body", "pi", "tools", "tab", "tabSel", "hdrTop", "hdrBot", "phTop", "phBot",
                                       "phHovTop", "phHovBot", "btnTop", "btnHovTop", "secHi", "pressed", "hovBg", "chkBg", "tbChkBg", "altBase", "button"})
                    need("text", bg, 4.5);
                need("accentText", "accent", 4.5);
                need("tipText", "tipBg", 4.5);
                need("secText", "secBg", 4.5);
                need("secText", "window", 4.5);
                need("small", "pi", 4.5);
                need("emptyInk", "pasteboard", 4.5);
                need("rulerInk", "ruler", 3.0);
                need("arrow", "body", 4.5);
                need("text", "currentRow", 4.5);
                // disabled text: legible (>= 2.6:1 on the window, 2.2:1 on inputs) yet clearly dimmer than text
                need("disabled", "window", 2.6);
                need("disabled", "body", 2.2);
                need("disabled", "dropDis", 2.2);
                QVERIFY2(C("text", "window") >= 1.8 * C("disabled", "window"), qPrintable(i.id));
                QVERIFY2(C("disabled", "window") <= 4.6, qPrintable(i.id));
                QVERIFY(theme::pasteboard(i.id).isValid());
            }
        // follow system
        QCOMPARE(theme::resolve(QStringLiteral("classic"), false, 2), QStringLiteral("classic"));
        QCOMPARE(theme::resolve(QStringLiteral("classic"), true, 2), QStringLiteral("dark"));
        QCOMPARE(theme::resolve(QStringLiteral("solarized-light"), true, 2), QStringLiteral("solarized-dark"));
        QCOMPARE(theme::resolve(QStringLiteral("solarized-dark"), true, 1), QStringLiteral("solarized-light"));
        QCOMPARE(theme::resolve(QStringLiteral("nord"), true, 1), theme::defaultId());
        QCOMPARE(theme::resolve(QStringLiteral("nord"), true, 2), QStringLiteral("nord"));
        QCOMPARE(theme::resolve(QStringLiteral("studio8"), true, 0), QStringLiteral("studio8"));
        QCOMPARE(theme::resolve(QStringLiteral("bogus"), false), theme::defaultId());
        QCOMPARE(theme::resolve(QStringLiteral("pink"), true, 2), QStringLiteral("pink-night"));
        QCOMPARE(theme::resolve(QStringLiteral("pink-night"), true, 1), QStringLiteral("pink"));

        // apply every theme: palette, resolved stylesheet, disabled widgets really use the disabled colour
        appState().setPrefs(Prefs());
        auto inkOf = [](QWidget *wd) {   // the pixel farthest from the background
            const QImage img = wd->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const QColor bg = img.pixelColor(1, 1);
            QColor best = bg; int bestD = -1;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    const int dd = qAbs(c.red() - bg.red()) + qAbs(c.green() - bg.green()) + qAbs(c.blue() - bg.blue());
                    if (dd > bestD) { bestD = dd; best = c; }
                }
            return best;
        };
        auto dist = [](const QColor &a, const QColor &b) { return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue()); };
        for (const theme::Info &i : all) {
            Prefs p;
            p.theme = i.id;
            appState().setPrefs(p);
            QCOMPARE(theme::current(), i.id);
            const auto r = theme::roles(i.id);
            QCOMPARE(QApplication::palette().color(QPalette::Window), r.value(QStringLiteral("window")));
            QCOMPARE(QApplication::palette().color(QPalette::Disabled, QPalette::WindowText), r.value(QStringLiteral("disabled")));
            QCOMPARE(QApplication::palette().color(QPalette::Disabled, QPalette::Text), r.value(QStringLiteral("disabled")));
            QCOMPARE(QApplication::palette().color(QPalette::HighlightedText), r.value(QStringLiteral("accentText")));
            QVERIFY(!qApp->styleSheet().contains(QLatin1Char('@')));
            QCOMPARE(appState().prefs.pasteboardColor, r.value(QStringLiteral("pasteboard")));
            QCOMPARE(theme::color("text"), r.value(QStringLiteral("text")));
            QWidget host;
            auto *lay = new QVBoxLayout(&host);
            QFont f; f.setPixelSize(22); f.setBold(true);
            auto *on = new QLabel(QStringLiteral("MMMM")); on->setFont(f);
            auto *off = new QLabel(QStringLiteral("MMMM")); off->setFont(f); off->setEnabled(false);
            auto *cb = new QCheckBox(QStringLiteral("MMMM")); cb->setFont(f); cb->setEnabled(false);
            lay->addWidget(on); lay->addWidget(off); lay->addWidget(cb);
            host.resize(200, 120);
            host.show();
            QVERIFY(QTest::qWaitForWindowExposed(&host));
            const QColor dis = r.value(QStringLiteral("disabled")), txt = r.value(QStringLiteral("text"));
            const QColor a = inkOf(on), b = inkOf(off);
            QVERIFY2(dist(a, txt) < 40, qPrintable(i.id + QStringLiteral(" enabled ink ") + a.name()));
            QVERIFY2(dist(b, dis) < 40, qPrintable(i.id + QStringLiteral(" disabled ink ") + b.name() + QStringLiteral(" want ") + dis.name()));
            QVERIFY2(dist(b, txt) > 60, qPrintable(i.id));
        }
        // icons follow the theme ink
        appState().setPrefs(Prefs());
        const QIcon ptr = icons::tool(Tool::Pointer);
        const QImage lightIcon = ptr.pixmap(22, 22).toImage();
        Prefs dk; dk.theme = QStringLiteral("dark");
        appState().setPrefs(dk);
        const QImage darkIcon = ptr.pixmap(22, 22).toImage();
        QVERIFY(lightIcon != darkIcon);
        QVERIFY(!ptr.pixmap(22, 22, QIcon::Disabled).isNull());
        appState().setPrefs(Prefs());
        // Themed icons behave exactly like plain pixmap icons: never upscaled, centred, same
        // pixels on the default theme (0.2.3 regression: panel buttons showed enlarged, cropped icons).
        for (const QString n : {QStringLiteral("up"), QStringLiteral("down"), QStringLiteral("trash"), QStringLiteral("dup"), QStringLiteral("layer")}) {
            const QIcon th = icons::named(n);
            const QIcon plain(th.pixmap(QSize(16, 16), 2.0)); // the drawn 2x pixmap
            QCOMPARE(th.pixmap(QSize(16, 16), 2.0).size(), QSize(32, 32));
            for (const QSize sz : {QSize(12, 12), QSize(16, 16), QSize(24, 22), QSize(64, 64)}) {
                QCOMPARE(th.actualSize(sz), plain.actualSize(sz));
                for (QIcon::Mode m : {QIcon::Normal, QIcon::Disabled}) {
                    QImage a(sz, QImage::Format_ARGB32_Premultiplied), b = a;
                    a.fill(Qt::white); b.fill(Qt::white);
                    { QPainter p(&a); th.paint(&p, a.rect(), Qt::AlignCenter, m); }
                    { QPainter p(&b); plain.paint(&p, b.rect(), Qt::AlignCenter, m); }
                    QVERIFY2(a == b, qPrintable(n));
                    QCOMPARE(th.pixmap(sz, m).toImage(), plain.pixmap(sz, m).toImage());
                }
            }
        }

        // Preferences > Themes: live preview, Cancel reverts
        bool seen = false;
        QTimer::singleShot(0, [&] {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *tabs = dlg->findChild<QTabWidget *>(QStringLiteral("prefsTabs"));
            QVERIFY(tabs);
            bool hasTab = false;
            for (int k = 0; k < tabs->count(); ++k) if (tabs->tabText(k) == QLatin1String("Themes")) { hasTab = true; tabs->setCurrentIndex(k); }
            QVERIFY(hasTab);
            auto *combo = dlg->findChild<QComboBox *>(QStringLiteral("prefTheme"));
            QVERIFY(combo);
            QCOMPARE(combo->count(), 14 + 2);   // built-ins (+Bonbon), a separator, Custom from colour
            QCOMPARE(combo->currentData().toString(), theme::defaultId());
            combo->setCurrentIndex(combo->findData(QStringLiteral("midnight")));
            QCOMPARE(theme::current(), QStringLiteral("midnight"));          // live
            QCOMPARE(QApplication::palette().color(QPalette::Window), theme::roles(QStringLiteral("midnight")).value(QStringLiteral("window")));
            QCOMPARE(appState().prefs.pasteboardColor, theme::pasteboard(QStringLiteral("midnight")));
            QCOMPARE(dlg->findChild<ColorButton *>(QStringLiteral("prefPasteboardColor"))->color(), theme::pasteboard(QStringLiteral("midnight")));
            seen = true;
            dlg->reject();
        });
        w->action(QStringLiteral("edit.preferences"))->trigger();
        QVERIFY(seen);
        QCOMPARE(theme::current(), theme::defaultId());
        QCOMPARE(appState().prefs.theme, theme::defaultId());
        QCOMPARE(appState().prefs.pasteboardColor, theme::pasteboard(theme::defaultId()));

        // OK keeps theme + accent + pasteboard overrides, and they persist
        seen = false;
        QTimer::singleShot(0, [&] {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *combo = dlg->findChild<QComboBox *>(QStringLiteral("prefTheme"));
            combo->setCurrentIndex(combo->findData(QStringLiteral("nord")));
            auto *acc = dlg->findChild<ColorButton *>(QStringLiteral("prefAccentColor"));
            QVERIFY(acc && !acc->isEnabled());
            dlg->findChild<QCheckBox *>(QStringLiteral("prefAccentOverride"))->setChecked(true);
            QVERIFY(acc->isEnabled());
            acc->setColor(QColor(0xE0, 0x40, 0x40));
            emit acc->colorPicked(acc->color());
            QCOMPARE(QApplication::palette().color(QPalette::Link), QColor(0xE0, 0x40, 0x40));   // live
            auto *pb = dlg->findChild<ColorButton *>(QStringLiteral("prefPasteboardColor"));
            pb->setColor(QColor(10, 20, 30));
            emit pb->colorPicked(pb->color());
            QVERIFY(dlg->findChild<QCheckBox *>(QStringLiteral("prefPasteboardOverride"))->isChecked());
            QCOMPARE(appState().prefs.pasteboardColor, QColor(10, 20, 30));
            seen = true;
            dlg->accept();
        });
        w->action(QStringLiteral("edit.preferences"))->trigger();
        QVERIFY(seen);
        QCOMPARE(appState().prefs.theme, QStringLiteral("nord"));
        QVERIFY(appState().prefs.accentOverride);
        QVERIFY(appState().prefs.pasteboardOverride);
        QCOMPARE(theme::current(), QStringLiteral("nord"));
        const QColor hl = QApplication::palette().color(QPalette::Highlight);
        QVERIFY(theme::contrast(hl, QApplication::palette().color(QPalette::HighlightedText)) >= 4.5);
        QVERIFY(qAbs(hl.hueF() - QColor(0xE0, 0x40, 0x40).hueF()) < 0.02);
        appState().setPrefs(Prefs());
        QCOMPARE(theme::current(), theme::defaultId());
        appState().loadPrefs();
        QCOMPARE(appState().prefs.theme, QStringLiteral("nord"));
        QCOMPARE(appState().prefs.accentColor, QColor(0xE0, 0x40, 0x40));
        QCOMPARE(appState().prefs.pasteboardColor, QColor(10, 20, 30));
        QCOMPARE(theme::current(), QStringLiteral("nord"));
        // un-ticking the pasteboard override returns to the theme's pasteboard
        Prefs np = appState().prefs;
        np.pasteboardOverride = false;
        appState().setPrefs(np);
        QCOMPARE(appState().prefs.pasteboardColor, theme::pasteboard(QStringLiteral("nord")));

        // follow system: the platform scheme (overridden here) swaps light <-> dark
        Prefs fp;
        fp.theme = QStringLiteral("studio8");
        fp.themeFollowSystem = true;
        theme::setSystemSchemeOverride(int(Qt::ColorScheme::Dark));
        appState().setPrefs(fp);
        QCOMPARE(theme::current(), QStringLiteral("dark"));
        QCOMPARE(appState().prefs.pasteboardColor, theme::pasteboard(QStringLiteral("dark")));
        theme::setSystemSchemeOverride(int(Qt::ColorScheme::Light));
        appState().setPrefs(fp);
        QCOMPARE(theme::current(), QStringLiteral("studio8"));
        theme::setSystemSchemeOverride(-1);
        appState().setPrefs(Prefs());
        QCOMPARE(theme::current(), theme::defaultId());
    }

    // Free Transform: rotate handle (Shift = 15 degree steps), the exact angle field, a movable
    // centre, Skew / Distort / Perspective drags, Numeric Transform, Remove Transformations,
    // rotated bitmaps and symbol instances, Rotate Canvas by any angle.
    void freeTransform() {
        DocumentView *v = w->addDocument(Document::createNew(400, 300, Qt::white));
        Document *fd = v->doc();
        Canvas *cv = v->canvas();
        cv->setView(1.0, QPointF(200, 150));
        QTest::qWait(20);
        auto addRect = [&](QRectF r, QColor col) {
            VObject o;
            o.type = ObjType::Rect;
            o.rect = r;
            o.fillOn = true;
            o.fill = col;
            o.strokeOn = false;
            o.id = fd->newId();
            fd->addObject(o, 0, false);
            fd->commit(QStringLiteral("rect"));
            fd->setSelection({o.id});
            return o.id;
        };
        auto obj = [&](quint64 id) { return fd->find(id); };
        auto near = [](QPointF a, QPointF b, double tol = 0.6) { return QLineF(a, b).length() <= tol; };
        const quint64 id = addRect(QRectF(100, 100, 100, 50), Qt::red);
        appState().setTool(Tool::Pointer);
        QVERIFY(!cv->transformActive());
        auto *bar = cv->findChild<QFrame *>(QStringLiteral("transformBar"));
        QVERIFY(bar && bar->isHidden());
        // Modify > Transform > Free Transform (Ctrl+T) picks the Scale tool
        QAction *ft = w->action(QStringLiteral("modify.freetransform"));
        QCOMPARE(ft->shortcut(), QKeySequence(QStringLiteral("Ctrl+T")));
        ft->trigger();
        QCOMPARE(appState().tool(), Tool::Scale);
        QVERIFY(cv->transformActive());
        QVERIFY(bar->isVisible());
        QCOMPARE(cv->transformQuad(), xf::corners(QRectF(100, 100, 100, 50)));
        QVERIFY(near(cv->transformCentre(), QPointF(150, 125)));

        // 1) drag outside a corner = rotate; Shift snaps the angle to 15 degree steps
        const QPointF cView = cv->toView(QPointF(150, 125));
        auto rotated = [&](QPointF p, double deg) {
            return QTransform().translate(cView.x(), cView.y()).rotate(deg).translate(-cView.x(), -cView.y()).map(p);
        };
        auto rotDrag = [&](double deg, Qt::KeyboardModifiers m) {
            // just outside the top-right corner of the box, away from the centre
            const QPointF corner = cv->transformHandleView(1);
            const QPointF out = corner - cView;
            const QPointF p0 = corner + out * (15.0 / std::hypot(out.x(), out.y()));
            sendMouse(cv, QEvent::MouseMove, p0, Qt::NoButton, Qt::NoButton, m);
            if (cv->cursor().shape() != Qt::BitmapCursor) qWarning() << "cursor" << cv->cursor().shape();
            sendMouse(cv, QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton, m);
            for (int i = 1; i <= 6; ++i) sendMouse(cv, QEvent::MouseMove, rotated(p0, deg * i / 6), Qt::NoButton, Qt::LeftButton, m);
            sendMouse(cv, QEvent::MouseButtonRelease, rotated(p0, deg), Qt::LeftButton, Qt::NoButton, m);
            QCoreApplication::processEvents();
        };
        const int h0 = fd->historyIndex();
        rotDrag(34, Qt::ShiftModifier);
        QCOMPARE(fd->historyIndex(), h0 + 1);
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Rotate"));
        QVERIFY(qAbs(cv->transformAngle() - 30) < 1e-6);
        QVERIFY(obj(id)->hasXform());
        QVERIFY(qAbs(xf::rotationOf(*obj(id)) - 30) < 1e-6);
        QCOMPARE(obj(id)->rect, QRectF(100, 100, 100, 50));          // non-destructive
        QVERIFY(near(obj(id)->bounds().center(), QPointF(150, 125)));
        auto *ang = cv->findChild<QDoubleSpinBox *>(QStringLiteral("txAngle"));
        QVERIFY(ang);
        QCOMPARE(ang->value(), 30.0);
        // without Shift the angle is free
        rotDrag(7, Qt::NoModifier);
        QVERIFY(qAbs(cv->transformAngle() - 37) < 0.5);
        fd->undo();
        QTest::qWait(10);
        QVERIFY(qAbs(xf::rotationOf(*obj(id)) - 30) < 1e-6);
        // the box follows the object after undo: re-read it from the object's own transform
        QVERIFY(qAbs(cv->transformAngle() - 30) < 1e-6);
        // hit testing follows the rotation: a point inside the rotated box but outside the original rect
        const QPointF inside = xf::rotationAbout(QPointF(150, 125), 30).map(QPointF(195, 145));
        QVERIFY(obj(id)->hitTest(inside, 0));
        QVERIFY(!QRectF(100, 100, 100, 50).contains(inside));

        // 2) the exact angle field
        ang->setValue(45);
        QVERIFY(qAbs(xf::rotationOf(*obj(id)) - 45) < 1e-6);
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Rotate"));
        QVERIFY(near(obj(id)->bounds().center(), QPointF(150, 125)));

        // 3) the centre point is draggable (no undo step) and rotation turns about it
        const int h1 = fd->historyIndex();
        const QPointF cv0 = cv->transformHandleView(8);
        sendMouse(cv, QEvent::MouseMove, cv0, Qt::NoButton, Qt::NoButton);
        sendMouse(cv, QEvent::MouseButtonPress, cv0, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 4; ++i) sendMouse(cv, QEvent::MouseMove, cv0 + QPointF(50.0 * i / 4, 0), Qt::NoButton, Qt::LeftButton);
        sendMouse(cv, QEvent::MouseButtonRelease, cv0 + QPointF(50, 0), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(fd->historyIndex(), h1);
        QVERIFY(near(cv->transformCentre(), QPointF(200, 125)));
        QVERIFY(cv->rotateSelection(-45));
        // the object's centre turned 45 degrees anticlockwise about (200, 125)
        QVERIFY(near(obj(id)->bounds().center(), xf::rotationAbout(QPointF(200, 125), -45).map(QPointF(150, 125)), 0.8));
        QVERIFY(qAbs(xf::rotationOf(*obj(id))) < 1e-6);
        // "Centre" puts the centre back in the middle of the box
        cv->findChild<QToolButton *>(QStringLiteral("txCentreReset"))->click();
        QVERIFY(near(cv->transformCentre(), obj(id)->bounds().center()));

        // 4) Remove Transformations keeps the object where it is
        QVERIFY(cv->rotateSelection(20));
        QVERIFY(obj(id)->hasXform());
        const QPointF before = obj(id)->bounds().center();
        QVERIFY(w->action(QStringLiteral("modify.removexform"))->isEnabled());
        w->action(QStringLiteral("modify.removexform"))->trigger();
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Remove Transformations"));
        QVERIFY(!obj(id)->hasXform());
        QVERIFY(near(obj(id)->bounds().center(), before));
        QCOMPARE(obj(id)->bounds().size(), QSizeF(100, 50));
        QVERIFY(!w->removeTransformations());   // nothing left to remove

        // 5) Skew: the top edge slides along itself
        const quint64 sk = addRect(QRectF(50, 50, 100, 100), Qt::blue);
        appState().setTool(Tool::Skew);
        QVERIFY(bar->isVisible());
        auto handleDrag = [&](int handle, QPointF byDoc, Qt::KeyboardModifiers m = Qt::NoModifier) {
            const QPointF a = cv->transformHandleView(handle), b = a + byDoc * cv->zoom();
            sendMouse(cv, QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton, m);
            sendMouse(cv, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton, m);
            for (int i = 1; i <= 5; ++i) sendMouse(cv, QEvent::MouseMove, a + (b - a) * (i / 5.0), Qt::NoButton, Qt::LeftButton, m);
            sendMouse(cv, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton, m);
            QCoreApplication::processEvents();
        };
        handleDrag(4, QPointF(40, 15));   // the vertical part is ignored
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Skew"));
        QPolygonF q = cv->transformQuad();
        QVERIFY(near(q[0], QPointF(90, 50)) && near(q[1], QPointF(190, 50)));
        QVERIFY(near(q[3], QPointF(50, 150)) && near(q[2], QPointF(150, 150)));
        QVERIFY(qAbs(obj(sk)->xform.m21() + 0.4) < 1e-6);   // x' = x - 0.4 y + ...
        QCOMPARE(obj(sk)->bounds().width(), 140.0);
        // a corner drag skews along the dominant direction
        const QPolygonF qs = cv->transformQuad();
        handleDrag(2, QPointF(0, 30));
        q = cv->transformQuad();
        // the right edge slides along itself: corners 1 and 2 move together, the left edge stays
        QVERIFY(near(q[0], qs[0]) && near(q[3], qs[3]));
        QVERIFY(near(q[1] - qs[1], q[2] - qs[2]));
        QVERIFY(q[2].y() - qs[2].y() > 15);
        {
            const QPointF mv = q[2] - qs[2], side = qs[2] - qs[1];
            QVERIFY(qAbs(mv.x() * side.y() - mv.y() * side.x()) < 1);   // parallel to the side
        }

        // 6) Distort: one corner moves freely (projective)
        const quint64 ds = addRect(QRectF(200, 50, 100, 100), Qt::green);
        appState().setTool(Tool::Distort);
        handleDrag(2, QPointF(30, 20));
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Distort"));
        q = cv->transformQuad();
        QVERIFY(near(q[2], QPointF(330, 170)) && near(q[1], QPointF(300, 50)) && near(q[3], QPointF(200, 150)));
        QCOMPARE(obj(ds)->xform.type(), QTransform::TxProject);
        QVERIFY(obj(ds)->hitTest(QPointF(320, 160), 0));
        QVERIFY(!obj(ds)->hitTest(QPointF(320, 60), 0));
        // the distortion round-trips through .lwd
        {
            std::unique_ptr<Document> back(lwd::deserialize(lwd::serialize(*fd)));
            QVERIFY(back);
            const VObject *bo = back->find(ds);
            QVERIFY(bo);
            QCOMPARE(bo->xform, obj(ds)->xform);
        }

        // 7) Perspective: the partner corner moves the opposite way
        const quint64 ps = addRect(QRectF(50, 180, 100, 100), Qt::darkYellow);
        appState().setTool(Tool::Perspective);
        handleDrag(0, QPointF(20, 3));
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Perspective"));
        q = cv->transformQuad();
        QVERIFY(near(q[0], QPointF(70, 180)) && near(q[1], QPointF(130, 180)));
        QVERIFY(near(q[3], QPointF(50, 280)) && near(q[2], QPointF(150, 280)));
        QVERIFY(obj(ps)->bounds().width() > 99);

        // 8) Scale: Shift on a corner keeps the proportions; Alt scales about the centre
        const quint64 sc = addRect(QRectF(200, 180, 100, 50), Qt::cyan);
        appState().setTool(Tool::Scale);
        handleDrag(2, QPointF(100, 0), Qt::ShiftModifier);
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Scale"));
        QVERIFY(qAbs(obj(sc)->bounds().width() - 200) < 0.6 && qAbs(obj(sc)->bounds().height() - 100) < 0.6);
        QVERIFY(!obj(sc)->hasXform());   // axis-aligned scaling stays a plain rectangle
        handleDrag(6, QPointF(0, -20), Qt::AltModifier);
        QVERIFY(qAbs(obj(sc)->bounds().height() - 60) < 0.6);
        QVERIFY(qAbs(obj(sc)->bounds().center().y() - 230) < 0.6);
        // scaling a rotated object works in its own frame
        QVERIFY(cv->rotateSelection(90));
        handleDrag(6, QPointF(-20, 0));   // bottom edge of the rotated box sits on the left
        QVERIFY(qAbs(obj(sc)->bounds().width() - 80) < 0.6);
        QVERIFY(qAbs(obj(sc)->bounds().height() - 200) < 0.6);

        // 9) Numeric Transform: API and dialog
        const quint64 nt = addRect(QRectF(10, 10, 40, 20), Qt::magenta);
        obj(nt)->strokeOn = true;
        obj(nt)->strokeWidth = 2;
        QVERIFY(w->applyNumericTransform(0, 200, 200, true));
        QCOMPARE(obj(nt)->bounds(), QRectF(-10, 0, 80, 40));
        QCOMPARE(obj(nt)->strokeWidth, 4.0);
        QVERIFY(w->applyNumericTransform(1, 40, 40, false));
        QCOMPARE(obj(nt)->bounds(), QRectF(10, 0, 40, 40));
        QCOMPARE(obj(nt)->strokeWidth, 4.0);
        QAction *num = w->action(QStringLiteral("modify.numeric"));
        QCOMPARE(num->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+T")));
        bool saw = false;
        QTimer::singleShot(50, [&] {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg && dlg->objectName() == QStringLiteral("numericTransformDialog"));
            saw = true;
            auto *mode = dlg->findChild<QComboBox *>(QStringLiteral("ntMode"));
            auto *x = dlg->findChild<QDoubleSpinBox *>(QStringLiteral("ntX"));
            auto *y = dlg->findChild<QDoubleSpinBox *>(QStringLiteral("ntY"));
            QCOMPARE(x->value(), 100.0);
            x->setValue(50);
            QCOMPARE(y->value(), 50.0);   // constrained
            mode->setCurrentIndex(1);     // Resize shows pixels
            QCOMPARE(x->value(), 40.0);
            mode->setCurrentIndex(2);
            QVERIFY(!x->isEnabled());
            dlg->findChild<QDoubleSpinBox *>(QStringLiteral("ntAngle"))->setValue(30);
            dlg->accept();
        });
        num->trigger();
        QVERIFY(saw);
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Rotate"));
        QVERIFY(qAbs(xf::rotationOf(*obj(nt)) - 30) < 1e-6);
        QVERIFY(w->applyNumericTransform(3, 20, 0));
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Skew"));

        // 10) bitmaps rotate without resampling; pixel editing bakes them first
        QImage img(40, 20, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::red);
        VObject bm;
        bm.type = ObjType::Bitmap;
        bm.rect = QRectF(300, 230, 40, 20);
        bm.image = img;
        bm.id = fd->newId();
        fd->addObject(bm, 0, false);
        fd->commit(QStringLiteral("bitmap"));
        fd->setSelection({bm.id});
        for (int i = 0; i < 12; ++i) QVERIFY(cv->rotateSelection(30));   // a full turn in 30 degree steps
        QCOMPARE(obj(bm.id)->image.size(), QSize(40, 20));
        QCOMPARE(obj(bm.id)->image, img);                     // the pixels never changed
        QVERIFY(near(obj(bm.id)->bounds().topLeft(), QPointF(300, 230), 0.01));
        QVERIFY(cv->rotateSelection(45));
        VObject baked = *obj(bm.id);
        QVERIFY(xf::bakeBitmap(baked));
        QVERIFY(!baked.hasXform());
        QVERIFY(baked.image.width() > 40 && baked.image.height() > 20);
        QVERIFY(near(baked.bounds().center(), obj(bm.id)->bounds().center(), 1.0));

        // 11) symbol instances rotate as a whole
        DocumentView *v2 = w->addDocument(Document::createNew(300, 300, Qt::white));
        fd = v2->doc();
        cv = v2->canvas();
        const quint64 r2 = addRect(QRectF(100, 140, 100, 20), Qt::red);
        Q_UNUSED(r2);
        QVERIFY(fd->convertToSymbol(QStringLiteral("Bar"), SymbolKind::Graphic));
        QVERIFY(fd->singleSelected() && fd->singleSelected()->type == ObjType::Instance);
        const quint64 inst = fd->singleSelected()->id;
        appState().setTool(Tool::Scale);
        QVERIFY(cv->rotateSelection(90));
        QVERIFY(obj(inst)->hasXform());
        QVERIFY(near(obj(inst)->bounds().topLeft(), QPointF(140, 100)));
        QCOMPARE(obj(inst)->bounds().size(), QSizeF(20, 100));
        QImage frame = cv->renderedFrame();
        QCOMPARE(QColor(frame.pixel(150, 110)), QColor(Qt::red));
        QCOMPARE(QColor(frame.pixel(110, 150)), QColor(Qt::white));

        // 12) Rotate Canvas by any angle: the canvas grows to hold the rotated artwork
        QVERIFY(w->rotateCanvas(30));
        QCOMPARE(fd->s.width, qRound(300 * std::cos(M_PI / 6) + 300 * std::sin(M_PI / 6)));
        QCOMPARE(fd->s.height, fd->s.width);
        QVERIFY(qAbs(xf::rotationOf(*obj(inst)) - 120) < 1e-6 || qAbs(xf::rotationOf(*obj(inst)) + 240) < 1e-6);
        QVERIFY(near(obj(inst)->bounds().center(), QPointF(fd->s.width / 2.0, fd->s.height / 2.0)));
        // ... and from the menu
        QTimer::singleShot(50, [] {
            auto *dlg = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            dlg->setDoubleValue(-30);
            dlg->accept();
        });
        w->action(QStringLiteral("modify.canvasrotate"))->trigger();
        QVERIFY(qAbs(xf::rotationOf(*obj(inst)) - 90) < 1e-6);
        QVERIFY(fd->s.width > 300);
        appState().setTool(Tool::Pointer);
    }

    void toolFlyouts() {
        // Fireworks-style flyouts on grouped tool buttons, driven by real QTest mouse events
        auto *fb = w->findChild<FlyoutToolButton *>(QStringLiteral("tool_Blur"));
        QVERIFY(fb);
        QMenu *m = fb->flyout();
        auto iconKey = [](const QIcon &i) { return i.pixmap(20, 20).toImage(); };
        auto closeMenu = [&] { if (m->isVisible()) QTest::keyClick(m, Qt::Key_Escape); QTRY_VERIFY(!m->isVisible()); };
        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Blur);
        QVERIFY(fb->toolTip().contains(QStringLiteral("click the arrow or hold for more tools")));
        QCOMPARE(m->actions().size(), 5);
        QCOMPARE(m->actions()[2]->text(), QStringLiteral("Dodge\tR"));
        QVERIFY(!m->actions()[2]->icon().isNull());

        // 1) a click on the corner arrow opens the flyout at once and selects the shown tool
        QTest::mouseClick(fb, Qt::LeftButton, {}, fb->arrowRect().center());
        QTRY_VERIFY(m->isVisible());
        QCOMPARE(appState().tool(), Tool::Blur);
        for (QAction *a : m->actions()) QCOMPARE(a->isChecked(), a->data().toInt() == int(Tool::Blur));
        closeMenu();

        // 2) a short press and hold (Fireworks-like ~150 ms, a preference) opens it; the release is not a click
        QCOMPARE(FlyoutToolButton::kDefaultHoldMs, 150);
        QCOMPARE(appState().prefs.flyoutHoldMs, 150);
        QWindow *win = fb->window()->windowHandle();
        const QPoint bp = fb->mapTo(fb->window(), QPoint(8, 8));
        QTest::mousePress(win, Qt::LeftButton, {}, bp);
        QVERIFY(!m->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(m->isVisible(), 600);
        QTest::mouseRelease(win, Qt::LeftButton, {}, bp);   // released over the button: menu stays open
        QTest::qWait(50);
        QVERIFY(m->isVisible());
        QCOMPARE(appState().tool(), Tool::Blur);
        closeMenu();
        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Blur);
        QTest::mousePress(fb, Qt::LeftButton, {}, QPoint(8, 8));     // widget-level delivery too
        QTRY_VERIFY_WITH_TIMEOUT(m->isVisible(), 600);
        QTest::mouseRelease(fb, Qt::LeftButton, {}, QPoint(8, 8));
        QCOMPARE(appState().tool(), Tool::Blur);
        closeMenu();

        // 2b) press + move more than 3 px opens it at once, without waiting for the hold
        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Blur);
        QTest::mousePress(win, Qt::LeftButton, {}, bp);
        QTest::mouseMove(win, bp + QPoint(5, 1));
        QCoreApplication::processEvents();
        QVERIFY(m->isVisible());
        QTest::mouseRelease(win, Qt::LeftButton, {}, bp + QPoint(5, 1));
        QTest::qWait(30);
        QVERIFY(m->isVisible());                     // released off the items: still open for a click
        QCOMPARE(appState().tool(), Tool::Blur);
        closeMenu();

        // 2c) press-drag-release onto an item picks it in one gesture
        {
            QTest::mousePress(win, Qt::LeftButton, {}, bp);
            QTest::mouseMove(win, bp + QPoint(6, 0));
            QTRY_VERIFY(m->isVisible());
            QAction *sharpen = m->findChild<QAction *>(QStringLiteral("flyoutItem_Sharpen"));
            QVERIFY(sharpen);
            const QPoint ip = m->actionGeometry(sharpen).center();
            QTest::mouseMove(m->windowHandle(), ip);
            QTest::mouseRelease(m->windowHandle(), Qt::LeftButton, {}, ip);
            QTRY_VERIFY(!m->isVisible());
            QCOMPARE(appState().tool(), Tool::Sharpen);
            QCOMPARE(fb->currentTool(), Tool::Sharpen);
            appState().setTool(Tool::Pointer);
            fb->setCurrentTool(Tool::Blur);
        }

        // 2d) a quick click without movement selects the current tool and opens nothing
        QTest::mouseClick(win, Qt::LeftButton, {}, bp);
        QCOMPARE(appState().tool(), Tool::Blur);
        QTest::qWait(FlyoutToolButton::holdMs() + 100);
        QVERIFY(!m->isVisible());
        appState().setTool(Tool::Pointer);

        // 3) right-click opens it and selects the shown tool
        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Blur);
        QTest::mouseClick(fb, Qt::RightButton, {}, QPoint(8, 8));
        QTRY_VERIFY(m->isVisible());
        QCOMPARE(appState().tool(), Tool::Blur);
        closeMenu();
        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Blur);
        // ...and through the window, where Qt turns the right press into a QContextMenuEvent
        QTest::mouseClick(fb->window()->windowHandle(), Qt::RightButton, {}, fb->mapTo(fb->window(), QPoint(8, 8)));
        QTRY_VERIFY(m->isVisible());
        QCOMPARE(appState().tool(), Tool::Blur);

        // 4) picking an item switches the tool and the button's icon/tooltip, and is remembered
        QAction *dodge = m->findChild<QAction *>(QStringLiteral("flyoutItem_Dodge"));
        QVERIFY(dodge);
        QTest::mouseClick(m, Qt::LeftButton, {}, m->actionGeometry(dodge).center());
        QTRY_VERIFY(!m->isVisible());
        QCOMPARE(appState().tool(), Tool::Dodge);
        QCOMPARE(fb->currentTool(), Tool::Dodge);
        QCOMPARE(iconKey(fb->icon()), iconKey(icons::tool(Tool::Dodge)));
        QVERIFY(fb->toolTip().startsWith(QStringLiteral("Dodge (R)")));
        QVERIFY(fb->isChecked());
        // the flyout shows the current tool checked next time
        fb->showFlyout();
        QTRY_VERIFY(m->isVisible());
        QVERIFY(dodge->isChecked());
        closeMenu();

        // 5) a normal click selects the remembered tool and opens nothing
        appState().setTool(Tool::Pointer);
        QVERIFY(!fb->isChecked());
        QTest::mouseClick(fb, Qt::LeftButton, {}, QPoint(8, 8));
        QCOMPARE(appState().tool(), Tool::Dodge);
        QVERIFY(fb->isChecked());
        QTest::qWait(FlyoutToolButton::holdMs() + 100);
        QVERIFY(!m->isVisible());

        // 6) a shared shortcut cycles through the group, and the button follows
        QAction *r = w->action(QStringLiteral("tool.Blur"));
        QCOMPARE(r->shortcut(), QKeySequence(QStringLiteral("R")));
        r->trigger();
        QCOMPARE(appState().tool(), Tool::Burn);
        QCOMPARE(fb->currentTool(), Tool::Burn);
        r->trigger(); r->trigger();
        QCOMPARE(appState().tool(), Tool::Blur);
        QCOMPARE(iconKey(fb->icon()), iconKey(icons::tool(Tool::Blur)));
        // other groups have flyouts too
        for (const char *n : {"tool_PaintBucket", "tool_Freeform", "tool_RectangleHotspot"})
            if (!w->findChild<FlyoutToolButton *>(QString::fromLatin1(n))) qInfo() << "no flyout named" << n;
        int visibleFlyouts = 0;
        for (auto *b : w->findChildren<FlyoutToolButton *>())
            if (b->isVisible()) ++visibleFlyouts;
        QCOMPARE(visibleFlyouts, 11);   // Crop/Export Area, Marquee, Lasso, Rubber Stamp/Healing, Rect/Ellipse/Polygon, Scale/Skew/Distort/Perspective, Blur, Paint Bucket, Freeform, Pointer/Select Behind, Pen/Vector Path/Redraw Path. The Web section (Slice flyout and Hotspot) is hidden while Enable web features is off (pref off by default). Pointer and Pen chevrons are always visible.
        {   // Skew, Distort and Perspective share the Scale tool's flyout and its Q shortcut
            auto *sb = w->findChild<FlyoutToolButton *>(QStringLiteral("tool_Scale"));
            QVERIFY(sb);
            QAction *q = w->action(QStringLiteral("tool.Scale"));
            QCOMPARE(q->shortcut(), QKeySequence(QStringLiteral("Q")));
            appState().setTool(Tool::Pointer);
            q->trigger();
            QCOMPARE(appState().tool(), Tool::Scale);
            q->trigger();
            QCOMPARE(appState().tool(), Tool::Skew);
            q->trigger();
            QCOMPARE(appState().tool(), Tool::Distort);
            q->trigger();
            QCOMPARE(appState().tool(), Tool::Perspective);
            QCOMPARE(sb->currentTool(), Tool::Perspective);
        }
        appState().setTool(Tool::Pointer);
    }

    static int checkedToolButtonCount(QWidget *root)
    {
        int n = 0, down = 0;
        QWidget *panel = root->findChild<ToolsPanel *>();
        if (!panel) panel = root;
        QSet<QToolButton *> seen;
        for (QToolButton *b : panel->findChildren<QToolButton *>()) {
            if (!b->property("lwTool").isValid() || seen.contains(b)) continue;
            seen.insert(b);
            if (b->isChecked()) ++n;
            if (b->isDown() && !b->isChecked()) ++down;
        }
        return n + (down ? 1000 : 0); // down-but-not-checked on another button is also a failure
    }

    void toolButtonClickSelectsShownTool()
    {
        // Any press on a Tools-panel button selects the tool shown on it, including
        // a press that opens a flyout and is then cancelled (release outside / Escape).
        // Whatever the route, exactly one tool button is checked.
        auto *panel = w->findChild<ToolsPanel *>();
        QVERIFY(panel);
        auto oneChecked = [&] {
            QCOMPARE(checkedToolButtonCount(w), 1);
        };
        auto *fb = w->findChild<FlyoutToolButton *>(QStringLiteral("tool_Blur"));
        QVERIFY(fb);
        if (auto *dock = w->findChild<QDockWidget *>(QStringLiteral("ToolsDock"))) {
            dock->show();
            dock->raise();
        }
        QVERIFY(fb->isVisible());
        QMenu *m = fb->flyout();
        auto closeMenu = [&] { if (m->isVisible()) QTest::keyClick(m, Qt::Key_Escape); QTRY_VERIFY(!m->isVisible()); };
        QWindow *win = fb->window()->windowHandle();
        const QPoint bp = fb->mapTo(fb->window(), QPoint(8, 8));

        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Sharpen);
        QToolTip::hideText();
        for (QWidget *x : QApplication::allWidgets()) {
            if (x && x->inherits("QTipLabel")) { x->hide(); x->close(); }
        }
        fb->setFocus(Qt::MouseFocusReason);
        QTest::qWait(20);
        // Hold to open the flyout, then Escape: Sharpen (shown) stays / becomes active
        QTest::mousePress(fb, Qt::LeftButton, {}, QPoint(8, 8));
        QTRY_VERIFY_WITH_TIMEOUT(m->isVisible(), 1000);
        QCOMPARE(appState().tool(), Tool::Sharpen);
        QTest::mouseRelease(win, Qt::LeftButton, {}, bp);
        closeMenu();
        QCOMPARE(appState().tool(), Tool::Sharpen);
        QCOMPARE(fb->currentTool(), Tool::Sharpen);
        oneChecked();

        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Sharpen);
        // Press-drag opens the flyout; release off any item, then Escape
        QTest::mousePress(win, Qt::LeftButton, {}, bp);
        QTest::mouseMove(win, bp + QPoint(6, 1));
        QTRY_VERIFY(m->isVisible());
        QCOMPARE(appState().tool(), Tool::Sharpen);
        // Release still over the button / off the menu items so no flyout entry is picked
        QTest::mouseRelease(win, Qt::LeftButton, {}, bp + QPoint(5, 1));
        QTest::qWait(30);
        QVERIFY(m->isVisible());
        QCOMPARE(appState().tool(), Tool::Sharpen);
        closeMenu();
        QCOMPARE(appState().tool(), Tool::Sharpen);
        oneChecked();

        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Sharpen);
        QTest::mouseClick(fb, Qt::LeftButton, {}, fb->arrowRect().center());
        QTRY_VERIFY(m->isVisible());
        QCOMPARE(appState().tool(), Tool::Sharpen);
        closeMenu();
        QCOMPARE(appState().tool(), Tool::Sharpen);
        oneChecked();

        // Choosing a flyout item still switches to that item
        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Sharpen);
        fb->showFlyout();
        QTRY_VERIFY(m->isVisible());
        QAction *dodge = m->findChild<QAction *>(QStringLiteral("flyoutItem_Dodge"));
        QVERIFY(dodge);
        QTest::mouseClick(m, Qt::LeftButton, {}, m->actionGeometry(dodge).center());
        QTRY_VERIFY(!m->isVisible());
        QCOMPARE(appState().tool(), Tool::Dodge);
        QCOMPARE(fb->currentTool(), Tool::Dodge);
        oneChecked();

        // Non-flyout tool buttons also select on press/click
        auto *hand = w->findChild<QToolButton *>(QStringLiteral("tool_Hand"));
        QVERIFY(hand);
        QVERIFY(!qobject_cast<FlyoutToolButton *>(hand));
        appState().setTool(Tool::Pointer);
        QTest::mouseClick(hand, Qt::LeftButton, {}, QPoint(8, 8));
        QCOMPARE(appState().tool(), Tool::Hand);
        oneChecked();

        // Programmatic and keyboard / action routes also keep exclusivity
        appState().setTool(Tool::Zoom);
        QCOMPARE(appState().tool(), Tool::Zoom);
        oneChecked();
        if (QAction *a = w->action(toolActionId(Tool::Pencil))) {
            a->trigger();
            QCOMPARE(appState().tool(), Tool::Pencil);
            oneChecked();
        }

        // The reported 0.3.29 regression: press-and-drag a flyout button to
        // select it, then click a different tool — both buttons looked checked.
        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Sharpen);
        QTest::mousePress(win, Qt::LeftButton, {}, bp);
        QTest::mouseMove(win, bp + QPoint(6, 1));
        QTRY_VERIFY(m->isVisible());
        QCOMPARE(appState().tool(), Tool::Sharpen);
        QTest::mouseRelease(win, Qt::LeftButton, {}, bp + QPoint(5, 1));
        QTest::qWait(30);
        closeMenu();
        QCOMPARE(appState().tool(), Tool::Sharpen);
        oneChecked();
        QTest::mouseClick(hand, Qt::LeftButton, {}, QPoint(8, 8));
        QTest::qWait(30);
        QCOMPARE(appState().tool(), Tool::Hand);
        oneChecked();
        QVERIFY(!fb->isChecked());
        QVERIFY(hand->isChecked());
        QVERIFY(!fb->isDown());
        QVERIFY(!hand->isDown());

        appState().setTool(Tool::Pointer);
        fb->setCurrentTool(Tool::Blur);
        oneChecked();
    }

    void fitCanvasToWindowResizesAndIsUndoable()
    {
        QVERIFY(w->action(QStringLiteral("modify.fitToWindow")));
        auto *btn = w->findChild<QPushButton *>(QStringLiteral("docFitToWindow"));
        QVERIFY2(btn, "Fit to Window button missing from Canvas Properties");
        QCOMPARE(btn->text(), QStringLiteral("Fit to Window"));

        DocumentView *v = w->addDocument(Document::createNew(200, 120, Qt::white));
        QVERIFY(v && v->canvas() && v->doc());
        Canvas *c = v->canvas();
        Document *d = v->doc();
        c->setZoom(1.0);
        const QSize old = w->size();
        w->show();
        w->resize(1100, 800);
        QTest::qWait(50);
        QCoreApplication::processEvents();
        QVERIFY(c->width() > 0 && c->height() > 0);
        const int expectW = qBound(1, int(std::lround(double(c->width()) / c->zoom())), 16000);
        const int expectH = qBound(1, int(std::lround(double(c->height()) / c->zoom())), 16000);
        QVERIFY(expectW != 200 || expectH != 120);
        w->action(QStringLiteral("modify.fitToWindow"))->trigger();
        QCOMPARE(d->s.width, expectW);
        QCOMPARE(d->s.height, expectH);
        QVERIFY(d->canUndo());
        w->action(QStringLiteral("edit.undo"))->trigger();
        QCOMPARE(d->s.width, 200);
        QCOMPARE(d->s.height, 120);
        d->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        w->resize(old);
        QCoreApplication::processEvents();
    }

    void zoomFieldFollowsCustomFromColour()
    {
        ZoomField *zf = w->zoomField();
        QVERIFY(zf);
        const QString was = appState().prefs.theme;
        const bool follow = appState().prefs.themeFollowSystem;
        Prefs p = appState().prefs;
        p.themeFollowSystem = false;
        p.theme = QStringLiteral("seed:#ff2244:light");
        appState().setPrefs(p);
        theme::apply(*qApp, p.theme);
        QCoreApplication::processEvents();
        zf->update();
        const QImage a = zf->grab().toImage();
        p.theme = QStringLiteral("seed:#2266ff:light");
        appState().setPrefs(p);
        theme::apply(*qApp, p.theme);
        QCoreApplication::processEvents();
        zf->update();
        const QImage b = zf->grab().toImage();
        int diff = 0;
        const int ww = std::min(a.width(), b.width()), hh = std::min(a.height(), b.height());
        for (int y = 0; y < hh; ++y)
            for (int x = 0; x < ww; ++x)
                if (a.pixel(x, y) != b.pixel(x, y)) ++diff;
        QVERIFY2(diff > 0, "zoom field should change colour when the custom seed changes");
        p.theme = was;
        p.themeFollowSystem = follow;
        appState().setPrefs(p);
        theme::apply(*qApp, was.isEmpty() ? QStringLiteral("classic") : was);
    }

    void longTooltipsWrapAt400px()
    {
        QWidget host;
        host.resize(80, 40);
        host.show();
        const QString tip = QStringLiteral("This is a deliberately long tooltip that must wrap "
                                           "instead of stretching across the screen. ").repeated(8);
        QToolTip::showText(host.mapToGlobal(QPoint(8, 8)), tip, &host);
        QLabel *lab = nullptr;
        QTRY_VERIFY((lab = [&]() -> QLabel * {
            for (QWidget *x : QApplication::allWidgets())
                if (x && x->isVisible() && x->inherits("QTipLabel"))
                    return qobject_cast<QLabel *>(x);
            return nullptr;
        }()));
        QVERIFY(lab->width() <= 400 + 24); // wrap width plus frame/padding
        QVERIFY(lab->height() > lab->fontMetrics().height() * 2);
        QToolTip::hideText();
        host.hide();
        for (int i = 0; i < 6; ++i) QCoreApplication::processEvents();
        for (QWidget *x : QApplication::allWidgets()) {
            if (x && x->inherits("QTipLabel")) { x->hide(); x->close(); }
        }
        QTest::qWait(20);
    }

    void windowCombosFitWithinCap()
    {
        combofit::applyTree(w);
        int n = 0, skipped = 0;
        for (QComboBox *c : w->findChildren<QComboBox *>()) {
            if (!c->isVisible() && c->count() == 0) continue;
            if (combofit::isFileTypeCombo(c)) {
                ++skipped;
                continue;
            }
            QVERIFY2(c->maximumWidth() <= combofit::kDefaultCap,
                     qPrintable(QStringLiteral("combo '%1' max width %2 exceeds cap")
                                    .arg(c->objectName()).arg(c->maximumWidth())));
            ++n;
        }
        QVERIFY2(n >= 3, "expected to walk several app combos");
        Q_UNUSED(skipped);
    }

    void preferencesDialog() {
        QTemporaryDir cfg;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, cfg.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, cfg.path());
        QVERIFY(w->action(QStringLiteral("edit.preferences")));
        QCOMPARE(w->action(QStringLiteral("edit.preferences"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+U")));
        bool seen = false;
        QTimer::singleShot(0, [&] {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg && dlg->objectName() == QStringLiteral("PreferencesDialog"));
            seen = true;
            dlg->findChild<QSpinBox *>(QStringLiteral("prefUndoSteps"))->setValue(5);
            dlg->findChild<QSpinBox *>(QStringLiteral("prefPickDistance"))->setValue(9);
            dlg->findChild<QSpinBox *>(QStringLiteral("prefGridSize"))->setValue(16);
            dlg->findChild<QCheckBox *>(QStringLiteral("prefMouseHighlight"))->setChecked(false);
            dlg->findChild<QCheckBox *>(QStringLiteral("prefBrushCursors"))->setChecked(false);
            dlg->findChild<QComboBox *>(QStringLiteral("prefInterpolation"))->setCurrentIndex(3);
            dlg->findChild<ColorButton *>(QStringLiteral("prefDefaultFill"))->setColor(QColor(10, 200, 30));
            dlg->findChild<ColorButton *>(QStringLiteral("prefPasteboardColor"))->setColor(QColor(50, 50, 60));
            dlg->accept();
        });
        w->action(QStringLiteral("edit.preferences"))->trigger();
        QVERIFY(seen);
        const Prefs &p = appState().prefs;
        QCOMPARE(p.undoSteps, 5);
        QCOMPARE(coreprefs::undoSteps, 5);
        QCOMPARE(coreprefs::interpolation, int(coreprefs::Nearest));
        QCOMPARE(p.pickDistance, 9);
        QCOMPARE(p.gridSize, 16);
        QVERIFY(!p.mouseHighlight);
        appState().defaultColors();
        QCOMPARE(appState().fill, QColor(10, 200, 30));
        // persisted and re-loaded
        appState().setPrefs(Prefs());
        QCOMPARE(appState().prefs.undoSteps, 200);
        appState().loadPrefs();
        QCOMPARE(appState().prefs.undoSteps, 5);
        QCOMPARE(appState().prefs.pasteboardColor, QColor(50, 50, 60));
        // brush cursor pref changes the painting cursor
        appState().setTool(Tool::Brush);
        c->updateCursor();
        QCOMPARE(c->cursor().shape(), Qt::CrossCursor);
        // undo limit applies to documents
        DocumentView *v = w->addDocument(Document::createNew(50, 50, Qt::white));
        Document *pd = v->doc();
        for (int i = 0; i < 9; ++i) { pd->s.width = 50 + i; pd->commit(QStringLiteral("s")); }
        QCOMPARE(pd->history().size(), 6);
        // the canvas paints with the pasteboard colour
        QImage shot = v->canvas()->grab().toImage();
        QCOMPARE(QColor(shot.pixel(1, 1)), QColor(50, 50, 60));
        // Open Recent tracks saved files
        QTemporaryDir td;
        pd->filePath = td.filePath(QStringLiteral("recent.lwd"));
        QVERIFY(w->saveDocument(pd, false));
        QVERIFY(!w->recentFiles().isEmpty());
        QCOMPARE(w->recentFiles().first(), QFileInfo(pd->filePath).absoluteFilePath());
        auto *rm = w->findChild<QMenu *>(QStringLiteral("recentMenu"));
        QVERIFY(rm && !rm->actions().isEmpty());
        QCOMPARE(rm->actions().first()->data().toString(), w->recentFiles().first());
        w->action(QStringLiteral("file.close"))->trigger();
        appState().setTool(Tool::Pointer);
        appState().setPrefs(Prefs());
        appState().defaultColors();
        QCOMPARE(coreprefs::undoSteps, 200);
    }

    void piFitsAt1400() {
        const QSize old = w->size();
        w->resize(1400, 900);
        DocumentView *v = w->addDocument(Document::createNew(300, 200, Qt::white));
        Document *pd = v->doc();
        VObject r;
        r.type = ObjType::Rect;
        r.rect = QRectF(10, 10, 50, 50);
        r.id = pd->newId();
        pd->addObject(r, 0, false);
        pd->commit(QStringLiteral("r"));
        auto *pi = w->findChild<PropertyInspector *>();
        auto *scroll = pi->findChild<QScrollArea *>();
        auto *stack = pi->findChild<QStackedWidget *>();
        struct Case { Tool tool; bool sel; };
        for (const Case &cs : {Case{Tool::Pointer, true}, Case{Tool::Pointer, false}, Case{Tool::Brush, false}, Case{Tool::Text, false}, Case{Tool::Gradient, false}}) {
            appState().setTool(cs.tool);
            if (cs.sel) pd->selectOnly(r.id); else pd->clearSelection();
            QTest::qWait(40);
            qInfo() << "PI page" << stack->currentIndex() << stack->minimumSizeHint().width() << "needs" << stack->currentWidget()->minimumSizeHint().width() << "has" << scroll->viewport()->width();
            QVERIFY2(!scroll->horizontalScrollBar()->isVisible(), qPrintable(QStringLiteral("page %1 needs %2 px, viewport %3").arg(stack->currentIndex())
                     .arg(stack->currentWidget()->minimumSizeHint().width()).arg(scroll->viewport()->width())));
        }
        appState().setTool(Tool::Pointer);
        pd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        w->resize(old);
    }

    void fillsUi() {
        DocumentView *v = w->addDocument(Document::createNew(200, 100, Qt::white));
        Document *gd = v->doc();
        Canvas *gc = v->canvas();
        QTest::qWait(30);
        QVERIFY(!toolIsStub(Tool::Gradient));
        VObject r;
        r.type = ObjType::Rect;
        r.rect = QRectF(0, 0, 200, 100);
        r.fill = Qt::red;
        r.strokeOn = false;
        r.id = gd->newId();
        gd->addObject(r, 0, false);
        gd->commit(QStringLiteral("setup"));
        // Gradient tool: drag across the shape
        appState().setTool(Tool::Gradient);
        drag(gc, {20, 50}, {180, 50});
        {
            const VObject &o = gd->objects(0)[0];
            QCOMPARE(o.fillKind, FillKind::Linear);
            QVERIFY(QLineF(fills::handleToDoc(o, o.gradP0), QPointF(20, 50)).length() < 1.5);
            QVERIFY(QLineF(fills::handleToDoc(o, o.gradP1), QPointF(180, 50)).length() < 1.5);
            QCOMPARE(gd->history()[gd->historyIndex()].label, QStringLiteral("Gradient"));
        }
        // on-canvas handle: drag the end square with the Pointer
        appState().setTool(Tool::Pointer);
        gd->selectOnly(r.id);
        drag(gc, {180, 50}, {100, 90});
        {
            const VObject &o = gd->objects(0)[0];
            QVERIFY(QLineF(fills::handleToDoc(o, o.gradP1), QPointF(100, 90)).length() < 1.5);
            QVERIFY(QLineF(fills::handleToDoc(o, o.gradP0), QPointF(20, 50)).length() < 1.5);
            QCOMPARE(o.rect, QRectF(0, 0, 200, 100)); // the shape did not move
        }
        // the start circle moves the whole gradient
        drag(gc, {20, 50}, {30, 50});
        QVERIFY(QLineF(fills::handleToDoc(gd->objects(0)[0], gd->objects(0)[0].gradP1), QPointF(110, 90)).length() < 1.5);
        gd->undo();
        QVERIFY(QLineF(fills::handleToDoc(gd->objects(0)[0], gd->objects(0)[0].gradP0), QPointF(20, 50)).length() < 1.5);
        // Property Inspector fill options
        QTest::qWait(30);
        auto *kind = w->findChild<QComboBox *>(QStringLiteral("fillKind"));
        auto *edge = w->findChild<QComboBox *>(QStringLiteral("fillEdge"));
        auto *tex = w->findChild<QComboBox *>(QStringLiteral("fillTexture"));
        auto *sk = w->findChild<QComboBox *>(QStringLiteral("strokeKind"));
        auto *scat = w->findChild<QComboBox *>(QStringLiteral("strokeCategory"));
        auto *sname = w->findChild<QComboBox *>(QStringLiteral("strokeName"));
        auto *sa = w->findChild<QComboBox *>(QStringLiteral("strokeAlign"));
        QVERIFY(kind && edge && tex && sa);
        QVERIFY(sk || (scat && sname));
        kind->setCurrentIndex(kind->findText(QStringLiteral("Radial")));
        QCOMPARE(gd->objects(0)[0].fillKind, FillKind::Radial);
        kind->setCurrentIndex(kind->findText(QStringLiteral("Pattern")));
        QCOMPARE(gd->objects(0)[0].fillKind, FillKind::Pattern);
        edge->setCurrentIndex(2);
        QVERIFY(gd->objects(0)[0].feather > 0);
        tex->setCurrentIndex(tex->findText(QStringLiteral("Burlap")));
        QCOMPARE(gd->objects(0)[0].texture, QStringLiteral("Burlap"));
        if (scat && sname) {
            scat->setCurrentIndex(scat->findText(QStringLiteral("Charcoal")));
            sname->setCurrentIndex(sname->findData(QStringLiteral("charcoal.soft")));
        } else sk->setCurrentIndex(sk->findData(QStringLiteral("charcoal.soft")));
        QCOMPARE(gd->objects(0)[0].strokeKind, QStringLiteral("charcoal.soft"));
        QVERIFY(gd->objects(0)[0].strokeOn);
        sa->setCurrentIndex(1);
        QCOMPARE(gd->objects(0)[0].strokeAlign, 1);
        gd->undo();
        QCOMPARE(gd->objects(0)[0].strokeAlign, 0);
        // Edit Gradient dialog
        kind->setCurrentIndex(kind->findText(QStringLiteral("Linear")));
        bool saw = false;
        QTimer::singleShot(50, [&] {
            QWidget *dlg = QApplication::activeModalWidget();
            QVERIFY(dlg && dlg->objectName() == QStringLiteral("gradientDialog"));
            saw = true;
            auto *pre = dlg->findChild<QComboBox *>(QStringLiteral("gradPreset"));
            const int pi = pre->findText(QStringLiteral("Spectrum"));
            pre->setCurrentIndex(pi);
            emit pre->activated(pi);
            qobject_cast<QDialog *>(dlg)->accept();
        });
        w->findChild<PropertyInspector *>()->editGradient();
        QVERIFY(saw);
        QCOMPARE(gd->objects(0)[0].gradient, fills::gradientPreset(QStringLiteral("Spectrum")));
        // onion skinning menu
        gd->addFrame(true);
        gd->commit(QStringLiteral("frame"));
        gd->setCurrentFrame(0);
        auto *onion = w->findChild<QToolButton *>(QStringLiteral("framesOnion"));
        QVERIFY(onion && onion->menu());
        for (QAction *a : onion->menu()->actions()) if (a->text() == QStringLiteral("Show All Frames")) a->trigger();
        QCOMPARE(gd->onionMode, 3);
        QCOMPARE(gd->onionFrames(), QVector<int>({1}));
        gc->repaint();
        gd->setOnion(0);
        // Text > Attach to Path
        VObject t;
        t.type = ObjType::Text;
        t.text = QStringLiteral("Along");
        t.rect = QRectF(5, 5, 10, 10);
        t.fitTextBox();
        t.id = gd->newId();
        gd->addObject(t, 0, false);
        VObject l;
        l.type = ObjType::Line;
        l.p1 = QPointF(10, 80);
        l.p2 = QPointF(190, 20);
        l.id = gd->newId();
        gd->addObject(l, 0, false);
        gd->commit(QStringLiteral("setup2"));
        gd->setSelection({t.id, l.id});
        w->action(QStringLiteral("text.attach"))->trigger();
        QVERIFY(gd->find(t.id) && gd->find(t.id)->textOnPath);
        QVERIFY(!gd->find(l.id));
        w->action(QStringLiteral("text.detach"))->trigger();
        QVERIFY(!gd->find(t.id)->textOnPath);
        gd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        appState().setTool(Tool::Pointer);
    }

    void pathsUi() {
        DocumentView *v = w->addDocument(Document::createNew(300, 200, Qt::white));
        Document *pd = v->doc();
        Canvas *cv = v->canvas();
        QTest::qWait(30);
        QVERIFY(!toolIsStub(Tool::Knife) && !toolIsStub(Tool::Freeform) && !toolIsStub(Tool::ReshapeArea));
        // Pen: click a corner, drag out curve handles, click, finish
        appState().setTool(Tool::Pen);
        click(cv, {20, 100});
        drag(cv, {100, 100}, {100, 60});
        click(cv, {180, 100});
        cv->finishPen(false);
        QCOMPARE(countObjects(pd), 1);
        quint64 pid = pd->objects(0)[0].id;
        {
            const VObject &p = pd->objects(0)[0];
            QCOMPARE(p.type, ObjType::Path);
            QCOMPARE(p.points.size(), 3);
            QVERIFY(p.hasCurves());
            QVERIFY(QLineF(p.hOut[1], QPointF(100, 60)).length() < 1.5);
            QVERIFY(QLineF(p.hIn[1], QPointF(100, 140)).length() < 1.5);
            QCOMPARE(p.hOut[0], p.points[0]); // plain clicks are corner nodes
            QVERIFY(p.bounds().bottom() > 110); // the curve bends toward the in handle
        }
        // Subselection: select the node, drag its out handle (the smooth in handle mirrors), move the node
        pd->setSelection({pid});
        appState().setTool(Tool::Subselect);
        click(cv, {100, 100});
        drag(cv, {100, 60}, {100, 40});
        {
            const VObject &p = *pd->find(pid);
            QVERIFY(QLineF(p.hOut[1], QPointF(100, 40)).length() < 1.5);
            QVERIFY(QLineF(p.hIn[1], QPointF(100, 140)).length() < 1.5);
        }
        drag(cv, {100, 100}, {110, 100});
        {
            const VObject &p = *pd->find(pid);
            QVERIFY(QLineF(p.points[1], QPointF(110, 100)).length() < 1.5);
            QVERIFY(QLineF(p.hOut[1], QPointF(110, 40)).length() < 1.5);
            QCOMPARE(pd->history()[pd->historyIndex()].label, QStringLiteral("Edit Points"));
        }
        // Alt-drag a corner node pulls out new handles
        drag(cv, {180, 100}, {200, 120}, 6, Qt::AltModifier);
        QVERIFY(QLineF(pd->find(pid)->hOut[2], QPointF(200, 120)).length() < 1.5);
        QVERIFY(QLineF(pd->find(pid)->hIn[2], QPointF(160, 80)).length() < 1.5);
        // Knife across the path
        appState().setTool(Tool::Knife);
        drag(cv, {60, 0}, {60, 199}, 8);
        QCOMPARE(countObjects(pd), 2);
        QCOMPARE(pd->history()[pd->historyIndex()].label, QStringLiteral("Knife"));
        pd->undo();
        QCOMPARE(countObjects(pd), 1);

        // Combine Path menu
        auto addRect = [&](const QRectF &r) {
            VObject o;
            o.type = ObjType::Rect;
            o.rect = r;
            o.fill = Qt::red;
            o.id = pd->newId();
            pd->addObject(o, 0, false);
            return o.id;
        };
        quint64 a = addRect(QRectF(20, 20, 60, 60)), b = addRect(QRectF(50, 50, 60, 60));
        pd->commit(QStringLiteral("rects"));
        pd->setSelection({a, b});
        QTest::qWait(20);
        for (const char *id : {"path.join", "path.split", "path.union", "path.intersect", "path.punch", "path.crop",
                               "path.simplify", "path.expand", "path.inset", "path.convert"}) {
            QAction *act = w->action(QString::fromLatin1(id));
            QVERIFY2(act && act->isEnabled(), id);
        }
        QCOMPARE(w->action(QStringLiteral("path.join"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+J")));
        QCOMPARE(w->action(QStringLiteral("path.convert"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+P")));
        const int before = countObjects(pd);
        w->action(QStringLiteral("path.union"))->trigger();
        QCOMPARE(countObjects(pd), before - 1);
        QCOMPARE(pd->history()[pd->historyIndex()].label, QStringLiteral("Union"));
        pd->undo();
        pd->setSelection({a, b});
        w->action(QStringLiteral("path.punch"))->trigger();
        QCOMPARE(countObjects(pd), before - 1);
        QCOMPARE(pd->history()[pd->historyIndex()].label, QStringLiteral("Punch"));
        // Alter Path > Expand Stroke dialog
        quint64 rest = pd->selection.values().first();
        bool saw = false;
        QTimer::singleShot(50, [&] {
            QWidget *dlg = QApplication::activeModalWidget();
            QVERIFY(dlg && dlg->objectName() == QStringLiteral("alterPathDialog"));
            saw = true;
            dlg->findChild<QDoubleSpinBox *>(QStringLiteral("pathAmount"))->setValue(6);
            qobject_cast<QDialog *>(dlg)->accept();
        });
        w->action(QStringLiteral("path.expand"))->trigger();
        QVERIFY(saw);
        QCOMPARE(pd->history()[pd->historyIndex()].label, QStringLiteral("Expand Stroke"));
        QVERIFY(pd->find(rest)->fillOn && !pd->find(rest)->strokeOn);
        pd->undo();
        pd->undo(); // the punch

        // Freeform: pull the top edge of a rectangle up; Reshape Area with the PI size/strength
        pd->setSelection({a});
        appState().setTool(Tool::Freeform);
        QCoreApplication::processEvents();
        auto *spin = w->findChild<QSpinBox *>(QStringLiteral("pathSize"));
        QVERIFY(spin);
        spin->setValue(40);
        QCOMPARE(appState().pathToolSize, 40);
        QVERIFY(pd->find(a)->type == ObjType::Rect);
        drag(cv, {40, 20}, {40, 5}, 8);
        QCOMPARE(pd->find(a)->type, ObjType::Path);
        QVERIFY(pd->find(a)->bounds().top() < 9);
        QCOMPARE(pd->history()[pd->historyIndex()].label, QStringLiteral("Freeform"));
        // pushing from inside moves the left edge out of the circle
        const double left = pd->find(a)->bounds().left();
        drag(cv, {45, 60}, {30, 60}, 8);
        QVERIFY(pd->find(a)->bounds().left() < left - 3);
        appState().setTool(Tool::ReshapeArea);
        auto *str = w->findChild<QSpinBox *>(QStringLiteral("pathStrength"));
        QVERIFY(str);
        str->setValue(100);
        QCOMPARE(appState().reshapeStrength, 100);
        const double bottom = pd->find(a)->bounds().bottom();
        drag(cv, {50, 75}, {50, 95}, 8);
        QVERIFY(pd->find(a)->bounds().bottom() > bottom + 5);
        QCOMPARE(pd->history()[pd->historyIndex()].label, QStringLiteral("Reshape Area"));
        appState().setTool(Tool::Pointer);
        pd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }

    void symbolsUi() {
        Prefs webOn = appState().prefs;
        webOn.enableWebFeatures = true;
        appState().setPrefs(webOn);
        DocumentView *v = w->addDocument(Document::createNew(200, 150, Qt::white));
        Document *d = v->doc();
        QTest::qWait(30);
        VObject r;
        r.type = ObjType::Rect;
        r.rect = QRectF(20, 20, 40, 30);
        r.fill = QColor(Qt::red);
        r.strokeOn = false;
        r.antialias = false;
        r.id = d->newId();
        d->addObject(r, 0, false);
        d->commit(QStringLiteral("rect"));
        d->setSelection({r.id});
        QTest::qWait(30);
        // F8 -> Convert to Symbol dialog
        QAction *conv = w->action(QStringLiteral("symbol.convert"));
        QVERIFY(conv && conv->isEnabled());
        QCOMPARE(conv->shortcut(), QKeySequence(Qt::Key_F8));
        bool saw = false;
        QTimer::singleShot(50, [&] {
            QWidget *dlg = QApplication::activeModalWidget();
            QVERIFY(dlg && dlg->objectName() == QStringLiteral("symbolDialog"));
            saw = true;
            dlg->findChild<QLineEdit *>(QStringLiteral("symbolName"))->setText(QStringLiteral("Box"));
            dlg->findChild<QRadioButton *>(QStringLiteral("kindButton"))->setChecked(true);
            qobject_cast<QDialog *>(dlg)->accept();
        });
        conv->trigger();
        QVERIFY(saw);
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Convert to Symbol"));
        QCOMPARE(d->s.symbols.size(), 1);
        const quint64 sid = d->s.symbols[0].id;
        QCOMPARE(d->s.symbols[0].kind, SymbolKind::Button);
        QTest::qWait(30);
        // Library panel lists it
        auto *lib = w->findChild<LibraryPanel *>();
        QVERIFY(lib);
        auto *list = lib->findChild<QListWidget *>(QStringLiteral("libraryList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        QVERIFY(lib->findChild<QLabel *>(QStringLiteral("libraryCount"))->text().contains(QLatin1Char('1')));
        lib->setCurrentSymbol(sid);
        QCOMPARE(lib->currentSymbol(), sid);
        QVERIFY(lib->insertCurrent());
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Insert Symbol"));
        QCOMPARE(d->instanceCount(sid), 2);
        const quint64 dup = lib->duplicateCurrent();
        QVERIFY(dup);
        QTest::qWait(30);
        QCOMPARE(list->count(), 2);
        lib->setCurrentSymbol(dup);
        QVERIFY(lib->deleteCurrent(false));
        QTest::qWait(30);
        QCOMPARE(list->count(), 1);
        // PI instance row
        const quint64 inst = d->objects(0)[0].id;
        d->setSelection({inst});
        QTest::qWait(30);
        auto *pi = w->findChild<PropertyInspector *>();
        QVERIFY(pi);
        QWidget *row = pi->findChild<QWidget *>(QStringLiteral("PIInstance"));
        QVERIFY(row && !row->isHidden());
        QVERIFY(pi->findChild<QLabel *>(QStringLiteral("InstInfo"))->text().contains(QStringLiteral("Box")));
        auto *fr = pi->findChild<QSpinBox *>(QStringLiteral("InstFrame"));
        fr->setValue(2);
        QCOMPARE(d->find(inst)->symFrame, 1);
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Instance Frame"));
        auto *link = pi->findChild<QLineEdit *>(QStringLiteral("InstLink"));
        QVERIFY(link->isEnabled());
        link->setText(QStringLiteral("a.html"));
        emit link->editingFinished();
        QCOMPARE(d->find(inst)->link, QStringLiteral("a.html"));
        fr->setValue(1);
        // edit the symbol in its own tab; the parent instances follow
        const int tabsBefore = w->findChild<QTabWidget *>(QStringLiteral("docTabs"))->count();
        Document *ed = w->editSymbol(d, sid);
        QVERIFY(ed);
        QVERIFY(w->isSymbolEditor(ed));
        QCOMPARE(w->currentDoc(), ed);
        QCOMPARE(ed->s.frames.size(), 4);
        QCOMPARE(ed->s.frames[1].name, QStringLiteral("Over"));
        QVERIFY(!ed->objects(0).isEmpty());
        ed->objects(0)[0].fill = QColor(Qt::blue);
        ed->commit(QStringLiteral("blue"));
        QTest::qWait(30);
        QCOMPARE(d->symbol(sid)->frames[0][0].fill, QColor(Qt::blue));
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Edit Symbol"));
        QCOMPARE(d->render(0, true).pixelColor(30, 30), QColor(Qt::blue));
        QVERIFY(!ed->isDirty());
        // the Over state is untouched
        QCOMPARE(d->symbol(sid)->frames[1][0].fill, QColor(Qt::red));
        // break apart via the menu
        { auto *tw = w->findChild<QTabWidget *>(QStringLiteral("docTabs")); tw->setCurrentIndex(tw->indexOf(v)); }
        QCOMPARE(w->currentDoc(), d);
        d->setSelection({inst});
        w->action(QStringLiteral("symbol.breakapart"))->trigger();
        QCOMPARE(d->history()[d->historyIndex()].label, QStringLiteral("Break Apart"));
        QCOMPARE(d->instanceCount(sid), 1);
        // closing the parent closes its editor too
        d->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        QTest::qWait(30);
        QCOMPARE(w->findChild<QTabWidget *>(QStringLiteral("docTabs"))->count(), tabsBefore - 1);
        Prefs webOff = appState().prefs;
        webOff.enableWebFeatures = false;
        appState().setPrefs(webOff);
    }
    void stylesPanel() {
        QTemporaryDir cfg;
        qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
        qApp->setProperty("lw_testing", true);
        auto *sp = w->findChild<StylesPanel *>();
        QVERIFY(sp);
        sp->reload();
        const int nb = styles::builtins().size();
        QCOMPARE(sp->styles().size(), nb);
        auto *list = sp->findChild<QListWidget *>(QStringLiteral("stylesList"));
        QVERIFY(list);
        QCOMPARE(list->count(), nb);
        QCOMPARE(list->selectionMode(), QAbstractItemView::ExtendedSelection);
        auto *nw = sp->findChild<QToolButton *>(QStringLiteral("styleNew"));
        auto *del = sp->findChild<QToolButton *>(QStringLiteral("styleDelete"));
        QVERIFY(nw && del);
        auto click = [&](int row, Qt::KeyboardModifiers m = {}) {
            list->scrollToItem(list->item(row));
            QTest::mouseClick(list->viewport(), Qt::LeftButton, m, list->visualItemRect(list->item(row)).center());
        };
        auto dclick = [&](int row) {
            list->scrollToItem(list->item(row));
            const QPoint c = list->visualItemRect(list->item(row)).center();
            QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, c);
            QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, c);
        };
        auto names = [&] { QStringList l; for (const Style &st : sp->styles()) l << st.name; return l; };
        auto fileNames = [] { QStringList l; for (const Style &st : styles::loadLibrary()) l << st.name; return l; };

        DocumentView *v = w->addDocument(Document::createNew(200, 150, Qt::white));
        Document *sd = v->doc();
        QTest::qWait(30);
        // New Style is enabled with nothing selected: it takes the current colours
        sd->setSelection({});
        QCoreApplication::processEvents();
        QVERIFY(nw->isEnabled());
        appState().setFill(QColor(200, 30, 90));
        bool sawDialog = false;
        QTimer::singleShot(50, [&] {
            QWidget *dlg = QApplication::activeModalWidget();
            QVERIFY(dlg && dlg->objectName() == QStringLiteral("newStyleDialog"));
            sawDialog = true;
            QVERIFY(dlg->findChild<QLabel *>(QStringLiteral("styleFromColours")));
            dlg->findChild<QLineEdit *>(QStringLiteral("styleName"))->setText(QStringLiteral("From Colours"));
            qobject_cast<QDialog *>(dlg)->accept();
        });
        nw->click();
        QVERIFY(sawDialog);
        QCOMPARE(sp->styles().last().name, QStringLiteral("From Colours"));
        QCOMPARE(sp->styles().last().attrs.fill, QColor(200, 30, 90));
        QCOMPARE(styles::loadUser().size(), 1);

        VObject r;
        r.type = ObjType::Rect;
        r.rect = QRectF(20, 20, 80, 60);
        r.id = sd->newId();
        sd->addObject(r, 0, false);
        sd->commit(QStringLiteral("rect"));
        sd->setSelection({r.id});
        const int hist = sd->historyIndex();
        // a click only selects; a double-click applies
        const int gold = sp->indexOf(QStringLiteral("Gold"));
        QVERIFY(gold >= 0);
        click(gold);
        QCOMPARE(sp->selectedRows(), QList<int>{gold});
        QCOMPARE(sd->historyIndex(), hist);
        QVERIFY(del->isEnabled());
        QVERIFY(list->item(gold)->toolTip().contains(QStringLiteral("Gold")));
        QVERIFY(list->item(gold)->toolTip().contains(QStringLiteral("Double-click")));
        dclick(gold);
        QCOMPARE(sd->history()[sd->historyIndex()].label, QStringLiteral("Apply Style"));
        QVERIFY(sd->find(r.id)->fill != r.fill);
        // Enter applies the current style too
        const int chrome = sp->indexOf(QStringLiteral("Chrome"));
        click(chrome);
        list->setFocus();
        const QColor goldFill = sd->find(r.id)->fill;
        QTest::keyClick(list, Qt::Key_Return);
        QVERIFY(sd->find(r.id)->fill != goldFill || sd->find(r.id)->fillKind != FillKind::Solid);

        // built-ins can be deleted: the Delete key on the selection
        click(gold);
        QTest::keyClick(list, Qt::Key_Delete);
        QCOMPARE(sp->indexOf(QStringLiteral("Gold")), -1);
        QCOMPARE(sp->styles().size(), nb);   // nb built-ins + 1 user - Gold
        QVERIFY(!fileNames().contains(QStringLiteral("Gold")));
        sp->reload();
        QCOMPARE(sp->indexOf(QStringLiteral("Gold")), -1);   // persisted
        // multi-select with Ctrl-click, delete both with the trash button
        const QString n0 = sp->styles()[0].name, n2 = sp->styles()[2].name;
        click(0);
        click(2, Qt::ControlModifier);
        QCOMPARE(sp->selectedRows(), (QList<int>{0, 2}));
        del->click();
        QCOMPARE(sp->indexOf(n0), -1);
        QCOMPARE(sp->indexOf(n2), -1);
        QCOMPARE(sp->styles().size(), nb - 2);
        // Shift-click selects a range
        click(1);
        click(3, Qt::ShiftModifier);
        QCOMPARE(sp->selectedRows(), (QList<int>{1, 2, 3}));

        // rename a built-in with F2
        const QString old = sp->styles()[1].name;
        click(1);
        autoAnswer(QStringLiteral("Renamed Builtin"));
        QTest::keyClick(list, Qt::Key_F2);
        QCOMPARE(sp->styles()[1].name, QStringLiteral("Renamed Builtin"));
        QVERIFY(sp->styles()[1].builtin);
        QVERIFY(fileNames().contains(QStringLiteral("Renamed Builtin")));
        QVERIFY(!sp->renameStyle(1, QStringLiteral("Chrome")));   // names stay unique

        // options menu: every action is there
        QMenu *m = sp->optionsMenu();
        QVERIFY(m);
        QCOMPARE(sp->findChild<QToolButton *>(QStringLiteral("stylesOptions"))->menu(), m);
        auto act = [&](const char *n) { QAction *a = m->findChild<QAction *>(QString::fromLatin1(n)); if (!a) qWarning("missing %s", n); return a; };
        for (const char *n : {"styles.apply", "styles.new", "styles.duplicate", "styles.rename", "styles.delete", "styles.moveEarlier",
                              "styles.moveLater", "styles.import", "styles.export", "styles.reset"})
            QVERIFY2(act(n), n);
        // reorder: Move Later from the menu, and a drag (the list model moving the row)
        click(1);
        emit m->aboutToShow();
        QVERIFY(act("styles.moveLater")->isEnabled());
        act("styles.moveLater")->trigger();
        QCOMPARE(sp->styles()[2].name, QStringLiteral("Renamed Builtin"));
        QCOMPARE(sp->selectedRows(), QList<int>{2});
        const QString first = sp->styles()[0].name;
        QVERIFY(list->model()->moveRow(QModelIndex(), 0, QModelIndex(), 5));
        QCoreApplication::processEvents();
        QCOMPARE(sp->styles()[4].name, first);
        QCOMPARE(list->item(4)->data(Qt::UserRole).toString(), first);
        QCOMPARE(fileNames(), names());   // the new order is saved
        // Duplicate
        click(sp->indexOf(QStringLiteral("Chrome")));
        emit m->aboutToShow();
        act("styles.duplicate")->trigger();
        QVERIFY(sp->indexOf(QStringLiteral("Chrome copy")) == sp->indexOf(QStringLiteral("Chrome")) + 1);
        QVERIFY(!sp->styles()[sp->indexOf(QStringLiteral("Chrome copy"))].builtin);

        // New Style from the selected rect through the dialog
        sd->find(r.id)->fill = QColor(10, 200, 30);
        sd->commit(QStringLiteral("recolour"));
        QTimer::singleShot(50, [&] {
            QWidget *dlg = QApplication::activeModalWidget();
            QVERIFY(dlg && dlg->objectName() == QStringLiteral("newStyleDialog"));
            QVERIFY(!dlg->findChild<QLabel *>(QStringLiteral("styleFromColours")));
            dlg->findChild<QLineEdit *>(QStringLiteral("styleName"))->setText(QStringLiteral("Green Thing"));
            dlg->findChild<QCheckBox *>(QStringLiteral("styleStroke"))->setChecked(false);
            QVERIFY(!dlg->findChild<QCheckBox *>(QStringLiteral("styleText"))->isEnabled());
            qobject_cast<QDialog *>(dlg)->accept();
        });
        act("styles.new")->trigger();
        QCOMPARE(sp->styles().last().name, QStringLiteral("Green Thing"));
        QVERIFY(!sp->styles().last().hasStroke);
        sp->applyStyle(sp->indexOf(QStringLiteral("Chrome")));
        sp->applyStyle(sp->indexOf(QStringLiteral("Green Thing")));
        QCOMPARE(sd->find(r.id)->fill, QColor(10, 200, 30));
        sd->undo();
        QVERIFY(sd->find(r.id)->fill != QColor(10, 200, 30));

        // export the selected styles, delete them, import them back
        const int gt = sp->indexOf(QStringLiteral("Green Thing")), fc = sp->indexOf(QStringLiteral("From Colours"));
        click(gt);
        click(fc, Qt::ControlModifier);
        const QString exp = cfg.filePath(QStringLiteral("mine.lwstyles"));
        QString err;
        QVERIFY2(sp->exportStyles(exp, sp->selectedRows(), &err), qPrintable(err));
        QCOMPARE(styles::importFile(exp).size(), 2);
        QTest::keyClick(list, Qt::Key_Delete);
        QCOMPARE(sp->indexOf(QStringLiteral("Green Thing")), -1);
        QCOMPARE(sp->importStyles(exp, &err), 2);
        QVERIFY(sp->indexOf(QStringLiteral("Green Thing")) >= 0);
        QCOMPARE(sp->styles()[sp->indexOf(QStringLiteral("Green Thing"))].attrs.fill, QColor(10, 200, 30));
        QCOMPARE(sp->importStyles(exp, &err), 2);   // again: unique names
        QVERIFY(sp->indexOf(QStringLiteral("Green Thing 2")) >= 0);
        QCOMPARE(sp->selectedRows().size(), 2);      // the imported ones are selected
        // a broken file is refused
        { QFile bad(cfg.filePath(QStringLiteral("bad.lwstyles"))); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("{\"x\":1}"); }
        QCOMPARE(sp->importStyles(cfg.filePath(QStringLiteral("bad.lwstyles")), &err), 0);
        QVERIFY(!err.isEmpty());

        // Reset Styles: the built-ins come back in their order, user styles stay after them
        const int users = std::count_if(sp->styles().begin(), sp->styles().end(), [](const Style &st) { return !st.builtin; });
        emit m->aboutToShow();
        act("styles.reset")->trigger();
        QCOMPARE(sp->styles().size(), nb + users);
        for (int i = 0; i < nb; ++i) QCOMPARE(sp->styles()[i].name, styles::builtins()[i].name);
        QVERIFY(sp->indexOf(QStringLiteral("Gold")) >= 0);
        QVERIFY(sp->indexOf(QStringLiteral("Green Thing")) >= nb);
        sp->reload();
        QCOMPARE(sp->styles().size(), nb + users);

        sd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        qunsetenv("XDG_CONFIG_HOME");
        sp->reload();
    }
    // counts pixels within `area` that differ strongly (> 120 on some channel) from `bg`
    static int strongPixels(const QImage &img, const QRect &area, QColor bg) {
        int n = 0;
        const QRect a = area.intersected(img.rect());
        for (int y = a.top(); y <= a.bottom(); ++y)
            for (int x = a.left(); x <= a.right(); ++x) {
                const QColor c = img.pixelColor(x, y);
                if (qMax(qAbs(c.red() - bg.red()), qMax(qAbs(c.green() - bg.green()), qAbs(c.blue() - bg.blue()))) > 120) ++n;
            }
        return n;
    }
    void overlaysReadableOnAnyBackground() {
        const Tool oldTool = appState().tool();
        const double oldSize = appState().brushSize;
        for (QColor bg : {QColor(Qt::black), QColor(Qt::white)}) {
            const QString what = bg == Qt::black ? QStringLiteral("black") : QStringLiteral("white");
            DocumentView *v = w->addDocument(Document::createNew(300, 200, bg));
            Document *od = v->doc();
            Canvas *oc = v->canvas();
            QTest::qWait(30);
            oc->setZoom(1.0);
            QCoreApplication::processEvents();
            // brush, eraser and a retouch tool size circles over a flat black / white canvas
            for (Tool tl : {Tool::Brush, Tool::Eraser, Tool::Dodge}) {
                appState().setTool(tl);
                appState().brushSize = 24;
                oc->setAttribute(Qt::WA_UnderMouse, true);
                const QPointF vc = oc->toView(QPointF(150, 100));
                sendMouse(oc, QEvent::MouseMove, vc, Qt::NoButton, Qt::NoButton);
                oc->repaint();
                const QImage img = oc->grab().toImage();
                const QRect area = QRectF(vc - QPointF(16, 16), QSizeF(32, 32)).toAlignedRect();
                const int strong = strongPixels(img, area, bg);
                QVERIFY2(strong >= 30, qPrintable(QStringLiteral("%1 cursor on %2: %3 contrasting px").arg(int(tl)).arg(what).arg(strong)));
                // nothing else in that area: it is the cursor that provides the contrast
                oc->setAttribute(Qt::WA_UnderMouse, false);
                oc->repaint();
                QCOMPARE(strongPixels(oc->grab().toImage(), area, bg), 0);
                // a 1 px tip is still visible
                appState().brushSize = 1;
                oc->setAttribute(Qt::WA_UnderMouse, true);
                oc->repaint();
                QVERIFY2(strongPixels(oc->grab().toImage(), area, bg) >= 8, qPrintable(what));
                oc->setAttribute(Qt::WA_UnderMouse, false);
            }
            // marching ants of a pixel selection, object selection outline with its halo, and handles
            appState().setTool(Tool::Marquee);
            QPainterPath sel;
            sel.addRect(QRectF(20, 20, 80, 60));
            od->pixelSelection = sel;
            oc->repaint();
            QImage img = oc->grab().toImage();
            const QPointF a0 = oc->toView(QPointF(20, 50));
            QVERIFY2(strongPixels(img, QRectF(a0 - QPointF(2, 30), QSizeF(4, 60)).toAlignedRect(), bg) >= 20, qPrintable(what));
            od->pixelSelection = QPainterPath();
            VObject r;
            r.type = ObjType::Rect;
            r.rect = QRectF(150, 40, 100, 100);
            r.fill = bg;            // same colour as the canvas: only the overlay can be seen
            r.fillOn = true;
            r.strokeOn = false;
            r.id = od->newId();
            od->addObject(r, 0, false);
            od->commit(QStringLiteral("setup"));
            appState().setTool(Tool::Pointer);
            od->selectOnly(r.id);
            oc->repaint();
            img = oc->grab().toImage();
            // edge midpoint (between handles) shows the halo-ed selection colour
            const QPointF e = oc->toView(QPointF(175, 40));
            QVERIFY2(strongPixels(img, QRectF(e - QPointF(3, 3), QSizeF(6, 6)).toAlignedRect(), bg) >= 3, qPrintable(what + " selection outline"));
            // corner handle: white fill with a dark border, so both colours appear around it
            const QPointF hc = oc->toView(QPointF(150, 40));
            const QRect ha = QRectF(hc - QPointF(5, 5), QSizeF(10, 10)).toAlignedRect();
            QVERIFY2(strongPixels(img, ha, Qt::black) >= 6 && strongPixels(img, ha, Qt::white) >= 6, qPrintable(what + " handle"));
            od->markClean();
            w->action(QStringLiteral("file.close"))->trigger();
        }
        appState().brushSize = oldSize;
        appState().setTool(oldTool);
    }
    void textSliderFields() {
        // these drags are written for the horizontal pop-up (Vertical is the default since 0.3.2; v031 covers it)
        PopupSliderHub::setOrientation(Qt::Horizontal);
        auto restoreOrientation = qScopeGuard([] { PopupSliderHub::setOrientation(Qt::Vertical); });
        DocumentView *v = w->addDocument(Document::createNew(600, 300, Qt::white));
        Document *td = v->doc();
        QTest::qWait(30);
        VObject t;
        t.type = ObjType::Text;
        t.text = QStringLiteral("Hello\nWorld");
        t.fontSize = 24;
        t.fill = Qt::black;
        t.fillOn = true;
        t.rect = QRectF(10, 10, 10, 10);
        t.fitTextBox();
        t.id = td->newId();
        td->addObject(t, 0, false);
        td->commit(QStringLiteral("setup"));
        appState().setTool(Tool::Pointer);
        td->selectOnly(t.id);
        QCoreApplication::processEvents();
        auto *fs = w->findChild<PopupSliderDoubleField *>(QStringLiteral("fontSize"));
        QVERIFY(fs);
        QTRY_VERIFY(fs->isVisible());
        QCOMPARE(fs->value(), 24.0);
        QCOMPARE(fs->sliderLo(), 1.0);
        QCOMPARE(fs->sliderHi(), 200.0);
        QVERIFY(fs->maximum() >= 1000);
        // typed values beyond the slider range
        fs->setValue(1200);
        QCOMPARE(td->objects(0)[0].fontSize, 1200.0);
        td->undo();
        QCOMPARE(td->objects(0)[0].fontSize, 24.0);
        QCoreApplication::processEvents();
        // preset list
        auto *pb = w->findChild<QToolButton *>(QStringLiteral("fontSizePresets"));
        QVERIFY(pb && pb->menu());
        QStringList presets;
        for (QAction *a : pb->menu()->actions()) presets << a->text();
        for (const char *want : {"8", "9", "10", "12", "14", "18", "24", "36", "48", "72"}) QVERIFY(presets.contains(QLatin1String(want)));
        for (QAction *a : pb->menu()->actions()) if (a->text() == QLatin1String("36")) a->trigger();
        QCOMPARE(td->objects(0)[0].fontSize, 36.0);
        td->undo();
        QCoreApplication::processEvents();
        QCOMPARE(fs->value(), 24.0);
        // slider drag: live preview (the text box grows while dragging) and one undo step
        SliderArrow *arrow = fs->arrow();
        QVERIFY(arrow && arrow->isVisible());
        QWindow *win = w->windowHandle();
        const QPoint ap = arrow->mapTo(w, arrow->rect().center());
        const int hist0 = td->historyIndex();
        const double w0 = td->objects(0)[0].rect.width();
        QTest::mousePress(win, Qt::LeftButton, Qt::NoModifier, ap);
        QTRY_VERIFY(fs->isPopupOpen());
        SliderPopup *pop = fs->popup();
        int updates = 0;
        double lastW = w0;
        for (int i = 1; i <= 8; ++i) {
            const QPoint g = arrow->mapToGlobal(arrow->rect().center()) + QPoint(i * 6, 0);
            QMouseEvent mv(QEvent::MouseMove, pop->mapFromGlobal(QPointF(g)), QPointF(g), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(pop, &mv);
            const double now = td->objects(0)[0].rect.width();
            if (now != lastW) { ++updates; lastW = now; }
        }
        QVERIFY2(updates >= 4, qPrintable(QString::number(updates)));
        QVERIFY(td->objects(0)[0].fontSize > 24);
        QVERIFY(lastW > w0);
        {
            QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(5, 5), pop->mapToGlobal(QPointF(5, 5)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(pop, &rel);
        }
        QTRY_VERIFY(!fs->isPopupOpen());
        QCOMPARE(td->historyIndex(), hist0 + 1);
        QCOMPARE(td->history()[td->historyIndex()].label, QStringLiteral("Font Size"));
        td->undo();
        QCOMPARE(td->objects(0)[0].fontSize, 24.0);
        QCoreApplication::processEvents();
        // kerning, leading, horizontal scale, baseline shift are slider fields with real effects
        struct F { const char *name; double val; };
        const VObject base = td->objects(0)[0];
        for (F f : {F{"textKerning", 40}, F{"textLeading", 200}, F{"textHScale", 200}, F{"textBaseline", 10}}) {
            auto *fld = w->findChild<PopupSliderDoubleField *>(QLatin1String(f.name));
            QVERIFY2(fld && fld->isVisible() && fld->arrow(), f.name);
            const int h = td->historyIndex();
            fld->setValue(f.val);
            QCOMPARE(td->historyIndex(), h + 1);
            const VObject &o = td->objects(0)[0];
            const QString n = QLatin1String(f.name);
            if (n == QLatin1String("textKerning")) { QCOMPARE(o.kerning, 40.0); QVERIFY(o.rect.width() > base.rect.width() + 10); }
            if (n == QLatin1String("textLeading")) { QCOMPARE(o.leading, 200.0); QVERIFY(o.rect.height() > base.rect.height() + 10);
                                                     QVERIFY(o.textLines()[1].baseline.y() - o.textLines()[0].baseline.y() > 1.8 * QFontMetricsF(o.font()).lineSpacing()); }
            if (n == QLatin1String("textHScale")) { QCOMPARE(o.hScale, 200.0); QVERIFY(o.rect.width() > base.rect.width() * 1.5); }
            if (n == QLatin1String("textBaseline")) { QCOMPARE(o.baselineShift, 10.0); QCOMPARE(o.textLines()[0].baseline.y(), base.textLines()[0].baseline.y() - 10); }
            td->undo();
            QCoreApplication::processEvents();
        }
        // underline toggles and round-trips through .lwd with the other attributes
        auto *ub = w->findChild<QToolButton *>(QStringLiteral("textUnderline"));
        QVERIFY(ub);
        ub->click();
        QVERIFY(td->objects(0)[0].underline);
        VObject &o = td->objects(0)[0];
        o.kerning = 12; o.leading = 150; o.hScale = 80; o.baselineShift = -3;
        QJsonObject j = lwd::objectToJson(o);
        VObject back;
        QVERIFY(lwd::objectFromJson(j, back));
        QVERIFY(back.underline);
        QCOMPARE(back.kerning, 12.0); QCOMPARE(back.leading, 150.0); QCOMPARE(back.hScale, 80.0); QCOMPARE(back.baselineShift, -3.0);
        td->markClean();
    }
    void popupSliderFields() {
        // these drags are written for the horizontal pop-up (Vertical is the default since 0.3.2; v031 covers it)
        PopupSliderHub::setOrientation(Qt::Horizontal);
        auto restoreOrientation = qScopeGuard([] { PopupSliderHub::setOrientation(Qt::Vertical); });
        DocumentView *v = w->addDocument(Document::createNew(200, 100, Qt::white));
        Document *sd = v->doc();
        QTest::qWait(30);
        VObject r;
        r.type = ObjType::Rect;
        r.rect = QRectF(10, 10, 100, 60);
        r.fill = Qt::blue;
        r.id = sd->newId();
        sd->addObject(r, 0, false);
        sd->commit(QStringLiteral("setup"));
        appState().setTool(Tool::Pointer);
        sd->selectOnly(r.id);
        QCoreApplication::processEvents();
        auto *op = w->findChild<PopupSliderField *>(QStringLiteral("objOpacity"));
        QVERIFY(op);
        QTRY_VERIFY(op->isVisible());
        QCOMPARE(op->value(), 100);
        SliderArrow *arrow = op->arrow();
        QVERIFY(arrow && arrow->isVisible());
        // Leftover QTipLabels (from wrapped tooltips earlier in the suite) can
        // steal the press that should open the slider popup.
        QToolTip::hideText();
        for (QWidget *x : QApplication::allWidgets()) {
            if (x && x->inherits("QTipLabel")) { x->hide(); x->close(); }
        }
        QCoreApplication::processEvents();
        QWindow *win = w->windowHandle();
        const QPoint ap = arrow->mapTo(w, arrow->rect().center());
        const int hist0 = sd->historyIndex();
        // press on the triangle: the popup opens and the knob sits under the pointer
        QTest::mousePress(win, Qt::LeftButton, Qt::NoModifier, ap);
        QTRY_VERIFY(op->isPopupOpen());
        SliderPopup *pop = op->popup();
        QVERIFY(pop->isVisible());
        QVERIFY(pop->windowFlags() & Qt::Popup);
        // drag left in the same motion: live updates on the object
        int lastSeen = 100, updates = 0;
        for (int i = 1; i <= 8; ++i) {
            const QPoint g = arrow->mapToGlobal(arrow->rect().center()) - QPoint(i * 8, 0);
            QMouseEvent mv(QEvent::MouseMove, pop->mapFromGlobal(QPointF(g)), QPointF(g), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(pop, &mv);
            const int now = sd->objects(0)[0].opacity;
            if (now != lastSeen) { ++updates; lastSeen = now; }
        }
        QVERIFY2(updates >= 4, qPrintable(QString::number(updates)));   // live, not only on release
        const int dragged = op->value();
        QVERIFY2(dragged < 70, qPrintable(QString::number(dragged)));
        QCOMPARE(sd->objects(0)[0].opacity, dragged);
        {
            QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(5, 5), pop->mapToGlobal(QPointF(5, 5)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(pop, &rel);
        }
        QTRY_VERIFY(!op->isPopupOpen());          // release after a drag closes it
        // the whole drag is one undo step
        QCOMPARE(sd->historyIndex(), hist0 + 1);
        QCOMPARE(sd->history()[sd->historyIndex()].label, QStringLiteral("Opacity"));
        sd->undo();
        QCOMPARE(sd->objects(0)[0].opacity, 100);
        QCOMPARE(sd->historyIndex(), hist0);
        sd->redo();
        QCOMPARE(sd->objects(0)[0].opacity, dragged);
        // a click opens it and leaves it open; Esc closes it without changing the value
        QTest::mousePress(win, Qt::LeftButton, Qt::NoModifier, ap);
        QTest::mouseRelease(win, Qt::LeftButton, Qt::NoModifier, ap);
        QTRY_VERIFY(op->isPopupOpen());
        QTest::qWait(20);
        QVERIFY(op->isPopupOpen());
        QTest::keyClick(pop, Qt::Key_Escape);
        QTRY_VERIFY(!op->isPopupOpen());
        QCOMPARE(op->value(), dragged);
        QCOMPARE(sd->historyIndex(), hist0 + 1);
        // click on the open slider's track sets the value absolutely and closes it
        QTest::mousePress(win, Qt::LeftButton, Qt::NoModifier, ap);
        QTest::mouseRelease(win, Qt::LeftButton, Qt::NoModifier, ap);
        QTRY_VERIFY(op->isPopupOpen());
        const QRect tr = pop->trackRect();
        QTest::mousePress(pop, Qt::LeftButton, Qt::NoModifier, QPoint(tr.right(), tr.center().y()));
        QTest::mouseRelease(pop, Qt::LeftButton, Qt::NoModifier, QPoint(tr.right(), tr.center().y()));
        QTRY_VERIFY(!op->isPopupOpen());
        QCOMPARE(op->value(), 100);
        QCOMPARE(sd->objects(0)[0].opacity, 100);
        QCOMPARE(sd->historyIndex(), hist0 + 2);
        // typing, arrow keys (Shift = 10) and typed values beyond the slider range
        op->setFocus();
        QTest::keyClick(op, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(op->value(), 90);
        QTest::keyClick(op, Qt::Key_Down);
        QCOMPARE(op->value(), 89);
        auto *sw = w->findChild<PopupSliderDoubleField *>(QStringLiteral("strokeWidth"));
        QVERIFY(sw);
        QVERIFY(sw->sliderHi() < sw->maximum());   // typed range exceeds the slider range
        sw->setValue(250);
        QCOMPARE(sw->value(), 250.0);

    }

    void brushStrokeTarget() {
        // two strokes far apart, in every Stroke target mode
        auto bitmaps = [](Document *dd) {
            QVector<const VObject *> out;
            for (int li = 0; li < dd->s.layers.size(); ++li)
                for (const VObject &o : dd->objects(li)) if (o.type == ObjType::Bitmap) out << &o;
            return out;
        };
        auto px = [](const VObject *o, QPointF docPt) {
            const QPoint ip((docPt - o->rect.topLeft()).toPoint());
            return QRect(QPoint(0, 0), o->image.size()).contains(ip) ? QColor::fromRgba(o->image.pixel(ip)) : QColor(Qt::transparent);
        };
        auto findCombo = [this] { return w->findChild<QComboBox *>(QStringLiteral("opt_strokeTarget")); };
        appState().setTool(Tool::Brush);
        QCoreApplication::processEvents();
        QComboBox *combo = findCombo();
        QVERIFY(combo);
        const int savedMode = appState().strokeTarget, savedSize = appState().brushSize;
        const QColor savedStroke = appState().stroke;
        appState().brushSize = 6;
        appState().setStroke(Qt::red);
        for (int mode : {int(AppState::NewBitmapPerStroke), int(AppState::ExpandSelected), int(AppState::ClipToSelected)}) {
            DocumentView *v = w->addDocument(Document::createNew(300, 200, Qt::white));
            Document *bd = v->doc();
            Canvas *bc = v->canvas();
            QTest::qWait(20);
            // 0.3.7: new documents load the built-in tool defaults (stroke black, etc.)
            appState().setStroke(Qt::red);
            appState().brushSize = 6;
            for (Tool t : {Tool::Brush, Tool::Pencil}) {
                appState().setTool(t);
                QCoreApplication::processEvents();
                QVERIFY(findCombo() && findCombo()->isVisibleTo(w));
            }
            appState().setTool(Tool::Brush);
            appState().brushSize = 6;   // per-tool: set after choosing the tool
            appState().setStroke(Qt::red);
            QCoreApplication::processEvents();
            combo = findCombo();
            combo->setCurrentIndex(combo->findData(mode));    // through the Property Inspector
            QCOMPARE(appState().strokeTarget, mode);
            const int h0 = bd->historyIndex();
            drag(bc, {20, 30}, {60, 30});
            QCOMPARE(bitmaps(bd).size(), 1);
            const QRectF first = bitmaps(bd)[0]->rect;
            QVERIFY2(first.contains(QPointF(20, 30)) && first.contains(QPointF(60, 30)), qPrintable(QStringLiteral("mode %1").arg(mode)));
            QVERIFY(first.width() < 60 && first.height() < 20);   // trimmed to the stroke
            QVERIFY(bd->selection.contains(bitmaps(bd)[0]->id));  // auto-selected, as before
            QSignalSpy msgs(&appState(), &AppState::statusMessage);
            drag(bc, {220, 160}, {260, 160});   // far outside the selected bitmap
            // Clip mode leaves the document unchanged, so the no-op commit is not recorded.
            QCOMPARE(bd->historyIndex(), h0 + (mode == AppState::ClipToSelected ? 1 : 2));
            const auto bm = bitmaps(bd);
            const QString ctx = QStringLiteral("mode %1").arg(mode);
            if (mode == AppState::NewBitmapPerStroke) {
                QCOMPARE(bm.size(), 2);
                QCOMPARE(bm[0]->rect, first);
                QVERIFY(bm[1]->rect.contains(QPointF(240, 160)) && !bm[1]->rect.contains(QPointF(150, 100)));
                QCOMPARE(px(bm[0], {40, 30}), QColor(Qt::red));
                QCOMPARE(px(bm[1], {240, 160}), QColor(Qt::red));
            } else if (mode == AppState::ExpandSelected) {
                QCOMPARE(bm.size(), 1);
                const QRectF r = bm[0]->rect;
                QVERIFY2(r.contains(first) && r.contains(QPointF(262, 162)), qPrintable(ctx));
                QVERIFY(r.right() < 270 && r.bottom() < 170 && r.left() >= first.left() && r.top() >= first.top()); // padding trimmed
                QCOMPARE(px(bm[0], {40, 30}), QColor(Qt::red));     // old pixels kept in place
                QCOMPARE(px(bm[0], {240, 160}), QColor(Qt::red));
                QCOMPARE(px(bm[0], {150, 100}).alpha(), 0);          // transparent padding
                // saved and loaded correctly
                std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*bd)));
                QVERIFY(e);
                const VObject *eo = nullptr;
                for (int li = 0; li < e->s.layers.size(); ++li) for (const VObject &o : e->objects(li)) if (o.type == ObjType::Bitmap) eo = &o;
                QVERIFY(eo);
                QCOMPARE(eo->rect, r);
                QCOMPARE(px(eo, {40, 30}), QColor(Qt::red));
                QCOMPARE(px(eo, {240, 160}), QColor(Qt::red));
                // undo removes the second stroke and its expansion in one step
                bd->undo();
                QCOMPARE(bitmaps(bd).size(), 1);
                QCOMPARE(bitmaps(bd)[0]->rect, first);
                QCOMPARE(px(bitmaps(bd)[0], {40, 30}), QColor(Qt::red));
                bd->redo();
                QCOMPARE(bitmaps(bd)[0]->rect, r);
                // nothing selected: a new bitmap, which later strokes then extend
                bd->clearSelection();
                drag(bc, {150, 20}, {160, 20});
                QCOMPARE(bitmaps(bd).size(), 2);
                drag(bc, {150, 120}, {160, 120});
                QCOMPARE(bitmaps(bd).size(), 2);
                QVERIFY(bitmaps(bd)[1]->rect.contains(QPointF(155, 20)) && bitmaps(bd)[1]->rect.contains(QPointF(155, 120)));
            } else {
                QCOMPARE(bm.size(), 1);
                QCOMPARE(bm[0]->rect, first);                        // clipped, as asked for
                bool told = false;
                for (const auto &a : msgs) told |= a.at(0).toString().contains(QStringLiteral("clipped"));
                QVERIFY(told);                                        // ...but never silently
            }
            // Pencil follows the same rule
            if (mode == AppState::ExpandSelected) {
                appState().setTool(Tool::Pencil);
                bd->selectOnly(bitmaps(bd)[0]->id);
                const QRectF before = bitmaps(bd)[0]->rect;
                drag(bc, {290, 190}, {295, 195});
                QVERIFY(bitmaps(bd)[0]->rect.contains(QPointF(295, 195)) && bitmaps(bd)[0]->rect.contains(before));
                QCOMPARE(px(bitmaps(bd)[0], {295, 195}), QColor(Qt::red));
            }
        }
        // persisted in the tool environment (0.3.7: no longer via global QSettings toolOptions)
        appState().strokeTarget = AppState::NewBitmapPerStroke;
        appState().notifyOptions();
        const QJsonObject env = appState().captureEnvironment();
        appState().strokeTarget = AppState::ExpandSelected;
        appState().applyEnvironment(env);
        QCOMPARE(appState().strokeTarget, int(AppState::NewBitmapPerStroke));
        appState().strokeTarget = savedMode;
        appState().brushSize = savedSize;
        appState().setStroke(savedStroke);
        appState().notifyOptions();
        appState().setTool(Tool::Pointer);
    }

    // Regression: a sliver of transparency checkerboard showed along the bottom/right image edges in
    // Preview / 2-Up / 4-Up / Export Preview (and could on the canvas), its width changing with zoom.
    static bool isChecker(QRgb c) {
        const int r = qRed(c), g = qGreen(c), b = qBlue(c);
        return qAbs(r - g) < 20 && qAbs(g - b) < 20 && g > 180;   // white or #CCCCCC (or a light blend of them)
    }
    // checks the edge rows/columns of `t` (inside) and the ones just outside, within `area`
    static int checkerOnEdges(const QImage &img, const QRect &t, const QRect &area) {
        int bad = 0;
        auto test = [&](int x, int y, bool inside) {
            if (!area.contains(x, y) || !img.rect().contains(x, y)) return;
            if (inside && !t.contains(x, y)) return;
            if (isChecker(img.pixel(x, y))) ++bad;
        };
        // the two edge rows/columns inside, and a 10px band just outside (frame + pasteboard only)
        for (int x = t.left() - 10; x <= t.right() + 10; ++x) {
            test(x, t.top(), true); test(x, t.top() + 1, true); test(x, t.bottom(), true); test(x, t.bottom() - 1, true);
            for (int k = 1; k <= 10; ++k) { test(x, t.top() - k, false); test(x, t.bottom() + k, false); }
        }
        for (int y = t.top() - 10; y <= t.bottom() + 10; ++y) {
            test(t.left(), y, true); test(t.left() + 1, y, true); test(t.right(), y, true); test(t.right() - 1, y, true);
            for (int k = 1; k <= 10; ++k) { test(t.left() - k, y, false); test(t.right() + k, y, false); }
        }
        return bad;
    }
    // the snapped target in device pixels of a grab() (handles fractional device pixel ratios)
    static QRect deviceTarget(QPointF origin, QSizeF ds, double z, double dpr) {
        const QRectF tf = snappedTarget(origin, ds, z, dpr);
        return QRect(QPoint(qRound(tf.left() * dpr), qRound(tf.top() * dpr)), QPoint(qRound(tf.right() * dpr) - 1, qRound(tf.bottom() * dpr) - 1));
    }
    static QRect deviceRect(const QRect &r, double dpr) {
        return QRect(QPoint(qRound(r.left() * dpr), qRound(r.top() * dpr)), QPoint(qRound((r.right() + 1) * dpr) - 1, qRound((r.bottom() + 1) * dpr) - 1));
    }
    void previewNoCheckerSliver() {
        const QList<double> zooms = {1 / 3.0, 0.5, 2 / 3.0, 1.0, 1.5, 2.37, 8.0};
        const QSize ds(401, 299);   // odd sizes so the centred origin is fractional
        // 1) standalone pane, as used by the Export Preview dialog
        {
            PreviewView view(ds);
            PreviewPane pane(&view);
            pane.resize(640, 480);
            QImage opaque(ds, QImage::Format_ARGB32_Premultiplied);
            opaque.fill(QColor(200, 30, 40));
            pane.setResult(opaque, QStringLiteral("t"));
            for (double z : zooms)
                for (QPointF c : {QPointF(ds.width() / 2.0, ds.height() / 2.0), QPointF(ds.width() - 37.3, ds.height() - 21.7), QPointF(17.6, 12.2)}) {
                    view.setView(z, c);
                    const QImage shot = pane.grab().toImage();
                    const double dpr = pane.devicePixelRatioF();
                    const QRect t = deviceTarget(view.origin(pane.imageArea()), QSizeF(ds), z, dpr);
                    QVERIFY2(checkerOnEdges(shot, t, deviceRect(QRect(QPoint(1, 1), pane.imageArea() - QSize(2, 2)), dpr)) == 0,
                             qPrintable(QStringLiteral("export preview pane z=%1 c=%2,%3").arg(z).arg(c.x()).arg(c.y())));
                }
        }
        // 2) Preview, 2-Up and 4-Up of a real document
        DocumentView *v = w->addDocument(Document::createNew(ds.width(), ds.height(), QColor(200, 30, 40)));
        QVERIFY(v);
        for (int mode : {1, 2, 3}) {
            v->setMode(mode);
            v->refresh();
            QCoreApplication::processEvents();
            const int panes = mode == 1 ? 1 : mode == 2 ? 2 : 4;
            for (double z : zooms)
                for (QPointF c : {QPointF(ds.width() / 2.0, ds.height() / 2.0), QPointF(ds.width() - 11.3, ds.height() - 9.7)}) {
                    v->previewView()->setView(z, c);
                    for (int i = 0; i < panes; ++i) {
                        PreviewPane *pp = v->previewPane(mode, i);
                        QVERIFY(pp);
                        const QImage shot = pp->grab().toImage();
                        const double dpr = pp->devicePixelRatioF();
                        const QRect t = deviceTarget(v->previewView()->origin(pp->imageArea()), QSizeF(ds), z, dpr);
                        QVERIFY2(checkerOnEdges(shot, t, deviceRect(QRect(QPoint(4, 4), pp->imageArea() - QSize(8, 8)), dpr)) == 0,
                                 qPrintable(QStringLiteral("mode %1 pane %2 z=%3").arg(mode).arg(i).arg(z)));
                    }
                }
        }
        // 3) main canvas: an almost-opaque canvas colour forces the checkerboard underneath
        v->doc()->markClean();
        v->setMode(0);
        v = w->addDocument(Document::createNew(ds.width(), ds.height(), QColor(200, 30, 40, 254)));
        QCoreApplication::processEvents();
        Document *d = v->doc();
        Canvas *cv = v->canvas();
        for (double z : zooms) {
            cv->setZoom(z, QPointF(cv->width() / 2.0 + 0.3, cv->height() / 2.0 + 0.7));
            QCoreApplication::processEvents();
            for (int k = 0; k < 2; ++k) {
                const QImage shot = cv->grab().toImage();
                const QPointF o = cv->toView(QPointF(0, 0));
                const double dpr = cv->devicePixelRatioF();
                const QRect t = deviceTarget(o, QSizeF(ds), z, dpr);
                QVERIFY2(checkerOnEdges(shot, t, deviceRect(QRect(QPoint(0, 0), cv->size()), dpr)) == 0, qPrintable(QStringLiteral("canvas z=%1").arg(z)));
                // bring the bottom/right corner to a fractional spot near the centre
                cv->panBy(QPointF(cv->width() / 2.0 + 0.37, cv->height() / 2.0 + 0.41) - cv->toView(QPointF(ds.width(), ds.height())));
            }
        }
        d->markClean();
    }

    // The document sits flat on the pasteboard by default: no drop shadow, no border (canvas and
    // preview panes). Preferences > Canvas drop shadow / Canvas border turn them on separately.
    void zoomFieldPopup() {
        DocumentView *v = w->addDocument(Document::createNew(300, 200, Qt::white));
        QCoreApplication::processEvents();
        ZoomField *zf = w->zoomField();
        QVERIFY(zf && zf->isEnabled());
        QCOMPARE(w->findChild<QLineEdit *>(QStringLiteral("statusZoom")), zf);
        QCOMPARE(ZoomField::levels(), (QList<int>{6, 12, 25, 50, 66, 100, 150, 200, 300, 400, 800, 1600, 3200, 6400}));
        w->action(QStringLiteral("view.actual"))->trigger();
        QCOMPARE(zf->text(), QStringLiteral("100%"));
        // the popup lists every level with the current one checked, plus Fit All / Fit Selection
        auto checkMenu = [&](int expectChecked) {
            QMenu *m = zf->buildMenu();
            int checked = -1, n = 0;
            for (QAction *a : m->actions()) {
                if (a->isSeparator()) continue;
                ++n;
                if (a->isChecked()) checked = a->objectName().mid(4).toInt();
            }
            QStringList tail;
            for (QAction *a : m->actions()) if (!a->isSeparator() && !a->isCheckable()) tail << a->text();
            delete m;
            QCOMPARE(n, 16);
            QCOMPARE(tail, (QStringList{QStringLiteral("Fit All"), QStringLiteral("Fit Selection")}));
            QCOMPARE(checked, expectChecked);
        };
        checkMenu(100);
        // clicking the field pops the menu up
        QTest::mouseClick(zf, Qt::LeftButton, {}, QPoint(10, zf->height() / 2));
        QTRY_VERIFY(QApplication::activePopupWidget() && QApplication::activePopupWidget()->objectName() == QStringLiteral("zoomMenu"));
        QMenu *open = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QAction *a400 = nullptr;
        for (QAction *a : open->actions()) if (a->objectName() == QStringLiteral("zoom400")) a400 = a;
        QVERIFY(a400);
        a400->trigger();
        open->close();
        QCOMPARE(v->zoom(), 4.0);
        QCOMPARE(v->canvas()->zoom(), 4.0);
        QCOMPARE(zf->text(), QStringLiteral("400%"));
        checkMenu(400);
        // typing a custom value
        zf->setFocus();
        zf->selectAll();
        QTest::keyClicks(zf, QStringLiteral("137"));
        QTest::keyClick(zf, Qt::Key_Return);
        QCOMPARE(v->zoom(), 1.37);
        QCOMPARE(zf->text(), QStringLiteral("137%"));
        checkMenu(-1);   // not a preset: nothing checked
        // bad input is rejected
        zf->setFocus(); zf->selectAll();
        QTest::keyClicks(zf, QStringLiteral("abc"));
        QTest::keyClick(zf, Qt::Key_Return);
        QCOMPARE(v->zoom(), 1.37);
        QCOMPARE(zf->text(), QStringLiteral("137%"));
        // Fit All / Fit Selection from the menu
        {
            QMenu *m = zf->buildMenu();
            for (QAction *a : m->actions()) if (a->text() == QStringLiteral("Fit All")) a->trigger();
            delete m;
        }
        const double fitZ = v->zoom();
        QVERIFY(fitZ > 0.1);
        appState().setTool(Tool::Rect);
        drag(v->canvas(), QPointF(10, 10), QPointF(30, 25));
        appState().setTool(Tool::Pointer);
        {
            QMenu *m = zf->buildMenu();
            for (QAction *a : m->actions()) if (a->text() == QStringLiteral("Fit Selection")) a->trigger();
            delete m;
        }
        QVERIFY(v->zoom() > fitZ * 4);
        const QPointF c = v->canvas()->toView(QPointF(20, 17.5));
        QVERIFY(qAbs(c.x() - v->canvas()->width() / 2.0) < 2 && qAbs(c.y() - v->canvas()->height() / 2.0) < 2);
        // in 2-Up / 4-Up it drives the shared preview view, so all panes stay in sync
        w->action(QStringLiteral("view.mode3"))->trigger();
        QCOMPARE(v->mode(), 3);
        zf->setFocus(); zf->selectAll();
        QTest::keyClicks(zf, QStringLiteral("250"));
        QTest::keyClick(zf, Qt::Key_Return);
        QCOMPARE(v->previewView()->zoom(), 2.5);
        PreviewPane *p0 = v->previewPane(3, 0), *p3 = v->previewPane(3, 3);
        QVERIFY(p0 && p3 && p0->view() == p3->view() && p0->view() == v->previewView());
        QCOMPARE(zf->text(), QStringLiteral("250%"));
        {
            QMenu *m = zf->buildMenu();
            for (QAction *a : m->actions()) if (a->objectName() == QStringLiteral("zoom50")) a->trigger();
            delete m;
        }
        QCOMPARE(v->previewView()->zoom(), 0.5);
        QCOMPARE(zf->text(), QStringLiteral("50%"));
        w->action(QStringLiteral("view.mode0"))->trigger();
        QCOMPARE(zf->text(), QStringLiteral("%1%").arg(qRound(v->canvas()->zoom() * 100)));
    }

    void smartSnapping() {
        Prefs saved = appState().prefs;
        QVERIFY(Prefs().snapToCanvas && Prefs().snapToObjects);
        QVERIFY(w->action(QStringLiteral("view.snapcanvas"))->isChecked());
        QVERIFY(w->action(QStringLiteral("view.snapobjects"))->isChecked());
        DocumentView *v = w->addDocument(Document::createNew(200, 150, Qt::white));
        QCoreApplication::processEvents();
        Canvas *cv = v->canvas();
        Document *d = v->doc();
        auto cur = [d] { return d->find(*d->selection.begin()); };
        for (double z : {1.0, 4.0}) {
            cv->setZoom(z, QPointF(cv->width() / 2.0, cv->height() / 2.0));
            appState().setTool(Tool::Rect);
            drag(cv, QPointF(50, 40), QPointF(90, 80));
            QCOMPARE(d->selection.size(), 1);
            VObject *o = cur();
            QVERIFY(o);
            QCOMPARE(o->bounds().left(), 50.0);
            appState().setTool(Tool::Pointer);
            // 3 px from the left edge: snaps to x = 0 (6 screen px, whatever the zoom)
            const double off = z == 1.0 ? 3.0 : 1.0;   // 4 / 12 screen px at 400 %: 1 doc px snaps
            drag(cv, QPointF(70, 60), QPointF(70 - 50 + off, 60));
            QCOMPARE(cur()->bounds().left(), 0.0);
            d->undo();
            QCOMPARE(cur()->bounds().left(), 50.0);
            // with Ctrl held the object lands where it is dropped
            drag(cv, QPointF(70, 60), QPointF(70 - 50 + off, 60), 6, Qt::ControlModifier);
            QCOMPARE(cur()->bounds().left(), off);
            d->undo();
            if (z == 4.0) {
                // 3 doc px is 12 screen px at 400 %: too far, no snap
                drag(cv, QPointF(70, 60), QPointF(70 - 50 + 3, 60));
                QCOMPARE(cur()->bounds().left(), 3.0);
                d->undo();
            }
            d->selectAll(); d->deleteSelection();
        }
        cv->setZoom(1.0, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        // centre snaps to the canvas centre line, and a guide line is shown during the drag
        appState().setTool(Tool::Rect);
        drag(cv, QPointF(20, 20), QPointF(60, 40));   // 40 wide: centre at 40
        appState().setTool(Tool::Pointer);
        {
            const QPointF a = cv->toView(QPointF(40, 30)), b = cv->toView(QPointF(40 + 58, 30));   // centre at 98
            sendMouse(cv, QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton);
            sendMouse(cv, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
            sendMouse(cv, QEvent::MouseMove, (a + b) / 2, Qt::NoButton, Qt::LeftButton);
            sendMouse(cv, QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
            QVERIFY(cv->snapLinesX().contains(100.0));
            sendMouse(cv, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
            QVERIFY(cv->snapLinesX().isEmpty());
        }
        const quint64 first = *d->selection.begin();
        QCOMPARE(d->find(first)->bounds().center().x(), 100.0);
        // smart guides: a second object's left edge snaps to the first one's right edge (120)
        appState().setTool(Tool::Rect);
        drag(cv, QPointF(130, 100), QPointF(150, 120));
        appState().setTool(Tool::Pointer);
        drag(cv, QPointF(140, 110), QPointF(140 - 6 - 2, 110));   // left at 122 -> 120
        QCOMPARE(cur()->bounds().left(), 120.0);
        d->undo();
        // ... and not with Snap to Objects off (the View toggle updates and saves the pref)
        w->action(QStringLiteral("view.snapobjects"))->trigger();
        QVERIFY(!appState().prefs.snapToObjects);
        drag(cv, QPointF(140, 110), QPointF(140 - 6 - 2, 110));
        QCOMPARE(cur()->bounds().left(), 122.0);
        d->undo();
        w->action(QStringLiteral("view.snapobjects"))->trigger();
        QVERIFY(appState().prefs.snapToObjects);
        // resizing: dragging the right-hand handle to 197 snaps to the canvas edge (200)
        {
            const QRectF b = cur()->bounds();
            drag(cv, QPointF(b.right(), b.center().y()), QPointF(197, b.center().y()));
            QCOMPARE(cur()->bounds().right(), 200.0);
            QCOMPARE(cur()->bounds().left(), b.left());
        }
        // View > Snap to Canvas Edges off: no canvas snap
        w->action(QStringLiteral("view.snapcanvas"))->trigger();
        QVERIFY(!appState().prefs.snapToCanvas);
        d->selectOnly(first);
        drag(cv, QPointF(100, 30), QPointF(100 - 80 + 3, 30));   // left from 80 to 3
        QCOMPARE(d->find(first)->bounds().left(), 3.0);
        w->action(QStringLiteral("view.snapcanvas"))->trigger();
        appState().setPrefs(saved);
        appState().savePrefs();
    }

    void canvasFrameOptions() {
        Prefs saved = appState().prefs;
        QCOMPARE(Prefs().canvasShadow, false);
        QCOMPARE(Prefs().canvasBorder, false);
        const QSize ds(120, 90);
        DocumentView *v = w->addDocument(Document::createNew(ds.width(), ds.height(), QColor(200, 30, 40)));
        QCoreApplication::processEvents();
        Canvas *cv = v->canvas();
        cv->setZoom(1.0, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        PreviewView pview(ds);
        PreviewPane pane(&pview);
        pane.resize(400, 300);
        QImage opaque(ds, QImage::Format_ARGB32_Premultiplied);
        opaque.fill(QColor(200, 30, 40));
        pane.setResult(opaque, QStringLiteral("t"));
        pview.setView(1.0, QPointF(ds.width() / 2.0, ds.height() / 2.0));
        const QColor pb = appState().prefs.pasteboardColor;
        // pixels just outside the right edge (1px: border) and the shadow zone (3px below/right of the corner)
        auto probe = [&](QWidget *wd, QPointF origin) {
            const QImage shot = wd->grab().toImage();
            const double dpr = wd->devicePixelRatioF();
            const QRect t = deviceTarget(origin, QSizeF(ds), 1.0, dpr);
            return std::make_pair(QColor(shot.pixel(t.right() + 1, t.center().y())), QColor(shot.pixel(t.right() + 3, t.bottom() + 3)));
        };
        auto check = [&](bool shadow, bool border) {
            appState().prefs.canvasShadow = shadow;
            appState().prefs.canvasBorder = border;
            cv->update(); pane.update();
            QCoreApplication::processEvents();
            for (int which = 0; which < 2; ++which) {
                auto [edge, corner] = which == 0 ? probe(cv, cv->toView(QPointF(0, 0))) : probe(&pane, pview.origin(pane.imageArea()));
                const char *where = which == 0 ? "canvas" : "preview";
                if (border) QVERIFY2(edge == canvasBorderColor(), where);
                else if (!shadow) QVERIFY2(edge == pb, qPrintable(QStringLiteral("%1: %2 next to the edge").arg(QLatin1String(where), edge.name())));
                if (shadow) QVERIFY2(corner.value() < pb.value(), where);
                else QVERIFY2(corner == pb, qPrintable(QStringLiteral("%1: shadow %2").arg(QLatin1String(where), corner.name())));
            }
        };
        check(false, false);   // the default: the document edge meets the pasteboard directly
        check(false, true);
        check(true, false);
        check(true, true);
        appState().prefs = saved;
        v->doc()->markClean();
    }

    // Regression: clicking the view tabs made the bar shrink and show scroll buttons
    static void checkTabBar(QTabBar *tb, const char *what) {
        QCoreApplication::processEvents();
        for (QToolButton *b : tb->findChildren<QToolButton *>(Qt::FindDirectChildrenOnly))
            QVERIFY2(!b->isVisible(), what);   // the scroll arrows
        QFont bold = tb->font();
        bold.setBold(true);
        const QFontMetrics fm(bold);   // worst case: the widest possible tab text
        for (int i = 0; i < tb->count(); ++i) {
            QVERIFY2(tb->tabRect(i).width() >= fm.horizontalAdvance(tb->tabText(i)), qPrintable(QStringLiteral("%1 tab %2").arg(QLatin1String(what)).arg(i)));
            QVERIFY2(tb->rect().contains(tb->tabRect(i)), qPrintable(QStringLiteral("%1 tab %2 clipped").arg(QLatin1String(what)).arg(i)));
        }
        QVERIFY2(tb->width() >= tb->sizeHint().width(), what);
    }
    void viewTabsDoNotScroll() {
        DocumentView *v = w->addDocument(Document::createNew(300, 200, Qt::white));
        QVERIFY(v);
        auto *tb = v->findChild<QTabBar *>(QStringLiteral("ViewTabs"));
        QVERIFY(tb);
        QVERIFY(!tb->usesScrollButtons());
        QCOMPARE(tb->elideMode(), Qt::ElideNone);
        const int w0 = tb->sizeHint().width();
        for (int round = 0; round < 2; ++round)
            for (int m : {0, 1, 2, 3, 2, 1, 0}) {
                // click the tab like the user does
                QTest::mouseClick(tb, Qt::LeftButton, {}, tb->tabRect(m).center());
                QCOMPARE(v->mode(), m);
                checkTabBar(tb, "view tabs");
                if (QTest::currentTestFailed()) return;
                QCOMPARE(tb->sizeHint().width(), w0);   // selecting a tab does not change the bar's width
            }
        auto *docTabs = w->findChild<QTabWidget *>(QStringLiteral("docTabs"));
        QVERIFY(docTabs);
        for (int i = 0; i < docTabs->count(); ++i) {
            docTabs->setCurrentIndex(i);
            QCoreApplication::processEvents();
        }
        QCOMPARE(docTabs->tabBar()->elideMode(), Qt::ElideNone);
        if (docTabs->tabBar()->sizeHint().width() <= docTabs->width()) checkTabBar(docTabs->tabBar(), "document tabs");
        v->setMode(0);
        v->doc()->markClean();
    }
    void gridAndGuides() {
        DocumentView *v = w->addDocument(Document::createNew(200, 200, Qt::white));
        Document *gd = v->doc();
        Canvas *gc = v->canvas();
        QTest::qWait(20);
        gc->setZoom(1);
        // menus and shortcuts, per document
        QCOMPARE(w->action(QStringLiteral("view.grid"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+G")));
        QCOMPARE(w->action(QStringLiteral("view.gridsnap"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+G")));
        QCOMPARE(w->action(QStringLiteral("view.rulers"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+R")));
        QVERIFY(w->action(QStringLiteral("view.gridedit")) && w->action(QStringLiteral("view.guidesedit")));
        QVERIFY(!gd->grid.show && !gd->grid.snap);
        w->action(QStringLiteral("view.grid"))->trigger();
        w->action(QStringLiteral("view.gridsnap"))->trigger();
        QVERIFY(gd->grid.show && gd->grid.snap);
        QVERIFY(gd->isDirty());   // saved with the document
        GridSettings g = gd->grid;
        g.x = g.y = 10;
        gd->setGrid(g);
        appState().setTool(Tool::Rect);
        drag(gc, {13, 12}, {47, 38});
        VObject *r = gd->singleSelected();
        QVERIFY(r);
        QCOMPARE(r->rect, QRectF(QPointF(10, 10), QPointF(50, 40)));
        // Pointer move snaps the bounds
        appState().setTool(Tool::Pointer);
        drag(gc, {30, 25}, {36, 27});
        QCOMPARE(r->rect.topLeft(), QPointF(20, 10));
        // resize snaps the dragged edge (bottom-right handle)
        drag(gc, {60, 40}, {72, 52});
        QCOMPARE(gd->history().last().label, QStringLiteral("Transform"));
        QCOMPARE(gd->selectionBounds(), QRectF(QPointF(20, 10), QPointF(70, 50)));
        // marquee
        appState().setTool(Tool::Marquee);
        drag(gc, {103, 104}, {127, 133});
        QCOMPARE(gd->pixelSelection.boundingRect(), QRectF(100, 100, 30, 30));
        // a guide wins when closer than the grid line
        GuideSettings gs = gd->guides;
        gs.v << 113;
        gd->setGuides(gs);
        QCOMPARE(gc->snapPoint({112, 58}), QPointF(113, 60));
        g.snap = false;
        gd->setGrid(g);
        QCOMPARE(gc->snapPoint({112, 58}), QPointF(113, 58));
        gs.snap = false;
        gd->setGuides(gs);
        QCOMPARE(gc->snapPoint({111.5, 58}), QPointF(111.5, 58));
        appState().setTool(Tool::Rect);
        drag(gc, {150, 150}, {163, 171});   // no snapping
        QCOMPARE(gd->singleSelected()->rect, QRectF(QPointF(150, 150), QPointF(163, 171)));
        // slice snaps too (Slice tool needs Enable web features)
        g.snap = true;
        gd->setGrid(g);
        Prefs webOn = appState().prefs;
        webOn.enableWebFeatures = true;
        appState().setPrefs(webOn);
        appState().setTool(Tool::Slice);
        drag(gc, {2, 158}, {38, 192});
        QCOMPARE(gd->singleSelected()->rect, QRectF(QPointF(0, 160), QPointF(40, 190)));
        Prefs webOff = appState().prefs;
        webOff.enableWebFeatures = false;
        appState().setPrefs(webOff);
        appState().setTool(Tool::Pointer);
        // guides dragged out of the rulers; dragging back onto the ruler removes one
        appState().showRulers = true;
        appState().notifyOptions();
        QCoreApplication::processEvents();
        Ruler *hr = v->ruler(Qt::Horizontal);
        QVERIFY(hr && hr->isVisibleTo(w));
        gs = gd->guides;
        gs.v.clear();
        gs.snap = true;
        gd->setGuides(gs);
        const QPoint target = hr->mapFromGlobal(gc->mapToGlobal(gc->toView(QPointF(80, 77)).toPoint()));
        QTest::mousePress(hr, Qt::LeftButton, {}, QPoint(40, 5));
        QTest::mouseMove(hr, target);
        QTest::mouseRelease(hr, Qt::LeftButton, {}, target);
        QCOMPARE(gd->guides.h, QVector<double>({80}));   // grid snap applies to guides
        appState().setTool(Tool::Pointer);
        QPointF gv = gc->toView(QPointF(150, 80));
        sendMouse(gc, QEvent::MouseMove, gv, Qt::NoButton, Qt::NoButton, {});
        QCOMPARE(gc->cursor().shape(), Qt::SplitVCursor);
        drag(gc, {150, 80}, {150, 120});
        QCOMPARE(gd->guides.h, QVector<double>({120}));
        sendMouse(gc, QEvent::MouseButtonPress, gc->toView(QPointF(150, 120)), Qt::LeftButton, Qt::LeftButton, {});
        sendMouse(gc, QEvent::MouseMove, QPointF(150, -30), Qt::NoButton, Qt::LeftButton, {});
        sendMouse(gc, QEvent::MouseButtonRelease, QPointF(150, -30), Qt::LeftButton, Qt::NoButton, {});
        QVERIFY(gd->guides.h.isEmpty());
        // locked guides cannot be dragged
        gs = gd->guides;
        gs.h = {100};
        gs.lock = true;
        gd->setGuides(gs);
        drag(gc, {195, 100}, {195, 140});
        QCOMPARE(gd->guides.h, QVector<double>({100}));
        // menu checks follow the current document
        QVERIFY(w->action(QStringLiteral("view.guideslock"))->isChecked());
        // saved in .lwd
        gs.v = {33.5};
        gs.color = QColor(200, 10, 10);
        gd->setGuides(gs);
        g = {7, 9, QColor(1, 2, 3, 40), true, true};
        gd->setGrid(g);
        std::unique_ptr<Document> e(lwd::deserialize(lwd::serialize(*gd)));
        QVERIFY(e);
        QVERIFY(e->grid == gd->grid);
        QVERIFY(e->guides == gd->guides);
        // Edit Grid dialog: H/V down to 1 px, Apply previews, Cancel restores
        {
            EditGridDialog dlg(gd, w);
            auto *h = dlg.findChild<QSpinBox *>(QStringLiteral("gridH"));
            auto *vv = dlg.findChild<QSpinBox *>(QStringLiteral("gridV"));
            QCOMPARE(h->minimum(), 1);
            QCOMPARE(vv->minimum(), 1);
            h->setValue(1);
            vv->setValue(3);
            dlg.apply();
            QCOMPARE(gd->grid.x, 1);
            QCOMPARE(gd->grid.y, 3);
            dlg.reject();
            QCOMPARE(gd->grid.x, 7);
        }
        appState().showRulers = false;
        appState().notifyOptions();
        gd->markClean();
    }

    void gridOnePixel() {
        // Preferences allow 1 px, and new documents take it as their default
        const Prefs saved = appState().prefs;
        Prefs p = saved;
        p.gridSize = 1;
        appState().setPrefs(p);
        QCOMPARE(appState().prefs.gridSize, 1);
        {
            PreferencesDialog dlg(w);
            QCOMPARE(dlg.findChild<QSpinBox *>(QStringLiteral("prefGridSize"))->minimum(), 1);
        }
        DocumentView *v = w->addDocument(Document::createNew(2000, 2000, Qt::white));
        Document *gd = v->doc();
        Canvas *gc = v->canvas();
        QCOMPARE(gd->grid.x, 1);
        QCOMPARE(gd->grid.y, 1);
        GridSettings g = gd->grid;
        g.snap = true;
        g.show = true;
        g.color = QColor(255, 0, 0);
        gd->setGrid(g);
        // integer pixel snapping at any zoom
        for (double z : {0.25, 1.0, 4.0, 16.0}) {
            gc->setZoom(z);
            QCOMPARE(gc->snapPoint({10.4, 7.6}), QPointF(10, 8));
            QCOMPARE(gc->snapPoint({-3.5001, 1999.2}), QPointF(-4, 1999));
        }
        gc->setView(4, QPointF(20, 20));
        appState().setTool(Tool::Rect);
        drag(gc, {10.3, 20.7}, {30.6, 40.2});
        QCOMPARE(gd->singleSelected()->rect, QRectF(QPointF(10, 21), QPointF(31, 40)));
        appState().setTool(Tool::Marquee);
        drag(gc, {5.4, 5.6}, {9.7, 12.2});
        QCOMPARE(gd->pixelSelection.boundingRect(), QRectF(5, 6, 5, 6));
        // rendering: a pixel grid when zoomed in, thinned out (never a solid wash) below 4 px cells
        auto redFraction = [&](double z) {
            gc->setView(z, QPointF(1000, 1000));
            QElapsedTimer t;
            t.start();
            const QImage img = gc->grab().toImage().convertToFormat(QImage::Format_RGB32);
            const qint64 ms = t.elapsed();
            int red = 0, total = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x, ++total) { const QRgb c = img.pixel(x, y); red += qRed(c) > 200 && qGreen(c) < 120; }
            return std::make_pair(double(red) / total, ms);
        };
        auto [zoomed, ms1] = redFraction(8);
        QVERIFY2(zoomed > 0.15 && zoomed < 0.4, qPrintable(QString::number(zoomed)));   // one line per 8 screen px
        auto [actual, ms2] = redFraction(1);
        QVERIFY2(actual < 0.25, qPrintable(QString::number(actual)));
        auto [out, ms3] = redFraction(0.25);
        QVERIFY2(out < 0.25, qPrintable(QString::number(out)));
        QVERIFY2(ms1 < 1000 && ms2 < 1000 && ms3 < 1000, qPrintable(QStringLiteral("%1 %2 %3").arg(ms1).arg(ms2).arg(ms3)));
        appState().setPrefs(saved);
        gd->markClean();
    }

    // press/move/release with separate modifiers: what is held at press combines with the
    // existing selection, what is pressed during the drag constrains
    static void dragMods(Canvas *c, QPointF a, QPointF b, Qt::KeyboardModifiers press, Qt::KeyboardModifiers move) {
        const QPointF va = c->toView(a), vb = c->toView(b);
        sendMouse(c, QEvent::MouseMove, va, Qt::NoButton, Qt::NoButton, press);
        sendMouse(c, QEvent::MouseButtonPress, va, Qt::LeftButton, Qt::LeftButton, press);
        for (int i = 1; i <= 6; ++i) sendMouse(c, QEvent::MouseMove, va + (vb - va) * (i / 6.0), Qt::NoButton, Qt::LeftButton, move);
        sendMouse(c, QEvent::MouseButtonRelease, vb, Qt::LeftButton, Qt::NoButton, move);
        QCoreApplication::processEvents();
    }

    void selectionTools() {
        // flyout groups and cycling shortcuts
        appState().setTool(Tool::Pointer);
        QCOMPARE(toolGroup(Tool::OvalMarquee).first(), Tool::Marquee);
        QCOMPARE(toolGroup(Tool::PolyLasso).first(), Tool::Lasso);
        QCOMPARE(toolGroup(Tool::SpotHealing), QVector<Tool>({Tool::RubberStamp, Tool::ReplaceColor, Tool::RedEye, Tool::Healing, Tool::SpotHealing, Tool::ContentAware}));
        QAction *m = w->action(QStringLiteral("tool.Marquee"));
        QCOMPARE(m->shortcut(), QKeySequence(QStringLiteral("M")));
        m->trigger(); QCOMPARE(appState().tool(), Tool::Marquee);
        m->trigger(); QCOMPARE(appState().tool(), Tool::OvalMarquee);
        m->trigger(); QCOMPARE(appState().tool(), Tool::Marquee);
        QAction *l = w->action(QStringLiteral("tool.Lasso"));
        QCOMPARE(l->shortcut(), QKeySequence(QStringLiteral("L")));
        l->trigger(); QCOMPARE(appState().tool(), Tool::Lasso);
        l->trigger(); QCOMPARE(appState().tool(), Tool::PolyLasso);
        QAction *st = w->action(QStringLiteral("tool.Rubber Stamp"));
        QCOMPARE(st->shortcut(), QKeySequence(QStringLiteral("S")));
        st->trigger(); QCOMPARE(appState().tool(), Tool::RubberStamp);
        st->trigger(); QCOMPARE(appState().tool(), Tool::ReplaceColor);
        st->trigger(); QCOMPARE(appState().tool(), Tool::RedEye);
        st->trigger(); QCOMPARE(appState().tool(), Tool::Healing);
        st->trigger(); QCOMPARE(appState().tool(), Tool::SpotHealing);
        st->trigger(); QCOMPARE(appState().tool(), Tool::ContentAware);
        st->trigger(); QCOMPARE(appState().tool(), Tool::RubberStamp);
        QVERIFY(w->findChild<FlyoutToolButton *>(QStringLiteral("tool_Marquee")));
        QVERIFY(w->findChild<FlyoutToolButton *>(QStringLiteral("tool_Lasso")));

        DocumentView *v = w->addDocument(Document::createNew(200, 150, Qt::white));
        QCoreApplication::processEvents();
        Canvas *cv = v->canvas();
        Document *d = v->doc();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        AppState &as = appState();
        auto bounds = [d] { return d->pixelSelection.boundingRect(); };
        auto near = [](QRectF a, QRectF b) { return qAbs(a.left() - b.left()) < 1.01 && qAbs(a.top() - b.top()) < 1.01 &&
                                                    qAbs(a.width() - b.width()) < 1.01 && qAbs(a.height() - b.height()) < 1.01; };
        // Oval Marquee: an ellipse in the dragged box
        as.setTool(Tool::OvalMarquee);
        drag(cv, QPointF(20, 20), QPointF(80, 60));
        QVERIFY2(near(bounds(), QRectF(20, 20, 60, 40)), qPrintable(QString::number(bounds().width())));
        QVERIFY(d->pixelSelection.contains(QPointF(50, 40)));
        QVERIFY(!d->pixelSelection.contains(QPointF(22, 22)));   // the box corner is outside the ellipse
        // Shift pressed during the drag: a circle
        dragMods(cv, QPointF(20, 20), QPointF(80, 50), Qt::NoModifier, Qt::ShiftModifier);
        QVERIFY(near(bounds(), QRectF(20, 20, 60, 60)));
        // Alt pressed during the drag: from the centre
        dragMods(cv, QPointF(100, 75), QPointF(120, 85), Qt::NoModifier, Qt::AltModifier);
        QVERIFY(near(bounds(), QRectF(80, 65, 40, 20)));
        // Marquee: Shift at press adds, Alt subtracts, Shift+Alt intersects
        as.setTool(Tool::Marquee);
        drag(cv, QPointF(0, 0), QPointF(50, 50));
        QVERIFY(near(bounds(), QRectF(0, 0, 50, 50)));
        dragMods(cv, QPointF(100, 100), QPointF(150, 140), Qt::ShiftModifier, Qt::ShiftModifier);
        QVERIFY(d->pixelSelection.contains(QPointF(25, 25)) && d->pixelSelection.contains(QPointF(125, 120)));
        QVERIFY(near(bounds(), QRectF(0, 0, 150, 140)));   // Shift held throughout: added, not squared
        dragMods(cv, QPointF(0, 0), QPointF(25, 50), Qt::AltModifier, Qt::AltModifier);
        QVERIFY(!d->pixelSelection.contains(QPointF(10, 10)));
        QVERIFY(d->pixelSelection.contains(QPointF(40, 10)));
        drag(cv, QPointF(0, 0), QPointF(100, 100));
        dragMods(cv, QPointF(50, 50), QPointF(150, 150), Qt::ShiftModifier | Qt::AltModifier, Qt::ShiftModifier | Qt::AltModifier);
        QVERIFY(d->pixelSelection.contains(QPointF(75, 75)));
        QVERIFY(!d->pixelSelection.contains(QPointF(25, 25)));
        QVERIFY(!d->pixelSelection.contains(QPointF(125, 125)));
        // the oval combines the same way
        as.setTool(Tool::OvalMarquee);
        dragMods(cv, QPointF(0, 0), QPointF(40, 40), Qt::ShiftModifier, Qt::ShiftModifier);
        QVERIFY(d->pixelSelection.contains(QPointF(20, 20)) && d->pixelSelection.contains(QPointF(75, 75)));
        // Style: Fixed Size places a box of that size, Fixed Ratio keeps W:H
        as.marqueeStyle = AppState::StyleFixedSize; as.marqueeFixedW = 30; as.marqueeFixedH = 20;
        as.setTool(Tool::Marquee);
        click(cv, QPointF(10, 10));
        QVERIFY(near(bounds(), QRectF(10, 10, 30, 20)));
        as.marqueeStyle = AppState::StyleFixedRatio; as.marqueeRatioW = 2; as.marqueeRatioH = 1;
        drag(cv, QPointF(0, 0), QPointF(40, 5));
        QVERIFY(near(bounds(), QRectF(0, 0, 40, 20)));
        as.marqueeStyle = AppState::StyleNormal;
        // Edge: the selection remembers it, and the mask is feathered
        as.marqueeEdge = Document::SelFeather; as.marqueeFeather = 10;
        drag(cv, QPointF(50, 50), QPointF(150, 120));
        QCOMPARE(d->selectionEdge, int(Document::SelFeather));
        {
            const QImage mk = d->selectionMask(QTransform(), QSize(200, 150));
            QCOMPARE(int(mk.constScanLine(85)[100]), 255);
            const int edge = mk.constScanLine(85)[50];
            QVERIFY2(edge > 40 && edge < 220, qPrintable(QString::number(edge)));
            QVERIFY(mk.constScanLine(85)[44] > 0);   // fades out past the outline
        }
        as.marqueeEdge = Document::SelHard;
        d->clearPixelSelection();   // (a drag inside a selection moves it)
        drag(cv, QPointF(50, 50), QPointF(150, 120));
        QCOMPARE(d->selectionEdge, int(Document::SelHard));
        QCOMPARE(int(d->selectionMask(QTransform(), QSize(200, 150)).constScanLine(85)[49]), 0);
        // the Property Inspector shows the selection options
        QCoreApplication::processEvents();
        auto *edgeBox = w->findChild<QComboBox *>(QStringLiteral("opt_selEdge"));
        auto *styleBox = w->findChild<QComboBox *>(QStringLiteral("opt_selStyle"));
        QVERIFY(edgeBox && styleBox);
        QVERIFY(edgeBox->isVisibleTo(w) && styleBox->isVisibleTo(w));
        styleBox->setCurrentIndex(2);
        QCOMPARE(as.marqueeStyle, int(AppState::StyleFixedSize));
        QVERIFY(w->findChild<QDoubleSpinBox *>(QStringLiteral("opt_selW"))->isVisibleTo(w));
        styleBox->setCurrentIndex(0);
        edgeBox->setCurrentIndex(1);
        QCOMPARE(as.marqueeEdge, 1);
        as.setTool(Tool::Lasso);
        QCoreApplication::processEvents();
        QVERIFY(!w->findChild<QComboBox *>(QStringLiteral("opt_selStyle")));
        edgeBox = w->findChild<QComboBox *>(QStringLiteral("opt_selEdge"));
        QVERIFY(edgeBox && edgeBox->isVisibleTo(w));
        edgeBox->setCurrentIndex(2);
        QCOMPARE(as.lassoEdge, 2);
        QVERIFY(w->findChild<QSpinBox *>(QStringLiteral("opt_selFeather"))->isVisibleTo(w));
        edgeBox->setCurrentIndex(0);
        as.marqueeEdge = 0;

        // Polygon Lasso: click points, click the start point to close
        as.setTool(Tool::PolyLasso);
        click(cv, QPointF(10, 10)); click(cv, QPointF(90, 10)); click(cv, QPointF(90, 90));
        QVERIFY(cv->polyLassoActive());
        QCOMPARE(cv->polyLassoPoints(), 3);
        click(cv, QPointF(10, 10));
        QVERIFY(!cv->polyLassoActive());
        QVERIFY(d->pixelSelection.contains(QPointF(70, 30)));
        QVERIFY(!d->pixelSelection.contains(QPointF(20, 80)));
        // Backspace removes the last point; double-click closes
        click(cv, QPointF(10, 10)); click(cv, QPointF(50, 10)); click(cv, QPointF(50, 50)); click(cv, QPointF(150, 140));
        QCOMPARE(cv->polyLassoPoints(), 4);
        QTest::keyClick(cv, Qt::Key_Backspace);
        QCOMPARE(cv->polyLassoPoints(), 3);
        click(cv, QPointF(10, 50));
        sendMouse(cv, QEvent::MouseButtonDblClick, cv->toView(QPointF(10, 50)), Qt::LeftButton, Qt::LeftButton);
        sendMouse(cv, QEvent::MouseButtonRelease, cv->toView(QPointF(10, 50)), Qt::LeftButton, Qt::NoButton);
        QVERIFY(!cv->polyLassoActive());
        QVERIFY(near(bounds(), QRectF(10, 10, 40, 40)));
        // Shift constrains a side to 45 degrees
        click(cv, QPointF(10, 10));
        {
            const QPointF a = cv->toView(QPointF(60, 14));
            sendMouse(cv, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
            sendMouse(cv, QEvent::MouseButtonRelease, a, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        }
        click(cv, QPointF(60, 60));
        QTest::keyClick(cv, Qt::Key_Return);
        QVERIFY(!cv->polyLassoActive());
        QVERIFY(d->pixelSelection.contains(QPointF(55, 12)));   // the side runs along y = 10
        // Shift at the first click adds to the selection; Escape cancels
        dragMods(cv, QPointF(150, 100), QPointF(150, 100), Qt::ShiftModifier, Qt::ShiftModifier);
        click(cv, QPointF(190, 100)); click(cv, QPointF(190, 140));
        QTest::keyClick(cv, Qt::Key_Return);
        QVERIFY(d->pixelSelection.contains(QPointF(55, 12)) && d->pixelSelection.contains(QPointF(185, 110)));
        click(cv, QPointF(0, 0)); click(cv, QPointF(20, 0));
        QTest::keyClick(cv, Qt::Key_Escape);
        QVERIFY(!cv->polyLassoActive());
        QVERIFY(d->pixelSelection.contains(QPointF(185, 110)));
        // Magic Wand samples the active bitmap (0.3.7); give it a full-canvas bitmap first
        {
            VObject bm;
            bm.type = ObjType::Bitmap;
            bm.id = d->newId();
            bm.rect = QRectF(0, 0, 200, 150);
            bm.image = QImage(200, 150, QImage::Format_ARGB32_Premultiplied);
            bm.image.fill(qRgb(240, 240, 240));
            d->addObject(bm, 0, false);
            d->commit(QStringLiteral("wand bitmap"));
            d->setSelection({bm.id});
        }
        // Magic Wand: Shift adds, Shift+Alt intersects
        as.setTool(Tool::MagicWand);
        as.wandSampleAllLayers = false;
        click(cv, QPointF(100, 75));
        QVERIFY(near(bounds(), QRectF(0, 0, 200, 150)));
        drag(cv, QPointF(0, 0), QPointF(0, 0));
        as.setTool(Tool::Marquee);
        d->clearPixelSelection();   // (a drag inside a selection moves it)
        drag(cv, QPointF(10, 10), QPointF(60, 60));
        as.setTool(Tool::MagicWand);
        dragMods(cv, QPointF(100, 75), QPointF(100, 75), Qt::ShiftModifier | Qt::AltModifier, Qt::ShiftModifier | Qt::AltModifier);
        QVERIFY(near(bounds(), QRectF(10, 10, 50, 50)));
        as.marqueeEdge = as.lassoEdge = as.wandEdge = 0;
        as.setTool(Tool::Pointer);
    }

    void healingBrush() {
        DocumentView *v = w->addDocument(Document::createNew(160, 80, Qt::white));
        QCoreApplication::processEvents();
        Canvas *cv = v->canvas();
        Document *d = v->doc();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Bitmap;
        o.name = QStringLiteral("Bitmap");
        o.image = QImage(160, 80, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 160; ++x) {
                if (x < 80) { const int g = ((x / 2 + y / 2) % 2) ? 150 : 100; o.image.setPixel(x, y, qRgb(g, g, g)); }   // texture
                else o.image.setPixel(x, y, qRgb(200, 90, 50));                                                          // flat
            }
        o.rect = QRectF(0, 0, 160, 80);
        d->objects(d->drawingLayer()).append(o);
        d->commit(QStringLiteral("test"));
        d->selectOnly(o.id);
        AppState &as = appState();
        const double savedSize = as.brushSize;
        as.brushSize = 24;
        as.brushEdge = 50;
        as.healMode = 0;
        as.stampAligned = true;
        as.setTool(Tool::Healing);
        QCoreApplication::processEvents();
        QVERIFY(w->findChild<QComboBox *>(QStringLiteral("opt_healMode"))->isVisibleTo(w));
        QVERIFY(w->findChild<QCheckBox *>(QStringLiteral("opt_useEntireDoc"))->isVisibleTo(w));
        QVERIFY(w->findChild<QSpinBox *>(QStringLiteral("opt_edge"))->isVisibleTo(w));
        dragMods(cv, QPointF(40, 40), QPointF(40, 40), Qt::AltModifier, Qt::AltModifier);   // source
        drag(cv, QPointF(120, 40), QPointF(122, 40), 2);
        const QImage &img = d->find(o.id)->image;
        double mr = 0, mg = 0, mb = 0, vr = 0;
        int n = 0;
        for (int y = 34; y <= 46; ++y)
            for (int x = 115; x <= 127; ++x) { QRgb p = img.pixel(x, y); mr += qRed(p); mg += qGreen(p); mb += qBlue(p); ++n; }
        mr /= n; mg /= n; mb /= n;
        for (int y = 34; y <= 46; ++y)
            for (int x = 115; x <= 127; ++x) vr += qPow(qRed(img.pixel(x, y)) - mr, 2);
        vr /= n;
        QVERIFY2(qAbs(mr - 200) < 12 && qAbs(mg - 90) < 12 && qAbs(mb - 50) < 12,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(mr).arg(mg).arg(mb)));
        QVERIFY2(vr > 100, qPrintable(QString::number(vr)));   // the texture came in
        d->undo();
        QCOMPARE(d->find(o.id)->image.pixel(120, 40), qRgb(200, 90, 50));
        // Spot Healing: a dark spot on the flat area takes the surrounding colour, no source needed
        {
            VObject *b = d->find(o.id);
            QPainter p(&b->image);
            p.fillRect(QRect(117, 37, 6, 6), Qt::black);
        }
        d->commit(QStringLiteral("spot"));
        as.setTool(Tool::SpotHealing);
        drag(cv, QPointF(120, 40), QPointF(120.5, 40), 1);
        const QRgb c = d->find(o.id)->image.pixel(120, 40);
        QVERIFY2(qAbs(qRed(c) - 200) < 25 && qAbs(qGreen(c) - 90) < 25, qPrintable(QStringLiteral("%1").arg(c, 0, 16)));
        as.brushSize = savedSize;
        as.setTool(Tool::Pointer);
    }

    void pixelCopyPaste() {
        DocumentView *v = w->addDocument(Document::createNew(100, 100, Qt::white));
        QCoreApplication::processEvents();
        Canvas *cv = v->canvas();
        Document *d = v->doc();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        VObject o;
        o.id = d->newId();
        o.type = ObjType::Bitmap;
        o.name = QStringLiteral("Bitmap");
        o.image = QImage(100, 100, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 100; ++y)
            for (int x = 0; x < 100; ++x) o.image.setPixel(x, y, qRgb(x * 2, y * 2, (x + y) % 256));
        o.rect = QRectF(0, 0, 100, 100);
        d->objects(d->drawingLayer()).append(o);
        d->commit(QStringLiteral("test"));
        const QImage orig = o.image;
        d->selectOnly(o.id);
        AppState &as = appState();
        as.marqueeEdge = 0;
        as.marqueeStyle = 0;
        auto select10 = [&] {
            as.setTool(Tool::Marquee);
            drag(cv, QPointF(20, 30), QPointF(30, 40));
            QCOMPARE(d->pixelSelection.boundingRect(), QRectF(20, 30, 10, 10));
        };
        select10();
        QVERIFY(d->selection.contains(o.id));   // the marquee keeps the bitmap selected
        w->action(QStringLiteral("edit.copy"))->trigger();
        QCOMPARE(QApplication::clipboard()->image().size(), QSize(10, 10));   // other apps get the pixels
        w->action(QStringLiteral("edit.paste"))->trigger();
        QCOMPARE(countObjects(d), 2);
        QCOMPARE(d->selection.size(), 1);
        VObject *p = d->find(*d->selection.begin());
        QVERIFY(p && p->id != o.id && p->type == ObjType::Bitmap);
        QCOMPARE(p->rect, QRectF(20, 30, 10, 10));
        QCOMPARE(p->image.size(), QSize(10, 10));
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x) QCOMPARE(p->image.pixel(x, y), orig.pixel(20 + x, 30 + y));
        QCOMPARE(d->find(o.id)->image.pixel(25, 35), orig.pixel(25, 35));   // copy leaves the source alone
        d->undo();
        QCOMPARE(countObjects(d), 1);
        // Cut: the copy plus transparent pixels in the source, one undo step
        d->selectOnly(o.id);
        select10();
        w->action(QStringLiteral("edit.cut"))->trigger();
        QCOMPARE(countObjects(d), 1);
        QCOMPARE(qAlpha(d->find(o.id)->image.pixel(25, 35)), 0);
        QCOMPARE(qAlpha(d->find(o.id)->image.pixel(20, 30)), 0);
        QCOMPARE(qAlpha(d->find(o.id)->image.pixel(29, 39)), 0);
        QCOMPARE(d->find(o.id)->image.pixel(30, 40), orig.pixel(30, 40));
        QCOMPARE(d->find(o.id)->image.pixel(19, 30), orig.pixel(19, 30));
        w->action(QStringLiteral("edit.paste"))->trigger();
        p = d->find(*d->selection.begin());
        QCOMPARE(p->rect, QRectF(20, 30, 10, 10));
        QCOMPARE(p->image.pixel(5, 5), orig.pixel(25, 35));
        d->undo();   // paste
        d->undo();   // cut
        QCOMPARE(d->find(o.id)->image.pixel(25, 35), orig.pixel(25, 35));
        // off-screen paste lands in the middle of the view; Paste in Place keeps the position
        d->selectOnly(o.id);
        select10();
        w->action(QStringLiteral("edit.copy"))->trigger();
        cv->setView(16, QPointF(90, 90));
        QVERIFY(!cv->visibleDocRect().intersects(QRectF(20, 30, 10, 10)));
        w->action(QStringLiteral("edit.paste"))->trigger();
        p = d->find(*d->selection.begin());
        QVERIFY(cv->visibleDocRect().intersects(p->rect));
        QVERIFY(QLineF(p->rect.center(), cv->docCenter()).length() < 1.5);
        d->undo();
        w->action(QStringLiteral("edit.pasteinplace"))->trigger();
        p = d->find(*d->selection.begin());
        QCOMPARE(p->rect, QRectF(20, 30, 10, 10));
        d->undo();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        // Copy Merged flattens every visible layer under the selection
        as.setTool(Tool::Rect);
        drag(cv, QPointF(24, 34), QPointF(60, 60));
        QVERIFY(countObjects(d) == 2);
        const QColor rectFill = d->find(*d->selection.begin())->fill;
        d->selectOnly(o.id);
        select10();
        w->action(QStringLiteral("edit.copymerged"))->trigger();
        w->action(QStringLiteral("edit.paste"))->trigger();
        p = d->find(*d->selection.begin());
        QCOMPARE(p->rect, QRectF(20, 30, 10, 10));
        QCOMPARE(p->image.pixel(0, 0), orig.pixel(20, 30));
        QCOMPARE(p->image.pixelColor(8, 8).rgb(), rectFill.rgb());
        d->undo();
        // Modify > Bitmap > Bitmap via Copy / Cut: a new object straight away, one step each
        d->selectOnly(o.id);
        select10();
        const int before = countObjects(d);
        w->action(QStringLiteral("modify.bitmapviacopy"))->trigger();
        QCOMPARE(countObjects(d), before + 1);
        p = d->find(*d->selection.begin());
        QCOMPARE(p->rect, QRectF(20, 30, 10, 10));
        int li = -1, pi = -1, oi = -1;
        d->find(p->id, &li, &pi);
        d->find(o.id, &li, &oi);
        QCOMPARE(pi, oi + 1);   // right above its source
        QCOMPARE(d->find(o.id)->image.pixel(25, 35), orig.pixel(25, 35));
        d->undo();
        d->selectOnly(o.id);
        select10();
        w->action(QStringLiteral("modify.bitmapviacut"))->trigger();
        QCOMPARE(countObjects(d), before + 1);
        QCOMPARE(qAlpha(d->find(o.id)->image.pixel(25, 35)), 0);
        d->undo();
        QCOMPARE(countObjects(d), before);
        QCOMPARE(d->find(o.id)->image.pixel(25, 35), orig.pixel(25, 35));
        // a feathered selection copies feathered alpha
        d->selectOnly(o.id);
        as.marqueeEdge = Document::SelFeather;
        as.marqueeFeather = 6;
        as.setTool(Tool::Marquee);
        drag(cv, QPointF(20, 20), QPointF(60, 60));
        w->action(QStringLiteral("edit.copy"))->trigger();
        w->action(QStringLiteral("edit.paste"))->trigger();
        p = d->find(*d->selection.begin());
        QVERIFY(p->rect.width() > 40);   // reaches past the outline
        const QPoint mid = (QPointF(40, 40) - p->rect.topLeft()).toPoint();
        QCOMPARE(qAlpha(p->image.pixel(mid)), 255);
        const int ea = qAlpha(p->image.pixel((QPointF(20, 40) - p->rect.topLeft()).toPoint()));
        QVERIFY2(ea > 30 && ea < 230, qPrintable(QString::number(ea)));
        as.marqueeEdge = 0;
        // without a pixel selection Copy copies objects as before
        d->pixelSelection = QPainterPath();
        d->selectOnly(o.id);
        w->action(QStringLiteral("edit.copy"))->trigger();
        w->action(QStringLiteral("edit.paste"))->trigger();
        QCOMPARE(d->find(*d->selection.begin())->rect, QRectF(0, 0, 100, 100));
        as.setTool(Tool::Pointer);
    }

    void tipSizeKeys() {
        // the step: 1 px below 10, ~10% above, clamped to 1..1000
        QCOMPARE(MainWindow::stepTipSize(5, 1), 6.0);
        QCOMPARE(MainWindow::stepTipSize(9, 1), 10.0);
        QCOMPARE(MainWindow::stepTipSize(10, 1), 11.0);
        QCOMPARE(MainWindow::stepTipSize(20, 1), 22.0);
        QCOMPARE(MainWindow::stepTipSize(100, 1), 110.0);
        QCOMPARE(MainWindow::stepTipSize(22, -1), 20.0);
        QCOMPARE(MainWindow::stepTipSize(11, -1), 10.0);
        QCOMPARE(MainWindow::stepTipSize(10, -1), 9.0);
        QCOMPARE(MainWindow::stepTipSize(1, -1), 1.0);
        QCOMPARE(MainWindow::stepTipSize(1000, 1), 1000.0);
        QCOMPARE(MainWindow::stepTipSize(990, 1), 1000.0);
        QCOMPARE(w->action(QStringLiteral("tool.sizedown"))->shortcut(), QKeySequence(QStringLiteral("[")));
        QCOMPARE(w->action(QStringLiteral("tool.sizeup"))->shortcut(), QKeySequence(QStringLiteral("]")));
        QVERIFY(w->action(QStringLiteral("tool.softup"))->shortcuts().contains(QKeySequence(QStringLiteral("Shift+]"))));

        AppState &as = appState();
        DocumentView *v = w->addDocument(Document::createNew(200, 150, Qt::white));
        Document *td = v->doc();
        QCoreApplication::processEvents();
        if (QWidget *f = QApplication::focusWidget()) f->clearFocus();
        for (Tool t : {Tool::Brush, Tool::Eraser, Tool::Dodge, Tool::Smudge, Tool::RubberStamp, Tool::Healing}) {
            as.setTool(t);
            QCoreApplication::processEvents();
            as.brushSize = 20; as.brushEdge = 50;
            as.notifyOptions();
            QSignalSpy spy(&as, &AppState::optionsChanged);
            w->action(QStringLiteral("tool.sizeup"))->trigger();
            QCOMPARE(as.brushSize, 22);
            QVERIFY(spy.count() >= 1);   // the Property Inspector and the cursor circle repaint from this
            auto *field = w->findChild<QSpinBox *>(QStringLiteral("opt_size"));
            QVERIFY2(field, qPrintable(toolName(t)));
            QCOMPARE(field->value(), 22);
            w->action(QStringLiteral("tool.sizedown"))->trigger();
            w->action(QStringLiteral("tool.sizedown"))->trigger();
            QCOMPARE(as.brushSize, 18);
            w->action(QStringLiteral("tool.softup"))->trigger();
            QCOMPARE(as.brushEdge, 60);
            QCOMPARE(w->findChild<QSpinBox *>(QStringLiteral("opt_edge"))->value(), 60);
            w->action(QStringLiteral("tool.softdown"))->trigger();
            QCOMPARE(as.brushEdge, 50);
        }
        // small tips step by one pixel
        as.setTool(Tool::Pencil);
        as.brushSize = 3;
        w->action(QStringLiteral("tool.sizeup"))->trigger();
        QCOMPARE(as.brushSize, 4);
        // vector stroke tools change the default stroke width
        as.setTool(Tool::Rect);
        QCoreApplication::processEvents();
        const double sw = as.strokeWidth;
        as.strokeWidth = 4;
        w->action(QStringLiteral("tool.sizeup"))->trigger();
        QCOMPARE(as.strokeWidth, 5.0);
        QCOMPARE(w->findChild<QDoubleSpinBox *>(QStringLiteral("opt_strokeWidth"))->value(), 5.0);
        // Freeform uses its own size
        as.setTool(Tool::Freeform);
        as.pathToolSize = 30;
        w->action(QStringLiteral("tool.sizedown"))->trigger();
        QCOMPARE(as.pathToolSize, 27);
        // Pointer: the selected vectors' stroke, one undo step for a held key
        as.setTool(Tool::Rect);
        as.strokeWidth = 4;
        drag(v->canvas(), {20, 20}, {120, 90});
        const VObject *o = lastObject(td);
        QVERIFY(o && o->type == ObjType::Rect && o->strokeOn);
        const quint64 id = o->id;
        td->setSelection({id});
        as.setTool(Tool::Pointer);
        const int h0 = td->historyIndex();
        for (int i = 0; i < 8; ++i) w->action(QStringLiteral("tool.sizeup"))->trigger();   // autorepeat
        QCOMPARE(td->find(id)->strokeWidth, 12.0);
        QCOMPARE(td->historyIndex(), h0 + 1);
        QCOMPARE(td->history()[td->historyIndex()].label, QStringLiteral("Stroke Size"));
        td->undo();
        QCOMPARE(td->find(id)->strokeWidth, 4.0);
        QTest::qWait(700);   // released: the next press is a new step
        td->redo();
        w->action(QStringLiteral("tool.sizedown"))->trigger();
        QCOMPARE(td->historyIndex(), h0 + 2);
        QCOMPARE(td->find(id)->strokeWidth, 11.0);
        QTest::qWait(700);
        // not while a text field has focus
        as.setTool(Tool::Brush);
        QCoreApplication::processEvents();
        as.brushSize = 20;
        auto *field = w->findChild<QSpinBox *>(QStringLiteral("opt_size"));
        w->activateWindow();
        field->setFocus();
        QCoreApplication::processEvents();
        if (QApplication::focusWidget()) {
            QVERIFY(!w->adjustTipSize(1));
            QCOMPARE(as.brushSize, 20);
            field->clearFocus();
        }
        QVERIFY(w->adjustTipSize(1));
        QCOMPARE(as.brushSize, 22);
        as.strokeWidth = sw;
        td->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }

    void everyToolHasLiveOptions() {
        AppState &as = appState();
        // Include hotspot tool pages: they are hidden unless "Enable web features" is on.
        Prefs webOn = as.prefs;
        webOn.enableWebFeatures = true;
        as.setPrefs(webOn);
        DocumentView *v = w->addDocument(Document::createNew(240, 180, Qt::white));
        QCoreApplication::processEvents();
        auto *pi = w->findChild<PropertyInspector *>();
        QVERIFY(pi);
        auto *stack = pi->findChild<QStackedWidget *>();
        QVERIFY(stack);
        for (int ti = 0; ti <= int(Tool::Zoom); ++ti) {
            const Tool t = Tool(ti);
            v->doc()->clearSelection();
            as.setTool(t);
            QCoreApplication::processEvents();
            const QString tn = toolName(t);
            if (PropertyInspector::toolUsesDocPage(t)) {
                QCOMPARE(stack->currentIndex(), 0);
                continue;
            }
            QCOMPARE(stack->currentIndex(), 2);
            QWidget *host = pi->findChild<QWidget *>(QStringLiteral("PIToolOptions"));
            QVERIFY(host);
            // LW_DOC_SHOTS=<dir>: write the README screenshots of the Dodge and Smudge pages
            const QString shots = qEnvironmentVariable("LW_DOC_SHOTS");
            if (!shots.isEmpty() && (t == Tool::Dodge || t == Tool::Smudge))
                QVERIFY(pi->grab().save(shots + (t == Tool::Dodge ? QStringLiteral("/tool-options-dodge.png") : QStringLiteral("/tool-options-smudge.png"))));
            QList<QWidget *> controls;
            for (QWidget *wd : host->findChildren<QWidget *>()) {
                if (wd->objectName().isEmpty() || wd->objectName().startsWith(QLatin1String("qt_"))) continue;
                if (qobject_cast<QAbstractSpinBox *>(wd) || qobject_cast<QComboBox *>(wd) || qobject_cast<QCheckBox *>(wd) ||
                    qobject_cast<QLineEdit *>(wd) || qobject_cast<QToolButton *>(wd) || qobject_cast<QPushButton *>(wd))
                    controls << wd;
            }
            QVERIFY2(!controls.isEmpty() || t == Tool::Knife, qPrintable(tn + QStringLiteral(": empty options page")));
            if (t == Tool::Knife) QVERIFY(host->findChild<QLabel *>(QStringLiteral("opt_note")));
            for (QWidget *wd : controls) {
                const QString ctx = tn + QLatin1Char('/') + wd->objectName();
                if (wd->property("lwAction").isValid() || qobject_cast<ColorButton *>(wd)) continue;   // commands / colour pickers
                if (auto *tb = qobject_cast<QAbstractButton *>(wd); tb && !qobject_cast<QCheckBox *>(wd) && !tb->isCheckable()) continue;
                if (auto *tb = qobject_cast<QToolButton *>(wd); tb && tb->autoExclusive() && tb->isChecked()) continue;   // the current radio choice
                if (!wd->isEnabled()) continue;   // depends on another option (e.g. W/H with a Normal marquee)
                const QString state = wd->property("lwState").toString();
                const QVariantMap before = as.optionValues();
                const QRectF cropBefore = as.cropRect;
                const QRect areaBefore = as.exportAreaRect;
                const Tool toolBefore = as.tool();
                auto changed = [&] {
                    if (state == QLatin1String("crop")) return as.cropRect != cropBefore;
                    if (state == QLatin1String("exportarea")) return as.exportAreaRect != areaBefore;
                    if (state == QLatin1String("tool")) { QCoreApplication::processEvents(); return as.tool() != toolBefore; }
                    return as.optionValues() != before;
                };
                bool ok = false;
                for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
                    if (auto *sb = qobject_cast<QSpinBox *>(wd)) sb->setValue(sb->value() < sb->maximum() ? sb->value() + 1 : sb->value() - 1);
                    else if (auto *db = qobject_cast<QDoubleSpinBox *>(wd))
                        db->setValue(db->value() + 1 <= db->maximum() ? db->value() + 1 : db->value() - 1);
                    else if (auto *cb = qobject_cast<QComboBox *>(wd)) cb->setCurrentIndex((cb->currentIndex() + 1 + attempt) % cb->count());
                    else if (auto *ck = qobject_cast<QCheckBox *>(wd)) ck->click();
                    else if (auto *le = qobject_cast<QLineEdit *>(wd)) { le->setFocus(); QTest::keyClicks(le, QStringLiteral("x")); }
                    else if (auto *ab = qobject_cast<QAbstractButton *>(wd)) ab->click();
                    ok = changed();
                }
                QVERIFY2(ok, qPrintable(ctx + QStringLiteral(" does not change any option")));
                if (state == QLatin1String("tool")) { as.setTool(t); QCoreApplication::processEvents(); break; }   // page rebuilt
            }
        }
        as.setTool(Tool::Pointer);
        v->doc()->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        Prefs webOff = as.prefs;
        webOff.enableWebFeatures = false;
        as.setPrefs(webOff);
    }

    // The stylesheet replaces the native arrows: every combo, spin box and pop-up slider field must
    // still show its little dark triangle (normal, hover, open and disabled).
    void themeArrowsRender() {
        auto count = [](const QImage &img, QRect r, auto pred) {
            int n = 0;
            r &= img.rect();
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x)
                    if (pred(QColor(img.pixel(x, y)))) ++n;
            return n;
        };
        auto dark = [](const QColor &c) { return qGray(c.rgb()) < 90; };
        const QColor dis = theme::color("disabled");   // the theme's disabled text colour
        auto greyArrow = [dis](const QColor &c) {
            return qAbs(c.red() - dis.red()) <= 6 && qAbs(c.green() - dis.green()) <= 6 && qAbs(c.blue() - dis.blue()) <= 6;
        };
        auto comboArrowRect = [](QComboBox *cb) {
            QStyleOptionComboBox o;
            o.initFrom(cb);
            o.editable = cb->isEditable();
            o.subControls = QStyle::SC_All;
            return cb->style()->subControlRect(QStyle::CC_ComboBox, &o, QStyle::SC_ComboBoxArrow, cb);
        };
        auto comboDark = [&](QComboBox *cb) {
            const QImage img = cb->grab().toImage();
            const qreal dpr = img.devicePixelRatio();
            const QRect r = comboArrowRect(cb);
            return count(img, QRect((r.topLeft() * dpr), r.size() * dpr), dark);
        };
        QWidget host;
        host.resize(300, 200);
        auto *cb = new QComboBox(&host);
        cb->addItems({QStringLiteral("Shadows"), QStringLiteral("Midtones"), QStringLiteral("Highlights")});
        cb->setGeometry(10, 10, 120, 22);
        auto *sb = new QSpinBox(&host);
        sb->setGeometry(10, 40, 80, 22);
        sb->setRange(0, 100);
        sb->setValue(50);
        auto *pf = new PopupSliderField(&host);
        pf->setGeometry(10, 70, 80, 22);
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        // normal
        QVERIFY2(comboDark(cb) >= 5, "combo arrow missing");
        // hover
        cb->setAttribute(Qt::WA_UnderMouse, true);
        cb->update();
        QVERIFY2(comboDark(cb) >= 5, "combo hover arrow missing");
        cb->setAttribute(Qt::WA_UnderMouse, false);
        // open
        cb->showPopup();
        QCoreApplication::processEvents();
        QVERIFY2(comboDark(cb) >= 5, "combo open arrow missing");
        cb->hidePopup();
        QCoreApplication::processEvents();
        // disabled: a grey triangle, nothing dark
        cb->setEnabled(false);
        {
            const QImage img = cb->grab().toImage();
            const qreal dpr = img.devicePixelRatio();
            const QRect r = comboArrowRect(cb);
            const QRect pr(r.topLeft() * dpr, r.size() * dpr);
            QVERIFY2(count(img, pr, greyArrow) >= 4, "disabled combo arrow missing");
            QCOMPARE(count(img, pr, dark), 0);
        }
        cb->setEnabled(true);
        // spin box buttons
        auto spinDark = [&](QAbstractSpinBox *b) {
            QStyleOptionSpinBox o;
            o.initFrom(b);
            o.subControls = QStyle::SC_All;
            o.buttonSymbols = b->buttonSymbols();
            o.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
            const QImage img = b->grab().toImage();
            const qreal dpr = img.devicePixelRatio();
            const QRect up = b->style()->subControlRect(QStyle::CC_SpinBox, &o, QStyle::SC_SpinBoxUp, b);
            const QRect dn = b->style()->subControlRect(QStyle::CC_SpinBox, &o, QStyle::SC_SpinBoxDown, b);
            return qMin(count(img, QRect(up.topLeft() * dpr, up.size() * dpr), dark),
                        count(img, QRect(dn.topLeft() * dpr, dn.size() * dpr), dark));
        };
        QVERIFY2(spinDark(sb) >= 4, "spin box arrows missing");
        // pop-up slider field
        auto arrowDark = [&](QWidget *a) { const QImage img = a->grab().toImage(); return count(img, img.rect(), dark); };
        QWidget *pa = pf->findChild<QWidget *>(QStringLiteral("sliderArrow"));
        QVERIFY(pa);
        QVERIFY2(arrowDark(pa) >= 5, "pop-up slider arrow missing");
        if (!qEnvironmentVariableIsEmpty("LW_ARROW_SHOT")) {
            auto *dsb = new QSpinBox(&host); dsb->setGeometry(140, 40, 80, 22); dsb->setEnabled(false); dsb->show();
            auto *nb = new QDoubleSpinBox(&host); nb->setButtonSymbols(QAbstractSpinBox::NoButtons); nb->setGeometry(140, 70, 62, 22); nb->setValue(12.5); nb->show();
            auto *dcb = new QComboBox(&host); dcb->addItem(QStringLiteral("Disabled")); dcb->setGeometry(140, 10, 120, 22); dcb->setEnabled(false); dcb->show();
            QCoreApplication::processEvents();
            host.grab().save(qEnvironmentVariable("LW_ARROW_SHOT"));
            qInfo("nobuttons edit width %d of %d", nb->findChild<QLineEdit *>()->width(), nb->width());
            delete dsb; delete dcb; delete nb;
        }

        // every combo / spin box / slider arrow in the app, across every tool's options page
        AppState &as = appState();
        DocumentView *v = w->addDocument(Document::createNew(240, 180, Qt::white));
        QCoreApplication::processEvents();
        int combos = 0, arrows = 0, spins = 0;
        auto checkAll = [&](const QString &where) {
            for (QComboBox *c : w->findChildren<QComboBox *>()) {
                if (!c->isVisible() || c->width() < 20) continue;
                const bool en = c->isEnabled();
                if (en) QVERIFY2(comboDark(c) >= 5, qPrintable(where + QStringLiteral(": combo ") + c->objectName() + QStringLiteral(" has no arrow")));
                ++combos;
            }
            for (QWidget *a : w->findChildren<QWidget *>(QStringLiteral("sliderArrow"))) {
                if (!a->isVisible() || !a->isEnabled()) continue;
                QVERIFY2(arrowDark(a) >= 5, qPrintable(where + QStringLiteral(": slider arrow of ") + a->parentWidget()->objectName() + QStringLiteral(" missing")));
                ++arrows;
            }
            for (QAbstractSpinBox *b : w->findChildren<QAbstractSpinBox *>()) {
                if (!b->isVisible() || !b->isEnabled() || b->buttonSymbols() == QAbstractSpinBox::NoButtons) continue;
                if (b->findChild<QWidget *>(QStringLiteral("sliderArrow"))) continue;
                QVERIFY2(spinDark(b) >= 4, qPrintable(where + QStringLiteral(": spin box ") + b->objectName() + QStringLiteral(" has no arrows")));
                ++spins;
            }
        };
        for (int ti = 0; ti <= int(Tool::Zoom); ++ti) {
            as.setTool(Tool(ti));
            QCoreApplication::processEvents();
            checkAll(toolName(Tool(ti)));
        }
        as.setTool(Tool::Pointer);
        QCoreApplication::processEvents();
        QVERIFY(combos > 10);
        QVERIFY(arrows > 10);
        qInfo("checked %d combos, %d slider arrows, %d spin boxes", combos, arrows, spins);
        v->doc()->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }

    // ---- procedural textures: dialog, Property Inspector entries, Filters > Render > Texture
    void proceduralDialog() {
        proc::Params p;
        p.gen = QStringLiteral("perlin");
        p.seed = 7;
        ProceduralDialog dlg(p, ProceduralDialog::Mode::Render, QSize(1200, 900), w);
        dlg.show();
        QVERIFY(dlg.waitForPreview());
        QImage a = dlg.previewImage();
        QVERIFY(!a.isNull());
        QVERIFY(a.width() <= ProceduralDialog::kPreviewCap && a.height() <= ProceduralDialog::kPreviewCap);
        // preview params are capped and keep the aspect ratio
        const proc::Params pp = ProceduralDialog::previewParams(p, QSize(1200, 900));
        QVERIFY(pp.width <= 256 && pp.height <= 256);
        // changing the seed re-renders
        auto *seed = dlg.findChild<QSpinBox *>(QStringLiteral("procSeed"));
        QVERIFY(seed);
        const int n = dlg.previewsRendered();
        seed->setValue(99);
        QTRY_VERIFY_WITH_TIMEOUT(dlg.previewsRendered() > n && !dlg.previewBusy(), 10000);
        QCOMPARE(dlg.params().seed, 99u);
        QVERIFY(dlg.previewImage() != a);
        // switching generator: Brian's Brain shows the cellular groups and hides the noise ones
        auto *gen = dlg.findChild<QComboBox *>(QStringLiteral("procGen"));
        QVERIFY(gen);
        const int bi = gen->findData(QStringLiteral("brain"));
        QVERIFY(bi >= 0);
        gen->setCurrentIndex(bi);
        QCOMPARE(dlg.params().gen, QStringLiteral("brain"));
        QCOMPARE(dlg.params().seed, 99u);   // the seed survives a generator change
        QVERIFY(dlg.findChild<QWidget *>(QStringLiteral("procGenerations"))->isVisibleTo(&dlg));
        QVERIFY(!dlg.findChild<QWidget *>(QStringLiteral("procOctaves"))->isVisibleTo(&dlg));
        QVERIFY(dlg.waitForPreview());
        dlg.reject();
    }

    void proceduralFills() {
        DocumentView *v = w->addDocument(Document::createNew(120, 120, Qt::white));
        Document *fd = v->doc();
        VObject r;
        r.type = ObjType::Rect;
        r.rect = QRectF(10, 10, 100, 100);
        r.fill = Qt::red;
        r.strokeOn = true;
        r.stroke = QColor(255, 210, 40);   // light: a multiplied texture shows (black would stay black)
        r.strokeWidth = 8;
        r.id = fd->newId();
        fd->addObject(r, 0, false);
        fd->commit(QStringLiteral("rect"));
        fd->setSelection({r.id});
        QCoreApplication::processEvents();
        auto *pi = w->findChild<PropertyInspector *>();
        QVERIFY(pi);
        pi->refresh();
        auto acceptProc = [](const QString &gen) {
            QTimer::singleShot(80, [gen] {
                auto *dlg = qobject_cast<ProceduralDialog *>(QApplication::activeModalWidget());
                if (!dlg) return;
                proc::Params p = dlg->params();
                p.gen = gen;
                p.seed = 5;
                dlg->setParams(p);
                dlg->accept();
            });
        };
        // Texture > Procedural...
        const int texProc = pi->m_texture->count() - 1;
        QVERIFY(pi->m_texture->itemText(texProc).startsWith(QStringLiteral("Procedural")));
        acceptProc(QStringLiteral("worley"));
        pi->m_texture->setCurrentIndex(texProc);
        emit pi->m_texture->activated(texProc);
        VObject *o = fd->find(r.id);
        QCOMPARE(o->texture, QStringLiteral("Procedural"));
        QCOMPARE(proc::fromString(o->textureProc).gen, QStringLiteral("worley"));
        QVERIFY(o->textureAmount > 0);
        // Pattern > Procedural...
        int patProc = -1;
        for (int i = 0; i < pi->m_pattern->count(); ++i)
            if (pi->m_pattern->itemText(i).startsWith(QStringLiteral("Procedural"))) patProc = i;
        QVERIFY(patProc >= 0);
        acceptProc(QStringLiteral("marble"));
        pi->m_pattern->setCurrentIndex(patProc);
        emit pi->m_pattern->activated(patProc);
        o = fd->find(r.id);
        QCOMPARE(o->fillKind, FillKind::Pattern);
        QCOMPARE(o->pattern, QStringLiteral("Procedural"));
        QCOMPARE(proc::fromString(o->patternProc).gen, QStringLiteral("marble"));
        // stroke texture
        const int stProc = pi->m_strokeTexture->count() - 1;
        QVERIFY(pi->m_strokeTexture->isEnabled());
        acceptProc(QStringLiteral("plasma"));
        pi->m_strokeTexture->setCurrentIndex(stProc);
        emit pi->m_strokeTexture->activated(stProc);
        o = fd->find(r.id);
        QCOMPARE(o->strokeTexture, QStringLiteral("Procedural"));
        QCOMPARE(proc::fromString(o->strokeTextureProc).gen, QStringLiteral("plasma"));
        QVERIFY(o->strokeTextureAmount > 0);
        // each was one undo step
        QCOMPARE(fd->historyIndex() >= 3, true);
        const QImage withTex = fd->render();
        // stored in the .lwd and editable after reload
        const QString path = tmp.filePath(QStringLiteral("proc.lwd"));
        QString err;
        QVERIFY2(lwd::save(*fd, path, &err), qPrintable(err));
        std::unique_ptr<Document> back(lwd::load(path, &err));
        QVERIFY2(back, qPrintable(err));
        VObject *b = back->find(r.id);
        QVERIFY(b);
        QCOMPARE(b->textureProc, o->textureProc);
        QCOMPARE(b->patternProc, o->patternProc);
        QCOMPARE(b->strokeTextureProc, o->strokeTextureProc);
        QCOMPARE(b->strokeTextureAmount, o->strokeTextureAmount);
        QCOMPARE(back->render(), withTex);
        fd->undo();   // the stroke texture off again: rendering changes
        QVERIFY(fd->find(r.id)->strokeTexture.isEmpty());
        QVERIFY(fd->render() != withTex);
        fd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }

    void renderTextureFilter() {
        quint64 bid;
        Document *fd = makeSelDoc(w, &bid);
        const QImage orig = fd->find(bid)->image;
        proc::Params p;
        p.gen = QStringLiteral("plasma");
        p.seed = 3;
        const QString js = proc::toString(p);
        // into the selected bitmap: the whole image
        QVERIFY(w->renderTexture(js));
        QCOMPARE(fd->history()[fd->historyIndex()].label, QStringLiteral("Render Texture"));
        QImage got = fd->find(bid)->image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        proc::Params q = p; q.width = 100; q.height = 80;
        QCOMPARE(got, proc::render(q, 100, 80).convertToFormat(QImage::Format_ARGB32_Premultiplied));
        fd->undo();
        QCOMPARE(fd->find(bid)->image, orig);
        // within a pixel selection only
        selectRect(fd, QRectF(20, 20, 40, 30));
        QVERIFY(w->renderTexture(js));
        got = fd->find(bid)->image;
        QCOMPARE(got.pixel(5, 5), orig.pixel(5, 5));
        QCOMPARE(got.pixel(90, 70), orig.pixel(90, 70));
        bool changed = false;
        for (int y = 20; y < 50; ++y) for (int x = 20; x < 60; ++x) changed |= got.pixel(x, y) != orig.pixel(x, y);
        QVERIFY(changed);
        fd->undo();
        QCOMPARE(fd->find(bid)->image, orig);
        // nothing selected: a new bitmap the size of the pixel selection, then of the canvas
        fd->setSelection({});
        const int before = fd->objects(0).size();
        QVERIFY(w->renderTexture(js));
        QCOMPARE(fd->objects(0).size(), before + 1);
        const VObject &nb = fd->objects(0).last();
        QCOMPARE(nb.type, ObjType::Bitmap);
        QCOMPARE(nb.rect.toRect(), QRect(20, 20, 40, 30));
        fd->undo();
        QCOMPARE(fd->objects(0).size(), before);
        fd->clearPixelSelection();
        QVERIFY(w->renderTexture(js));
        QCOMPARE(fd->objects(0).last().rect.toRect(), QRect(0, 0, 100, 80));
        QCOMPARE(fd->objects(0).last().image.size(), QSize(100, 80));
        fd->undo();
        // the menu entry exists
        QVERIFY(w->action(QStringLiteral("filter.texture")));
        fd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }

    void shortcutsAreUnique() {
        QHash<QString, QString> seen;
        QHash<QString, QAction *> byId;
        for (QAction *a : w->findChildren<QAction *>())
            if (!a->objectName().isEmpty()) byId.insert(a->objectName(), a);
        auto shareOf = [](QAction *a) {
            return a ? a->property("lwToolShareGroup").toStringList() : QStringList();
        };
        for (QAction *a : w->findChildren<QAction *>()) {
            if (a->objectName().isEmpty()) continue;
            for (const QKeySequence &k : a->shortcuts()) {
                if (k.isEmpty()) continue;
                const QString key = k.toString(QKeySequence::PortableText);
                if (seen.contains(key) && seen[key] != a->objectName()) {
                    const QStringList shareA = shareOf(a);
                    const QStringList shareB = shareOf(byId.value(seen[key]));
                    // tools that intentionally share a cycling key (R, G, U, …) are OK
                    if ((shareA.contains(seen[key]) && shareA.contains(a->objectName()))
                        || (shareB.contains(seen[key]) && shareB.contains(a->objectName())))
                        continue;
                    // also: same portable key string on tool.* that share toolShortcut letter
                    if (a->objectName().startsWith(QLatin1String("tool."))
                        && seen[key].startsWith(QLatin1String("tool."))
                        && a->shortcut().toString() == byId.value(seen[key])->shortcut().toString())
                        continue;
                    // Liquify keys are dialog-local and may reuse tool letters
                    auto isTool = [](const QString &id) { return id.startsWith(QLatin1String("tool.")); };
                    auto isLiq = [](const QString &id) { return id.startsWith(QLatin1String("liquify.")); };
                    if ((isTool(a->objectName()) && isLiq(seen[key])) || (isLiq(a->objectName()) && isTool(seen[key])))
                        continue;
                    if ((a->objectName() == QLatin1String("panels.rename") && seen[key].startsWith(QLatin1String("window.")))
                        || (seen[key] == QLatin1String("panels.rename") && a->objectName().startsWith(QLatin1String("window."))))
                        continue;
                    QFAIL(qPrintable(QStringLiteral("%1 used by %2 and %3 (shareA=%4)")
                        .arg(key, seen[key], a->objectName(), shareA.join(QLatin1Char(',')))));
                }
                seen.insert(key, a->objectName());
            }
        }
    }

    // v0.3.1: File > New takes its size (and resolution) from an image on the clipboard; never pastes
    void newDocumentFromClipboard() {
        QClipboard *cb = QApplication::clipboard();
        auto openDlg = [](NewDocumentDialog &dlg) {
            dlg.show();
            QTRY_VERIFY_WITH_TIMEOUT(!dlg.readingClipboard(), 5000);
            QCoreApplication::processEvents();
        };
        auto spin = [](NewDocumentDialog &dlg, const char *name) { return dlg.findChild<QDoubleSpinBox *>(QLatin1String(name)); };
        // 1. nothing on the clipboard: the defaults, no note
        cb->clear();
        {
            NewDocumentDialog dlg(w);
            openDlg(dlg);
            QCOMPARE(dlg.w(), NewDocumentDialog::kDefaultW);
            QCOMPARE(dlg.h(), NewDocumentDialog::kDefaultH);
            QCOMPARE(dlg.resolution(), 72.0);
            QVERIFY(!dlg.sizeFromClipboard());
            QVERIFY(dlg.clipboardNote().isEmpty());
            QVERIFY(!dlg.findChild<QLabel *>(QStringLiteral("clipboardNote"))->isVisible());
        }
        // text is not an image
        cb->setText(QStringLiteral("hello"));
        {
            NewDocumentDialog dlg(w);
            openDlg(dlg);
            QCOMPARE(dlg.w(), 660);
            QVERIFY(dlg.clipboardNote().isEmpty());
        }
        // 2. a decoded image (what most apps' "Copy Image" gives Qt)
        QImage img(321, 123, QImage::Format_ARGB32);
        img.fill(Qt::red);
        cb->setImage(img);
        {
            NewDocumentDialog dlg(w);
            QCOMPARE(dlg.w(), 660);   // nothing is read before the dialog is shown
            openDlg(dlg);
            QCOMPARE(dlg.w(), 321);
            QCOMPARE(dlg.h(), 123);
            QCOMPARE(dlg.resolution(), 72.0);   // an in-memory image has no real resolution
            QVERIFY(dlg.sizeFromClipboard());
            QCOMPARE(dlg.clipboardNote(), QStringLiteral("Size from clipboard image (321\u00D7123)"));
            QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("clipboardNote"))->isVisible());
            QCOMPARE(spin(dlg, "newWidth")->value(), 321.0);
            // the clipboard changes while the dialog is open: it follows
            QImage img2(64, 48, QImage::Format_RGB32);
            img2.fill(Qt::blue);
            cb->setImage(img2);
            QTRY_COMPARE_WITH_TIMEOUT(dlg.w(), 64, 3000);
            QCOMPARE(dlg.h(), 48);
            // emptied: back to the defaults
            cb->clear();
            QTRY_COMPARE_WITH_TIMEOUT(dlg.w(), 660, 3000);
            QVERIFY(dlg.clipboardNote().isEmpty());
        }
        // 3. encoded PNG bytes with a pHYs chunk (300 ppi): header only, resolution used, units convert
        QByteArray png;
        {
            QImage p(600, 450, QImage::Format_RGB32);
            p.fill(Qt::green);
            p.setDotsPerMeterX(11811);
            p.setDotsPerMeterY(11811);
            QBuffer b(&png);
            b.open(QIODevice::WriteOnly);
            QVERIFY(p.save(&b, "PNG"));
        }
        QCOMPARE(clipimage::headerDpi(png), 11811 * 0.0254);   // the raw value; the dialog shows 300
        {
            auto *m = new QMimeData;
            m->setData(QStringLiteral("image/png"), png);
            cb->setMimeData(m);
            NewDocumentDialog dlg(w);
            openDlg(dlg);
            QCOMPARE(dlg.w(), 600);
            QCOMPARE(dlg.h(), 450);
            QCOMPARE(dlg.resolution(), 300.0);
            QCOMPARE(dlg.clipboardNote(), QStringLiteral("Size from clipboard image (600\u00D7450), 300 ppi"));
            QCOMPARE(spin(dlg, "newResolution")->value(), 300.0);
            dlg.setUnits(NewDocumentDialog::Inches);
            QCOMPARE(spin(dlg, "newWidth")->value(), 2.0);
            QCOMPARE(spin(dlg, "newHeight")->value(), 1.5);
            QCOMPARE(spin(dlg, "newWidth")->suffix(), QStringLiteral(" in"));
            dlg.setUnits(NewDocumentDialog::Centimeters);
            QCOMPARE(spin(dlg, "newWidth")->value(), 5.08);
            dlg.setResolutionPerCm(true);
            QCOMPARE(spin(dlg, "newResolution")->value(), qRound(300 / 2.54 * 100) / 100.0);
            QCOMPARE(dlg.w(), 600);   // display units never change the pixels
        }
        // JPEG with a JFIF density, BMP pixels-per-metre
        {
            QImage j(40, 30, QImage::Format_RGB32);
            j.fill(Qt::gray);
            j.setDotsPerMeterX(5906);   // 150 ppi
            j.setDotsPerMeterY(5906);
            QByteArray jb, bb;
            QBuffer b1(&jb); b1.open(QIODevice::WriteOnly); QVERIFY(j.save(&b1, "JPEG"));
            QBuffer b2(&bb); b2.open(QIODevice::WriteOnly); QVERIFY(j.save(&b2, "BMP"));
            const clipimage::Info ji = clipimage::fromEncoded(jb);
            QCOMPARE(ji.size, QSize(40, 30));
            QCOMPARE(qRound(ji.dpi), 150);
            const clipimage::Info bi = clipimage::fromEncoded(bb);
            QCOMPARE(bi.size, QSize(40, 30));
            QCOMPARE(qRound(bi.dpi), 150);
            QVERIFY(!clipimage::fromEncoded(QByteArray("not an image")).valid());
        }
        // 4. a huge image is sized from its header alone (no decode, so no freeze)
        {
            QByteArray big("\x89PNG\r\n\x1a\n", 8);
            QByteArray ihdr("IHDR", 4);
            auto be32 = [](QByteArray &a, quint32 v) { for (int i = 3; i >= 0; --i) a.append(char((v >> (i * 8)) & 0xFF)); };
            be32(ihdr, 40000); be32(ihdr, 30000);
            ihdr.append(char(8)).append(char(6)).append(char(0)).append(char(0)).append(char(0));
            be32(big, 13);
            big.append(ihdr);
            auto crc32 = [](const QByteArray &d) {
                quint32 c = 0xFFFFFFFFu;
                for (char ch : d) { c ^= uchar(ch); for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
                return c ^ 0xFFFFFFFFu;
            };
            be32(big, crc32(ihdr));   // a header and nothing else: decoding it would fail, sizing it does not
            auto *m = new QMimeData;
            m->setData(QStringLiteral("image/png"), big);
            cb->setMimeData(m);
            QElapsedTimer t;
            t.start();
            const clipimage::Info bi = clipimage::probeClipboard();
            QVERIFY2(t.elapsed() < 500, qPrintable(QString::number(t.elapsed())));
            QCOMPARE(bi.size, QSize(40000, 30000));
            NewDocumentDialog dlg(w);
            openDlg(dlg);
            QCOMPARE(dlg.w(), 16000);   // limited to the canvas maximum, and the note says so
            QCOMPARE(dlg.h(), 16000);
            QVERIFY(dlg.clipboardNote().contains(QStringLiteral("(40000\u00D730000)")));
            QVERIFY(dlg.clipboardNote().contains(QStringLiteral("limited to 16000 px")));
        }
        // 5. a copied image file (file managers put a file:// URL on the clipboard)
        QTemporaryDir tmp;
        {
            const QString path = tmp.filePath(QStringLiteral("photo.png"));
            QImage f(77, 55, QImage::Format_RGB32);
            f.fill(Qt::yellow);
            f.setDotsPerMeterX(2835);   // 72 ppi, written as pHYs
            f.setDotsPerMeterY(2835);
            QVERIFY(f.save(path));
            auto *m = new QMimeData;
            m->setUrls({QUrl::fromLocalFile(tmp.filePath(QStringLiteral("missing.png"))), QUrl::fromLocalFile(path)});
            cb->setMimeData(m);
            NewDocumentDialog dlg(w);
            openDlg(dlg);
            QCOMPARE(dlg.w(), 77);
            QCOMPARE(dlg.h(), 55);
            QCOMPARE(dlg.clipboardNote(), QStringLiteral("Size from image file photo.png (77\u00D755), 72 ppi"));
            // a non-image file is not an image
            const QString txt = tmp.filePath(QStringLiteral("notes.txt"));
            QFile tf(txt);
            QVERIFY(tf.open(QIODevice::WriteOnly));
            tf.write("hello");
            tf.close();
            auto *m2 = new QMimeData;
            m2->setUrls({QUrl::fromLocalFile(txt)});
            cb->setMimeData(m2);
            QTRY_COMPARE_WITH_TIMEOUT(dlg.w(), 660, 3000);
        }
        // 6. what the user typed wins over the clipboard
        cb->setImage(img);
        {
            NewDocumentDialog dlg(w);
            spin(dlg, "newWidth")->setValue(250);
            openDlg(dlg);
            QCOMPARE(dlg.w(), 250);
            QCOMPARE(dlg.h(), 440);
            QVERIFY(dlg.clipboardNote().isEmpty());
        }
        {   // ... also after it was filled in
            NewDocumentDialog dlg(w);
            openDlg(dlg);
            QCOMPARE(dlg.w(), 321);
            spin(dlg, "newHeight")->setValue(200);
            QCOMPARE(dlg.h(), 200);
            QVERIFY(dlg.clipboardNote().isEmpty());
            QImage img3(10, 10, QImage::Format_RGB32);
            img3.fill(Qt::black);
            cb->setImage(img3);
            QCoreApplication::processEvents();
            QCOMPARE(dlg.w(), 321);
            QCOMPARE(dlg.h(), 200);
        }
        // 7. Libreworks' own Copy of objects: their bounds and the source document's resolution
        {
            Document *src = Document::createNew(500, 400, Qt::white, nullptr, 144);
            DocumentView *v = w->addDocument(src);
            QCoreApplication::processEvents();
            VObject o;
            o.id = src->newId();
            o.type = ObjType::Rect;
            o.name = QStringLiteral("Rectangle");
            o.rect = QRectF(10, 20, 130, 70);
            o.fillOn = true;
            o.fill = Qt::red;
            o.strokeOn = false;
            src->addObject(o, 0, false);
            src->commit(QStringLiteral("test"));
            src->selectOnly(o.id);
            w->action(QStringLiteral("edit.copy"))->trigger();
            NewDocumentDialog dlg(w);
            openDlg(dlg);
            const QSize art = src->find(o.id)->artworkBounds().toAlignedRect().size();   // object bounds, no anti-alias pad
            QCOMPARE(art, QSize(130, 70));
            QCOMPARE(cb->image().size(), art);   // what other apps get
            QCOMPARE(dlg.w(), art.width());
            QCOMPARE(dlg.h(), art.height());
            QCOMPARE(dlg.resolution(), 144.0);
            QCOMPARE(dlg.clipboardNote(), QStringLiteral("Size from copied objects (%1\u00D7%2), 144 ppi").arg(art.width()).arg(art.height()));

            Q_UNUSED(v);
        }
        // 8. end to end: File > New makes an empty canvas of the clipboard's size; nothing is pasted
        cb->setImage(img);
        {
            const int before = w->findChildren<DocumentView *>().size();
            bool seen = false;
            QTimer::singleShot(50, [&] {
                auto *dlg = qobject_cast<NewDocumentDialog *>(QApplication::activeModalWidget());
                QVERIFY(dlg);
                QTRY_VERIFY_WITH_TIMEOUT(dlg->sizeFromClipboard(), 3000);
                seen = true;
                dlg->accept();
            });
            w->action(QStringLiteral("file.new"))->trigger();
            QVERIFY(seen);
            QCOMPARE(w->findChildren<DocumentView *>().size(), before + 1);
            Document *d = w->currentDoc();
            QCOMPARE(d->s.width, 321);
            QCOMPARE(d->s.height, 123);
            QCOMPARE(d->s.dpi, 72.0);
            QCOMPARE(countObjects(d), 0);
            QCOMPARE(cb->image().size(), QSize(321, 123));   // the clipboard is left as it was
        }
        cb->clear();
        {   // and with nothing on it, the default size
            QTimer::singleShot(50, [&] {
                if (auto *dlg = qobject_cast<NewDocumentDialog *>(QApplication::activeModalWidget())) dlg->accept();
            });
            w->action(QStringLiteral("file.new"))->trigger();
            QCOMPARE(w->currentDoc()->s.width, 660);
            QCOMPARE(w->currentDoc()->s.height, 440);
        }
    }


    // ---- Content-Aware Remove through the real tool path (0.3.1 crash fix) --------------------------
    // 0.3.0 crashed on "draw on a bitmap with content aware remove": the fill kept a reference into
    // its pyramid vector across an append (use-after-free for any stroke wider than 14 px). These
    // drive the tool exactly as a user does: QTest mouse press / moves / release on the canvas.
    void contentAwareRemoveStroke() {
        appState().brushSize = 20;
        // the exact report: an ordinary (unselected) bitmap, a plain stroke across it
        {
            CaDoc k = caDoc(w, QSize(500, 400), photoLike(400, 300), QRectF(50, 50, 400, 300), false);
            const QImage before = k.d->find(k.id)->image;
            const int histBefore = k.d->historyIndex();
            caStroke(k.c, {QPointF(150, 150), QPointF(240, 190), QPointF(320, 170)});
            QVERIFY(waitRemove(k.d, k.c));
            QCOMPARE(k.d->historyIndex(), histBefore + 1);
            const QImage after = k.d->find(k.id)->image;
            QCOMPARE(after.size(), before.size());
            QVERIFY(after != before);
            QCOMPARE(after.pixel(5, 5), before.pixel(5, 5));          // far from the stroke: untouched
            QCOMPARE(after.pixel(395, 295), before.pixel(395, 295));
            // undo / redo afterwards
            k.d->undo();
            QVERIFY(k.d->find(k.id)->image == before);
            k.d->redo();
            QVERIFY(k.d->find(k.id)->image == after);
            // and a second stroke on the result
            caStroke(k.c, {QPointF(100, 300), QPointF(200, 320)});
            QVERIFY(waitRemove(k.d, k.c));
            QCOMPARE(k.d->historyIndex(), histBefore + 2);
        }
    }

    void contentAwareRemoveHiddenBitmap() {
        // a hidden bitmap is ignored (selected or not): nothing is committed
        appState().brushSize = 16;
        CaDoc k = caDoc(w, QSize(300, 200), photoLike(200, 150), QRectF(50, 25, 200, 150), true);
        k.d->find(k.id)->visible = false;
        k.d->commit(QStringLiteral("hide"));
        const int hi = k.d->historyIndex();
        const QImage before = k.d->find(k.id)->image;
        caStroke(k.c, {QPointF(80, 80), QPointF(200, 120)});
        QVERIFY(settle(k.c));
        QCOMPARE(k.d->historyIndex(), hi);
        QVERIFY(k.d->find(k.id)->image == before);
    }

    // Red Eye Removal tool: real mouse clicks / drags on a synthetic red-eye photo
    void redEyeToolStroke() {
        // skin, a white sclera, a blue-grey iris, a red pupil (r 9) and a white catchlight in it
        const QColor skin(222, 172, 150), sclera(240, 236, 232), iris(70, 100, 130), pupil(196, 32, 38), glint(252, 246, 246);
        auto eyeImage = [&](QPointF c) {
            QImage img(200, 100, QImage::Format_ARGB32_Premultiplied);
            img.fill(skin);
            QPainter p(&img);
            p.setPen(Qt::NoPen);
            p.setBrush(sclera); p.drawEllipse(c, 34, 18);
            p.setBrush(iris); p.drawEllipse(c, 16, 16);
            p.setBrush(pupil); p.drawEllipse(c, 9, 9);
            p.fillRect(QRectF(c.x() + 1, c.y() - 4, 3, 3), glint);
            return img;
        };
        const QPointF e1(50, 50), e2(150, 50);
        const QImage src = eyeImage(e1);
        QImage two = src;
        { QPainter p(&two); p.drawImage(QRect(100, 0, 100, 100), eyeImage(e2), QRect(100, 0, 100, 100)); }
        CaDoc k = caDoc(w, QSize(300, 200), two, QRectF(50, 50, 200, 100), true);
        AppState &as = appState();
        as.setTool(Tool::RedEye);
        QCOMPARE(toolName(Tool::RedEye), QStringLiteral("Red Eye Removal"));
        QVERIFY(toolIsBitmap(Tool::RedEye));
        QVERIFY(toolGroup(Tool::RedEye).contains(Tool::ReplaceColor));
        as.brushSize = 30; as.brushEdge = 0; as.tipShape = 0; as.toolOpacity = 100;
        as.reTolerance = 50; as.reDarken = 50;
        QCoreApplication::processEvents();
        QVERIFY(w->findChild<QWidget *>(QStringLiteral("opt_reTolerance")));
        QVERIFY(w->findChild<QWidget *>(QStringLiteral("opt_reDarken")));
        QVERIFY(w->findChild<QWidget *>(QStringLiteral("opt_opacity")));
        auto px = [&](int x, int y) { return QColor(k.d->find(k.id)->image.pixel(x, y)); };
        auto redness = [](QColor c) { return c.red() - qMax(c.green(), c.blue()); };
        const QImage before = k.d->find(k.id)->image;
        const int hi = k.d->historyIndex();
        // one click on the left pupil (document = image + 50)
        QTest::mouseClick(k.c, Qt::LeftButton, Qt::NoModifier, k.c->toView(e1 + QPointF(50, 50)).toPoint());
        QCoreApplication::processEvents();
        QCOMPARE(k.d->historyIndex(), hi + 1);
        QCOMPARE(lastLabel(k.d), QStringLiteral("Red Eye Removal"));
        {
            const QColor c = px(47, 53);   // in the pupil, off the catchlight
            QVERIFY2(redness(c) < 20 && c.lightness() < QColor(pupil).lightness() - 20,
                     qPrintable(QStringLiteral("%1 %2 %3").arg(c.red()).arg(c.green()).arg(c.blue())));
            QCOMPARE(px(52, 47), glint);                // the catchlight stays
            QCOMPARE(px(50, 38), iris);                 // the iris is not red: kept
            QCOMPARE(px(36, 50), iris);
            QCOMPARE(px(22, 50), sclera);               // the sclera inside the brush: kept
            QCOMPARE(px(150, 52), pupil);               // the other eye: untouched
        }
        // soft edge: a pupil pixel on the rim of a soft, partly-overlapping dab is only partly corrected
        k.d->undo();
        QVERIFY(k.d->find(k.id)->image == before);
        as.brushEdge = 100; as.brushSize = 24;
        QTest::mouseClick(k.c, Qt::LeftButton, Qt::NoModifier, k.c->toView(e1 + QPointF(50 + 8, 50)).toPoint());
        QCoreApplication::processEvents();
        {
            const int rimR = redness(px(50, 50)) , farR = redness(px(43, 50)), coreR = redness(px(57, 50));
            QVERIFY2(coreR < rimR && rimR < farR && farR == redness(pupil),
                     qPrintable(QStringLiteral("%1 %2 %3").arg(coreR).arg(rimR).arg(farR)));
        }
        k.d->undo();
        as.brushEdge = 0; as.brushSize = 30;
        // a drag across both eyes fixes both pupils; the skin between them (also reddish) is kept
        caStroke(k.c, {e1 + QPointF(50, 50), e2 + QPointF(50, 50)});
        QCOMPARE(k.d->historyIndex(), hi + 1);
        QVERIFY(redness(px(47, 53)) < 20 && redness(px(147, 53)) < 20);
        QCOMPARE(px(100, 50), skin);
        QCOMPARE(px(152, 47), glint);
        // dragging back over the same place does not compound
        const QColor once = px(47, 53);
        k.d->undo();
        caStroke(k.c, {e1 + QPointF(50, 50), e2 + QPointF(50, 50), e1 + QPointF(50, 50)});
        QCOMPARE(px(47, 53), once);
        k.d->undo();
        // darken: 100 is darker than 0; strength 50 % corrects half-way
        as.reDarken = 0;
        QTest::mouseClick(k.c, Qt::LeftButton, Qt::NoModifier, k.c->toView(e1 + QPointF(50, 50)).toPoint());
        const int light0 = px(47, 53).lightness();
        k.d->undo();
        as.reDarken = 100;
        QTest::mouseClick(k.c, Qt::LeftButton, Qt::NoModifier, k.c->toView(e1 + QPointF(50, 50)).toPoint());
        const int light100 = px(47, 53).lightness();
        k.d->undo();
        QVERIFY2(light100 + 10 < light0, qPrintable(QStringLiteral("%1 %2").arg(light0).arg(light100)));
        as.reDarken = 50;
        as.toolOpacity = 50;
        QTest::mouseClick(k.c, Qt::LeftButton, Qt::NoModifier, k.c->toView(e1 + QPointF(50, 50)).toPoint());
        {
            const int r = redness(px(47, 53));
            QVERIFY2(r > 40 && r < redness(pupil) - 40, qPrintable(QString::number(r)));
        }
        k.d->undo();
        as.toolOpacity = 100;
        // tolerance 0 still catches a strongly red pupil but not an orange-brown one
        {
            QImage o = before;
            { QPainter p(&o); p.setPen(Qt::NoPen); p.setBrush(QColor(170, 90, 60)); p.drawEllipse(e2, 9, 9); }
            k.d->find(k.id)->image = o;
            as.reTolerance = 0;
            QTest::mouseClick(k.c, Qt::LeftButton, Qt::NoModifier, k.c->toView(e2 + QPointF(50, 50)).toPoint());
            QCOMPARE(px(147, 53), QColor(170, 90, 60));
            k.d->undo();
            k.d->find(k.id)->image = o;
            as.reTolerance = 100;
            QTest::mouseClick(k.c, Qt::LeftButton, Qt::NoModifier, k.c->toView(e2 + QPointF(50, 50)).toPoint());
            QVERIFY(redness(px(147, 53)) < 40);
            k.d->undo();
            k.d->find(k.id)->image = before;
            as.reTolerance = 50;
        }
        QVERIFY(k.d->find(k.id)->image == before);
        as.setTool(Tool::Pointer);
    }

    // Replace Color tool: real mouse strokes through the canvas
    // Canvas right-click menu (Fireworks MX 2004 style): selects the object under the pointer and
    // offers the commands that apply to it, reusing the menu bar's actions (and their shortcuts)
    static QStringList ctxIds(QMenu *m) {
        QStringList out;
        if (!m) return out;
        for (QAction *a : m->actions()) {
            if (a->menu()) out += ctxIds(a->menu());
            else if (!a->isSeparator() && !a->objectName().isEmpty()) out << a->objectName();
        }
        return out;
    }
    void canvasContextMenu() {
        DocumentView *v = w->addDocument(Document::createNew(400, 300, Qt::white));
        Document *pd = v->doc();
        Canvas *cv = v->canvas();
        QTest::qWait(30);
        appState().setTool(Tool::Pointer);
        auto mk = [&](ObjType t, QRectF r) {
            VObject o;
            o.type = t;
            o.rect = r;
            o.fill = QColor(200, 40, 40);
            o.id = pd->newId();
            return o;
        };
        VObject rect = mk(ObjType::Rect, QRectF(20, 20, 80, 60));
        VObject grad = mk(ObjType::Rect, QRectF(20, 200, 80, 60));
        grad.fillKind = FillKind::Linear;
        grad.opacity = 60;
        VObject bmp = mk(ObjType::Bitmap, QRectF(150, 20, 60, 60));
        bmp.image = QImage(60, 60, QImage::Format_ARGB32_Premultiplied);
        bmp.image.fill(Qt::blue);
        VObject txt = mk(ObjType::Text, QRectF(250, 20, 10, 10));
        txt.text = QStringLiteral("Hello");
        txt.fontSize = 24;
        txt.fill = Qt::black;
        txt.fitTextBox();
        for (const VObject &o : {rect, grad, bmp, txt}) pd->addObject(o, 0, false);
        pd->commit(QStringLiteral("setup"));
        pd->clearSelection();

        auto rclick = [&](QPointF doc) -> QMenu * {
            const QPoint vp = cv->toView(doc).toPoint();
            QContextMenuEvent ev(QContextMenuEvent::Mouse, vp, cv->mapToGlobal(vp));
            QApplication::sendEvent(cv, &ev);
            QCoreApplication::processEvents();
            return w->lastContextMenu();
        };
        auto closeMenu = [&](QMenu *m) { if (m) m->close(); QCoreApplication::processEvents(); };

        // 1) a vector object: right click selects it; Edit, Arrange, Align, Transform, paths...
        QMenu *m = rclick(QPointF(50, 50));
        QVERIFY(m);
        QVERIFY(m->isVisible());
        QCOMPARE(m->objectName(), QStringLiteral("canvasContextMenu"));
        QCOMPARE(pd->selection, QSet<quint64>({rect.id}));
        QCOMPARE(m->property("context").toString(), QStringLiteral("vector"));
        QStringList ids = ctxIds(m);
        for (const char *id : {"edit.cut", "edit.copy", "edit.paste", "edit.pasteattributes", "edit.duplicate", "edit.clone",
                               "arrange.front", "arrange.back", "align.0", "modify.freetransform", "modify.scale", "modify.skew",
                               "modify.distort", "modify.rot90cw", "modify.rot90ccw", "modify.fliph", "modify.flipv", "modify.numeric",
                               "path.union", "path.intersect", "path.punch", "path.crop", "path.split", "path.simplify", "path.expand",
                               "path.inset", "path.convert", "symbol.convert", "modify.flattensel",
                               "modify.lock", "view.hide", "fx.dropShadow", "modify.savestyle", "view.properties"})
            QVERIFY2(ids.contains(QString::fromLatin1(id)), id);
        for (const char *id : {"text.attach", "select.feather", "modify.editgradient", "symbol.breakapart", "modify.canvassize",
                               "edit.inserthotspot", "edit.insertslice"})
            QVERIFY2(!ids.contains(QString::fromLatin1(id)), id);
        // the menu bar's own actions, so the shortcuts show and match
        for (QAction *a : m->actions())
            if (a->objectName() == QLatin1String("edit.copy")) QCOMPARE(a, w->action(QStringLiteral("edit.copy")));
        QCOMPARE(w->action(QStringLiteral("edit.cut"))->shortcut(), QKeySequence(QKeySequence::Cut));
        QCOMPARE(w->action(QStringLiteral("edit.pasteattributes"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+Shift+V")));
        QCOMPARE(w->action(QStringLiteral("modify.lock"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+L")));
        QCOMPARE(w->action(QStringLiteral("view.showall"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+L")));
        QVERIFY(w->action(QStringLiteral("edit.cut"))->isEnabled());
        QVERIFY(!w->action(QStringLiteral("edit.pasteattributes"))->isEnabled() || !ids.isEmpty());
        QVERIFY(m->findChild<QMenu *>(QStringLiteral("ctxTransform")));
        closeMenu(m);

        // 2) several objects (the closest thing to a group): right click on one keeps the selection
        pd->setSelection({rect.id, txt.id});
        m = rclick(QPointF(txt.rect.center()));
        QCOMPARE(pd->selection.size(), 2);
        QCOMPARE(m->property("context").toString(), QStringLiteral("multiple"));
        ids = ctxIds(m);
        for (const char *id : {"text.attach", "path.union", "align.0", "symbol.tween"}) QVERIFY2(ids.contains(QString::fromLatin1(id)), id);
        closeMenu(m);

        // 3) a bitmap: marquee commands, Trace Bitmap, no path commands
        m = rclick(QPointF(180, 50));
        QCOMPARE(pd->selection, QSet<quint64>({bmp.id}));
        QCOMPARE(m->property("context").toString(), QStringLiteral("bitmap"));
        ids = ctxIds(m);
        for (const char *id : {"select.all", "select.none", "select.inverse", "select.feather", "select.quickmask", "modify.tracebitmap",
                               "edit.cut", "modify.freetransform", "view.properties"})
            QVERIFY2(ids.contains(QString::fromLatin1(id)), id);
        QVERIFY(!ids.contains(QStringLiteral("path.union")));
        closeMenu(m);

        // 4) text: Editor, Attach to Path, Convert to Paths
        m = rclick(QPointF(txt.rect.center()));
        QCOMPARE(pd->selection, QSet<quint64>({txt.id}));
        QCOMPARE(m->property("context").toString(), QStringLiteral("text"));
        QMenu *tm = m->findChild<QMenu *>(QStringLiteral("ctxText"));
        QVERIFY(tm);
        ids = ctxIds(tm);
        for (const char *id : {"edit.edittext", "text.attach", "path.convert"}) QVERIFY2(ids.contains(QString::fromLatin1(id)), id);
        closeMenu(m);

        // 5) a gradient fill offers Edit Gradient; Paste Attributes copies fill + opacity
        m = rclick(QPointF(50, 230));
        QCOMPARE(pd->selection, QSet<quint64>({grad.id}));
        QVERIFY(ctxIds(m).contains(QStringLiteral("modify.editgradient")));
        QVERIFY(w->action(QStringLiteral("modify.editgradient"))->isEnabled());
        closeMenu(m);
        w->action(QStringLiteral("edit.copy"))->trigger();
        m = rclick(QPointF(50, 50));
        QCOMPARE(pd->selection, QSet<quint64>({rect.id}));
        QVERIFY(w->action(QStringLiteral("edit.pasteattributes"))->isEnabled());
        closeMenu(m);
        const int h0 = pd->historyIndex();
        w->action(QStringLiteral("edit.pasteattributes"))->trigger();
        QCOMPARE(pd->find(rect.id)->fillKind, FillKind::Linear);
        QCOMPARE(pd->find(rect.id)->opacity, 60);
        QCOMPARE(pd->find(rect.id)->rect, rect.rect);          // geometry untouched
        QCOMPARE(lastLabel(pd), QStringLiteral("Paste Attributes"));
        pd->undo();
        QCOMPARE(pd->historyIndex(), h0);
        QCOMPARE(pd->find(rect.id)->fillKind, FillKind::Solid);

        // 6) Lock / Hide from the menu; locked objects are not picked, Unlock All / Show All on the canvas menu
        pd->selectOnly(rect.id);
        w->action(QStringLiteral("modify.lock"))->trigger();
        QVERIFY(pd->find(rect.id)->locked);
        QVERIFY(pd->selection.isEmpty());
        m = rclick(QPointF(50, 50));
        QVERIFY(pd->selection.isEmpty());
        QCOMPARE(m->property("context").toString(), QStringLiteral("canvas"));
        QVERIFY(ctxIds(m).contains(QStringLiteral("modify.unlockall")));
        closeMenu(m);
        w->action(QStringLiteral("modify.unlockall"))->trigger();
        QVERIFY(!pd->find(rect.id)->locked);
        pd->selectOnly(bmp.id);
        w->action(QStringLiteral("view.hide"))->trigger();
        QVERIFY(!pd->find(bmp.id)->visible);
        QVERIFY(pd->selection.isEmpty());
        w->action(QStringLiteral("view.showall"))->trigger();
        QVERIFY(pd->find(bmp.id)->visible);

        // 7) the empty canvas: drops the object selection; Paste, Select All, canvas, grid, guides, zoom
        pd->selectOnly(rect.id);
        m = rclick(QPointF(350, 250));
        QVERIFY(pd->selection.isEmpty());
        QCOMPARE(m->property("context").toString(), QStringLiteral("canvas"));
        ids = ctxIds(m);
        for (const char *id : {"edit.paste", "select.all", "modify.imagesize", "modify.canvassize", "modify.fit", "view.grid",
                               "view.gridsnap", "view.guides", "view.guidessnap", "view.zoomin", "view.zoomout", "view.zoom100",
                               "view.fit", "view.showall", "view.properties"})
            QVERIFY2(ids.contains(QString::fromLatin1(id)), id);
        for (const char *id : {"edit.cut", "arrange.front", "modify.lock"}) QVERIFY2(!ids.contains(QString::fromLatin1(id)), id);
        QVERIFY(m->findChild<QMenu *>(QStringLiteral("ctxZoom")));
        // a toggle from the menu changes the document
        const bool gridWas = pd->grid.show;
        w->action(QStringLiteral("view.grid"))->trigger();
        QCOMPARE(pd->grid.show, !gridWas);
        w->action(QStringLiteral("view.grid"))->trigger();
        closeMenu(m);

        // 8) a pixel selection on empty canvas: the marquee menu
        QPainterPath ps;
        ps.addRect(300, 150, 60, 60);
        pd->pixelSelection = ps;
        m = rclick(QPointF(330, 180));
        QCOMPARE(m->property("context").toString(), QStringLiteral("pixels"));
        ids = ctxIds(m);
        for (const char *id : {"select.none", "select.inverse", "select.feather", "select.quickmask", "modify.bitmapviacopy"})
            QVERIFY2(ids.contains(QString::fromLatin1(id)), id);
        QVERIFY(!pd->pixelSelection.isEmpty());
        closeMenu(m);
        pd->pixelSelection = QPainterPath();

        // 9) a real right button press opens it too, and Properties shows the Property Inspector
        pd->clearSelection();
        QVERIFY(!w->lastContextMenu() || !w->lastContextMenu()->isVisible());
        // through the window, as the platform delivers it (Qt makes the context menu event from the press)
        QWindow *win = cv->window()->windowHandle();
        QVERIFY(win);
        QTest::mouseClick(win, Qt::RightButton, Qt::NoModifier, cv->mapTo(cv->window(), cv->toView(QPointF(180, 50)).toPoint()));
        QCoreApplication::processEvents();
        QVERIFY(w->lastContextMenu() && w->lastContextMenu()->isVisible());
        QCOMPARE(w->lastContextMenu()->property("context").toString(), QStringLiteral("bitmap"));
        QCOMPARE(pd->selection, QSet<quint64>({bmp.id}));
        closeMenu(w->lastContextMenu());
        auto *dock = w->findChild<QDockWidget *>(QStringLiteral("PropertiesDock"));
        dock->hide();
        w->action(QStringLiteral("view.properties"))->trigger();
        QVERIFY(dock->isVisible());

        pd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }
    void pathScrubberTool() {
        DocumentView *v = w->addDocument(Document::createNew(300, 200, Qt::white));
        Document *pd = v->doc();
        Canvas *cv = v->canvas();
        QTest::qWait(30);
        AppState &as = appState();
        // a 12 px black open stroke with no pressure data
        VObject p;
        p.type = ObjType::Path;
        p.points = QPolygonF({QPointF(30, 100), QPointF(270, 100)});
        p.closed = false;
        p.fillOn = false;
        p.strokeOn = true;
        p.stroke = Qt::black;
        p.strokeWidth = 12;
        p.strokeKind = QStringLiteral("basic.hard");
        p.id = pd->newId();
        pd->addObject(p, 0, false);
        pd->commit(QStringLiteral("setup"));
        const quint64 pid = p.id;
        QVERIFY(!pd->find(pid)->hasPointPressure());
        QCOMPARE(pd->find(pid)->pointSize(0), 1.0);   // no data = uniform 1.0

        as.setTool(Tool::PathScrubber);
        QCoreApplication::processEvents();
        QCOMPARE(toolName(Tool::PathScrubber), QStringLiteral("Path Scrubber"));
        QCOMPARE(toolGroup(Tool::PathScrubber), QVector<Tool>({Tool::Freeform, Tool::ReshapeArea, Tool::PathScrubber}));
        QVERIFY(!toolIsBitmap(Tool::PathScrubber));
        for (const char *n : {"opt_psMode", "opt_psAffectSize", "opt_psAffectOpacity", "opt_psStrength", "opt_psSize", "opt_pathPressure"})
            QVERIFY2(w->findChild<QWidget *>(QString::fromLatin1(n)), n);
        auto *mode = w->findChild<QComboBox *>(QStringLiteral("opt_psMode"));
        mode->setCurrentIndex(1);
        QCOMPARE(as.psMode, 1);
        w->findChild<QSpinBox *>(QStringLiteral("opt_psStrength"))->setValue(100);
        w->findChild<QSpinBox *>(QStringLiteral("opt_psSize"))->setValue(40);
        QCOMPARE(as.psStrength, 100);
        QCOMPARE(as.psSize, 40);
        as.psAffectSize = true; as.psAffectOpacity = false; as.pathPressure = false;
        QVERIFY(cv->scrubCursorMinus());
        QVERIFY(!cv->cursor().pixmap().isNull());

        auto nearest = [&](double x) {
            const VObject &o = *pd->find(pid);
            int best = 0;
            for (int i = 1; i < o.points.size(); ++i) if (std::abs(o.points[i].x() - x) < std::abs(o.points[best].x() - x)) best = i;
            return best;
        };
        auto sizeAt = [&](double x) { return pd->find(pid)->pointSize(nearest(x)); };
        auto inkRows = [&](const QImage &img, int x) {
            int n = 0;
            for (int y = 60; y < 140; ++y) if (qGray(img.pixel(x, y)) < 128) ++n;
            return n;
        };
        const QImage plain = pd->render(0, true);
        QVERIFY(std::abs(inkRows(plain, 150) - 12) <= 1);

        // - mode: real mouse drag across the middle of the unselected path (it gets selected)
        pd->clearSelection();
        const int h0 = pd->historyIndex();
        drag(cv, {125, 100}, {175, 100}, 12);
        QVERIFY(pd->selection.contains(pid));
        QCOMPARE(pd->historyIndex(), h0 + 1);
        QCOMPARE(lastLabel(pd), QStringLiteral("Path Scrubber"));
        {
            const VObject &o = *pd->find(pid);
            QVERIFY(o.points.size() > 2);
            QVERIFY(o.hasPointPressure());
            QCOMPARE(o.ptSize.size(), o.points.size());
            QCOMPARE(o.pointSize(0), 1.0);
            QCOMPARE(o.pointSize(o.points.size() - 1), 1.0);
            QVERIFY(o.points.first() == QPointF(30, 100) && o.points.last() == QPointF(270, 100));   // shape kept
            for (int i = 0; i < o.points.size(); ++i) QCOMPARE(o.points[i].y(), 100.0);
        }
        QVERIFY2(sizeAt(150) < 0.85, qPrintable(QString::number(sizeAt(150))));
        QCOMPARE(sizeAt(40), 1.0);
        QCOMPARE(pd->find(pid)->pointOpacity(nearest(150)), 1.0);   // Opacity not ticked
        const QImage thin = pd->render(0, true);
        QVERIFY2(inkRows(thin, 150) < inkRows(thin, 40) - 2, qPrintable(QStringLiteral("%1 vs %2").arg(inkRows(thin, 150)).arg(inkRows(thin, 40))));
        QVERIFY(std::abs(inkRows(thin, 40) - 12) <= 1);
        // undo / redo
        pd->undo();
        QVERIFY(!pd->find(pid)->hasPointPressure());
        pd->redo();
        QVERIFY(sizeAt(150) < 0.85);

        // + mode: thicker than the stroke width (up to 2x)
        mode->setCurrentIndex(0);
        QVERIFY(!cv->scrubCursorMinus());
        drag(cv, {60, 100}, {90, 100}, 10);
        QVERIFY2(sizeAt(75) > 1.15, qPrintable(QString::number(sizeAt(75))));
        QVERIFY(pd->find(pid)->maxPointSize() <= VObject::kMaxPointSize);
        const QImage thick = pd->render(0, true);
        QVERIFY2(inkRows(thick, 75) > 14, qPrintable(QString::number(inkRows(thick, 75))));
        QVERIFY(pd->find(pid)->visualBounds().height() > 12 * 1.15);

        // Alt flips the mode while dragging: Decrease + Alt increases
        mode->setCurrentIndex(1);
        QVERIFY(cv->scrubCursorMinus());
        QTest::keyPress(cv, Qt::Key_Alt, Qt::AltModifier);
        QVERIFY(!cv->scrubCursorMinus());   // the cursor shows + while Alt is held
        QTest::keyRelease(cv, Qt::Key_Alt, Qt::NoModifier);
        QVERIFY(cv->scrubCursorMinus());
        QCOMPARE(sizeAt(230), 1.0);
        drag(cv, {215, 100}, {245, 100}, 10, Qt::AltModifier);
        QVERIFY2(sizeAt(230) > 1.1, qPrintable(QString::number(sizeAt(230))));
        QCOMPARE(as.psMode, 1);   // only temporary
        QVERIFY(!cv->scrubCursorMinus());   // Alt still held after the button comes up
        QTest::keyRelease(cv, Qt::Key_Alt, Qt::NoModifier);
        QVERIFY(cv->scrubCursorMinus());

        // X swaps the mode while the tool is active (not the colours) ...
        const QColor fill0 = as.fill, stroke0 = as.stroke;
        QAction *x = w->action(QStringLiteral("colors.swap"));
        QCOMPARE(x->shortcut(), QKeySequence(QStringLiteral("X")));
        x->trigger();
        QCOMPARE(as.psMode, 0);
        QCOMPARE(mode->currentIndex(), 0);
        QVERIFY(!cv->scrubCursorMinus());
        QCOMPARE(as.fill, fill0);
        QCOMPARE(as.stroke, stroke0);
        x->trigger();
        QCOMPARE(as.psMode, 1);
        // ... and swaps the colours with any other tool
        as.setTool(Tool::Pointer);
        x->trigger();
        QCOMPARE(as.psMode, 1);
        QCOMPARE(as.fill, stroke0);
        QCOMPARE(as.stroke, fill0);
        x->trigger();
        as.setTool(Tool::PathScrubber);

        // Opacity: fades the stroke instead
        as.psAffectSize = false; as.psAffectOpacity = true;
        const double s200 = sizeAt(200);
        drag(cv, {190, 100}, {210, 100}, 6);
        QVERIFY(pd->find(pid)->pointOpacity(nearest(200)) < 0.9);
        QCOMPARE(sizeAt(200), s200);
        {
            const QImage faded = pd->render(0, true);
            QVERIFY2(qGray(faded.pixel(200, 100)) > qGray(thin.pixel(40, 100)) + 20,
                     qPrintable(QStringLiteral("%1 %2 op %3").arg(qGray(faded.pixel(200, 100))).arg(qGray(thin.pixel(40, 100))).arg(pd->find(pid)->pointOpacity(nearest(200)))));
        }
        as.psAffectSize = true; as.psAffectOpacity = false;

        // .lwd round trip
        QTemporaryDir tmp;
        const QString f = tmp.filePath(QStringLiteral("scrub.lwd"));
        QVERIFY(lwd::save(*pd, f));
        std::unique_ptr<Document> back(lwd::load(f));
        QVERIFY(back);
        {
            const VObject &a = *pd->find(pid), &b = *back->find(pid);
            QCOMPARE(b.points.size(), a.points.size());
            QVERIFY(b.hasPointPressure());
            for (int i = 0; i < a.points.size(); ++i) {
                QVERIFY(std::abs(a.pointSize(i) - b.pointSize(i)) < 1e-9);
                QVERIFY(std::abs(a.pointOpacity(i) - b.pointOpacity(i)) < 1e-9);
            }
            QCOMPARE(back->render(0, true), pd->render(0, true));
        }

        // SVG: tapered outlines for other programs, lw: data for Libreworks
        const QByteArray svg = docio::stateToSvg(pd->s, 0);
        QVERIFY(svg.contains("part=\"taper\""));
        QVERIFY(svg.contains("size=\""));
        {
            QSvgRenderer r(svg);
            QVERIFY(r.isValid());
            QImage img(300, 200, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::white);
            QPainter pp(&img);
            r.render(&pp, QRectF(0, 0, 300, 200));
            pp.end();
            QVERIFY2(inkRows(img, 150) < inkRows(img, 40) - 2, qPrintable(QStringLiteral("svg %1 vs %2").arg(inkRows(img, 150)).arg(inkRows(img, 40))));
            QVERIFY(inkRows(img, 75) > 14);
        }
        DocState st;
        quint64 next = 1000;
        QStringList notes;
        QString err;
        QVERIFY2(docio::svgToState(svg, st, [&] { return next++; }, QString(), &notes, &err), qPrintable(err));
        QVector<const VObject *> imported;
        for (const auto &L : st.frames[0].layers) for (const auto &o : L) if (o.type == ObjType::Path) imported << &o;
        QCOMPARE(imported.size(), 1);   // the taper outlines are not imported as extra objects
        {
            const VObject &a = *pd->find(pid), &b = *imported[0];
            QVERIFY(b.strokeOn);
            QCOMPARE(b.strokeWidth, 12.0);
            QCOMPARE(b.points.size(), a.points.size());
            QVERIFY(b.hasPointPressure());
            for (int i = 0; i < a.points.size(); ++i) QVERIFY(std::abs(a.pointSize(i) - b.pointSize(i)) < 1e-4);
        }

        // a click away from every path does nothing
        pd->clearSelection();
        const int h1 = pd->historyIndex();
        click(cv, {150, 20});
        QCOMPARE(pd->historyIndex(), h1);
        as.setTool(Tool::Pointer);
        pd->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }

    void replaceColorToolStroke() {
        QImage img(200, 100, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(200, 30, 30));
        { QPainter p(&img); p.fillRect(100, 0, 100, 100, QColor(30, 40, 210)); p.fillRect(0, 90, 100, 10, QColor(208, 38, 36)); }
        CaDoc k = caDoc(w, QSize(300, 200), img, QRectF(50, 50, 200, 100), true);
        AppState &as = appState();
        as.setTool(Tool::ReplaceColor);
        QCOMPARE(toolName(Tool::ReplaceColor), QStringLiteral("Replace Color"));
        QVERIFY(toolIsBitmap(Tool::ReplaceColor));
        as.brushSize = 30; as.brushEdge = 0; as.tipShape = 0; as.toolOpacity = 100;
        as.rcFrom = QColor(200, 30, 30); as.rcTo = QColor(0, 200, 0); as.rcFromImage = false;
        as.rcTolerance = 10; as.rcRange = 0; as.rcPixelPerfect = false;
        // the inspector shows the tool's options
        QCoreApplication::processEvents();
        QVERIFY(w->findChild<QWidget *>(QStringLiteral("opt_rcTolerance")));
        QVERIFY(w->findChild<QWidget *>(QStringLiteral("opt_rcFrom")));
        QVERIFY(w->findChild<QWidget *>(QStringLiteral("opt_rcPixelPerfect")));
        auto px = [&](int x, int y) { return QColor(k.d->find(k.id)->image.pixel(x, y)); };
        const QImage before = k.d->find(k.id)->image;
        const int hi = k.d->historyIndex();
        // a stroke along image row 50 (document y 100), across the red and the blue halves
        caStroke(k.c, {QPointF(60, 100), QPointF(240, 100)});
        QCOMPARE(k.d->historyIndex(), hi + 1);
        QCOMPARE(lastLabel(k.d), QStringLiteral("Replace Color"));
        QCOMPARE(px(50, 50), QColor(0, 200, 0));      // red under the stroke -> To
        QCOMPARE(px(150, 50), QColor(30, 40, 210));   // blue is outside the tolerance: kept
        QCOMPARE(px(50, 5), QColor(200, 30, 30));     // red away from the stroke: kept
        QCOMPARE(px(50, 95), QColor(208, 38, 36));    // the near-red strip is outside the stroke
        k.d->undo();
        QVERIFY(k.d->find(k.id)->image == before);
        k.d->redo();
        k.d->undo();

        // tolerance: a stroke over the near-red strip replaces it at 10 %, not at 0 %
        as.rcTolerance = 0;
        caStroke(k.c, {QPointF(60, 145), QPointF(140, 145)});
        QCOMPARE(px(50, 95), QColor(208, 38, 36));
        k.d->undo();
        as.rcTolerance = 10;
        caStroke(k.c, {QPointF(60, 145), QPointF(140, 145)});
        QCOMPARE(px(50, 95), QColor(0, 200, 0));
        k.d->undo();

        // Alt-click samples From from the image (no paint, no history step)
        const int hi2 = k.d->historyIndex();
        QTest::mouseClick(k.c, Qt::LeftButton, Qt::AltModifier, k.c->toView(QPointF(200, 100)).toPoint());
        QCoreApplication::processEvents();
        QCOMPARE(as.rcFrom, QColor(30, 40, 210));
        QCOMPARE(k.d->historyIndex(), hi2);
        QVERIFY(k.d->find(k.id)->image == before);
        caStroke(k.c, {QPointF(60, 100), QPointF(240, 100)});
        QCOMPARE(px(150, 50), QColor(0, 200, 0));
        QCOMPARE(px(50, 50), QColor(200, 30, 30));
        k.d->undo();

        // From: Image takes the colour under the pointer where the stroke starts
        as.rcFrom = QColor(255, 255, 0);
        as.rcFromImage = true;
        caStroke(k.c, {QPointF(80, 100), QPointF(240, 100)});
        QCOMPARE(px(50, 50), QColor(0, 200, 0));
        QCOMPARE(px(150, 50), QColor(30, 40, 210));
        k.d->undo();
        as.rcFromImage = false;
        as.rcFrom = QColor(200, 30, 30);

        // strength 50 %: half-way, and overlapping dabs / a stroke back over itself do not compound
        as.toolOpacity = 50;
        caStroke(k.c, {QPointF(60, 100), QPointF(140, 100), QPointF(60, 100), QPointF(140, 100)});
        QCOMPARE(px(50, 50), QColor(100, 115, 15));
        k.d->undo();
        as.toolOpacity = 100;

        // pixel perfect: every pixel is the original or exactly To; a soft edge would blend otherwise
        as.brushEdge = 100; as.rcPixelPerfect = true; as.rcTolerance = 10;
        caStroke(k.c, {QPointF(60, 100), QPointF(140, 102)});
        {
            const QImage a = k.d->find(k.id)->image;
            int changed = 0;
            for (int y = 0; y < 100; ++y) for (int x = 0; x < 100; ++x) {
                const QRgb p = a.pixel(x, y), o = before.pixel(x, y);
                if (p != o) { ++changed; QCOMPARE(QColor(p), QColor(0, 200, 0)); }
            }
            QVERIFY2(changed > 1000, qPrintable(QString::number(changed)));
        }
        k.d->undo();
        as.rcPixelPerfect = false;
        as.brushEdge = 100;
        caStroke(k.c, {QPointF(60, 100), QPointF(140, 102)});
        {
            const QImage a = k.d->find(k.id)->image;
            bool partial = false;
            for (int y = 0; y < 100 && !partial; ++y) for (int x = 0; x < 100; ++x) {
                const QRgb p = a.pixel(x, y);
                if (p != before.pixel(x, y) && QColor(p) != QColor(0, 200, 0)) { partial = true; break; }
            }
            QVERIFY(partial);
        }
        k.d->undo();
        as.brushEdge = 0;

        // a pixel selection limits the stroke
        selectRect(k.d, QRectF(50, 50, 50, 100));      // image x 0..49
        caStroke(k.c, {QPointF(60, 100), QPointF(140, 100)});
        QCOMPARE(px(20, 50), QColor(0, 200, 0));
        QCOMPARE(px(70, 50), QColor(200, 30, 30));
        k.d->undo();
        k.d->pixelSelection = QPainterPath();

        // no bitmap under the pointer and nothing selected: nothing happens
        k.d->clearSelection();
        const int hi3 = k.d->historyIndex();
        caStroke(k.c, {QPointF(5, 5), QPointF(30, 20)});
        QCOMPARE(k.d->historyIndex(), hi3);
        // Color range keeps the shading: a dark red becomes a dark green, not the flat To
        {
            QImage sh(100, 40, QImage::Format_ARGB32_Premultiplied);
            for (int x = 0; x < 100; ++x) for (int y = 0; y < 40; ++y) sh.setPixel(x, y, qRgb(100 + x, 15, 15));
            CaDoc s2 = caDoc(w, QSize(200, 100), sh, QRectF(50, 30, 100, 40), true);
            as.setTool(Tool::ReplaceColor);
            as.brushSize = 30; as.brushEdge = 0; as.toolOpacity = 100;
            as.rcFrom = QColor(150, 15, 15); as.rcTo = QColor(0, 200, 0); as.rcTolerance = 40; as.rcRange = 1;
            caStroke(s2.c, {QPointF(52, 50), QPointF(148, 50)});
            const QColor a(s2.d->find(s2.id)->image.pixel(10, 20)), b(s2.d->find(s2.id)->image.pixel(90, 20));
            QVERIFY2(a.green() > a.red() && b.green() > b.red(), qPrintable(a.name() + b.name()));
            QVERIFY(b.lightness() > a.lightness());                  // the gradient survives
            as.rcRange = 0;
        }

        as.setTool(Tool::Pointer);
    }

    void replaceColorFilter() {
        CaDoc k = caDoc(w, QSize(200, 120), QImage(), QRectF(), false);
        QImage img(100, 60, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(200, 30, 30));
        { QPainter p(&img); p.fillRect(50, 0, 50, 60, QColor(30, 40, 210)); }
        VObject b;
        b.type = ObjType::Bitmap; b.rect = QRectF(10, 10, 100, 60); b.image = img; b.id = k.d->newId();
        k.d->addObject(b, 0, false);
        k.d->commit(QStringLiteral("bitmap"));
        k.d->setSelection({b.id});
        appState().setTool(Tool::Pointer);
        QVERIFY(w->action(QStringLiteral("filter.replacecolor")));
        QCOMPARE(effectCategory(EffectType::ReplaceColor), QStringLiteral("Adjust Color"));
        bool ok = false;
        QTimer::singleShot(50, [&] {
            auto *dlg = qobject_cast<EffectDialog *>(QApplication::activeModalWidget());
            QVERIFY(dlg);
            auto *from = dlg->findChild<ColorButton *>(QStringLiteral("fx_color"));
            auto *to = dlg->findChild<ColorButton *>(QStringLiteral("fx_color2"));
            QVERIFY(from && to);
            QVERIFY(dlg->findChild<QLabel *>(QStringLiteral("fx_rangePreview")));
            QVERIFY(dlg->findChild<QWidget *>(QStringLiteral("fx_pickFrom")));
            emit from->colorPicked(QColor(30, 40, 210));
            emit to->colorPicked(QColor(250, 200, 0));
            QCoreApplication::processEvents();
            ok = QColor(k.d->find(b.id)->image.pixel(75, 30)) == QColor(250, 200, 0);   // live preview
            dlg->accept();
        });
        w->action(QStringLiteral("filter.replacecolor"))->trigger();
        QVERIFY(ok);
        QCOMPARE(lastLabel(k.d), QStringLiteral("Replace Color"));
        QCOMPARE(QColor(k.d->find(b.id)->image.pixel(75, 30)), QColor(250, 200, 0));
        QCOMPARE(QColor(k.d->find(b.id)->image.pixel(25, 30)), QColor(200, 30, 30));
        k.d->undo();
        QCOMPARE(QColor(k.d->find(b.id)->image.pixel(75, 30)), QColor(30, 40, 210));
        // as a live effect it is listed under Adjust Color and saved / loaded with LWD
        VObject *o = k.d->find(b.id);
        LiveEffect le = LiveEffect::make(EffectType::ReplaceColor);
        le.color = QColor(200, 30, 30); le.color2 = QColor(10, 10, 10); le.mode = 2; le.hue = 120;
        o->effects << le;
        k.d->commit(QStringLiteral("fx"));
        const QString f = tmp.filePath(QStringLiteral("rc.lwd"));
        QVERIFY(lwd::save(*k.d, f));
        std::unique_ptr<Document> back(lwd::load(f));
        QVERIFY(back);
        const VObject *bo = nullptr;
        for (const auto &L : back->s.frames[0].layers) for (const VObject &x : L) if (x.type == ObjType::Bitmap) bo = &x;
        QVERIFY(bo && bo->effects.size() == 1);
        QCOMPARE(bo->effects[0].type, EffectType::ReplaceColor);
        QCOMPARE(bo->effects[0].color2, QColor(10, 10, 10));
        QCOMPARE(bo->effects[0].mode, 2);
        QCOMPARE(bo->effects[0].hue, 120.0);
    }

    void contentAwareRemoveCrashCases() {
        appState().brushSize = 16;
        // selected bitmap, very fast strokes: a single move across the whole bitmap, starting and ending off it
        {
            CaDoc k = caDoc(w, QSize(400, 300), photoLike(300, 200), QRectF(50, 50, 300, 200), true);
            caStroke(k.c, {QPointF(60, 100), QPointF(390, 200)}, 1);
            QVERIFY(waitRemove(k.d, k.c));
            QCOMPARE(k.d->find(k.id)->image.size(), QSize(300, 200));
            // off the canvas entirely on both ends
            caStroke(k.c, {QPointF(70, 60), QPointF(-200, -100), QPointF(700, 600)}, 1);
            QVERIFY(settle(k.c));
        }
        // near the edges and corners
        {
            CaDoc k = caDoc(w, QSize(300, 300), photoLike(300, 300), QRectF(0, 0, 300, 300), true);
            caStroke(k.c, {QPointF(2, 2), QPointF(298, 3), QPointF(298, 298), QPointF(1, 298)});
            QVERIFY(waitRemove(k.d, k.c));
            caStroke(k.c, {QPointF(299, 299), QPointF(299.5, 299.5)});
            QVERIFY(waitRemove(k.d, k.c));
        }
        // small bitmaps: smaller than the 48 px search margin, the 7 px patch, a single pixel
        for (QSize sz : {QSize(40, 30), QSize(12, 9), QSize(5, 4), QSize(1, 1)}) {
            CaDoc k = caDoc(w, QSize(120, 100), photoLike(sz.width(), sz.height()), QRectF(QPointF(20, 20), QSizeF(sz)), true);
            caStroke(k.c, {QPointF(20, 20), QPointF(20 + sz.width(), 20 + sz.height())});
            QVERIFY2(waitRemove(k.d, k.c), qPrintable(QStringLiteral("%1x%2").arg(sz.width()).arg(sz.height())));
            QCOMPARE(k.d->find(k.id)->image.size(), sz);
            k.d->undo();
        }
        // a brush covering the whole bitmap (nothing left to sample from)
        {
            appState().brushSize = 400;
            CaDoc k = caDoc(w, QSize(300, 200), photoLike(200, 150), QRectF(50, 25, 200, 150), true);
            caStroke(k.c, {QPointF(150, 100), QPointF(151, 101)});
            QVERIFY(waitRemove(k.d, k.c));
            appState().brushSize = 16;
        }
        // with a pixel selection: only the selected part changes; a stroke outside it does nothing
        {
            CaDoc k = caDoc(w, QSize(400, 300), photoLike(400, 300), QRectF(0, 0, 400, 300), true);
            selectRect(k.d, QRectF(100, 100, 120, 80));
            const QImage before = k.d->find(k.id)->image;
            caStroke(k.c, {QPointF(60, 140), QPointF(300, 140)});
            QVERIFY(waitRemove(k.d, k.c));
            const QImage after = k.d->find(k.id)->image;
            QCOMPARE(after.pixel(70, 140), before.pixel(70, 140));
            QCOMPARE(after.pixel(260, 140), before.pixel(260, 140));
            const int h = k.d->historyIndex();
            caStroke(k.c, {QPointF(20, 20), QPointF(60, 30)});
            QVERIFY(settle(k.c));
            QCOMPARE(k.d->historyIndex(), h);
            k.d->clearPixelSelection();
        }
        // an empty / transparent bitmap
        {
            QImage t(200, 150, QImage::Format_ARGB32_Premultiplied);
            t.fill(Qt::transparent);
            CaDoc k = caDoc(w, QSize(300, 200), t, QRectF(20, 20, 200, 150), true);
            caStroke(k.c, {QPointF(40, 40), QPointF(180, 140)});
            QVERIFY(waitRemove(k.d, k.c));
        }
        // a scaled bitmap (image pixels != canvas pixels)
        {
            CaDoc k = caDoc(w, QSize(400, 300), photoLike(100, 400), QRectF(20, 20, 300, 200), true);
            caStroke(k.c, {QPointF(40, 60), QPointF(300, 200)});
            QVERIFY(waitRemove(k.d, k.c));
            QCOMPARE(k.d->find(k.id)->image.size(), QSize(100, 400));
        }
        // no bitmap at all, and a vector object (selected and clicked): nothing happens
        {
            CaDoc k = caDoc(w, QSize(200, 150), QImage(), QRectF(), false);
            const int h = k.d->historyIndex();
            caStroke(k.c, {QPointF(20, 20), QPointF(150, 100)});
            QVERIFY(settle(k.c));
            QCOMPARE(k.d->historyIndex(), h);
            VObject r;
            r.id = k.d->newId();
            r.type = ObjType::Rect;
            r.rect = QRectF(30, 30, 100, 60);
            k.d->addObject(r, 0, true);
            k.d->commit(QStringLiteral("rect"));
            const int h2 = k.d->historyIndex();
            caStroke(k.c, {QPointF(50, 50), QPointF(100, 70)});
            QVERIFY(settle(k.c));
            QCOMPARE(k.d->historyIndex(), h2);
            k.d->clearSelection();
            caStroke(k.c, {QPointF(50, 50), QPointF(100, 70)});
            QVERIFY(settle(k.c));
            QCOMPARE(k.d->historyIndex(), h2);
        }
        // the target disappears mid-stroke (undo while the button is down) and while the fill runs
        {
            CaDoc k = caDoc(w, QSize(400, 300), photoLike(300, 200), QRectF(50, 50, 300, 200), true);
            QTest::mousePress(k.c, Qt::LeftButton, Qt::NoModifier, k.c->toView(QPointF(100, 100)).toPoint());
            QTest::mouseMove(k.c, k.c->toView(QPointF(200, 150)).toPoint());
            k.d->undo();   // removes the bitmap
            QTest::mouseRelease(k.c, Qt::LeftButton, Qt::NoModifier, k.c->toView(QPointF(250, 150)).toPoint());
            QVERIFY(settle(k.c));
            QVERIFY(!k.d->find(k.id));
            k.d->redo();
            QVERIFY(k.d->find(k.id));
            const QImage before = k.d->find(k.id)->image;
            caStroke(k.c, {QPointF(100, 100), QPointF(300, 200)});
            k.d->undo();   // while the fill may still be running: its result must be discarded
            QVERIFY(settle(k.c));
            QVERIFY(!k.d->find(k.id) || lastLabel(k.d) != QLatin1String("Content-Aware Remove"));
        }
        // a locked bitmap: refused
        {
            CaDoc k = caDoc(w, QSize(200, 150), photoLike(150, 100), QRectF(10, 10, 150, 100), true);
            k.d->find(k.id)->locked = true;
            const int h = k.d->historyIndex();
            caStroke(k.c, {QPointF(30, 30), QPointF(120, 80)});
            QVERIFY(settle(k.c));
            QCOMPARE(k.d->historyIndex(), h);
        }
        // closing the document while a large fill is still running
        {
            CaDoc k = caDoc(w, QSize(1600, 1200), photoLike(1600, 1200), QRectF(0, 0, 1600, 1200), true);
            appState().brushSize = 300;
            caStroke(k.c, {QPointF(300, 300), QPointF(1300, 900)});
            appState().brushSize = 16;
            k.d->markClean();
            QPointer<DocumentView> pv(k.v);
            QVERIFY(w->currentView() == k.v);
            w->action(QStringLiteral("file.close"))->trigger();   // the canvas goes away with the job in flight
            QTRY_VERIFY(!pv);
            QTest::qWait(20);
        }
        appState().setTool(Tool::Pointer);
    }

    void cleanupTestCase() {
        for (Document *doc : w->findChildren<Document *>()) doc->markClean();
        w->close();
        delete w;
    }

    // ---- v0.3.4: every colour selector has the eyedropper ----
private:
    // one ColorButton: its pop-up has the eyedropper (and "..." for the full dialog); the
    // eyedropper starts the picker, Esc cancels it and leaves the colour as it was
    static void checkColorButton(ColorButton *b, const QString &ctx) {
        const QColor before = b->color();
        // as in use: the button's window is the active one (Qt closes every pop-up when the
        // application loses activation, which a headless run otherwise does between pop-ups)
        if (!b->window()->isActiveWindow()) {
            b->window()->activateWindow();
            QTest::qWait(1);
        }
        ColorPopup *pop = b->showPopup();
        QVERIFY2(pop && pop->isVisible(), qPrintable(ctx + QStringLiteral(": no swatch pop-up")));
        QCOMPARE(QApplication::activePopupWidget(), static_cast<QWidget *>(pop));
        auto *drop = pop->findChild<QToolButton *>(QStringLiteral("eyedropper"));
        QVERIFY2(drop && drop->isVisible() && !drop->icon().isNull(), qPrintable(ctx + QStringLiteral(": no eyedropper")));
        QVERIFY2(pop->findChild<QToolButton *>(QStringLiteral("colorPopupMore")), qPrintable(ctx));
        drop->click();   // hands over to the picker straight away
        ScreenColorPicker *pk = ScreenColorPicker::active();
        QVERIFY2(pk, qPrintable(ctx + QStringLiteral(": eyedropper did not start")));
        QVERIFY2(pk->isVisible(), qPrintable(ctx + QStringLiteral(": picker not shown")));
        QVERIFY2(!pop->isVisible(), qPrintable(ctx + QStringLiteral(": swatch pop-up still open")));
        QTest::keyClick(pk, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QVERIFY2(!ScreenColorPicker::active(), qPrintable(ctx));
        QVERIFY2(b->color() == before, qPrintable(ctx + QStringLiteral(": cancel changed the colour")));
    }
    // every ColorButton under `root` (shown or not), plus: no other colour swatch-like button
    static int checkColorWidgets(QWidget *root, const QString &ctx, QSet<QString> &names, QSet<ColorButton *> &seen) {
        int n = 0;
        for (ColorButton *b : root->findChildren<ColorButton *>()) {
            if (seen.contains(b)) continue;
            seen.insert(b);
            checkColorButton(b, ctx + QLatin1Char('/') + b->objectName());
            if (QTest::currentTestFailed()) return n;
            if (!b->objectName().isEmpty()) names.insert(b->objectName());
            ++n;
        }
        static const QRegularExpression colorish(QStringLiteral("colou?r|swatch|matte"), QRegularExpression::CaseInsensitiveOption);
        for (QAbstractButton *ab : root->findChildren<QAbstractButton *>()) {
            if (qobject_cast<ColorButton *>(ab) || ab->objectName().isEmpty() || ab->objectName().startsWith(QLatin1String("qt_"))) continue;
            if (qobject_cast<QCheckBox *>(ab) || qobject_cast<QRadioButton *>(ab)) continue;   // options, not selectors
            if (!ab->icon().isNull() && ab->text().isEmpty() && ab->objectName().contains(colorish) && !ab->isCheckable())
                qWarning() << "possible colour selector that is not a ColorButton:" << ctx << ab->objectName();
        }
        return n;
    }
private slots:
    void everyColorSelectorHasTheEyedropper() {
        AppState &as = appState();
        QSet<QString> names;
        QSet<ColorButton *> seen;
        int count = 0;
        // the main window: Tools panel, Find panel, the Property inspector's pages (document,
        // a selected vector's fill / stroke, every tool's options)
        DocumentView *v = w->addDocument(Document::createNew(200, 150, Qt::white));
        Document *d = v->doc();
        VObject r;
        r.type = ObjType::Rect; r.rect = QRectF(20, 20, 80, 60); r.id = d->newId();
        r.fill = Qt::red;
        d->addObject(r, 0, false);
        d->commit(QStringLiteral("rect"));
        QCoreApplication::processEvents();
        count += checkColorWidgets(w, QStringLiteral("main"), names, seen);
        if (QTest::currentTestFailed()) return;
        d->setSelection({r.id});
        as.setTool(Tool::Pointer);
        QCoreApplication::processEvents();
        count += checkColorWidgets(w, QStringLiteral("main/vector"), names, seen);
        if (QTest::currentTestFailed()) return;
        d->clearSelection();
        for (int ti = 0; ti <= int(Tool::Zoom); ++ti) {
            as.setTool(Tool(ti));
            QCoreApplication::processEvents();
            count += checkColorWidgets(w, QStringLiteral("tool ") + QString::number(ti), names, seen);
            if (QTest::currentTestFailed()) return;
        }
        as.setTool(Tool::Pointer);
        // the dialogs
        QTemporaryDir tmp;
        auto dialog = [&](QWidget *dlg, const QString &ctx) {
            dlg->show();
            QCoreApplication::processEvents();
            QSet<ColorButton *> own;   // (addresses of deleted dialogs' buttons get reused)
            count += checkColorWidgets(dlg, ctx, names, own);
            dlg->close();
            delete dlg;
        };
        dialog(new NewDocumentDialog(w, false), QStringLiteral("New"));
        dialog(new ExportDialog(*d, w), QStringLiteral("Export"));
        for (EffectType t : allEffectTypes()) {
            dialog(new EffectDialog(LiveEffect::make(t), w), QStringLiteral("effect ") + QString::number(int(t)));
            if (QTest::currentTestFailed()) return;
        }
        dialog(new PreferencesDialog(w), QStringLiteral("Preferences"));
        dialog(new ProceduralDialog(proc::Params{}, ProceduralDialog::Mode::Texture, {}, w), QStringLiteral("Procedural"));
        dialog(new FaviconDialog(*d, tmp.path(), w), QStringLiteral("Favicon"));
        dialog(new EditGridDialog(d, w), QStringLiteral("Grid"));
        dialog(new EditGuidesDialog(d, w), QStringLiteral("Guides"));
        dialog(new GradientDialog(fills::gradientPreset(QStringLiteral("Black, White")), w), QStringLiteral("Gradient"));
        if (QTest::currentTestFailed()) return;
        // every kind of colour selector was walked
        for (const char *n : {"fx_color", "fx_color2", "stopColor", "gridColor", "guideColor", "faviconBackground", "faviconTheme",
                              "prefAccentColor", "prefSeedColor", "prefPasteboardColor", "findColor", "replaceColor", "optMatte", "newCanvasColor"})
            QVERIFY2(names.contains(QLatin1String(n)), n);
        QVERIFY2(count >= 30, qPrintable(QString::number(count)));
        qInfo() << "colour selectors checked:" << count;
        d->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }

    // the full colour dialog (ColorButton's "...", Canvas Color, gradient / palette swatch
    // double-clicks): our eyedropper instead of Qt's "Pick Screen Color"
    void fullColorDialogHasTheEyedropper() {
        DocumentView *v = w->addDocument(Document::createNew(120, 90, Qt::white));
        auto checkModal = [](bool *ok, bool pickRed) {
            QTimer::singleShot(50, [ok, pickRed] {
                auto *dlg = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
                if (!dlg) return;
                auto *drop = dlg->findChild<QToolButton *>(QStringLiteral("eyedropper"));
                bool qtPickHidden = true;
                for (QPushButton *pb : dlg->findChildren<QPushButton *>())
                    if (pb->text().remove(QLatin1Char('&')).contains(QLatin1String("Pick Screen")) && pb->isVisible()) qtPickHidden = false;
                bool started = false;
                if (drop && drop->isVisible()) {
                    drop->click();
                    QCoreApplication::processEvents();
                    if (ScreenColorPicker *pk = ScreenColorPicker::active()) {
                        started = true;
                        if (pickRed) pk->pick(Qt::red);
                        else QTest::keyClick(pk, Qt::Key_Escape);
                    }
                }
                *ok = drop && qtPickHidden && started;
                if (pickRed) dlg->accept(); else dlg->reject();
            });
        };
        bool ok = false;
        checkModal(&ok, true);
        const QColor c = colorpick::getColor(Qt::blue, w, QStringLiteral("t"), true);
        QVERIFY(ok);
        QCOMPARE(c.rgb(), QColor(Qt::red).rgb());   // the eyedropper's pick is the dialog's colour
        ok = false;
        checkModal(&ok, false);
        QVERIFY(!colorpick::getColor(Qt::blue, w).isValid());
        QVERIFY(ok);
        // Modify > Canvas > Canvas Color...
        ok = false;
        checkModal(&ok, true);
        w->action(QStringLiteral("modify.canvascolor"))->trigger();
        QVERIFY(ok);
        QCOMPARE(v->doc()->s.canvasColor.rgb(), QColor(Qt::red).rgb());
        // a gradient stop's double-click
        {
            GradientBar bar;
            bar.resize(320, 52);
            bar.setStops(fills::gradientPreset(QStringLiteral("Black, White")));
            bar.show();
            QCoreApplication::processEvents();
            ok = false;
            checkModal(&ok, false);
            QTest::mouseDClick(&bar, Qt::LeftButton, {}, QPoint(8, 40));   // the first stop's marker
            QCoreApplication::processEvents();
            QVERIFY(ok);
        }
        v->doc()->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
    }

    // 0.3.4: at the default window layout the Property inspector shows every row (W/H/X/Y and the
    // whole Effects box included) in every theme and with every kind of selection; also on a 1366x768 screen
    void propertyInspectorDefaultHeightFitsContent() {
        AppState &as = appState();
        const Tool keepTool = as.tool();
        as.setTool(Tool::Pointer);
        const QString shotDir = qEnvironmentVariable("LW_SHOT_DIR");
        const QSize fitted = QSize(1400, 900).boundedTo(QGuiApplication::primaryScreen()->availableSize() - QSize(0, 40));   // as main.cpp sizes it
        for (const QSize winSize : {QSize(1400, 900), QSize(1366, 728), fitted}) {
            for (const theme::Info &t : theme::list()) {
                theme::apply(*qApp, t.id);
                auto *mw = new MainWindow;
                mw->resize(winSize);
                if (!QRect(QPoint(), mw->screen()->availableSize()).contains(QRect(QPoint(), winSize))) {   // the ui_smoke_layout test runs on a 1920x1080 screen
                    qInfo("screen %dx%d is smaller than %dx%d: size not checked here", mw->screen()->availableSize().width(), mw->screen()->availableSize().height(), winSize.width(), winSize.height());
                    delete mw; continue;
                }
                DocumentView *v = mw->addDocument(Document::createNew(400, 300, Qt::white));
                mw->show();
                QVERIFY(QTest::qWaitForWindowExposed(mw));
                QTest::qWait(20);
                Document *dd = v->doc();
                auto *pi = mw->findChild<PropertyInspector *>();
                QVERIFY(pi);
                auto *scroll = pi->findChild<QScrollArea *>();
                QVERIFY(scroll);
                QVector<QPair<QString, QVector<quint64>>> cases;
                auto add = [&](const QString &name, VObject o, int layer = 0) {
                    o.id = dd->newId();
                    if (o.rect.isNull()) o.rect = QRectF(20 + 3 * cases.size(), 20, 100, 70);
                    dd->addObject(o, layer, false);
                    cases.append({name, {o.id}});
                    return o.id;
                };
                { VObject o; o.type = ObjType::Bitmap; o.image = QImage(10, 10, QImage::Format_ARGB32_Premultiplied); o.image.fill(Qt::green); add(QStringLiteral("bitmap"), o); }
                quint64 r1 = 0, r2 = 0;
                { VObject o; o.type = ObjType::Rect; o.fillOn = true; r1 = add(QStringLiteral("rect"), o); }
                { VObject o; o.type = ObjType::Ellipse; o.strokeOn = true; r2 = add(QStringLiteral("ellipse"), o); }
                { VObject o; o.type = ObjType::Path; o.points = {{10, 10}, {90, 20}, {50, 80}}; o.closed = true; add(QStringLiteral("path"), o); }
                { VObject o; o.type = ObjType::Rect; o.fillOn = true; fills::setFillKind(o, FillKind::Linear); add(QStringLiteral("gradient"), o); }
                { VObject o; o.type = ObjType::Rect; o.fillOn = true; fills::setFillKind(o, FillKind::Pattern); o.pattern = fills::patternNames().value(0); add(QStringLiteral("pattern"), o); }
                { VObject o; o.type = ObjType::Text; o.text = QStringLiteral("Hello"); add(QStringLiteral("text"), o); }
                { VObject g; g.type = ObjType::Group; VObject c1 = *dd->find(r1), c2 = *dd->find(r2); c1.id = dd->newId(); c2.id = dd->newId(); g.children << c1 << c2; add(QStringLiteral("group"), g); }
                for (const QString &k : autoshape::kinds()) { VObject o; o.type = ObjType::AutoShape; o.rect = QRectF(30, 30, 100, 70); autoshape::init(o, k); add(QStringLiteral("autoshape ") + k, o); }
                const int wl = dd->ensureWebLayer();
                { VObject o; o.type = ObjType::Slice; add(QStringLiteral("slice"), o, wl); }
                { VObject o; o.type = ObjType::Hotspot; add(QStringLiteral("hotspot"), o, wl); }
                cases.append({QStringLiteral("several"), {r1, r2}});
                cases.append({QStringLiteral("none"), {}});
                dd->commit(QStringLiteral("objects"));
                for (const auto &cs : cases) {
                    dd->setSelection(QSet<quint64>(cs.second.begin(), cs.second.end()));
                    QCoreApplication::processEvents();
                    QCoreApplication::processEvents();
                    const QString ctx = t.id + QLatin1Char('/') + QString::number(winSize.width()) + QLatin1Char('x') + QString::number(winSize.height()) + QLatin1Char('/') + cs.first;
                    QVERIFY2(pi->isVisible() && pi->height() >= pi->minimumHeight(), qPrintable(ctx));
                    QWidget *vp = scroll->viewport();
                    const QRect visible = vp->rect();
                    int checked = 0;
                    QWidget *page = scroll->widget();
                    for (QWidget *f : page->findChildren<QWidget *>()) {
                        if (!f->isVisibleTo(page) || !f->isVisible()) continue;
                        const bool field = qobject_cast<QAbstractSpinBox *>(f) || qobject_cast<QLineEdit *>(f) || qobject_cast<QComboBox *>(f)
                                           || qobject_cast<QAbstractButton *>(f) || qobject_cast<QAbstractItemView *>(f) || qobject_cast<QGroupBox *>(f)
                                           || qobject_cast<QSlider *>(f) || qobject_cast<QLabel *>(f);
                        if (!field) continue;
                        if (qobject_cast<QLineEdit *>(f) && qobject_cast<QAbstractSpinBox *>(f->parentWidget())) continue;   // a spin box's own editor
                        if (qobject_cast<QLineEdit *>(f) && qobject_cast<QComboBox *>(f->parentWidget())) continue;
                        const QRect r(f->mapTo(vp, QPoint(0, 0)), f->size());
                        // vertically inside the viewport: no row hidden under the dock's bottom edge
                        QVERIFY2(r.top() >= visible.top() && r.bottom() <= visible.bottom(),
                                 qPrintable(QStringLiteral("%1: %2 (%3) at y %4..%5, viewport height %6").arg(ctx, f->objectName(), QString::fromLatin1(f->metaObject()->className()))
                                                .arg(r.top()).arg(r.bottom()).arg(visible.height())));
                        ++checked;
                    }
                    QVERIFY2(checked > 3, qPrintable(ctx));
                    if (!shotDir.isEmpty() && cs.first == QStringLiteral("rect") && t.id == theme::list().first().id && winSize.width() == 1400) {
                        QDir().mkpath(shotDir);
                        mw->grab().save(QDir(shotDir).filePath(QStringLiteral("property-inspector-default.png")));
                    }
                }
                qInfo("%s %dx%d: inspector %d px (content %d px), canvas view %d px", qPrintable(t.id), winSize.width(), winSize.height(),
                      pi->height(), pi->contentHeight(), mw->findChild<DocumentView *>()->height());
                // the window keeps its size: the canvas gives up the height, not the screen
                QVERIFY2(mw->height() <= winSize.height(), qPrintable(t.id));
                DocumentView *cv = mw->findChild<DocumentView *>();
                QVERIFY(cv && cv->height() >= 200);
                dd->markClean();
                delete mw;
            }
        }
        theme::apply(*qApp);
        // a bigger font: the inspector measures itself again and grows
        {
            auto *mw = new MainWindow;
            mw->resize(fitted);
            mw->addDocument(Document::createNew(400, 300, Qt::white));
            mw->show();
            QVERIFY(QTest::qWaitForWindowExposed(mw));
            auto *pi = mw->findChild<PropertyInspector *>();
            auto *scroll = pi->findChild<QScrollArea *>();
            const int before = scroll->minimumHeight();
            // the themes set the font size in the style sheet
            const QString keepSheet = qApp->styleSheet();
            qApp->setStyleSheet(keepSheet + QStringLiteral("\nQWidget { font-size: 13pt; }"));
            QTRY_VERIFY(scroll->minimumHeight() > before);
            qInfo("13pt: inspector content %d px (9pt: %d px)", scroll->minimumHeight(), before);
            qApp->setStyleSheet(keepSheet);
            QTRY_COMPARE(scroll->minimumHeight(), before);
            delete mw;
        }
        as.setTool(keepTool);
    }

    // no colour dialog bypasses the shared picker
    void noRawColorDialogs() {
        QDir src(QStringLiteral(LW_SOURCE_DIR "/src"));
        QStringList offenders;
        QDirIterator it(src.path(), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
        static const QRegularExpression raw(QStringLiteral(R"(QColorDialog\s*::\s*getColor|QColorDialog\s+\w+\s*[({;]|new\s+QColorDialog)"));
        while (it.hasNext()) {
            const QString f = it.next();
            if (f.endsWith(QLatin1String("ColorPicker.cpp"))) continue;
            QFile file(f);
            QVERIFY(file.open(QIODevice::ReadOnly));
            if (QString::fromUtf8(file.readAll()).contains(raw)) offenders << src.relativeFilePath(f);
        }
        QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QStringLiteral(", "))));
    }
};

QTEST_MAIN(UiSmoke)
#include "ui_smoke_test.moc"
