// v0.3.5 tests: Preferences > Shift fine control speed - the pop-up slider field in Preferences,
// Shift-drag speed (with no-jump re-anchoring, also when the speed changes mid-drag), Shift+wheel and
// Shift+arrow steps on decimal fields (scaled, never below the smallest unit), whole-number fields
// keep Shift+arrow = 10, saving / loading, and live application; Normal Map / AO wrap and clamp at the object edges (no seam column / row).
#include "core/FxExtra.h"
#include "core/Object.h"
#include "core/Render.h"
#include "ui/AppState.h"
#include "ui/PopupSlider.h"
#include "ui/PreferencesDialog.h"
#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QWheelEvent>
#include <QtTest>
#include <cmath>

static void drainEvents() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::processEvents();
    }
}

class V035 : public QObject {
    Q_OBJECT
    // a drag on the field's pop-up: returns (normal delta, fine delta) for the same 30 px of travel
    static QPair<double, double> dragDeltas(PopupSliderField &f) {
        f.openPopup(true);
        SliderPopup *pop = f.popup();
        const bool vert = pop->orientation() == Qt::Vertical;
        const QPointF a = f.arrow()->mapToGlobal(QPointF(f.arrow()->width() / 2.0, vert ? f.arrow()->height() / 2.0 : f.arrow()->height()));
        auto at = [&](double t) { return vert ? a - QPointF(0, t) : a + QPointF(t, 0); };
        pop->dragTo(at(-30), Qt::NoModifier);
        const int base = f.value();
        pop->dragTo(at(0), Qt::NoModifier);
        const double normal = f.value() - base;
        pop->dragTo(at(0), Qt::ShiftModifier);
        const int v0 = f.value();
        pop->dragTo(at(30), Qt::ShiftModifier);
        const double fine = f.value() - v0;
        pop->finishDrag();
        return {normal, fine};
    }

    // a seamlessly tileable heightmap (one sine period across, two down)
    static QImage tileHeight(int w, int h, bool flat = false) {
        QImage im(w, h, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double v = flat ? 0.5 : 0.5 + 0.25 * std::sin(2 * M_PI * x / w) + 0.2 * std::cos(4 * M_PI * y / h);
                const int g = qBound(0, int(std::lround(v * 255)), 255);
                im.setPixel(x, y, qRgb(g, g, g));
            }
        return im;
    }
    static QImage circShift(const QImage &im, int dx, int dy) {
        QImage o(im.size(), im.format());
        for (int y = 0; y < im.height(); ++y)
            for (int x = 0; x < im.width(); ++x)
                o.setPixel((x + dx) % im.width(), (y + dy) % im.height(), im.pixel(x, y));
        return o;
    }
    // the live effect as the canvas draws it, cropped back to the bitmap's own pixels
    static QImage live(const QImage &height, const LiveEffect &e, const QRect &clip, QImage *full = nullptr, QRect *areaOut = nullptr) {
        VObject o;
        o.type = ObjType::Bitmap;
        o.image = height;
        o.rect = QRectF(10, 7, height.width(), height.height());
        o.effects << e;
        QRect area;
        const QImage img = render::renderObject(o, &area, clip);
        if (full) *full = img;
        if (areaOut) *areaOut = area;
        return img.copy(QRect(QPoint(10, 7) - area.topLeft(), height.size()));
    }
    static int maxDiff(const QImage &a, const QImage &b) {
        int m = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x) {
                const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
                m = qMax(m, qMax(qMax(qAbs(qRed(p) - qRed(q)), qAbs(qGreen(p) - qGreen(q))), qMax(qAbs(qBlue(p) - qBlue(q)), qAbs(qAlpha(p) - qAlpha(q)))));
            }
        return m;
    }
