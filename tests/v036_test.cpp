// v0.3.6 tests: Shift scales fill handles / Ctrl snaps; drag-drop & auto-trim; Black/White gradient
// default; Export Selected; stroke Category+Type & brush tips; shortcuts sets; font prefs; wide font popup.
#include "core/Document.h"
#include "core/Fills.h"
#include "core/BrushEngine.h"
#include "core/PixelEdit.h"
#include "core/Filters.h"
#include "io/ImageIO.h"
#include "ui/AppState.h"
#include "ui/Canvas.h"
#include "ui/DocumentView.h"
#include "ui/MainWindow.h"
#include "ui/PropertyInspector.h"
#include "ui/PreferencesDialog.h"
#include "ui/Shortcuts.h"
#include "ui/KeyboardPrefs.h"
#include "ui/GradientEditor.h"
#include <QAction>
#include <QFontComboBox>
#include "ui/WideFontCombo.h"
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QTemporaryDir>
#include <QImageWriter>
#include <QSettings>
#include <QTreeWidget>
#include <QtTest>
#include "Brand.h"
#include <cmath>

static void drainEvents() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

class V036 : public QObject {
    Q_OBJECT
    std::unique_ptr<MainWindow> w;
    Document *d = nullptr;
    Canvas *cv = nullptr;

    quint64 fillDoc(FillKind k) {
        d = Document::createNew(300, 240, Qt::white);
        DocumentView *v = w->addDocument(d);
        drainEvents();
        cv = v->canvas();
        cv->setZoom(1, QPointF(cv->width() / 2.0, cv->height() / 2.0));
        drainEvents();
        VObject o; o.type = ObjType::Rect; o.rect = QRectF(50, 50, 180, 120); o.id = d->newId(); o.fillOn = true;
        o.gradient = {{0, QColor(10, 10, 60)}, {0.45, QColor(240, 200, 40)}, {1, QColor(255, 255, 255)}};
        if (k == FillKind::Pattern) { fills::setFillKind(o, FillKind::Linear); o.fillKind = FillKind::Pattern; o.pattern = QStringLiteral("Bricks"); }
        else fills::setFillKind(o, k);
        d->addObject(o, 0, false);
        d->selectOnly(o.id);
        d->commit(QStringLiteral("add"));
        drainEvents();
        return o.id;
    }
    void closeDoc() {
        if (d) d->markClean();
        w->action(QStringLiteral("file.close"))->trigger();
        drainEvents();
        d = nullptr;
    }
    QPointF vp(QPointF p) const { return cv->toView(p); }
    void send(QEvent::Type t, QPointF doc, Qt::MouseButtons b, Qt::KeyboardModifiers m) {
        const QPointF v = vp(doc);
        QMouseEvent e(t, v, cv->mapToGlobal(v), t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, b, m);
        QApplication::sendEvent(cv, &e);
    }
    void drag(QPointF a, QPointF z, Qt::KeyboardModifiers m) {
        send(QEvent::MouseMove, a, Qt::NoButton, Qt::NoModifier);
        send(QEvent::MouseButtonPress, a, Qt::LeftButton, m);
        for (int k = 1; k <= 8; ++k) send(QEvent::MouseMove, a + (z - a) * (k / 8.0), Qt::LeftButton, m);
        send(QEvent::MouseButtonRelease, z, Qt::NoButton, m);
        drainEvents();
    }
    void handles(quint64 id, QPointF h[3]) { fills::fillHandles(*d->find(id), h); }
    static double len(QPointF p) { return std::hypot(p.x(), p.y()); }
    static double ang(QPointF p) { return std::atan2(p.y(), p.x()); }
    static double angDiff(double a, double b) { double x = std::remainder(a - b, 2 * M_PI); return std::abs(x); }
    static QList<FillKind> kinds() {
        return {FillKind::Linear, FillKind::Radial, FillKind::Cone, FillKind::Contour, FillKind::Satin, FillKind::Starburst, FillKind::Bars,
                FillKind::Ripples, FillKind::Waves, FillKind::Ellipse, FillKind::Rectangle, FillKind::Pattern};
    }
    static QList<int> dragHandles(FillKind k) {
        if (k == FillKind::Pattern || fills::usesSecondAxis(k)) return {1, 2};
        return {1};
    }
private slots:
    void initTestCase() {
        QCoreApplication::instance()->setProperty("lw_testing", true);
        w = std::make_unique<MainWindow>();
        w->resize(1000, 800);
        w->show();
        drainEvents();
    }
    void cleanupTestCase() { w.reset(); }

