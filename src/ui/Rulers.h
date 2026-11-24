#pragma once
#include <QWidget>

class Canvas;

// Fireworks-style ruler along the top (horizontal) or left (vertical) edge of the canvas.
// Shows document pixels at the current zoom, marks the cursor position, and drags out guides:
// press in the top ruler and drag down for a horizontal guide, in the left ruler for a vertical one.
class Ruler : public QWidget {
    Q_OBJECT
public:
    Ruler(Qt::Orientation o, Canvas *canvas, QWidget *parent = nullptr);
    static constexpr int kThickness = 18;
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
private:
    Qt::Orientation m_orient;
    Canvas *m_canvas;
    double m_cursor = -1e9;   // document coordinate of the mouse
};
