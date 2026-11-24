#include "GradientEditor.h"
#include "PopupSlider.h"
#include "Dialogs.h"
#include "core/Fills.h"
#include "ColorPicker.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

GradientBar::GradientBar(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("gradStops"));
    setMinimumSize(240, 52);
    setCursor(Qt::PointingHandCursor);
    m_stops = {{0, Qt::black}, {1, Qt::white}};
}

void GradientBar::setStops(const QVector<GradStop> &s) {
    m_stops = s.size() >= 2 ? s : QVector<GradStop>{{0, Qt::black}, {1, Qt::white}};
    m_cur = qBound(0, m_cur, int(m_stops.size()) - 1);
    update();
    emit currentChanged(m_cur);
}
void GradientBar::setCurrent(int i) {
    m_cur = qBound(0, i, int(m_stops.size()) - 1);
    update();
    emit currentChanged(m_cur);
}
void GradientBar::setCurrentColor(const QColor &c) {
    if (m_cur < 0 || m_cur >= m_stops.size()) return;
    m_stops[m_cur].color = c;
    update();
    emit changed();
}
void GradientBar::setCurrentPos(double p) {
    if (m_cur < 0 || m_cur >= m_stops.size()) return;
    m_stops[m_cur].pos = qBound(0.0, p, 1.0);
    update();
    emit changed();
}

QRectF GradientBar::ramp() const { return QRectF(8, 4, width() - 16, height() - 24); }

int GradientBar::stopAt(const QPointF &p) const {
    const QRectF r = ramp();
    int best = -1;
    double bd = 8;
    for (int i = 0; i < m_stops.size(); ++i) {
        const double x = r.left() + m_stops[i].pos * r.width();
        const double d = std::abs(p.x() - x);
        if (p.y() >= r.bottom() - 2 && d < bd) { bd = d; best = i; }
    }
    return best;
}

void GradientBar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const QRectF r = ramp();
    for (int y = 0; y < int(r.height()); y += 6)
        for (int x = 0; x < int(r.width()); x += 6)
            p.fillRect(QRectF(r.left() + x, r.top() + y, qMin(6.0, r.width() - x), qMin(6.0, r.height() - y)), ((x + y) / 6) % 2 ? QColor(0xcc, 0xcc, 0xcc) : Qt::white);
    QLinearGradient g(r.topLeft(), r.topRight());
    for (const GradStop &s : m_stops) g.setColorAt(s.pos, s.color);
    p.fillRect(r, g);
    p.setPen(palette().color(QPalette::Mid));
    p.drawRect(r);
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_stops.size(); ++i) {
        const double x = r.left() + m_stops[i].pos * r.width();
        QPolygonF tri({QPointF(x, r.bottom() + 1), QPointF(x + 6, r.bottom() + 9), QPointF(x - 6, r.bottom() + 9)});
        p.setPen(QPen(i == m_cur ? palette().color(QPalette::Highlight) : Qt::black, i == m_cur ? 2 : 1));
        p.setBrush(Qt::white);
        p.drawPolygon(tri);
        p.setBrush(m_stops[i].color);
        p.drawRect(QRectF(x - 6, r.bottom() + 9, 12, 9));
    }
}

void GradientBar::mousePressEvent(QMouseEvent *e) {
    const QPointF pos = e->position();
    int i = stopAt(pos);
    if (i < 0) {
        const QRectF r = ramp();
        const double t = qBound(0.0, (pos.x() - r.left()) / r.width(), 1.0);
        m_stops.append({t, fills::sampleGradient(m_stops, t)});
        i = int(m_stops.size()) - 1;
        emit changed();
    }
    m_cur = i;
    m_drag = true;
    update();
    emit currentChanged(m_cur);
}
void GradientBar::mouseMoveEvent(QMouseEvent *e) {
    if (!m_drag || m_cur < 0) return;
    const QRectF r = ramp();
    m_stops[m_cur].pos = qBound(0.0, (e->position().x() - r.left()) / r.width(), 1.0);
    update();
    emit changed();
    emit currentChanged(m_cur);
}
void GradientBar::mouseReleaseEvent(QMouseEvent *e) {
    if (m_drag && m_cur >= 0 && e->position().y() > height() + 16 && m_stops.size() > 2) { // dragged off: remove
        m_stops.removeAt(m_cur);
        m_cur = qMin(m_cur, int(m_stops.size()) - 1);
        update();
        emit changed();
        emit currentChanged(m_cur);
    }
    m_drag = false;
}
void GradientBar::mouseDoubleClickEvent(QMouseEvent *e) {
    const int i = stopAt(e->position());
    if (i < 0) return;
    m_cur = i;
    const QColor c = colorpick::getColor(m_stops[i].color, this, QStringLiteral("Stop Color"), true);
    if (c.isValid()) setCurrentColor(c);
}

