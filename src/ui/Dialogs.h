#pragma once
#include "core/Document.h"
#include "io/HtmlExport.h"
#include "core/Object.h"
#include "io/ImageIO.h"
#include "ClipboardImage.h"
#include <QDialog>
#include <QToolButton>

class QSpinBox;
class QRadioButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSlider;
class QDoubleSpinBox;
class Document;

// The one colour selector of the app: a swatch; a click opens the Fireworks-style swatch pop-up
// (ColorPopup: swatches, hex, the eyedropper, "..." for the full colour dialog, which has the
// eyedropper too). Optional "none" state. colorPreview() fires while hovering a swatch or
// picking with the eyedropper (for live previews), and again with color() when that ends
// without a pick.
class ColorPopup;
class ColorButton : public QToolButton {
    Q_OBJECT
public:
    explicit ColorButton(QWidget *parent = nullptr, bool allowAlpha = true);
    QColor color() const { return m_color; }
    void setColor(const QColor &c);
    void setNone(bool none) { m_none = none; update(); }
    void setIconHint(const QIcon &i) { m_hint = i; update(); }
    void setTransparentChoice(bool on) { m_transparentChoice = on; }   // the pop-up offers "Transparent"
    bool isPreviewing() const { return m_previewing; }   // showing a hovered colour, not yet picked
    ColorPopup *showPopup();
    void startEyedropper();
    void openDialog();
    void openDialog(const QColor &initial);
signals:
    void colorPicked(const QColor &c);
    void colorPreview(const QColor &c);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    void choose(const QColor &c);
    void endPreview();
    bool m_previewing = false;
    QColor m_color = Qt::black;
    bool m_alpha;
    bool m_none = false;
    bool m_transparentChoice = false;
    QIcon m_hint;
    QColor m_popupWorking;
};

// Format/quality/palette controls shared by the Optimize panel and the export dialog.
// The colour table of an indexed export (GIF / PNG 8 / Tiny PNG), as Fireworks' Optimize panel shows it.
class ColorTableView : public QWidget {
    Q_OBJECT
public:
    explicit ColorTableView(QWidget *parent = nullptr);
    void setColors(const QVector<QRgb> &c);
    QVector<QRgb> colors() const { return m_colors; }
    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QVector<QRgb> m_colors;
};

// Editable palette (v0.3.0): swatch grid (click selects, double-click edits) with Add / Remove /
// From Image / Load / Save (.gpl, .pal, .act, or a .png / .gif's colours).
class PaletteEditor : public QWidget {
    Q_OBJECT
public:
    explicit PaletteEditor(QWidget *parent = nullptr, const QImage &sample = QImage());
    void setColors(const QVector<QRgb> &c);
    QVector<QRgb> colors() const { return m_colors; }
    int current() const { return m_cur; }
    void setCurrent(int i);
    class PaletteSwatches *swatches() const { return m_sw; }
signals:
    void changed();
private:
    void sync();
    QVector<QRgb> m_colors;
    int m_cur = -1;
    QImage m_sample;
    class PaletteSwatches *m_sw;
    QLabel *m_count;
};

