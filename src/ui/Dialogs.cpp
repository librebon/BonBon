#include "Brand.h"
#include "Theme.h"
#include "Dialogs.h"
#include "AppState.h"
#include "Shortcuts.h"
#include "PreferencesDialog.h"
#include "MainWindow.h"
#include <QFile>
#include <QFileDialog>
#include "FileDialogs.h"
#include <QTextStream>
#include <QSettings>
#include <QStandardItemModel>
#include "core/PixelScale.h"
#include "core/CorePrefs.h"
#include "core/Effects.h"
#include <QMouseEvent>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QLineEdit>
#include "DocumentView.h"
#include "PopupSlider.h"
#include "core/Document.h"
#include "io/Codecs.h"
#include "io/Quantize.h"
#include "io/ExportPresets.h"
#include "core/FxExtra.h"
#include <QMessageBox>
#include <QEventLoop>
#include <QIcon>
#include <QPixmap>
#include <QProgressDialog>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <atomic>
#include "GradientEditor.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QKeyEvent>
#include <QToolButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTimer>
#include <QClipboard>
#include <QMimeData>
#include <QGuiApplication>
#include <QFutureWatcher>
#include <QLocale>
#include <QtConcurrent/QtConcurrentRun>
#include <QPainterPath>
#include <cmath>
#include <QVBoxLayout>
#include <QScreen>
#include "ColorPicker.h"

// ---------------------------------------------------------------- ColorButton
ColorButton::ColorButton(QWidget *parent, bool allowAlpha) : QToolButton(parent), m_alpha(allowAlpha) {
    setFixedSize(28, 22);
    setCursor(Qt::PointingHandCursor);
    setToolTip(m_color.name(QColor::HexArgb).toUpper());
    connect(this, &QToolButton::clicked, this, [this] { showPopup(); });
}

ColorPopup *ColorButton::showPopup() {
    auto *pop = new ColorPopup(m_color, m_alpha, this);
    if (m_transparentChoice) pop->addTransparentChoice();
    m_popupWorking = m_color;
    connect(pop, &ColorPopup::hovered, this, [this, pop](const QColor &c) {
        m_popupWorking = pop->workingColor().isValid() ? pop->workingColor() : c;
        m_previewing = true;
        emit colorPreview(c);
    });
    connect(pop, &ColorPopup::picked, this, [this](const QColor &c) { choose(c); });
    connect(pop, &ColorPopup::eyedropperRequested, this, [this, pop] {
        m_popupWorking = pop->workingColor();
        startEyedropper();
    });
    connect(pop, &ColorPopup::dialogRequested, this, [this, pop] {
        const QColor start = pop->workingColor().isValid() ? pop->workingColor() : m_popupWorking;
        m_popupWorking = start;
        QTimer::singleShot(0, this, [this, start] { openDialog(start); });
    });
    connect(pop, &QObject::destroyed, this, [this] { if (!ScreenColorPicker::active()) endPreview(); });
    // below the button, kept on its screen
    QPoint at = mapToGlobal(QPoint(0, height()));
    if (QScreen *s = screen()) {
        const QRect sg = s->availableGeometry();
        if (at.x() + pop->width() > sg.right()) at.setX(sg.right() - pop->width());
        if (at.y() + pop->height() > sg.bottom()) at.setY(mapToGlobal(QPoint(0, 0)).y() - pop->height());
    }
    pop->move(at);
    pop->show();
    return pop;
}

void ColorButton::startEyedropper() {
    ScreenColorPicker *p = ScreenColorPicker::start(this);
    const int keepA = (m_popupWorking.isValid() ? m_popupWorking : m_color).alpha();
    connect(p, &ScreenColorPicker::hovered, this, [this, keepA](const QColor &c) {
        QColor v = c;
        if (m_alpha && keepA > 0) v.setAlpha(keepA);
        m_previewing = true;
        emit colorPreview(v);
    });
    connect(p, &ScreenColorPicker::picked, this, [this, keepA](const QColor &c) {
        QColor v = c;
        if (m_alpha && keepA > 0) v.setAlpha(keepA);
        choose(v);
    });
    connect(p, &ScreenColorPicker::cancelled, this, [this] { endPreview(); });
}

void ColorButton::openDialog() { openDialog(m_popupWorking.isValid() ? m_popupWorking : m_color); }

void ColorButton::openDialog(const QColor &initial) {
    const QColor c = colorpick::getColor(initial.isValid() ? initial : m_color, this, QStringLiteral("Choose Colour"), m_alpha);
    if (c.isValid()) choose(c);
}

void ColorButton::choose(const QColor &c) {
    m_previewing = false;
    setColor(c);
    emit colorPicked(c);
}

void ColorButton::endPreview() {
    if (!m_previewing) return;
    m_previewing = false;
    emit colorPreview(m_color);
}

void ColorButton::setColor(const QColor &c) {
    m_color = c;
    setToolTip(c.alpha() == 0 && m_transparentChoice ? QStringLiteral("Transparent") : c.name(QColor::HexArgb).toUpper());
    update();
}

void ColorButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    QRect r = rect().adjusted(1, 1, -2, -2);
    int left = 0;
    if (!m_hint.isNull()) {
        m_hint.paint(&p, QRect(r.left(), r.top() + (r.height() - 16) / 2, 16, 16));
        left = 17;
    }
    QRect sw = r.adjusted(left, 0, 0, 0);
    for (int y = sw.top(); y <= sw.bottom(); y += 4)
        for (int x = sw.left(); x <= sw.right(); x += 4)
            p.fillRect(QRect(x, y, 4, 4).intersected(sw), ((x - sw.left()) / 4 + (y - sw.top()) / 4) % 2 ? QColor(0xCC, 0xCC, 0xCC) : QColor(Qt::white));
    if (m_none || (m_transparentChoice && m_color.alpha() == 0)) {
        p.fillRect(sw, Qt::white);
        p.setPen(QPen(QColor(0xD0, 0x20, 0x20), 2));
        p.drawLine(sw.bottomLeft(), sw.topRight());
    } else {
        p.fillRect(sw, m_color);
    }
    p.setPen(QColor(0x40, 0x40, 0x40));
    p.drawRect(sw);
    if (underMouse()) { p.setPen(theme::color("accent")); p.drawRect(rect().adjusted(0, 0, -1, -1)); }
}

// ---------------------------------------------------------------- OptimizeWidget
using exportpresets::Preset;
using exportpresets::kPresets;

