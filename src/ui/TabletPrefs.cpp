#include "TabletPrefs.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTabletEvent>
#include <cmath>

// ---------------------------------------------------------------- curve
PressureCurveEdit::PressureCurveEdit(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("tabletCurve"));
    setMinimumSize(160, 160);
    setMouseTracking(true);
    setToolTip(QStringLiteral("Drag the points; click to add one, double-click a point to remove it"));
}

void PressureCurveEdit::setCurve(const QVector<QPointF> &pts) {
    m_pts = tablet::normalizedCurve(pts);
    update();
}

QRectF PressureCurveEdit::plotRect() const {
    const double s = qMin(width(), height()) - 24;
    return QRectF((width() - s) / 2, (height() - s) / 2, s, s);
}
QPointF PressureCurveEdit::toWidget(QPointF c) const {
    const QRectF r = plotRect();
    return QPointF(r.left() + c.x() * r.width(), r.bottom() - c.y() * r.height());
}
QPointF PressureCurveEdit::toCurve(QPointF w) const {
    const QRectF r = plotRect();
    return QPointF(qBound(0.0, (w.x() - r.left()) / r.width(), 1.0), qBound(0.0, (r.bottom() - w.y()) / r.height(), 1.0));
}
int PressureCurveEdit::pointAt(QPointF w) const {
    for (int i = 0; i < m_pts.size(); ++i)
        if (QLineF(toWidget(m_pts[i]), w).length() <= 7) return i;
    return -1;
}

