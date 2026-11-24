#pragma once
#include "core/Procedural.h"
#include <QDialog>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <atomic>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTimer;
class QWidget;
class GradientBar;
class ColorButton;

// Filters > Render > Texture... and the Property inspector's "Procedural..." Texture / Pattern
// entries: every procedural generator with its parameters, a colour ramp and a live preview
// rendered off the UI thread at a capped resolution.
class ProceduralDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Texture, Pattern, Render };
    // `target` is the render size for Mode::Render (the tile size is edited for the fill modes)
    ProceduralDialog(const proc::Params &initial, Mode mode, QSize target = {}, QWidget *parent = nullptr);
    ~ProceduralDialog() override;
    proc::Params params() const { return m_p; }
    void setParams(const proc::Params &p);

    static constexpr int kPreviewCap = 256;
    // the preview's parameters: size capped to `cap` px, cell size scaled with it
    static proc::Params previewParams(const proc::Params &p, QSize size, int cap = kPreviewCap);
    // test hooks
    QImage previewImage() const { return m_preview; }
    bool previewBusy() const;
    bool waitForPreview(int ms = 10000);
    int previewsRendered() const { return m_rendered; }

    // Render mode: "Preview" -- show the texture on the canvas (MainWindow applies it)
    bool canvasPreview() const;
signals:
    void previewUpdated();
    void canvasPreviewToggled(bool on);

private:
    void build();
    void syncWidgets();
    void readWidgets();
    void changed();
    void startPreview();
    void showPreview();
    QSize renderSize() const;

    Mode m_mode;
    QSize m_target;
    proc::Params m_p;
    bool m_sync = false;
    QImage m_preview;
    int m_rendered = 0;
    bool m_pending = false;
    bool m_unshown = false;  // a render was started and its result is not on screen yet
    std::shared_ptr<std::atomic<bool>> m_cancel;
    QFutureWatcher<QImage> m_watch;
    QTimer *m_debounce;

    QComboBox *m_gen, *m_preset, *m_basis, *m_variant, *m_metric, *m_pattern, *m_lifePreset, *m_rampPreset;
    QSpinBox *m_seed, *m_w, *m_h, *m_octaves, *m_cell, *m_gens, *m_rule, *m_states, *m_threshold, *m_range;
    QDoubleSpinBox *m_scale, *m_contrast, *m_bright, *m_pers, *m_lac, *m_warp, *m_width2, *m_density, *m_feed, *m_kill;
    QCheckBox *m_canvasPv = nullptr;
    QCheckBox *m_tile, *m_invert, *m_single, *m_moore;
    QLineEdit *m_life;
    GradientBar *m_ramp;
    ColorButton *m_stateCol[3];
    QLabel *m_previewLabel, *m_info;
    QHash<QString, QList<QWidget *>> m_groups;   // usedParams key -> its rows
};
