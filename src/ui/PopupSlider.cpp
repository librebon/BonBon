#include "Theme.h"
#include "PopupSlider.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------- hub
PopupSliderHub *PopupSliderHub::instance() {
    static PopupSliderHub *h = new PopupSliderHub;
    return h;
}

void PopupSliderHub::markCancelled() { m_cancelled = true; }

bool PopupSliderHub::consumeCancelled() {
    const bool c = m_cancelled;
    m_cancelled = false;
    return c;
}

namespace {
Qt::Orientation g_orientation = Qt::Vertical;
double g_fineFactor = PopupSliderHub::kDefaultFineFactor;
int g_snapIncrement = PopupSliderHub::kDefaultSnapIncrement;
}

double PopupSliderHub::fineFactor() { return g_fineFactor; }

void PopupSliderHub::setFineFactor(double f) {
    f = std::isfinite(f) ? std::clamp(f, kMinFineFactor, 1.0) : kDefaultFineFactor;
    if (f == g_fineFactor) return;
    g_fineFactor = f;
    emit instance()->fineFactorChanged(f);
}

double PopupSliderHub::fineStep(const QDoubleSpinBox *box) {
    const double unit = std::pow(10.0, -box->decimals());
    return qMax(unit, std::round(box->singleStep() * g_fineFactor / unit) * unit);
}

int PopupSliderHub::snapIncrement() { return g_snapIncrement; }

void PopupSliderHub::setSnapIncrement(int n) {
    n = qBound(1, n, 100);
    if (n == g_snapIncrement) return;
    g_snapIncrement = n;
    emit instance()->snapIncrementChanged(n);
}

int PopupSliderHub::snapNearest(int v, int step) {
    step = qMax(1, step);
    return int(std::round(double(v) / step)) * step;
}
double PopupSliderHub::snapNearest(double v, int step) {
    step = qMax(1, step);
    return std::round(v / step) * step;
}
int PopupSliderHub::snapStep(int v, int step, int dir) {
    step = qMax(1, step);
    if (dir > 0) return int(std::floor(double(v) / step) + 1) * step;
    if (dir < 0) return int(std::ceil(double(v) / step) - 1) * step;
    return snapNearest(v, step);
}
double PopupSliderHub::snapStep(double v, int step, int dir) {
    step = qMax(1, step);
    if (dir > 0) return (std::floor(v / step) + 1) * step;
    if (dir < 0) return (std::ceil(v / step) - 1) * step;
    return snapNearest(v, step);
}

Qt::Orientation PopupSliderHub::orientation() { return g_orientation; }

void PopupSliderHub::setOrientation(Qt::Orientation o) {
    if (o == g_orientation) return;
    g_orientation = o;
    emit instance()->orientationChanged(o);
}

