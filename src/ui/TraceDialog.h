#pragma once
// Modify > Trace Bitmap... (v0.3.1): options for core/Trace with a live preview traced on a worker
// thread (debounced, cancellable), and the path / node count of the current settings.
#include "core/Trace.h"
#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QWidget;

class TraceDialog : public QDialog {
    Q_OBJECT
public:
    // Palette choices for the colours mode
    enum PaletteChoice { PalAdaptive = 0, PalImage = 1, PalWeb = 2, PalGray = 3 };

    explicit TraceDialog(const QImage &src, QWidget *parent = nullptr);
    ~TraceDialog() override;

    trace::Options options() const;           // from the widgets
    void setOptions(const trace::Options &o, int paletteChoice = PalAdaptive);
    trace::Insert insertOptions() const;
    void setInsertOptions(const trace::Insert &i);
    int paletteChoice() const;

    // The result for the current options: the preview's when it is up to date, else traced now.
    trace::Result result();
    bool previewBusy() const;
    bool waitForPreview(int ms = 10000);
    int previewsRendered() const { return m_rendered; }
    QString statsText() const;
    QImage previewImage() const { return m_shown; }

    static QVector<QRgb> paletteFor(int choice, const QImage &img, int colors);
signals:
    void previewUpdated();
private:
    void build();
    void changed();
    void startPreview();
    void showPreview();
    void paintPreview();
    void syncVisibility();

    QImage m_src;            // ARGB32
    QImage m_shown;          // last preview picture
    QComboBox *m_mode = nullptr, *m_palette = nullptr, *m_layout = nullptr;
    QStackedWidget *m_pages = nullptr;
    QSpinBox *m_threshold = nullptr, *m_colors = nullptr, *m_smoothing = nullptr, *m_turd = nullptr;
    QCheckBox *m_invert = nullptr, *m_optimize = nullptr, *m_ignoreAlpha = nullptr, *m_removeBg = nullptr;
    QCheckBox *m_group = nullptr, *m_keep = nullptr, *m_live = nullptr, *m_showSrc = nullptr;
    QCheckBox *m_asGroup = nullptr;   // v0.3.2: output the paths as one group
    QDoubleSpinBox *m_alpha = nullptr, *m_tol = nullptr;
    QWidget *m_curveBox = nullptr;
    QLabel *m_preview = nullptr, *m_stats = nullptr;
    QTimer *m_debounce = nullptr;
    QFutureWatcher<trace::Result> m_watch;
    std::shared_ptr<std::atomic_bool> m_cancel;
    trace::Options m_running;     // options of the render in flight
    trace::Result m_result;       // last finished preview
    trace::Options m_resultOpts;
    bool m_hasResult = false, m_pending = false, m_unshown = false, m_sync = false;
    int m_rendered = 0;
};
