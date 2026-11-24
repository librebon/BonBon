#pragma once
#include <QLineEdit>
#include <functional>

class QAction;
class QMenu;

// The status-bar zoom field (as in Fireworks): clicking it pops up the zoom levels (current one
// checked) plus Fit All / Fit Selection; it is also editable, so typing "137" + Enter sets 137 %.
class ZoomField : public QLineEdit {
    Q_OBJECT
public:
    explicit ZoomField(QWidget *parent = nullptr);
    static const QList<int> &levels();   // 6 .. 6400
    void setZoom(double z);              // shows qRound(z*100)%
    double zoom() const { return m_zoom; }
    QMenu *buildMenu();                  // a fresh popup reflecting the current zoom (for tests too)
    void popup();
signals:
    void zoomRequested(double z);
    void fitAllRequested();
    void fitSelectionRequested();
protected:
    void mousePressEvent(QMouseEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void changeEvent(QEvent *e) override;
private:
    void commitText();
    void refreshArrow();
    double m_zoom = 1;
    QAction *m_arrow = nullptr;
};