// ---------------------------------------------------------------- adoption of foreign spin boxes
namespace {
bool spinWheel(QAbstractSpinBox *box, QWheelEvent *e); // defined with handleModifierStep
class AdoptedSlider : public QObject, public SliderTarget {
public:
    explicit AdoptedSlider(QAbstractSpinBox *box) : QObject(box), m_box(box) {
        setObjectName(QStringLiteral("adoptedSlider"));
        box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        m_arrow = new SliderArrow(this, box);
        m_arrow->setObjectName(QStringLiteral("sliderArrow"));
        m_arrow->setToolTip(QStringLiteral("Click or drag for a slider"));
        box->setMinimumWidth(qMax(box->minimumWidth(), box->minimumSizeHint().width() + SliderArrow::kWidth));
        box->installEventFilter(this);
        layoutArrow(box);
    }
    double sliderValue() const override {
        if (auto *d = qobject_cast<QDoubleSpinBox *>(m_box)) return d->value();
        if (auto *i = qobject_cast<QSpinBox *>(m_box)) return i->value();
        return 0;
    }
    void setSliderValue(double v) override {
        if (auto *d = qobject_cast<QDoubleSpinBox *>(m_box)) { const double p = std::pow(10.0, d->decimals()); d->setValue(std::round(v * p) / p); }
        else if (auto *i = qobject_cast<QSpinBox *>(m_box)) i->setValue(qRound(v));
    }
    double sliderLo() const override { double mn, mx; range(mn, mx); return autoLo(mn, mx); }
    double sliderHi() const override { double mn, mx; range(mn, mx); return autoHi(mn, mx); }
    QAbstractSpinBox *spinBox() override { return m_box; }
protected:
    bool eventFilter(QObject *o, QEvent *e) override {
        if (o == m_box) {
            switch (e->type()) {
            case QEvent::Resize: case QEvent::Show: case QEvent::LayoutRequest: case QEvent::StyleChange:
                layoutArrow(m_box); break;
            case QEvent::EnabledChange: if (m_arrow) m_arrow->update(); break;
            case QEvent::KeyPress: if (handleModifierStep(m_box, static_cast<QKeyEvent *>(e))) return true; break;
            case QEvent::Wheel: if (spinWheel(m_box, static_cast<QWheelEvent *>(e))) return true; break;
            default: break;
            }
        }
        return false;
    }
private:
    void range(double &mn, double &mx) const {
        mn = 0; mx = 100;
        if (auto *d = qobject_cast<QDoubleSpinBox *>(m_box)) { mn = d->minimum(); mx = d->maximum(); }
        else if (auto *i = qobject_cast<QSpinBox *>(m_box)) { mn = i->minimum(); mx = i->maximum(); }
    }
    QAbstractSpinBox *m_box;
};

class AdoptionFilter : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *o, QEvent *e) override {
        if (e->type() == QEvent::Polish && o->isWidgetType()) {
            if (auto *b = qobject_cast<QAbstractSpinBox *>(o)) PopupSliderHub::adopt(b);
            if (auto *s = qobject_cast<QSlider *>(o)) PopupSliderHub::adoptSlider(s);
        }
        return false;
    }
};
}

bool PopupSliderHub::adopt(QAbstractSpinBox *box) {
    if (!box || dynamic_cast<SliderTarget *>(box)) return false;
    if (box->findChild<QObject *>(QStringLiteral("adoptedSlider"), Qt::FindDirectChildrenOnly)) return false;
    // only plain number boxes; date/time edits and other spin boxes are left alone
    if (!qobject_cast<QSpinBox *>(box) && !qobject_cast<QDoubleSpinBox *>(box)) return false;
    new AdoptedSlider(box);
    return true;
}

void PopupSliderHub::installGlobalAdoption() {
    static QPointer<AdoptionFilter> f;
    if (f || !qApp) return;
    f = new AdoptionFilter(qApp);
    qApp->installEventFilter(f);
}

// Shift fine-control drag + Ctrl snap on a plain QSlider (colour-picker alpha / hue, vibrancy, ...).
namespace {
int sliderSpan(QSlider *s) {
    QStyleOptionSlider opt;
    opt.initFrom(s);
    opt.orientation = s->orientation();
    opt.minimum = s->minimum();
    opt.maximum = s->maximum();
    opt.sliderPosition = s->sliderPosition();
    opt.sliderValue = s->value();
    opt.singleStep = s->singleStep();
    opt.pageStep = s->pageStep();
    opt.upsideDown = s->invertedAppearance() != (s->orientation() == Qt::Vertical);
    const QRect gr = s->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, s);
    const QRect hd = s->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, s);
    if (s->orientation() == Qt::Horizontal) return qMax(1, gr.width() - hd.width());
    return qMax(1, gr.height() - hd.height());
}
double sliderAxis(QSlider *s, const QPointF &global) {
    const QPointF local = s->mapFromGlobal(global);
    // value grows to the right, or upward on a vertical slider
    return s->orientation() == Qt::Vertical ? -local.y() : local.x();
}

