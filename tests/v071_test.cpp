// 0.3.33: Ctrl snap increment (default 10) on sliders, and Shift fine control
// on the colour picker's alpha slider at the same speed as the number sliders.
#include "ui/AppState.h"
#include "ui/ColorPicker.h"
#include "ui/PopupSlider.h"
#include "ui/PreferencesDialog.h"
#include <QApplication>
#include <QColorDialog>
#include <QKeyEvent>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTest>
#include <QWheelEvent>
#include <cmath>

class V071Test : public QObject {
    Q_OBJECT

    static void drain() { QCoreApplication::processEvents(); }

    static QRect handleRect(QSlider *s)
    {
        QStyleOptionSlider opt;
        opt.initFrom(s);
        opt.orientation = s->orientation();
        opt.minimum = s->minimum();
        opt.maximum = s->maximum();
        opt.sliderPosition = s->sliderPosition();
        opt.sliderValue = s->value();
        opt.singleStep = s->singleStep();
        opt.pageStep = s->pageStep();
        return s->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, s);
    }

    static void dragSlider(QSlider *s, int dx, Qt::KeyboardModifiers mods)
    {
        const QPoint c = handleRect(s).center();
        const QPointF g0 = s->mapToGlobal(c);
        const QPointF g1 = s->mapToGlobal(c + QPoint(dx, 0));
        auto send = [&](QEvent::Type t, const QPointF &g, Qt::MouseButtons btns) {
            QMouseEvent ev(t, s->mapFromGlobal(g), g, Qt::LeftButton, btns, mods);
            QApplication::sendEvent(s, &ev);
        };
        send(QEvent::MouseButtonPress, g0, Qt::LeftButton);
        send(QEvent::MouseMove, g0 + QPointF(dx / 2.0, 0), Qt::LeftButton);
        send(QEvent::MouseMove, g1, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, g1, Qt::NoButton);
        drain();
    }