private slots:
    // Normal Map / AO as live effects with 'Wrap edges': the render buffer's transparent padding
    // (and the visible-area clip) must not take part in the wrap - edge columns and rows must
    // match the interior, so a 2x2 tiling shows no seam in the gradient
    void normalMapWrapSeamless() {
        const QImage hm = tileHeight(64, 48);
        LiveEffect e = LiveEffect::make(EffectType::NormalMap);
        QVERIFY(e.flag2);   // wrap on by default
        for (int op = 0; op < 2; ++op) for (int src = 0; src < 2; ++src) {
            e.mode = op; e.mode2 = src;
            const QImage ref = fx2::normalMap(hm, op, e.amount, e.flag, true, src).convertToFormat(QImage::Format_ARGB32);
            // shifting a tileable heightmap shifts its normal map: no seam anywhere, edges included
            const QImage shifted = fx2::normalMap(circShift(hm, 32, 24), op, e.amount, e.flag, true, src).convertToFormat(QImage::Format_ARGB32);
            QCOMPARE(maxDiff(shifted, circShift(ref, 32, 24)), 0);
            QImage full; QRect area;
            const QImage got = live(hm, e, QRect(0, 0, 200, 200), &full, &area).convertToFormat(QImage::Format_ARGB32);
            QCOMPARE(maxDiff(got, ref), 0);
            // nothing is drawn in the buffer padding (alpha source would otherwise paint a frame)
            const QRect obj(QPoint(10, 7) - area.topLeft(), hm.size());
            for (int y = 0; y < full.height(); ++y) for (int x = 0; x < full.width(); ++x)
                if (!obj.contains(x, y)) QCOMPARE(qAlpha(full.pixel(x, y)), 0);
            // a view showing only part of the object still wraps over the whole object
            const QImage part = live(hm, e, QRect(40, 0, 200, 200)).convertToFormat(QImage::Format_ARGB32);
            QCOMPARE(maxDiff(part.copy(40, 0, 34, 48), ref.copy(40, 0, 34, 48)), 0);
        }
        // AO wraps the same way
        LiveEffect ao = LiveEffect::make(EffectType::AmbientOcclusion);
        QVERIFY(ao.flag2);
        const QImage aref = fx2::ambientOcclusion(hm, ao.width, ao.amount, true, ao.mode2).convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(maxDiff(live(hm, ao, QRect(0, 0, 200, 200)).convertToFormat(QImage::Format_ARGB32), aref), 0);
        QCOMPARE(maxDiff(fx2::ambientOcclusion(circShift(hm, 32, 24), ao.width, ao.amount, true, ao.mode2).convertToFormat(QImage::Format_ARGB32), circShift(aref, 32, 24)), 0);
    }
    // wrap off: edges are clamped at the object's own edge, never against the transparent padding
    void normalMapClampedEdges() {
        LiveEffect e = LiveEffect::make(EffectType::NormalMap);
        e.flag2 = false;
        const QImage flat = live(tileHeight(32, 20, true), e, QRect(0, 0, 200, 200)).convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < flat.height(); ++y) for (int x = 0; x < flat.width(); ++x)
            QCOMPARE(flat.pixel(x, y), flat.pixel(16, 10));   // flat height: flat normals, edges too
        const QImage hm = tileHeight(64, 48);
        QCOMPARE(maxDiff(live(hm, e, QRect(0, 0, 200, 200)).convertToFormat(QImage::Format_ARGB32),
                         fx2::normalMap(hm, e.mode, e.amount, e.flag, false, e.mode2).convertToFormat(QImage::Format_ARGB32)), 0);
        LiveEffect ao = LiveEffect::make(EffectType::AmbientOcclusion);
        ao.flag2 = false;
        const QImage af = live(tileHeight(32, 20, true), ao, QRect(0, 0, 200, 200)).convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < af.height(); ++y) for (int x = 0; x < af.width(); ++x)
            QCOMPARE(af.pixel(x, y), af.pixel(16, 10));
    }
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("LibreworksV035Test"));
        QCoreApplication::instance()->setProperty("lw_testing", true);
        QSettings().clear();
    }
    void cleanupTestCase() { QSettings().clear(); }

    void defaultsAndPersistence() {
        QCOMPARE(Prefs().shiftFineSpeed, 33);
        QCOMPARE(PopupSliderHub::fineFactor(), 0.33);
        Prefs p = appState().prefs;
        p.shiftFineSpeed = 12;
        appState().setPrefs(p);
        QCOMPARE(PopupSliderHub::fineFactor(), 0.12);   // live, no restart
        appState().savePrefs();
        QCOMPARE(QSettings().value(QStringLiteral("prefs/shiftFineSpeedPercent")).toInt(), 12);
        p.shiftFineSpeed = 50;
        appState().setPrefs(p);
        appState().loadPrefs();
        QCOMPARE(appState().prefs.shiftFineSpeed, 12);
        QCOMPARE(PopupSliderHub::fineFactor(), 0.12);
        // out-of-range values are clamped (settings file and API)
        QSettings().setValue(QStringLiteral("prefs/shiftFineSpeedPercent"), 1000);
        appState().loadPrefs();
        QCOMPARE(appState().prefs.shiftFineSpeed, 100);
        p.shiftFineSpeed = 0;
        appState().setPrefs(p);
        QCOMPARE(appState().prefs.shiftFineSpeed, 5);
        QCOMPARE(PopupSliderHub::fineFactor(), 0.05);
        PopupSliderHub::setFineFactor(std::nan(""));
        QCOMPARE(PopupSliderHub::fineFactor(), PopupSliderHub::kDefaultFineFactor);
        p.shiftFineSpeed = 33;
        appState().setPrefs(p);
    }

    void preferencesField() {
        PreferencesDialog dlg;
        dlg.setPrefs(appState().prefs);
        auto *f = dlg.findChild<PopupSliderField *>(QStringLiteral("prefShiftFineSpeed"));
        QVERIFY(f);
        QCOMPARE(f->buttonSymbols(), QAbstractSpinBox::NoButtons);   // pop-up slider field, no spin arrows
        QVERIFY(f->arrow());
        QCOMPARE(f->minimum(), 5);
        QCOMPARE(f->maximum(), 100);
        QCOMPARE(f->suffix(), QStringLiteral("%"));
        QCOMPARE(f->value(), 33);
        // next to the slider orientation option, on the same page
        auto *orient = dlg.findChild<QWidget *>(QStringLiteral("prefSliderOrientation"));
        QVERIFY(orient && orient->parentWidget() == f->parentWidget());
        f->setValue(60);
        QCOMPARE(dlg.prefs().shiftFineSpeed, 60);
        appState().setPrefs(dlg.prefs());
        QCOMPARE(PopupSliderHub::fineFactor(), 0.6);
        Prefs p = appState().prefs;
        p.shiftFineSpeed = 33;
        appState().setPrefs(p);
    }

    void shiftDragSpeed() {
        for (Qt::Orientation orient : {Qt::Vertical, Qt::Horizontal}) {
            const Qt::Orientation was = PopupSliderHub::orientation();
            PopupSliderHub::setOrientation(orient);
            for (int pct : {10, 33, 50, 100}) {
                PopupSliderHub::setFineFactor(pct / 100.0);
                PopupSliderField f(0, 1000);
                f.setValue(300);
                f.show();
                drainEvents();
                const auto [normal, fine] = dragDeltas(f);
                QVERIFY(normal > 20);
                QVERIFY2(std::abs(fine - normal * pct / 100.0) <= 2,
                         qPrintable(QStringLiteral("%1%: normal %2 fine %3").arg(pct).arg(normal).arg(fine)));
            }
            PopupSliderHub::setOrientation(was);
        }
        // the speed changed (in Preferences) during a Shift drag: no jump, the new speed from there on
        PopupSliderHub::setFineFactor(0.5);
        PopupSliderField f(0, 1000);
        f.setValue(500);
        f.show();
        drainEvents();
        f.openPopup(true);
        SliderPopup *pop = f.popup();
        const bool vert = pop->orientation() == Qt::Vertical;
        const QPointF a = f.arrow()->mapToGlobal(QPointF(f.arrow()->width() / 2.0, vert ? f.arrow()->height() / 2.0 : f.arrow()->height()));
        auto at = [&](double t) { return vert ? a - QPointF(0, t) : a + QPointF(t, 0); };
        pop->dragTo(at(-30), Qt::NoModifier);
        pop->dragTo(at(0), Qt::ShiftModifier);
        const int start = f.value();
        pop->dragTo(at(40), Qt::ShiftModifier);
        const int half = f.value();
        PopupSliderHub::setFineFactor(0.1);
        pop->dragTo(at(40), Qt::ShiftModifier);
        QCOMPARE(f.value(), half);
        pop->dragTo(at(80), Qt::ShiftModifier);
        const int tenth = f.value() - half;
        QVERIFY2(half - start > 10 && std::abs(tenth * 5 - (half - start)) <= 5, qPrintable(QStringLiteral("half %1, tenth %2").arg(half - start).arg(tenth)));
        pop->finishDrag();
        PopupSliderHub::setFineFactor(0.33);
    }

    void shiftWheelAndArrows() {
        auto wheel = [](QWidget *w, int dy) {
            QWheelEvent we(QPointF(5, 5), w->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, dy), Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(w, &we);
        };
        PopupSliderDoubleField g(0, 100, 2);   // step 0.1, smallest unit 0.01
        g.show();
        drainEvents();
        g.setFocus();
        struct Case { int pct; double step; };
        for (const Case c : {Case{33, 0.03}, Case{50, 0.05}, Case{100, 0.1}, Case{5, 0.01}, Case{10, 0.01}}) {
            PopupSliderHub::setFineFactor(c.pct / 100.0);
            g.setValue(10);
            wheel(&g, 120);
            QVERIFY2(std::abs(g.value() - (10 + c.step)) < 1e-9, qPrintable(QStringLiteral("%1%: wheel %2").arg(c.pct).arg(g.value())));
            wheel(&g, -240);   // two notches down
            QVERIFY(std::abs(g.value() - (10 - c.step)) < 1e-9);
            g.setValue(10);
            QTest::keyClick(&g, Qt::Key_Up, Qt::ShiftModifier);
            QVERIFY2(std::abs(g.value() - (10 + c.step)) < 1e-9, qPrintable(QStringLiteral("%1%: arrow %2").arg(c.pct).arg(g.value())));
            QTest::keyClick(&g, Qt::Key_Down, Qt::ShiftModifier);
            QVERIFY(std::abs(g.value() - 10) < 1e-9);
        }
        // the minimum is the field's smallest unit: one decimal, step 1 at 5% -> 0.1 (not 0.05)
        PopupSliderHub::setFineFactor(0.05);
        PopupSliderDoubleField one(0, 100, 1);
        one.setSingleStep(1);
        one.show();
        one.setFocus();
        one.setValue(10);
        QTest::keyClick(&one, Qt::Key_Up, Qt::ShiftModifier);
        QVERIFY(std::abs(one.value() - 10.1) < 1e-9);
        // inside an open pop-up, Shift+arrows take the same fine step
        PopupSliderHub::setFineFactor(0.5);
        g.setValue(10);
        g.openPopup(false);
        QTest::keyClick(g.popup(), Qt::Key_Up, Qt::ShiftModifier);
        QVERIFY(std::abs(g.value() - 10.05) < 1e-9);
        QTest::keyClick(g.popup(), Qt::Key_Up);
        QVERIFY(std::abs(g.value() - 10.15) < 1e-9);
        g.popup()->close();
        // whole-number fields: Shift+arrow is still 10 steps
        PopupSliderField i(0, 100);
        i.show();
        i.setFocus();
        i.setValue(50);
        QTest::keyClick(&i, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(i.value(), 60);
        PopupSliderHub::setFineFactor(0.33);
    }
};

QTEST_MAIN(V035)
#include "v035_test.moc"