class SliderMods : public QObject {
public:
    explicit SliderMods(QSlider *s) : QObject(s), m_s(s) {
        setObjectName(QStringLiteral("sliderMods"));
        s->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *o, QEvent *e) override {
        if (o != m_s || !m_s->isEnabled()) return false;
        switch (e->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) break;
            m_press = true;
            m_moved = false;
            m_start = m_s->value();
            m_accum = m_start;
            m_pressAX = sliderAxis(m_s, me->globalPosition());
            m_lastAX = m_pressAX;
            m_fine = me->modifiers() & Qt::ShiftModifier;
            m_snap = me->modifiers() & Qt::ControlModifier;
            m_fineUsed = PopupSliderHub::fineFactor();
            m_relative = m_fine || m_snap;
            if (m_relative) return false; // let QSlider take the press; moves are intercepted
            break;
        }
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(e);
            if (!m_press || !(me->buttons() & Qt::LeftButton)) break;
            return drag(me->globalPosition(), me->modifiers());
        }
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() == Qt::LeftButton) m_press = false;
            break;
        }
        case QEvent::Wheel: {
            auto *we = static_cast<QWheelEvent *>(e);
            const QPoint ad = we->angleDelta();
            const int d = ad.y() ? ad.y() : ad.x();
            if (!d) break;
            const int dir = d > 0 ? 1 : -1;
            const int notches = qMax(1, std::abs(d) / 120);
            if (we->modifiers() & Qt::ControlModifier) {
                int v = m_s->value();
                for (int n = 0; n < notches; ++n)
                    v = qBound(m_s->minimum(), PopupSliderHub::snapStep(v, PopupSliderHub::snapIncrement(), dir), m_s->maximum());
                m_s->setValue(v);
                we->accept();
                return true;
            }
            break;
        }
        case QEvent::KeyPress: {
            auto *ke = static_cast<QKeyEvent *>(e);
            if (!(ke->modifiers() & Qt::ControlModifier)) break;
            int dir = 0;
            if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Right) dir = 1;
            else if (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Left) dir = -1;
            if (!dir) break;
            const int v = qBound(m_s->minimum(),
                                 PopupSliderHub::snapStep(m_s->value(), PopupSliderHub::snapIncrement(), dir),
                                 m_s->maximum());
            m_s->setValue(v);
            ke->accept();
            return true;
        }
        default: break;
        }
        return false;
    }
private:
    bool drag(const QPointF &global, Qt::KeyboardModifiers mods) {
        const double ax = sliderAxis(m_s, global);
        if (std::abs(ax - m_pressAX) >= 2) m_moved = true;
        const bool snap = mods & Qt::ControlModifier;
        const bool fine = !snap && (mods & Qt::ShiftModifier);
        const double factor = PopupSliderHub::fineFactor();
        const double lo = m_s->minimum(), hi = m_s->maximum();
        if (fine != m_fine || snap != m_snap || (fine && factor != m_fineUsed)) {
            m_accum = m_s->value();
            m_start = qBound(lo, m_accum, hi);
            m_pressAX = m_lastAX;
            m_relative = true;
            m_fine = fine;
            m_snap = snap;
            m_fineUsed = factor;
            if (snap) {
                m_accum = PopupSliderHub::snapNearest(m_accum, PopupSliderHub::snapIncrement());
                m_s->setValue(int(qBound(lo, m_accum, hi)));
            }
        }
        m_lastAX = ax;
        if (!m_relative && !fine && !snap) return false;
        const double start = qBound(lo, m_start, hi);
        const double v = start + (ax - m_pressAX) / double(sliderSpan(m_s)) * (hi - lo) * (m_fine ? m_fineUsed : 1.0);
        m_accum = qBound(lo, v, hi);
        double out = m_accum;
        if (m_snap) out = PopupSliderHub::snapNearest(m_accum, PopupSliderHub::snapIncrement());
        m_s->setValue(int(qBound(lo, out, hi)));
        return true;
    }
    QSlider *m_s;
    bool m_press = false, m_moved = false, m_relative = false, m_fine = false, m_snap = false;
    double m_pressAX = 0, m_lastAX = 0, m_start = 0, m_accum = 0, m_fineUsed = 0;
};
} // namespace

bool PopupSliderHub::adoptSlider(QSlider *s) {
    if (!s) return false;
    if (s->findChild<QObject *>(QStringLiteral("sliderMods"), Qt::FindDirectChildrenOnly)) return false;
    new SliderMods(s);
    return true;
}

// ---------------------------------------------------------------- SliderTarget
SliderPopup *SliderTarget::popup() {
    if (!m_popup) m_popup = new SliderPopup(this);
    return m_popup;
}

bool SliderTarget::isPopupOpen() const { return m_popup && m_popup->isVisible(); }

