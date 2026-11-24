#pragma once
#include "core/Stroke.h"
#include "core/BrushEngine.h"
#include <QDialog>
#include <QPair>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QDoubleSpinBox;
class PreviewStrokeWidget;

// Fireworks-style Edit Stroke dialog: Options / Shape / Sensitivity, with a
// left-to-right pressure/speed preview at the bottom of every tab.
class EditStrokeDialog : public QDialog {
    Q_OBJECT
public:
    explicit EditStrokeDialog(QWidget *parent = nullptr);
    void setParams(const stroke::Params &p, const QColor &color, double width, quint32 seed = 0);
    void setLibrarySymbols(const QVector<QPair<quint64, QString>> &syms);
    // Vector strokes have no target bitmap to sample: the two Cellular-under options
    // are shown disabled with a tooltip. The bitmap Brush keeps them enabled.
    void setVectorStroke(bool on);
    // When set, the preview uses the Brush dab / path engine with these params
    // (updated from the dialog fields). Cleared by setVectorStroke(true).
    void setBrushSource(const brush::Params &bp);
    stroke::Params params() const { return m_p; }
    // Last rendered preview (for tests). May be null before the dialog is shown.
    QImage previewImage() const;
    // Preview background: 0 White, 1 Black, 2 Auto (default).
    // Auto uses the rendered stroke's alpha-weighted luminance, falling back to the palette colour
    // when the preview has no paint (so a mostly-white look such as Lightning gets black).
    int previewBackgroundMode() const { return m_previewBg; }
    void setPreviewBackgroundMode(int mode);
    QColor previewBackground() const;
    static QColor previewBackgroundFor(int mode, const QColor &stroke);
    static QColor previewBackgroundFor(int mode, const QColor &stroke, const QImage &rendered);

signals:
    void applied(const stroke::Params &p);

private:
    void syncFromParams();
    void syncToParams();
    void refreshPreview();
    void syncPreviewBgButton();
    stroke::Sensitivity &sensFor(int prop);
    const stroke::Sensitivity &sensFor(int prop) const;

    stroke::Params m_p;
    QColor m_color = Qt::black;
    double m_width = 8;
    bool m_updating = false;

    QSpinBox *m_ink = nullptr, *m_spacing = nullptr, *m_flow = nullptr, *m_texAmt = nullptr;
    QSpinBox *m_edgeTex = nullptr, *m_tips = nullptr, *m_tipSpacing = nullptr;
    QCheckBox *m_buildUp = nullptr, *m_square = nullptr;
    QComboBox *m_colorVar = nullptr, *m_texture = nullptr;
    QSpinBox *m_dashOn[3] = {}, *m_dashOff[3] = {};
    QWidget *m_dashRow = nullptr;
    QSpinBox *m_sprayDensity = nullptr, *m_spraySpread = nullptr;
    QCheckBox *m_sprayDrips = nullptr;
    QDoubleSpinBox *m_size = nullptr;
    QSpinBox *m_edge = nullptr, *m_aspect = nullptr, *m_angle = nullptr;
    QComboBox *m_prop = nullptr;
    QSpinBox *m_pressure = nullptr, *m_speed = nullptr, *m_horizontal = nullptr, *m_vertical = nullptr, *m_random = nullptr;
    PreviewStrokeWidget *m_preview = nullptr;
    class QToolButton *m_previewBgBtn = nullptr;
    int m_previewBg = 2; // 0 White, 1 Black, 2 Auto (default)
    QColor m_autoPreviewBg = QColor(250, 250, 248);
    bool m_autoPreviewBgSet = false;
    QWidget *m_stampRow = nullptr;
    class QLineEdit *m_stampEdit = nullptr;
    class QComboBox *m_stampSymbol = nullptr;
    QVector<QPair<quint64, QString>> m_library;

    QWidget *m_cellBox = nullptr;
    class QComboBox *m_cellRule = nullptr;
    class QLineEdit *m_cellCustom = nullptr;
    QSpinBox *m_cellSize = nullptr, *m_cellSteps = nullptr, *m_cellDensity = nullptr;
    class ColorButton *m_cellAgeColor = nullptr;
    QCheckBox *m_cellSampleUnder = nullptr, *m_cellFollowEdges = nullptr;
    bool m_vectorStroke = false;
    bool m_brushMode = false;
    brush::Params m_brush;
    quint32 m_seed = stroke::kPreviewSeed;
};
