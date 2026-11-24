#pragma once
// Preferences > Tablet (v0.3.1): the pressure curve editor and the test area.
#include "core/Tablet.h"
#include <QImage>
#include <QWidget>
#include <functional>

// The pressure curve: x = pen pressure, y = what the tools get. Drag a point, click the empty graph to add
// one, double-click (or right-click) an inner point to remove it. The end points move only vertically.
class PressureCurveEdit : public QWidget {
    Q_OBJECT
public:
    explicit PressureCurveEdit(QWidget *parent = nullptr);
    QVector<QPointF> curve() const { return m_pts; }
    void setCurve(const QVector<QPointF> &pts);
    void setMarker(double raw) { m_marker = raw; update(); }   // the test area's live pressure (-1: none)
    QSize sizeHint() const override { return {220, 220}; }
    QRectF plotRect() const;
    QPointF toWidget(QPointF c) const;
    QPointF toCurve(QPointF w) const;
signals:
    void curveChanged();
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
private:
    int pointAt(QPointF w) const;
    QVector<QPointF> m_pts = tablet::defaultCurve();
    int m_drag = -1;
    double m_marker = -1;
};

// Scribble with the pen: the line follows the curve and minimums being edited (width and opacity), and a
// readout shows raw / mapped pressure, tilt, rotation and which end of the pen is in use.
class TabletTestArea : public QWidget {
    Q_OBJECT
public:
    struct Settings { QVector<QPointF> curve; double minSize = 0.1, minOpacity = 0; };
    explicit TabletTestArea(std::function<Settings()> settings, QWidget *parent = nullptr);
    QSize sizeHint() const override { return {320, 160}; }
    void clear();
    QString readout() const { return m_readout; }
    const QImage &image() const { return m_img; }
    // one pen sample (raw pressure); tests and tabletEvent both come here
    void penAt(QPointF pos, double rawPressure, double tiltX, double tiltY, double rotation, bool eraser, bool down);
signals:
    void rawPressure(double p);
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void tabletEvent(QTabletEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
private:
    std::function<Settings()> m_settings;
    QImage m_img;
    QPointF m_last;
    double m_lastP = 1;
    bool m_down = false;
    QString m_readout;
};