void SliderTarget::openPopup(bool armedByPress) {
    if (!spinBox()->isEnabled() || spinBox()->isReadOnly()) return;
    popup()->open(armedByPress);
}

void SliderTarget::initSlider(QAbstractSpinBox *box) {
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);
    box->setKeyboardTracking(false);
    box->setAccelerated(true);
    m_arrow = new SliderArrow(this, box);
    m_arrow->setObjectName(QStringLiteral("sliderArrow"));
    m_arrow->setToolTip(QStringLiteral("Click or drag for a slider"));
}

void SliderTarget::layoutArrow(QAbstractSpinBox *box) {
    if (!m_arrow) return;
    const int fw = box->hasFrame() ? 1 : 0;
    const QRect r(box->width() - fw - SliderArrow::kWidth, fw, SliderArrow::kWidth, box->height() - 2 * fw);
    m_arrow->setGeometry(r);
    m_arrow->raise();
    // keep the text clear of the triangle
    if (QLineEdit *le = box->findChild<QLineEdit *>()) {
        QRect g = le->geometry();
        if (g.right() >= r.left()) {
            g.setRight(r.left() - 1);
            le->setGeometry(g);
        }
    }
}

namespace {
int wheelDir(const QWheelEvent *e) {
    const QPoint ad = e->angleDelta();
    const int d = ad.y() ? ad.y() : ad.x();
    if (!d) return 0;
    return (d > 0 ? 1 : -1) * qMax(1, std::abs(d) / 120);
}
bool applySnapToSpin(QAbstractSpinBox *box, int dir) {
    const int step = PopupSliderHub::snapIncrement();
    if (auto *d = qobject_cast<QDoubleSpinBox *>(box)) {
        d->setValue(qBound(d->minimum(), PopupSliderHub::snapStep(d->value(), step, dir), d->maximum()));
        return true;
    }
    if (auto *i = qobject_cast<QSpinBox *>(box)) {
        i->setValue(qBound(i->minimum(), PopupSliderHub::snapStep(i->value(), step, dir), i->maximum()));
        return true;
    }
    return false;
}
bool spinWheel(QAbstractSpinBox *box, QWheelEvent *e) {
    if (!box->isEnabled() || box->isReadOnly()) return false;
    const int notches = wheelDir(e);
    if (!notches) return false;
    const int dir = notches > 0 ? 1 : -1;
    if (e->modifiers() & Qt::ControlModifier) {
        for (int n = 0; n < std::abs(notches); ++n) applySnapToSpin(box, dir);
        e->accept();
        return true;
    }
    auto *d = qobject_cast<QDoubleSpinBox *>(box);
    if ((e->modifiers() & Qt::ShiftModifier) && d && d->decimals() > 0) {
        d->setValue(d->value() + dir * PopupSliderHub::fineStep(d) * std::abs(notches));
        e->accept();
        return true;
    }
    return false;
}
} // namespace

bool SliderTarget::handleModifierStep(QAbstractSpinBox *box, QKeyEvent *e) {
    if (e->key() != Qt::Key_Up && e->key() != Qt::Key_Down
        && e->key() != Qt::Key_Left && e->key() != Qt::Key_Right) return false;
    const int dir = (e->key() == Qt::Key_Up || e->key() == Qt::Key_Right) ? 1 : -1;
    if (e->modifiers() & Qt::ControlModifier) {
        applySnapToSpin(box, dir);
        e->accept();
        return true;
    }
    if (!(e->modifiers() & Qt::ShiftModifier)) return false;
    // decimal fields: a fine step (Preferences > Shift fine control speed); whole-number fields: 10 steps
    auto *d = qobject_cast<QDoubleSpinBox *>(box);
    if (d && d->decimals() > 0) d->setValue(d->value() + dir * PopupSliderHub::fineStep(d));
    else box->stepBy(dir * 10);
    e->accept();
    return true;
}

// ---------------------------------------------------------------- fields
namespace {
QSize withArrow(QSize s) { return {s.width() + SliderArrow::kWidth, s.height()}; }
}

