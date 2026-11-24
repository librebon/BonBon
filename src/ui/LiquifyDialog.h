#pragma once
// Filters > Distort > Liquify... (v0.3.0): forward warp, pinch, bloat, twirl (CW / CCW) and reconstruct
// brushes with size and pressure, on a zoomable preview of the bitmap.
#include "core/FxExtra.h"
#include <QDialog>
#include <QImage>

class QCheckBox;
class PopupSliderDoubleField;
class LiquifyView;

class LiquifyDialog : public QDialog {
    Q_OBJECT
public:
    explicit LiquifyDialog(const QImage &src, QWidget *parent = nullptr);
    QImage liquified() const { return m_liq.render(); }   // not result(): that is QDialog::result()
    fx2::Liquify &liquify() { return m_liq; }
    int mode() const { return m_mode; }
    void setMode(int m);
    double brushSize() const;
    double pressure() const;   // 0..1
    // one stroke segment in image coordinates with the current mode / size / pressure (also used by tests)
    void strokeImage(QPointF from, QPointF to);
    // the same with tablet pen pressure (after the Preferences curve) at both ends, interpolated per dab:
    // the pressure scales the rate and, with "Pen pressure changes size", the brush size (v0.3.1)
    void strokeImage(QPointF from, QPointF to, double penFrom, double penTo);
    bool pressureSize() const;
    void setPressureSize(bool on);
    bool showPreview() const;
signals:
    void changed();
private:
    friend class LiquifyView;
    fx2::Liquify m_liq;
    int m_mode = fx2::Liquify::ForwardWarp;
    LiquifyView *m_view = nullptr;
    PopupSliderDoubleField *m_size = nullptr, *m_pressure = nullptr;
    QCheckBox *m_preview = nullptr, *m_pressureSize = nullptr;
};
