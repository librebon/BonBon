// v0.3.2 tests: theme-switch performance, flatten fidelity, pop-up slider fields, context-menu Transform,
// groups and masks.
#include "core/Document.h"
#include "core/Render.h"
#include "io/LwdFormat.h"
#include "io/DocIO.h"
#include "io/Codecs.h"
#include "io/Zip.h"
#include <QJsonDocument>
#include <QJsonObject>
#include "core/Transform.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/Dialogs.h"
#include "ui/PopupSlider.h"
#include "ui/PropertyInspector.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/Panels.h"
#include "ui/Theme.h"
#include "ui/TraceDialog.h"
#include "core/Trace.h"
#include "core/FindReplace.h"
#include <QCheckBox>
#include <QApplication>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QMouseEvent>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QElapsedTimer>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QMenuBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QKeyEvent>
#include <QtTest>
#include <algorithm>
#include <memory>

static void drainEvents() {
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
    for (QWidget *w : QApplication::topLevelWidgets()) if (w->isVisible()) w->repaint();
    QCoreApplication::processEvents();
}


static int maxDiff(const QImage &a0, const QImage &b0, QPoint *where = nullptr) {
    const QImage a = a0.convertToFormat(QImage::Format_ARGB32), b = b0.convertToFormat(QImage::Format_ARGB32);
    if (a.size() != b.size()) return 999;
    int m = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y)), *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            // compare premultiplied: fully transparent pixels of any colour are equal
            const QRgb A = qPremultiply(pa[x]), B = qPremultiply(pb[x]);
            const int d = qMax(qMax(qAbs(qRed(A) - qRed(B)), qAbs(qGreen(A) - qGreen(B))), qMax(qAbs(qBlue(A) - qBlue(B)), qAbs(qAlpha(A) - qAlpha(B))));
            if (d > m) { m = d; if (where) *where = QPoint(x, y); }
        }
    }
    return m;
}
static VObject shapeObj(Document *d, ObjType t, const QRectF &r, const QColor &fill, bool aa = true) {
    VObject o;
    o.type = t;
    o.rect = r;
    o.fillOn = true;
    o.fill = fill;
    o.antialias = aa;
    o.id = d->newId();
    return o;
}

