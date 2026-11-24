#pragma once
#include "core/Object.h"
#include <QDialog>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class ColorButton;

// Gradient ramp with draggable colour stops (Fireworks' Edit Gradient pop-up).
// Click below the ramp to add a stop, drag a stop to move it, drag it off downwards to
// remove it, double-click it to pick its colour.
class GradientBar : public QWidget {
    Q_OBJECT
public:
    explicit GradientBar(QWidget *parent = nullptr);
    void setStops(const QVector<GradStop> &s);
    QVector<GradStop> stops() const { return m_stops; }
    int current() const { return m_cur; }
    void setCurrent(int i);
    void setCurrentColor(const QColor &c);
    void setCurrentPos(double p);
    QSize sizeHint() const override { return {320, 52}; }
signals:
    void changed();
    void currentChanged(int);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
private:
    QRectF ramp() const;
    int stopAt(const QPointF &p) const;
    QVector<GradStop> m_stops;
    int m_cur = 0;
    bool m_drag = false;
};

class GradientDialog : public QDialog {
    Q_OBJECT
public:
    explicit GradientDialog(const QVector<GradStop> &stops, QWidget *parent = nullptr);
    QVector<GradStop> stops() const;
    GradientBar *bar() const { return m_bar; }
private:
    void sync();
    GradientBar *m_bar;
    QComboBox *m_preset;
    ColorButton *m_color;
    QDoubleSpinBox *m_pos;
    bool m_syncing = false;
};
