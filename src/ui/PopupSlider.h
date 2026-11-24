#pragma once
// Fireworks MX 2004-style pop-up slider fields.
//
// A text field showing the value (with its unit) and a small down-triangle button on
// the right. Pressing the triangle pops a small slider (vertical by default) up next to it in a
// frameless popup. Drag straight onto it in one motion and release to close it, or click
// the triangle to leave it open; the next click, Esc or a click elsewhere closes it.
// Typing, the arrow keys (Shift = steps of 10, Ctrl = snap to the Preferences increment) and the
// mouse wheel still work. Holding Shift while dragging the slider gives fine control (1/3 speed by
// default, v0.3.4; set in Preferences since 0.3.5). Holding Ctrl while dragging, scrolling or using
// the arrow keys snaps to multiples of Preferences > Ctrl snap increment (default 10, in the
// slider's own units). Shift fine control still applies when Ctrl is not held.
// The fields derive from QSpinBox / QDoubleSpinBox, so their API and signals are unchanged.
// The slider range can be narrower than the typed range (setSliderRange), as in Fireworks.

#include <QDoubleSpinBox>
#include <QPointer>
#include <QSpinBox>
#include <QWidget>

class SliderPopup;
class SliderArrow;

// Interface shared by the int and double fields; the popup talks to this.
class SliderTarget {
public:
    virtual ~SliderTarget() = default;
    virtual double sliderValue() const = 0;
    virtual void setSliderValue(double v) = 0;
    virtual double sliderLo() const = 0;
    virtual double sliderHi() const = 0;
    virtual QAbstractSpinBox *spinBox() = 0;
    SliderPopup *popup();               // created on first use
    SliderArrow *arrow() const { return m_arrow; }
    bool isPopupOpen() const;
    void openPopup(bool armedByPress);  // armedByPress: the triangle is held, drag goes to the slider
protected:
    void initSlider(QAbstractSpinBox *box);
    void layoutArrow(QAbstractSpinBox *box);
    bool handleModifierStep(QAbstractSpinBox *box, class QKeyEvent *e);
    SliderArrow *m_arrow = nullptr;
    QPointer<SliderPopup> m_popup;
    bool m_customRange = false;
    double m_lo = 0, m_hi = 100;
    // without setSliderRange, very wide typed ranges (e.g. 1..16000 px) get a usable slider span
    // of at most +-4096; typed / arrow-key values up to the real limits still work
    static double autoLo(double mn, double mx) { return (mn < -kAutoSpan && mx > -kAutoSpan) ? -kAutoSpan : mn; }
    static double autoHi(double mn, double mx) { return (mx > kAutoSpan && mn < kAutoSpan) ? kAutoSpan : mx; }
    static constexpr double kAutoSpan = 4096;
};

// App-wide notifications: a slider drag started / finished on some field. Owners use this
// to merge the live updates of one drag into a single undo step.
class PopupSliderHub : public QObject {
    Q_OBJECT
public:
    static PopupSliderHub *instance();
    // Preferences > Pop-up slider orientation (default Vertical since 0.3.2). Vertical pop-ups work like
    // Fireworks' vertical sliders: the value increases upward. Applies to every field at once,
    // including a pop-up that is open.
    static Qt::Orientation orientation();
    static void setOrientation(Qt::Orientation o);
    // Preferences > Shift fine control speed (0.3.5): the fraction of the normal speed used while Shift
    // is held - Shift-drag on every slider field, Shift+wheel / Shift+arrow steps on decimal fields.
    // Clamped to kMinFineFactor..1; default 1/3 (shown as 33%). Applies live, also to an open pop-up.
    static constexpr double kDefaultFineFactor = 0.33;
    static constexpr double kMinFineFactor = 0.05;
    static double fineFactor();
    static void setFineFactor(double f);
    // The Shift step of a decimal field: singleStep x fineFactor, rounded to the field's smallest
    // unit (10^-decimals) and never below it
    static double fineStep(const class QDoubleSpinBox *box);
    // Preferences > Ctrl snap increment (0.3.33): multiples that Ctrl+drag / scroll / arrow keys
    // snap to, in the slider's own units. Clamped to 1..100; default 10. Applies live.
    static constexpr int kDefaultSnapIncrement = 10;
    static int snapIncrement();
    static void setSnapIncrement(int n);
    static int snapNearest(int v, int step);
    static double snapNearest(double v, int step);
    static int snapStep(int v, int step, int dir);       // next multiple strictly in dir (+1 / -1)
    static double snapStep(double v, int step, int dir);
    // Gives a plain QSpinBox / QDoubleSpinBox that is not one of our fields (e.g. the ones inside
    // Qt's own QColorDialog / QInputDialog) the same look: no up/down arrows, a pop-up slider triangle.
    // Returns false if the box already is a slider field or was adopted before.
    static bool adopt(QAbstractSpinBox *box);
    // Shift fine-control drag and Ctrl snap on a plain QSlider (colour-picker alpha / hue, etc.).
    static bool adoptSlider(class QSlider *s);
    // Installs an application-wide filter that adopts every foreign spin box when it is polished,
    // so no number field in the app shows spin arrows, and the same modifier behaviour on QSlider.
    // Idempotent.
    static void installGlobalAdoption();
    // Set by SliderPopup::cancelDrag; MainWindow consumes it in interactionFinished
    // so endMerge can drop the undo step without a history digest.
    void markCancelled();
    bool consumeCancelled();
signals:
    void orientationChanged(Qt::Orientation o);
    void fineFactorChanged(double f);
    void snapIncrementChanged(int n);
    void interactionStarted(QWidget *field);
    void interactionFinished(QWidget *field);
private:
    bool m_cancelled = false;
};

