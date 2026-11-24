#include "DragReorder.h"
#include "Theme.h"
#include <QPainter>

QColor dragreorder::color() {
    QColor c = theme::color("accent");
    if (!c.isValid() || c.alpha() == 0) c = QColor(0x00, 0x66, 0xFF);
    return c;
}

void dragreorder::paintLine(QPainter &p, const QRect &r) {
    p.setRenderHint(QPainter::Antialiasing, false);
    const QColor c = color();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const int mid = (r.top() + r.bottom()) / 2;
    p.fillRect(QRect(r.left(), mid - 1, r.width(), 2), c);
    // triangular end caps so the line is obvious even on a busy list
    QPolygon left, right;
    left << QPoint(r.left(), mid - 4) << QPoint(r.left() + 5, mid) << QPoint(r.left(), mid + 4);
    right << QPoint(r.right() + 1, mid - 4) << QPoint(r.right() - 4, mid) << QPoint(r.right() + 1, mid + 4);
    p.setBrush(c);
    p.drawPolygon(left);
    p.drawPolygon(right);
}

void dragreorder::paintInto(QPainter &p, const QRect &r) {
    QColor c = color();
    c.setAlpha(50);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(r, c);
    QColor edge = color();
    edge.setAlpha(220);
    p.setPen(QPen(edge, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(1, 1, -1, -1));
}

DragInsertIndicator::DragInsertIndicator(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    hide();
}

void DragInsertIndicator::showLine(const QRect &span) {
    m_into = false;
    m_active = true;
    setGeometry(span.adjusted(0, -5, 0, 5));
    show();
    raise();
    update();
}

void DragInsertIndicator::showInto(const QRect &itemRect) {
    m_into = true;
    m_active = true;
    setGeometry(itemRect);
    show();
    raise();
    update();
}

void DragInsertIndicator::clear() {
    m_active = false;
    hide();
}

void DragInsertIndicator::paintEvent(QPaintEvent *) {
    if (!m_active) return;
    QPainter p(this);
    if (m_into) dragreorder::paintInto(p, rect());
    else dragreorder::paintLine(p, rect());
}