OptimizeWidget::OptimizeWidget(QWidget *parent) : QWidget(parent) {
    auto *g = new QGridLayout(this);
    g->setContentsMargins(6, 6, 6, 6);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(4);
    int row = 0;
    g->addWidget(new QLabel(QStringLiteral("Settings:")), row, 0);
    m_preset = new QComboBox;
    m_preset->addItem(QStringLiteral("Custom"));
    for (const auto &p : kPresets) m_preset->addItem(QString::fromLatin1(p.name));
    g->addWidget(m_preset, row++, 1, 1, 2);
    g->addWidget(new QLabel(QStringLiteral("Format:")), row, 0);
    m_format = new QComboBox;
    m_format->setObjectName(QStringLiteral("optFormat"));
    for (Fmt f : ImageIO::exportFormats()) {
        m_format->addItem(ImageIO::formatName(f), int(f));
        if (!ImageIO::available(f)) {
            m_format->setItemData(m_format->count() - 1, 0, Qt::UserRole - 1); // disable
            m_format->setItemText(m_format->count() - 1, ImageIO::formatName(f) + QStringLiteral(" (not built)"));
        }
    }
    g->addWidget(m_format, row++, 1, 1, 2);
    m_qualityLabel = new QLabel(QStringLiteral("Quality:"));
    g->addWidget(m_qualityLabel, row, 0);
    m_quality = new PopupSliderField(0, 100);
    m_quality->setObjectName(QStringLiteral("optQuality"));
    g->addWidget(m_quality, row++, 1, Qt::AlignLeft);
    m_lossless = new QCheckBox(QStringLiteral("Lossless"));
    g->addWidget(m_lossless, row++, 1, 1, 2);
    m_effortLabel = new QLabel(QStringLiteral("Effort:"));
    g->addWidget(m_effortLabel, row, 0);
    m_effort = new PopupSliderField(0, 6);
    m_effort->setObjectName(QStringLiteral("optWebpEffort"));
    m_effort->setToolTip(QStringLiteral("Higher is smaller and slower (libwebp method 0–6)."));
    m_effortLabel->setToolTip(m_effort->toolTip());
    g->addWidget(m_effort, row++, 1, Qt::AlignLeft);
    m_webpAlphaLabel = new QLabel(QStringLiteral("Alpha quality:"));
    g->addWidget(m_webpAlphaLabel, row, 0);
    m_webpAlpha = new PopupSliderField(0, 100);
    m_webpAlpha->setObjectName(QStringLiteral("optWebpAlphaQuality"));
    m_webpAlpha->setToolTip(QStringLiteral("WebP alpha-channel quality, 0–100. 100 (the previous default) keeps the sharpest alpha."));
    m_webpAlphaLabel->setToolTip(m_webpAlpha->toolTip());
    g->addWidget(m_webpAlpha, row++, 1, Qt::AlignLeft);
    m_webpSharpYuv = new QCheckBox(QStringLiteral("Sharp YUV"));
    m_webpSharpYuv->setObjectName(QStringLiteral("optWebpSharpYuv"));
    m_webpSharpYuv->setToolTip(QStringLiteral("Use sharper YUV downsampling (lossy WebP). Off is the previous default."));
    m_webpExact = new QCheckBox(QStringLiteral("Exact"));
    m_webpExact->setObjectName(QStringLiteral("optWebpExact"));
    m_webpExact->setToolTip(QStringLiteral("Lossless WebP only: preserve the RGB of fully transparent pixels. Off is the previous default."));
    {
        auto *r2 = new QHBoxLayout;
        r2->setContentsMargins(0, 0, 0, 0);
        r2->addWidget(m_webpSharpYuv);
        r2->addWidget(m_webpExact);
        r2->addStretch();
        g->addLayout(r2, row++, 1, 1, 2);
    }
    m_avifSpeedLabel = new QLabel(QStringLiteral("AVIF speed:"));
    g->addWidget(m_avifSpeedLabel, row, 0);
    m_avifSpeed = new PopupSliderField(0, 10);
    m_avifSpeed->setObjectName(QStringLiteral("optAvifSpeed"));
    m_avifSpeed->setToolTip(QStringLiteral("AVIF encoder speed: 0 is slowest / smaller, 10 is fastest. 8 is the previous default."));
    m_avifSpeedLabel->setToolTip(m_avifSpeed->toolTip());
    g->addWidget(m_avifSpeed, row++, 1, Qt::AlignLeft);
    m_avifChromaLabel = new QLabel(QStringLiteral("AVIF chroma:"));
    g->addWidget(m_avifChromaLabel, row, 0);
    m_avifChroma = new QComboBox;
    m_avifChroma->setObjectName(QStringLiteral("optAvifChroma"));
    m_avifChroma->addItem(QStringLiteral("4:4:4"), 0);
    m_avifChroma->addItem(QStringLiteral("4:2:2"), 1);
    m_avifChroma->addItem(QStringLiteral("4:2:0"), 2);
    m_avifChroma->setToolTip(QStringLiteral("Chroma subsampling. Lossless AVIF always writes 4:4:4. 4:2:0 is the previous default."));
    m_avifChromaLabel->setToolTip(m_avifChroma->toolTip());
    g->addWidget(m_avifChroma, row++, 1, 1, 2);
    m_avifDepthLabel = new QLabel(QStringLiteral("AVIF bit depth:"));
    g->addWidget(m_avifDepthLabel, row, 0);
    m_avifDepth = new QComboBox;
    m_avifDepth->setObjectName(QStringLiteral("optAvifDepth"));
    m_avifDepth->addItem(QStringLiteral("8-bit"), 8);
    m_avifDepth->addItem(QStringLiteral("10-bit"), 10);
    m_avifDepth->addItem(QStringLiteral("12-bit"), 12);
    m_avifDepth->setToolTip(QStringLiteral("AVIF colour depth. 8-bit is the previous default."));
    m_avifDepthLabel->setToolTip(m_avifDepth->toolTip());
    g->addWidget(m_avifDepth, row++, 1, 1, 2);
    m_avifAlphaLabel = new QLabel(QStringLiteral("AVIF alpha quality:"));
    g->addWidget(m_avifAlphaLabel, row, 0);
    m_avifAlpha = new PopupSliderField(0, 100);
    m_avifAlpha->setObjectName(QStringLiteral("optAvifAlphaQuality"));
    m_avifAlpha->setToolTip(QStringLiteral("AVIF alpha quality, 0–100. Auto (the previous default) uses the main quality plus 10."));
    m_avifAlphaLabel->setToolTip(m_avifAlpha->toolTip());
    m_avifAlphaAuto = new QCheckBox(QStringLiteral("Auto"));
    m_avifAlphaAuto->setObjectName(QStringLiteral("optAvifAlphaAuto"));
    m_avifAlphaAuto->setToolTip(m_avifAlpha->toolTip());
    {
        auto *r2 = new QHBoxLayout;
        r2->setContentsMargins(0, 0, 0, 0);
        r2->addWidget(m_avifAlpha);
        r2->addWidget(m_avifAlphaAuto);
        r2->addStretch();
        g->addLayout(r2, row++, 1, 1, 2);
    }
    m_jxlEffortLabel = new QLabel(QStringLiteral("JXL effort:"));
    g->addWidget(m_jxlEffortLabel, row, 0);
    m_jxlEffort = new PopupSliderField(1, 9);
    m_jxlEffort->setObjectName(QStringLiteral("optJxlEffort"));
    m_jxlEffort->setToolTip(QStringLiteral("JPEG XL effort: 1 is fastest, 9 is slowest / smaller. 5 is the previous default."));
    m_jxlEffortLabel->setToolTip(m_jxlEffort->toolTip());
    g->addWidget(m_jxlEffort, row++, 1, Qt::AlignLeft);
    m_heifPresetLabel = new QLabel(QStringLiteral("HEIF preset:"));
    g->addWidget(m_heifPresetLabel, row, 0);
    m_heifPreset = new QComboBox;
    m_heifPreset->setObjectName(QStringLiteral("optHeifPreset"));
    m_heifPreset->addItem(QStringLiteral("Default"), QString());
    for (const char *pr : {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo"})
        m_heifPreset->addItem(QString::fromLatin1(pr), QString::fromLatin1(pr));
    m_heifPreset->setToolTip(QStringLiteral("x265 speed preset. Default leaves the encoder’s previous behaviour."));
    m_heifPresetLabel->setToolTip(m_heifPreset->toolTip());
    g->addWidget(m_heifPreset, row++, 1, 1, 2);
    m_heifChromaLabel = new QLabel(QStringLiteral("HEIF chroma:"));
    g->addWidget(m_heifChromaLabel, row, 0);
    m_heifChroma = new QComboBox;
    m_heifChroma->setObjectName(QStringLiteral("optHeifChroma"));
    m_heifChroma->addItem(QStringLiteral("Default"), -1);
    m_heifChroma->addItem(QStringLiteral("4:2:0"), 0);
    m_heifChroma->addItem(QStringLiteral("4:2:2"), 1);
    m_heifChroma->addItem(QStringLiteral("4:4:4"), 2);
    m_heifChroma->setToolTip(QStringLiteral("HEIF chroma subsampling, when the encoder exposes a chroma parameter. Default is the previous behaviour."));
    m_heifChromaLabel->setToolTip(m_heifChroma->toolTip());
    g->addWidget(m_heifChroma, row++, 1, 1, 2);
    m_colorsLabel = new QLabel(QStringLiteral("Colours:"));
    g->addWidget(m_colorsLabel, row, 0);
    m_colors = new PopupSliderField(2, 256);
    m_colors->setObjectName(QStringLiteral("optColors"));
    g->addWidget(m_colors, row++, 1, Qt::AlignLeft);
    m_paletteLabel = new QLabel(QStringLiteral("Palette:"));
    g->addWidget(m_paletteLabel, row, 0);
    m_palette = new QComboBox;
    m_palette->setObjectName(QStringLiteral("optPalette"));
    m_palette->addItems(quant::paletteNames());
    m_loadPalette = new QToolButton;
    m_loadPalette->setObjectName(QStringLiteral("optLoadPalette"));
    m_loadPalette->setText(QStringLiteral("Load..."));
    m_loadPalette->setToolTip(QStringLiteral("Load a custom palette (.act, .gpl, .pal or the colours of a GIF / PNG)"));
    {
        auto *pr = new QHBoxLayout;
        pr->setContentsMargins(0, 0, 0, 0);
        pr->addWidget(m_palette, 1);
        pr->addWidget(m_loadPalette);
        g->addLayout(pr, row++, 1, 1, 2);
    }
    m_dither = new QCheckBox(QStringLiteral("Dither"));
    m_ditherAmount = new PopupSliderField(0, 100, QStringLiteral("%"));
    m_ditherAmount->setObjectName(QStringLiteral("optDither"));
    {
        auto *dr = new QHBoxLayout;
        dr->setContentsMargins(0, 0, 0, 0);
        dr->addWidget(m_dither);
        dr->addWidget(m_ditherAmount);
        dr->addStretch();
        g->addLayout(dr, row++, 1, 1, 2);
    }
    m_lossLabel = new QLabel(QStringLiteral("Loss:"));
    g->addWidget(m_lossLabel, row, 0);
    m_loss = new PopupSliderField(0, 100);
    m_loss->setObjectName(QStringLiteral("optLoss"));
    m_loss->setToolTip(QStringLiteral("Lossy GIF compression: higher values give smaller files with more artefacts"));
    g->addWidget(m_loss, row++, 1, Qt::AlignLeft);
    m_netpbmLabel = new QLabel(QStringLiteral("Encoding:"));
    g->addWidget(m_netpbmLabel, row, 0);
    m_netpbm = new QComboBox;
    m_netpbm->addItem(QStringLiteral("Binary (P5 / P6)"));
    m_netpbm->addItem(QStringLiteral("ASCII (P2 / P3)"));
    g->addWidget(m_netpbm, row++, 1, 1, 2);
    m_tiffCompLabel = new QLabel(QStringLiteral("Compression:"));
    g->addWidget(m_tiffCompLabel, row, 0);
    m_tiffComp = new QComboBox;
    m_tiffComp->setObjectName(QStringLiteral("optTiffCompression"));
    m_tiffComp->addItems({QStringLiteral("None"), QStringLiteral("LZW"), QStringLiteral("Deflate (ZIP)"), QStringLiteral("PackBits")});
    g->addWidget(m_tiffComp, row++, 1, 1, 2);
    m_depthLabel = new QLabel(QStringLiteral("Bit depth:"));
    g->addWidget(m_depthLabel, row, 0);
    m_tiffDepth = new QComboBox;
    m_tiffDepth->setObjectName(QStringLiteral("optTiffDepth"));
    m_tiffDepth->addItems({QStringLiteral("8 bits/channel"), QStringLiteral("16 bits/channel")});
    m_tgaDepth = new QComboBox;
    m_tgaDepth->setObjectName(QStringLiteral("optTgaDepth"));
    m_tgaDepth->addItems({QStringLiteral("24-bit (RGB)"), QStringLiteral("32-bit (RGB + alpha)")});
    {
        auto *dr = new QHBoxLayout;
        dr->setContentsMargins(0, 0, 0, 0);
        dr->addWidget(m_tiffDepth);
        dr->addWidget(m_tgaDepth);
        g->addLayout(dr, row++, 1, 1, 2);
    }
    m_alpha = new QCheckBox(QStringLiteral("Alpha channel (transparency)"));
    m_alpha->setObjectName(QStringLiteral("optAlpha"));
    g->addWidget(m_alpha, row++, 0, 1, 3);
    m_tgaRle = new QCheckBox(QStringLiteral("RLE compression"));
    m_tgaRle->setObjectName(QStringLiteral("optTgaRle"));
    g->addWidget(m_tgaRle, row++, 1, 1, 2);
    m_tgaOriginLabel = new QLabel(QStringLiteral("Origin:"));
    g->addWidget(m_tgaOriginLabel, row, 0);
    m_tgaOrigin = new QComboBox;
    m_tgaOrigin->setObjectName(QStringLiteral("optTgaOrigin"));
    m_tgaOrigin->addItems({QStringLiteral("Bottom-left (standard)"), QStringLiteral("Top-left")});
    g->addWidget(m_tgaOrigin, row++, 1, 1, 2);
    // ---- PNG (v0.2.5)
    m_pngTypeLabel = new QLabel(QStringLiteral("PNG type:"));
    g->addWidget(m_pngTypeLabel, row, 0);
    m_pngType = new QComboBox;
    m_pngType->setObjectName(QStringLiteral("optPngType"));
    m_pngType->addItems({QStringLiteral("PNG 32 (RGBA)"), QStringLiteral("PNG 24 (RGB on matte)"), QStringLiteral("PNG 8 (indexed)"), QStringLiteral("Greyscale")});
    g->addWidget(m_pngType, row++, 1, 1, 2);
    m_pngTransLabel = new QLabel(QStringLiteral("Transparency:"));
    g->addWidget(m_pngTransLabel, row, 0);
    m_pngTrans = new QComboBox;
    m_pngTrans->setObjectName(QStringLiteral("optPngTransparency"));
    m_pngTrans->addItems({QStringLiteral("No Transparency"), QStringLiteral("Index Transparency"), QStringLiteral("Alpha Transparency")});
    g->addWidget(m_pngTrans, row++, 1, 1, 2);
    m_png16 = new QCheckBox(QStringLiteral("16-bit"));
    m_png16->setObjectName(QStringLiteral("optPng16"));
    m_interlace = new QCheckBox(QStringLiteral("Interlaced"));
    m_interlace->setObjectName(QStringLiteral("optPngInterlace"));
    {
        auto *r2 = new QHBoxLayout;
        r2->setContentsMargins(0, 0, 0, 0);
        r2->addWidget(m_png16);
        r2->addWidget(m_interlace);
        r2->addStretch();
        g->addLayout(r2, row++, 1, 1, 2);
    }
    m_pngCompLabel = new QLabel(QStringLiteral("Compression:"));
    g->addWidget(m_pngCompLabel, row, 0);
    m_pngComp = new PopupSliderField(0, 9);
    m_pngComp->setObjectName(QStringLiteral("optPngCompression"));
    m_pngComp->setToolTip(QStringLiteral("zlib level: 0 stores, 9 is smallest and slowest"));
    g->addWidget(m_pngComp, row++, 1, Qt::AlignLeft);
    m_tiny = new QCheckBox(QStringLiteral("Tiny compression"));
    m_tiny->setObjectName(QStringLiteral("optTiny"));
    m_tiny->setToolTip(QStringLiteral("Lossy palette reduction (libimagequant) with alpha, then a zopfli lossless pass; metadata is stripped"));
    m_tiny->setEnabled(codec::tinyAvailable());
    g->addWidget(m_tiny, row++, 1, 1, 2);
    m_tinyBox = new QWidget;
    {
        auto *tr = new QHBoxLayout(m_tinyBox);
        tr->setContentsMargins(16, 0, 0, 0);
        m_tinyMin = new PopupSliderField; m_tinyMin->setObjectName(QStringLiteral("optTinyMin")); m_tinyMin->setRange(0, 100);
        m_tinyMax = new PopupSliderField; m_tinyMax->setObjectName(QStringLiteral("optTinyMax")); m_tinyMax->setRange(0, 100);
        m_tinySpeed = new PopupSliderField; m_tinySpeed->setObjectName(QStringLiteral("optTinySpeed")); m_tinySpeed->setRange(1, 10);
        m_tinySpeed->setToolTip(QStringLiteral("1 is slowest and best, 10 fastest (8 and above skip the zopfli pass)"));
        m_tinyMin->setToolTip(QStringLiteral("Minimum quality: below it the lossless PNG is kept"));
        m_tinyMax->setToolTip(QStringLiteral("Target quality"));
        tr->addWidget(new QLabel(QStringLiteral("Q")));
        tr->addWidget(m_tinyMin);
        tr->addWidget(new QLabel(QStringLiteral("-")));
        tr->addWidget(m_tinyMax);
        tr->addWidget(new QLabel(QStringLiteral("Spd")));
        tr->addWidget(m_tinySpeed);
        m_tinyDither = new PopupSliderField(0, 100, QStringLiteral("%"));
        m_tinyDither->setObjectName(QStringLiteral("optTinyDither"));
        m_tinyDither->setToolTip(QStringLiteral("libimagequant dither amount, 0–100 %. 100 is the previous default."));
        tr->addWidget(new QLabel(QStringLiteral("Dith")));
        tr->addWidget(m_tinyDither);
        tr->addStretch();
    }
    g->addWidget(m_tinyBox, row++, 0, 1, 3);
    m_editable = new QCheckBox(QStringLiteral("Editable"));
    m_editable->setObjectName(QStringLiteral("optPngEditable"));
    m_editable->setToolTip(QStringLiteral("Embed the %1 document in the PNG, as Fireworks PNGs do; %1 re-opens it fully editable").arg(brand::name()));
    g->addWidget(m_editable, row++, 1, 1, 2);
    // ---- JPEG
    m_jpegEncLabel = new QLabel(QStringLiteral("Encoder:"));
    g->addWidget(m_jpegEncLabel, row, 0);
    m_jpegEnc = new QComboBox;
    m_jpegEnc->setObjectName(QStringLiteral("optJpegEncoder"));
    m_jpegEnc->addItem(QStringLiteral("Standard (libjpeg-turbo)"));
    m_jpegEnc->addItem(codec::jpegEncoderAvailable(1) ? QStringLiteral("MozJPEG %1").arg(codec::jpegEncoderVersion(1)) : QStringLiteral("MozJPEG (not installed)"));
    m_jpegEnc->addItem(codec::jpegEncoderAvailable(2) ? QStringLiteral("jpegli %1").arg(codec::jpegEncoderVersion(2)) : QStringLiteral("jpegli (not installed)"));
    for (int i = 1; i <= 2; ++i) if (!codec::jpegEncoderAvailable(i)) m_jpegEnc->setItemData(i, 0, Qt::UserRole - 1);
    g->addWidget(m_jpegEnc, row++, 1, 1, 2);
    m_progressive = new QCheckBox(QStringLiteral("Progressive"));
    m_progressive->setObjectName(QStringLiteral("optJpegProgressive"));
    m_optimize = new QCheckBox(QStringLiteral("Optimise"));
    m_optimize->setObjectName(QStringLiteral("optJpegOptimize"));
    {
        auto *r2 = new QHBoxLayout;
        r2->setContentsMargins(0, 0, 0, 0);
        r2->addWidget(m_progressive);
        r2->addWidget(m_optimize);
        r2->addStretch();
        g->addLayout(r2, row++, 1, 1, 2);
    }
    m_subLabel = new QLabel(QStringLiteral("Subsampling:"));
    g->addWidget(m_subLabel, row, 0);
    m_sub = new QComboBox;
    m_sub->setObjectName(QStringLiteral("optJpegSubsampling"));
    m_sub->addItems({QStringLiteral("4:4:4 (best colour)"), QStringLiteral("4:2:2"), QStringLiteral("4:2:0 (smallest)")});
    g->addWidget(m_sub, row++, 1, 1, 2);
    m_smoothLabel = new QLabel(QStringLiteral("Smoothing:"));
    g->addWidget(m_smoothLabel, row, 0);
    m_smooth = new PopupSliderField(0, 8);
    m_smooth->setObjectName(QStringLiteral("optJpegSmoothing"));
    g->addWidget(m_smooth, row++, 1, Qt::AlignLeft);
    m_sharpen = new QCheckBox(QStringLiteral("Sharpen edges"));
    m_sharpen->setObjectName(QStringLiteral("optJpegSharpen"));
    g->addWidget(m_sharpen, row++, 1, 1, 2);
    m_strip = new QCheckBox(QStringLiteral("Strip meta"));
    m_strip->setObjectName(QStringLiteral("optJpegStrip"));
    m_keepIcc = new QCheckBox(QStringLiteral("Keep ICC"));
    m_keepIcc->setObjectName(QStringLiteral("optJpegKeepIcc"));
    {
        auto *r2 = new QHBoxLayout;
        r2->setContentsMargins(0, 0, 0, 0);
        r2->addWidget(m_strip);
        r2->addWidget(m_keepIcc);
        r2->addStretch();
        g->addLayout(r2, row++, 1, 1, 2);
    }
    // ---- BMP
    m_bmpLabel = new QLabel(QStringLiteral("Bit depth:"));
    g->addWidget(m_bmpLabel, row, 0);
    m_bmpBits = new QComboBox;
    m_bmpBits->setObjectName(QStringLiteral("optBmpBits"));
    for (int b : {1, 4, 8, 16, 24, 32}) m_bmpBits->addItem(b == 16 ? QStringLiteral("16-bit (RGB 565)") : b == 32 ? QStringLiteral("32-bit (alpha)") : QStringLiteral("%1-bit").arg(b), b);
    m_bmpRle = new QCheckBox(QStringLiteral("RLE"));
    m_bmpRle->setObjectName(QStringLiteral("optBmpRle"));
    {
        auto *r2 = new QHBoxLayout;
        r2->setContentsMargins(0, 0, 0, 0);
        r2->addWidget(m_bmpBits);
        r2->addWidget(m_bmpRle);
        g->addLayout(r2, row++, 1, 1, 2);
    }
    // ---- ICO / CUR / ICNS
    m_iconLabel = new QLabel(QStringLiteral("Sizes:"));
    g->addWidget(m_iconLabel, row, 0, Qt::AlignTop);
    m_iconBox = new QWidget;
    {
        auto *ig = new QGridLayout(m_iconBox);
        ig->setContentsMargins(0, 0, 0, 0);
        ig->setHorizontalSpacing(4);
        ig->setVerticalSpacing(0);
        const QVector<int> &sz = ImageIO::iconSizes();
        for (int i = 0; i < sz.size(); ++i) {
            auto *c = new QCheckBox(QString::number(sz[i]));
            c->setObjectName(QStringLiteral("optIcon%1").arg(sz[i]));
            m_iconChecks << c;
            ig->addWidget(c, i / 5, i % 5);
        }
    }
    g->addWidget(m_iconBox, row++, 1, 1, 2);
    m_hotLabel = new QLabel(QStringLiteral("Hotspot:"));
    g->addWidget(m_hotLabel, row, 0);
    m_hotX = new PopupSliderField; m_hotX->setObjectName(QStringLiteral("optHotX")); m_hotX->setRange(0, 4096); m_hotX->setPrefix(QStringLiteral("x "));
    m_hotY = new PopupSliderField; m_hotY->setObjectName(QStringLiteral("optHotY")); m_hotY->setRange(0, 4096); m_hotY->setPrefix(QStringLiteral("y "));
    {
        auto *r2 = new QHBoxLayout;
        r2->setContentsMargins(0, 0, 0, 0);
        r2->addWidget(m_hotX);
        r2->addWidget(m_hotY);
        r2->addStretch();
        g->addLayout(r2, row++, 1, 1, 2);
    }
    // ---- DDS / KTX2
    m_texLabel = new QLabel(QStringLiteral("Texture:"));
    g->addWidget(m_texLabel, row, 0);
    m_tex = new QComboBox;
    m_tex->setObjectName(QStringLiteral("optTexture"));
    m_tex->addItems({QStringLiteral("RGBA8 (uncompressed)"), QStringLiteral("BC1 / DXT1"), QStringLiteral("BC3 / DXT5")});
    g->addWidget(m_tex, row++, 1, 1, 2);
    m_mipmaps = new QCheckBox(QStringLiteral("Mipmaps"));
    m_mipmaps->setObjectName(QStringLiteral("optMipmaps"));
    m_ktxZstd = new QCheckBox(QStringLiteral("Zstd"));
    m_ktxZstd->setObjectName(QStringLiteral("optKtxZstd"));
    {
        auto *r2 = new QHBoxLayout;
        r2->setContentsMargins(0, 0, 0, 0);
        r2->addWidget(m_mipmaps);
        r2->addWidget(m_ktxZstd);
        r2->addStretch();
        g->addLayout(r2, row++, 1, 1, 2);
    }
    m_ktxZstdLevelLabel = new QLabel(QStringLiteral("Zstd level:"));
    g->addWidget(m_ktxZstdLevelLabel, row, 0);
    m_ktxZstdLevel = new PopupSliderField(1, 22);
    m_ktxZstdLevel->setObjectName(QStringLiteral("optKtxZstdLevel"));
    m_ktxZstdLevel->setToolTip(QStringLiteral("KTX2 Zstandard level, 1–22. 19 is the previous default."));
    m_ktxZstdLevelLabel->setToolTip(m_ktxZstdLevel->toolTip());
    g->addWidget(m_ktxZstdLevel, row++, 1, Qt::AlignLeft);
    // ---- EXR / PCX
    m_exrLabel = new QLabel(QStringLiteral("Compression:"));
    g->addWidget(m_exrLabel, row, 0);
    m_exrComp = new QComboBox;
    m_exrComp->setObjectName(QStringLiteral("optExrCompression"));
    m_exrComp->addItems({QStringLiteral("None"), QStringLiteral("ZIP"), QStringLiteral("PIZ"), QStringLiteral("RLE"), QStringLiteral("ZIPS"),
                         QStringLiteral("PXR24"), QStringLiteral("B44"), QStringLiteral("DWAA")});
    g->addWidget(m_exrComp, row++, 1, 1, 2);
    m_exrHalf = new QCheckBox(QStringLiteral("Half float (16-bit)"));
    m_exrHalf->setObjectName(QStringLiteral("optExrHalf"));
    g->addWidget(m_exrHalf, row++, 1, 1, 2);
    // ---- C Header (.h), v0.2.6
    m_chBox = new QWidget;
    m_chBox->setObjectName(QStringLiteral("optCHeader"));
    {
        auto *cg = new QGridLayout(m_chBox);
        cg->setContentsMargins(0, 0, 0, 0);
        cg->setHorizontalSpacing(6);
        cg->setVerticalSpacing(4);
        int r = 0;
        auto mkCombo = [&](const char *name, const QString &label, const QStringList &items, const QString &tip) {
            auto *c = new QComboBox;
            c->setObjectName(QLatin1String(name));
            c->addItems(items);
            c->setToolTip(tip);
            c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            c->setMinimumContentsLength(10);
            cg->addWidget(new QLabel(label), r, 0);
            cg->addWidget(c, r++, 1, 1, 2);
            return c;
        };
        auto mkCheck = [&](const char *name, const QString &text, const QString &tip) {
            auto *c = new QCheckBox(text);
            c->setObjectName(QLatin1String(name));
            c->setToolTip(tip);
            return c;
        };
        cg->addWidget(new QLabel(QStringLiteral("Name:")), r, 0);
        m_chName = new QLineEdit;
        m_chName->setObjectName(QStringLiteral("optCName"));
        m_chName->setPlaceholderText(QStringLiteral("from the file name"));
        m_chName->setToolTip(QStringLiteral("C identifier for the arrays and defines (sanitised; empty = the export file name)"));
        cg->addWidget(m_chName, r++, 1, 1, 2);
        m_chStyle = mkCombo("optCStyle", QStringLiteral("Style:"), {QStringLiteral("C arrays"), QStringLiteral("XBM-style (1-bit)"),
                            QStringLiteral("GIMP C source (string)"), QStringLiteral("GIMP header (HEADER_PIXEL)")},
                            QStringLiteral("XBM-style: name_width / name_height / name_bits, 1-bit, LSB first, 1 = black\n"
                                           "GIMP C source: GIMP's .c export - the pixels as one short C string literal (RGBA, RGB or RGB565)\n"
                                           "GIMP header: GIMP's .h export - width, height, header_data and the HEADER_PIXEL macro (RGB)"));
        m_chPixel = mkCombo("optCPixel", QStringLiteral("Pixels:"), cheader::pixelNames(), QStringLiteral("Pixel format of the array data"));
        m_chDither = mkCombo("optCDither", QStringLiteral("Dither:"), fx2::ditherMethodNames(),
                             QStringLiteral("Dither before reducing to the pixel format (RGB565 / 555 / 4444, grey 4, 1-bit) or to the indexed palette"));
        m_chElement = mkCombo("optCElement", QStringLiteral("Elements:"), {QStringLiteral("Bytes (uint8_t)"), QStringLiteral("Packed words (uint16_t / uint32_t)")},
                              QStringLiteral("Packed words are possible for the 16 and 32-bit pixel formats"));
        m_chRadix = mkCombo("optCRadix", QStringLiteral("Values:"), {QStringLiteral("Hexadecimal"), QStringLiteral("Decimal")}, QString());
        cg->addWidget(new QLabel(QStringLiteral("Per line:")), r, 0);
        m_chPerLine = new PopupSliderField;
        m_chPerLine->setObjectName(QStringLiteral("optCPerLine"));
        m_chPerLine->setRange(1, 256);
        m_chPerLine->setToolTip(QStringLiteral("Values per line"));
        cg->addWidget(m_chPerLine, r++, 1, Qt::AlignLeft);
        m_chConstants = mkCombo("optCConstants", QStringLiteral("Sizes as:"), {QStringLiteral("#define NAME_WIDTH ..."), QStringLiteral("static const unsigned")}, QString());
        m_chFrameLayout = mkCombo("optCFrameLayout", QStringLiteral("Frames:"), {QStringLiteral("One array per frame"), QStringLiteral("One 2D array")},
                                  QStringLiteral("With Export all frames: separate arrays (plus a pointer table) or name_data[FRAMES][N]"));
        m_chConst = mkCheck("optCConst", QStringLiteral("const"), QStringLiteral("Declare the arrays const"));
        m_chStdint = mkCheck("optCStdint", QStringLiteral("#include <stdint.h>"),
                             QStringLiteral("Off: no #include and only built-in types - unsigned char / unsigned short / unsigned int (C89)"));
        m_chProgmem = mkCheck("optCProgmem", QStringLiteral("PROGMEM"), QStringLiteral("AVR / Arduino: keep the arrays in flash (defined empty elsewhere)"));
        m_chAligned = mkCheck("optCAligned", QStringLiteral("aligned(4)"), QStringLiteral("__attribute__((aligned(4)))"));
        m_chSection = mkCheck("optCSection", QStringLiteral("Section:"), QStringLiteral("__attribute__((section(\"...\")))"));
        m_chSectionName = new QLineEdit;
        m_chSectionName->setObjectName(QStringLiteral("optCSectionName"));
        m_chDelays = mkCheck("optCDelays", QStringLiteral("Frame delays array"), QStringLiteral("name_delays[FRAMES] in milliseconds"));
        m_chRle = mkCheck("optCRle", QStringLiteral("RLE compress"), QStringLiteral("PackBits run-length coding of the bytes; the header comment has a small decoder"));
        m_chRawBin = mkCheck("optCRawBin", QStringLiteral("Also write raw .bin"), QStringLiteral("NAME.bin next to NAME.h with the array bytes"));
        m_chLsb = mkCheck("optCLsbFirst", QStringLiteral("LSB first"), QStringLiteral("1 / 4-bit formats: the first pixel goes in the low bits"));
        m_chInvert = mkCheck("optCInvert", QStringLiteral("Invert bits"), QStringLiteral("1-bit: 1 = black (XBM-style: 1 = white)"));
        auto row2 = [&](std::initializer_list<QWidget *> ws) {
            auto *l = new QHBoxLayout;
            l->setContentsMargins(0, 0, 0, 0);
            for (QWidget *w : ws) l->addWidget(w);
            l->addStretch();
            cg->addLayout(l, r++, 1, 1, 2);
        };
        row2({m_chConst, m_chStdint});
        row2({m_chProgmem, m_chAligned});
        {
            auto *l = new QHBoxLayout;
            l->setContentsMargins(0, 0, 0, 0);
            l->addWidget(m_chSection);
            l->addWidget(m_chSectionName, 1);
            cg->addLayout(l, r++, 1, 1, 2);
        }
        row2({m_chLsb, m_chInvert});
        row2({m_chRle, m_chDelays});
        row2({m_chRawBin});
        m_chStrip = mkCheck("optCStrip", QStringLiteral("strip spaces/tabs/newlines from arrays"),
                            QStringLiteral("The array / string data on one line with no whitespace, e.g. {0xFF,0x00,...} (smallest file); off: readable lines"));
        row2({m_chStrip});
        // v0.2.7: GIMP C source options (as in GIMP's "Export Image as C-Source" dialog)
        m_chGimpBox = new QWidget;
        m_chGimpBox->setObjectName(QStringLiteral("optCGimp"));
        {
            auto *gl = new QGridLayout(m_chGimpBox);
            gl->setContentsMargins(0, 0, 0, 0);
            gl->setHorizontalSpacing(6);
            gl->setVerticalSpacing(4);
            m_chGimpMacros = mkCheck("optCGimpMacros", QStringLiteral("Use macros instead of a struct"), QStringLiteral("NAME_WIDTH / NAME_HEIGHT / NAME_BYTES_PER_PIXEL and NAME_pixel_data"));
            m_chGimpGlib = mkCheck("optCGimpGlib", QStringLiteral("Use GLib types (guint8*)"), QStringLiteral("guint8 / guint / gchar (needs glib.h) instead of unsigned char / unsigned int / char"));
            m_chGimpComment = mkCheck("optCGimpComment", QStringLiteral("Save comment:"), QStringLiteral("A comment field (struct) or NAME_COMMENT (macros)"));
            m_chGimpCommentText = new QLineEdit;
            m_chGimpCommentText->setObjectName(QStringLiteral("optCGimpCommentText"));
            m_chGimpOpacity = new PopupSliderField;
            m_chGimpOpacity->setObjectName(QStringLiteral("optCGimpOpacity"));
            m_chGimpOpacity->setRange(0, 100);
            m_chGimpOpacity->setSuffix(QStringLiteral(" %"));
            m_chGimpOpacity->setToolTip(QStringLiteral("GIMP's Opacity: scales alpha (RGBA) or blends onto the matte (RGB / RGB565; black matte = GIMP)"));
            gl->addWidget(m_chGimpMacros, 0, 0, 1, 2);
            gl->addWidget(m_chGimpGlib, 1, 0, 1, 2);
            gl->addWidget(m_chGimpComment, 2, 0);
            gl->addWidget(m_chGimpCommentText, 2, 1);
            gl->addWidget(new QLabel(QStringLiteral("Opacity:")), 3, 0);
            gl->addWidget(m_chGimpOpacity, 3, 1, Qt::AlignLeft);
            gl->setColumnStretch(1, 1);
        }
        cg->addWidget(m_chGimpBox, r++, 1, 1, 2);
        cg->setColumnStretch(1, 1);
    }
    g->addWidget(m_chBox, row++, 0, 1, 3);
    for (QComboBox *c : {m_preset, m_format}) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(16);
    }
    for (QComboBox *c : {m_pngType, m_pngTrans, m_jpegEnc, m_sub, m_bmpBits, m_tex, m_exrComp}) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(10);
    }
    m_pcx8 = new QCheckBox(QStringLiteral("8-bit palette"));
    m_pcx8->setObjectName(QStringLiteral("optPcx8"));
    g->addWidget(m_pcx8, row++, 0, 1, 3);
    m_animated = new QCheckBox(QStringLiteral("Export all frames (animation)"));
    g->addWidget(m_animated, row++, 0, 1, 3);
    m_matteLabel = new QLabel(QStringLiteral("Matte:"));
    g->addWidget(m_matteLabel, row, 0);
    m_matte = new ColorButton(this, false);
    m_matte->setObjectName(QStringLiteral("optMatte"));
    g->addWidget(m_matte, row++, 1, Qt::AlignLeft);
    m_optSize = new QToolButton;
    m_optSize->setObjectName(QStringLiteral("optimizeToSize"));
    m_optSize->setAutoRaise(true);
    m_optSize->setToolTip(QStringLiteral("Optimize to Size: pick a target file size and search quality, colours or compression"));
    m_optSize->setText(QStringLiteral("Optimize to Size..."));
    {
        // A compact clamp (Fireworks-style) so the control is recognisable next to the size readout.
        QPixmap pm(16, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(60, 60, 60), 1.6));
        p.setBrush(QColor(200, 200, 200));
        p.drawRoundedRect(QRectF(2, 3, 5, 10), 1.2, 1.2);
        p.drawRoundedRect(QRectF(9, 3, 5, 10), 1.2, 1.2);
        p.setPen(QPen(QColor(40, 40, 40), 1.8));
        p.drawLine(QPointF(5, 8), QPointF(11, 8));
        p.setBrush(QColor(80, 80, 80));
        p.drawEllipse(QRectF(6.5, 6.5, 3, 3));
        p.end();
        m_optSize->setIcon(QIcon(pm));
        m_optSize->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    g->addWidget(m_optSize, row++, 0, 1, 3);
    connect(m_optSize, &QToolButton::clicked, this, &OptimizeWidget::optimizeToSizeRequested);
    m_size = new QLabel;
    m_size->setProperty("lwNote", true);   // themed (QLabel[lwNote])
    m_size->setWordWrap(true);
    m_size->setVisible(false);
    g->addWidget(m_size, row++, 0, 1, 3);
    m_table = new ColorTableView;
    m_table->setObjectName(QStringLiteral("optColorTable"));
    g->addWidget(m_table, row++, 0, 1, 3);
    g->setColumnStretch(1, 1);

    connect(m_preset, &QComboBox::activated, this, [this](int i) {
        if (i <= 0) return;
        const ExportOptions o = exportpresets::apply(i - 1, m_o);
        setOptions(o);
        emit optionsChanged(m_o);
    });
    auto changed = [this] { if (!m_updating) { fromUi(); emit optionsChanged(m_o); } };
    connect(m_format, &QComboBox::currentIndexChanged, this, changed);
    for (QSpinBox *sb : {m_quality, m_effort, m_colors, m_ditherAmount, m_loss, m_pngComp, m_tinyMin, m_tinyMax, m_tinySpeed, m_tinyDither, m_smooth, m_hotX, m_hotY, m_chPerLine, m_webpAlpha, m_avifSpeed, m_avifAlpha, m_jxlEffort, m_ktxZstdLevel})
        connect(sb, &QSpinBox::valueChanged, this, changed);
    connect(m_chGimpOpacity, &QSpinBox::valueChanged, this, changed);
    for (QCheckBox *c : {m_chGimpMacros, m_chGimpGlib, m_chGimpComment, m_chStrip}) connect(c, &QCheckBox::toggled, this, changed);
    connect(m_chGimpCommentText, &QLineEdit::textEdited, this, changed);
    // the strip choice is remembered (QSettings) as the default for new documents and exports
    connect(m_chStrip, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating) return;
        cheader_setStripDefault(on);
        QSettings().setValue(QStringLiteral("export/chStrip"), on);
    });
    for (QCheckBox *c : {m_lossless, m_dither, m_animated, m_alpha, m_tgaRle, m_png16, m_interlace, m_editable, m_tiny, m_progressive, m_optimize,
                         m_sharpen, m_strip, m_keepIcc, m_bmpRle, m_mipmaps, m_ktxZstd, m_exrHalf, m_pcx8, m_webpSharpYuv, m_webpExact, m_avifAlphaAuto,
                         m_chConst, m_chProgmem, m_chAligned, m_chSection, m_chStdint, m_chDelays, m_chRle, m_chRawBin, m_chLsb, m_chInvert})
        connect(c, &QCheckBox::toggled, this, changed);
    for (QLineEdit *e : {m_chName, m_chSectionName}) connect(e, &QLineEdit::textEdited, this, changed);
    for (QCheckBox *c : m_iconChecks) connect(c, &QCheckBox::toggled, this, changed);
    for (QComboBox *c : {m_palette, m_netpbm, m_tiffComp, m_tiffDepth, m_tgaDepth, m_tgaOrigin, m_pngType, m_pngTrans, m_jpegEnc, m_sub, m_bmpBits, m_tex, m_exrComp, m_avifChroma, m_avifDepth, m_heifPreset, m_heifChroma,
                         m_chPixel, m_chElement, m_chRadix, m_chConstants, m_chFrameLayout, m_chStyle, m_chDither})
        connect(c, &QComboBox::currentIndexChanged, this, changed);
    connect(m_loadPalette, &QToolButton::clicked, this, &OptimizeWidget::loadCustomPalette);
    connect(m_matte, &ColorButton::colorPicked, this, changed);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    setOptions(m_o);
}