class OptimizeWidget : public QWidget {
    Q_OBJECT
public:
    explicit OptimizeWidget(QWidget *parent = nullptr);
    void setOptions(const ExportOptions &o);
    ExportOptions options() const { return m_o; }
    void setSizeText(const QString &t);
    void setColorTable(const QVector<QRgb> &c);
    static QStringList presetNames();
    QToolButton *optimizeToSizeButton() const { return m_optSize; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
signals:
    void optionsChanged(const ExportOptions &o);
    void optimizeToSizeRequested();
private:
    void sync();
    void fromUi();
    void loadCustomPalette();
    ExportOptions m_o;
    bool m_updating = false;
    QComboBox *m_preset, *m_format, *m_palette, *m_netpbm;
    QComboBox *m_tiffComp, *m_tiffDepth, *m_tgaDepth, *m_tgaOrigin;   // TIFF / TGA options
    QCheckBox *m_alpha, *m_tgaRle;
    QLabel *m_tiffCompLabel, *m_depthLabel, *m_tgaOriginLabel;
    QSpinBox *m_quality, *m_colors, *m_ditherAmount, *m_loss;   // Fireworks pop-up slider fields
    QSpinBox *m_effort;                                         // WebP libwebp method 0..6
    QLabel *m_lossLabel, *m_effortLabel;
    QCheckBox *m_lossless, *m_dither, *m_animated;
    ColorButton *m_matte;
    QToolButton *m_optSize = nullptr;
    QLabel *m_size, *m_qualityLabel, *m_colorsLabel, *m_paletteLabel, *m_netpbmLabel, *m_matteLabel;
    // v0.2.5: PNG
    QLabel *m_pngTypeLabel, *m_pngTransLabel, *m_pngCompLabel;
    QComboBox *m_pngType, *m_pngTrans;
    QSpinBox *m_pngComp;
    QCheckBox *m_png16, *m_interlace, *m_editable, *m_tiny;
    QToolButton *m_loadPalette;
    QWidget *m_tinyBox;
    QSpinBox *m_tinyMin, *m_tinyMax, *m_tinySpeed;
    ColorTableView *m_table;
    // JPEG
    QLabel *m_jpegEncLabel, *m_subLabel, *m_smoothLabel;
    QComboBox *m_jpegEnc, *m_sub;
    QSpinBox *m_smooth;
    QCheckBox *m_progressive, *m_optimize, *m_sharpen, *m_strip, *m_keepIcc;
    // BMP / icons / textures / EXR / PCX
    QLabel *m_bmpLabel, *m_iconLabel, *m_hotLabel, *m_texLabel, *m_exrLabel;
    QComboBox *m_bmpBits, *m_tex, *m_exrComp;
    QCheckBox *m_bmpRle, *m_mipmaps, *m_ktxZstd, *m_exrHalf, *m_pcx8;
    QWidget *m_iconBox;
    QList<QCheckBox *> m_iconChecks;
    QSpinBox *m_hotX, *m_hotY;
    // 0.3.20 encoder controls (visible only for the relevant format)
    QLabel *m_webpAlphaLabel = nullptr, *m_avifSpeedLabel = nullptr, *m_avifChromaLabel = nullptr;
    QLabel *m_avifDepthLabel = nullptr, *m_avifAlphaLabel = nullptr, *m_jxlEffortLabel = nullptr;
    QLabel *m_heifPresetLabel = nullptr, *m_heifChromaLabel = nullptr, *m_ktxZstdLevelLabel = nullptr;
    QSpinBox *m_webpAlpha = nullptr, *m_avifSpeed = nullptr, *m_avifAlpha = nullptr, *m_jxlEffort = nullptr;
    QSpinBox *m_ktxZstdLevel = nullptr, *m_tinyDither = nullptr;
    QComboBox *m_avifChroma = nullptr, *m_avifDepth = nullptr, *m_heifPreset = nullptr, *m_heifChroma = nullptr;
    QCheckBox *m_webpSharpYuv = nullptr, *m_webpExact = nullptr, *m_avifAlphaAuto = nullptr;
    // v0.2.6: C Header (.h)
    QWidget *m_chBox;
    QComboBox *m_chPixel, *m_chElement, *m_chRadix, *m_chConstants, *m_chFrameLayout, *m_chStyle, *m_chDither;
    QSpinBox *m_chPerLine;
    QCheckBox *m_chConst, *m_chProgmem, *m_chAligned, *m_chSection, *m_chStdint, *m_chDelays, *m_chRle, *m_chRawBin, *m_chLsb, *m_chInvert;
    QLineEdit *m_chSectionName, *m_chName;
    // v0.2.7: GIMP-style output and whitespace
    QCheckBox *m_chGimpMacros, *m_chGimpGlib, *m_chGimpComment, *m_chStrip;
    QLineEdit *m_chGimpCommentText;
    QSpinBox *m_chGimpOpacity;
    QWidget *m_chGimpBox;
public:
    // for tests
    QComboBox *presetCombo() const { return m_preset; }
    QComboBox *paletteCombo() const { return m_palette; }
    ColorTableView *colorTable() const { return m_table; }
};

// File > New. Like Fireworks, an image on the clipboard sizes the canvas (v0.3.1): when the dialog
// is shown it reads the clipboard (header only; a full decode, when needed, runs on a worker thread),
// fills Width/Height and Resolution from it and says so. It never pastes. Values the user typed win.
class NewDocumentDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewDocumentDialog(QWidget *parent = nullptr, bool readClipboard = true);
    int w() const;               // pixels
    int h() const;
    double resolution() const;   // pixels/inch
    QColor canvasColor() const;
    enum Units { Pixels, Inches, Centimeters };
    void setUnits(Units u);
    void setResolutionPerCm(bool perCm);
    QString clipboardNote() const;      // "" when the size did not come from the clipboard
    bool sizeFromClipboard() const { return m_fromClipboard; }
    bool readingClipboard() const { return m_pending; }
    void probeClipboard();              // read the clipboard now (also done on show and on change)
    void applyClipboardInfo(const clipimage::Info &info);
    static constexpr int kDefaultW = 660, kDefaultH = 440;
protected:
    void showEvent(QShowEvent *e) override;
private:
    void refresh();
    void sizeEdited();
    void resolutionEdited();
    double unitFactor() const;   // pixels per displayed unit
    QDoubleSpinBox *m_w, *m_h, *m_res;
    QComboBox *m_units, *m_resUnits;
    QComboBox *m_bg;
    ColorButton *m_custom;
    QLabel *m_note;
    int m_pxW = kDefaultW, m_pxH = kDefaultH;
    double m_dpi = 72;
    bool m_read = true, m_probed = false, m_updating = false;
    bool m_sizeEdited = false, m_resEdited = false, m_fromClipboard = false, m_pending = false;
    int m_gen = 0;
};