PopupSliderField::PopupSliderField(QWidget *parent) : QSpinBox(parent) { initSlider(this); }
PopupSliderField::PopupSliderField(int mn, int mx, const QString &suffix, QWidget *parent) : QSpinBox(parent) {
    initSlider(this);
    setRange(mn, mx);
    setSuffix(suffix);
}
QSize PopupSliderField::sizeHint() const { return withArrow(QSpinBox::sizeHint()); }
QSize PopupSliderField::minimumSizeHint() const { return withArrow(QSpinBox::minimumSizeHint()); }
void PopupSliderField::resizeEvent(QResizeEvent *e) { QSpinBox::resizeEvent(e); layoutArrow(this); }
void PopupSliderField::changeEvent(QEvent *e) { QSpinBox::changeEvent(e); layoutArrow(this); if (m_arrow) m_arrow->update(); }
void PopupSliderField::showEvent(QShowEvent *e) { QSpinBox::showEvent(e); layoutArrow(this); }
void PopupSliderField::keyPressEvent(QKeyEvent *e) { if (!handleModifierStep(this, e)) QSpinBox::keyPressEvent(e); }
void PopupSliderField::wheelEvent(QWheelEvent *e) {
    if (!spinWheel(this, e)) QSpinBox::wheelEvent(e);
}

PopupSliderDoubleField::PopupSliderDoubleField(QWidget *parent) : QDoubleSpinBox(parent) { initSlider(this); }
PopupSliderDoubleField::PopupSliderDoubleField(double mn, double mx, int decimals, const QString &suffix, QWidget *parent)
    : QDoubleSpinBox(parent) {
    initSlider(this);
    setDecimals(decimals);
    setRange(mn, mx);
    setSuffix(suffix);
    setSingleStep(decimals ? 0.1 : 1);
}
void PopupSliderDoubleField::setSliderValue(double v) {
    const double p = std::pow(10.0, decimals());
    setValue(std::round(v * p) / p);
}
QSize PopupSliderDoubleField::sizeHint() const { return withArrow(QDoubleSpinBox::sizeHint()); }
QSize PopupSliderDoubleField::minimumSizeHint() const { return withArrow(QDoubleSpinBox::minimumSizeHint()); }
void PopupSliderDoubleField::resizeEvent(QResizeEvent *e) { QDoubleSpinBox::resizeEvent(e); layoutArrow(this); }
void PopupSliderDoubleField::changeEvent(QEvent *e) { QDoubleSpinBox::changeEvent(e); layoutArrow(this); if (m_arrow) m_arrow->update(); }
void PopupSliderDoubleField::showEvent(QShowEvent *e) { QDoubleSpinBox::showEvent(e); layoutArrow(this); }
void PopupSliderDoubleField::keyPressEvent(QKeyEvent *e) { if (!handleModifierStep(this, e)) QDoubleSpinBox::keyPressEvent(e); }
void PopupSliderDoubleField::wheelEvent(QWheelEvent *e) {
    // Ctrl + wheel snaps; Shift + wheel takes a fine step on decimal fields (axis: some platforms
    // report Shift + wheel as horizontal scrolling).
    if (!spinWheel(this, e)) QDoubleSpinBox::wheelEvent(e);
}

// ---------------------------------------------------------------- arrow
SliderArrow::SliderArrow(SliderTarget *t, QWidget *parent) : QWidget(parent), m_t(t) {
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::NoFocus);
}

void SliderArrow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const bool on = m_pressed || m_t->isPopupOpen();
    // light Macromedia bevel
    p.fillRect(rect(), on ? theme::color("pressed") : theme::color("body"));
    p.setPen(theme::color("line"));
    p.drawLine(0, 0, 0, height() - 1);
    const bool en = isEnabled();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(en ? theme::color("arrow") : theme::color("disabled"));
    const double cx = width() / 2.0 + 0.5, cy = height() / 2.0 + 0.5;
    QPainterPath tri;
    tri.moveTo(cx - 3.5, cy - 2);
    tri.lineTo(cx + 3.5, cy - 2);
    tri.lineTo(cx, cy + 2);
    tri.closeSubpath();
    p.drawPath(tri);
}

void SliderArrow::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::RightButton && m_t->isPopupOpen()) {
        m_t->popup()->cancelDrag();
        m_pressed = false;
        e->accept();
        return;
    }
    if (e->button() != Qt::LeftButton) { e->ignore(); return; }
    e->accept();
    if (m_t->isPopupOpen()) { m_t->popup()->close(); update(); return; }   // second click closes
    m_pressed = true;
    update();
    m_t->spinBox()->setFocus(Qt::MouseFocusReason);
    m_t->openPopup(true);
}