static bool layoutItemExplicitlyVisible(QLayoutItem *it)
{
    if (!it) return false;
    if (it->spacerItem()) return false;
    if (QWidget *w = it->widget())
        return !w->isHidden();
    if (QLayout *ly = it->layout()) {
        for (int i = 0; i < ly->count(); ++i)
            if (layoutItemExplicitlyVisible(ly->itemAt(i))) return true;
        return false;
    }
    return !it->isEmpty();
}

// Size a QGridLayout from visible rows only. Hidden format-specific rows
// (and empty size/colour-table placeholders) must not inflate the hint or
// a content-sized Optimize panel is left with slack above Export.
QSize OptimizeWidget::sizeHint() const
{
    return QSize(qMax(160, QWidget::sizeHint().width()), heightForWidth(qMax(160, width())));
}

QSize OptimizeWidget::minimumSizeHint() const
{
    return QSize(120, heightForWidth(qMax(120, width() > 0 ? width() : 120)));
}

int OptimizeWidget::heightForWidth(int w) const
{
    auto *g = qobject_cast<QGridLayout *>(layout());
    if (!g) return QWidget::sizeHint().height();
    const auto m = g->contentsMargins();
    const int inner = qMax(1, w - m.left() - m.right());
    const int cols = qMax(1, g->columnCount());
    // Column 0 is the short label column; remaining width is shared.
    const int labelCol = qMin(inner / 3, 80);
    int h = m.top() + m.bottom();
    int visRows = 0;
    for (int r = 0; r < g->rowCount(); ++r) {
        int rowH = 0;
        bool vis = false;
        QLayoutItem *seen = nullptr;
        for (int c = 0; c < cols; ++c) {
            QLayoutItem *it = g->itemAtPosition(r, c);
            if (!it || it == seen || !layoutItemExplicitlyVisible(it)) continue;
            seen = it;
            vis = true;
            int avail = inner;
            if (c >= 1) avail = inner - labelCol;
            int ih;
            if (it->hasHeightForWidth())
                ih = it->heightForWidth(qMax(1, avail));
            else
                ih = it->sizeHint().height();
            rowH = qMax(rowH, ih);
        }
        if (!vis) continue;
        h += rowH;
        ++visRows;
    }
    if (visRows > 1)
        h += g->verticalSpacing() * (visRows - 1);
    return h;
}

void OptimizeWidget::setSizeText(const QString &t)
{
    m_size->setText(t);
    m_size->setVisible(!t.trimmed().isEmpty());
    updateGeometry();
}

void OptimizeWidget::setOptions(const ExportOptions &o) {
    m_o = o;
    sync();
}

QStringList OptimizeWidget::presetNames() {
    QStringList l;
    for (const auto &p : kPresets) l << QString::fromLatin1(p.name);
    return l;
}

void OptimizeWidget::setColorTable(const QVector<QRgb> &c) {
    m_table->setColors(c);
    const Fmt f = m_o.format;
    m_table->setVisible(!c.isEmpty() && (f == Fmt::GIF || ((f == Fmt::PNG || f == Fmt::APNG) && (m_o.pngType == 2 || m_o.tinyPng))));
}

void OptimizeWidget::loadCustomPalette() {
    const QString path = filedialogs::getOpenFileName(this, QStringLiteral("Load Palette"), QString(),
                                                      QStringLiteral("Palettes (*.act *.gpl *.pal *.gif *.png);;All files (*)"));
    if (path.isEmpty()) return;
    QString err;
    const QVector<QRgb> pal = quant::loadPalette(path, &err);
    if (pal.isEmpty()) { m_size->setText(QStringLiteral("Could not load palette: %1").arg(err)); return; }
    m_o.customPalette = pal;
    m_o.palette = quant::Custom;
    m_o.colors = qBound(2, int(pal.size()), 256);
    sync();
    emit optionsChanged(m_o);
}

void OptimizeWidget::sync() {
    m_updating = true;
    m_format->setCurrentIndex(qMax(0, m_format->findData(int(m_o.format))));
    m_quality->setValue(m_o.quality);
    m_lossless->setChecked(m_o.lossless);
    m_effort->setValue(m_o.webpEffort);
    m_webpAlpha->setValue(m_o.webpAlphaQuality);
    m_webpSharpYuv->setChecked(m_o.webpSharpYuv);
    m_webpExact->setChecked(m_o.webpExact);
    m_avifSpeed->setValue(m_o.avifSpeed);
    m_avifChroma->setCurrentIndex(qMax(0, m_avifChroma->findData(m_o.avifChroma)));
    m_avifDepth->setCurrentIndex(qMax(0, m_avifDepth->findData(m_o.avifDepth)));
    m_avifAlphaAuto->setChecked(m_o.avifAlphaQuality < 0);
    m_avifAlpha->setValue(m_o.avifAlphaQuality < 0 ? qBound(0, m_o.quality + 10, 100) : m_o.avifAlphaQuality);
    m_avifAlpha->setEnabled(m_o.avifAlphaQuality >= 0);
    m_jxlEffort->setValue(m_o.jxlEffort);
    m_heifPreset->setCurrentIndex(qMax(0, m_heifPreset->findData(m_o.heifPreset)));
    m_heifChroma->setCurrentIndex(qMax(0, m_heifChroma->findData(m_o.heifChroma)));
    m_tinyDither->setValue(m_o.tinyDither);
    m_ktxZstdLevel->setValue(m_o.ktxZstdLevel);
    m_colors->setValue(m_o.colors);
    m_ditherAmount->setValue(m_o.ditherAmount);
    m_ditherAmount->setEnabled(m_o.dither);
    m_loss->setValue(m_o.loss);
    m_palette->setCurrentIndex(qMax(0, quant::paletteOrder().indexOf(m_o.palette)));
    m_dither->setChecked(m_o.dither);
    m_netpbm->setCurrentIndex(m_o.netpbmAscii ? 1 : 0);
    m_animated->setChecked(m_o.animated);
    m_matte->setColor(m_o.matte);
    m_tiffComp->setCurrentIndex(qBound(0, m_o.tiffCompression, 3));
    m_tiffDepth->setCurrentIndex(m_o.tiff16 ? 1 : 0);
    m_tgaDepth->setCurrentIndex(m_o.alpha ? 1 : 0);
    m_alpha->setChecked(m_o.alpha);
    m_tgaRle->setChecked(m_o.tgaRle);
    m_tgaOrigin->setCurrentIndex(m_o.tgaTopLeft ? 1 : 0);
    m_pngType->setCurrentIndex(qBound(0, m_o.pngType, 3));
    m_pngTrans->setCurrentIndex(qBound(0, m_o.pngTransparency, 2));
    m_png16->setChecked(m_o.png16);
    m_interlace->setChecked(m_o.pngInterlace);
    m_pngComp->setValue(m_o.pngCompression);
    m_tiny->setChecked(m_o.tinyPng && codec::tinyAvailable());
    m_tinyMin->setValue(m_o.tinyQualityMin);
    m_tinyMax->setValue(m_o.tinyQualityMax);
    m_tinySpeed->setValue(m_o.tinySpeed);
    m_editable->setChecked(m_o.pngEditable);
    m_jpegEnc->setCurrentIndex(qBound(0, m_o.jpegEncoder, 2));
    m_progressive->setChecked(m_o.jpegProgressive);
    m_optimize->setChecked(m_o.jpegOptimize);
    m_sub->setCurrentIndex(qBound(0, m_o.jpegSubsampling, 2));
    m_smooth->setValue(m_o.jpegSmoothing);
    m_sharpen->setChecked(m_o.jpegSharpenEdges);
    m_strip->setChecked(m_o.jpegStripMeta);
    m_keepIcc->setChecked(m_o.jpegKeepIcc);
    m_keepIcc->setEnabled(m_o.jpegStripMeta);
    m_bmpBits->setCurrentIndex(qMax(0, m_bmpBits->findData(m_o.bmpBits)));
    m_bmpRle->setChecked(m_o.bmpRle);
    m_bmpRle->setEnabled(m_o.bmpBits == 4 || m_o.bmpBits == 8);
    m_tex->setCurrentIndex(qBound(0, m_o.ddsFormat, 2));
    m_mipmaps->setChecked(m_o.mipmaps);
    m_ktxZstd->setChecked(m_o.ktxZstd);
    m_exrComp->setCurrentIndex(qBound(0, m_o.exrCompression, 7));
    m_exrHalf->setChecked(m_o.exrHalf);
    m_pcx8->setChecked(m_o.pcx8);
    m_hotX->setValue(m_o.curHotX);
    m_hotY->setValue(m_o.curHotY);
    m_chPixel->setCurrentIndex(qBound(0, m_o.chPixel, int(cheader::PixelCount) - 1));
    m_chDither->setCurrentIndex(qBound(0, m_o.chDither, int(fx2::kDitherMethods) - 1));
    m_chElement->setCurrentIndex(m_o.chElement ? 1 : 0);
    m_chRadix->setCurrentIndex(m_o.chHex ? 0 : 1);
    m_chPerLine->setValue(m_o.chPerLine);
    m_chConstants->setCurrentIndex(m_o.chDefines ? 0 : 1);
    m_chFrameLayout->setCurrentIndex(m_o.chFrameLayout ? 1 : 0);
    m_chStyle->setCurrentIndex(qBound(0, m_o.chStyle, 3));
    m_chGimpMacros->setChecked(m_o.chGimpMacros);
    m_chGimpGlib->setChecked(m_o.chGimpGlib);
    m_chGimpComment->setChecked(m_o.chGimpComment);
    if (m_chGimpCommentText->text() != m_o.chGimpCommentText) m_chGimpCommentText->setText(m_o.chGimpCommentText);
    m_chGimpOpacity->setValue(m_o.chGimpOpacity);
    m_chStrip->setChecked(m_o.chStrip);
    m_chConst->setChecked(m_o.chConst);
    m_chProgmem->setChecked(m_o.chProgmem);
    m_chAligned->setChecked(m_o.chAligned);
    m_chSection->setChecked(m_o.chSection);
    if (m_chSectionName->text() != m_o.chSectionName) m_chSectionName->setText(m_o.chSectionName);
    m_chStdint->setChecked(m_o.chStdint);
    m_chDelays->setChecked(m_o.chDelays);
    m_chRle->setChecked(m_o.chRle);
    m_chRawBin->setChecked(m_o.chRawBin);
    m_chLsb->setChecked(m_o.chLsbFirst);
    m_chInvert->setChecked(m_o.chInvert);
    if (m_chName->text() != m_o.chName) m_chName->setText(m_o.chName);
    const Fmt f = m_o.format;
    const int iconMask = f == Fmt::ICNS ? m_o.icnsSizes : m_o.icoSizes;
    const QVector<int> &isz = ImageIO::iconSizes();
    for (int i = 0; i < m_iconChecks.size(); ++i) {
        m_iconChecks[i]->setChecked(iconMask & (1 << i));
        // ICO / CUR stop at 256; ICNS has no 24 or 48
        const bool ok = f == Fmt::ICNS ? (isz[i] != 24 && isz[i] != 48) : isz[i] <= 256;
        m_iconChecks[i]->setVisible(ok);
    }
    const bool png = f == Fmt::PNG, apng = f == Fmt::APNG, png8 = png && m_o.pngType == 2 && !m_o.tinyPng;
    const bool q = ImageIO::hasQuality(f), ll = ImageIO::hasLossless(f), gif = f == Fmt::GIF, pnm = f == Fmt::PPM || f == Fmt::PGM || f == Fmt::PBM;
    const bool tiff = f == Fmt::TIFF, tga = f == Fmt::TGA, jpeg = f == Fmt::JPEG, icon = f == Fmt::ICO || f == Fmt::CUR || f == Fmt::ICNS;
    const bool tex = f == Fmt::DDS || f == Fmt::KTX2;
    const bool noAlpha = !ImageIO::hasAlpha(m_o);
    const bool ch = f == Fmt::CHeader, chXbm = ch && m_o.chStyle == 1;
    const bool chIndexed = ch && !chXbm && cheader::isIndexed(m_o.chPixel);
    const bool chMono = ch && (chXbm || m_o.chPixel == cheader::Mono1);
    const bool indexed = gif || png8 || chIndexed;
    m_tiffCompLabel->setVisible(tiff); m_tiffComp->setVisible(tiff);
    m_depthLabel->setVisible(tiff || tga); m_tiffDepth->setVisible(tiff); m_tgaDepth->setVisible(tga);
    m_alpha->setVisible(tiff || f == Fmt::PAM || (png && m_o.pngType == 3 && !m_o.tinyPng) || (f == Fmt::PCX && !m_o.pcx8));
    m_tgaRle->setVisible(tga); m_tgaOriginLabel->setVisible(tga); m_tgaOrigin->setVisible(tga);
    m_animated->setText(tiff ? QStringLiteral("Export all frames (multi-page)") : QStringLiteral("Export all frames (animation)"));
    m_qualityLabel->setVisible(q); m_quality->setVisible(q);
    m_quality->setEnabled(!(ll && m_o.lossless));
    m_lossless->setVisible(ll);
    const bool webp = f == Fmt::WEBP;
    m_effortLabel->setVisible(webp); m_effort->setVisible(webp);
    m_webpAlphaLabel->setVisible(webp); m_webpAlpha->setVisible(webp);
    m_webpSharpYuv->setVisible(webp);
    m_webpExact->setVisible(webp && m_o.lossless);
    const bool avif = f == Fmt::AVIF;
    m_avifSpeedLabel->setVisible(avif); m_avifSpeed->setVisible(avif);
    m_avifChromaLabel->setVisible(avif && !m_o.lossless); m_avifChroma->setVisible(avif && !m_o.lossless);
    m_avifDepthLabel->setVisible(avif); m_avifDepth->setVisible(avif);
    m_avifAlphaLabel->setVisible(avif); m_avifAlpha->setVisible(avif); m_avifAlphaAuto->setVisible(avif);
    const bool jxl = f == Fmt::JXL;
    m_jxlEffortLabel->setVisible(jxl); m_jxlEffort->setVisible(jxl);
    const bool heif = f == Fmt::HEIC;
    const bool heifChroma = heif && !codec::heifEncoderChromaParameterName().isEmpty();
    m_heifPresetLabel->setVisible(heif); m_heifPreset->setVisible(heif);
    m_heifChromaLabel->setVisible(heifChroma); m_heifChroma->setVisible(heifChroma);
    m_colorsLabel->setVisible(indexed); m_colors->setVisible(indexed);
    m_paletteLabel->setVisible(indexed); m_palette->setVisible(indexed); m_loadPalette->setVisible(indexed);
    m_dither->setVisible(indexed); m_ditherAmount->setVisible(indexed);
    m_lossLabel->setVisible(indexed); m_loss->setVisible(indexed);
    m_netpbmLabel->setVisible(pnm); m_netpbm->setVisible(pnm);
    m_pngTypeLabel->setVisible(png); m_pngType->setVisible(png);
    m_pngType->setEnabled(!m_o.tinyPng);
    m_pngTransLabel->setVisible(png8); m_pngTrans->setVisible(png8);
    m_png16->setVisible(png && m_o.pngType != 2 && !m_o.tinyPng);
    m_interlace->setVisible(png && !m_o.tinyPng);
    m_pngCompLabel->setVisible(png && !m_o.tinyPng); m_pngComp->setVisible(png && !m_o.tinyPng);
    m_tiny->setVisible(png || apng);
    m_tinyBox->setVisible((png || apng) && m_o.tinyPng);
    m_editable->setVisible(png);
    for (QWidget *w : std::initializer_list<QWidget *>{m_jpegEncLabel, m_jpegEnc, m_progressive, m_optimize, m_subLabel, m_sub, m_smoothLabel, m_smooth,
                                                        m_sharpen, m_strip, m_keepIcc})
        w->setVisible(jpeg);
    m_bmpLabel->setVisible(f == Fmt::BMP); m_bmpBits->setVisible(f == Fmt::BMP); m_bmpRle->setVisible(f == Fmt::BMP);
    m_iconLabel->setVisible(icon); m_iconBox->setVisible(icon);
    m_hotLabel->setVisible(f == Fmt::CUR); m_hotX->setVisible(f == Fmt::CUR); m_hotY->setVisible(f == Fmt::CUR);
    m_texLabel->setVisible(tex); m_tex->setVisible(tex); m_mipmaps->setVisible(tex); m_ktxZstd->setVisible(f == Fmt::KTX2);
    m_ktxZstdLevelLabel->setVisible(f == Fmt::KTX2 && m_o.ktxZstd); m_ktxZstdLevel->setVisible(f == Fmt::KTX2 && m_o.ktxZstd);
    m_exrLabel->setVisible(f == Fmt::EXR); m_exrComp->setVisible(f == Fmt::EXR); m_exrHalf->setVisible(f == Fmt::EXR);
    m_pcx8->setVisible(f == Fmt::PCX);
    m_chBox->setVisible(ch);
    if (ch) {
        const bool gimp = m_o.chStyle >= 2, gimpSrc = m_o.chStyle == 2, arrays = !chXbm && !gimp;
        const bool rle = m_o.chRle && !chXbm;
        // GIMP C source writes RGBA8888, RGB888 or RGB565 (little-endian) only; GIMP header always RGB
        if (auto *model = qobject_cast<QStandardItemModel *>(m_chPixel->model()))
            for (int i = 0; i < model->rowCount(); ++i)
                if (QStandardItem *it = model->item(i)) it->setEnabled(!gimp || i == cheader::RGBA8888 || i == cheader::RGB888 || i == cheader::RGB565LE);
        m_chPixel->setEnabled(arrays || gimpSrc);
        m_chElement->setEnabled(arrays && !rle && cheader::wordBits(m_o.chPixel) > 0);
        m_chRadix->setEnabled(arrays);
        m_chPerLine->setEnabled(arrays && !m_o.chStrip);
        m_chConstants->setEnabled(arrays);
        m_chFrameLayout->setEnabled(arrays && !rle && m_o.animated);
        m_chDelays->setEnabled(!gimp && m_o.animated);
        m_chStdint->setEnabled(arrays);
        m_chConst->setEnabled(!gimp);
        m_chProgmem->setEnabled(arrays);
        m_chAligned->setEnabled(!gimp);
        m_chSection->setEnabled(!gimp);
        m_chRle->setEnabled(arrays || gimpSrc);
        m_chRle->setToolTip(gimpSrc ? QStringLiteral("GIMP 1-byte run-length encoding of the pixels; NAME_RUN_LENGTH_DECODE unpacks it")
                                    : QStringLiteral("PackBits run-length coding of the bytes; the header comment has a small decoder"));
        m_chSectionName->setEnabled(!gimp && m_o.chSection);
        m_chGimpBox->setVisible(gimp);
        for (QWidget *w : std::initializer_list<QWidget *>{m_chGimpMacros, m_chGimpGlib, m_chGimpComment}) w->setEnabled(gimpSrc);
        m_chGimpCommentText->setEnabled(gimpSrc && m_o.chGimpComment);
        const int bpp = chXbm ? 1 : cheader::bitsPerPixel(m_o.chPixel);
        m_chLsb->setEnabled(arrays && bpp < 8);
        m_chInvert->setEnabled(chMono && !gimp);
        // 1-bit keeps its Dither switch (Floyd-Steinberg vs a 50 % threshold)
        if (chMono) { m_dither->setVisible(true); m_ditherAmount->setVisible(true); }
        if (chIndexed && m_o.chPixel == cheader::Indexed4) m_colors->setMaximum(16); else m_colors->setMaximum(256);
    } else m_colors->setMaximum(256);
    m_animated->setVisible(ImageIO::canAnimate(f));
    m_matteLabel->setVisible(noAlpha); m_matte->setVisible(noAlpha);
    if (!(indexed || ((png || apng) && m_o.tinyPng))) m_table->setVisible(false);
    // reflect a matching preset
    int match = 0;
    for (int i = 0; i < int(std::size(kPresets)); ++i) {
        const Preset &p = kPresets[i];
        if (p.f != f || p.anim != (m_o.animated && ImageIO::canAnimate(f))) continue;
        if (q && !(ll && m_o.lossless) && p.q != m_o.quality) continue;
        if (ll && p.lossless != m_o.lossless) continue;
        const bool pIndexed = gif || (png && p.pngType == 2);
        if (pIndexed && (p.colors != m_o.colors || p.palette != m_o.palette || p.dither != m_o.dither || m_o.loss != 0 || (m_o.dither && m_o.ditherAmount != 100))) continue;
        if (png && (p.pngType != m_o.pngType || m_o.tinyPng || m_o.png16 || m_o.pngEditable || (p.pngType == 2 && m_o.pngTransparency != 2))) continue;
        if (jpeg && (p.jpegEnc != m_o.jpegEncoder || p.jpegProg != m_o.jpegProgressive || p.jpegOpt != m_o.jpegOptimize
                     || m_o.jpegSubsampling != 2 || m_o.jpegSmoothing != 0 || m_o.jpegSharpenEdges)) continue;
        if (pnm && p.ascii != m_o.netpbmAscii) continue;
        if (tiff && (p.tiffComp != m_o.tiffCompression || p.b16 != m_o.tiff16 || p.alpha != m_o.alpha)) continue;
        if (tga && (p.alpha != m_o.alpha || p.rle != m_o.tgaRle || m_o.tgaTopLeft)) continue;
        if (ch && p.chStyle != m_o.chStyle) continue;
        if (ch && p.chStyle == 2 && (p.chPixel != m_o.chPixel || m_o.chRle != (p.chPixel == cheader::RGB565LE))) continue;
        if (ch && p.chStyle == 0 && (p.chPixel != m_o.chPixel || p.chElement != m_o.chElement || m_o.chRle || m_o.chLsbFirst || m_o.chInvert || !m_o.chHex)) continue;
        if (chIndexed && (p.colors != m_o.colors || p.palette != m_o.palette || p.dither != m_o.dither)) continue;
        match = i + 1;
        break;
    }
    m_preset->setCurrentIndex(match);
    m_updating = false;
    if (auto *ly = layout())
        ly->invalidate();
    updateGeometry();
}

