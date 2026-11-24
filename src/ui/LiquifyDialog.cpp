#include "LiquifyDialog.h"
#include "Shortcuts.h"
#include "PopupSlider.h"
#include "AppState.h"
#include "core/Tablet.h"
#include <QTabletEvent>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

class LiquifyView : public QWidget {
public:
    explicit LiquifyView(LiquifyDialog *d) : QWidget(d), m_d(d) {
        setObjectName(QStringLiteral("liquifyView"));
        setMouseTracking(true);
        setAttribute(Qt::WA_TabletTracking);
        setMinimumSize(320, 240);
        setCursor(Qt::CrossCursor);
        refresh();
    }
    QSize sizeHint() const override {
        const QSize s = m_d->m_liq.source().size();
        return s.scaled(QSize(720, 540).boundedTo(s.expandedTo(QSize(320, 240))), Qt::KeepAspectRatio);
    }
    void refresh() { m_shown = m_d->showPreview() ? m_d->m_liq.render() : m_d->m_liq.source(); update(); }
    QRectF imageRect() const {
        const QSizeF s = m_d->m_liq.source().size();
        if (s.isEmpty()) return {};
        const double k = qMin(width() / s.width(), height() / s.height());
        const QSizeF ds = s * k;
        return QRectF(QPointF((width() - ds.width()) / 2, (height() - ds.height()) / 2), ds);
    }
    QPointF toImage(const QPointF &w) const {
        const QRectF r = imageRect();
        if (r.isEmpty()) return {};
        return (w - r.topLeft()) * (m_d->m_liq.source().width() / r.width());
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(128, 128, 128));
        const QRectF r = imageRect();
        // checkerboard behind transparent pixels
        const int c = 8;
        for (int y = int(r.top()); y < r.bottom(); y += c)
            for (int x = int(r.left()); x < r.right(); x += c)
                p.fillRect(QRectF(x, y, c, c) & r, (((x - int(r.left())) / c + (y - int(r.top())) / c) & 1) ? QColor(204, 204, 204) : Qt::white);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(r, m_shown);
        if (m_hover) {
            const double k = r.width() / qMax(1, m_d->m_liq.source().width());
            const double rad = m_d->brushSize() / 2 * k;
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(Qt::black, 1));
            p.drawEllipse(m_pos, rad, rad);
            p.setPen(QPen(Qt::white, 1));
            p.drawEllipse(m_pos, rad + 1, rad + 1);
        }
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        m_down = true;
        m_last = toImage(e->position());
        m_lastPen = m_pen;
        m_d->strokeImage(m_last, m_last, m_pen, m_pen);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        m_pos = e->position();
        m_hover = true;
        if (m_down) {
            const QPointF p = toImage(e->position());
            m_d->strokeImage(m_last, p, m_lastPen, m_pen);
            m_last = p;
            m_lastPen = m_pen;
        } else {
            update();
        }
    }
    // a tablet pen runs the mouse handlers with its pressure (a mouse presses at 1.0)
    void tabletEvent(QTabletEvent *e) override {
        QEvent::Type mt;
        switch (e->type()) {
        case QEvent::TabletPress: mt = QEvent::MouseButtonPress; break;
        case QEvent::TabletRelease: mt = QEvent::MouseButtonRelease; break;
        case QEvent::TabletMove: mt = QEvent::MouseMove; break;
        default: e->ignore(); return;
        }
        const QPointingDevice *dev = e->pointingDevice();
        const double raw = !dev || dev->capabilities().testFlag(QInputDevice::Capability::Pressure) ? e->pressure() : 1.0;
        if (mt != QEvent::MouseButtonRelease) m_pen = tablet::mapCurve(appState().prefs.tabletCurve, raw);
        const Qt::MouseButton b = mt == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent me(mt, e->position(), e->globalPosition(), b, mt == QEvent::MouseButtonRelease ? Qt::NoButton : (e->buttons() | b),
                       e->modifiers(), dev);
        if (mt == QEvent::MouseButtonPress) mousePressEvent(&me);
        else if (mt == QEvent::MouseButtonRelease) mouseReleaseEvent(&me);
        else mouseMoveEvent(&me);
        m_pen = mt == QEvent::MouseButtonRelease ? 1.0 : m_pen;
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent *) override { m_down = false; }
    void leaveEvent(QEvent *) override { m_hover = false; update(); }
private:
    LiquifyDialog *m_d;
    QImage m_shown;
    QPointF m_pos, m_last;
    double m_pen = 1.0, m_lastPen = 1.0;
    bool m_hover = false, m_down = false;
};