void PressureCurveEdit::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plotRect();
    const QPalette &pal = palette();
    p.fillRect(r, pal.color(QPalette::Base));
    p.setPen(QPen(pal.color(QPalette::Mid), 1));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()), QPointF(r.left() + r.width() * i / 4, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    p.setPen(QPen(pal.color(QPalette::Mid), 1, Qt::DashLine));
    p.drawLine(r.bottomLeft(), r.topRight());
    p.setPen(QPen(pal.color(QPalette::Text), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    QPainterPath path;
    for (int i = 0; i <= 100; ++i) {
        const double x = i / 100.0;
        const QPointF w = toWidget(QPointF(x, tablet::mapCurve(m_pts, x)));
        if (i == 0) path.moveTo(w); else path.lineTo(w);
    }
    p.setPen(QPen(pal.color(QPalette::Highlight), 2));
    p.drawPath(path);
    if (m_marker >= 0) {
        const QPointF w = toWidget(QPointF(m_marker, tablet::mapCurve(m_pts, m_marker)));
        p.setPen(QPen(pal.color(QPalette::Text), 1, Qt::DotLine));
        p.drawLine(QPointF(w.x(), r.bottom()), w);
        p.drawLine(QPointF(r.left(), w.y()), w);
    }
    p.setPen(QPen(pal.color(QPalette::Text), 1));
    for (int i = 0; i < m_pts.size(); ++i) {
        p.setBrush(i == m_drag ? pal.color(QPalette::Highlight) : pal.color(QPalette::Base));
        p.drawRect(QRectF(toWidget(m_pts[i]) - QPointF(4, 4), QSizeF(8, 8)));
    }
}

void PressureCurveEdit::mousePressEvent(QMouseEvent *e) {
    const int i = pointAt(e->position());
    if (e->button() == Qt::RightButton) {
        if (i > 0 && i < m_pts.size() - 1) { m_pts.remove(i); update(); emit curveChanged(); }
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (i >= 0) { m_drag = i; update(); return; }
    if (!plotRect().adjusted(-4, -4, 4, 4).contains(e->position())) return;
    const QPointF c = toCurve(e->position());
    int at = 1;
    while (at < m_pts.size() && m_pts[at].x() < c.x()) ++at;
    if (at >= m_pts.size()) at = m_pts.size() - 1;
    if (c.x() - m_pts[at - 1].x() < 0.02 || m_pts[at].x() - c.x() < 0.02) return;
    m_pts.insert(at, c);
    m_drag = at;
    update();
    emit curveChanged();
}

void PressureCurveEdit::mouseMoveEvent(QMouseEvent *e) {
    if (m_drag < 0) return;
    QPointF c = toCurve(e->position());
    const int n = m_pts.size();
    if (m_drag == 0) c.setX(0);
    else if (m_drag == n - 1) c.setX(1);
    else c.setX(qBound(m_pts[m_drag - 1].x() + 0.02, c.x(), m_pts[m_drag + 1].x() - 0.02));
    m_pts[m_drag] = c;
    update();
    emit curveChanged();
}

void PressureCurveEdit::mouseReleaseEvent(QMouseEvent *) { m_drag = -1; update(); }

void PressureCurveEdit::mouseDoubleClickEvent(QMouseEvent *e) {
    const int i = pointAt(e->position());
    if (i > 0 && i < m_pts.size() - 1) {
        m_pts.remove(i);
        m_drag = -1;
        update();
        emit curveChanged();
    }
}

// ---------------------------------------------------------------- test area
TabletTestArea::TabletTestArea(std::function<Settings()> settings, QWidget *parent) : QWidget(parent), m_settings(std::move(settings)) {
    setObjectName(QStringLiteral("tabletTestArea"));
    setMinimumSize(200, 120);
    setAttribute(Qt::WA_TabletTracking);
    setCursor(Qt::CrossCursor);
    m_readout = QStringLiteral("Draw here with the pen (a mouse presses at 100%)");
}

void TabletTestArea::clear() {
    m_img.fill(Qt::transparent);
    update();
}

void TabletTestArea::resizeEvent(QResizeEvent *) {
    QImage n(size(), QImage::Format_ARGB32_Premultiplied);
    n.fill(Qt::transparent);
    if (!m_img.isNull()) { QPainter p(&n); p.drawImage(0, 0, m_img); }
    m_img = n;
}

void TabletTestArea::penAt(QPointF pos, double raw, double tiltX, double tiltY, double rotation, bool eraser, bool down) {
    const Settings st = m_settings ? m_settings() : Settings{tablet::defaultCurve(), 0.1, 0};
    const double mapped = tablet::mapCurve(st.curve, raw);
    m_readout = QStringLiteral("%1  pressure %2% \u2192 %3%   tilt %4\u00B0, %5\u00B0   rotation %6\u00B0")
                    .arg(eraser ? QStringLiteral("Eraser end") : QStringLiteral("Pen tip"))
                    .arg(qRound(raw * 100)).arg(qRound(mapped * 100))
                    .arg(qRound(tiltX)).arg(qRound(tiltY)).arg(qRound(rotation));
    emit rawPressure(down ? raw : -1);
    if (down && m_img.size() != size()) resizeEvent(nullptr);
    if (down) {
        const double w = qMax(0.5, 18 * tablet::withFloor(mapped, st.minSize));
        const double a = tablet::withFloor(mapped, st.minOpacity);
        QPainter p(&m_img);
        p.setRenderHint(QPainter::Antialiasing);
        if (eraser) {
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            p.setPen(QPen(Qt::transparent, w, Qt::SolidLine, Qt::RoundCap));
        } else {
            QColor c = palette().color(QPalette::Text);
            c.setAlphaF(a);
            p.setPen(QPen(c, w, Qt::SolidLine, Qt::RoundCap));
        }
        if (m_down) p.drawLine(m_last, pos);
        else p.drawPoint(pos);
        m_down = true;
        m_last = pos;
        m_lastP = mapped;
    } else {
        m_down = false;
    }
    update();
}

void TabletTestArea::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Base));
    p.drawImage(0, 0, m_img);
    p.setPen(palette().color(QPalette::PlaceholderText));
    p.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::AlignBottom, m_readout);
    p.setPen(palette().color(QPalette::Mid));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
}

void TabletTestArea::tabletEvent(QTabletEvent *e) {
    const bool eraser = e->pointerType() == QPointingDevice::PointerType::Eraser;
    const QPointingDevice *dev = e->pointingDevice();
    const double raw = !dev || dev->capabilities().testFlag(QInputDevice::Capability::Pressure) ? e->pressure() : 1.0;
    const bool down = e->type() != QEvent::TabletRelease && (e->buttons() & Qt::LeftButton || e->type() == QEvent::TabletPress);
    penAt(e->position(), raw, e->xTilt(), e->yTilt(), e->rotation(), eraser, down);
    e->accept();
}

void TabletTestArea::mousePressEvent(QMouseEvent *e) { if (e->button() == Qt::LeftButton) penAt(e->position(), 1.0, 0, 0, 0, false, true); }
void TabletTestArea::mouseMoveEvent(QMouseEvent *e) { if (e->buttons() & Qt::LeftButton) penAt(e->position(), 1.0, 0, 0, 0, false, true); }
void TabletTestArea::mouseReleaseEvent(QMouseEvent *e) { penAt(e->position(), 1.0, 0, 0, 0, false, false); }
