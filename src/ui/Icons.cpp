#include <cmath>
#include "Icons.h"
#include "Brand.h"
#include "core/AutoShape.h"
#include "Theme.h"
#include <QApplication>
#include <QIconEngine>
#include <QStyle>
#include <QStyleOption>
#include <functional>
#include <QPainter>
#include <QFile>
#include <QDebug>
#include <QPainterPath>
#include <QtMath>


// Pull brand.qrc out of the static lwui library (linker otherwise drops the qrc object).
extern int qInitResources_brand();
namespace {
const int kBrandResource = qInitResources_brand();
}

namespace icons {
namespace {
// Outline ink: near black on light themes, the theme's text colour on dark ones.
QColor ink() {
    const QColor w = theme::color("window");
    return w.lightnessF() < 0.45 ? theme::color("text") : QColor(0x22, 0x22, 0x22);
}

// Icons are re-drawn when the theme's ink changes (live theme switching). Everything else is
// delegated to a plain QIcon(pixmap), so sizing, centring, never-upscaling and the disabled look
// are exactly those of the original pixmap icons; only the ink colour can differ (dark themes).
class ThemedIconEngine : public QIconEngine {
public:
    explicit ThemedIconEngine(std::function<QPixmap()> draw) : m_draw(std::move(draw)) {}
    const QIcon &icon() {
        const QRgb key = ink().rgb();
        if (m_icon.isNull() || key != m_key) { m_key = key; m_icon = QIcon(m_draw()); }
        return m_icon;
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return icon().pixmap(size, 1.0, mode, state);
    }
    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        return icon().pixmap(size, scale, mode, state);
    }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        icon().paint(p, rect, Qt::AlignCenter, mode, state);
    }
    QSize actualSize(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return icon().actualSize(size, mode, state);
    }
    QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override { return icon().availableSizes(mode, state); }
    QIconEngine *clone() const override { return new ThemedIconEngine(m_draw); }
    QString key() const override { return QStringLiteral("libreworks-themed"); }
private:
    std::function<QPixmap()> m_draw;
    QIcon m_icon;
    QRgb m_key = 0;
};
QIcon themed(std::function<QPixmap()> draw) { return QIcon(new ThemedIconEngine(std::move(draw))); }
const QColor kBlue(0x2E, 0x6D, 0xC4);

QPixmap canvas(int s = 22) {
    QPixmap pm(s * 2, s * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    return pm;
}

// Compact Pen-family nib (same silhouette and white highlight as Tool::Pen).
// `tip` is the top point; `s` is the scale relative to the full-size Pen.
void penNib(QPainter &p, const QPointF &tip, qreal s) {
    QPainterPath n;
    n.moveTo(tip);
    n.lineTo(tip + QPointF(6 * s, 10 * s));
    n.lineTo(tip + QPointF(3 * s, 17 * s));
    n.lineTo(tip + QPointF(-3 * s, 17 * s));
    n.lineTo(tip + QPointF(-6 * s, 10 * s));
    n.closeSubpath();
    p.setPen(QPen(ink(), 1.0));
    p.setBrush(ink());
    p.drawPath(n);
    p.setPen(QPen(Qt::white, qMax(0.9, 1.2 * s)));
    p.drawLine(tip + QPointF(0, 2 * s), tip + QPointF(0, 10 * s));
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(tip + QPointF(0, 10.5 * s), 1.4 * s, 1.4 * s);
}

void arrow(QPainter &p, const QPointF &tip, bool hollow) {
    QPainterPath a;
    a.moveTo(tip);
    a.lineTo(tip + QPointF(0, 14));
    a.lineTo(tip + QPointF(3.5, 10.5));
    a.lineTo(tip + QPointF(6, 15.5));
    a.lineTo(tip + QPointF(8, 14.5));
    a.lineTo(tip + QPointF(5.6, 9.6));
    a.lineTo(tip + QPointF(10, 9.6));
    a.closeSubpath();
    p.setPen(QPen(ink(), 1.1));
    p.setBrush(hollow ? QColor(Qt::white) : ink());
    p.drawPath(a);
}
}