void OptimizeWidget::fromUi() {
    m_o.format = Fmt(m_format->currentData().toInt());
    m_o.quality = m_quality->value();
    m_o.lossless = m_lossless->isChecked();
    m_o.webpEffort = m_effort->value();
    m_o.webpAlphaQuality = m_webpAlpha->value();
    m_o.webpSharpYuv = m_webpSharpYuv->isChecked();
    m_o.webpExact = m_webpExact->isChecked();
    m_o.avifSpeed = m_avifSpeed->value();
    m_o.avifChroma = m_avifChroma->currentData().toInt();
    m_o.avifDepth = m_avifDepth->currentData().toInt();
    m_o.avifAlphaQuality = m_avifAlphaAuto->isChecked() ? -1 : m_avifAlpha->value();
    m_o.jxlEffort = m_jxlEffort->value();
    m_o.heifPreset = m_heifPreset->currentData().toString();
    m_o.heifChroma = m_heifChroma->currentData().toInt();
    m_o.tinyDither = m_tinyDither->value();
    m_o.ktxZstdLevel = m_ktxZstdLevel->value();
    m_o.colors = m_colors->value();
    m_o.ditherAmount = m_ditherAmount->value();
    m_o.loss = m_loss->value();
    m_o.palette = quant::paletteOrder().value(m_palette->currentIndex(), 0);
    m_o.dither = m_dither->isChecked();
    m_o.netpbmAscii = m_netpbm->currentIndex() == 1;
    m_o.animated = m_animated->isChecked();
    m_o.matte = m_matte->color();
    m_o.tiffCompression = m_tiffComp->currentIndex();
    m_o.tiff16 = m_tiffDepth->currentIndex() == 1;
    m_o.tgaRle = m_tgaRle->isChecked();
    m_o.tgaTopLeft = m_tgaOrigin->currentIndex() == 1;
    // TIFF shows an alpha checkbox, TGA a 24/32-bit choice; both drive the same option
    if (m_o.format == Fmt::TGA) m_o.alpha = m_tgaDepth->currentIndex() == 1;
    else if (m_alpha->isVisible() || m_o.format == Fmt::TIFF) m_o.alpha = m_alpha->isChecked();
    m_o.pngType = m_pngType->currentIndex();
    m_o.pngTransparency = m_pngTrans->currentIndex();
    m_o.png16 = m_png16->isChecked();
    m_o.pngInterlace = m_interlace->isChecked();
    m_o.pngCompression = m_pngComp->value();
    m_o.tinyPng = m_tiny->isChecked();
    m_o.tinyQualityMin = qMin(m_tinyMin->value(), m_tinyMax->value());
    m_o.tinyQualityMax = qMax(m_tinyMin->value(), m_tinyMax->value());
    m_o.tinySpeed = m_tinySpeed->value();
    m_o.pngEditable = m_editable->isChecked();
    m_o.jpegEncoder = codec::jpegEncoderAvailable(m_jpegEnc->currentIndex()) ? m_jpegEnc->currentIndex() : 0;
    m_o.jpegProgressive = m_progressive->isChecked();
    m_o.jpegOptimize = m_optimize->isChecked();
    m_o.jpegSubsampling = m_sub->currentIndex();
    m_o.jpegSmoothing = m_smooth->value();
    m_o.jpegSharpenEdges = m_sharpen->isChecked();
    m_o.jpegStripMeta = m_strip->isChecked();
    m_o.jpegKeepIcc = m_keepIcc->isChecked();
    m_o.bmpBits = m_bmpBits->currentData().toInt();
    m_o.bmpRle = m_bmpRle->isChecked();
    m_o.ddsFormat = m_tex->currentIndex();
    m_o.mipmaps = m_mipmaps->isChecked();
    m_o.ktxZstd = m_ktxZstd->isChecked();
    m_o.exrCompression = m_exrComp->currentIndex();
    m_o.exrHalf = m_exrHalf->isChecked();
    m_o.pcx8 = m_pcx8->isChecked();
    m_o.curHotX = m_hotX->value();
    m_o.curHotY = m_hotY->value();
    m_o.chPixel = m_chPixel->currentIndex();
    m_o.chDither = qMax(0, m_chDither->currentIndex());
    m_o.chElement = m_chElement->currentIndex();
    m_o.chHex = m_chRadix->currentIndex() == 0;
    m_o.chPerLine = m_chPerLine->value();
    m_o.chDefines = m_chConstants->currentIndex() == 0;
    m_o.chFrameLayout = m_chFrameLayout->currentIndex();
    m_o.chStyle = m_chStyle->currentIndex();
    m_o.chConst = m_chConst->isChecked();
    m_o.chProgmem = m_chProgmem->isChecked();
    m_o.chAligned = m_chAligned->isChecked();
    m_o.chSection = m_chSection->isChecked();
    m_o.chSectionName = m_chSectionName->text();
    m_o.chStdint = m_chStdint->isChecked();
    m_o.chDelays = m_chDelays->isChecked();
    m_o.chRle = m_chRle->isChecked();
    m_o.chRawBin = m_chRawBin->isChecked();
    m_o.chLsbFirst = m_chLsb->isChecked();
    m_o.chInvert = m_chInvert->isChecked();
    m_o.chName = m_chName->text();
    m_o.chGimpMacros = m_chGimpMacros->isChecked();
    m_o.chGimpGlib = m_chGimpGlib->isChecked();
    m_o.chGimpComment = m_chGimpComment->isChecked();
    m_o.chGimpCommentText = m_chGimpCommentText->text();
    m_o.chGimpOpacity = m_chGimpOpacity->value();
    m_o.chStrip = m_chStrip->isChecked();
    if (m_o.chStyle == 2) m_o.chPixel = cheader::gimpPixel(2, m_o.chPixel);   // GIMP C source: RGBA8888 / RGB888 / RGB565LE
    if (m_o.format == Fmt::ICO || m_o.format == Fmt::CUR || m_o.format == Fmt::ICNS) {
        int mask = 0;
        for (int i = 0; i < m_iconChecks.size(); ++i) if (m_iconChecks[i]->isChecked() && !m_iconChecks[i]->isHidden()) mask |= 1 << i;
        if (mask) (m_o.format == Fmt::ICNS ? m_o.icnsSizes : m_o.icoSizes) = mask;
    }
    sync();
}

// ---------------------------------------------------------------- ColorTableView
ColorTableView::ColorTableView(QWidget *parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setVisible(false);
}

void ColorTableView::setColors(const QVector<QRgb> &c) {
    m_colors = c;
    setToolTip(QStringLiteral("%1 colours").arg(c.size()));
    updateGeometry();
    update();
}

static constexpr int kSwatch = 11;
QSize ColorTableView::sizeHint() const { return {16 * kSwatch + 2, heightForWidth(16 * kSwatch + 2)}; }
int ColorTableView::heightForWidth(int w) const {
    const int cols = qMax(1, (w - 2) / kSwatch);
    return qMax(1, int((m_colors.size() + cols - 1) / cols)) * kSwatch + 2;
}

void ColorTableView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const int cols = qMax(1, (width() - 2) / kSwatch);
    for (int i = 0; i < m_colors.size(); ++i) {
        const QRect r(1 + (i % cols) * kSwatch, 1 + (i / cols) * kSwatch, kSwatch - 1, kSwatch - 1);
        const QRgb c = m_colors[i];
        if (qAlpha(c) < 255) {
            p.fillRect(r, Qt::white);
            p.fillRect(QRect(r.topLeft(), QSize(r.width() / 2, r.height() / 2)), QColor(0xCC, 0xCC, 0xCC));
            p.fillRect(QRect(r.center() + QPoint(1, 1), r.bottomRight()), QColor(0xCC, 0xCC, 0xCC));
        }
        p.fillRect(r, QColor::fromRgba(c));
        if (qAlpha(c) == 0) { p.setPen(Qt::red); p.drawLine(r.bottomLeft(), r.topRight()); }
    }
}

