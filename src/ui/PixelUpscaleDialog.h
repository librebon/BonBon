#pragma once
#include "core/PixelScale.h"
#include <QDialog>
#include <QImage>

class QComboBox;
class QSpinBox;
class QCheckBox;
class QLabel;

// Filters > Pixel Art > Upscale... (v0.3.1): enlarge the selected bitmaps by a whole factor with a
// pixel-art scaler (EPX / Scale2x-4x, Eagle, 2xSaI, Super 2xSaI, Super Eagle, hqx, xBR, xBRZ, MMPX,
// or Nearest Neighbor). The preview shows a crop of the first bitmap before and after.
class PixelUpscaleDialog : public QDialog {
    Q_OBJECT
public:
    explicit PixelUpscaleDialog(const QImage &sample, QWidget *parent = nullptr);
    pixelscale::Method method() const;
    int factor() const;
    bool enlargeObject() const;     // false: keep the object's size (more pixels in the same area)
    void setMethod(pixelscale::Method m);
    void setFactor(int f);
    void setEnlargeObject(bool on);
    QImage previewImage() const { return m_after; }
private:
    void updatePreview();
    void updateNote();
    QImage m_crop, m_after;
    QComboBox *m_method;
    QSpinBox *m_factor;
    QCheckBox *m_enlarge;
    QLabel *m_before, *m_afterLabel, *m_note;
};