class PopupSliderField : public QSpinBox, public SliderTarget {
    Q_OBJECT
public:
    explicit PopupSliderField(QWidget *parent = nullptr);
    PopupSliderField(int mn, int mx, const QString &suffix = {}, QWidget *parent = nullptr);
    void setSliderRange(double lo, double hi) { m_lo = lo; m_hi = hi; m_customRange = true; }
    double sliderValue() const override { return value(); }
    void setSliderValue(double v) override { setValue(qRound(v)); }
    double sliderLo() const override { return m_customRange ? m_lo : autoLo(minimum(), maximum()); }
    double sliderHi() const override { return m_customRange ? m_hi : autoHi(minimum(), maximum()); }
    QAbstractSpinBox *spinBox() override { return this; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
protected:
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
};

class PopupSliderDoubleField : public QDoubleSpinBox, public SliderTarget {
    Q_OBJECT
public:
    explicit PopupSliderDoubleField(QWidget *parent = nullptr);
    PopupSliderDoubleField(double mn, double mx, int decimals = 0, const QString &suffix = {}, QWidget *parent = nullptr);
    void setSliderRange(double lo, double hi) { m_lo = lo; m_hi = hi; m_customRange = true; }
    double sliderValue() const override { return value(); }
    void setSliderValue(double v) override;
    double sliderLo() const override { return m_customRange ? m_lo : autoLo(minimum(), maximum()); }
    double sliderHi() const override { return m_customRange ? m_hi : autoHi(minimum(), maximum()); }
    QAbstractSpinBox *spinBox() override { return this; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
protected:
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;   // Shift: a tenth of the step when the decimals allow
};

// The small down-triangle button inside the field.
class SliderArrow : public QWidget {
    Q_OBJECT
public:
    SliderArrow(SliderTarget *t, QWidget *parent);
    static constexpr int kWidth = 13;
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
private:
    SliderTarget *m_t;
    bool m_pressed = false;
};

// The frameless popup with the horizontal slider.
class SliderPopup : public QWidget {
    Q_OBJECT
public:
    explicit SliderPopup(SliderTarget *t);
    void open(bool armedByPress);
    QRect trackRect() const;            // the groove, in popup coordinates
    int knobX() const;                  // knob position along the groove (horizontal layout coordinates)
    QPoint knobPos() const;             // the knob's tip on the groove, in popup coordinates
    Qt::Orientation orientation() const { return m_vertical ? Qt::Vertical : Qt::Horizontal; }
    // drag handling, also fed by the arrow when it keeps the mouse grab
    // Shift held = fine control (PopupSliderHub::fineFactor() of the speed, 1/3 by default); Ctrl held
    // = snap to snapIncrement() multiples (Ctrl wins over Shift). Pressing or releasing a modifier
    // mid-drag re-anchors at the current value and position, so the value never jumps (except a
    // Ctrl press, which snaps to the nearest multiple). The unrounded value still accumulates.
    void dragTo(const QPointF &global, Qt::KeyboardModifiers mods);
    void dragTo(const QPointF &global);   // uses the current keyboard modifiers
    void finishDrag();
    // Right-click or Escape: restore the value from when the popup opened, no undo, close.
    void cancelDrag();
    bool isDragging() const { return m_dragging; }
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;
private:
    double fraction() const;
    void setFromX(double x);            // absolute, along the groove in horizontal layout coordinates
    void applyOrientation();
    QPointF toLayout(const QPointF &local) const;   // popup -> horizontal layout coordinates
    double axisOf(const QPointF &global) const { return m_vertical ? -global.y() : global.x(); }   // grows with the value
    void place();
    SliderTarget *m_t;
    bool m_vertical = false;
    bool m_dragging = false, m_relative = false, m_moved = false, m_armed = false;
    double m_pressGX = 0, m_startValue = 0, m_originalValue = 0;
    bool m_fine = false;
    bool m_snap = false;
    double m_fineUsed = 0;   // the factor the current fine anchor was made with
    double m_lastGX = 0, m_accum = 0;   // the last drag position and the unrounded value there
};