// ---------------------------------------------------------------- NewDocumentDialog
NewDocumentDialog::NewDocumentDialog(QWidget *parent, bool readClipboard) : QDialog(parent), m_read(readClipboard) {
    setWindowTitle(QStringLiteral("New Document"));
    auto *f = new QFormLayout(this);
    auto spin = [](const QString &name) {
        auto *sb = new PopupSliderDoubleField;
        sb->setObjectName(name);
        sb->setKeyboardTracking(false);
        return sb;
    };
    m_w = spin(QStringLiteral("newWidth"));
    m_h = spin(QStringLiteral("newHeight"));
    m_units = new QComboBox;
    m_units->setObjectName(QStringLiteral("newUnits"));
    m_units->addItems({QStringLiteral("Pixels"), QStringLiteral("Inches"), QStringLiteral("Centimeters")});
    auto *wr = new QHBoxLayout;
    wr->addWidget(m_w, 1);
    wr->addWidget(m_units);
    f->addRow(QStringLiteral("Width:"), wr);
    f->addRow(QStringLiteral("Height:"), m_h);
    m_res = spin(QStringLiteral("newResolution"));
    m_res->setDecimals(2);
    m_resUnits = new QComboBox;
    m_resUnits->setObjectName(QStringLiteral("newResolutionUnits"));
    m_resUnits->addItems({QStringLiteral("Pixels/Inch"), QStringLiteral("Pixels/Centimeter")});
    auto *rr = new QHBoxLayout;
    rr->addWidget(m_res, 1);
    rr->addWidget(m_resUnits);
    f->addRow(QStringLiteral("Resolution:"), rr);
    m_bg = new QComboBox;
    m_bg->addItems({QStringLiteral("White"), QStringLiteral("Transparent"), QStringLiteral("Custom")});
    m_custom = new ColorButton(this, false);
    m_custom->setObjectName(QStringLiteral("newCanvasColor"));
    m_custom->setColor(QColor(0xCC, 0xDD, 0xEE));
    auto *row = new QHBoxLayout;
    row->addWidget(m_bg);
    row->addWidget(m_custom);
    f->addRow(QStringLiteral("Canvas colour:"), row);
    m_note = new QLabel;
    m_note->setObjectName(QStringLiteral("clipboardNote"));
    QFont nf = m_note->font();
    nf.setPointSizeF(nf.pointSizeF() * 0.9);
    m_note->setFont(nf);
    m_note->setEnabled(false);   // the dimmed "hint" look
    m_note->hide();
    f->addRow(m_note);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(bb);
    refresh();
    connect(m_w, &QDoubleSpinBox::valueChanged, this, [this] { sizeEdited(); });
    connect(m_h, &QDoubleSpinBox::valueChanged, this, [this] { sizeEdited(); });
    connect(m_res, &QDoubleSpinBox::valueChanged, this, [this] { resolutionEdited(); });
    connect(m_units, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(m_resUnits, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    if (m_read) {
        // Wayland only hands the clipboard to a focused window, and X11 may answer late: read it once
        // shown, and again whenever it changes while the dialog is open.
        if (QClipboard *cb = QGuiApplication::clipboard())
            connect(cb, &QClipboard::dataChanged, this, [this] { if (isVisible()) probeClipboard(); });
    }
}

void NewDocumentDialog::showEvent(QShowEvent *e) {
    QDialog::showEvent(e);
    if (m_read && !m_probed) {
        m_probed = true;
        QTimer::singleShot(0, this, [this] { probeClipboard(); });   // after the dialog has painted
    }
}

double NewDocumentDialog::unitFactor() const {
    switch (m_units->currentIndex()) {
    case Inches: return m_dpi;
    case Centimeters: return m_dpi / 2.54;
    default: return 1;
    }
}

void NewDocumentDialog::refresh() {
    m_updating = true;
    const double k = unitFactor();
    const bool px = m_units->currentIndex() == Pixels;
    const QString suffix = px ? QStringLiteral(" px") : m_units->currentIndex() == Inches ? QStringLiteral(" in") : QStringLiteral(" cm");
    for (QDoubleSpinBox *sb : {m_w, m_h}) {
        sb->setDecimals(px ? 0 : 2);
        sb->setSuffix(suffix);
        sb->setSingleStep(px ? 1 : 0.1);
        sb->setRange(px ? 1 : 0.01, 16000 / k);
    }
    m_w->setValue(m_pxW / k);
    m_h->setValue(m_pxH / k);
    const bool perCm = m_resUnits->currentIndex() == 1;
    m_res->setRange(perCm ? 0.4 : 1, perCm ? 100000 / 2.54 : 100000);
    m_res->setValue(perCm ? m_dpi / 2.54 : m_dpi);
    m_updating = false;
}

void NewDocumentDialog::sizeEdited() {
    if (m_updating) return;
    const double k = unitFactor();
    m_pxW = qBound(1, qRound(m_w->value() * k), 16000);
    m_pxH = qBound(1, qRound(m_h->value() * k), 16000);
    m_sizeEdited = true;   // the user's numbers win over a later clipboard read
    if (m_fromClipboard) { m_fromClipboard = false; m_note->hide(); }
}

void NewDocumentDialog::resolutionEdited() {
    if (m_updating) return;
    const double v = m_res->value();
    const double dpi = m_resUnits->currentIndex() == 1 ? v * 2.54 : v;
    if (m_units->currentIndex() != Pixels) {   // print size stays, pixels follow (as in Fireworks)
        const double wIn = m_pxW / m_dpi, hIn = m_pxH / m_dpi;
        m_pxW = qBound(1, qRound(wIn * dpi), 16000);
        m_pxH = qBound(1, qRound(hIn * dpi), 16000);
    }
    m_dpi = dpi;
    m_resEdited = true;
    refresh();
}

void NewDocumentDialog::setUnits(Units u) { m_units->setCurrentIndex(int(u)); }
void NewDocumentDialog::setResolutionPerCm(bool perCm) { m_resUnits->setCurrentIndex(perCm ? 1 : 0); }
QString NewDocumentDialog::clipboardNote() const { return m_note->isHidden() ? QString() : m_note->text(); }

void NewDocumentDialog::probeClipboard() {
    const int gen = ++m_gen;
    clipimage::Info info = clipimage::probeClipboard();
    if (!info.decodeLater.isEmpty() && !info.valid()) {
        // only a full decode tells the size: do it off the GUI thread so the dialog stays usable
        m_pending = true;
        if (!m_sizeEdited) { m_note->setText(QStringLiteral("Reading clipboard image\u2026")); m_note->show(); }
        auto *watch = new QFutureWatcher<clipimage::Info>(this);
        const QString source = info.source;
        connect(watch, &QFutureWatcher<clipimage::Info>::finished, this, [this, watch, gen, source] {
            watch->deleteLater();
            if (gen != m_gen) return;   // the clipboard changed meanwhile
            m_pending = false;
            clipimage::Info r = watch->result();
            r.source = source;
            if (!r.valid() && !m_fromClipboard) m_note->hide();
            applyClipboardInfo(r);
        });
        watch->setFuture(QtConcurrent::run([bytes = info.decodeLater] { return clipimage::decode(bytes); }));
        return;
    }
    m_pending = false;
    applyClipboardInfo(info);
}

void NewDocumentDialog::applyClipboardInfo(const clipimage::Info &info) {
    if (m_sizeEdited) return;
    if (!info.valid()) {
        if (m_fromClipboard) {   // the clipboard was emptied while open: back to the defaults
            m_fromClipboard = false;
            m_pxW = kDefaultW; m_pxH = kDefaultH;
            if (!m_resEdited) m_dpi = 72;
            refresh();
        }
        if (!m_pending) m_note->hide();
        return;
    }
    const QSize s = info.size;
    m_pxW = qBound(1, s.width(), 16000);
    m_pxH = qBound(1, s.height(), 16000);
    if (!m_resEdited) m_dpi = info.dpi > 0 ? info.dpi : 72;
    m_fromClipboard = true;
    refresh();
    const QString what = info.source.isEmpty() ? QStringLiteral("clipboard image") : info.source;
    QString note = QStringLiteral("Size from %1 (%2\u00D7%3)").arg(what).arg(s.width()).arg(s.height());
    if (info.dpi > 0 && !m_resEdited) note += QStringLiteral(", %1 ppi").arg(QLocale().toString(info.dpi, 'g', 6));
    if (s.width() > 16000 || s.height() > 16000) note += QStringLiteral(" \u2014 limited to 16000 px");
    m_note->setText(note);
    m_note->setToolTip(QStringLiteral("Width and height were filled in from what is on the clipboard. Nothing is pasted; Edit > Paste adds it."));
    m_note->show();
}

int NewDocumentDialog::w() const { return m_pxW; }
int NewDocumentDialog::h() const { return m_pxH; }
double NewDocumentDialog::resolution() const { return m_dpi; }
QColor NewDocumentDialog::canvasColor() const {
    switch (m_bg->currentIndex()) {
    case 0: return Qt::white;
    case 1: return Qt::transparent;
    default: return m_custom->color();
    }
}

// ---------------------------------------------------------------- SizeDialog
SizeDialog::SizeDialog(bool canvas, int w, int h, QWidget *parent) : QDialog(parent), m_canvasMode(canvas), m_ow(w), m_oh(h) {
    setWindowTitle(canvas ? QStringLiteral("Canvas Size") : QStringLiteral("Image Size"));
    auto *f = new QFormLayout(this);
    f->addRow(new QLabel(QStringLiteral("Current size: %1 x %2 pixels").arg(w).arg(h)));
    m_w = new PopupSliderField; m_w->setRange(1, 16000); m_w->setValue(w); m_w->setSuffix(QStringLiteral(" px"));
    m_h = new PopupSliderField; m_h->setRange(1, 16000); m_h->setValue(h); m_h->setSuffix(QStringLiteral(" px"));
    f->addRow(QStringLiteral("Width:"), m_w);
    f->addRow(QStringLiteral("Height:"), m_h);
    if (!canvas) {
        m_constrain = new QCheckBox(QStringLiteral("Constrain proportions"));
        m_constrain->setChecked(true);
        f->addRow(m_constrain);
        m_resample = new QComboBox; m_resample->setObjectName(QStringLiteral("imageSizeResample"));
        m_resample->addItems(pixelscale::resampleNames());
        m_resample->setCurrentIndex(qBound(0, coreprefs::interpolation, m_resample->count() - 1));
        m_resample->setToolTip(QStringLiteral("Pixel-art scalers enlarge by whole factors (2x, 3x...) and resample down for in-between sizes"));
        f->addRow(QStringLiteral("Resample:"), m_resample);
        connect(m_w, &QSpinBox::valueChanged, this, [this](int v) {
            if (m_constrain->isChecked()) { QSignalBlocker b(m_h); m_h->setValue(qMax(1, qRound(v * double(m_oh) / m_ow))); }
        });
        connect(m_h, &QSpinBox::valueChanged, this, [this](int v) {
            if (m_constrain->isChecked()) { QSignalBlocker b(m_w); m_w->setValue(qMax(1, qRound(v * double(m_ow) / m_oh))); }
        });
    } else {
        auto *grid = new QGridLayout;
        auto *group = new QButtonGroup(this);
        for (int i = 0; i < 9; ++i) {
            auto *b = new QToolButton;
            b->setCheckable(true);
            b->setFixedSize(22, 22);
            b->setChecked(i == 4);
            group->addButton(b, i);
            grid->addWidget(b, i / 3, i % 3);
        }
        connect(group, &QButtonGroup::idClicked, this, [this](int id) { m_anchor = id; });
        auto *wrap = new QWidget;
        wrap->setLayout(grid);
        f->addRow(QStringLiteral("Anchor:"), wrap);
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(bb);
}
int SizeDialog::w() const { return m_w->value(); }
int SizeDialog::h() const { return m_h->value(); }
int SizeDialog::resample() const { return m_resample ? m_resample->currentIndex() : -1; }
QPoint SizeDialog::offsetFor(int oldW, int oldH) const {
    int ax = m_anchor % 3, ay = m_anchor / 3;
    return QPoint((w() - oldW) * ax / 2, (h() - oldH) * ay / 2);
}

// ---------------------------------------------------------------- PaletteEditor
class PaletteSwatches : public QWidget {
public:
    explicit PaletteSwatches(PaletteEditor *ed) : QWidget(ed), m_ed(ed) {
        setObjectName(QStringLiteral("paletteSwatches"));
        setMinimumSize(16 * kCell + 2, 2 * kCell + 2);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    }
    static constexpr int kCell = 14;
    int perRow() const { return qMax(1, (width() - 2) / kCell); }
    int indexAt(const QPoint &p) const {
        const int i = (p.y() - 1) / kCell * perRow() + (p.x() - 1) / kCell;
        return (p.x() >= 1 && p.y() >= 1 && (p.x() - 1) / kCell < perRow() && i < m_ed->colors().size()) ? i : -1;
    }
    QSize sizeHint() const override {
        const int rows = qMax(2, int((m_ed->colors().size() + 15) / 16));
        return {16 * kCell + 2, qMin(rows, 16) * kCell + 2};
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), palette().base());
        const QVector<QRgb> c = m_ed->colors();
        const int n = perRow();
        for (int i = 0; i < c.size(); ++i) {
            const QRect r(1 + (i % n) * kCell, 1 + (i / n) * kCell, kCell - 1, kCell - 1);
            p.fillRect(r, QColor::fromRgb(c[i]));
            if (i == m_ed->current()) {
                p.setPen(QPen(Qt::white, 1)); p.drawRect(r.adjusted(0, 0, -1, -1));
                p.setPen(QPen(Qt::black, 1)); p.drawRect(r.adjusted(-1, -1, 0, 0));
            }
        }
    }
    void mousePressEvent(QMouseEvent *e) override { m_ed->setCurrent(indexAt(e->position().toPoint())); }
    void mouseDoubleClickEvent(QMouseEvent *e) override {
        const int i = indexAt(e->position().toPoint());
        if (i < 0) return;
        QVector<QRgb> c = m_ed->colors();
        const QColor nc = colorpick::getColor(QColor::fromRgb(c[i]), this, QStringLiteral("Edit Palette Colour"), false);
        if (!nc.isValid()) return;
        c[i] = nc.rgb() | 0xFF000000u;
        m_ed->setColors(c);
        emit m_ed->changed();
    }
private:
    PaletteEditor *m_ed;
};

PaletteEditor::PaletteEditor(QWidget *parent, const QImage &sample) : QWidget(parent), m_sample(sample) {
    setObjectName(QStringLiteral("paletteEditor"));
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(3);
    m_sw = new PaletteSwatches(this);
    v->addWidget(m_sw);
    auto *row = new QHBoxLayout;
    row->setSpacing(3);
    auto btn = [&](const QString &name, const QString &text, const QString &tip) {
        auto *b = new QToolButton;
        b->setObjectName(QStringLiteral("pal_") + name);
        b->setText(text);
        b->setToolTip(tip);
        row->addWidget(b);
        return b;
    };
    auto *add = btn(QStringLiteral("add"), QStringLiteral("+"), QStringLiteral("Add a colour"));
    auto *del = btn(QStringLiteral("remove"), QStringLiteral("\u2212"), QStringLiteral("Remove the selected colour"));
    auto *fromImg = btn(QStringLiteral("fromImage"), QStringLiteral("From Image"), QStringLiteral("The image's colours (up to 256, most used first via median cut)"));
    auto *load = btn(QStringLiteral("load"), QStringLiteral("Load..."), QStringLiteral("Load a palette (.gpl, .pal, .act, .png, .gif)"));
    auto *save = btn(QStringLiteral("save"), QStringLiteral("Save..."), QStringLiteral("Save the palette (.gpl, .pal, .act)"));
    m_count = new QLabel;
    m_count->setObjectName(QStringLiteral("pal_count"));
    row->addWidget(m_count);
    row->addStretch();
    v->addLayout(row);
    fromImg->setEnabled(!m_sample.isNull());
    connect(add, &QToolButton::clicked, this, [this] {
        if (m_colors.size() >= 256) return;
        const QColor c = colorpick::getColor(m_cur >= 0 ? QColor::fromRgb(m_colors[m_cur]) : QColor(Qt::black), this, QStringLiteral("Add Palette Colour"), false);
        if (!c.isValid()) return;
        m_colors.insert(m_cur >= 0 ? m_cur + 1 : m_colors.size(), c.rgb() | 0xFF000000u);
        m_cur = m_cur >= 0 ? m_cur + 1 : m_colors.size() - 1;
        sync(); emit changed();
    });
    connect(del, &QToolButton::clicked, this, [this] {
        if (m_cur < 0 || m_cur >= m_colors.size()) return;
        m_colors.remove(m_cur);
        m_cur = qMin(m_cur, int(m_colors.size()) - 1);
        sync(); emit changed();
    });
    connect(fromImg, &QToolButton::clicked, this, [this] {
        QVector<QRgb> c = fx2::uniqueColors(m_sample, 257);
        if (c.size() > 256) c = quant::buildPalette({m_sample}, 256, quant::Adaptive);
        setColors(c);
        emit changed();
    });
    connect(load, &QToolButton::clicked, this, [this] {
        const QString path = filedialogs::getOpenFileName(this, QStringLiteral("Load Palette"), {},
                                                          QStringLiteral("Palettes (*.gpl *.pal *.act *.png *.gif);;All files (*)"));
        if (path.isEmpty()) return;
        QString err;
        const QVector<QRgb> c = quant::loadPalette(path, &err);
        if (c.isEmpty()) { QMessageBox::warning(this, QStringLiteral("Load Palette"), err.isEmpty() ? QStringLiteral("No colours in the file.") : err); return; }
        setColors(c);
        emit changed();
    });
    connect(save, &QToolButton::clicked, this, [this] {
        QString path = filedialogs::getSaveFileName(this, QStringLiteral("Save Palette"), QStringLiteral("palette.gpl"),
                                                    QStringLiteral("GIMP palette (*.gpl);;JASC palette (*.pal);;Adobe Colour Table (*.act)"));
        if (path.isEmpty()) return;
        QString err;
        if (!quant::savePalette(path, m_colors, &err)) QMessageBox::warning(this, QStringLiteral("Save Palette"), err);
    });
    sync();
}
void PaletteEditor::setColors(const QVector<QRgb> &c) {
    m_colors = c.mid(0, 256);
    for (QRgb &x : m_colors) x |= 0xFF000000u;
    if (m_cur >= m_colors.size()) m_cur = int(m_colors.size()) - 1;
    sync();
}
void PaletteEditor::setCurrent(int i) { m_cur = i; m_sw->update(); }
void PaletteEditor::sync() {
    m_count->setText(QStringLiteral("%1 colour(s)").arg(m_colors.size()));
    m_sw->updateGeometry();
    m_sw->update();
}

// ---------------------------------------------------------------- CurveEditor
CurveEditor::CurveEditor(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("curveEditor"));
    setMinimumSize(160, 160);
    m_pts = QPolygonF({QPointF(0, 0), QPointF(255, 255)});
}
void CurveEditor::setCurve(const QPolygonF &c) { m_pts = c.size() >= 2 ? c : QPolygonF({QPointF(0, 0), QPointF(255, 255)}); update(); }
QRectF CurveEditor::area() const { return QRectF(rect()).adjusted(6, 6, -6, -6); }
QPointF CurveEditor::toWidget(const QPointF &c) const {
    QRectF a = area();
    return {a.left() + c.x() / 255.0 * a.width(), a.bottom() - c.y() / 255.0 * a.height()};
}
QPointF CurveEditor::toCurve(const QPointF &w) const {
    QRectF a = area();
    return {qBound(0.0, (w.x() - a.left()) / a.width() * 255.0, 255.0), qBound(0.0, (a.bottom() - w.y()) / a.height() * 255.0, 255.0)};
}
void CurveEditor::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF a = area();
    p.fillRect(a, Qt::white);
    p.setPen(QColor(0xDD, 0xDD, 0xDD));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(a.left() + a.width() * i / 4, a.top()), QPointF(a.left() + a.width() * i / 4, a.bottom()));
        p.drawLine(QPointF(a.left(), a.top() + a.height() * i / 4), QPointF(a.right(), a.top() + a.height() * i / 4));
    }
    if (m_hist.size() == 256) {   // histogram behind the curve (v0.3.0)
        int mx = 1;
        for (int v : m_hist) mx = qMax(mx, v);
        QColor hc = m_histColor.isValid() ? m_histColor : QColor(Qt::gray);
        hc.setAlpha(90);
        QPainterPath hp;
        hp.moveTo(toWidget(QPointF(0, 0)));
        for (int x = 0; x < 256; ++x) {
            const double h = std::sqrt(double(m_hist[x]) / mx) * 255.0;   // sqrt: small counts stay visible
            hp.lineTo(toWidget(QPointF(x, h)));
        }
        hp.lineTo(toWidget(QPointF(255, 0)));
        hp.closeSubpath();
        p.fillPath(hp, hc);
    }
    p.setPen(Qt::gray);
    p.drawRect(a);
    const QVector<uchar> lut = fx::curveLut(m_pts);
    QPolygonF line;
    for (int x = 0; x < 256; ++x) line << toWidget(QPointF(x, lut[x]));
    p.setPen(QPen(Qt::black, 1.5));
    p.drawPolyline(line);
    p.setBrush(Qt::white);
    for (const QPointF &c : m_pts) p.drawRect(QRectF(toWidget(c) - QPointF(3, 3), QSizeF(6, 6)));
}
void CurveEditor::mousePressEvent(QMouseEvent *e) {
    const QPointF pos = e->position();
    m_drag = -1;
    for (int i = 0; i < m_pts.size(); ++i)
        if (QLineF(toWidget(m_pts[i]), pos).length() <= 6) { m_drag = i; break; }
    if (e->button() == Qt::RightButton) {
        if (m_drag > 0 && m_drag < m_pts.size() - 1) { m_pts.remove(m_drag); emit curveChanged(m_pts); update(); }
        m_drag = -1;
        return;
    }
    if (m_drag < 0) {
        QPointF c = toCurve(pos);
        int i = 0;
        while (i < m_pts.size() && m_pts[i].x() < c.x()) ++i;
        if (i == 0 || i == m_pts.size()) return; // end points already exist
        m_pts.insert(i, c);
        m_drag = i;
        emit curveChanged(m_pts);
        update();
    }
}
void CurveEditor::mouseMoveEvent(QMouseEvent *e) {
    if (m_drag < 0) return;
    QPointF c = toCurve(e->position());
    const bool end = m_drag == 0 || m_drag == m_pts.size() - 1;
    if (!end) {
        double lo = m_pts[m_drag - 1].x() + 1, hi = m_pts[m_drag + 1].x() - 1;
        c.setX(qBound(lo, c.x(), hi));
    } else {
        c.setX(m_pts[m_drag].x()); // end points move vertically only
    }
    m_pts[m_drag] = c;
    emit curveChanged(m_pts);
    update();
}
void CurveEditor::mouseReleaseEvent(QMouseEvent *e) {
    // dragging an interior point far outside the box deletes it
    if (m_drag > 0 && m_drag < m_pts.size() - 1 && !rect().adjusted(-20, -20, 20, 20).contains(e->position().toPoint())) {
        m_pts.remove(m_drag);
        emit curveChanged(m_pts);
        update();
    }
    m_drag = -1;
}

// ---------------------------------------------------------------- HistogramView / SamplePicker
HistogramView::HistogramView(QWidget *parent) : QWidget(parent) { setObjectName(QStringLiteral("fx_histogram")); setMinimumSize(200, 80); }
void HistogramView::setData(const QVector<int> &hist, const LevelsChannel &lc) { m_hist = hist; m_lc = lc; update(); }
void HistogramView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    QRectF a = QRectF(rect()).adjusted(4, 2, -4, -10);
    p.fillRect(a, Qt::white);
    p.setPen(Qt::gray);
    p.drawRect(a);
    int mx = 1;
    for (int v : m_hist) mx = qMax(mx, v);
    p.setPen(QColor(0x33, 0x33, 0x33));
    for (int i = 0; i < m_hist.size(); ++i) {
        double x = a.left() + (i + 0.5) * a.width() / 256.0;
        double hgt = std::sqrt(double(m_hist[i]) / mx) * (a.height() - 2); // sqrt scale keeps small bins visible
        p.drawLine(QPointF(x, a.bottom()), QPointF(x, a.bottom() - hgt));
    }
    auto marker = [&](double v, QColor fill) {
        double x = a.left() + v / 255.0 * a.width();
        QPolygonF tri({QPointF(x, a.bottom() + 1), QPointF(x - 4, a.bottom() + 9), QPointF(x + 4, a.bottom() + 9)});
        p.setPen(Qt::black);
        p.setBrush(fill);
        p.drawPolygon(tri);
    };
    const double mid = m_lc.inBlack + (m_lc.inWhite - m_lc.inBlack) * std::pow(0.5, m_lc.gamma);
    marker(m_lc.inBlack, Qt::black);
    marker(mid, Qt::gray);
    marker(m_lc.inWhite, Qt::white);
}

SamplePicker::SamplePicker(const QImage &img, QWidget *parent) : QWidget(parent), m_img(img.convertToFormat(QImage::Format_ARGB32)) {
    setObjectName(QStringLiteral("fx_sample"));
    setCursor(Qt::CrossCursor);
    setToolTip(QStringLiteral("Choose an eyedropper, then click the image to set the black, grey or white point"));
}
QSize SamplePicker::sizeHint() const {
    QSize sz = m_img.size();
    if (sz.isEmpty()) return {120, 90};
    return sz.scaled(160, 120, Qt::KeepAspectRatio);
}
QRect SamplePicker::target() const {
    if (m_img.isNull()) return {};
    QSize sz = m_img.size().scaled(size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - sz.width()) / 2, (height() - sz.height()) / 2), sz);
}
void SamplePicker::paintEvent(QPaintEvent *) {
    QPainter p(this);
    QRect t = target();
    for (int y = t.top(); y < t.bottom(); y += 8)
        for (int x = t.left(); x < t.right(); x += 8)
            p.fillRect(QRect(x, y, 8, 8).intersected(t), ((x - t.left()) / 8 + (y - t.top()) / 8) % 2 ? QColor(0xCC, 0xCC, 0xCC) : Qt::white);
    p.drawImage(t, m_img);
}
void SamplePicker::mousePressEvent(QMouseEvent *e) {
    QRect t = target();
    if (!t.contains(e->position().toPoint()) || t.isEmpty()) return;
    int x = qBound(0, int((e->position().x() - t.left()) * m_img.width() / t.width()), m_img.width() - 1);
    int y = qBound(0, int((e->position().y() - t.top()) * m_img.height() / t.height()), m_img.height() - 1);
    emit picked(m_img.pixelColor(x, y));
}