// Normally the popup gets the moves once it is shown; this covers platforms where
// the implicit grab keeps them on the arrow.
void SliderArrow::mouseMoveEvent(QMouseEvent *e) {
    if (m_pressed && m_t->isPopupOpen()) m_t->popup()->dragTo(e->globalPosition(), e->modifiers());
}

void SliderArrow::mouseReleaseEvent(QMouseEvent *e) {
    if (!m_pressed) return;
    m_pressed = false;
    if (m_t->isPopupOpen()) m_t->popup()->finishDrag();
    update();
    e->accept();
}

// ---------------------------------------------------------------- popup
SliderPopup::SliderPopup(SliderTarget *t) : QWidget(t->spinBox(), Qt::Popup | Qt::FramelessWindowHint), m_t(t) {
    setObjectName(QStringLiteral("sliderPopup"));
    setAttribute(Qt::WA_NoMouseReplay);   // a click on the arrow that closes us must not reopen
    setFocusPolicy(Qt::StrongFocus);
    applyOrientation();
    connect(PopupSliderHub::instance(), &PopupSliderHub::orientationChanged, this, [this] {
        applyOrientation();
        if (isVisible()) { place(); update(); }
    });
}

// Everything is laid out as the horizontal slider (150 x 26); the vertical one is the same
// drawing turned 90 degrees anticlockwise, so its low end is at the bottom.
namespace { constexpr int kLong = 150, kShort = 26; const QRect kTrack(9, 10, kLong - 18, 4); }

void SliderPopup::applyOrientation() {
    m_vertical = PopupSliderHub::orientation() == Qt::Vertical;
    setFixedSize(m_vertical ? QSize(kShort, kLong) : QSize(kLong, kShort));
}

QPointF SliderPopup::toLayout(const QPointF &local) const {
    return m_vertical ? QPointF(kLong - 1 - local.y(), local.x()) : local;   // pixel row r <-> layout column kLong-1-r
}

QRect SliderPopup::trackRect() const {
    if (!m_vertical) return kTrack;
    return QRect(kTrack.top(), kLong - kTrack.right() - 1, kTrack.height(), kTrack.width());
}

QPoint SliderPopup::knobPos() const {
    const int k = knobX();
    return m_vertical ? QPoint(kTrack.center().y(), kLong - 1 - k) : QPoint(k, kTrack.center().y());
}

double SliderPopup::fraction() const {
    const double lo = m_t->sliderLo(), hi = m_t->sliderHi();
    if (hi <= lo) return 0;
    return qBound(0.0, (m_t->sliderValue() - lo) / (hi - lo), 1.0);
}

int SliderPopup::knobX() const {
    return kTrack.left() + qRound(fraction() * (kTrack.width() - 1));
}

void SliderPopup::open(bool armedByPress) {
    QAbstractSpinBox *box = m_t->spinBox();
    m_armed = armedByPress;
    m_moved = false;
    m_startValue = m_t->sliderValue();
    m_originalValue = m_startValue;
    m_accum = m_startValue;
    m_fine = false;
    m_snap = false;
    m_dragging = armedByPress;
    m_relative = armedByPress;
    applyOrientation();
    m_pressGX = axisOf(QCursor::pos());
    PopupSliderHub::instance()->consumeCancelled(); // clear a stale flag
    place();
    QWidget *a = m_t->arrow();
    if (armedByPress && a) m_pressGX = axisOf(a->mapToGlobal(QPointF(a->width() / 2.0, m_vertical ? a->height() / 2.0 : a->height())));
    m_lastGX = m_pressGX;
    emit PopupSliderHub::instance()->interactionStarted(box);
    show();
    setFocus(Qt::PopupFocusReason);
    qApp->installEventFilter(this);
    if (a) a->update();
}