class SizeDialog : public QDialog {
    Q_OBJECT
public:
    // canvas = true: Canvas Size with anchor grid; false: Image Size with constrain checkbox
    SizeDialog(bool canvas, int w, int h, QWidget *parent = nullptr);
    int w() const;
    int h() const;
    int resample() const;   // Image Size: Resample list entry (pixelscale); -1 for Canvas Size
    QPoint offsetFor(int oldW, int oldH) const;
private:
    bool m_canvasMode;
    int m_ow, m_oh;
    QSpinBox *m_w, *m_h;
    QCheckBox *m_constrain = nullptr;
    QComboBox *m_resample = nullptr;
    int m_anchor = 4;
};

// Small tone-curve editor (Curves effect): click to add a point, drag to move, drag off / right-click to delete.
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget *parent = nullptr);
    void setCurve(const QPolygonF &c);
    QPolygonF curve() const { return m_pts; }
    QSize sizeHint() const override { return {200, 200}; }
signals:
    void curveChanged(const QPolygonF &c);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
private:
    QPointF toCurve(const QPointF &w) const;
    QPointF toWidget(const QPointF &c) const;
    QRectF area() const;
    QPolygonF m_pts;
    int m_drag = -1;
    QVector<int> m_hist;
    QColor m_histColor;
public:
    // v0.3.0: the image's histogram for the edited channel, drawn behind the curve
    void setHistogram(const QVector<int> &h, const QColor &c) { m_hist = h; m_histColor = c; update(); }
};

// Histogram display for the Levels dialog (with input black / gamma / white markers).
class HistogramView : public QWidget {
    Q_OBJECT
public:
    explicit HistogramView(QWidget *parent = nullptr);
    void setData(const QVector<int> &hist, const LevelsChannel &lc);
    QSize sizeHint() const override { return {256, 90}; }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QVector<int> m_hist;
    LevelsChannel m_lc;
};

// Thumbnail of the image being adjusted; clicking it samples a colour (Levels eyedroppers).
class SamplePicker : public QWidget {
    Q_OBJECT
public:
    explicit SamplePicker(const QImage &img, QWidget *parent = nullptr);
    QSize sizeHint() const override;
signals:
    void picked(const QColor &c);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
private:
    QRect target() const;
    QImage m_img;
};

class EffectDialog : public QDialog {
    Q_OBJECT
public:
    // `sample` (optional) is the unadjusted image: Levels uses it for the histogram and eyedroppers.
    EffectDialog(const LiveEffect &e, QWidget *parent = nullptr, const QImage &sample = QImage());
    LiveEffect effect() const { return m_e; }
    enum Dropper { NoDropper, BlackPoint, GreyPoint, WhitePoint };
    void applyDropper(Dropper d, const QColor &c); // Levels eyedroppers (public for tests)
    bool previewEnabled() const;
signals:
    void changed(const LiveEffect &e); // the effect to preview (the original when Preview is off)
private:
    void emitChange();
    void syncChannel();
    void updateRangePreview();   // Replace Color: the grey colour-range preview
    LiveEffect m_e, m_orig;
    QImage m_sample;
    QCheckBox *m_preview = nullptr;
    int m_channel = 0;
    Dropper m_dropper = NoDropper;
    HistogramView *m_hist = nullptr;
    CurveEditor *m_curve = nullptr;
    QList<QDoubleSpinBox *> m_levelSpins; // inBlack, gamma, inWhite, outBlack, outWhite
    QList<QDoubleSpinBox *> m_balanceSpins;
    int m_range = 1;
    bool m_syncing = false;
    class QLabel *m_rcMask = nullptr;
};

