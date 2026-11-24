#include "Theme.h"
#include "Rulers.h"
#include "Canvas.h"
#include <QMouseEvent>
#include <QPainter>
#include <cmath>

Ruler::Ruler(Qt::Orientation o, Canvas *canvas, QWidget *parent) : QWidget(parent), m_orient(o), m_canvas(canvas) {
    setObjectName(o == Qt::Horizontal ? QStringLiteral("hRuler") : QStringLiteral("vRuler"));
    if (o == Qt::Horizontal) setFixedHeight(kThickness); else setFixedWidth(kThickness);
    setCursor(o == Qt::Horizontal ? Qt::SplitVCursor : Qt::SplitHCursor);
    setToolTip(o == Qt::Horizontal ? QStringLiteral("Drag down to create a horizontal guide")
                                   : QStringLiteral("Drag right to create a vertical guide"));
    connect(canvas, &Canvas::viewChanged, this, qOverload<>(&QWidget::update));
    connect(canvas, &Canvas::zoomChanged, this, qOverload<>(&QWidget::update));
    connect(canvas, &Canvas::cursorMoved, this, [this](QPointF d) { m_cursor = m_orient == Qt::Horizontal ? d.x() : d.y(); update(); });
}

QSize Ruler::sizeHint() const { return m_orient == Qt::Horizontal ? QSize(100, kThickness) : QSize(kThickness, 100); }

void Ruler::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), theme::color("ruler"));
    p.setPen(theme::color("rulerInk"));
    const bool h = m_orient == Qt::Horizontal;
    if (h) p.drawLine(0, height() - 1, width(), height() - 1); else p.drawLine(width() - 1, 0, width() - 1, height());
    const double z = m_canvas->zoom();
    // canvas position of this ruler's origin (the rulers sit flush with the canvas edges)
    const QPoint off = m_canvas->mapTo(window(), QPoint(0, 0)) - mapTo(window(), QPoint(0, 0));
    auto toWidget = [&](double docV) { const QPointF v = m_canvas->toView(QPointF(docV, docV)); return h ? v.x() + off.x() : v.y() + off.y(); };
    auto toDocV = [&](double w) { const QPointF d = m_canvas->toDoc(QPointF(w - off.x(), w - off.y())); return h ? d.x() : d.y(); };
    // major tick spacing: a "nice" number of document px that is at least ~50 screen px apart
    static const int nice[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000, 10000};
    int major = nice[0];
    for (int n : nice) { major = n; if (n * z >= 50) break; }
    const int minor = major % 10 == 0 ? major / 10 : major % 5 == 0 ? major / 5 : major >= 2 ? major / 2 : 1;
    const double len = h ? width() : height();
    const double d0 = std::floor(toDocV(0) / minor) * minor, d1 = toDocV(len);
    QFont f = font();
    f.setPixelSize(9);
    p.setFont(f);
    for (double v = d0; v <= d1; v += minor) {
        const int pos = int(std::round(toWidget(v)));
        const bool isMajor = std::fmod(std::abs(v), major) < 1e-6;
        const int tick = isMajor ? kThickness - 2 : (minor * z >= 4 ? 4 : 0);
        if (!tick) continue;
        if (h) p.drawLine(pos, kThickness - 1 - tick, pos, kThickness - 1); else p.drawLine(kThickness - 1 - tick, pos, kThickness - 1, pos);
        if (isMajor) {
            const QString s = QString::number(qRound(v));
            if (h) p.drawText(pos + 2, 9, s);
            else { p.save(); p.translate(9, pos + 2); p.rotate(90); p.drawText(0, 0, s); p.restore(); }
        }
    }
    if (m_cursor > -1e8) {
        p.setPen(QColor(0xD0, 0x20, 0x20));
        const int pos = int(std::round(toWidget(m_cursor)));
        if (h) p.drawLine(pos, 0, pos, kThickness); else p.drawLine(0, pos, kThickness, pos);
    }
}

void Ruler::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    // top ruler -> horizontal guide; left ruler -> vertical guide
    m_canvas->beginGuideDrag(m_orient == Qt::Vertical);
    m_canvas->guideDragTo(e->globalPosition().toPoint());
}

void Ruler::mouseMoveEvent(QMouseEvent *e) {
    if (m_canvas->guideDragActive()) m_canvas->guideDragTo(e->globalPosition().toPoint());
}

void Ruler::mouseReleaseEvent(QMouseEvent *e) {
    if (m_canvas->guideDragActive()) m_canvas->endGuideDrag(e->globalPosition().toPoint());
}