// Horizontal: the knob straight under the triangle, the popup just below the field (above it
// near the bottom of the screen). Vertical: the knob level with the triangle and the groove
// over it, so the value follows the pointer up and down. Always kept on screen; the drag is
// relative, so a clamped popup still starts from the current value.
void SliderPopup::place() {
    QAbstractSpinBox *box = m_t->spinBox();
    QWidget *a = m_t->arrow();
    const QPoint k = knobPos();
    QPoint pos;
    if (!m_vertical) {
        const QPoint anchor = a ? a->mapToGlobal(QPoint(a->width() / 2, a->height())) : box->mapToGlobal(QPoint(box->width(), box->height()));
        pos = QPoint(anchor.x() - k.x(), anchor.y() + 1);
    } else {
        const QPoint anchor = a ? a->mapToGlobal(QPoint(a->width() / 2, a->height() / 2)) : box->mapToGlobal(QPoint(box->width(), box->height() / 2));
        pos = QPoint(anchor.x() - k.x(), anchor.y() - k.y());
    }
    if (QScreen *s = box->screen()) {
        const QRect sr = s->availableGeometry();
        if (!m_vertical && pos.y() + height() > sr.bottom()) pos.setY(box->mapToGlobal(QPoint(0, 0)).y() - height() - 1);
        pos.setX(qBound(sr.left(), pos.x(), sr.right() + 1 - width()));
        pos.setY(qBound(sr.top(), pos.y(), sr.bottom() + 1 - height()));
    }
    move(pos);
}

void SliderPopup::setFromX(double x) {
    const QRect tr = kTrack;
    const double f = qBound(0.0, (x - tr.left()) / double(qMax(1, tr.width() - 1)), 1.0);
    m_t->setSliderValue(m_t->sliderLo() + f * (m_t->sliderHi() - m_t->sliderLo()));
    update();
}

void SliderPopup::dragTo(const QPointF &global) { dragTo(global, QGuiApplication::queryKeyboardModifiers()); }

void SliderPopup::dragTo(const QPointF &global, Qt::KeyboardModifiers mods) {
    if (!m_dragging) return;
    const double ax = axisOf(global);
    if (std::abs(ax - m_pressGX) >= 2) m_moved = true;
    if (!m_moved) return;
    const bool snap = mods & Qt::ControlModifier;
    const bool fine = !snap && (mods & Qt::ShiftModifier);
    const double lo = m_t->sliderLo(), hi = m_t->sliderHi();
    const double factor = PopupSliderHub::fineFactor();
    if (fine != m_fine || snap != m_snap || (fine && !m_relative) || (fine && factor != m_fineUsed) || (snap && !m_relative)) {
        // Shift / Ctrl pressed or released: carry on from where the value is now, at the last
        // position (an absolute drag on the groove turns relative so it does not jump to the pointer).
        if (!m_relative) { m_accum = m_t->sliderValue(); m_lastGX = ax; }
        m_relative = true;
        m_startValue = qBound(lo, m_accum, hi);
        m_pressGX = m_lastGX;
        m_fine = fine;
        m_snap = snap;
        m_fineUsed = factor;   // a speed changed in Preferences mid-drag re-anchors too (no jump)
        if (snap) {
            m_accum = PopupSliderHub::snapNearest(m_accum, PopupSliderHub::snapIncrement());
            m_accum = qBound(lo, m_accum, hi);
            m_startValue = m_accum;
            m_t->setSliderValue(m_accum);
        }
    }
    m_lastGX = ax;
    if (m_relative) {
        const double start = qBound(lo, m_startValue, hi);
        const double v = start + (ax - m_pressGX) / double(qMax(1, kTrack.width())) * (hi - lo) * (m_fine ? m_fineUsed : 1.0);
        m_accum = qBound(lo, v, hi);
        m_t->setSliderValue(m_snap ? PopupSliderHub::snapNearest(m_accum, PopupSliderHub::snapIncrement()) : m_accum);
        update();
    } else {
        setFromX(toLayout(mapFromGlobal(global)).x());
        m_accum = m_t->sliderValue();
        if (snap) {
            m_accum = PopupSliderHub::snapNearest(m_accum, PopupSliderHub::snapIncrement());
            m_t->setSliderValue(m_accum);
        }
    }
}