private slots:
    void cleanup()
    {
        Prefs p = appState().prefs;
        p.ctrlSnapIncrement = 10;
        p.shiftFineSpeed = 33;
        appState().setPrefs(p);
        PopupSliderHub::setSnapIncrement(10);
        PopupSliderHub::setFineFactor(PopupSliderHub::kDefaultFineFactor);
    }

    void snapHelpers()
    {
        QCOMPARE(PopupSliderHub::kDefaultSnapIncrement, 10);
        QCOMPARE(PopupSliderHub::snapIncrement(), 10);
        QCOMPARE(PopupSliderHub::snapNearest(17, 10), 20);
        QCOMPARE(PopupSliderHub::snapNearest(14, 10), 10);
        QCOMPARE(PopupSliderHub::snapStep(17, 10, 1), 20);
        QCOMPARE(PopupSliderHub::snapStep(20, 10, 1), 30);
        QCOMPARE(PopupSliderHub::snapStep(17, 10, -1), 10);
        QCOMPARE(PopupSliderHub::snapStep(20, 10, -1), 10);
    }

    void prefDefaultAndPersistence()
    {
        QCOMPARE(Prefs().ctrlSnapIncrement, 10);
        QCOMPARE(PopupSliderHub::snapIncrement(), 10);
        Prefs p = appState().prefs;
        p.ctrlSnapIncrement = 25;
        appState().setPrefs(p);
        QCOMPARE(PopupSliderHub::snapIncrement(), 25);
        appState().savePrefs();
        QCOMPARE(QSettings().value(QStringLiteral("prefs/ctrlSnapIncrement")).toInt(), 25);
        p.ctrlSnapIncrement = 7;
        appState().setPrefs(p);
        appState().loadPrefs();
        QCOMPARE(appState().prefs.ctrlSnapIncrement, 25);
        QCOMPARE(PopupSliderHub::snapIncrement(), 25);
        QSettings().setValue(QStringLiteral("prefs/ctrlSnapIncrement"), 1000);
        appState().loadPrefs();
        QCOMPARE(appState().prefs.ctrlSnapIncrement, 100);
        p = appState().prefs;
        p.ctrlSnapIncrement = 0;
        appState().setPrefs(p);
        QCOMPARE(appState().prefs.ctrlSnapIncrement, 1);
        QCOMPARE(PopupSliderHub::snapIncrement(), 1);
        p.ctrlSnapIncrement = 10;
        appState().setPrefs(p);
    }

    void preferencesField()
    {
        PreferencesDialog dlg;
        dlg.setPrefs(appState().prefs);
        auto *f = dlg.findChild<PopupSliderField *>(QStringLiteral("prefCtrlSnapIncrement"));
        QVERIFY(f);
        QCOMPARE(f->minimum(), 1);
        QCOMPARE(f->maximum(), 100);
        QCOMPARE(f->value(), 10);
        auto *fine = dlg.findChild<QWidget *>(QStringLiteral("prefShiftFineSpeed"));
        QVERIFY(fine && fine->parentWidget() == f->parentWidget()); // Editing page
        f->setValue(15);
        QCOMPARE(dlg.prefs().ctrlSnapIncrement, 15);
    }

    void popupSliderCtrlSnap()
    {
        PopupSliderHub::setSnapIncrement(10);
        PopupSliderField f(0, 100);
        f.setValue(17);
        f.show();
        drain();
        QTest::keyClick(&f, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(f.value(), 20);
        QTest::keyClick(&f, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(f.value(), 30);
        QTest::keyClick(&f, Qt::Key_Down, Qt::ControlModifier);
        QCOMPARE(f.value(), 20);

        f.setValue(17);
        QWheelEvent we(QPointF(5, 5), f.mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&f, &we);
        QCOMPARE(f.value(), 20);

        // Shift fine still works when Ctrl is not held (whole-number field: Shift+arrow = 10)
        f.setValue(20);
        QTest::keyClick(&f, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(f.value(), 30);

        // Ctrl drag snaps; Escape restores the original
        const Qt::Orientation was = PopupSliderHub::orientation();
        PopupSliderHub::setOrientation(Qt::Horizontal);
        f.setValue(17);
        f.openPopup(true);
        SliderPopup *pop = f.popup();
        QVERIFY(pop && pop->isDragging());
        const QPointF a = f.arrow()->mapToGlobal(QPointF(f.arrow()->width() / 2.0, f.arrow()->height()));
        pop->dragTo(a + QPointF(40, 0), Qt::ControlModifier);
        QVERIFY(f.value() % 10 == 0);
        QVERIFY(f.value() != 17);
        pop->cancelDrag();
        QCOMPARE(f.value(), 17);
        PopupSliderHub::setOrientation(was);
    }

    void alphaSliderShiftFineControl()
    {
        PopupSliderHub::setFineFactor(PopupSliderHub::kDefaultFineFactor);
        ColorPopup pop(QColor(10, 20, 30, 128), true, nullptr);
        pop.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pop));
        drain();
        auto *s = pop.findChild<QSlider *>(QStringLiteral("colorPopupAlpha"));
        QVERIFY(s);
        QVERIFY(s->isEnabled());
        QCOMPARE(s->minimum(), 0);
        QCOMPARE(s->maximum(), 100);

        s->setValue(20);
        dragSlider(s, 40, Qt::NoModifier);
        const int normal = s->value() - 20;
        QVERIFY2(normal > 8, qPrintable(QStringLiteral("unshifted drag moved by %1").arg(normal)));

        s->setValue(20);
        dragSlider(s, 40, Qt::ShiftModifier);
        const int fine = s->value() - 20;
        QVERIFY2(fine > 0, qPrintable(QStringLiteral("Shift drag did not move (fine=%1)").arg(fine)));
        QVERIFY2(fine < normal, qPrintable(QStringLiteral("Shift should be slower: normal %1 fine %2").arg(normal).arg(fine)));
        QVERIFY2(std::abs(fine * 3 - normal) <= qMax(4, normal / 4),
                 qPrintable(QStringLiteral("alpha Shift speed != number-slider fine factor: normal %1 fine %2")
                                .arg(normal).arg(fine)));
        pop.hide();
    }

    void alphaSliderCtrlSnap()
    {
        PopupSliderHub::setSnapIncrement(10);
        ColorPopup pop(QColor(10, 20, 30, 128), true, nullptr);
        pop.show();
        QVERIFY(QTest::qWaitForWindowExposed(&pop));
        auto *s = pop.findChild<QSlider *>(QStringLiteral("colorPopupAlpha"));
        QVERIFY(s);
        s->setFocus(Qt::OtherFocusReason);
        s->setValue(17);
        QTest::keyClick(s, Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(s->value(), 20);
        QTest::keyClick(s, Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(s->value(), 30);
        QTest::keyClick(s, Qt::Key_Left, Qt::ControlModifier);
        QCOMPARE(s->value(), 20);
        QCOMPARE(qRound(pop.workingColor().alphaF() * 100.0), 20);

        s->setValue(17);
        QWheelEvent we(QPointF(5, 5), s->mapToGlobal(handleRect(s).center()), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(s, &we);
        QCOMPARE(s->value(), 20);
        pop.hide();
    }

    void hueFieldCtrlSnap()
    {
        // Qt's QColorDialog has no QSlider; hue is a 0..359 spin box, adopted as a
        // pop-up slider field (same Ctrl snap as the other number sliders).
        QColorDialog dlg(QColor(255, 0, 0));
        dlg.setOption(QColorDialog::DontUseNativeDialog, true);
        dlg.setOption(QColorDialog::NoButtons, true);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        drain();
        PopupSliderHub::installGlobalAdoption();
        QSpinBox *hue = nullptr;
        for (QSpinBox *s : dlg.findChildren<QSpinBox *>()) {
            PopupSliderHub::adopt(s);
            if (s->minimum() == 0 && s->maximum() == 359) hue = s;
        }
        QVERIFY2(hue, "QColorDialog has no 0..359 hue field");
        PopupSliderHub::setSnapIncrement(10);
        hue->setFocus(Qt::OtherFocusReason);
        hue->setValue(17);
        QTest::keyClick(hue, Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(hue->value(), 20);
        dlg.hide();
    }
};

QTEST_MAIN(V071Test)
#include "v071_test.moc"