static QPixmap drawTool(Tool t) {
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(ink(), 1.3);
    p.setPen(pen);
    switch (t) {
    case Tool::Pointer: arrow(p, {6, 3}, false); break;
    case Tool::Subselect: arrow(p, {6, 3}, true); break;
    case Tool::Scale:
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ink(), 1, Qt::DashLine));
        p.drawRect(QRectF(4, 4, 14, 14));
        p.setPen(pen);
        p.setBrush(Qt::white);
        for (QPointF c : {QPointF(4, 4), QPointF(18, 4), QPointF(4, 18), QPointF(18, 18)}) p.drawRect(QRectF(c.x() - 2, c.y() - 2, 4, 4));
        break;
    case Tool::Skew: case Tool::Distort: case Tool::Perspective: {
        const QPolygonF q = t == Tool::Skew ? QPolygonF({QPointF(8, 4), QPointF(20, 4), QPointF(14, 18), QPointF(2, 18)})
                          : t == Tool::Distort ? QPolygonF({QPointF(5, 3), QPointF(19, 6), QPointF(17, 19), QPointF(3, 16)})
                                               : QPolygonF({QPointF(8, 4), QPointF(14, 4), QPointF(20, 18), QPointF(2, 18)});
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ink(), 1, Qt::DashLine));
        p.drawPolygon(q);
        p.setPen(pen);
        p.setBrush(Qt::white);
        for (QPointF c : q) p.drawRect(QRectF(c.x() - 2, c.y() - 2, 4, 4));
        break;
    }
    case Tool::Crop:
        p.setPen(QPen(ink(), 2));
        p.drawPolyline(QPolygonF({QPointF(6, 2), QPointF(6, 16), QPointF(20, 16)}));
        p.drawPolyline(QPolygonF({QPointF(2, 6), QPointF(16, 6), QPointF(16, 20)}));
        break;
    case Tool::ExportArea: {
        // Fireworks' Export Area: a dashed frame with corner handles and an outgoing arrow
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ink(), 1, Qt::DashLine));
        p.drawRect(QRectF(2.5, 4.5, 14, 12));
        p.setPen(QPen(ink(), 1));
        p.setBrush(Qt::white);
        for (QPointF c : {QPointF(2.5, 4.5), QPointF(16.5, 4.5), QPointF(2.5, 16.5), QPointF(16.5, 16.5)})
            p.drawRect(QRectF(c.x() - 1.5, c.y() - 1.5, 3, 3));
        p.setPen(QPen(QColor(0x20, 0x80, 0x30), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(9.5, 10.5), QPointF(20, 20));
        p.drawLine(QPointF(20, 20), QPointF(20, 15.5));
        p.drawLine(QPointF(20, 20), QPointF(15.5, 20));
        break;
    }
    case Tool::Marquee:
        p.setPen(QPen(ink(), 1.2, Qt::DashLine));
        p.drawRect(QRectF(3.5, 5.5, 15, 11));
        break;
    case Tool::OvalMarquee:
        p.setPen(QPen(ink(), 1.2, Qt::DashLine));
        p.drawEllipse(QRectF(3.5, 5.5, 15, 11));
        break;
    case Tool::PolyLasso:
        p.setPen(QPen(ink(), 1.3));
        p.drawPolyline(QPolygonF({QPointF(8, 19), QPointF(3, 9), QPointF(10, 3), QPointF(19, 7), QPointF(16, 15), QPointF(9, 13)}));
        break;
    case Tool::Lasso: {
        p.setPen(QPen(ink(), 1.3));
        QPainterPath l;
        l.moveTo(8, 17);
        l.cubicTo(1, 15, 2, 4, 11, 4);
        l.cubicTo(20, 4, 21, 13, 13, 14);
        l.cubicTo(9, 14.5, 7, 15, 8, 20);
        p.drawPath(l);
        break;
    }
    case Tool::MagicWand:
        p.setPen(QPen(ink(), 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4, 19), QPointF(13, 10));
        p.setPen(QPen(QColor(0xE0, 0xA0, 0x10), 1.4));
        for (int i = 0; i < 8; ++i) {
            double a = i * M_PI / 4;
            p.drawLine(QPointF(16 + 2 * qCos(a), 6 + 2 * qSin(a)), QPointF(16 + 4.5 * qCos(a), 6 + 4.5 * qSin(a)));
        }
        break;
    case Tool::Brush: {
        p.setPen(QPen(QColor(0x8A, 0x5A, 0x2B), 2.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(19, 3), QPointF(10, 12));
        QPainterPath tipPath;
        tipPath.moveTo(10, 11);
        tipPath.cubicTo(6, 11, 6, 15, 3, 19);
        tipPath.cubicTo(8, 19, 12, 17, 11.5, 13);
        p.setPen(QPen(ink(), 1));
        p.setBrush(kBlue);
        p.drawPath(tipPath);
        break;
    }
    case Tool::Pencil: {
        QPolygonF body({QPointF(16, 3), QPointF(19, 6), QPointF(7, 18), QPointF(4, 15)});
        p.setBrush(QColor(0xF2, 0xC1, 0x3B));
        p.drawPolygon(body);
        p.setBrush(ink());
        p.drawPolygon(QPolygonF({QPointF(4, 15), QPointF(7, 18), QPointF(2.5, 19.5)}));
        break;
    }
    case Tool::Eraser: {
        QPolygonF e({QPointF(12, 3), QPointF(20, 11), QPointF(12, 19), QPointF(4, 11)});
        p.setBrush(QColor(0xF0, 0x9A, 0xA8));
        p.drawPolygon(e);
        p.setBrush(Qt::white);
        p.drawPolygon(QPolygonF({QPointF(8, 15), QPointF(12, 19), QPointF(16, 15), QPointF(12, 11)}));
        break;
    }
    case Tool::Blur:
        p.setBrush(QColor(0x7F, 0xB2, 0xE8));
        p.drawPath([] { QPainterPath d; d.moveTo(11, 2); d.cubicTo(17, 10, 18, 13, 18, 14.5); d.arcTo(QRectF(4, 8, 14, 13), 0, -180); d.cubicTo(4, 13, 5, 10, 11, 2); return d; }());
        break;
    case Tool::Sharpen:
        p.setBrush(QColor(0x7F, 0xB2, 0xE8));
        p.drawPolygon(QPolygonF({QPointF(11, 2), QPointF(17, 19), QPointF(5, 19)}));
        break;
    case Tool::Dodge:
        p.setPen(QPen(ink(), 1.6));
        p.drawLine(QPointF(5, 19), QPointF(12, 11));
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(14.5, 7.5), 5, 5);
        break;
    case Tool::Burn: {
        p.setBrush(QColor(0xF4, 0xD9, 0xB8));
        QPainterPath h;
        h.addRoundedRect(QRectF(6, 4, 10, 15), 4, 4);
        p.drawPath(h);
        p.setBrush(ink());
        p.drawEllipse(QPointF(11, 9), 2, 2);
        break;
    }
    case Tool::Smudge:
        p.setPen(QPen(ink(), 1.4, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(QColor(0xF4, 0xD9, 0xB8));
        p.drawRoundedRect(QRectF(9, 2, 5, 12), 2.5, 2.5);
        p.setPen(QPen(kBlue, 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4, 19), QPointF(18, 17));
        break;
    case Tool::Gradient: {
        QLinearGradient g(3, 0, 19, 0);
        g.setColorAt(0, Qt::white);
        g.setColorAt(1, kBlue);
        p.setBrush(g);
        p.drawRect(QRectF(3.5, 5.5, 15, 11));
        break;
    }
    case Tool::Knife:
        p.setBrush(QColor(0xD0, 0xD0, 0xD0));
        p.drawPolygon(QPolygonF({QPointF(4, 19), QPointF(16, 3), QPointF(18, 5), QPointF(8, 19)}));
        break;
    case Tool::Freeform:
    case Tool::ReshapeArea: {
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(3, 5, 16, 14), 0, 180 * 16);
        p.setBrush(t == Tool::Freeform ? ink() : QColor(0x90, 0x90, 0x90, 120));
        p.drawEllipse(QPointF(11, 8), t == Tool::Freeform ? 2.0 : 5.0, t == Tool::Freeform ? 2.0 : 5.0);
        break;
    }
    case Tool::PathScrubber: {
        // a stroke that swells and tapers, with a +/- badge
        QPainterPath s;
        s.moveTo(2, 15);
        s.cubicTo(6, 9, 10, 8.4, 13, 10.6);
        s.cubicTo(10, 12.6, 6, 13.4, 2, 15);
        p.setBrush(ink());
        p.drawPath(s);
        p.setPen(QPen(ink(), 1.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(15, 5), QPointF(20, 5));
        p.drawLine(QPointF(17.5, 2.5), QPointF(17.5, 7.5));
        p.drawLine(QPointF(15, 17), QPointF(20, 17));
        break;
    }
    case Tool::CircleHotspot:
        p.setBrush(QColor(0x30, 0x70, 0xE0, 110));
        p.setPen(QPen(QColor(0x20, 0x40, 0xA0), 1.2));
        p.drawEllipse(QRectF(3.5, 3.5, 15, 15));
        break;
    case Tool::PolyHotspot:
        p.setBrush(QColor(0x30, 0x70, 0xE0, 110));
        p.setPen(QPen(QColor(0x20, 0x40, 0xA0), 1.2));
        p.drawPolygon(QPolygonF({QPointF(3, 8), QPointF(12, 3), QPointF(19, 10), QPointF(14, 19), QPointF(5, 17)}));
        break;
    case Tool::RubberStamp:
        p.setBrush(QColor(0x9A, 0x6B, 0x3E));
        p.drawRoundedRect(QRectF(8, 2, 6, 9), 2, 2);
        p.setBrush(QColor(0x6E, 0x6E, 0x6E));
        p.drawRect(QRectF(4, 11, 14, 4));
        p.setBrush(QColor(0xC0, 0x30, 0x30));
        p.drawRect(QRectF(4, 16, 14, 3));
        break;
    case Tool::Healing: case Tool::SpotHealing: {
        // a sticking plaster (spot healing: with a dotted spot)
        p.save();
        p.translate(11, 11);
        p.rotate(-45);
        p.setBrush(QColor(0xE8, 0xC0, 0x90));
        p.setPen(QPen(ink(), 1));
        p.drawRoundedRect(QRectF(-9, -3.5, 18, 7), 3.5, 3.5);
        p.setBrush(QColor(0xFF, 0xF0, 0xE0));
        p.drawRect(QRectF(-3, -3.5, 6, 7));
        p.restore();
        if (t == Tool::SpotHealing) {
            p.setPen(QPen(ink(), 1, Qt::DotLine));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(13, 13, 7, 7));
        }
        break;
    }
    case Tool::Eyedropper:
        p.setPen(QPen(ink(), 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(5, 18), QPointF(14, 9));
        p.setPen(pen);
        p.setBrush(ink());
        p.drawEllipse(QPointF(16.5, 6.5), 3.2, 3.2);
        break;
    case Tool::PaintBucket: {
        p.setBrush(Qt::white);
        p.drawPolygon(QPolygonF({QPointF(9, 3), QPointF(17, 11), QPointF(10, 18), QPointF(2, 10)}));
        p.setBrush(kBlue);
        p.drawEllipse(QPointF(18, 16), 2.2, 3);
        break;
    }
    case Tool::Line:
        p.setPen(QPen(ink(), 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(4, 18), QPointF(18, 4));
        break;
    case Tool::Pen: {
        QPainterPath n;
        n.moveTo(11, 2);
        n.lineTo(17, 12);
        n.lineTo(14, 19);
        n.lineTo(8, 19);
        n.lineTo(5, 12);
        n.closeSubpath();
        p.setBrush(ink());
        p.drawPath(n);
        p.setPen(QPen(Qt::white, 1.2));
        p.drawLine(QPointF(11, 4), QPointF(11, 12));
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(11, 12.5), 1.4, 1.4);
        break;
    }
    case Tool::VectorPath: {
        // Fireworks-style: a compact pen nib and a freehand squiggle
        penNib(p, QPointF(7.5, 1.2), 0.58);
        p.setPen(QPen(ink(), 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath w;
        w.moveTo(2.5, 17.2);
        w.cubicTo(5.5, 12.2, 8.2, 20.4, 12.0, 15.6);
        w.cubicTo(14.8, 12.2, 16.8, 19.0, 20.6, 14.0);
        p.drawPath(w);
        break;
    }
    case Tool::RedrawPath: {
        // Fireworks-style: a compact pen nib over a short path segment being redrawn
        // (dashed original with end nodes, solid stroke for the redrawn middle).
        penNib(p, QPointF(11.0, 0.8), 0.52);
        const QPointF a(3.0, 16.5), b(19.5, 15.5);
        p.setPen(QPen(ink(), 1.15, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath orig;
        orig.moveTo(a);
        orig.quadTo(QPointF(11.0, 14.2), b);
        p.drawPath(orig);
        p.setPen(QPen(ink(), 1.55, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath red;
        red.moveTo(QPointF(6.5, 16.0));
        red.cubicTo(QPointF(9.0, 19.6), QPointF(13.5, 11.8), QPointF(17.0, 15.6));
        p.drawPath(red);
        p.setPen(QPen(ink(), 1.0));
        p.setBrush(Qt::white);
        for (QPointF c : {a, b})
            p.drawRect(QRectF(c.x() - 1.6, c.y() - 1.6, 3.2, 3.2));
        break;
    }
    case Tool::SelectBehind:
        arrow(p, {8, 1}, false);
        p.setPen(QPen(ink(), 1.15, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(3, 7), QPointF(3, 18));
        p.drawLine(QPointF(3, 18), QPointF(8, 14));
        break;
    case Tool::Rect:
        p.setBrush(kBlue.lighter(130));
        p.drawRect(QRectF(3.5, 5.5, 15, 11));
        break;
    case Tool::Ellipse:
        p.setBrush(kBlue.lighter(130));
        p.drawEllipse(QRectF(3.5, 4.5, 15, 13));
        break;
    case Tool::Polygon: {
        QPolygonF pg;
        for (int i = 0; i < 5; ++i) {
            const double a = -M_PI / 2 + i * 2 * M_PI / 5;
            pg << QPointF(11 + 8 * std::cos(a), 11.5 + 8 * std::sin(a));
        }
        p.setBrush(kBlue.lighter(130));
        p.drawPolygon(pg);
        break;
    }
    case Tool::Text: {
        QFont f(QStringLiteral("Serif"));
        f.setPixelSize(19);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, 22, 22), Qt::AlignCenter, QStringLiteral("A"));
        break;
    }
    case Tool::Slice:
        p.setBrush(QColor(0x40, 0xB0, 0x40, 110));
        p.setPen(QPen(QColor(0x20, 0x80, 0x20), 1.2));
        p.drawRect(QRectF(3.5, 3.5, 15, 15));
        p.drawLine(QPointF(3.5, 11), QPointF(18.5, 11));
        p.drawLine(QPointF(11, 3.5), QPointF(11, 18.5));
        break;
    case Tool::PolySlice:
        p.setBrush(QColor(0x40, 0xB0, 0x40, 110));
        p.setPen(QPen(QColor(0x20, 0x80, 0x20), 1.2));
        p.drawPolygon(QPolygonF({QPointF(3, 8), QPointF(12, 3), QPointF(19, 10), QPointF(14, 19), QPointF(5, 17)}));
        break;
    case Tool::Hotspot:
        p.setBrush(QColor(0x30, 0x70, 0xE0, 110));
        p.setPen(QPen(QColor(0x20, 0x40, 0xA0), 1.2));
        p.drawRect(QRectF(3.5, 5.5, 15, 11));
        break;
    case Tool::Hand: {
        p.setBrush(QColor(0xF4, 0xD9, 0xB8));
        QPainterPath h;
        h.addRoundedRect(QRectF(5, 9, 12, 10), 3, 3);
        for (int i = 0; i < 4; ++i) h.addRoundedRect(QRectF(5 + i * 3, 3 + (i == 0 || i == 3 ? 2 : 0), 3, 10), 1.5, 1.5);
        p.drawPath(h.simplified());
        break;
    }
    case Tool::Zoom:
        p.setPen(QPen(ink(), 1.6));
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(9, 9), 6, 6);
        p.setPen(QPen(ink(), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(13.5, 13.5), QPointF(19, 19));
        break;
    case Tool::ReplaceColor: {
        // Fireworks' Replace Color: a brush tip over a colour swatch turning into another
        p.setPen(QPen(ink(), 1));
        p.setBrush(QColor(0xE0, 0x40, 0x30));
        p.drawRect(QRectF(2.5, 12.5, 7, 7));
        p.setBrush(QColor(0x30, 0x70, 0xE0));
        p.drawRect(QRectF(12.5, 12.5, 7, 7));
        p.setPen(QPen(ink(), 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(9.5, 9), QPointF(13.5, 9));
        p.drawLine(QPointF(11.8, 7.2), QPointF(13.5, 9));
        p.drawLine(QPointF(11.8, 10.8), QPointF(13.5, 9));
        // the brush handle
        p.setPen(QPen(QColor(0x8A, 0x5A, 0x2B), 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(15, 2), QPointF(19, 6));
        p.setPen(Qt::NoPen);
        p.setBrush(ink());
        p.drawEllipse(QPointF(14, 7), 1.6, 1.6);
        break;
    }
    case Tool::RedEye: {
        // an eye with a red pupil being corrected (half dark) and a catchlight
        p.setPen(QPen(ink(), 1.2));
        p.setBrush(Qt::white);
        QPainterPath eye;
        eye.moveTo(1.5, 11); eye.quadTo(11, 2, 20.5, 11); eye.quadTo(11, 20, 1.5, 11);
        p.drawPath(eye);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xD8, 0x20, 0x20));
        p.drawEllipse(QPointF(11, 11), 4.6, 4.6);
        p.setBrush(QColor(0x20, 0x20, 0x24));
        p.drawPie(QRectF(6.4, 6.4, 9.2, 9.2), 90 * 16, 180 * 16);
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(12.6, 9.4), 1.2, 1.2);
        break;
    }
    case Tool::ContentAware: {
        // a dotted selection being filled from its surroundings + a sparkle
        p.setBrush(QColor(0xE8, 0xC0, 0x90, 160));
        p.setPen(QPen(ink(), 1, Qt::DotLine));
        p.drawEllipse(QRectF(3, 6, 12, 11));
        p.setPen(QPen(QColor(0xD0, 0x90, 0x10), 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(17, 2), QPointF(17, 9));
        p.drawLine(QPointF(13.5, 5.5), QPointF(20.5, 5.5));
        break;
    }
    default:
        if (toolIsAutoShape(t)) {
            const QString k = toolAutoShapeKind(t);
            const QPainterPath path = autoshape::previewPath(k, QRectF(3.5, 3.5, 15, 15));
            if (autoshape::isLineKind(k)) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(kBlue.darker(130), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            } else {
                p.setBrush(kBlue.lighter(130));
                p.setPen(QPen(ink(), 1));
            }
            p.drawPath(path);
            // the Fireworks auto-shape mark: a small yellow diamond
            p.setPen(QPen(QColor(0x80, 0x60, 0x00), 0.8));
            p.setBrush(QColor(0xFF, 0xD7, 0x00));
            p.drawPolygon(QPolygonF({QPointF(18.5, 15), QPointF(21, 17.5), QPointF(18.5, 20), QPointF(16, 17.5)}));
        }
        break;
    }
    p.end();
    return pm;
}

static QPixmap drawNamed(const QString &n) {
    QPixmap pm = canvas(16);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(ink(), 1.2));
    if (n == "eye") {
        QPainterPath e;
        e.moveTo(1.5, 8); e.quadTo(8, 1.5, 14.5, 8); e.quadTo(8, 14.5, 1.5, 8);
        p.setBrush(Qt::white); p.drawPath(e);
        p.setBrush(ink()); p.drawEllipse(QPointF(8, 8), 2.3, 2.3);
    } else if (n == "link") {   // two chain links (mask linked to its group, v0.3.2)
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ink(), 1.6));
        p.drawRoundedRect(QRectF(1.5, 5.5, 7.5, 5), 2.5, 2.5);
        p.drawRoundedRect(QRectF(7, 5.5, 7.5, 5), 2.5, 2.5);
    } else if (n == "lock") {
        p.setBrush(Qt::NoBrush); p.drawArc(QRectF(4.5, 2, 7, 8), 0, 180 * 16);
        p.drawLine(QPointF(4.5, 6), QPointF(4.5, 8)); p.drawLine(QPointF(11.5, 6), QPointF(11.5, 8));
        p.setBrush(QColor(0xD9, 0xA4, 0x2A)); p.drawRect(QRectF(3, 7.5, 10, 7));
    } else if (n == "plus" || n == "layer" || n == "frame" || n == "dup") {
        p.setBrush(Qt::white);
        if (n == "dup") { p.drawRect(QRectF(1.5, 1.5, 9, 9)); }
        p.drawRect(QRectF(n == "dup" ? 5.5 : 2.5, n == "dup" ? 5.5 : 2.5, n == "dup" ? 9 : 11, n == "dup" ? 9 : 11));
        if (n != "dup") {
            p.setPen(QPen(QColor(0x20, 0x90, 0x20), 2));
            p.drawLine(QPointF(8, 5), QPointF(8, 11)); p.drawLine(QPointF(5, 8), QPointF(11, 8));
        }
    } else if (n == "trash") {
        p.setBrush(QColor(0xC8, 0xC8, 0xC8));
        p.drawRect(QRectF(4, 5, 8, 9.5));
        p.drawLine(QPointF(2.5, 4), QPointF(13.5, 4)); p.drawLine(QPointF(6.5, 2.5), QPointF(9.5, 2.5));
        p.drawLine(QPointF(6.5, 7), QPointF(6.5, 12.5)); p.drawLine(QPointF(9.5, 7), QPointF(9.5, 12.5));
    } else if (n == "up" || n == "down") {
        p.setBrush(ink());
        if (n == "up") p.drawPolygon(QPolygonF({QPointF(8, 3), QPointF(13, 11), QPointF(3, 11)}));
        else p.drawPolygon(QPolygonF({QPointF(8, 13), QPointF(13, 5), QPointF(3, 5)}));
    } else if (n == "play") {
        p.setBrush(QColor(0x20, 0x90, 0x20)); p.drawPolygon(QPolygonF({QPointF(4, 2.5), QPointF(13, 8), QPointF(4, 13.5)}));
    } else if (n == "stop") {
        p.setBrush(QColor(0xB0, 0x20, 0x20)); p.drawRect(QRectF(3.5, 3.5, 9, 9));
    } else if (n == "prev" || n == "next") {
        p.setBrush(ink());
        if (n == "prev") { p.drawPolygon(QPolygonF({QPointF(11, 3), QPointF(11, 13), QPointF(4, 8)})); p.drawLine(QPointF(3.5, 3), QPointF(3.5, 13)); }
        else { p.drawPolygon(QPolygonF({QPointF(5, 3), QPointF(5, 13), QPointF(12, 8)})); p.drawLine(QPointF(12.5, 3), QPointF(12.5, 13)); }
    } else if (n == "swap") {
        p.setBrush(Qt::NoBrush); p.drawArc(QRectF(3, 3, 10, 10), 0, 90 * 16);
        p.setBrush(ink());
        p.drawPolygon(QPolygonF({QPointF(13, 9), QPointF(15.5, 6), QPointF(10.5, 6)}));
        p.drawPolygon(QPolygonF({QPointF(7, 3), QPointF(10, 0.5), QPointF(10, 5.5)}));
    } else if (n == "default") {
        p.setBrush(Qt::white); p.drawRect(QRectF(6.5, 6.5, 7, 7));
        p.setBrush(ink()); p.drawRect(QRectF(2.5, 2.5, 7, 7));
    } else if (n == "fx") {
        QFont f; f.setPixelSize(11); f.setBold(true); f.setItalic(true); p.setFont(f);
        p.setPen(kBlue); p.drawText(QRectF(0, 0, 16, 16), Qt::AlignCenter, QStringLiteral("fx"));
    } else if (n == "minus") {
        p.setPen(QPen(QColor(0xB0, 0x20, 0x20), 2)); p.drawLine(QPointF(4, 8), QPointF(12, 8));
    } else if (n == "web") {
        p.setBrush(QColor(0x40, 0xB0, 0x40, 160)); p.drawRect(QRectF(2.5, 2.5, 11, 11));
        p.drawLine(QPointF(2.5, 8), QPointF(13.5, 8)); p.drawLine(QPointF(8, 2.5), QPointF(8, 13.5));
    } else if (n == "webhide") {
        p.setBrush(QColor(0xA0, 0xA0, 0xA0, 160)); p.drawRect(QRectF(2.5, 2.5, 11, 11));
        p.setPen(QPen(QColor(0xB0, 0x20, 0x20), 1.6)); p.drawLine(QPointF(2, 14), QPointF(14, 2));
    } else if (n == "canvas") {
        // Small artboard / canvas: theme ink outline, white page, a hint of a horizon.
        p.setBrush(Qt::white);
        p.drawRoundedRect(QRectF(1.8, 2.2, 12.4, 11.6), 1.2, 1.2);
        p.setPen(QPen(ink(), 1.0));
        p.drawLine(QPointF(3.4, 10.4), QPointF(6.6, 7.6));
        p.drawLine(QPointF(6.6, 7.6), QPointF(8.6, 9.2));
        p.drawLine(QPointF(8.6, 9.2), QPointF(13.0, 5.4));
        p.setBrush(ink());
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(5.2, 5.4), 1.1, 1.1);
    }
    p.end();
    return pm;
}

QIcon tool(Tool t) { return themed([t] { return drawTool(t); }); }
QIcon named(const QString &n) { return themed([n] { return drawNamed(n); }); }

QPixmap paintedBn(int size) {
    // Last-resort Bonbon mark when brand.qrc failed to link — never the orange Lw icon.
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double s = size / 64.0;
    p.scale(s, s);
    QLinearGradient g(0, 0, 0, 64);
    g.setColorAt(0, QColor(0xF4, 0x8F, 0xB1));
    g.setColorAt(1, QColor(0xC2, 0x3B, 0x7A));
    p.setPen(QPen(QColor(0x7A, 0x1E, 0x48), 1.5));
    p.setBrush(g);
    p.drawRoundedRect(QRectF(3, 3, 58, 58), 12, 12);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 70));
    p.drawRoundedRect(QRectF(5, 5, 54, 26), 10, 10);
    QFont f(QStringLiteral("Sans Serif"));
    f.setPixelSize(28);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(0x5A, 0x14, 0x34));
    p.drawText(QRectF(4, 8, 58, 52), Qt::AlignCenter, QStringLiteral("Bn"));
    p.setPen(Qt::white);
    p.drawText(QRectF(2, 6, 58, 52), Qt::AlignCenter, QStringLiteral("Bn"));
    return pm;
}