    void shiftScalesAllHandles_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("handle");
        QTest::addColumn<int>("tool");
        for (FillKind k : kinds())
            for (int h : dragHandles(k))
                for (Tool t : {Tool::Pointer, Tool::PaintBucket})
                    QTest::newRow(qPrintable(QStringLiteral("kind%1/h%2/%3").arg(int(k)).arg(h).arg(t == Tool::Pointer ? "pointer" : "bucket"))) << int(k) << h << int(t);
    }
    void shiftScalesAllHandles() {
        QFETCH(int, kind); QFETCH(int, handle); QFETCH(int, tool);
        const quint64 id = fillDoc(FillKind(kind));
        appState().setTool(Tool(tool));
        drainEvents();
        QPointF o[3]; handles(id, o);
        const int other = handle == 1 ? 2 : 1;
        const int hist = d->historyIndex();
        // stretch the dragged handle by 1.5 and turn it 20 degrees
        QLineF l(o[0], o[handle]); l.setLength(l.length() * 1.5); l.setAngle(l.angle() - 20);
        drag(o[handle], l.p2(), Qt::ShiftModifier);
        QPointF n[3]; handles(id, n);
        QVERIFY2(QLineF(n[handle], l.p2()).length() < 1.5, "the dragged handle follows the pointer");
        QVERIFY(QLineF(n[0], o[0]).length() < 0.01);   // the centre / start stays
        const double s = len(n[handle] - n[0]) / len(o[handle] - o[0]);
        const double so = len(n[other] - n[0]) / len(o[other] - o[0]);
        QVERIFY2(std::abs(s - 1.5) < 0.02 && std::abs(so - s) < 0.02, qPrintable(QStringLiteral("%1 %2").arg(s).arg(so)));
        // the same turn: the angle between the two axes is kept
        QVERIFY(angDiff(ang(n[other] - n[0]) - ang(n[handle] - n[0]), ang(o[other] - o[0]) - ang(o[handle] - o[0])) < 0.01);
        QCOMPARE(d->historyIndex(), hist + 1);   // one undo step
        d->undo();
        drainEvents();
        QPointF u[3]; handles(id, u);
        for (int i = 0; i < 3; ++i) QVERIFY(QLineF(u[i], o[i]).length() < 0.01);
        closeDoc();
    }
    // no modifier: exactly the old behaviour - only the dragged handle moves (two-axis kinds and
    // patterns); Ctrl snaps the dragged handle to 45 degrees; Ctrl+Shift snaps and scales all
    void plainAndCtrlDrags_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("handle");
        for (FillKind k : kinds())
            for (int h : dragHandles(k))
                QTest::newRow(qPrintable(QStringLiteral("kind%1/h%2").arg(int(k)).arg(h))) << int(k) << h;
    }
    void plainAndCtrlDrags() {
        QFETCH(int, kind); QFETCH(int, handle);
        const FillKind k = FillKind(kind);
        const quint64 id = fillDoc(k);
        appState().setTool(Tool::Pointer);
        drainEvents();
        const int other = handle == 1 ? 2 : 1;
        const bool twoAxis = k == FillKind::Pattern || fills::usesSecondAxis(k);
        QPointF o[3]; handles(id, o);
        const QPointF to = o[handle] + QPointF(-21, 13);
        drag(o[handle], to, Qt::NoModifier);
        QPointF n[3]; handles(id, n);
        QVERIFY(QLineF(n[handle], to).length() < 1.5);
        if (twoAxis) QVERIFY(QLineF(n[other], o[other]).length() < 0.01);
        else { // hidden second handle: square to the vector, length kept
            QVERIFY(std::abs(QPointF::dotProduct(n[1] - n[0], n[2] - n[0])) < 0.5);
            QVERIFY(std::abs(len(n[2] - n[0]) - len(o[2] - o[0])) < 0.5);
        }
        d->undo(); drainEvents();
        handles(id, o);
        // Ctrl: 45 degree steps around the start
        QLineF l(o[0], o[handle]); l.setAngle(l.angle() + 50); l.setLength(l.length() * 0.8);
        drag(o[handle], l.p2(), Qt::ControlModifier);
        handles(id, n);
        double a = QLineF(n[0], n[handle]).angle();
        QVERIFY2(std::abs(std::remainder(a, 45.0)) < 0.05, qPrintable(QString::number(a)));
        if (twoAxis) QVERIFY(QLineF(n[other], o[other]).length() < 0.01);
        d->undo(); drainEvents();
        // Ctrl+Shift: snapped and scaled together
        drag(o[handle], l.p2(), Qt::ControlModifier | Qt::ShiftModifier);
        handles(id, n);
        a = QLineF(n[0], n[handle]).angle();
        QVERIFY(std::abs(std::remainder(a, 45.0)) < 0.05);
        const double s = len(n[handle] - n[0]) / len(o[handle] - o[0]);
        QVERIFY(std::abs(len(n[other] - n[0]) / len(o[other] - o[0]) - s) < 0.02);
        closeDoc();
    }
    // the pattern's bar turn (grab a bar's middle): Ctrl = 15 degree steps (Shift until 0.3.6);
    // Shift alone no longer snaps (the turn already keeps both axes' lengths)
    void patternBarTurnCtrlSnaps() {
        for (const Qt::KeyboardModifiers m : {Qt::KeyboardModifiers(Qt::ControlModifier), Qt::KeyboardModifiers(Qt::ShiftModifier), Qt::KeyboardModifiers(Qt::NoModifier)}) {
            const quint64 id = fillDoc(FillKind::Pattern);
            appState().setTool(Tool::Pointer);
            drainEvents();
            QPointF o[3]; handles(id, o);
            const QPointF grab = (o[0] + o[1]) / 2;
            QLineF l(o[0], grab); l.setAngle(l.angle() - 22);
            const int hist = d->historyIndex();
            drag(grab, l.p2(), m);
            QPointF n[3]; handles(id, n);
            const double turned = QLineF(o[0], o[1]).angleTo(QLineF(n[0], n[1]));
            if (m & Qt::ControlModifier) QVERIFY2(std::abs(std::remainder(turned, 15.0)) < 0.05 && std::abs(std::remainder(turned, 360.0)) > 1, qPrintable(QString::number(turned)));
            else QVERIFY2(std::abs(std::remainder(turned + 22, 360.0)) < 1.0 || std::abs(std::remainder(turned - 22, 360.0)) < 1.0, qPrintable(QString::number(turned)));
            QVERIFY(std::abs(len(n[1] - n[0]) - len(o[1] - o[0])) < 0.05);
            QVERIFY(std::abs(len(n[2] - n[0]) - len(o[2] - o[0])) < 0.05);
            QCOMPARE(d->historyIndex(), hist + 1);
            closeDoc();
        }
    }
    // Shift alone never snaps the dragged handle's angle any more (it only scales)
    void shiftDoesNotSnap() {
        for (FillKind k : {FillKind::Linear, FillKind::Contour, FillKind::Pattern}) {
            const quint64 id = fillDoc(k);
            appState().setTool(Tool::Pointer);
            drainEvents();
            QPointF o[3]; handles(id, o);
            QLineF l(o[0], o[1]); l.setAngle(l.angle() + 17);
            drag(o[1], l.p2(), Qt::ShiftModifier);
            QPointF n[3]; handles(id, n);
            QVERIFY(QLineF(n[1], l.p2()).length() < 1.5);
            QVERIFY(std::abs(std::remainder(QLineF(n[0], n[1]).angle(), 45.0)) > 1);
            closeDoc();
        }
    }

    void autoTrimOnPasteAndImport() {
        d = Document::createNew(200, 200, Qt::white);
        w->addDocument(d);
        drainEvents();
        QImage img(40, 30, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        for (int y = 5; y < 20; ++y) for (int x = 8; x < 28; ++x) img.setPixel(x, y, qRgb(200, 40, 40));
        VObject o; o.id = d->newId(); o.type = ObjType::Bitmap; o.name = QStringLiteral("t");
        o.image = img; o.rect = QRectF(10, 10, 40, 30); o.fillOn = o.strokeOn = false;
        QVERIFY(pxedit::trimToAlpha(o));
        QCOMPARE(o.image.size(), QSize(20, 15));
        QCOMPARE(o.rect.topLeft(), QPointF(18, 15));
        // open-as-document keeps canvas size, trims the bitmap
        ImageSeq seq; seq.width = 40; seq.height = 30; seq.frames = {{img, 100}};
        std::unique_ptr<Document> doc(Document::fromImageSeq(seq));
        QCOMPARE(doc->s.width, 40);
        QCOMPARE(doc->s.height, 30);
        const VObject &b = doc->s.frames[0].layers[0][0];
        QCOMPARE(b.image.size(), QSize(20, 15));
        closeDoc();
    }
    void gradientDefaultsToBlackWhite() {
        VObject o; o.type = ObjType::Rect; o.rect = QRectF(0, 0, 50, 50); o.fill = Qt::red;
        fills::setFillKind(o, FillKind::Linear);
        QCOMPARE(o.gradient.size(), 2);
        QCOMPARE(o.gradient[0].color, QColor(Qt::black));
        QCOMPARE(o.gradient[1].color, QColor(Qt::white));
    }
    void strokeCategoryAndType() {
        QCOMPARE(fills::resolveStrokeKind(QStringLiteral("Air Brush")), QStringLiteral("airbrush.basic"));
        QCOMPARE(fills::resolveStrokeKind(QStringLiteral("airbrush.textured")), QStringLiteral("airbrush.textured"));
        QVERIFY(fills::strokeNamesInCategory(QStringLiteral("Calligraphy")).size() >= 4);
        QCOMPARE(fills::strokeKey(QStringLiteral("Calligraphy"), QStringLiteral("Bamboo")), QStringLiteral("calligraphy.bamboo"));
        QVERIFY(brush::tipNames(brush::Category::AirBrush).contains(QStringLiteral("Textured")));
        brush::Params p; brush::applyTip(p, brush::Category::AirBrush, 1);
        QVERIFY(!p.texture.isEmpty());
    }
    void shortcutSets() {
        shortcuts::Manager &sm = shortcuts::Manager::get();
        sm.captureDefaults(w.get());
        const QString id = sm.duplicate(brand::nameLower(), QStringLiteral("Test Set"));
        QVERIFY(!id.isEmpty());
        QVERIFY(!sm.set(id)->readOnly);
        sm.setSequences(QStringLiteral("file.new"), {QKeySequence(QStringLiteral("Ctrl+Alt+N"))});
        QCOMPARE(sm.sequences(QStringLiteral("file.new")).value(0).toString(), QStringLiteral("Ctrl+Alt+N"));
        // conflict with itself ignored
        QCOMPARE(sm.conflict(QKeySequence(QStringLiteral("Ctrl+Alt+N")), QStringLiteral("file.new")), QString());
        QVERIFY(sm.toHtml().contains(QStringLiteral("Test Set")) || sm.toHtml().contains(brand::name()));
        sm.setCurrent(brand::nameLower());
        sm.remove(id);
    }
    void fontPrefsAndWidePopup() {
        Prefs p = appState().prefs;
        p.useSystemFonts = true;
        p.useCustomFonts = false;
        appState().setPrefs(p);
        PreferencesDialog dlg(w.get());
        Prefs got = dlg.prefs();
        QVERIFY(got.useSystemFonts);
        // font combo popup width (sized on showPopup; measure via widenFontPopup)
        appState().setTool(Tool::Text);
        QCoreApplication::processEvents();
        QFontComboBox *font = w->findChild<WideFontComboBox *>();
        if (!font) font = w->findChild<QFontComboBox *>();
        if (font) {
            const int comboW = qMax(font->width(), font->sizeHint().width());
            font->resize(comboW, font->height());
            const int want = widenFontPopup(font);
            QVERIFY2(want >= qRound(font->width() * kFontPopupMinFactor),
                     qPrintable(QStringLiteral("font popup %1 < 2.3×%2").arg(want).arg(font->width())));
        }
    }
    void exportSelectedEnabled() {
        d = Document::createNew(100, 80, Qt::white);
        w->addDocument(d);
        drainEvents();
        auto *a = w->action(QStringLiteral("file.exportselected"));
        QVERIFY(a);
        QVERIFY(!a->isEnabled());
        VObject o; o.type = ObjType::Rect; o.rect = QRectF(10, 10, 40, 30); o.id = d->newId(); o.fillOn = true;
        d->addObject(o, 0, true);
        d->commit(QStringLiteral("add"));
        drainEvents();
        QVERIFY(a->isEnabled());
        closeDoc();
    }
    void similarityMath() {
        const QTransform t = Canvas::gradSimilarity(QPointF(10, 10), QPointF(20, 10), QPointF(10, 30));
        QVERIFY(QLineF(t.map(QPointF(20, 10)), QPointF(10, 30)).length() < 1e-9);
        QVERIFY(QLineF(t.map(QPointF(10, 15)), QPointF(0, 10)).length() < 1e-9);
        QVERIFY(Canvas::gradSimilarity(QPointF(1, 1), QPointF(1, 1), QPointF(5, 5)).isIdentity());
    }
};

QTEST_MAIN(V036)
#include "v036_test.moc"