void SliderPopup::finishDrag() {
    if (!m_dragging) return;
    m_dragging = false;
    // a plain click on the triangle leaves the slider open; a drag (or a click on it) closes it
    if (m_armed && !m_moved) { m_armed = false; return; }
    close();
}

void SliderPopup::cancelDrag() {
    // Restore first so interactionFinished / endMerge sees the original value.
    // Mark cancelled so endMerge can drop the merge tip without a digest.
    PopupSliderHub::instance()->markCancelled();
    m_dragging = false;
    m_moved = false;
    m_armed = false;
    m_t->setSliderValue(m_originalValue);
    m_accum = m_originalValue;
    close();
}

void SliderPopup::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), theme::color("hdrTop"));
    p.setPen(theme::color("sliderEdge"));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    if (m_vertical) {   // the horizontal drawing turned anticlockwise: low end at the bottom
        p.translate(0, kLong);
        p.rotate(-90);
    }
    const QRect tr = kTrack;
    // sunken groove
    p.fillRect(tr, theme::color("line"));
    p.setPen(theme::color("sliderEdge"));
    p.drawLine(tr.topLeft(), tr.topRight());
    p.setPen(theme::color("light"));
    p.drawLine(tr.bottomLeft() + QPoint(0, 1), tr.bottomRight() + QPoint(0, 1));
    // knob: a pointer under the groove like Fireworks
    const int kx = knobX();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath k;
    k.moveTo(kx, tr.top() - 1);
    k.lineTo(kx + 5, tr.bottom() + 5);
    k.lineTo(kx + 5, tr.bottom() + 9);
    k.lineTo(kx - 5, tr.bottom() + 9);
    k.lineTo(kx - 5, tr.bottom() + 5);
    k.closeSubpath();
    p.setPen(theme::color("secText"));
    p.setBrush(theme::color("button"));
    p.drawPath(k);
}

void SliderPopup::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::RightButton) {
        cancelDrag();
        return;
    }
    if (!rect().contains(e->position().toPoint())) {   // click elsewhere closes
        close();
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_relative = false;
    m_armed = false;
    m_moved = true;
    m_fine = false;
    m_snap = false;
    m_lastGX = axisOf(e->globalPosition());
    setFromX(toLayout(e->position()).x());
    m_accum = m_t->sliderValue();
    if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))
        dragTo(e->globalPosition(), e->modifiers());   // fine or snap from here on
}

void SliderPopup::mouseMoveEvent(QMouseEvent *e) {
    if (e->buttons() & Qt::LeftButton) dragTo(e->globalPosition(), e->modifiers());
}

void SliderPopup::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::RightButton) {
        if (m_dragging || isVisible()) cancelDrag();
        return;
    }
    if (e->button() == Qt::LeftButton) finishDrag();
}

void SliderPopup::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Escape:
        cancelDrag();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        close();
        return;
    case Qt::Key_Left: case Qt::Key_Down:
    case Qt::Key_Right: case Qt::Key_Up: {
        const int dir = (e->key() == Qt::Key_Right || e->key() == Qt::Key_Up) ? 1 : -1;
        if (e->modifiers() & Qt::ControlModifier) applySnapToSpin(m_t->spinBox(), dir);
        else {
            auto *d = qobject_cast<QDoubleSpinBox *>(m_t->spinBox());
            if ((e->modifiers() & Qt::ShiftModifier) && d && d->decimals() > 0) d->setValue(d->value() + dir * PopupSliderHub::fineStep(d));
            else m_t->spinBox()->stepBy(e->modifiers() & Qt::ShiftModifier ? dir * 10 : dir);
        }
        update();
        return;
    }
    default:
        QWidget::keyPressEvent(e);
    }
}

void SliderPopup::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    qApp->removeEventFilter(this);
    m_dragging = false;
    if (QWidget *a = m_t->arrow()) a->update();
    emit PopupSliderHub::instance()->interactionFinished(m_t->spinBox());
}

bool SliderPopup::eventFilter(QObject *, QEvent *e) {
    if (!isVisible()) return false;
    if (e->type() == QEvent::MouseButtonPress) {
        if (static_cast<QMouseEvent *>(e)->button() == Qt::RightButton) {
            cancelDrag();
            return true;
        }
    } else if (e->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
            cancelDrag();
            return true;
        }
    }
    return false;
}