// runs fn on the EffectDialog once it is the active modal window (the caller then enters exec())
static void whenEffectDialog(std::function<void(EffectDialog *)> fn) {
    auto *t = new QTimer;
    t->setInterval(10);
    QObject::connect(t, &QTimer::timeout, [t, fn] {
        if (auto *dlg = qobject_cast<EffectDialog *>(QApplication::activeModalWidget())) {
            t->stop();
            t->deleteLater();
            fn(dlg);
        }
    });
    t->start();
}
// closes the dialog the way the user would: 0 Cancel button, 1 Esc key, 2 the window's close
static void dismiss(EffectDialog *dlg, int how) {
    if (how == 0) QTest::mouseClick(dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    else if (how == 1) QTest::keyClick(dlg, Qt::Key_Escape);
    else dlg->close();
}
// nudges the first numeric field with the keyboard (Up a few times), so the live preview changes
static void nudge(EffectDialog *dlg) {
    QAbstractSpinBox *f = dlg->findChild<PopupSliderDoubleField *>();
    if (!f) f = dlg->findChild<PopupSliderField *>();
    QVERIFY(f);
    f->setFocus();
    for (int i = 0; i < 6; ++i) QTest::keyClick(f, Qt::Key_Up);
    QCoreApplication::processEvents();
}

// every number field with spin arrows under root ("Window title / objectName (class)")
static QStringList spinArrowOffenders(QWidget *root) {
    QStringList out;
    if (!root) return out;
    QList<QAbstractSpinBox *> boxes = root->findChildren<QAbstractSpinBox *>();
    if (auto *self = qobject_cast<QAbstractSpinBox *>(root)) boxes << self;
    for (QAbstractSpinBox *b : boxes)
    {
        const bool number = qobject_cast<QSpinBox *>(b) || qobject_cast<QDoubleSpinBox *>(b);
        const bool hasSlider = b->findChild<QWidget *>(QStringLiteral("sliderArrow"), Qt::FindDirectChildrenOnly);
        if (b->buttonSymbols() != QAbstractSpinBox::NoButtons || (number && !hasSlider))
            out << QStringLiteral("%1 / %2 (%3)").arg(root->window()->windowTitle(), b->objectName(), QLatin1String(b->metaObject()->className()));
    }
    return out;
}


static void sendMouse(QWidget *w, QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons bs, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QMouseEvent ev(t, p, w->mapToGlobal(p), b, bs, m);
    QApplication::sendEvent(w, &ev);
}
static void clickDoc(Canvas *c, QPointF a, Qt::KeyboardModifiers m = Qt::NoModifier) {
    const QPointF v = c->toView(a);
    sendMouse(c, QEvent::MouseMove, v, Qt::NoButton, Qt::NoButton, m);
    sendMouse(c, QEvent::MouseButtonPress, v, Qt::LeftButton, Qt::LeftButton, m);
    sendMouse(c, QEvent::MouseButtonRelease, v, Qt::LeftButton, Qt::NoButton, m);
    QCoreApplication::processEvents();
}
static void dblClickDoc(Canvas *c, QPointF a) {
    clickDoc(c, a);
    const QPointF v = c->toView(a);
    sendMouse(c, QEvent::MouseButtonDblClick, v, Qt::LeftButton, Qt::LeftButton);
    sendMouse(c, QEvent::MouseButtonRelease, v, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
}
static void dragDoc(Canvas *c, QPointF a, QPointF b, int steps = 6) {
    const QPointF va = c->toView(a), vb = c->toView(b);
    sendMouse(c, QEvent::MouseMove, va, Qt::NoButton, Qt::NoButton);
    sendMouse(c, QEvent::MouseButtonPress, va, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= steps; ++i) sendMouse(c, QEvent::MouseMove, va + (vb - va) * (double(i) / steps), Qt::NoButton, Qt::LeftButton);
    sendMouse(c, QEvent::MouseButtonRelease, vb, Qt::LeftButton, Qt::NoButton);
    QCoreApplication::processEvents();
}
static QTreeWidgetItem *findTreeItem(QTreeWidget *t, quint64 id) {
    QTreeWidgetItemIterator it(t);
    for (; *it; ++it) if ((*it)->parent() && (*it)->data(0, Qt::UserRole).toULongLong() == id) return *it;
    return nullptr;
}

static Document *groupDoc(VObject *a, VObject *b, VObject *c) {
    Document *d = Document::createNew(200, 160, QColor(Qt::white));
    *a = shapeObj(d, ObjType::Rect, QRectF(20, 20, 60, 40), QColor(200, 20, 20));
    *b = shapeObj(d, ObjType::Ellipse, QRectF(50, 40, 70, 60), QColor(20, 20, 200, 160));
    *c = shapeObj(d, ObjType::Rect, QRectF(130, 100, 40, 30), QColor(20, 160, 20));
    d->addObject(*a, 0, false);
    d->addObject(*b, 0, false);
    d->addObject(*c, 0, false);
    d->commit(QStringLiteral("setup"));
    return d;
}

static Document *maskedNestedDoc() {
    VObject a, b, c;
    Document *d = groupDoc(&a, &b, &c);
    d->setSelection({a.id, b.id});
    const quint64 g = d->groupSelection();
    d->find(g)->opacity = 70;
    d->find(g)->blend = BlendMode::Multiply;
    d->find(g)->name = QStringLiteral("inner");
    VObject m = shapeObj(d, ObjType::Ellipse, QRectF(25, 25, 80, 60), QColor(0, 0, 0));
    d->setSelection({g, c.id});
    const quint64 outer = d->groupSelection();
    d->setSelection({outer});
    d->pasteAsMask(m);
    d->commit(QStringLiteral("setup"));
    return d;
}

// a layered image with nested groups: bg | inner{L1} in outer{.., L2} | L3 (bottom first)
static codec::LayeredImage groupedLayers() {
    codec::LayeredImage li;
    li.width = 100; li.height = 80;
    auto layer = [&](const QString &n, const QRect &r, const QColor &c, int grp) {
        codec::RasterLayer L;
        L.name = n;
        L.image = QImage(r.size(), QImage::Format_ARGB32);
        L.image.fill(c);
        L.offset = r.topLeft();
        L.group = grp;
        li.layers << L;
    };
    li.groups << codec::LayerGroup{QStringLiteral("outer"), 200, int(BlendMode::Multiply), true, -1};
    li.groups << codec::LayerGroup{QStringLiteral("inner"), 128, int(BlendMode::Normal), true, 0};
    layer(QStringLiteral("bg"), QRect(0, 0, 100, 80), QColor(240, 230, 120), -1);
    layer(QStringLiteral("L1"), QRect(10, 10, 50, 40), QColor(200, 30, 30), 1);
    layer(QStringLiteral("L2"), QRect(40, 30, 50, 40), QColor(30, 30, 200), 0);
    layer(QStringLiteral("L3"), QRect(70, 5, 20, 20), QColor(30, 160, 30), -1);
    return li;
}
static Document *nestedGroupDoc() {
    VObject a, b, c;
    Document *d = groupDoc(&a, &b, &c);
    d->setSelection({a.id, b.id});
    const quint64 g = d->groupSelection();
    d->find(g)->opacity = 70;
    d->find(g)->blend = BlendMode::Multiply;
    d->find(g)->name = QStringLiteral("inner");
    d->setSelection({g, c.id});
    const quint64 outer = d->groupSelection();
    d->find(outer)->name = QStringLiteral("outer");
    d->commit(QStringLiteral("setup"));
    return d;
}

class V032 : public QObject {
    Q_OBJECT
private slots:
    // The theme switch got ~2x slower in 0.3.1 (Find & Replace's two ~1000-row font lists were re-measured on
    // every restyle), and the proxy style was re-created on every switch since 0.2.7. Generous threshold:
    // ~300 ms release on the reference box; ASan is several times slower.
    void themeSwitchPerf() {
        Prefs p0 = appState().prefs;
        p0.theme = QStringLiteral("classic");
        p0.themeFollowSystem = false;
        p0.accentOverride = false;
        appState().setPrefs(p0);
        MainWindow w;
        w.resize(1400, 900);
        w.addDocument(Document::createSample());
        w.show();
        for (const char *t : {"Optimize", "Layers", "Frames", "History", "Align", "Styles", "Library", "Auto Shapes"})
            if (auto *cp = w.panel(QString::fromLatin1(t))) cp->setCollapsed(false);
        drainEvents();
        QCOMPARE(theme::proxyStyleCount(), 1);
        QStyle *base = nullptr;
        for (QStyle *s : qApp->findChildren<QStyle *>()) if (!s->inherits("QStyleSheetStyle")) { base = s; break; }
        const int stylesBefore = int(qApp->findChildren<QStyle *>().size());
        // every long list keeps uniform item sizes (cheap restyle)
        for (QComboBox *c : w.findChildren<QComboBox *>())
            if (c->count() > 200)
                if (auto *lv = qobject_cast<QListView *>(c->view())) QVERIFY2(lv->uniformItemSizes(), qPrintable(c->objectName()));
        const QStringList ids = {QStringLiteral("dark"), QStringLiteral("light"), QStringLiteral("nord"), QStringLiteral("midnight"), QStringLiteral("classic")};
        QVector<double> ms;
        for (int r = 0; r < 2; ++r)
            for (const QString &id : ids) {
                Prefs p = appState().prefs;
                p.theme = id;
                QElapsedTimer t;
                t.start();
                appState().setPrefs(p);
                drainEvents();
                ms << t.nsecsElapsed() / 1e6;
                QCOMPARE(theme::current(), id);
            }
        std::sort(ms.begin(), ms.end());
        const double median = ms[ms.size() / 2];
        qInfo("theme switch median %.0f ms (min %.0f, max %.0f)", median, ms.first(), ms.last());
        const double limit = qEnvironmentVariableIntValue("LW_THEME_SWITCH_LIMIT_MS") > 0 ? qEnvironmentVariableIntValue("LW_THEME_SWITCH_LIMIT_MS") : 3000;
        QVERIFY2(median < limit, qPrintable(QStringLiteral("median %1 ms").arg(median)));
        // no stacking / re-creation: one proxy, the same object, and no new styles after 10 switches
        QCOMPARE(theme::proxyStyleCount(), 1);
        QCOMPARE(int(qApp->findChildren<QStyle *>().size()), stylesBefore);
        bool same = false;
        for (QStyle *s : qApp->findChildren<QStyle *>()) if (s == base) same = true;
        QVERIFY(same);
    }

    // the canvas right-click Transform submenu has exactly Modify > Transform's actions (incl. Perspective)
    void contextTransformMirrorsModify() {
        MainWindow w;
        w.resize(1000, 700);
        Document *d = Document::createNew(300, 200, Qt::white);
        w.addDocument(d);
        w.show();
        VObject r;
        r.type = ObjType::Rect;
        r.rect = QRectF(20, 20, 80, 60);
        r.fillOn = true;
        r.fill = Qt::red;
        r.id = d->newId();
        d->addObject(r, 0, false);
        d->setSelection({r.id});
        QMenu *mt = w.findChild<QMenu *>(QStringLiteral("modifyTransformMenu"));
        QVERIFY(mt);
        QMenu *ctx = w.buildCanvasContextMenu(d);
        QMenu *ct = ctx->findChild<QMenu *>(QStringLiteral("ctxTransform"));
        QVERIFY(ct);
        auto sig = [](QMenu *m) {
            QStringList out;
            for (QAction *a : m->actions()) out << (a->isSeparator() ? QStringLiteral("-") : a->objectName().isEmpty() ? a->text() : a->objectName());
            return out;
        };
        QCOMPARE(sig(ct), sig(mt));
        QCOMPARE(ct->actions().size(), mt->actions().size());
        for (int i = 0; i < mt->actions().size(); ++i)
            if (!mt->actions()[i]->isSeparator()) QCOMPARE(ct->actions()[i], mt->actions()[i]);   // the very same QActions
        QVERIFY(ct->actions().contains(w.action(QStringLiteral("modify.perspective"))));
        for (const char *id : {"modify.freetransform", "modify.scale", "modify.skew", "modify.distort", "modify.perspective", "modify.rot180",
                               "modify.rot90cw", "modify.rot90ccw", "modify.fliph", "modify.flipv", "modify.numeric", "modify.removexform"})
            QVERIFY2(ct->actions().contains(w.action(QString::fromLatin1(id))), id);
    }

    // Modify > Flatten Selection (and Convert to Bitmap / Merge Down) must look exactly like the objects did
    void flattenSelectionFidelity_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("white");
        const char *names[] = {"vector+vector", "vector+bitmap", "aa ellipses fractional", "drop shadow + glow", "stroke only", "transformed bitmap"};
        for (int k = 0; k < 6; ++k) {
            QTest::newRow(qPrintable(QStringLiteral("%1 transparent").arg(QLatin1String(names[k])))) << k << false;
            QTest::newRow(qPrintable(QStringLiteral("%1 white").arg(QLatin1String(names[k])))) << k << true;
        }
    }
    void flattenSelectionFidelity() {
        QFETCH(int, kind);
        QFETCH(bool, white);
        MainWindow w;
        Document *d = Document::createNew(240, 180, white ? QColor(Qt::white) : QColor(Qt::transparent));
        w.addDocument(d);
        VObject a = shapeObj(d, ObjType::Rect, QRectF(30, 30, 80, 60), QColor(200, 30, 30));
        VObject b = shapeObj(d, ObjType::Ellipse, QRectF(90, 60, 70, 70), QColor(30, 60, 200, 200));
        if (kind == 1) {
            b = VObject();
            b.type = ObjType::Bitmap;
            b.image = QImage(50, 40, QImage::Format_ARGB32_Premultiplied);
            b.image.fill(QColor(20, 160, 40));
            b.rect = QRectF(100, 70, 50, 40);
            b.id = d->newId();
        } else if (kind == 2) {
            a = shapeObj(d, ObjType::Ellipse, QRectF(30.3, 29.6, 81.4, 59.2), QColor(200, 30, 30));
            b = shapeObj(d, ObjType::Ellipse, QRectF(90.5, 60.25, 70.6, 70.1), QColor(30, 60, 200, 200));
        } else if (kind == 3) {
            LiveEffect sh; sh.type = EffectType::DropShadow;
            LiveEffect gl; gl.type = EffectType::OuterGlow; gl.color = Qt::yellow; gl.width = 6;
            a.effects << sh;
            b.effects << gl;
        } else if (kind == 4) {
            a.fillOn = false; a.strokeOn = true; a.stroke = Qt::black; a.strokeWidth = 3;
            b.fillOn = false; b.strokeOn = true; b.stroke = QColor(0, 120, 0); b.strokeWidth = 1.5;
        } else if (kind == 5) {
            b = VObject();
            b.type = ObjType::Bitmap;
            b.image = QImage(40, 30, QImage::Format_ARGB32_Premultiplied);
            b.image.fill(QColor(20, 160, 40));
            b.rect = QRectF(100, 70, 40, 30);
            b.xform = QTransform().translate(120, 85).rotate(17).scale(1.37, 1.21).translate(-120, -85);
            b.id = d->newId();
        }
        d->addObject(a, 0, false);
        d->addObject(b, 0, false);
        d->commit(QStringLiteral("setup"));
        w.resize(1200, 800);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        auto canvasShot = [&]() -> QImage {
            Canvas *c = nullptr;
            for (Canvas *k : w.findChildren<Canvas *>()) if (k->isVisible()) c = k;
            if (!c) return {};
            drainEvents();
            return c->grab().toImage();
        };
        d->clearSelection();
        const QImage shotBefore = canvasShot();
        const QImage before = render::renderFrame(d->s, 0, true);
        const QImage beforeNoBg = render::renderFrame(d->s, 0, false);
        d->setSelection({a.id, b.id});
        w.action(QStringLiteral("modify.flattensel"))->trigger();
        QCOMPARE(d->objects(0).size(), 1);
        const VObject &bm = d->objects(0)[0];
        QCOMPARE(bm.type, ObjType::Bitmap);
        QPoint at;
        const int diff = maxDiff(before, render::renderFrame(d->s, 0, true), &at);
        QVERIFY2(diff <= 1, qPrintable(QStringLiteral("with canvas: max diff %1 at %2,%3").arg(diff).arg(at.x()).arg(at.y())));
        const int diff2 = maxDiff(beforeNoBg, render::renderFrame(d->s, 0, false), &at);
        QVERIFY2(diff2 <= 1, qPrintable(QStringLiteral("no canvas: max diff %1 at %2,%3").arg(diff2).arg(at.x()).arg(at.y())));
        // what the canvas widget shows (selection cleared, so no overlay) must not change either
        d->clearSelection();
        const QImage shotAfter = canvasShot();
        const int diff3 = maxDiff(shotBefore, shotAfter, &at);
        // over the checkerboard, overlapping semi-transparent pixels can differ by 2/255 from 8-bit
        // premultiplied rounding (composite once vs twice); a border would differ by far more
        QVERIFY2(diff3 <= (white ? 1 : 2), qPrintable(QStringLiteral("canvas widget: max diff %1 at %2,%3").arg(diff3).arg(at.x()).arg(at.y())));
        d->selectOnly(bm.id);
        // tight: every edge row / column of the bitmap holds some ink, and nothing is opaque white unless the art was
        const QImage img = bm.image.convertToFormat(QImage::Format_ARGB32);
        auto rowInk = [&](int y) { for (int x = 0; x < img.width(); ++x) if (qAlpha(img.pixel(x, y))) return true; return false; };
        auto colInk = [&](int x) { for (int y = 0; y < img.height(); ++y) if (qAlpha(img.pixel(x, y))) return true; return false; };
        QVERIFY(rowInk(0) && rowInk(img.height() - 1) && colInk(0) && colInk(img.width() - 1));
        // undo restores the two objects
        d->undo();
        QCOMPARE(d->objects(0).size(), 2);
    }

    // Merge Down (a vector onto the bitmap below) keeps the look, tight, and stays in place in the stack

    // ---------------- groups and masks (Fireworks MX 2004 style) ----------------
    void groupUngroupRoundTrip() {
        VObject a, b, c;
        std::unique_ptr<Document> d(groupDoc(&a, &b, &c));
        const QImage before = render::renderFrame(d->s, 0, true);
        d->setSelection({a.id, b.id});
        const quint64 g = d->groupSelection();
        d->commit(QStringLiteral("Group"));
        QVERIFY(g);
        QCOMPARE(d->objects(0).size(), 2);
        QCOMPARE(d->objects(0)[0].type, ObjType::Group);   // in the slot of its top member (below c)
        QCOMPARE(d->objects(0)[0].children.size(), 2);
        QCOMPARE(d->objects(0)[1].id, c.id);
        QVERIFY(d->find(a.id));                            // members are still found
        QCOMPARE(d->topLevelOf(a.id)->id, g);
        QCOMPARE(d->objects(0)[0].bounds(), QRectF(20, 20, 100, 80));
        QCOMPARE(maxDiff(before, render::renderFrame(d->s, 0, true)), 0);
        // nested group
        d->setSelection({g, c.id});
        const quint64 outer = d->groupSelection();
        d->commit(QStringLiteral("Group"));
        QCOMPARE(d->objects(0).size(), 1);
        QCOMPARE(d->objects(0)[0].children.size(), 2);
        QCOMPARE(d->objects(0)[0].children[0].children.size(), 2);
        QCOMPARE(maxDiff(before, render::renderFrame(d->s, 0, true)), 0);
        // ungroup one level, then the inner group
        d->setSelection({outer});
        d->ungroupSelection();
        d->commit(QStringLiteral("Ungroup"));
        QCOMPARE(d->objects(0).size(), 2);
        d->setSelection({g});
        d->ungroupSelection();
        d->commit(QStringLiteral("Ungroup"));
        QCOMPARE(d->objects(0).size(), 3);
        QCOMPARE(d->objects(0)[0].id, a.id);
        QCOMPARE(d->objects(0)[2].id, c.id);
        QCOMPARE(maxDiff(before, render::renderFrame(d->s, 0, true)), 0);
        // undo back to the nested group, redo
        d->undo(); d->undo();
        QCOMPARE(d->objects(0).size(), 1);
        d->undo(); d->undo();
        QCOMPARE(d->objects(0).size(), 3);
        d->redo();
        QCOMPARE(d->objects(0)[0].type, ObjType::Group);
    }
    void groupOpacityAndBlendAsUnit() {
        VObject a, b, c;
        std::unique_ptr<Document> d(groupDoc(&a, &b, &c));
        d->setSelection({a.id, b.id});
        const quint64 g = d->groupSelection();
        d->find(g)->opacity = 50;
        const QImage img = render::renderFrame(d->s, 0, true);
        // overlap of a and b: the group is composited once at 50%, so the overlap shows b over a, then 50% white
        VObject ref;
        {
            Document *e = Document::createNew(200, 160, QColor(Qt::white));
            VObject a2 = a, b2 = b;
            a2.id = e->newId(); b2.id = e->newId();
            e->addObject(a2, 0, false); e->addObject(b2, 0, false);
            QImage solo = render::renderFrame(e->s, 0, true);
            const QRgb s = solo.pixel(60, 50), w = img.pixel(60, 50);
            QVERIFY(qAbs(qRed(w) - (qRed(s) + 255) / 2) <= 2);
            QVERIFY(qAbs(qBlue(w) - (qBlue(s) + 255) / 2) <= 2);
            delete e;
        }
        // ungroup keeps the look of each non-overlapping part: opacity multiplies into the members
        d->setSelection({g});
        d->ungroupSelection();
        QCOMPARE(d->find(a.id)->opacity, 50);
        QCOMPARE(d->find(b.id)->opacity, 50);
    }
    void nestedGroupTransforms() {
        VObject a, b, c;
        std::unique_ptr<Document> d(groupDoc(&a, &b, &c));
        d->setSelection({a.id, b.id});
        const quint64 g = d->groupSelection();
        d->setSelection({g, c.id});
        const quint64 outer = d->groupSelection();
        // move, scale (setBounds), rotate the outer group: every leaf follows
        VObject *o = d->find(outer);
        const QRectF b0 = o->bounds();
        o->moveBy(QPointF(10, 5));
        QCOMPARE(d->find(a.id)->rect, QRectF(30, 25, 60, 40));
        QCOMPARE(d->find(c.id)->rect, QRectF(140, 105, 40, 30));
        o = d->find(outer);
        o->setBounds(QRectF(b0.x() + 10, b0.y() + 5, b0.width() / 2, b0.height() / 2));
        QCOMPARE(d->find(outer)->bounds().size(), b0.size() / 2);
        QCOMPARE(d->find(a.id)->rect.width(), 30.0);
        const QPointF ctr = d->find(outer)->bounds().center();
        QTransform t; t.translate(ctr.x(), ctr.y()); t.rotate(90); t.translate(-ctr.x(), -ctr.y());
        xf::transformObject(*d->find(outer), t);
        const QRectF rb = d->find(outer)->bounds();
        QVERIFY2(qAbs(rb.width() - b0.height() / 2) < 1.0 && qAbs(rb.height() - b0.width() / 2) < 1.0,
                 qPrintable(QStringLiteral("%1x%2").arg(rb.width()).arg(rb.height())));
        QVERIFY(!d->find(outer)->hasXform());   // a group never has its own matrix
    }
    void effectsOnGroup() {
        VObject a, b, c;
        std::unique_ptr<Document> d(groupDoc(&a, &b, &c));
        d->setSelection({a.id, b.id});
        const quint64 g = d->groupSelection();
        LiveEffect sh = LiveEffect::make(EffectType::DropShadow);
        sh.distance = 10; sh.softness = 0; sh.opacity = 100; sh.angle = 315;
        d->find(g)->effects << sh;
        const QImage img = render::renderFrame(d->s, 0, true);
        // shadow of the whole group (offset down-right), not of each member separately: the shadow of a
        // under b is hidden by b's colour composited over the group shadow only once
        // below-right of b: the group's shadow, as dark as b's 63% alpha there
        QVERIFY2(qAbs(qGray(img.pixel(110, 98)) - 95) <= 3, qPrintable(QString::number(qGray(img.pixel(110, 98)))));
        QVERIFY(qGray(img.pixel(85, 65)) > 0);
        QVERIFY(d->find(g)->visualBounds().contains(QRectF(20, 20, 110, 90)));
        // Convert to Bitmap renders the group with its effects
        const QImage before = render::renderFrame(d->s, 0, true);
        d->setSelection({g});
        const QVector<quint64> bm = d->convertToBitmap();
        QCOMPARE(bm.size(), 1);
        QCOMPARE(d->find(bm[0])->type, ObjType::Bitmap);
        QVERIFY(maxDiff(before, render::renderFrame(d->s, 0, true)) <= 2);
    }
    void vectorAndBitmapMasks() {
        VObject a, b, c;
        std::unique_ptr<Document> d(groupDoc(&a, &b, &c));
        // Paste as Mask: an ellipse outline masks the rect
        VObject m = shapeObj(d.get(), ObjType::Ellipse, QRectF(30, 25, 30, 30), QColor(0, 0, 0, 40));
        d->setSelection({a.id});
        const quint64 g = d->pasteAsMask(m);
        QVERIFY(g);
        VObject *grp = d->find(g);
        QVERIFY(grp->maskActive());
        QCOMPARE(grp->maskMode, int(VObject::MaskOutline));
        QImage img = render::renderFrame(d->s, 0, true);
        QCOMPARE(QColor(img.pixel(45, 40)), QColor(200, 20, 20));   // inside the ellipse: full colour (outline ignores the mask's alpha)
        QCOMPARE(QColor(img.pixel(22, 22)), QColor(Qt::white));      // rect corner outside the mask
        QCOMPARE(grp->bounds(), QRectF(30, 25, 30, 30));
        // disable / enable
        d->setSelection({g});
        QCOMPARE(d->setMaskEnabled(false), 1);
        img = render::renderFrame(d->s, 0, true);
        QCOMPARE(QColor(img.pixel(22, 22)), QColor(200, 20, 20));
        d->setMaskEnabled(true);
        // linked: moving the group moves the mask; unlinked: only the content
        d->find(g)->moveBy(QPointF(5, 0));
        QCOMPARE(d->find(g)->maskObj[0].rect.x(), 35.0);
        d->setMaskLinked(false);
        d->find(g)->moveBy(QPointF(5, 0));
        QCOMPARE(d->find(g)->maskObj[0].rect.x(), 35.0);
        QCOMPARE(d->find(a.id)->rect.x(), 30.0);
        // delete mask: the content stays grouped
        QCOMPARE(d->deleteMask(), 1);
        QVERIFY(!d->find(g)->hasMask());
        QCOMPARE(d->find(g)->children.size(), 1);

        // grayscale bitmap mask: a left-to-right black..white gradient bitmap
        std::unique_ptr<Document> e(groupDoc(&a, &b, &c));
        VObject gm;
        gm.type = ObjType::Bitmap;
        gm.image = QImage(100, 80, QImage::Format_ARGB32_Premultiplied);
        for (int x = 0; x < 100; ++x) for (int y = 0; y < 80; ++y) gm.image.setPixel(x, y, qRgb(x * 255 / 99, x * 255 / 99, x * 255 / 99));
        gm.rect = QRectF(20, 20, 100, 80);
        e->setSelection({a.id, b.id});
        const quint64 g2 = e->pasteAsMask(gm);
        QCOMPARE(e->find(g2)->maskMode, int(VObject::MaskGrayscale));
        img = render::renderFrame(e->s, 0, true);
        QVERIFY(qGray(img.pixel(22, 30)) > 240);                 // black mask: hidden
        {   // 56% mask: red blended 56% over white
            const QRgb px = img.pixel(75, 30);
            QVERIFY2(qRed(px) > 190 && qGreen(px) > 95 && qGreen(px) < 155, qPrintable(QStringLiteral("%1 %2").arg(qRed(px)).arg(qGreen(px))));
        }
        // Hide All / Reveal All
        e->setSelection({c.id});
        const quint64 g3 = e->revealHideAll(false);
        img = render::renderFrame(e->s, 0, true);
        QCOMPARE(QColor(img.pixel(150, 115)), QColor(Qt::white));
        QCOMPARE(e->find(g3)->maskMode, int(VObject::MaskGrayscale));
        e->setSelection({g3});
        e->revealHideAll(true);
        img = render::renderFrame(e->s, 0, true);
        QCOMPARE(QColor(img.pixel(150, 115)), QColor(20, 160, 20));
    }
    void groupAsMaskAndPasteInside() {
        VObject a, b, c;
        std::unique_ptr<Document> d(groupDoc(&a, &b, &c));
        // Group as Mask: the top selected object (b) masks a
        d->setSelection({a.id, b.id});
        const quint64 g = d->groupAsMask();
        QVERIFY(g);
        QCOMPARE(d->find(g)->children.size(), 1);
        QCOMPARE(d->find(g)->maskObj[0].id, b.id);
        QVERIFY(d->isMaskObject(b.id));
        QCOMPARE(d->maskOwnerOfSelection(), g);
        // ungroup releases the mask as a normal object on top
        d->setSelection({g});
        d->ungroupSelection();
        QCOMPARE(d->objects(0).size(), 3);
        QVERIFY(!d->isMaskObject(b.id));
        // Paste Inside: the selected object (c) becomes the mask of the pasted content
        VObject content = shapeObj(d.get(), ObjType::Rect, QRectF(100, 90, 100, 60), QColor(250, 200, 0));
        d->setSelection({c.id});
        const quint64 g2 = d->pasteInside({content});
        QVERIFY(g2);
        QCOMPARE(d->find(g2)->maskObj[0].id, c.id);
        QCOMPARE(d->find(g2)->bounds(), QRectF(130, 100, 40, 30));
        const QImage img = render::renderFrame(d->s, 0, true);
        QCOMPARE(QColor(img.pixel(150, 115)), QColor(250, 200, 0));
        QCOMPARE(QColor(img.pixel(105, 95)), QColor(Qt::white));
    }
    void lwdRoundTripGroups() {
        std::unique_ptr<Document> d(maskedNestedDoc());
        const QImage before = render::renderFrame(d->s, 0, true);
        const QByteArray data = lwd::serialize(*d);
        QVERIFY(data.size() > 100);
        QString err;
        std::unique_ptr<Document> r(lwd::deserialize(data, &err));
        QVERIFY2(r, qPrintable(err));
        QCOMPARE(r->objects(0).size(), 1);
        const VObject &o = r->objects(0)[0];
        QCOMPARE(o.type, ObjType::Group);
        QVERIFY(o.maskActive());
        QCOMPARE(o.maskObj[0].type, ObjType::Ellipse);
        QCOMPARE(o.children.size(), 2);
        QCOMPARE(o.children[0].name, QStringLiteral("inner"));
        QCOMPARE(o.children[0].opacity, 70);
        QCOMPARE(o.children[0].blend, BlendMode::Multiply);
        QCOMPARE(o.children[0].children.size(), 2);
        QCOMPARE(maxDiff(before, render::renderFrame(r->s, 0, true)), 0);
        // ids stay unique: a new object after load gets an id above every nested one
        quint64 maxId = 0;
        o.forEachNested([&](const VObject &x) { maxId = qMax(maxId, x.id); });
        QVERIFY(r->newId() > maxId);
    }

    // a version 2 (0.3.1) document without groups still opens; a version 4 one (newer than this build) is refused
    void lwdOlderVersionsStillLoad() {
        std::unique_ptr<Document> d(Document::createNew(100, 80, QColor(Qt::white)));
        const VObject e = shapeObj(d.get(), ObjType::Rect, QRectF(10, 10, 40, 30), QColor(200, 30, 30));
        d->addObject(e, 0, false);
        QMap<QString, QByteArray> files;
        QVERIFY(zip::read(lwd::serialize(*d), files));
        auto withVersion = [&](int v) {
            QJsonObject root = QJsonDocument::fromJson(files.value(QStringLiteral("document.json"))).object();
            root[QStringLiteral("version")] = v;
            QVector<zip::Entry> entries;
            for (auto it = files.cbegin(); it != files.cend(); ++it)
                entries.append({it.key(), it.key() == QLatin1String("document.json") ? QJsonDocument(root).toJson() : it.value()});
            return zip::write(entries);
        };
        for (int v : {1, 2}) {
            QString err;
            std::unique_ptr<Document> r(lwd::deserialize(withVersion(v), &err));
            QVERIFY2(r, qPrintable(err));
            QCOMPARE(r->objects(0).size(), 1);
            QCOMPARE(r->objects(0)[0].type, e.type);
        }
        QString err;
        // 0.3.20 writes format version 7; a still-newer document must be refused
        std::unique_ptr<Document> r(lwd::deserialize(withVersion(8), &err));
        QVERIFY(!r);
        QVERIFY(err.contains(QStringLiteral("newer")));
    }
    // PSD / ORA / XCF layer groups: written as folders / nested stacks / group layers and read back with
    // their structure, opacity and blend mode; the composite matches the source
    void layeredGroupsRoundTrip() {
        const codec::LayeredImage src = groupedLayers();
        const QImage ref = codec::compositeLayers(src);
        using W = bool (*)(QByteArray &, const codec::LayeredImage &, QString *);
        using R = bool (*)(const QByteArray &, codec::LayeredImage &, QString *);
        const struct { const char *n; W w; R r; } fmts[] = {
            {"psd", codec::writePsd, codec::readPsd}, {"ora", codec::writeOra, codec::readOra}, {"xcf", codec::writeXcf, codec::readXcf}};
        for (const auto &f : fmts) {
            QByteArray data;
            QString err;
            QVERIFY2(f.w(data, src, &err), qPrintable(err));
            codec::LayeredImage li;
            QVERIFY2(f.r(data, li, &err), qPrintable(QString::fromLatin1(f.n) + err));
            QVERIFY2(!li.partial, f.n);
            QCOMPARE(li.layers.size(), 4);
            QCOMPARE(li.groups.size(), 2);
            QStringList order;
            for (const auto &L : li.layers) order << L.name;
            QCOMPARE(order, QStringList({"bg", "L1", "L2", "L3"}));
            const int go = li.layers[2].group, gi = li.layers[1].group;
            QVERIFY2(go >= 0 && gi >= 0 && go != gi, f.n);
            QCOMPARE(li.layers[0].group, -1);
            QCOMPARE(li.layers[3].group, -1);
            QCOMPARE(li.groups[go].name, QStringLiteral("outer"));
            QCOMPARE(li.groups[gi].name, QStringLiteral("inner"));
            QCOMPARE(li.groups[gi].parent, go);
            QCOMPARE(li.groups[go].parent, -1);
            QVERIFY2(qAbs(li.groups[go].opacity - 200) <= 1, f.n);
            QVERIFY2(qAbs(li.groups[gi].opacity - 128) <= 1, f.n);
            QCOMPARE(li.groups[go].blend, int(BlendMode::Multiply));
            QCOMPARE(li.groups[gi].blend, int(BlendMode::Normal));
            QPoint at;
            const int diff = maxDiff(ref, codec::compositeLayers(li), &at);
            QVERIFY2(diff <= 2, qPrintable(QStringLiteral("%1 diff %2 at %3,%4").arg(f.n).arg(diff).arg(at.x()).arg(at.y())));
        }
        // groups do change the composite (isolated Multiply group vs. flat layers)
        codec::LayeredImage flat = src;
        flat.groups.clear();
        for (auto &L : flat.layers) L.group = -1;
        QVERIFY(maxDiff(ref, codec::compositeLayers(flat)) > 10);
    }
    // a hand-written ORA with nested <stack> elements (independent of our writer)
    void oraNestedStackImport() {
        auto png = [](const QColor &c) {
            QImage img(20, 20, QImage::Format_ARGB32);
            img.fill(c);
            QByteArray b;
            QBuffer buf(&b);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            return b;
        };
        const QByteArray xml =
            "<?xml version='1.0' encoding='UTF-8'?>\n<image w=\"40\" h=\"30\" version=\"0.0.5\"><stack>"
            "<layer name=\"top\" src=\"data/t.png\" x=\"20\" y=\"10\" opacity=\"1.0\"/>"
            "<stack name=\"Folder\" opacity=\"0.5\" composite-op=\"svg:screen\" visibility=\"visible\">"
            "<stack name=\"Sub\" visibility=\"hidden\"><layer name=\"deep\" src=\"data/d.png\" opacity=\"0.8\"/></stack>"
            "<layer name=\"mid\" src=\"data/m.png\" x=\"5\" y=\"5\"/></stack>"
            "<layer name=\"base\" src=\"data/b.png\"/></stack></image>";
        const QByteArray z = zip::write({{QStringLiteral("mimetype"), QByteArray("image/openraster"), false},
                                         {QStringLiteral("stack.xml"), xml, true},
                                         {QStringLiteral("data/t.png"), png(Qt::red), false},
                                         {QStringLiteral("data/d.png"), png(Qt::green), false},
                                         {QStringLiteral("data/m.png"), png(Qt::blue), false},
                                         {QStringLiteral("data/b.png"), png(Qt::white), false}});
        codec::LayeredImage li;
        QString err;
        QVERIFY2(codec::readOra(z, li, &err), qPrintable(err));
        QCOMPARE(li.layers.size(), 4);
        QCOMPARE(li.layers[0].name, QStringLiteral("base"));
        QCOMPARE(li.layers[1].name, QStringLiteral("mid"));
        QCOMPARE(li.layers[2].name, QStringLiteral("deep"));
        QCOMPARE(li.layers[3].name, QStringLiteral("top"));
        QCOMPARE(li.groups.size(), 2);
        const int folder = li.layers[1].group, sub = li.layers[2].group;
        QVERIFY(folder >= 0 && sub >= 0);
        QCOMPARE(li.groups[folder].name, QStringLiteral("Folder"));
        QCOMPARE(li.groups[folder].opacity, 128);
        QCOMPARE(li.groups[folder].blend, int(BlendMode::Screen));
        QCOMPARE(li.groups[sub].parent, folder);
        QVERIFY(!li.groups[sub].visible);
        QCOMPARE(li.layers[2].opacity, 204);   // not multiplied by the stack's
        // into a document: the folder becomes one layer holding a Group (with the hidden nested group)
        std::unique_ptr<Document> d(docio::openData(z, Fmt::ORA, QString(), &err));
        QVERIFY2(d, qPrintable(err));
        const Frame &fr = d->s.frames[0];
        int found = 0;
        for (int L = 0; L < fr.layers.size(); ++L)
            for (const VObject &o : fr.layers[L])
                if (o.type == ObjType::Group) {
                    ++found;
                    QCOMPARE(d->s.layers[L].name, QStringLiteral("Folder"));
                    QCOMPARE(o.opacity, 50);
                    QCOMPARE(o.blend, BlendMode::Screen);
                    QCOMPARE(o.children.size(), 2);
                    QCOMPARE(o.children[1].type, ObjType::Group);
                    QVERIFY(!o.children[1].visible);
                    QCOMPARE(o.children[1].children.size(), 1);
                }
        QCOMPARE(found, 1);
    }
    // a document whose layer holds one group exports it as a layer group (PSD / ORA / XCF) and opens
    // back as nested Group objects that render like the original
    void documentGroupsViaLayeredFormats() {
        std::unique_ptr<Document> d(nestedGroupDoc());
        const QImage before = render::renderFrame(d->s, 0, false);
        const codec::LayeredImage li = docio::layersFromState(d->s, 0);
        QCOMPARE(li.groups.size(), 2);
        for (Fmt f : {Fmt::PSD, Fmt::ORA, Fmt::XCF}) {
            ExportOptions o = d->exportOptions;
            o.format = f;
            QByteArray data;
            QString err;
            QVERIFY2(docio::exportDocument(*d, 0, o, data, &err), qPrintable(err));
            std::unique_ptr<Document> back(docio::openData(data, f, QString(), &err));
            QVERIFY2(back, qPrintable(err));
            const VObject *outer = nullptr;
            for (const auto &layer : back->s.frames[0].layers)
                for (const VObject &obj : layer)
                    if (obj.type == ObjType::Group) outer = &obj;
            QVERIFY(outer);
            QCOMPARE(outer->name, QStringLiteral("outer"));
            QCOMPARE(outer->children.size(), 2);
            const VObject &inner = outer->children[0];
            QCOMPARE(inner.type, ObjType::Group);
            QCOMPARE(inner.name, QStringLiteral("inner"));
            QVERIFY(qAbs(inner.opacity - 70) <= 1);
            QCOMPARE(inner.blend, BlendMode::Multiply);
            QCOMPARE(inner.children.size(), 2);
            QPoint at;
            const int diff = maxDiff(before, render::renderFrame(back->s, 0, false), &at);
            QVERIFY2(diff <= 3, qPrintable(QStringLiteral("fmt %1 diff %2 at %3,%4").arg(int(f)).arg(diff).arg(at.x()).arg(at.y())));
        }
    }
    void svgRoundTripGroups() {
        std::unique_ptr<Document> d(maskedNestedDoc());
        // vector outline mask -> <clipPath>; nested groups -> <g lw:group>
        bool pure = false;
        const QByteArray svg = docio::stateToSvg(d->s, 0, &pure);
        QVERIFY(pure);
        QVERIFY(docio::isVectorDocument(d->s, 0));
        QVERIFY(svg.contains("<clipPath"));
        QVERIFY(svg.contains("mix-blend-mode"));
        QCOMPARE(svg.count("lw:group=\"1\""), 2);
        // QtSvg renders the export like the canvas (clip and nested opacity), allowing for AA differences
        // re-import: groups come back (the clipped file itself rasterizes on import: clipPath support is
        // export-only; strip it to check the structure)
        QString s = QString::fromUtf8(svg);
        s.remove(QRegularExpression(QStringLiteral("<defs>.*?</defs>"), QRegularExpression::DotMatchesEverythingOption));
        s.remove(QRegularExpression(QStringLiteral(" clip-path=\"[^\"]*\"")));
        DocState st;
        quint64 next = 1;
        QStringList notes;
        QString err;
        QVERIFY2(docio::svgToState(s.toUtf8(), st, [&] { return next++; }, QString(), &notes, &err), qPrintable(err));
        QVERIFY(st.frames[0].layers.size() >= 1);   // plus the Web Layer
        const QVector<VObject> &objs = st.frames[0].layers[0];
        QCOMPARE(objs.size(), 1);
        QCOMPARE(objs[0].type, ObjType::Group);
        QCOMPARE(objs[0].children.size(), 2);
        const VObject &inner = objs[0].children[0];
        QCOMPARE(inner.type, ObjType::Group);
        QCOMPARE(inner.name, QStringLiteral("inner"));
        QCOMPARE(inner.opacity, 70);
        QCOMPARE(inner.blend, BlendMode::Multiply);
        QCOMPARE(inner.children.size(), 2);
        QCOMPARE(inner.children[0].type, ObjType::Rect);
        QCOMPARE(inner.children[1].type, ObjType::Ellipse);
        // a foreign file: a wrapper <g> around one element is not a group; a <g> with two is
        const QByteArray foreign =
            "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
            "<g id='layer1'><g opacity='0.5'><rect x='1' y='1' width='10' height='10' fill='red'/></g>"
            "<g style='mix-blend-mode:screen'><rect x='20' y='1' width='10' height='10'/><circle cx='50' cy='50' r='5'/></g></g></svg>";
        DocState st2;
        QVERIFY(docio::svgToState(foreign, st2, [&] { return next++; }, QString(), &notes, &err));
        const QVector<VObject> &o2 = st2.frames[0].layers[0];
        QCOMPARE(o2.size(), 2);
        QCOMPARE(o2[0].type, ObjType::Rect);
        QCOMPARE(o2[0].opacity, 50);
        QCOMPARE(o2[1].type, ObjType::Group);
        QCOMPARE(o2[1].blend, BlendMode::Screen);
        // a grayscale bitmap mask exports as an SVG luminance <mask> with an embedded PNG
        std::unique_ptr<Document> e(maskedNestedDoc());
        e->setSelection({e->objects(0)[0].id});
        e->revealHideAll(true);
        const QByteArray svg2 = docio::stateToSvg(e->s, 0, &pure);
        QVERIFY(!pure);
        QVERIFY(svg2.contains("<mask"));
        QVERIFY(svg2.contains("mask=\"url(#lwm1)\""));
    }

    void mergeDownFidelity() {
        MainWindow w;
        Document *d = Document::createNew(200, 160, QColor(Qt::white));
        w.addDocument(d);
        VObject bmp;
        bmp.type = ObjType::Bitmap;
        bmp.image = QImage(60, 50, QImage::Format_ARGB32_Premultiplied);
        bmp.image.fill(QColor(20, 160, 40));
        bmp.rect = QRectF(20, 20, 60, 50);
        bmp.id = d->newId();
        VObject e = shapeObj(d, ObjType::Ellipse, QRectF(50.5, 40.25, 70.3, 50.6), QColor(200, 30, 30, 180));
        VObject top = shapeObj(d, ObjType::Rect, QRectF(100, 100, 40, 30), QColor(0, 0, 0));
        d->addObject(bmp, 0, false);
        d->addObject(e, 0, false);
        d->addObject(top, 0, false);
        d->commit(QStringLiteral("setup"));
        const QImage before = render::renderFrame(d->s, 0, true);
        d->setSelection({e.id});
        w.action(QStringLiteral("modify.mergedown"))->trigger();
        QCOMPARE(d->objects(0).size(), 2);
        QCOMPARE(d->objects(0)[0].type, ObjType::Bitmap);
        QCOMPARE(d->objects(0)[1].id, top.id);   // the unselected object above stays above
        QPoint at;
        const int diff = maxDiff(before, render::renderFrame(d->s, 0, true), &at);
        QVERIFY2(diff <= 1, qPrintable(QStringLiteral("max diff %1 at %2,%3").arg(diff).arg(at.x()).arg(at.y())));
    }

    // Fireworks: an effect with a settings window is only inserted when OK is clicked; Cancel / Esc /
    // close leave nothing behind. Editing an existing effect and cancelling restores it exactly.
    void effectsAddCancel_data() {
        QTest::addColumn<int>("type");
        QTest::addColumn<int>("how");
        QTest::addColumn<bool>("viaMenu");
        const EffectType types[] = {EffectType::DropShadow, EffectType::GaussianBlur, EffectType::OuterGlow, EffectType::InnerBevel};
        const char *hows[] = {"Cancel", "Esc", "close"};
        for (EffectType t : types)
            for (int h = 0; h < 3; ++h)
                for (bool menu : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("%1 %2 %3").arg(effectName(t), QLatin1String(hows[h]), menu ? QStringLiteral("menu") : QStringLiteral("PI+"))))
                        << int(t) << h << menu;
    }
    void effectsAddCancel() {
        QFETCH(int, type);
        QFETCH(int, how);
        QFETCH(bool, viaMenu);
        const EffectType t = EffectType(type);
        QVERIFY(effectHasSettings(t));
        MainWindow w;
        w.resize(1300, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document *d = Document::createNew(200, 160, QColor(Qt::white));
        w.addDocument(d);
        VObject r = shapeObj(d, ObjType::Rect, QRectF(40, 40, 80, 60), QColor(40, 90, 200));
        LiveEffect existing = LiveEffect::make(EffectType::HueSaturation);
        existing.hue = 40;
        r.effects << existing;
        d->addObject(r, 0, false);
        d->commit(QStringLiteral("setup"));
        d->markClean();
        d->setSelection({r.id});
        auto *pi = w.findChild<PropertyInspector *>();
        QVERIFY(pi);
        pi->refresh();
        auto *list = pi->findChild<QListWidget *>(QStringLiteral("fxList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        const QByteArray saved = lwd::serialize(*d);
        const int hist = d->historyIndex();
        const QImage before = render::renderFrame(d->s, 0, true);
        bool sawPreview = false, listUntouched = false, ran = false;
        whenEffectDialog([&](EffectDialog *dlg) {
            ran = true;
            drainEvents();
            listUntouched = list->count() == 1 && d->find(r.id)->effects.size() >= 1;
            nudge(dlg);
            drainEvents();
            sawPreview = render::renderFrame(d->s, 0, true) != before;
            listUntouched = listUntouched && list->count() == 1;
            dismiss(dlg, how);
        });
        if (viaMenu) w.action(QStringLiteral("fx.") + effectKey(t))->trigger();
        else pi->findChild<QAction *>(QStringLiteral("fx_add_") + effectName(t))->trigger();
        QVERIFY(ran);
        QVERIFY2(sawPreview, "the effect is previewed while its window is open");
        QVERIFY2(listUntouched, "the effect is not in the list while its window is open");
        drainEvents();
        QCOMPARE(d->find(r.id)->effects.size(), 1);
        QCOMPARE(list->count(), 1);
        QCOMPARE(d->historyIndex(), hist);
        QVERIFY(!d->isDirty());
        QCOMPARE(lwd::serialize(*d), saved);
        QCOMPARE(render::renderFrame(d->s, 0, true), before);

        // OK adds it: one undo step
        whenEffectDialog([&](EffectDialog *dlg) { nudge(dlg); QTest::mouseClick(dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok), Qt::LeftButton); });
        if (viaMenu) w.action(QStringLiteral("fx.") + effectKey(t))->trigger();
        else pi->findChild<QAction *>(QStringLiteral("fx_add_") + effectName(t))->trigger();
        drainEvents();
        QCOMPARE(d->find(r.id)->effects.size(), 2);
        QCOMPARE(d->find(r.id)->effects[1].type, t);
        QCOMPARE(list->count(), 2);
        QCOMPARE(d->historyIndex(), hist + 1);
        QVERIFY(d->isDirty());
        d->undo();
        QCOMPARE(lwd::serialize(*d), saved);
    }

    void effectsEditCancel_data() {
        QTest::addColumn<int>("type");
        QTest::addColumn<int>("how");
        QTest::addColumn<bool>("infoButton");
        const EffectType types[] = {EffectType::DropShadow, EffectType::GaussianBlur, EffectType::OuterGlow, EffectType::InnerBevel};
        const char *hows[] = {"Cancel", "Esc", "close"};
        for (EffectType t : types)
            for (int h = 0; h < 3; ++h)
                for (bool info : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("%1 %2 %3").arg(effectName(t), QLatin1String(hows[h]), info ? QStringLiteral("info") : QStringLiteral("dblclick"))))
                        << int(t) << h << info;
    }
    void effectsEditCancel() {
        QFETCH(int, type);
        QFETCH(int, how);
        QFETCH(bool, infoButton);
        const EffectType t = EffectType(type);
        MainWindow w;
        w.resize(1300, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document *d = Document::createNew(200, 160, QColor(Qt::white));
        w.addDocument(d);
        VObject r = shapeObj(d, ObjType::Rect, QRectF(40, 40, 80, 60), QColor(40, 90, 200));
        LiveEffect e = LiveEffect::make(t);
        e.distance = 9; e.softness = 3; e.width = 5; e.opacity = 70; e.angle = 300;
        r.effects << e;
        d->addObject(r, 0, false);
        d->commit(QStringLiteral("setup"));
        d->markClean();
        d->setSelection({r.id});
        auto *pi = w.findChild<PropertyInspector *>();
        pi->refresh();
        auto *list = pi->findChild<QListWidget *>(QStringLiteral("fxList"));
        QVERIFY(list && list->isVisible());
        QCOMPARE(list->count(), 1);
        const QByteArray saved = lwd::serialize(*d);
        const int hist = d->historyIndex();
        const QImage before = render::renderFrame(d->s, 0, true);
        auto open = [&] {
            list->setCurrentRow(0);
            if (infoButton) {
                auto *b = pi->findChild<QToolButton *>(QStringLiteral("fxEdit"));
                QVERIFY(b);
                QTest::mouseClick(b, Qt::LeftButton);
            } else {
                const QPoint c = list->visualItemRect(list->item(0)).center();
                // a real double-click: press/release, then the second press arrives as a double-click
                QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, c);
                QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, c);
            }
        };
        bool sawPreview = false, ran = false;
        whenEffectDialog([&](EffectDialog *dlg) {
            ran = true;
            nudge(dlg);
            drainEvents();
            sawPreview = render::renderFrame(d->s, 0, true) != before;
            dismiss(dlg, how);
        });
        open();
        QVERIFY(ran);
        QVERIFY(sawPreview);
        drainEvents();
        QCOMPARE(list->count(), 1);
        QCOMPARE(d->historyIndex(), hist);
        QVERIFY(!d->isDirty());
        QCOMPARE(lwd::serialize(*d), saved);    // previous parameters exactly
        QCOMPARE(render::renderFrame(d->s, 0, true), before);   // live preview reverted
        // OK commits one step
        whenEffectDialog([&](EffectDialog *dlg) { nudge(dlg); QTest::keyClick(dlg, Qt::Key_Return); });
        open();
        drainEvents();
        QCOMPARE(d->historyIndex(), hist + 1);
        QVERIFY(lwd::serialize(*d) != saved);
        d->undo();
        QCOMPARE(lwd::serialize(*d), saved);
    }

    // No number field anywhere may show spin arrows: every one is a pop-up slider field. The source
    // check catches dialogs built on demand; the walk opens every "..." command and checks the main
    // window with each tool and object type. The allowlist is empty on purpose.
    // Qt's own dialogs (QColorDialog, QInputDialog) build plain spin boxes; the app-wide adoption gives them
    // the pop-up slider too, and the slider really drives the value.
    void foreignSpinBoxesAdopted() {
        MainWindow w;   // installs the adoption filter
        QDialog host;
        auto *plain = new QSpinBox(&host);
        plain->setRange(0, 255);
        auto *dbl = new QDoubleSpinBox(&host);
        dbl->setRange(-360, 360);
        host.show();
        drainEvents();
        for (QAbstractSpinBox *b : {static_cast<QAbstractSpinBox *>(plain), static_cast<QAbstractSpinBox *>(dbl)}) {
            QCOMPARE(b->buttonSymbols(), QAbstractSpinBox::NoButtons);
            QVERIFY(b->findChild<QWidget *>(QStringLiteral("sliderArrow"), Qt::FindDirectChildrenOnly));
        }
        QVERIFY(!PopupSliderHub::adopt(plain));   // once only
        auto *own = new PopupSliderField(0, 10, {}, &host);
        QVERIFY(!PopupSliderHub::adopt(own));     // our own fields are left alone
        QCOMPARE(own->findChildren<QWidget *>(QStringLiteral("sliderArrow")).size(), 1);
        // drag on the adopted slider: press the triangle, move to the far (max) end, release
        auto *arrow = plain->findChild<QWidget *>(QStringLiteral("sliderArrow"));
        QTest::mousePress(arrow, Qt::LeftButton, {}, arrow->rect().center());
        QWidget *pop = plain->findChild<QWidget *>(QStringLiteral("sliderPopup"));
        QVERIFY(pop);
        QTRY_VERIFY(pop->isVisible());
        const QPoint far = PopupSliderHub::orientation() == Qt::Vertical ? QPoint(pop->width() / 2, 0) : QPoint(pop->width() - 1, pop->height() / 2);
        QMouseEvent mv(QEvent::MouseMove, QPointF(far), pop->mapToGlobal(QPointF(far)), Qt::NoButton, Qt::LeftButton, {});
        QApplication::sendEvent(pop, &mv);
        QMouseEvent up(QEvent::MouseButtonRelease, QPointF(far), pop->mapToGlobal(QPointF(far)), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(pop, &up);
        drainEvents();
        QCOMPARE(plain->value(), 255);
        // Shift+Up takes a fine step on decimal fields, as on our fields (0.3.5: step 1 x 33% = 0.33)
        dbl->setValue(0);
        dbl->setFocus();
        QTest::keyClick(dbl, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(dbl->value(), PopupSliderHub::fineStep(dbl));
        QCOMPARE(dbl->value(), 0.33);
        // a real QColorDialog
        QColorDialog cd;
        cd.setOption(QColorDialog::DontUseNativeDialog);
        cd.show();
        drainEvents();
        const auto boxes = cd.findChildren<QSpinBox *>();
        QVERIFY(!boxes.isEmpty());
        QVERIFY(spinArrowOffenders(&cd).isEmpty());
        cd.reject();
    }

    void noSpinArrowsAnywhere() {
        const QStringList allowlist;   // intentionally empty
        // 1. source: no plain spin boxes are created
        QStringList srcHits;
        QDirIterator it(QStringLiteral(LW_SOURCE_DIR "/src"), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
        const QRegularExpression re(QStringLiteral("new\\s+Q(Double)?SpinBox\\b|\\bQ(Double)?SpinBox\\s+\\w+\\s*[;({]|setButtonSymbols\\s*\\(\\s*QAbstractSpinBox::(UpDownArrows|PlusMinus)"));
        while (it.hasNext()) {
            QFile f(it.next());
            if (!f.open(QIODevice::ReadOnly)) continue;
            int line = 0;
            for (const QByteArray &l : f.readAll().split('\n')) {
                ++line;
                if (re.match(QString::fromUtf8(l)).hasMatch()) srcHits << QStringLiteral("%1:%2").arg(QFileInfo(f.fileName()).fileName()).arg(line);
            }
        }
        QVERIFY2(srcHits.isEmpty(), qPrintable(srcHits.join(QStringLiteral(", "))));

        // 2. the main window: panels, the Property Inspector for each object type, every tool's options
        MainWindow w;
        w.resize(1400, 950);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        Document *d = Document::createNew(300, 200, QColor(Qt::white));
        w.addDocument(d);
        VObject r = shapeObj(d, ObjType::Rect, QRectF(20, 20, 80, 60), QColor(40, 90, 200));
        VObject t; t.type = ObjType::Text; t.text = QStringLiteral("Hello"); t.rect = QRectF(120, 20, 100, 40); t.id = d->newId(); t.fillOn = true; t.fill = Qt::black;
        VObject bm; bm.type = ObjType::Bitmap; bm.image = QImage(40, 40, QImage::Format_ARGB32_Premultiplied); bm.image.fill(Qt::red); bm.rect = QRectF(20, 120, 40, 40); bm.id = d->newId();
        d->addObject(r, 0, false); d->addObject(t, 0, false); d->addObject(bm, 0, false);
        d->commit(QStringLiteral("setup"));
        QStringList bad;
        auto checkAll = [&](const QString &where) {
            drainEvents();
            for (QWidget *tl : QApplication::topLevelWidgets())
                if (tl->isVisible())
                    for (const QString &o : spinArrowOffenders(tl)) if (!allowlist.contains(o)) bad << where + QStringLiteral(": ") + o;
        };
        auto *pi = w.findChild<PropertyInspector *>();
        for (const QSet<quint64> &sel : {QSet<quint64>{}, QSet<quint64>{r.id}, QSet<quint64>{t.id}, QSet<quint64>{bm.id}}) {
            d->setSelection(sel);
            if (pi) pi->refresh();
            checkAll(QStringLiteral("selection"));
        }
        for (int ti = 0; ti <= int(Tool::Zoom); ++ti) {
            appState().setTool(Tool(ti));
            checkAll(QStringLiteral("tool %1").arg(ti));
        }
        appState().setTool(Tool::Pointer);
        // 3. every command that opens a window ("..."), dismissed straight away
        const QStringList skip = {QStringLiteral("file.quit"), QStringLiteral("file.close"), QStringLiteral("file.closeall")};
        QStringList opened;
        for (QAction *a : w.findChildren<QAction *>()) {
            const QString id = a->objectName();
            if (id.isEmpty() || skip.contains(id) || !a->text().contains(QStringLiteral("..."))) continue;
            if (!a->isEnabled()) continue;
            d->setSelection({bm.id});
            bool sawWindow = false;
            auto *poll = new QTimer;
            poll->setInterval(15);
            int ticks = 0;
            QObject::connect(poll, &QTimer::timeout, [&, poll] {
                ++ticks;
                if (QWidget *m = QApplication::activeModalWidget()) {
                    sawWindow = true;
                    checkAll(id);
                    if (auto *dl = qobject_cast<QDialog *>(m)) dl->reject(); else m->close();
                }
                if (ticks > 400) poll->stop();
            });
            poll->start();
            a->trigger();
            checkAll(id);   // modeless windows
            poll->stop();
            poll->deleteLater();
            for (QWidget *tl : QApplication::topLevelWidgets())
                if (tl != &w && tl->isVisible() && tl->isWindow() && qobject_cast<QDialog *>(tl)) tl->close();
            if (sawWindow) opened << id;
            if (w.currentDoc() != d) break;   // a command replaced the document; stop here
        }
        qInfo("checked %d windows opened by commands", int(opened.size()));
        QVERIFY2(opened.size() >= 20, qPrintable(opened.join(QLatin1Char(' '))));
        bad.removeDuplicates();
        QVERIFY2(bad.isEmpty(), qPrintable(bad.join(QLatin1Char('\n'))));
    }

    // Fireworks pointer semantics with real mouse events: Ctrl+G groups, a click picks the whole group,
    // a double-click enters it (picking the member), clicks then pick members, Esc leaves; Subselection
    // picks the leaf; dragging the group moves its members; Ungroup / undo / redo; the Layers tree.
    void groupMouseSelection() {
        MainWindow w;
        w.resize(1200, 850);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        DocumentView *v = w.addDocument(Document::createNew(300, 200, QColor(Qt::white)));
        QCoreApplication::processEvents();
        Document *d = v->doc();
        Canvas *cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        VObject a = shapeObj(d, ObjType::Rect, QRectF(20, 20, 60, 40), QColor(200, 20, 20));
        VObject b = shapeObj(d, ObjType::Rect, QRectF(100, 20, 60, 40), QColor(20, 200, 20));
        VObject c = shapeObj(d, ObjType::Rect, QRectF(200, 120, 50, 50), QColor(20, 20, 200));
        d->addObject(a, 0, false); d->addObject(b, 0, false); d->addObject(c, 0, false);
        d->commit(QStringLiteral("setup"));
        appState().setTool(Tool::Pointer);
        d->setSelection({a.id, b.id});
        QAction *grp = w.action(QStringLiteral("modify.group"));
        QCOMPARE(grp->shortcut(), QKeySequence(QStringLiteral("Ctrl+G")));
        QCOMPARE(w.action(QStringLiteral("modify.ungroup"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+G")));
        grp->trigger();
        QCOMPARE(d->objects(0).size(), 2);
        const VObject &g0 = d->objects(0)[0];
        QCOMPARE(g0.type, ObjType::Group);
        QCOMPARE(g0.children.size(), 2);
        const quint64 gid = g0.id;
        QCOMPARE(d->selection, QSet<quint64>{gid});
        // click on a member: the group is selected
        d->clearSelection();
        clickDoc(cv, QPointF(50, 40));
        QCOMPARE(d->selection, QSet<quint64>{gid});
        QCOMPARE(d->enteredGroup, quint64(0));
        // double-click: enter the group, select the member under the cursor
        dblClickDoc(cv, QPointF(50, 40));
        QCOMPARE(d->enteredGroup, gid);
        QCOMPARE(d->selection, QSet<quint64>{a.id});
        clickDoc(cv, QPointF(130, 40));   // inside the group, clicks pick members
        QCOMPARE(d->selection, QSet<quint64>{b.id});
        QTest::keyClick(cv, Qt::Key_Escape);
        QCOMPARE(d->enteredGroup, quint64(0));
        QCOMPARE(d->selection, QSet<quint64>{gid});
        // clicking outside any group member leaves the group for another object
        clickDoc(cv, QPointF(225, 145));
        QCOMPARE(d->selection, QSet<quint64>{c.id});
        // Subselection picks the leaf directly
        appState().setTool(Tool::Subselect);
        clickDoc(cv, QPointF(130, 40));
        QCOMPARE(d->selection, QSet<quint64>{b.id});
        appState().setTool(Tool::Pointer);
        // drag the group: both members move, the other object stays
        clickDoc(cv, QPointF(50, 40));
        QCOMPARE(d->selection, QSet<quint64>{gid});
        dragDoc(cv, QPointF(50, 40), QPointF(60, 50));
        QCOMPARE(d->find(a.id)->bounds().topLeft(), QPointF(30, 30));
        QCOMPARE(d->find(b.id)->bounds().topLeft(), QPointF(110, 30));
        QCOMPARE(d->find(c.id)->bounds().topLeft(), QPointF(200, 120));
        w.action(QStringLiteral("edit.undo"))->trigger();
        QCOMPARE(d->find(a.id)->bounds().topLeft(), QPointF(20, 20));
        QCOMPARE(d->find(b.id)->bounds().topLeft(), QPointF(100, 20));
        // the Layers tree shows the group with its members (top first), expandable
        auto *lp = w.findChild<LayersPanel *>();
        QVERIFY(lp);
        auto *tree = lp->findChild<QTreeWidget *>();
        QVERIFY(tree);
        QCoreApplication::processEvents();
        QTreeWidgetItem *gi = findTreeItem(tree, gid);
        QVERIFY(gi);
        QCOMPARE(gi->childCount(), 2);
        QCOMPARE(gi->child(0)->data(0, Qt::UserRole).toULongLong(), b.id);
        QCOMPARE(gi->child(1)->data(0, Qt::UserRole).toULongLong(), a.id);
        // picking a member in the Layers panel selects it inside the group
        gi->setExpanded(true);
        tree->scrollToItem(gi->child(1));
        const QRect r = tree->visualItemRect(gi->child(1));
        QVERIFY(r.isValid());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, QPoint(tree->columnViewportPosition(2) + 30, r.center().y()));
        QCOMPARE(d->selection, QSet<quint64>{a.id});
        QCOMPARE(d->enteredGroup, gid);
        QTest::keyClick(cv, Qt::Key_Escape);
        // Ungroup, undo, redo
        d->setSelection({gid});
        w.action(QStringLiteral("modify.ungroup"))->trigger();
        QCOMPARE(d->objects(0).size(), 3);
        QCOMPARE(d->selection, (QSet<quint64>{a.id, b.id}));
        w.action(QStringLiteral("edit.undo"))->trigger();
        QCOMPARE(d->objects(0).size(), 2);
        QCOMPARE(d->objects(0)[0].type, ObjType::Group);
        w.action(QStringLiteral("edit.redo"))->trigger();
        QCOMPARE(d->objects(0).size(), 3);
        // nested: group (a, b) then group that with c; Ctrl+Shift+G peels one level
        d->setSelection({a.id, b.id});
        grp->trigger();
        const quint64 inner = *d->selection.begin();
        d->setSelection({inner, c.id});
        grp->trigger();
        QCOMPARE(d->objects(0).size(), 1);
        const quint64 outer = d->objects(0)[0].id;
        clickDoc(cv, QPointF(50, 40));
        QCOMPARE(d->selection, QSet<quint64>{outer});
        dblClickDoc(cv, QPointF(50, 40));
        QCOMPARE(d->enteredGroup, outer);
        QCOMPARE(d->selection, QSet<quint64>{inner});
        dblClickDoc(cv, QPointF(50, 40));
        QCOMPARE(d->enteredGroup, inner);
        QCOMPARE(d->selection, QSet<quint64>{a.id});
        QTest::keyClick(cv, Qt::Key_Escape);
        QCOMPARE(d->enteredGroup, outer);
        QCOMPARE(d->selection, QSet<quint64>{inner});
        QTest::keyClick(cv, Qt::Key_Escape);
        QCOMPARE(d->enteredGroup, quint64(0));
        QCOMPARE(d->selection, QSet<quint64>{outer});
        QCoreApplication::processEvents();
        QTreeWidgetItem *oi = findTreeItem(tree, outer);
        QVERIFY(oi);
        QCOMPARE(oi->childCount(), 2);
        QTreeWidgetItem *ii = findTreeItem(tree, inner);
        QVERIFY(ii && ii->parent() == oi);
        QCOMPARE(ii->childCount(), 2);
    }

    // Paste Inside (Ctrl+Shift+V), mask enable / disable / link from the menus and the Layers mask row,
    // copy / paste / duplicate of groups (fresh ids all the way down), Convert to Bitmap.
    void maskCommandsAndClipboard() {
        MainWindow w;
        w.resize(1200, 850);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        DocumentView *v = w.addDocument(Document::createNew(200, 160, QColor(Qt::white)));
        QCoreApplication::processEvents();
        Document *d = v->doc();
        VObject content = shapeObj(d, ObjType::Rect, QRectF(20, 20, 120, 100), QColor(200, 30, 30), false);
        VObject circle = shapeObj(d, ObjType::Ellipse, QRectF(50, 40, 60, 60), QColor(0, 0, 0));
        d->addObject(content, 0, false); d->addObject(circle, 0, false);
        d->commit(QStringLiteral("setup"));
        d->setSelection({content.id});
        w.action(QStringLiteral("edit.cut"))->trigger();
        QCOMPARE(d->objects(0).size(), 1);
        d->setSelection({circle.id});
        // the Modify > Mask submenu and its context-menu mirror
        QMenu *mm = w.findChild<QMenu *>(QStringLiteral("modifyMaskMenu"));
        QVERIFY(mm);
        QStringList want = {QStringLiteral("mask.pasteasmask"), QStringLiteral("mask.pasteinside"), QStringLiteral("mask.groupasmask"),
                            QStringLiteral("mask.revealall"), QStringLiteral("mask.hideall"), QStringLiteral("mask.disable"),
                            QStringLiteral("mask.enable"), QStringLiteral("mask.link"), QStringLiteral("mask.delete")};
        for (const QString &id : want) QVERIFY2(mm->actions().contains(w.action(id)), qPrintable(id));
        QMenu *ctx = w.buildCanvasContextMenu(d);
        QMenu *cm = ctx->findChild<QMenu *>(QStringLiteral("ctxMask"));
        QVERIFY(cm);
        QCOMPARE(cm->actions().size(), mm->actions().size());
        QStringList ctxIds;
        for (QAction *a : ctx->actions()) ctxIds << a->objectName();
        QVERIFY(ctxIds.contains(QStringLiteral("modify.group")) && ctxIds.contains(QStringLiteral("modify.ungroup")));
        QVERIFY(ctxIds.contains(QStringLiteral("modify.converttobitmap")));
        QAction *pin = w.action(QStringLiteral("mask.pasteinside"));
        QCOMPARE(pin->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+V")));
        QVERIFY(pin->isEnabled());
        pin->trigger();
        QCOMPARE(d->objects(0).size(), 1);
        const VObject &g = d->objects(0)[0];
        QCOMPARE(g.type, ObjType::Group);
        QVERIFY(g.hasMask());
        const quint64 gid = g.id;
        QImage img = render::renderFrame(d->s, 0, true);
        QCOMPARE(img.pixelColor(80, 70).rgb(), QColor(200, 30, 30).rgb());   // inside the circle
        QCOMPARE(img.pixelColor(25, 25).rgb(), QColor(Qt::white).rgb());     // content outside the mask is hidden
        // the Property Inspector's mask controls (mode, enabled, linked), each one undoable step
        {
            d->setSelection({gid});
            auto *pi = w.findChild<PropertyInspector *>();
            pi->refresh();
            auto *mode = pi->findChild<QComboBox *>(QStringLiteral("MaskMode"));
            QVERIFY(mode && mode->isVisibleTo(pi));
            const int h0 = d->historyIndex();
            mode->setCurrentIndex(1);
            QCOMPARE(d->find(gid)->maskMode, 1);
            QCOMPARE(d->historyIndex(), h0 + 1);
            w.action(QStringLiteral("edit.undo"))->trigger();
            QCOMPARE(d->find(gid)->maskMode, 0);
        }
        w.action(QStringLiteral("mask.disable"))->trigger();
        QVERIFY(!d->find(gid)->maskEnabled);
        QCOMPARE(render::renderFrame(d->s, 0, true).pixelColor(25, 25).rgb(), QColor(200, 30, 30).rgb());
        w.action(QStringLiteral("edit.undo"))->trigger();
        QVERIFY(d->find(gid)->maskEnabled);
        // the Layers panel's mask row: column 1 toggles the link, column 0 the mask
        auto *tree = w.findChild<LayersPanel *>()->findChild<QTreeWidget *>();
        QCoreApplication::processEvents();
        QTreeWidgetItem *gi = findTreeItem(tree, gid);
        QVERIFY(gi);
        QTreeWidgetItem *mrow = gi->child(gi->childCount() - 1);
        QVERIFY(mrow->text(2).startsWith(QStringLiteral("Mask")));
        QVERIFY(!mrow->icon(1).isNull());   // linked
        QVERIFY(!mrow->data(2, Qt::DecorationRole).value<QPixmap>().isNull());
        gi->setExpanded(true);
        tree->scrollToItem(mrow);
        QRect r = tree->visualItemRect(mrow);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, QPoint(tree->columnViewportPosition(1) + 8, r.center().y()));
        QVERIFY(!d->find(gid)->maskLinked);
        QCoreApplication::processEvents();
        // move the group with the mask unlinked: the mask stays
        const QRectF maskAt = d->find(gid)->maskObj[0].bounds();
        d->setSelection({gid});
        d->find(gid)->moveBy(QPointF(5, 0));
        QCOMPARE(d->find(gid)->maskObj[0].bounds(), maskAt);
        w.action(QStringLiteral("edit.undo"))->trigger();   // (the move was not committed: undo the link toggle)
        QVERIFY(d->find(gid)->maskLinked);
        QCoreApplication::processEvents();
        // copy / paste the masked group: every nested id is new
        d->setSelection({gid});
        w.action(QStringLiteral("edit.copy"))->trigger();
        w.action(QStringLiteral("edit.paste"))->trigger();
        QCOMPARE(d->objects(0).size(), 2);
        QSet<quint64> ids;
        int total = 0;
        for (VObject &o : d->objects(0)) o.forEachNested([&](VObject &x) { ids.insert(x.id); ++total; });
        QCOMPARE(ids.size(), total);
        QVERIFY(d->objects(0)[1].hasMask());
        w.action(QStringLiteral("edit.duplicate"))->trigger();
        QCOMPARE(d->objects(0).size(), 3);
        // Convert to Bitmap: one bitmap that looks the same
        d->setSelection({gid});
        const QImage before = render::renderFrame(d->s, 0, true);
        for (int i = 2; i >= 1; --i) d->objects(0).removeAt(i);
        const QImage only = render::renderFrame(d->s, 0, true);
        w.action(QStringLiteral("modify.converttobitmap"))->trigger();
        QCOMPARE(d->objects(0).size(), 1);
        QCOMPARE(d->objects(0)[0].type, ObjType::Bitmap);
        QVERIFY(maxDiff(only, render::renderFrame(d->s, 0, true)) <= 2);
        (void)before;
    }

    // Trace Bitmap with "Output as a group": the traced paths arrive as one group (optionally on the new layer)
    void traceAsGroup() {
        MainWindow w;
        DocumentView *v = w.addDocument(Document::createNew(200, 120, Qt::white));
        Document *d = v->doc();
        QImage src(200, 120, QImage::Format_ARGB32_Premultiplied);
        src.fill(Qt::white);
        { QPainter p(&src); p.setPen(Qt::NoPen); p.setBrush(Qt::black); p.drawEllipse(QPointF(50, 60), 30, 30); p.drawRect(120, 30, 60, 60); }
        VObject bm;
        bm.id = d->newId(); bm.type = ObjType::Bitmap; bm.name = QStringLiteral("Art"); bm.image = src; bm.rect = QRectF(0, 0, 200, 120);
        d->addObject(bm);
        d->selectOnly(bm.id);
        d->commit(QStringLiteral("add"));
        bool hasBox = false;
        QVERIFY(w.traceBitmap([&](TraceDialog &dlg) {
            hasBox = dlg.findChild<QCheckBox *>(QStringLiteral("traceAsGroup")) != nullptr;
            trace::Options o;
            o.mode = trace::Mode::Bw;
            dlg.setOptions(o);
            dlg.setInsertOptions({false, false, false, true});
            return dlg.waitForPreview();
        }));
        QVERIFY(hasBox);
        QCOMPARE(d->objects(0).size(), 1);   // the bitmap was replaced by one group
        const VObject &g = d->objects(0)[0];
        QCOMPARE(g.type, ObjType::Group);
        QCOMPARE(g.name, QStringLiteral("Trace of Art"));
        QVERIFY2(g.children.size() >= 1 && g.children[0].type == ObjType::Path, qPrintable(QString::number(g.children.size())));
        QCOMPARE(d->selection, QSet<quint64>{g.id});
        w.action(QStringLiteral("edit.undo"))->trigger();
        QCOMPARE(d->objects(0)[0].type, ObjType::Bitmap);
        // with "new layer" too: one group on the new layer
        d->selectOnly(bm.id);
        const int layers0 = d->s.layers.size();
        QVERIFY(w.traceBitmap([&](TraceDialog &dlg) {
            trace::Options o; o.mode = trace::Mode::Bw; dlg.setOptions(o);
            dlg.setInsertOptions({true, true, false, true});
            return dlg.waitForPreview();
        }));
        QCOMPARE(d->s.layers.size(), layers0 + 1);
        const QVector<VObject> &nl = d->s.frames[0].layers.last();
        QCOMPARE(nl.size(), 1);
        QCOMPARE(nl[0].type, ObjType::Group);
    }

    // Find & Replace and the Batch Replace steps reach objects inside groups (members and masks)
    // README / release screenshots: only when LW_SHOTS_DIR is set (skipped in normal runs)
    void releaseScreenshots() {
        const QString dir = qEnvironmentVariable("LW_SHOTS_DIR");
        if (dir.isEmpty()) QSKIP("set LW_SHOTS_DIR to write the release screenshots");
        QDir().mkpath(dir);
        MainWindow w;
        w.resize(1280, 860);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        DocumentView *v = w.addDocument(Document::createNew(420, 300, QColor(Qt::white)));
        QCoreApplication::processEvents();
        Document *d = v->doc();
        auto add = [&](ObjType t, QRectF r, QColor c, const QString &name, int layer = 0) {
            VObject o = shapeObj(d, t, r, c);
            o.name = name;
            d->addObject(o, layer, false);
            return o.id;
        };
        const quint64 bg = add(ObjType::Rect, QRectF(20, 20, 380, 260), QColor(235, 240, 250), QStringLiteral("Backdrop"));
        Q_UNUSED(bg);
        const quint64 r1 = add(ObjType::Rect, QRectF(60, 60, 140, 90), QColor(220, 60, 50), QStringLiteral("Red tile"));
        const quint64 r2 = add(ObjType::Ellipse, QRectF(130, 90, 120, 120), QColor(40, 110, 220, 200), QStringLiteral("Blue disc"));
        const quint64 r3 = add(ObjType::Rect, QRectF(90, 150, 170, 60), QColor(250, 190, 40), QStringLiteral("Gold bar"));
        d->setSelection({r1, r2});
        const quint64 inner = d->groupSelection();
        QVERIFY(inner);
        d->find(inner)->name = QStringLiteral("Tiles");
        d->setSelection({inner, r3});
        const quint64 outer = d->groupSelection();
        QVERIFY(outer);
        d->find(outer)->name = QStringLiteral("Badge");
        const quint64 m = add(ObjType::Ellipse, QRectF(70, 55, 200, 170), QColor(0, 0, 0), QStringLiteral("Mask oval"));
        d->setSelection({outer, m});
        QVERIFY(d->groupAsMask());
        const int l2 = d->addLayer(QStringLiteral("Labels"));
        add(ObjType::Rect, QRectF(290, 220, 90, 40), QColor(60, 160, 90), QStringLiteral("Button"), l2);
        d->commit(QStringLiteral("setup"));
        for (const char *pn : {"Optimize", "Frames"}) if (auto *cp = w.panel(QLatin1String(pn))) cp->setCollapsed(true);
        d->setSelection({r2});   // a nested member: its groups open in the Layers panel
        drainEvents();
        QVERIFY(w.grab().save(dir + QStringLiteral("/groups-in-layers.png")));

        auto withMenus = [&](const QString &file, const QVector<QPair<QMenu *, QPoint>> &menus) {
            QImage img = w.grab().toImage();
            QPainter pt(&img);
            for (const auto &mp : menus) {
                mp.first->popup(w.mapToGlobal(mp.second));
                drainEvents();
                pt.drawPixmap(mp.second, mp.first->grab());
            }
            pt.end();
            for (auto it = menus.crbegin(); it != menus.crend(); ++it) it->first->hide();
            QVERIFY(img.save(dir + QLatin1Char('/') + file));
        };
        // Modify > Mask
        d->setSelection({d->objects(0).last().id});
        QMenu *mm = w.findChild<QMenu *>(QStringLiteral("modifyMaskMenu"));
        QVERIFY(mm);
        QMenu *modify = nullptr;
        QPoint modPos;
        for (QAction *a : w.menuBar()->actions())
            if (a->menu() && a->menu()->actions().contains(mm->menuAction())) {
                modify = a->menu();
                modPos = w.menuBar()->mapTo(&w, w.menuBar()->actionGeometry(a).bottomLeft());
            }
        QVERIFY(modify);
        modify->popup(w.mapToGlobal(modPos));
        drainEvents();
        modify->setActiveAction(mm->menuAction());
        drainEvents();
        const QRect ma = modify->actionGeometry(mm->menuAction());
        withMenus(QStringLiteral("mask-menu.png"), {{modify, modPos}, {mm, modPos + QPoint(modify->sizeHint().width() - 4, ma.top())}});
        // canvas context menu > Transform
        d->setSelection({r3});
        QMenu *ctx = w.buildCanvasContextMenu(d);
        QMenu *ct = ctx->findChild<QMenu *>(QStringLiteral("ctxTransform"));
        QVERIFY(ct);
        const QPoint cpos = v->mapTo(&w, v->rect().center() - QPoint(260, 200));
        ctx->popup(w.mapToGlobal(cpos));
        drainEvents();
        ctx->setActiveAction(ct->menuAction());
        drainEvents();
        const QRect ca = ctx->actionGeometry(ct->menuAction());
        withMenus(QStringLiteral("context-menu-transform.png"), {{ctx, cpos}, {ct, cpos + QPoint(ctx->sizeHint().width() - 4, ca.top())}});
        delete ctx;
        // File > New with the pop-up slider of the Width field open
        NewDocumentDialog nd(&w, false);
        nd.show();
        QVERIFY(QTest::qWaitForWindowExposed(&nd));
        drainEvents();
        QAbstractSpinBox *field = nullptr;
        for (QAbstractSpinBox *b : nd.findChildren<QAbstractSpinBox *>())
            if (b->isVisible() && (!field || b->mapTo(&nd, QPoint()).y() < field->mapTo(&nd, QPoint()).y())) field = b;
        QVERIFY(field);
        QWidget *arrow = field->findChild<QWidget *>(QStringLiteral("sliderArrow"));
        QVERIFY(arrow);
        QTest::mouseClick(arrow, Qt::LeftButton, {}, arrow->rect().center());
        QWidget *pop = field->findChild<QWidget *>(QStringLiteral("sliderPopup"));
        QVERIFY(pop);
        QTRY_VERIFY(pop->isVisible());
        drainEvents();
        const QPoint pp = pop->mapToGlobal(QPoint()) - nd.mapToGlobal(QPoint());
        QImage di = nd.grab().toImage();
        QRect all = di.rect().united(QRect(pp, pop->size())).adjusted(-12, -12, 12, 12);
        QImage out(all.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(nd.palette().color(QPalette::Window).darker(115));
        QPainter p2(&out);
        p2.drawImage(-all.topLeft(), di);
        p2.setPen(nd.palette().color(QPalette::Mid));
        p2.drawRect(QRect(-all.topLeft(), di.size()).adjusted(-1, -1, 0, 0));
        p2.drawPixmap(pp - all.topLeft(), pop->grab());
        p2.end();
        QVERIFY(out.save(dir + QStringLiteral("/new-dialog-slider-fields.png")));
        pop->hide();
        nd.reject();
    }

    void findReplaceInGroups() {
        std::unique_ptr<Document> d(Document::createNew(200, 120, Qt::white));
        VObject t1; t1.type = ObjType::Text; t1.text = QStringLiteral("hello world"); t1.rect = QRectF(10, 10, 80, 20); t1.id = d->newId();
        VObject r1 = shapeObj(d.get(), ObjType::Rect, QRectF(10, 40, 40, 30), QColor(255, 0, 0));
        VObject inner; inner.type = ObjType::Group; inner.id = d->newId(); inner.children = {t1, r1};
        VObject m = shapeObj(d.get(), ObjType::Ellipse, QRectF(0, 0, 100, 100), QColor(255, 0, 0));
        VObject outer; outer.type = ObjType::Group; outer.id = d->newId(); outer.children = {inner}; outer.maskObj = {m};
        VObject loose = shapeObj(d.get(), ObjType::Rect, QRectF(120, 40, 40, 30), QColor(255, 0, 0));
        d->addObject(outer, 0, false);
        d->addObject(loose, 0, false);
        fr::Query q;
        q.what = fr::What::Text; q.find = QStringLiteral("world"); q.replace = QStringLiteral("there");
        QVector<fr::Hit> hits = fr::findAll(*d, q, fr::Scope::Document);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits[0].objectId, t1.id);
        QCOMPARE(fr::replaceAll(*d, q, fr::Scope::Document), 1);
        QCOMPARE(d->find(t1.id)->text, QStringLiteral("hello there"));
        // colour: the nested rect, the mask and the loose rect; Selection scope via the selected outer group
        fr::Query c;
        c.what = fr::What::Color; c.from = QColor(255, 0, 0); c.to = QColor(0, 0, 255);
        QCOMPARE(fr::findAll(*d, c, fr::Scope::Document).size(), 3);
        const QSet<quint64> sel{outer.id};
        QCOMPARE(fr::findAll(*d, c, fr::Scope::Selection, &sel).size(), 2);
        // a locked group hides its contents
        d->find(inner.id)->locked = true;
        QCOMPARE(fr::findAll(*d, c, fr::Scope::Document).size(), 2);
        d->find(inner.id)->locked = false;
        // Batch (whole state)
        DocState st = d->s;
        QCOMPARE(fr::replaceAllInState(st, c), 3);
        int blue = 0;
        for (VObject &o : st.frames[0].layers[0]) o.forEachNested([&](VObject &x) { if (x.fillOn && x.fill == QColor(0, 0, 255)) ++blue; });
        QCOMPARE(blue, 3);
    }
};

QTEST_MAIN(V032)
#include "v032_test.moc"