bool effectHasSettings(EffectType t);

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    // area: the Export Area tool's rectangle (document px); when valid an "Export area only" box
    // (checked) limits the preview and options().area to it
    ExportDialog(const Document &doc, QWidget *parent = nullptr, const QRect &area = QRect());
    ExportOptions options() const;
    QRect area() const;          // the area in use (empty: the whole canvas)
    QSize outputSize() const;    // area (or canvas) times the export scale
    // Raster image formats can be copied; C Header, PDF and layered documents cannot.
    static bool canCopyFormat(Fmt f);
private:
    void updatePreview();
    void copyToClipboard();
    void updateCopyButton();
    void placeCopyAfterExport();
    const Document &m_doc;
    QRect m_area;
    class QCheckBox *m_areaOnly = nullptr;
    class QSpinBox *m_scale = nullptr;
    class QComboBox *m_resample = nullptr;
    OptimizeWidget *m_opt;
    class PreviewView *m_view;
    class PreviewPane *m_pane;
    QLabel *m_zoomLabel;
    class QDialogButtonBox *m_buttons = nullptr;
    class QPushButton *m_exportBtn = nullptr;
    class QPushButton *m_copyBtn = nullptr;
    QLabel *m_copyStatus = nullptr;
    QImage m_previewImage;       // last successful encode, for setImage
    QByteArray m_previewEncoded; // last successful file bytes
    bool m_previewOk = false;
    bool m_fitted = false;
public:
    PreviewPane *previewPane() const { return m_pane; }
    PreviewView *previewView() const { return m_view; }
protected:
    void keyPressEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
};

namespace dialogs {
void showShortcuts(QWidget *parent);
}

// File > Export HTML and Images
class HtmlExportDialog : public QDialog {
    Q_OBJECT
public:
    HtmlExportDialog(const QString &suggestedPath, QWidget *parent = nullptr);
    QString path() const;
    html::Options options() const;
private:
    class QLineEdit *m_path, *m_images;
    QComboBox *m_layout;
};

// View > Grid > Edit Grid...: spacing, colour, show and snap of the current document's grid.
// Apply previews live; Cancel restores the grid as it was.
class EditGridDialog : public QDialog {
    Q_OBJECT
public:
    EditGridDialog(Document *doc, QWidget *parent = nullptr);
    GridSettings settings() const;
    void apply();
    void reject() override;
    void accept() override;
private:
    Document *m_doc;
    GridSettings m_orig;
    QSpinBox *m_h, *m_v;
    ColorButton *m_color;
    QCheckBox *m_show, *m_snap;
};

// View > Guides > Edit Guides...
class EditGuidesDialog : public QDialog {
    Q_OBJECT
public:
    EditGuidesDialog(Document *doc, QWidget *parent = nullptr);
    void accept() override;
private:
    Document *m_doc;
    ColorButton *m_color;
    QCheckBox *m_show, *m_snap, *m_lock;
    bool m_clear = false;
};

// Copy selected objects onto other frames (All / Previous / Next / a 1-based range).
class CopyToFramesDialog : public QDialog {
    Q_OBJECT
public:
    CopyToFramesDialog(int frameCount, int currentFrame, QWidget *parent = nullptr);
    // 0-based target frames; the current frame is omitted.
    QVector<int> targets() const;
private:
    int m_count, m_cur;
    QRadioButton *m_all, *m_prev, *m_next, *m_range;
    QSpinBox *m_from, *m_to;
};

// Ask for a target size in KB, search settings off the UI thread, apply them to `io`.
// Returns false if the user cancelled. On success `io` is updated; if the target could not
// be met a message is shown and the smallest settings are still applied.
bool runOptimizeToSizeWizard(QWidget *parent, const Document &doc, ExportOptions &io, int frame);