QPixmap paintedLw(int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const double s = size / 64.0;
    p.scale(s, s);
    QLinearGradient g(0, 0, 0, 64);
    g.setColorAt(0, QColor(0xFF, 0xB3, 0x3B));
    g.setColorAt(1, QColor(0xE0, 0x5A, 0x12));
    p.setPen(QPen(QColor(0x80, 0x30, 0x08), 1.5));
    p.setBrush(g);
    p.drawRoundedRect(QRectF(3, 3, 58, 58), 10, 10);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 60));
    p.drawRoundedRect(QRectF(5, 5, 54, 26), 8, 8);
    QFont f(QStringLiteral("Sans Serif"));
    f.setPixelSize(34);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(0x5A, 0x1E, 0x02));
    p.drawText(QRectF(4, 6, 58, 58), Qt::AlignCenter, QStringLiteral("Lw"));
    p.setPen(Qt::white);
    p.drawText(QRectF(2, 4, 58, 58), Qt::AlignCenter, QStringLiteral("Lw"));
    return pm;
}

QPixmap appIcon(int size) {
#if LW_IS_BONBON
    static const int kSizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 512};
    int best = 256;
    for (int c : kSizes) { if (c >= size) { best = c; break; } best = c; }
    const QString path = QStringLiteral(":/brand/bonbon-%1.png").arg(best);
    if (QFile::exists(path)) {
        QPixmap src(path);
        if (!src.isNull()) {
            if (src.width() == size && src.height() == size) return src;
            return src.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
    }
    qWarning("Bonbon brand icon missing from resources (%s); using painted Bn fallback", qPrintable(path));
    return paintedBn(size);
#else
    return paintedLw(size);
#endif
}

QIcon appIconIcon() {
    QIcon ic;
#if LW_IS_BONBON
    static const int kSizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 512};
    for (int s : kSizes) {
        const QString path = QStringLiteral(":/brand/bonbon-%1.png").arg(s);
        if (QFile::exists(path)) {
            QPixmap pm(path);
            if (!pm.isNull()) ic.addPixmap(pm);
        }
    }
    if (!ic.isNull()) return ic;
    for (int s : kSizes) ic.addPixmap(paintedBn(s));
    return ic;
#else
    for (int s : {16, 22, 24, 32, 48, 64, 128, 256})
        ic.addPixmap(paintedLw(s));
    return ic;
#endif
}

}