// ---------------------------------------------------------------- EffectDialog
EffectDialog::EffectDialog(const LiveEffect &e, QWidget *parent, const QImage &sample)
    : QDialog(parent), m_e(e), m_orig(e), m_sample(sample) {
    setObjectName(QStringLiteral("effectDialog"));
    setWindowTitle(effectName(e.type));
    auto *f = new QFormLayout(this);
    auto dspin = [&](const QString &name, const QString &label, double v, double mn, double mx, const QString &suffix,
                     std::function<void(double)> set, int decimals = 0) {
        auto *s = new PopupSliderDoubleField;
        s->setObjectName(QStringLiteral("fx_") + name);
        s->setRange(mn, mx);
        s->setDecimals(decimals);
        s->setSingleStep(decimals ? 0.1 : 1);
        s->setValue(v);
        s->setSuffix(suffix);
        s->setKeyboardTracking(false);
        f->addRow(label, s);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, set](double x) { if (m_syncing) return; set(x); emitChange(); });
        return s;
    };
    auto check = [&](const QString &name, const QString &label, bool v, std::function<void(bool)> set) {
        auto *c = new QCheckBox(label);
        c->setObjectName(QStringLiteral("fx_") + name);
        c->setChecked(v);
        f->addRow(QString(), c);
        connect(c, &QCheckBox::toggled, this, [this, set](bool x) { set(x); emitChange(); });
    };
    auto color = [&](const QColor &c0, const QString &label = QStringLiteral("Colour:")) {
        auto *cb = new ColorButton(this, false);
        cb->setObjectName(QStringLiteral("fx_color"));
        cb->setColor(c0);
        f->addRow(label, cb);
        connect(cb, &ColorButton::colorPicked, this, [this](const QColor &c) { m_e.color = c; emitChange(); });
        connect(cb, &ColorButton::colorPreview, this, [this](const QColor &c) { m_e.color = c; emitChange(); });   // live while hovering / picking
    };
    auto combo = [&](const QString &name, const QString &label, const QStringList &items, int cur, std::function<void(int)> set) {
        auto *c = new QComboBox;
        c->setObjectName(QStringLiteral("fx_") + name);
        c->addItems(items);
        c->setCurrentIndex(cur);
        f->addRow(label, c);
        connect(c, &QComboBox::currentIndexChanged, this, set);
        return c;
    };
    auto opacity = [&](const QString &label = QStringLiteral("Opacity:")) {
        dspin(QStringLiteral("opacity"), label, m_e.opacity, 0, 100, QStringLiteral(" %"), [this](double v) { m_e.opacity = int(v); });
    };
    auto softness = [&](const QString &label) { dspin(QStringLiteral("softness"), label, m_e.softness, 0, 100, QStringLiteral(" px"), [this](double v) { m_e.softness = v; }); };
    auto angle = [&](const QString &label) { dspin(QStringLiteral("angle"), label, m_e.angle, 0, 359, QStringLiteral("°"), [this](double v) { m_e.angle = v; }); };
    auto width = [&] { dspin(QStringLiteral("width"), QStringLiteral("Width:"), m_e.width, 0, 100, QStringLiteral(" px"), [this](double v) { m_e.width = v; }); };
    auto channelCombo = [&] {
        combo(QStringLiteral("channel"), QStringLiteral("Channel:"), channelNames(), 0, [this](int i) { m_channel = i; syncChannel(); });
    };

    switch (e.type) {
    case EffectType::DropShadow: case EffectType::InnerShadow:
        dspin(QStringLiteral("distance"), QStringLiteral("Distance:"), e.distance, 0, 200, QStringLiteral(" px"), [this](double v) { m_e.distance = v; });
        angle(QStringLiteral("Angle:"));
        softness(QStringLiteral("Softness:"));
        opacity();
        color(e.color);
        break;
    case EffectType::OuterGlow: case EffectType::InnerGlow:
        width(); softness(QStringLiteral("Softness:")); opacity(); color(e.color);
        break;
    case EffectType::InnerBevel: case EffectType::OuterBevel:
        combo(QStringLiteral("preset"), QStringLiteral("Button preset:"), bevelPresetNames(), int(e.preset),
              [this](int i) { m_e.preset = BevelPreset(i); emitChange(); });
        width(); softness(QStringLiteral("Softness:")); angle(QStringLiteral("Lighting angle:"));
        opacity(QStringLiteral("Contrast:"));
        color(e.color, e.type == EffectType::OuterBevel ? QStringLiteral("Colour:") : QStringLiteral("Highlight:"));
        break;
    case EffectType::InsetEmboss: case EffectType::RaisedEmboss:
        width(); opacity(QStringLiteral("Contrast:"));
        softness(QStringLiteral("Softness:")); angle(QStringLiteral("Angle:"));
        check(QStringLiteral("showObject"), QStringLiteral("Show Object"), e.showObject, [this](bool v) { m_e.showObject = v; });
        break;
    case EffectType::GaussianBlur: softness(QStringLiteral("Radius:")); break;
    case EffectType::MotionBlur:
        angle(QStringLiteral("Angle:"));
        dspin(QStringLiteral("distance"), QStringLiteral("Distance:"), e.distance, 1, 200, QStringLiteral(" px"), [this](double v) { m_e.distance = v; });
        break;
    case EffectType::RadialBlur: case EffectType::ZoomBlur:
        dspin(QStringLiteral("amount"), QStringLiteral("Amount:"), e.amount, 1, 100, {}, [this](double v) { m_e.amount = v; });
        dspin(QStringLiteral("quality"), QStringLiteral("Quality:"), e.quality, 2, 100, {}, [this](double v) { m_e.quality = int(v); });
        break;
    case EffectType::UnsharpMask:
        dspin(QStringLiteral("amount"), QStringLiteral("Sharpen amount:"), e.amount, 1, 500, QStringLiteral(" %"), [this](double v) { m_e.amount = v; });
        dspin(QStringLiteral("softness"), QStringLiteral("Pixel radius:"), e.softness, 0.1, 250, QStringLiteral(" px"), [this](double v) { m_e.softness = v; }, 1);
        dspin(QStringLiteral("threshold"), QStringLiteral("Threshold:"), e.threshold, 0, 255, {}, [this](double v) { m_e.threshold = int(v); });
        break;
    case EffectType::HueSaturation:
        dspin(QStringLiteral("hue"), QStringLiteral("Hue:"), e.hue, -180, 360, {}, [this](double v) { m_e.hue = v; });
        dspin(QStringLiteral("saturation"), QStringLiteral("Saturation:"), e.saturation, -100, 100, {}, [this](double v) { m_e.saturation = v; });
        dspin(QStringLiteral("lightness"), QStringLiteral("Lightness:"), e.lightness, -100, 100, {}, [this](double v) { m_e.lightness = v; });
        check(QStringLiteral("colorize"), QStringLiteral("Colorize"), e.colorize, [this](bool v) { m_e.colorize = v; });
        break;
    case EffectType::Levels: {
        channelCombo();
        m_hist = new HistogramView;
        f->addRow(m_hist);
        m_levelSpins << dspin(QStringLiteral("inBlack"), QStringLiteral("Input black:"), 0, 0, 254, {}, [this](double v) { m_e.levels[m_channel].inBlack = int(v); syncChannel(); })
                     << dspin(QStringLiteral("gamma"), QStringLiteral("Gamma:"), 1, 0.1, 9.99, {}, [this](double v) { m_e.levels[m_channel].gamma = v; syncChannel(); }, 2)
                     << dspin(QStringLiteral("inWhite"), QStringLiteral("Input white:"), 255, 1, 255, {}, [this](double v) { m_e.levels[m_channel].inWhite = int(v); syncChannel(); })
                     << dspin(QStringLiteral("outBlack"), QStringLiteral("Output black:"), 0, 0, 255, {}, [this](double v) { m_e.levels[m_channel].outBlack = int(v); })
                     << dspin(QStringLiteral("outWhite"), QStringLiteral("Output white:"), 255, 0, 255, {}, [this](double v) { m_e.levels[m_channel].outWhite = int(v); });
        auto *dr = new QHBoxLayout;
        const char *names[] = {"pickBlack", "pickGrey", "pickWhite"};
        const char *tips[] = {"Set Black Point", "Set Grey Point", "Set White Point"};
        const QColor fills[] = {Qt::black, Qt::gray, Qt::white};
        auto *grp = new QButtonGroup(this);
        for (int i = 0; i < 3; ++i) {
            auto *b = new QToolButton;
            b->setObjectName(QStringLiteral("fx_") + QLatin1String(names[i]));
            b->setToolTip(QLatin1String(tips[i]));
            b->setCheckable(true);
            QPixmap pm(16, 16);
            pm.fill(Qt::transparent);
            { QPainter pp(&pm); pp.setPen(Qt::black); pp.setBrush(fills[i]); pp.drawEllipse(2, 2, 11, 11); }
            b->setIcon(pm);
            grp->addButton(b, i + 1);
            dr->addWidget(b);
        }
        auto *autoBtn = new QPushButton(QStringLiteral("Auto"));
        autoBtn->setObjectName(QStringLiteral("fx_auto"));
        dr->addWidget(autoBtn);
        dr->addStretch();
        f->addRow(QStringLiteral("Eyedroppers:"), dr);
        connect(grp, &QButtonGroup::idClicked, this, [this](int id) { m_dropper = Dropper(id); });
        connect(autoBtn, &QPushButton::clicked, this, [this] {
            if (m_sample.isNull()) return;
            m_e.levels[ChRGB] = LevelsChannel();
            for (int c = 1; c < kChannels; ++c) m_e.levels[c] = LevelsChannel();
            for (int c = ChRed; c <= ChBlue; ++c) m_e.levels[c] = fx::autoLevelsChannel(m_sample, c);   // alpha untouched
            syncChannel();
            emitChange();
        });
        // Auto Contrast: one black / white point for all channels (the darkest / brightest of the per-channel
        // clipped ranges), so the colour balance is kept.
        auto *autoCBtn = new QPushButton(QStringLiteral("Auto Contrast"));
        autoCBtn->setObjectName(QStringLiteral("fx_autoContrast"));
        dr->insertWidget(dr->count() - 1, autoCBtn);
        autoCBtn->setEnabled(!m_sample.isNull());
        autoBtn->setEnabled(!m_sample.isNull());
        connect(autoCBtn, &QPushButton::clicked, this, [this] {
            if (m_sample.isNull()) return;
            LevelsChannel m;
            int lo = 255, hi = 0;
            for (int c = ChRed; c <= ChBlue; ++c) {
                const LevelsChannel lc = fx::autoLevelsChannel(m_sample, c);
                lo = qMin(lo, int(lc.inBlack)); hi = qMax(hi, int(lc.inWhite));
            }
            if (hi <= lo) { lo = 0; hi = 255; }
            m.inBlack = lo; m.inWhite = hi;
            for (int c = 0; c < kChannels; ++c) m_e.levels[c] = LevelsChannel();
            m_e.levels[ChRGB] = m;
            syncChannel();
            emitChange();
        });
        if (!m_sample.isNull()) {
            auto *sp = new SamplePicker(m_sample);
            f->addRow(QStringLiteral("Sample:"), sp);
            connect(sp, &SamplePicker::picked, this, [this](const QColor &c) { if (m_dropper != NoDropper) applyDropper(m_dropper, c); });
        }
        syncChannel();
        break;
    }
    case EffectType::Curves: {
        channelCombo();
        m_curve = new CurveEditor;
        f->addRow(m_curve);
        auto *reset = new QPushButton(QStringLiteral("Reset"));
        reset->setObjectName(QStringLiteral("fx_reset"));
        f->addRow(QString(), reset);
        connect(m_curve, &CurveEditor::curveChanged, this, [this](const QPolygonF &c) { if (m_syncing) return; m_e.curves[m_channel] = c; emitChange(); });
        connect(reset, &QPushButton::clicked, this, [this] { m_e.curves[m_channel].clear(); syncChannel(); emitChange(); });
        syncChannel();
        break;
    }
    case EffectType::ColorFill:
        combo(QStringLiteral("blend"), QStringLiteral("Blend mode:"), blendModeNames(), int(e.blend), [this](int i) { m_e.blend = BlendMode(i); emitChange(); });
        color(e.color); opacity();
        break;
    case EffectType::BrightnessContrast:
        dspin(QStringLiteral("brightness"), QStringLiteral("Brightness:"), e.brightness, -100, 100, {}, [this](double v) { m_e.brightness = v; });
        dspin(QStringLiteral("contrast"), QStringLiteral("Contrast:"), e.contrast, -100, 100, {}, [this](double v) { m_e.contrast = v; });
        break;
    case EffectType::Noise:
        dspin(QStringLiteral("amount"), QStringLiteral("Amount:"), e.amount, 0, 400, {}, [this](double v) { m_e.amount = v; });
        check(QStringLiteral("colorNoise"), QStringLiteral("Color"), e.colorNoise, [this](bool v) { m_e.colorNoise = v; });
        break;
    case EffectType::Threshold:
        if (!sample.isNull()) {
            auto *hv = new HistogramView;
            LevelsChannel lc;
            hv->setData(fx::histogram(sample, ChRGB), lc);
            f->addRow(hv);
        }
        dspin(QStringLiteral("threshold"), QStringLiteral("Threshold level:"), e.threshold, 1, 255, {}, [this](double v) { m_e.threshold = int(v); });
        break;
    case EffectType::HighPass:
        dspin(QStringLiteral("softness"), QStringLiteral("Radius:"), e.softness, 0.1, 250, QStringLiteral(" px"), [this](double v) { m_e.softness = v; }, 1);
        check(QStringLiteral("keepColor"), QStringLiteral("Keep colour (off: greyscale)"), e.keepColor, [this](bool v) { m_e.keepColor = v; });
        break;
    case EffectType::Offset: {
        const int sw = qMax(1, m_sample.isNull() ? 500 : m_sample.width()), sh = qMax(1, m_sample.isNull() ? 500 : m_sample.height());
        auto *ox = dspin(QStringLiteral("offsetX"), QStringLiteral("Horizontal:"), 0, -double(sw), double(sw),
                         QStringLiteral(" px right"), [this](double v) { m_e.offsetX = v; });
        auto *oy = dspin(QStringLiteral("offsetY"), QStringLiteral("Vertical:"), 0, -double(sh), double(sh),
                         QStringLiteral(" px down"), [this](double v) { m_e.offsetY = v; });
        ox->setSliderRange(-sw, sw);
        oy->setSliderRange(-sh, sh);
        ox->setValue(e.offsetX);
        oy->setValue(e.offsetY);
        auto *halfBtn = new QPushButton(QStringLiteral("Half (w/2, h/2)"));
        halfBtn->setObjectName(QStringLiteral("fx_half"));
        halfBtn->setToolTip(QStringLiteral("Offset by half the width and height: the edges meet in the middle, to check a seamless texture"));
        f->addRow(QString(), halfBtn);
        connect(halfBtn, &QPushButton::clicked, this, [ox, oy, sw, sh] { ox->setValue(sw / 2); oy->setValue(sh / 2); });
        combo(QStringLiteral("undefined"), QStringLiteral("Undefined areas:"),
              {QStringLiteral("Wrap Around"), QStringLiteral("Repeat Edge Pixels"), QStringLiteral("Set to Transparent")}, e.undefinedAreas,
              [this](int i) { m_e.undefinedAreas = i; emitChange(); });
        break;
    }
    case EffectType::FeatherEdges:
        dspin(QStringLiteral("width"), QStringLiteral("Feather radius:"), e.width, 0, 250, QStringLiteral(" px"), [this](double v) { m_e.width = v; }, 1);
        combo(QStringLiteral("placement"), QStringLiteral("Placement:"), {QStringLiteral("Inside the edge"), QStringLiteral("Centred on the edge")},
              e.featherCentered ? 1 : 0, [this](int i) { m_e.featherCentered = i == 1; emitChange(); });
        dspin(QStringLiteral("amount"), QStringLiteral("Falloff (hard .. soft):"), e.amount, 0, 100, QStringLiteral(" %"), [this](double v) { m_e.amount = v; });
        break;
    case EffectType::Posterize:
        dspin(QStringLiteral("posterize"), QStringLiteral("Levels:"), e.posterize, 2, 255, {}, [this](double v) { m_e.posterize = int(v); });
        break;
    case EffectType::ColorBalance: {
        combo(QStringLiteral("range"), QStringLiteral("Tone balance:"), {QStringLiteral("Shadows"), QStringLiteral("Midtones"), QStringLiteral("Highlights")}, 1,
              [this](int i) { m_range = i; syncChannel(); });
        const char *labels[] = {"Cyan / Red:", "Magenta / Green:", "Yellow / Blue:"};
        const char *names[] = {"cyanRed", "magentaGreen", "yellowBlue"};
        for (int c = 0; c < 3; ++c)
            m_balanceSpins << dspin(QLatin1String(names[c]), QLatin1String(labels[c]), e.balance[1][c], -100, 100, {},
                                    [this, c](double v) { m_e.balance[m_range][c] = v; });
        check(QStringLiteral("preserve"), QStringLiteral("Preserve Luminosity"), e.preserveLuminosity, [this](bool v) { m_e.preserveLuminosity = v; });
        break;
    }
    case EffectType::PhotoFilter: {
        // presets after the classic photographic filters
        static const struct { const char *name; QRgb c; } kPf[] = {
            {"Warming Filter (85)", 0xEC8A00}, {"Warming Filter (81)", 0xEBB113}, {"Cooling Filter (80)", 0x006DFF},
            {"Cooling Filter (82)", 0x00B5FF}, {"Red", 0xEA1A1A}, {"Orange", 0xF28D1A}, {"Yellow", 0xF9E31C},
            {"Green", 0x19C919}, {"Blue", 0x1D35EA}, {"Violet", 0x9B1DEA}, {"Sepia", 0xAC7A33}};
        QStringList names;
        int cur = -1;
        for (int i = 0; i < int(std::size(kPf)); ++i) { names << QLatin1String(kPf[i].name); if (QColor(kPf[i].c).rgb() == e.color.rgb()) cur = i; }
        names << QStringLiteral("Custom");
        auto *cb = new ColorButton(this, false);
        cb->setObjectName(QStringLiteral("fx_color"));
        cb->setColor(e.color);
        combo(QStringLiteral("filter"), QStringLiteral("Filter:"), names, cur < 0 ? names.size() - 1 : cur, [this, cb](int i) {
            if (i >= int(std::size(kPf))) return;
            m_e.color = QColor(kPf[i].c);
            cb->setColor(m_e.color);
            emitChange();
        });
        f->addRow(QStringLiteral("Colour:"), cb);
        connect(cb, &ColorButton::colorPicked, this, [this](const QColor &c) { m_e.color = c; emitChange(); });
        connect(cb, &ColorButton::colorPreview, this, [this](const QColor &c) { m_e.color = c; emitChange(); });   // live while hovering / picking
        dspin(QStringLiteral("amount"), QStringLiteral("Density:"), e.amount, 1, 100, QStringLiteral(" %"), [this](double v) { m_e.amount = v; });
        check(QStringLiteral("preserve"), QStringLiteral("Preserve Luminosity"), e.preserveLuminosity, [this](bool v) { m_e.preserveLuminosity = v; });
        break;
    }
    // ---- v0.3.0 effects (parameter mapping: LiveEffect::make / core/FxExtra.h) ----
    case EffectType::GradientMap: {
        auto *bar = new GradientBar;
        bar->setObjectName(QStringLiteral("fx_stops"));
        bar->setStops(e.stops);
        f->addRow(QStringLiteral("Gradient:"), bar);
        connect(bar, &GradientBar::changed, this, [this, bar] { m_e.stops = bar->stops(); emitChange(); });
        auto *presets = new QComboBox;
        presets->setObjectName(QStringLiteral("fx_gradPreset"));
        presets->addItems({QStringLiteral("Black, White"), QStringLiteral("Sepia"), QStringLiteral("Infrared"),
                           QStringLiteral("Duotone Blue"), QStringLiteral("Gold"), QStringLiteral("Game Boy")});
        presets->setCurrentIndex(-1);
        presets->setPlaceholderText(QStringLiteral("Presets..."));
        f->addRow(QStringLiteral("Preset:"), presets);
        connect(presets, &QComboBox::activated, this, [this, bar](int i) {
            static const QVector<QVector<QRgb>> P = {
                {0xFF000000, 0xFFFFFFFF},
                {0xFF1E0F05, 0xFF8A5A2B, 0xFFF5E6C8},
                {0xFF000000, 0xFF5000A0, 0xFFFF2000, 0xFFFFE000, 0xFFFFFFFF},
                {0xFF0A1440, 0xFF7FC8FF},
                {0xFF201000, 0xFFB07818, 0xFFFFF0B0},
                {0xFF0F380F, 0xFF306230, 0xFF8BAC0F, 0xFF9BBC0F}};
            QVector<GradStop> st;
            const auto &c = P.value(i);
            for (int k = 0; k < c.size(); ++k) st.push_back({c.size() > 1 ? double(k) / (c.size() - 1) : 0.0, QColor::fromRgb(c[k])});
            m_e.stops = st;
            bar->setStops(st);
            emitChange();
        });
        check(QStringLiteral("reverse"), QStringLiteral("Reverse"), e.flag, [this](bool v) { m_e.flag = v; });
        opacity();
        break;
    }
    case EffectType::Dither: {
        combo(QStringLiteral("method"), QStringLiteral("Method:"), fx2::ditherMethodNames(), e.mode, [this](int i) { m_e.mode = i; emitChange(); });
        auto *pal = new PaletteEditor(this, m_sample);
        pal->setObjectName(QStringLiteral("fx_palette"));
        pal->setColors(e.palette);
        combo(QStringLiteral("target"), QStringLiteral("Palette:"), fx2::paletteTargetNames(), e.mode2, [this, pal, f](int i) {
            m_e.mode2 = i;
            f->setRowVisible(pal, i == fx2::PalCustom);
            emitChange();
        });
        dspin(QStringLiteral("colors"), QStringLiteral("Colours (adaptive / grey):"), e.posterize, 2, 256, QString(), [this](double v) { m_e.posterize = int(v); });
        f->addRow(QStringLiteral("Custom palette:"), pal);
        f->setRowVisible(pal, e.mode2 == fx2::PalCustom);
        connect(pal, &PaletteEditor::changed, this, [this, pal] { m_e.palette = pal->colors(); emitChange(); });
        dspin(QStringLiteral("strength"), QStringLiteral("Strength:"), e.amount, 0, 100, QStringLiteral(" %"), [this](double v) { m_e.amount = v; });
        check(QStringLiteral("serpentine"), QStringLiteral("Serpentine scan (error diffusion)"), e.flag, [this](bool v) { m_e.flag = v; });
        break;
    }
    case EffectType::Outline:
        combo(QStringLiteral("placement"), QStringLiteral("Placement:"), {QStringLiteral("Outer"), QStringLiteral("Inner")}, e.mode, [this](int i) { m_e.mode = i; emitChange(); });
        combo(QStringLiteral("connect"), QStringLiteral("Connectivity:"), {QStringLiteral("4-connected (plus)"), QStringLiteral("8-connected (square)")}, e.mode2, [this](int i) { m_e.mode2 = i; emitChange(); });
        dspin(QStringLiteral("width"), QStringLiteral("Width:"), e.width, 1, 32, QStringLiteral(" px"), [this](double v) { m_e.width = v; });
        color(e.color);
        dspin(QStringLiteral("threshold"), QStringLiteral("Alpha threshold:"), e.threshold, 1, 255, QString(), [this](double v) { m_e.threshold = int(v); });
        opacity();
        break;
    case EffectType::PaletteSwap: {
        auto *from = new PaletteEditor(this, m_sample);
        from->setObjectName(QStringLiteral("fx_paletteFrom"));
        from->setColors(e.palette);
        auto *to = new PaletteEditor(this, m_sample);
        to->setObjectName(QStringLiteral("fx_paletteTo"));
        to->setColors(e.palette2);
        f->addRow(QStringLiteral("From:"), from);
        f->addRow(QStringLiteral("To:"), to);
        auto *copy = new QPushButton(QStringLiteral("Copy From \u2192 To"));
        copy->setObjectName(QStringLiteral("fx_copyPalette"));
        f->addRow(QString(), copy);
        connect(from, &PaletteEditor::changed, this, [this, from] { m_e.palette = from->colors(); emitChange(); });
        connect(to, &PaletteEditor::changed, this, [this, to] { m_e.palette2 = to->colors(); emitChange(); });
        connect(copy, &QPushButton::clicked, this, [this, from, to] { to->setColors(from->colors()); m_e.palette2 = to->colors(); emitChange(); });
        check(QStringLiteral("nearest"), QStringLiteral("Map other colours to the nearest \"From\" entry"), e.flag, [this](bool v) { m_e.flag = v; });
        f->addRow(new QLabel(QStringLiteral("Colour i of \"From\" becomes colour i of \"To\". Double-click a swatch to edit it.")));
        break;
    }
    case EffectType::ReplaceColor: {
        // Photoshop-style Replace Color: a colour range with fuzziness, replaced by a target colour, the target's
        // colour keeping the shading, or a hue / saturation / lightness shift; the grey preview shows the range.
        auto *from = new ColorButton(this, false);
        from->setObjectName(QStringLiteral("fx_color"));
        from->setColor(e.color);
        f->addRow(QStringLiteral("From:"), from);
        if (!m_sample.isNull()) {
            auto *sp = new SamplePicker(m_sample);
            sp->setObjectName(QStringLiteral("fx_pickFrom"));
            sp->setToolTip(QStringLiteral("Click the image to pick the colour to replace"));
            f->addRow(QStringLiteral("Pick From:"), sp);
            connect(sp, &SamplePicker::picked, this, [this, from](const QColor &c) { m_e.color = c; from->setColor(c); emitChange(); });
        }
        connect(from, &ColorButton::colorPicked, this, [this](const QColor &c) { m_e.color = c; emitChange(); });
        connect(from, &ColorButton::colorPreview, this, [this](const QColor &c) { m_e.color = c; emitChange(); });
        dspin(QStringLiteral("tolerance"), QStringLiteral("Tolerance:"), e.amount, 0, 100, QStringLiteral(" %"), [this](double v) { m_e.amount = v; });
        dspin(QStringLiteral("fuzziness"), QStringLiteral("Fuzziness:"), e.p[0], 0, 100, QStringLiteral(" %"), [this](double v) { m_e.p[0] = v; });
        auto *to = new ColorButton(this, false);
        to->setObjectName(QStringLiteral("fx_color2"));
        to->setColor(e.color2);
        connect(to, &ColorButton::colorPicked, this, [this](const QColor &c) { m_e.color2 = c; emitChange(); });
        connect(to, &ColorButton::colorPreview, this, [this](const QColor &c) { m_e.color2 = c; emitChange(); });
        QList<QWidget *> shiftRows;
        auto *range = combo(QStringLiteral("range"), QStringLiteral("Replace with:"),
                            {QStringLiteral("Solid colour"), QStringLiteral("Colour (keep shading)"), QStringLiteral("Hue / Saturation / Lightness shift")},
                            e.mode, [](int) {});
        f->addRow(QStringLiteral("To:"), to);
        shiftRows << dspin(QStringLiteral("hue"), QStringLiteral("Hue:"), e.hue, -180, 180, QStringLiteral("\u00b0"), [this](double v) { m_e.hue = v; })
                  << dspin(QStringLiteral("saturation"), QStringLiteral("Saturation:"), e.saturation, -100, 100, {}, [this](double v) { m_e.saturation = v; })
                  << dspin(QStringLiteral("lightness"), QStringLiteral("Lightness:"), e.lightness, -100, 100, {}, [this](double v) { m_e.lightness = v; });
        auto vis = [f, to, shiftRows](int mode) {
            f->setRowVisible(to, mode != fx2::RcShift);
            for (QWidget *w : shiftRows) f->setRowVisible(w, mode == fx2::RcShift);
        };
        vis(e.mode);
        connect(range, &QComboBox::currentIndexChanged, this, [this, vis](int i) { m_e.mode = i; vis(i); emitChange(); });
        opacity(QStringLiteral("Strength:"));
        check(QStringLiteral("hard"), QStringLiteral("Hard edges (no partial replacement; pixel art)"), e.flag, [this](bool v) { m_e.flag = v; });
        if (!m_sample.isNull()) {
            m_rcMask = new QLabel;
            m_rcMask->setObjectName(QStringLiteral("fx_rangePreview"));
            m_rcMask->setToolTip(QStringLiteral("The colour range: white is replaced, grey partly, black kept"));
            f->addRow(QStringLiteral("Range:"), m_rcMask);
            updateRangePreview();
        }
        break;
    }
    case EffectType::NormalMap:
        combo(QStringLiteral("operator"), QStringLiteral("Operator:"), {QStringLiteral("Sobel"), QStringLiteral("Scharr")}, e.mode, [this](int i) { m_e.mode = i; emitChange(); });
        combo(QStringLiteral("source"), QStringLiteral("Height from:"), {QStringLiteral("Luminance"), QStringLiteral("Alpha")}, e.mode2, [this](int i) { m_e.mode2 = i; emitChange(); });
        dspin(QStringLiteral("strength"), QStringLiteral("Strength:"), e.amount, 0.1, 20, QString(), [this](double v) { m_e.amount = v; }, 1);
        check(QStringLiteral("invertY"), QStringLiteral("Invert Y (DirectX convention)"), e.flag, [this](bool v) { m_e.flag = v; });
        check(QStringLiteral("wrap"), QStringLiteral("Wrap edges (tileable)"), e.flag2, [this](bool v) { m_e.flag2 = v; });
        break;
    case EffectType::AmbientOcclusion:
        combo(QStringLiteral("source"), QStringLiteral("Height from:"), {QStringLiteral("Luminance"), QStringLiteral("Alpha")}, e.mode2, [this](int i) { m_e.mode2 = i; emitChange(); });
        dspin(QStringLiteral("radius"), QStringLiteral("Radius:"), e.width, 1, 64, QStringLiteral(" px"), [this](double v) { m_e.width = v; });
        dspin(QStringLiteral("strength"), QStringLiteral("Strength:"), e.amount, 0, 400, QStringLiteral(" %"), [this](double v) { m_e.amount = v; });
        check(QStringLiteral("wrap"), QStringLiteral("Wrap edges (tileable)"), e.flag2, [this](bool v) { m_e.flag2 = v; });
        break;
    case EffectType::LensCorrection: {
        struct LP { const char *name, *label; double mn, mx; const char *suf; };
        static const LP lp[8] = {{"distortion", "Remove distortion:", -100, 100, ""},
                                 {"fringeRC", "Fix red/cyan fringe:", -100, 100, ""},
                                 {"fringeBY", "Fix blue/yellow fringe:", -100, 100, ""},
                                 {"vignette", "Vignette amount:", -100, 100, ""},
                                 {"midpoint", "Vignette midpoint:", 0, 100, ""},
                                 {"vertical", "Vertical perspective:", -100, 100, ""},
                                 {"horizontal", "Horizontal perspective:", -100, 100, ""},
                                 {"straighten", "Angle:", -45, 45, "\u00b0"}};
        for (int i = 0; i < 8; ++i)
            dspin(QLatin1String(lp[i].name), QLatin1String(lp[i].label), e.p[i], lp[i].mn, lp[i].mx, QString::fromUtf8(lp[i].suf),
                  [this, i](double v) { m_e.p[i] = v; }, i == 7 ? 1 : 0);
        break;
    }
    case EffectType::SmartSharpen:
        dspin(QStringLiteral("amount"), QStringLiteral("Amount:"), e.amount, 1, 500, QStringLiteral(" %"), [this](double v) { m_e.amount = v; });
        dspin(QStringLiteral("radius"), QStringLiteral("Radius:"), e.softness, 0.1, 64, QStringLiteral(" px"), [this](double v) { m_e.softness = v; }, 1);
        combo(QStringLiteral("remove"), QStringLiteral("Remove:"), {QStringLiteral("Gaussian Blur"), QStringLiteral("Lens Blur"), QStringLiteral("Motion Blur")}, e.mode, [this](int i) { m_e.mode = i; emitChange(); });
        angle(QStringLiteral("Motion angle:"));
        dspin(QStringLiteral("noise"), QStringLiteral("Reduce noise:"), e.p[0], 0, 100, QStringLiteral(" %"), [this](double v) { m_e.p[0] = v; });
        break;
    case EffectType::SurfaceBlur:
        dspin(QStringLiteral("radius"), QStringLiteral("Radius:"), e.softness, 1, 100, QStringLiteral(" px"), [this](double v) { m_e.softness = v; });
        dspin(QStringLiteral("threshold"), QStringLiteral("Threshold:"), e.threshold, 2, 255, QStringLiteral(" levels"), [this](double v) { m_e.threshold = int(v); });
        break;
    case EffectType::BilateralDenoise:
        dspin(QStringLiteral("spatial"), QStringLiteral("Spatial sigma:"), e.softness, 0.5, 20, QStringLiteral(" px"), [this](double v) { m_e.softness = v; }, 1);
        dspin(QStringLiteral("range"), QStringLiteral("Range sigma:"), e.amount, 1, 100, QString(), [this](double v) { m_e.amount = v; });
        dspin(QStringLiteral("passes"), QStringLiteral("Passes:"), e.quality, 1, 5, QString(), [this](double v) { m_e.quality = int(v); });
        break;
    case EffectType::MakeSeamless:
        combo(QStringLiteral("method"), QStringLiteral("Method:"), {QStringLiteral("Offset and blend"), QStringLiteral("Mirror edges")}, e.mode, [this](int i) { m_e.mode = i; emitChange(); });
        dspin(QStringLiteral("width"), QStringLiteral("Blend width:"), e.width, 1, 512, QStringLiteral(" px"), [this](double v) { m_e.width = v; });
        break;
    default:
        f->addRow(new QLabel(QStringLiteral("This effect has no settings.")));
        break;
    }
    m_preview = new QCheckBox(QStringLiteral("Preview"));
    m_preview->setObjectName(QStringLiteral("fx_preview"));
    m_preview->setChecked(true);
    connect(m_preview, &QCheckBox::toggled, this, [this](bool on) { emit changed(on ? m_e : m_orig); });
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *row = new QHBoxLayout;
    row->addWidget(m_preview);
    row->addStretch();
    row->addWidget(bb);
    f->addRow(row);
}