LiquifyDialog::LiquifyDialog(const QImage &src, QWidget *parent)
    : QDialog(parent), m_liq(src.convertToFormat(QImage::Format_ARGB32_Premultiplied)) {
    setObjectName(QStringLiteral("liquifyDialog"));
    setWindowTitle(QStringLiteral("Liquify"));
    auto *h = new QHBoxLayout(this);
    // tools
    auto *tools = new QVBoxLayout;
    auto *grp = new QButtonGroup(this);
    const QStringList names = fx2::Liquify::modeNames();
    static const char *ids[] = {"warp", "pinch", "bloat", "twirlcw", "twirlccw", "reconstruct"};
    static const char *keys[] = {"W", "S", "B", "C", "Alt+C", "R"};
    auto &sm = shortcuts::Manager::get();
    for (int i = 0; i < names.size() && i < 6; ++i) {
        auto *b = new QToolButton;
        b->setObjectName(QStringLiteral("liq_") + QLatin1String(ids[i]));
        b->setText(names[i]);
        b->setCheckable(true);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        const QString sid = QStringLiteral("liquify.") + QLatin1String(ids[i]);
        QList<QKeySequence> seqs = sm.sequences(sid);
        if (seqs.isEmpty()) seqs = {QKeySequence(QLatin1String(keys[i]))};
        b->setShortcut(seqs.isEmpty() ? QKeySequence() : seqs.first());
        const QString tipKey = seqs.isEmpty() || seqs.first().isEmpty()
            ? QString() : seqs.first().toString(QKeySequence::NativeText);
        b->setToolTip(tipKey.isEmpty() ? names[i]
                                       : QStringLiteral("%1 (%2)").arg(names[i], tipKey));
        grp->addButton(b, i);
        tools->addWidget(b);
    }
    grp->button(0)->setChecked(true);
    tools->addStretch();
    h->addLayout(tools);
    connect(grp, &QButtonGroup::idClicked, this, [this](int id) { m_mode = id; });
    m_view = new LiquifyView(this);
    h->addWidget(m_view, 1);
    // options
    auto *side = new QVBoxLayout;
    auto *f = new QFormLayout;
    const int maxDim = qMax(16, qMax(src.width(), src.height()));
    m_size = new PopupSliderDoubleField(1, qMin(1000, maxDim), 0, QStringLiteral(" px"));
    m_size->setObjectName(QStringLiteral("liq_size"));
    m_size->setValue(qBound(8, maxDim / 6, 200));
    m_size->setKeyboardTracking(false);
    f->addRow(QStringLiteral("Brush size:"), m_size);
    m_pressure = new PopupSliderDoubleField(1, 100, 0, QStringLiteral(" %"));
    m_pressure->setObjectName(QStringLiteral("liq_pressure"));
    m_pressure->setValue(50);
    m_pressure->setKeyboardTracking(false);
    f->addRow(QStringLiteral("Pressure:"), m_pressure);
    m_pressureSize = new QCheckBox(QStringLiteral("Pen pressure changes size"));
    m_pressureSize->setObjectName(QStringLiteral("liq_pressureSize"));
    m_pressureSize->setToolTip(QStringLiteral("A tablet pen's pressure always scales the rate; with this it also scales the brush size\n"
                                              "(curve and minimum size in Preferences \u25B8 Tablet)"));
    f->addRow(QString(), m_pressureSize);
    m_preview = new QCheckBox(QStringLiteral("Preview"));
    m_preview->setObjectName(QStringLiteral("liq_preview"));
    m_preview->setChecked(true);
    f->addRow(QString(), m_preview);
    side->addLayout(f);
    auto *restore = new QPushButton(QStringLiteral("Restore All"));
    restore->setObjectName(QStringLiteral("liq_restore"));
    restore->setToolTip(QStringLiteral("Undo every distortion (the Reconstruct brush does it locally)"));
    side->addWidget(restore);
    side->addStretch();
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Vertical);
    side->addWidget(bb);
    h->addLayout(side);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(restore, &QPushButton::clicked, this, [this] { m_liq.reset(); m_view->refresh(); emit changed(); });
    connect(m_preview, &QCheckBox::toggled, this, [this] { m_view->refresh(); });
    connect(m_size, &QDoubleSpinBox::valueChanged, m_view, [this] { m_view->update(); });
}

void LiquifyDialog::setMode(int m) {
    m_mode = qBound(0, m, int(fx2::Liquify::kModes) - 1);
    if (auto *g = findChild<QButtonGroup *>()) if (g->button(m_mode)) g->button(m_mode)->setChecked(true);
}
double LiquifyDialog::brushSize() const { return m_size ? m_size->value() : 50.0; }
double LiquifyDialog::pressure() const { return m_pressure ? m_pressure->value() / 100.0 : 0.5; }
bool LiquifyDialog::showPreview() const { return !m_preview || m_preview->isChecked(); }

bool LiquifyDialog::pressureSize() const { return m_pressureSize && m_pressureSize->isChecked(); }
void LiquifyDialog::setPressureSize(bool on) { if (m_pressureSize) m_pressureSize->setChecked(on); }

void LiquifyDialog::strokeImage(QPointF from, QPointF to) { strokeImage(from, to, 1.0, 1.0); }

void LiquifyDialog::strokeImage(QPointF from, QPointF to, double penFrom, double penTo) {
    // break long segments into dabs no further apart than a quarter of the brush
    const double r0 = brushSize() / 2;
    const double len = std::hypot(to.x() - from.x(), to.y() - from.y());
    const int n = qMax(1, int(std::ceil(len / qMax(1.0, r0 / 2))));
    const double minSize = appState().prefs.tabletMinSize / 100.0;
    QPointF a = from;
    for (int i = 1; i <= n; ++i) {
        const double t = double(i) / n;
        const QPointF b = from + (to - from) * t;
        const double pen = qBound(0.0, penFrom + (penTo - penFrom) * t, 1.0);
        const double r = pressureSize() ? qMax(0.5, r0 * tablet::withFloor(pen, minSize)) : r0;
        m_liq.stroke(fx2::Liquify::Mode(m_mode), a, b, r, pressure() * pen);
        a = b;
    }
    m_view->refresh();
    emit changed();
}