GradientDialog::GradientDialog(const QVector<GradStop> &stops, QWidget *parent) : QDialog(parent) {
    setObjectName(QStringLiteral("gradientDialog"));
    setWindowTitle(QStringLiteral("Edit Gradient"));
    auto *v = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    m_preset = new QComboBox;
    m_preset->setObjectName(QStringLiteral("gradPreset"));
    m_preset->addItem(QStringLiteral("Preset..."));
    m_preset->addItems(fills::gradientPresetNames());
    // 0.3.6: Black, White is the default when a gradient type is chosen; highlight it when stops match
    top->addWidget(new QLabel(QStringLiteral("Preset:")));
    top->addWidget(m_preset, 1);
    auto *rev = new QPushButton(QStringLiteral("Reverse"));
    rev->setObjectName(QStringLiteral("gradReverse"));
    top->addWidget(rev);
    v->addLayout(top);
    m_bar = new GradientBar;
    m_bar->setStops(stops);
    v->addWidget(m_bar);
    auto *row = new QHBoxLayout;
    m_color = new ColorButton(this, true);
    m_color->setObjectName(QStringLiteral("stopColor"));
    m_color->setFixedSize(44, 22);
    m_pos = new PopupSliderDoubleField;
    m_pos->setObjectName(QStringLiteral("stopPos"));
    m_pos->setRange(0, 100);
    m_pos->setSuffix(QStringLiteral("%"));
    m_pos->setDecimals(1);
    auto *del = new QPushButton(QStringLiteral("Remove Stop"));
    del->setObjectName(QStringLiteral("stopRemove"));
    row->addWidget(new QLabel(QStringLiteral("Stop color:")));
    row->addWidget(m_color);
    row->addWidget(new QLabel(QStringLiteral("Location:")));
    row->addWidget(m_pos);
    row->addWidget(del);
    row->addStretch();
    v->addLayout(row);
    v->addWidget(new QLabel(QStringLiteral("<small>Click under the ramp to add a stop; drag a stop down off the ramp to remove it.</small>")));
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
    connect(m_bar, &GradientBar::currentChanged, this, &GradientDialog::sync);
    connect(m_bar, &GradientBar::changed, this, &GradientDialog::sync);
    connect(m_color, &ColorButton::colorPicked, this, [this](const QColor &c) { if (!m_syncing) m_bar->setCurrentColor(c); });
    connect(m_color, &ColorButton::colorPreview, this, [this](const QColor &c) { if (!m_syncing) m_bar->setCurrentColor(c); });   // live
    connect(m_pos, &QDoubleSpinBox::valueChanged, this, [this](double p) { if (!m_syncing) m_bar->setCurrentPos(p / 100); });
    connect(m_preset, &QComboBox::activated, this, [this](int i) { if (i > 0) m_bar->setStops(fills::gradientPreset(m_preset->itemText(i))); });
    auto selectPreset = [this] {
        const QVector<GradStop> cur = m_bar->stops();
        int hit = 0;
        for (int i = 0; i < fills::gradientPresetNames().size(); ++i) {
            const QVector<GradStop> p = fills::gradientPreset(fills::gradientPresetNames()[i]);
            if (p.size() != cur.size()) continue;
            bool ok = true;
            for (int k = 0; k < p.size() && ok; ++k)
                ok = qFuzzyCompare(p[k].pos + 1, cur[k].pos + 1) && p[k].color == cur[k].color;
            if (ok) { hit = i + 1; break; }
        }
        m_preset->setCurrentIndex(hit);
    };
    selectPreset();
    connect(m_bar, &GradientBar::changed, this, selectPreset);
    connect(rev, &QPushButton::clicked, this, [this] {
        QVector<GradStop> s = m_bar->stops();
        for (auto &g : s) g.pos = 1 - g.pos;
        m_bar->setStops(s);
    });
    connect(del, &QPushButton::clicked, this, [this] {
        QVector<GradStop> s = m_bar->stops();
        if (s.size() <= 2) return;
        s.removeAt(m_bar->current());
        m_bar->setStops(s);
    });
    sync();
}

void GradientDialog::sync() {
    m_syncing = true;
    const auto s = m_bar->stops();
    const int i = m_bar->current();
    if (i >= 0 && i < s.size()) {
        if (!m_color->isPreviewing()) m_color->setColor(s[i].color);   // (a live preview keeps the button's colour to go back to)
        m_pos->setValue(s[i].pos * 100);
    }
    m_syncing = false;
}

QVector<GradStop> GradientDialog::stops() const {
    QVector<GradStop> s = m_bar->stops();
    std::stable_sort(s.begin(), s.end(), [](const GradStop &a, const GradStop &b) { return a.pos < b.pos; });
    return s;
}
