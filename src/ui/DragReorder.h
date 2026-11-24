#pragma once
#include <QWidget>
#include <QRect>

class QPainter;

// A clear Fireworks-style insertion indicator for drag-reordering lists and trees:
// a 2px accent line between rows, or a rectangle around a container ("drop into").
class DragInsertIndicator : public QWidget {
    Q_OBJECT
public:
    explicit DragInsertIndicator(QWidget *parent);
    void showLine(const QRect &span);     // horizontal insertion line in parent (viewport) coords
    void showInto(const QRect &itemRect); // highlight a drop-into target
    void clear();
    bool active() const { return m_active; }
    bool into() const { return m_into; }
    QRect span() const { return geometry(); }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    bool m_active = false;
    bool m_into = false;
};

namespace dragreorder {
QColor color();
void paintLine(QPainter &p, const QRect &r);
void paintInto(QPainter &p, const QRect &r);
}
