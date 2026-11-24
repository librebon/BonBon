#pragma once
#include "core/Document.h"
#include <atomic>
#include <QTimer>
#include <QWidget>

class Canvas;
class QTabBar;
class QStackedWidget;

struct EncodeResult {
    QImage image;       // decoded result (current frame)
    QByteArray encoded; // exact file bytes of the export (may be empty on error)
    qint64 bytes = 0;
    int frames = 0;
    QString error;
    double ms = 0;
    qint64 originalBytes = 0;   // Tiny compression: the same export without it
    QVector<QRgb> colorTable;   // GIF / PNG 8 / Tiny: the colours of the encoded palette
    QStringList notes;          // approximations (SVG / layered formats)
    qint64 pixelBytes = -1;     // C Header: bytes in the pixel / palette arrays
    QString detail;             // C Header: element type, identifier
};
// Encode the document with `o` and decode it again, as an export would produce it.
EncodeResult encodePreview(const Document &doc, const ExportOptions &o, int frame, QByteArray *reuseBuf = nullptr);

// Search export settings so the encoded size is at most targetBytes, keeping the current format.
// Quality-based formats (JPEG, lossy WebP/AVIF/HEIF/JXL) binary-search quality; indexed GIF/PNG 8
// search colour count then dither; lossless formats search compression/effort. The largest
// "quality" that still fits is kept. If nothing fits, the smallest settings are returned and
// metTarget is false. cancel, if set, is checked between trial encodes.
struct OptimizeToSizeResult {
    ExportOptions options;
    EncodeResult encoded;
    bool metTarget = false;
    bool cancelled = false;
};
OptimizeToSizeResult optimizeToSize(const Document &doc, const ExportOptions &o, int frame, qint64 targetBytes,
                                    std::atomic<bool> *cancel = nullptr);
QString describeOptions(const ExportOptions &o);
QString describeSize(qint64 bytes);
// "123.4K (from 456.7K, 73 % saved)" for Tiny compression
QString describeSaving(const EncodeResult &r);

// Shared zoom/scroll state of the export preview panes (Preview, 2-Up, 4-Up, Export Preview):
// every pane shows `center` (document coordinates) in its middle at `zoom`, so panes stay in sync.
class PreviewView : public QObject {
    Q_OBJECT
public:
    explicit PreviewView(QSize docSize, QObject *parent = nullptr) : QObject(parent), m_doc(docSize), m_center(docSize.width() / 2.0, docSize.height() / 2.0) {}
    double zoom() const { return m_zoom; }
    QPointF center() const { return m_center; }
    QSize docSize() const { return m_doc; }
    void setDocSize(QSize s) { m_doc = s; }
    QPointF origin(QSize pane) const { return QPointF(pane.width() / 2.0, pane.height() / 2.0) - m_center * m_zoom; }
    QPointF toDoc(QPointF panePt, QSize pane) const { return (panePt - origin(pane)) / m_zoom; }
    void setView(double z, QPointF center);
    void zoomAt(double z, QPointF panePt, QSize pane); // keep the document point under panePt fixed
    void zoomIn(QPointF panePt = {-1, -1}, QSize pane = {});
    void zoomOut(QPointF panePt = {-1, -1}, QSize pane = {});
    void zoomToRect(const QRectF &docRect, QSize pane);
    void fit(QSize pane);
    void actualSize();
    void panBy(QPointF viewDelta) { setView(m_zoom, m_center - viewDelta / m_zoom); }
signals:
    void changed();
    void zoomChanged(double z);
private:
    QSize m_doc;
    double m_zoom = 1;
    QPointF m_center;
};

class PreviewPane : public QWidget {
    Q_OBJECT
public:
    PreviewPane(PreviewView *view, QWidget *parent = nullptr);
    void setResult(const QImage &img, const QString &caption);
    void setSelected(bool on);
    bool isSelected() const { return m_selected; }
    void setHint(const QString &h) { if (h != m_hint) { m_hint = h; update(); } }   // a one-time tip over the caption
    QString hint() const { return m_hint; }
    QString caption() const { return m_caption; }
    QSize imageArea() const { return QSize(width(), qMax(1, height() - kCaption)); }
    PreviewView *view() const { return m_view; }
    static constexpr int kCaption = 34;
    // zoom keys (Ctrl+= / Ctrl++ / Ctrl+- incl. keypad, Ctrl+0 fit, Ctrl+1 100%); true if handled
    static bool handleZoomKey(QKeyEvent *e, PreviewView *v, QSize pane);
protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
signals:
    // any left-button press (whatever the tool, Space or later movement) or Tab focus: selects
    // the pane. Selecting only picks which pane the Optimize panel edits; it never changes settings.
    void clicked();
private:
    void updateCursor();
    PreviewView *m_view;
    QImage m_img;
    QString m_caption, m_hint;
    bool m_selected = false;
    QPointF m_last, m_press;
    enum class Drag { None, Pan, ZoomRect } m_drag = Drag::None;
    QRectF m_zoomRect;  // pane coordinates
    bool m_space = false;
};

class DocumentView : public QWidget {
    Q_OBJECT
public:
    explicit DocumentView(Document *doc, QWidget *parent = nullptr);
    ~DocumentView() override;
    Document *doc() const { return m_doc; }
    Canvas *canvas() const { return m_canvas; }
    int mode() const { return m_mode; }
    void setMode(int m);              // 0 Original, 1 Preview, 2 2-Up, 3 4-Up
    void scheduleRefresh();
    // zoom commands act on the canvas (Original) or on the shared preview view
    double zoom() const;
    void zoomIn();
    void zoomOut();
    void setZoom(double z);
    void fit();
    void fitSelection();   // zoom to the selection (Fit All when nothing is selected)
    void actualSize();
    PreviewView *previewView() const { return m_view; }
    PreviewPane *previewPane(int mode, int index) const;
    void refresh();  // re-encode previews now
signals:
    void modeChanged(int m);
    void zoomChanged(double z);
    void paneSelected(int pane);   // 2-Up/4-Up optimised pane (0..2) chosen; the Optimize panel follows it
private:
    Document *m_doc;
    Canvas *m_canvas;
    QWidget *m_corner;
    class Ruler *m_hRuler, *m_vRuler;
public:
    Ruler *ruler(Qt::Orientation o) const { return o == Qt::Horizontal ? m_hRuler : m_vRuler; }
private:
    QTabBar *m_tabs;
    QStackedWidget *m_stack;
    PreviewView *m_view;
    QSize paneArea() const;
    PreviewPane *m_single;
    PreviewPane *m_two[2];
    PreviewPane *m_four[4];
    void selectPane(int pane);
    void updateSelection();
public:
    void resetPaneOptions();       // derive the 4-Up variants again from the selected settings
private:
    QTimer m_timer;
    int m_mode = 0;
};