bool EffectDialog::previewEnabled() const { return m_preview && m_preview->isChecked(); }

void EffectDialog::emitChange() {
    updateRangePreview();
    if (previewEnabled()) emit changed(m_e);
}

void EffectDialog::updateRangePreview() {
    if (!m_rcMask || m_sample.isNull()) return;
    QImage sm = m_sample.width() > 200 || m_sample.height() > 140 ? m_sample.scaled(200, 140, Qt::KeepAspectRatio, Qt::FastTransformation) : m_sample;
    QImage mask = fx2::replaceColorMask(sm, fx2::replaceOptsFrom(m_e));
    // small bitmaps (icons, pixel art): enlarge the range hard-edged so it is readable
    const int side = qMax(mask.width(), mask.height());
    if (side > 0 && side < 100) {
        const int z = qMax(1, 160 / side);
        mask = mask.scaled(mask.size() * z, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    m_rcMask->setPixmap(QPixmap::fromImage(mask));
}

void EffectDialog::syncChannel() {
    m_syncing = true;
    if (m_hist) {
        const LevelsChannel &lc = m_e.levels[m_channel];
        if (m_levelSpins.size() == 5) {
            m_levelSpins[0]->setValue(lc.inBlack);
            m_levelSpins[1]->setValue(lc.gamma);
            m_levelSpins[2]->setValue(lc.inWhite);
            m_levelSpins[3]->setValue(lc.outBlack);
            m_levelSpins[4]->setValue(lc.outWhite);
        }
        m_hist->setData(m_sample.isNull() ? QVector<int>(256, 0) : fx::histogram(m_sample, m_channel), lc);
    }
    if (m_curve) {
        m_curve->setCurve(m_e.curves[m_channel]);
        static const QColor hc[kChannels] = {Qt::gray, QColor(220, 40, 40), QColor(40, 170, 40), QColor(40, 90, 230), QColor(120, 120, 120)};
        m_curve->setHistogram(m_sample.isNull() ? QVector<int>() : fx::histogram(m_sample, m_channel), hc[qBound(0, m_channel, kChannels - 1)]);
    }
    for (int c = 0; c < m_balanceSpins.size(); ++c) m_balanceSpins[c]->setValue(m_e.balance[m_range][c]);
    m_syncing = false;
}

void EffectDialog::applyDropper(Dropper d, const QColor &c) {
    if (m_e.type != EffectType::Levels || d == NoDropper) return;
    const int v[3] = {c.red(), c.green(), c.blue()};
    if (d == BlackPoint || d == WhitePoint) {
        for (int ch = 0; ch < 3; ++ch) {
            LevelsChannel &lc = m_e.levels[ch + 1];
            if (d == BlackPoint) lc.inBlack = qBound(0, v[ch], lc.inWhite - 1);
            else lc.inWhite = qBound(lc.inBlack + 1, v[ch], 255);
        }
    } else {
        // grey point: pick per-channel gammas that make the sampled colour neutral (at its mean level)
        double n[3], mean = 0;
        for (int ch = 0; ch < 3; ++ch) {
            const LevelsChannel &lc = m_e.levels[ch + 1];
            n[ch] = qBound(0.01, double(v[ch] - lc.inBlack) / qMax(1, lc.inWhite - lc.inBlack), 0.99);
            mean += n[ch] / 3;
        }
        for (int ch = 0; ch < 3; ++ch)
            m_e.levels[ch + 1].gamma = qBound(0.1, std::log(n[ch]) / std::log(mean), 9.99);
    }
    syncChannel();
    emitChange();
}

// Whether opening an editor for this effect type makes sense (it has parameters).
bool effectHasSettings(EffectType t) {
    switch (t) {
    case EffectType::Blur: case EffectType::BlurMore: case EffectType::Sharpen: case EffectType::SharpenMore:
    case EffectType::Invert: case EffectType::FindEdges: case EffectType::ConvertToAlpha:
    case EffectType::Desaturate: case EffectType::AutoLevels: case EffectType::AutoContrast:
        return false;
    default: return true;
    }
}

// ---------------------------------------------------------------- ExportDialog
ExportDialog::ExportDialog(const Document &doc, QWidget *parent, const QRect &areaIn) : QDialog(parent), m_doc(doc) {
    setWindowTitle(QStringLiteral("Export Preview"));
    m_area = areaIn.normalized() & QRect(0, 0, doc.s.width, doc.s.height);
    auto *h = new QHBoxLayout(this);
    auto *left = new QVBoxLayout;
    m_opt = new OptimizeWidget;
    ExportOptions start = doc.exportOptions;
    start.area = QRect();
    m_opt->setOptions(start);
    left->addWidget(m_opt);
    if (!m_area.isEmpty()) {
        m_areaOnly = new QCheckBox(QStringLiteral("Export area only (%1, %2  %3 \u00D7 %4)")
                                       .arg(m_area.x()).arg(m_area.y()).arg(m_area.width()).arg(m_area.height()));
        m_areaOnly->setObjectName(QStringLiteral("exportAreaOnly"));
        m_areaOnly->setToolTip(QStringLiteral("Export only the rectangle drawn with the Export Area tool"));
        m_areaOnly->setChecked(true);
        left->addWidget(m_areaOnly);
    }
    {   // v0.3.1 export scale with the Resample list (pixel-art scalers included)
        auto *sr = new QHBoxLayout;
        m_scale = new PopupSliderField; m_scale->setObjectName(QStringLiteral("exportScalePct"));
        m_scale->setRange(1, 1600); m_scale->setSuffix(QStringLiteral(" %")); m_scale->setValue(qBound(1, start.scalePct, 1600));
        m_scale->setToolTip(QStringLiteral("Scale the exported image (bitmap formats); 200 % = 2x"));
        m_resample = new QComboBox; m_resample->setObjectName(QStringLiteral("exportScaleResample"));
        m_resample->addItem(QStringLiteral("Preferences default"));
        m_resample->addItems(pixelscale::resampleNames());
        m_resample->setCurrentIndex(qBound(-1, start.scaleResample, pixelscale::kResampleCount - 1) + 1);
        sr->addWidget(new QLabel(QStringLiteral("Scale:")));
        sr->addWidget(m_scale);
        sr->addWidget(m_resample, 1);
        left->addLayout(sr);
    }
    left->addStretch();
    m_copyStatus = new QLabel;
    m_copyStatus->setObjectName(QStringLiteral("exportCopyStatus"));
    left->addWidget(m_copyStatus);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_exportBtn = m_buttons->addButton(QStringLiteral("Export..."), QDialogButtonBox::AcceptRole);
    m_exportBtn->setObjectName(QStringLiteral("exportPreviewExport"));
    m_exportBtn->setDefault(true);
    m_copyBtn = new QPushButton(QStringLiteral("Copy to Clipboard"));
    m_copyBtn->setObjectName(QStringLiteral("exportCopyClipboard"));
    m_copyBtn->setToolTip(QStringLiteral("Copy the exported image to the clipboard"));
    m_buttons->addButton(m_copyBtn, QDialogButtonBox::ActionRole);
    placeCopyAfterExport();
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_copyBtn, &QPushButton::clicked, this, &ExportDialog::copyToClipboard);
    left->addWidget(m_buttons);
    h->addLayout(left);
    // zoomable preview: Ctrl+=/-/0/1, Ctrl+wheel, Zoom tool clicks/drag, Hand / Space-drag
    auto *right = new QVBoxLayout;
    m_view = new PreviewView(outputSize(), this);
    m_pane = new PreviewPane(m_view);
    m_pane->setObjectName(QStringLiteral("exportPreviewPane"));
    m_pane->setMinimumSize(420, 320);
    right->addWidget(m_pane, 1);
    auto *zr = new QHBoxLayout;
    auto mk = [&](const QString &text, const QString &tip, auto fn) {
        auto *b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        connect(b, &QToolButton::clicked, this, fn);
        zr->addWidget(b);
    };
    mk(QStringLiteral("-"), QStringLiteral("Zoom Out (Ctrl+-)"), [this] { m_view->zoomOut(); });
    mk(QStringLiteral("+"), QStringLiteral("Zoom In (Ctrl+=)"), [this] { m_view->zoomIn(); });
    mk(QStringLiteral("Fit"), QStringLiteral("Fit (Ctrl+0)"), [this] { m_view->fit(m_pane->imageArea()); });
    mk(QStringLiteral("100%"), QStringLiteral("Actual Size (Ctrl+1)"), [this] { m_view->actualSize(); });
    zr->addStretch();
    m_zoomLabel = new QLabel(QStringLiteral("Zoom: 100%"));
    m_zoomLabel->setObjectName(QStringLiteral("exportPreviewZoom"));
    zr->addWidget(m_zoomLabel);
    right->addLayout(zr);
    connect(m_view, &PreviewView::zoomChanged, this, [this](double z) { m_zoomLabel->setText(QStringLiteral("Zoom: %1%").arg(qRound(z * 100))); });
    h->addLayout(right, 1);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(200);
    connect(timer, &QTimer::timeout, this, &ExportDialog::updatePreview);
    connect(m_opt, &OptimizeWidget::optionsChanged, timer, qOverload<>(&QTimer::start));
    connect(m_opt, &OptimizeWidget::optimizeToSizeRequested, this, [this, timer] {
        ExportOptions o = options();
        if (!runOptimizeToSizeWizard(this, m_doc, o, m_doc.curFrame)) return;
        m_opt->setOptions(o);
        emit m_opt->optionsChanged(o);
        timer->start();
    });
    auto resized = [this] {
        m_view->setDocSize(outputSize());
        m_view->fit(m_pane->imageArea());
        updatePreview();
    };
    if (m_areaOnly) connect(m_areaOnly, &QCheckBox::toggled, this, resized);
    connect(m_scale, &QSpinBox::valueChanged, timer, [resized] { resized(); });
    connect(m_resample, &QComboBox::currentIndexChanged, timer, qOverload<>(&QTimer::start));
    updatePreview();
}

QRect ExportDialog::area() const { return m_areaOnly && m_areaOnly->isChecked() ? m_area : QRect(); }

QSize ExportDialog::outputSize() const {
    const QRect a = area();
    const QSize base = a.isEmpty() ? QSize(m_doc.s.width, m_doc.s.height) : a.size();
    const int pct = m_scale ? m_scale->value() : 100;
    return pct == 100 ? base : QSize(qMax(1, qRound(base.width() * pct / 100.0)), qMax(1, qRound(base.height() * pct / 100.0)));
}

ExportOptions ExportDialog::options() const {
    ExportOptions o = m_opt->options();
    o.area = area();
    o.scalePct = m_scale->value();
    o.scaleResample = m_resample->currentIndex() - 1;
    return o;
}

void ExportDialog::updatePreview() {
    m_copyStatus->clear();
    if (m_copyBtn)
        m_copyBtn->setToolTip(QStringLiteral("Copy the exported image to the clipboard"));
    EncodeResult r = encodePreview(m_doc, options(), m_doc.curFrame);
    if (!r.error.isEmpty() || r.image.isNull()) {
        m_previewOk = false;
        m_previewImage = QImage();
        m_previewEncoded.clear();
        m_pane->setResult({}, r.error.isEmpty() ? QStringLiteral("Nothing to export") : r.error);
        m_opt->setSizeText({});
        updateCopyButton();
        return;
    }
    m_previewOk = true;
    m_previewImage = r.image;
    m_previewEncoded = r.encoded;
    const QString saving = describeSaving(r);
    m_pane->setResult(r.image, describeSize(r.bytes) + (saving.isEmpty() ? QString() : QStringLiteral("\n") + saving));
    m_opt->setSizeText(describeSize(r.bytes) + (r.frames > 1 ? QStringLiteral("\n%1 frames").arg(r.frames) : QString())
                       + (saving.isEmpty() ? QString() : QStringLiteral("\n") + saving));
    m_opt->setColorTable(r.colorTable);
    updateCopyButton();
}

void ExportDialog::showEvent(QShowEvent *e) {
    QDialog::showEvent(e);
    placeCopyAfterExport();
    if (!m_fitted) {
        m_fitted = true;
        m_view->fit(m_pane->imageArea());
        m_pane->setFocus();
    }
}

bool ExportDialog::canCopyFormat(Fmt f) {
    // Not a pasteable image: C header text, PDF, or a layered/vector document.
    // HTML and slice exports live in other dialogs.
    switch (f) {
    case Fmt::CHeader:
    case Fmt::PDF:
    case Fmt::SVG:
    case Fmt::SVGZ:
    case Fmt::PSD:
    case Fmt::ORA:
    case Fmt::XCF:
    case Fmt::RAW:
        return false;
    default:
        return ImageIO::available(f) && ImageIO::canWrite(f);
    }
}

void ExportDialog::updateCopyButton() {
    if (!m_copyBtn) return;
    m_copyBtn->setEnabled(m_previewOk && !m_previewImage.isNull() && canCopyFormat(options().format));
}

void ExportDialog::placeCopyAfterExport() {
    if (!m_buttons || !m_exportBtn || !m_copyBtn) return;
    auto *lay = qobject_cast<QBoxLayout *>(m_buttons->layout());
    if (!lay) return;
    int exp = -1, copy = -1;
    for (int i = 0; i < lay->count(); ++i) {
        QWidget *w = lay->itemAt(i) ? lay->itemAt(i)->widget() : nullptr;
        if (w == m_exportBtn) exp = i;
        if (w == m_copyBtn) copy = i;
    }
    if (exp < 0 || copy < 0 || copy == exp + 1) return;
    QLayoutItem *it = lay->takeAt(copy);
    const int insertAt = (copy < exp) ? exp : exp + 1;
    lay->insertItem(insertAt, it);
}

void ExportDialog::copyToClipboard() {
    if (!m_copyBtn || !m_copyBtn->isEnabled() || !m_previewOk || m_previewImage.isNull()) return;
    QImage img = m_previewImage;
    if (img.hasAlphaChannel() && img.format() != QImage::Format_ARGB32
        && img.format() != QImage::Format_ARGB32_Premultiplied
        && img.format() != QImage::Format_RGBA8888)
        img = img.convertToFormat(QImage::Format_ARGB32);
    auto *mime = new QMimeData;
    mime->setImageData(img);
    const QString type = ImageIO::mimeType(options().format);
    if (!type.isEmpty() && !m_previewEncoded.isEmpty())
        mime->setData(type, m_previewEncoded);
    if (QClipboard *cb = QGuiApplication::clipboard())
        cb->setMimeData(mime);
    else {
        delete mime;
        return;
    }
    m_copyStatus->setText(QStringLiteral("Copied"));
    m_copyBtn->setToolTip(QStringLiteral("Copied"));
}

void ExportDialog::keyPressEvent(QKeyEvent *e) {
    if (PreviewPane::handleZoomKey(e, m_view, m_pane->imageArea())) return;
    QDialog::keyPressEvent(e);
}

// ---------------------------------------------------------------- helpers
namespace dialogs {

void showShortcuts(QWidget *parent) {
    QDialog d(parent);
    d.setObjectName(QStringLiteral("keyboardShortcutsDialog"));
    d.setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
    auto *l = new QVBoxLayout(&d);
    auto *t = new QTreeWidget;
    t->setObjectName(QStringLiteral("shortcutsTree"));
    t->setHeaderLabels({QStringLiteral("Command"), QStringLiteral("Shortcut(s)")});
    t->setColumnWidth(0, 300);
    auto &sm = shortcuts::Manager::get();
    auto addRoot = [&](const QString &title, const QString &cat) {
        auto *root = new QTreeWidgetItem(t, {title});
        for (const shortcuts::ActionInfo &a : sm.catalog()) {
            if (a.category != cat) continue;
            QStringList parts;
            for (const QKeySequence &k : sm.sequences(a.id)) parts << k.toString(QKeySequence::NativeText);
            new QTreeWidgetItem(root, {a.label, parts.isEmpty() ? QStringLiteral("—") : parts.join(QStringLiteral(", "))});
        }
        root->setExpanded(true);
    };
    addRoot(QStringLiteral("Menu Commands"), QStringLiteral("menu"));
    addRoot(QStringLiteral("Tools"), QStringLiteral("tools"));
    addRoot(QStringLiteral("Miscellaneous"), QStringLiteral("misc"));
    {
        auto *root = new QTreeWidgetItem(t, {QStringLiteral("Modifiers (not editable)")});
        root->setToolTip(0, QStringLiteral("Held during a drag or click. Not shortcut bindings."));
        for (const shortcuts::ModifierNote &n : shortcuts::Manager::modifierNotes())
            new QTreeWidgetItem(root, {QStringLiteral("%1 — %2").arg(n.when, n.effect), n.keys});
        root->setExpanded(true);
    }
    l->addWidget(t, 1);
    auto *row = new QHBoxLayout;
    auto *exp = new QPushButton(QStringLiteral("Export as HTML..."));
    exp->setObjectName(QStringLiteral("shortcutsExportHtml"));
    auto *cfg = new QPushButton(QStringLiteral("Configure..."));
    cfg->setObjectName(QStringLiteral("shortcutsConfigure"));
    row->addWidget(exp);
    row->addWidget(cfg);
    row->addStretch();
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    row->addWidget(bb);
    l->addLayout(row);
    QObject::connect(exp, &QPushButton::clicked, &d, [&] {
        const QString path = filedialogs::getSaveFileName(&d, QStringLiteral("Export Keyboard Shortcuts"),
            QStringLiteral("libreworks-shortcuts.html"), QStringLiteral("HTML (*.html)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(&d, QStringLiteral("Export"), f.errorString());
            return;
        }
        QTextStream o(&f);
        o << sm.toHtml();
    });
    QObject::connect(cfg, &QPushButton::clicked, &d, [parent, &d] {
        d.accept();
        // Keyboard tab is the last one added (after General, Editing, Themes, Tablet)
        PreferencesDialog dlg(parent, /*initialTab*/ 4);
        if (dlg.exec() == QDialog::Accepted) {
            appState().setPrefs(dlg.prefs());
            appState().savePrefs();
            QSettings st;
            shortcuts::Manager::get().save(st);
            if (auto *mw = qobject_cast<MainWindow *>(parent)) shortcuts::Manager::get().apply(mw);
        }
    });
    d.resize(640, 480);
    d.exec();
}
} // namespace dialogs

HtmlExportDialog::HtmlExportDialog(const QString &suggestedPath, QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Export HTML and Images"));
    setObjectName(QStringLiteral("HtmlExportDialog"));
    auto *f = new QFormLayout(this);
    auto *row = new QHBoxLayout;
    m_path = new QLineEdit(suggestedPath);
    m_path->setObjectName(QStringLiteral("htmlPath"));
    m_path->setMinimumWidth(320);
    auto *browse = new QPushButton(QStringLiteral("Browse..."));
    row->addWidget(m_path);
    row->addWidget(browse);
    f->addRow(QStringLiteral("HTML file:"), row);
    m_layout = new QComboBox;
    m_layout->setObjectName(QStringLiteral("htmlLayout"));
    m_layout->addItems({QStringLiteral("Table"), QStringLiteral("CSS (absolute positioning)")});
    f->addRow(QStringLiteral("Layout:"), m_layout);
    m_images = new QLineEdit(QStringLiteral("images"));
    m_images->setObjectName(QStringLiteral("htmlImagesDir"));
    m_images->setPlaceholderText(QStringLiteral("(same folder as the HTML file)"));
    f->addRow(QStringLiteral("Put images in subfolder:"), m_images);
    auto *note = new QLabel(QStringLiteral("Slices become separate images (each with its own export settings); the rest of the "
                                           "canvas is sliced automatically. Hotspots become image maps."));
    note->setWordWrap(true);
    f->addRow(note);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Export"));
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse, &QPushButton::clicked, this, [this] {
        QString p = filedialogs::getSaveFileName(this, QStringLiteral("Export HTML"), m_path->text(), QStringLiteral("HTML (*.html *.htm)"));
        if (!p.isEmpty()) m_path->setText(p);
    });
}

QString HtmlExportDialog::path() const { return m_path->text().trimmed(); }

html::Options HtmlExportDialog::options() const {
    html::Options o;
    o.layout = m_layout->currentIndex() == 1 ? html::Layout::Css : html::Layout::Table;
    o.imagesDir = m_images->text().trimmed();
    return o;
}

EditGridDialog::EditGridDialog(Document *doc, QWidget *parent) : QDialog(parent), m_doc(doc), m_orig(doc->grid) {
    setWindowTitle(QStringLiteral("Edit Grid"));
    setObjectName(QStringLiteral("EditGridDialog"));
    auto *f = new QFormLayout(this);
    m_color = new ColorButton(nullptr, true);
    m_color->setObjectName(QStringLiteral("gridColor"));
    m_color->setFixedSize(40, 22);
    m_color->setColor(m_orig.color);
    f->addRow(QStringLiteral("Color:"), m_color);
    m_show = new QCheckBox(QStringLiteral("Show grid"));
    m_show->setObjectName(QStringLiteral("gridShow"));
    m_show->setChecked(m_orig.show);
    m_snap = new QCheckBox(QStringLiteral("Snap to grid"));
    m_snap->setObjectName(QStringLiteral("gridSnap"));
    m_snap->setChecked(m_orig.snap);
    f->addRow(m_show);
    f->addRow(m_snap);
    auto spin = [](const char *name, int v) {
        auto *s = new PopupSliderField;
        s->setObjectName(QLatin1String(name));
        s->setRange(1, 1000);   // 1 px: a pixel grid
        s->setSuffix(QStringLiteral(" px"));
        s->setValue(v);
        return s;
    };
    m_h = spin("gridH", m_orig.x);
    m_v = spin("gridV", m_orig.y);
    f->addRow(QStringLiteral("Horizontal spacing (\u2194):"), m_h);
    f->addRow(QStringLiteral("Vertical spacing (\u2195):"), m_v);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &EditGridDialog::apply);
    f->addRow(bb);
}

