#include "PopupSlider.h"
#include "PixelUpscaleDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
constexpr int kView = 240;   // preview tile size (px)

// Checkerboard tile with `img` drawn hard-edged at `zoom` (integer), centred.
QPixmap tile(const QImage &img, int zoom) {
    QImage canvas(kView, kView, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&canvas);
    for (int y = 0; y < kView; y += 8)
        for (int x = 0; x < kView; x += 8)
            p.fillRect(x, y, 8, 8, ((x + y) / 8) % 2 ? QColor(204, 204, 204) : QColor(255, 255, 255));
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    if (!img.isNull()) {
        const int w = img.width() * zoom, h = img.height() * zoom;
        p.drawImage(QRect(qMax(0, (kView - w) / 2), qMax(0, (kView - h) / 2), w, h), img);
    }
    p.end();
    return QPixmap::fromImage(canvas);
}
}

PixelUpscaleDialog::PixelUpscaleDialog(const QImage &sample, QWidget *parent) : QDialog(parent) {
    setObjectName(QStringLiteral("pixelUpscaleDialog"));
    setWindowTitle(QStringLiteral("Upscale Pixel Art"));
    // preview crop: up to 48 x 48 source pixels from the middle of the bitmap
    const QImage src = sample.convertToFormat(QImage::Format_ARGB32);
    const int cw = qMin(48, src.width()), ch = qMin(48, src.height());
    m_crop = src.copy((src.width() - cw) / 2, (src.height() - ch) / 2, cw, ch);

    auto *v = new QVBoxLayout(this);
    auto *f = new QFormLayout;
    m_method = new QComboBox;
    m_method->setObjectName(QStringLiteral("upscaleMethod"));
    for (int i = 0; i < pixelscale::kMethodCount; ++i) m_method->addItem(pixelscale::name(pixelscale::Method(i)), i);
    m_method->setCurrentIndex(int(pixelscale::Method::Xbrz));
    f->addRow(QStringLiteral("Method:"), m_method);
    m_factor = new PopupSliderField;
    m_factor->setObjectName(QStringLiteral("upscaleFactor"));
    m_factor->setRange(2, 8);
    m_factor->setPrefix(QStringLiteral("\u00D7"));
    m_factor->setValue(2);
    f->addRow(QStringLiteral("Factor:"), m_factor);
    m_enlarge = new QCheckBox(QStringLiteral("Enlarge the object (otherwise keep its size and add pixels)"));
    m_enlarge->setObjectName(QStringLiteral("upscaleEnlarge"));
    m_enlarge->setChecked(true);
    f->addRow(m_enlarge);
    v->addLayout(f);
    m_note = new QLabel;
    m_note->setObjectName(QStringLiteral("upscaleNote"));
    m_note->setWordWrap(true);
    v->addWidget(m_note);
    auto *row = new QHBoxLayout;
    auto col = [&](const QString &title, QLabel *&img) {
        auto *c = new QVBoxLayout;
        c->addWidget(new QLabel(title));
        img = new QLabel;
        img->setFixedSize(kView, kView);
        img->setFrameShape(QFrame::Box);
        c->addWidget(img);
        row->addLayout(c);
    };
    col(QStringLiteral("Before (nearest)"), m_before);
    col(QStringLiteral("After"), m_afterLabel);
    m_before->setObjectName(QStringLiteral("upscaleBefore"));
    m_afterLabel->setObjectName(QStringLiteral("upscaleAfter"));
    v->addLayout(row);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
    connect(m_method, &QComboBox::currentIndexChanged, this, [this] { updateNote(); updatePreview(); });
    connect(m_factor, &QSpinBox::valueChanged, this, [this] { updateNote(); updatePreview(); });
    updateNote();
    updatePreview();
}

pixelscale::Method PixelUpscaleDialog::method() const { return pixelscale::Method(m_method->currentData().toInt()); }
int PixelUpscaleDialog::factor() const { return m_factor->value(); }
bool PixelUpscaleDialog::enlargeObject() const { return m_enlarge->isChecked(); }
void PixelUpscaleDialog::setMethod(pixelscale::Method m) { m_method->setCurrentIndex(m_method->findData(int(m))); }
void PixelUpscaleDialog::setFactor(int f) { m_factor->setValue(f); }
void PixelUpscaleDialog::setEnlargeObject(bool on) { m_enlarge->setChecked(on); }

void PixelUpscaleDialog::updateNote() {
    const QVector<int> native = pixelscale::nativeFactors(method());
    const QVector<int> passes = pixelscale::passesFor(method(), factor());
    QStringList n;
    for (int x : native) n << QStringLiteral("%1\u00D7").arg(x);
    int prod = 1;
    for (int p : passes) prod *= p;
    QString s = QStringLiteral("Native factors: %1.").arg(n.join(QStringLiteral(", ")));
    if (passes.size() > 1) {
        QStringList ps;
        for (int p : passes) ps << QStringLiteral("%1\u00D7").arg(p);
        s += QStringLiteral(" \u00D7%1 runs %2").arg(factor()).arg(ps.join(QStringLiteral(" then ")));
        s += prod == factor() ? QStringLiteral(".") : QStringLiteral(" (= \u00D7%1) and resamples down.").arg(prod);
    } else if (prod != factor()) {
        s += QStringLiteral(" \u00D7%1 runs \u00D7%2 and resamples down.").arg(factor()).arg(prod);
    }
    m_note->setText(s);
}

void PixelUpscaleDialog::updatePreview() {
    const int f = factor();
    m_after = pixelscale::upscale(m_crop, method(), f);
    // the same on-screen size for both: the after image at 1:1 (or 2:1 when small), before at f x that
    const int z = qMax(1, kView / qMax(1, qMax(m_after.width(), m_after.height())));
    m_before->setPixmap(tile(m_crop, f * z));
    m_afterLabel->setPixmap(tile(m_after, z));
}