GridSettings EditGridDialog::settings() const {
    GridSettings g;
    g.x = m_h->value();
    g.y = m_v->value();
    g.color = m_color->color();
    g.show = m_show->isChecked();
    g.snap = m_snap->isChecked();
    return g;
}

void EditGridDialog::apply() { if (!(settings() == m_doc->grid)) m_doc->setGrid(settings()); }
void EditGridDialog::accept() { apply(); QDialog::accept(); }
void EditGridDialog::reject() { if (!(m_orig == m_doc->grid)) m_doc->setGrid(m_orig); QDialog::reject(); }

EditGuidesDialog::EditGuidesDialog(Document *doc, QWidget *parent) : QDialog(parent), m_doc(doc) {
    setWindowTitle(QStringLiteral("Edit Guides"));
    setObjectName(QStringLiteral("EditGuidesDialog"));
    const GuideSettings &g = doc->guides;
    auto *f = new QFormLayout(this);
    m_color = new ColorButton(nullptr, false);
    m_color->setObjectName(QStringLiteral("guideColor"));
    m_color->setFixedSize(40, 22);
    m_color->setColor(g.color);
    f->addRow(QStringLiteral("Color:"), m_color);
    m_show = new QCheckBox(QStringLiteral("Show guides"));
    m_show->setChecked(g.show);
    m_snap = new QCheckBox(QStringLiteral("Snap to guides"));
    m_snap->setChecked(g.snap);
    m_lock = new QCheckBox(QStringLiteral("Lock guides"));
    m_lock->setChecked(g.lock);
    f->addRow(m_show);
    f->addRow(m_snap);
    f->addRow(m_lock);
    auto *clear = new QPushButton(QStringLiteral("Clear All"));
    clear->setObjectName(QStringLiteral("guidesClear"));
    connect(clear, &QPushButton::clicked, this, [this, clear] { m_clear = true; clear->setEnabled(false); });
    f->addRow(QStringLiteral("%1 vertical, %2 horizontal").arg(g.v.size()).arg(g.h.size()), clear);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(bb);
}

void EditGuidesDialog::accept() {
    GuideSettings g = m_doc->guides;
    g.color = m_color->color();
    g.show = m_show->isChecked();
    g.snap = m_snap->isChecked();
    g.lock = m_lock->isChecked();
    if (m_clear) { g.v.clear(); g.h.clear(); }
    if (!(g == m_doc->guides)) m_doc->setGuides(g);
    QDialog::accept();
}

#include <QRadioButton>

CopyToFramesDialog::CopyToFramesDialog(int frameCount, int currentFrame, QWidget *parent)
    : QDialog(parent), m_count(qMax(1, frameCount)), m_cur(qBound(0, currentFrame, qMax(0, frameCount - 1)))
{
    setWindowTitle(QStringLiteral("Copy to Frames"));
    setObjectName(QStringLiteral("copyToFramesDialog"));
    auto *v = new QVBoxLayout(this);
    m_all = new QRadioButton(QStringLiteral("All frames"));
    m_prev = new QRadioButton(QStringLiteral("Previous frame"));
    m_next = new QRadioButton(QStringLiteral("Next frame"));
    m_range = new QRadioButton(QStringLiteral("Range"));
    m_all->setObjectName(QStringLiteral("copyFramesAll"));
    m_prev->setObjectName(QStringLiteral("copyFramesPrevious"));
    m_next->setObjectName(QStringLiteral("copyFramesNext"));
    m_range->setObjectName(QStringLiteral("copyFramesRange"));
    m_all->setChecked(true);
    m_prev->setEnabled(m_cur > 0);
    m_next->setEnabled(m_cur + 1 < m_count);
    auto *grp = new QButtonGroup(this);
    for (auto *r : {m_all, m_prev, m_next, m_range}) { grp->addButton(r); v->addWidget(r); }
    auto *row = new QHBoxLayout;
    m_from = new PopupSliderField;
    m_to = new PopupSliderField;
    m_from->setObjectName(QStringLiteral("copyFramesFrom"));
    m_to->setObjectName(QStringLiteral("copyFramesTo"));
    m_from->setRange(1, m_count);
    m_to->setRange(1, m_count);
    m_from->setValue(1);
    m_to->setValue(m_count);
    static_cast<PopupSliderField *>(m_from)->setSliderRange(1, m_count);
    static_cast<PopupSliderField *>(m_to)->setSliderRange(1, m_count);
    row->addWidget(new QLabel(QStringLiteral("From:")));
    row->addWidget(m_from);
    row->addWidget(new QLabel(QStringLiteral("To:")));
    row->addWidget(m_to);
    v->addLayout(row);
    auto enableRange = [this] { const bool on = m_range->isChecked(); m_from->setEnabled(on); m_to->setEnabled(on); };
    connect(m_range, &QRadioButton::toggled, this, [enableRange](bool) { enableRange(); });
    enableRange();
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(bb);
}

QVector<int> CopyToFramesDialog::targets() const
{
    QVector<int> t;
    auto add = [&](int i) { if (i >= 0 && i < m_count && i != m_cur && !t.contains(i)) t.append(i); };
    if (m_prev->isChecked()) add(m_cur - 1);
    else if (m_next->isChecked()) add(m_cur + 1);
    else if (m_range->isChecked()) {
        int a = qMin(m_from->value(), m_to->value()) - 1;
        int b = qMax(m_from->value(), m_to->value()) - 1;
        for (int i = a; i <= b; ++i) add(i);
    } else {
        for (int i = 0; i < m_count; ++i) add(i);
    }
    return t;
}


bool runOptimizeToSizeWizard(QWidget *parent, const Document &doc, ExportOptions &io, int frame)
{
    QDialog ask(parent);
    ask.setWindowTitle(QStringLiteral("Optimize to Size"));
    ask.setObjectName(QStringLiteral("optimizeToSizeDialog"));
    auto *v = new QVBoxLayout(&ask);
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Target file size:")));
    auto *kb = new PopupSliderField;
    kb->setObjectName(QStringLiteral("optimizeToSizeKb"));
    kb->setRange(1, 100000);
    kb->setSuffix(QStringLiteral(" KB"));
    kb->setSliderRange(1, 500);
    const EncodeResult cur = encodePreview(doc, io, frame);
    const int def = cur.bytes > 0 ? qMax(1, int((cur.bytes + 1023) / 1024)) : 50;
    kb->setValue(def);
    row->addWidget(kb);
    v->addLayout(row);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &ask, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &ask, &QDialog::reject);
    v->addWidget(bb);
    if (ask.exec() != QDialog::Accepted) return false;

    const qint64 target = qint64(kb->value()) * 1024;
    auto *prog = new QProgressDialog(QStringLiteral("Searching export settings\u2026"), QStringLiteral("Cancel"), 0, 0, parent);
    prog->setWindowTitle(QStringLiteral("Optimize to Size"));
    prog->setObjectName(QStringLiteral("optimizeToSizeProgress"));
    prog->setWindowModality(Qt::WindowModal);
    prog->setMinimumDuration(0);
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    QObject::connect(prog, &QProgressDialog::canceled, [cancel] { cancel->store(true); });
    std::shared_ptr<const Document> snap(doc.snapshot());
    const ExportOptions start = io;
    const int fr = frame;
    QFutureWatcher<OptimizeToSizeResult> watch;
    QEventLoop loop;
    QObject::connect(&watch, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    watch.setFuture(QtConcurrent::run([snap, start, fr, target, cancel] {
        return optimizeToSize(*snap, start, fr, target, cancel.get());
    }));
    loop.exec();
    prog->reset();
    prog->deleteLater();
    const OptimizeToSizeResult r = watch.result();
    if (r.cancelled) return false;
    if (!r.encoded.error.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("Optimize to Size"), r.encoded.error);
        return false;
    }
    io = r.options;
    if (!r.metTarget) {
        QMessageBox::information(parent, QStringLiteral("Optimize to Size"),
                                 QStringLiteral("Nothing fit the target of %1 KB. The smallest settings have been applied (%2).")
                                     .arg(kb->value())
                                     .arg(describeSize(r.encoded.bytes)));
    }
    return true;
}
