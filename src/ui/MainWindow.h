#pragma once
#include "AppState.h"
#include "core/Document.h"
#include <QHash>
#include <QMainWindow>
#include <functional>
#include <QPointer>
class ZoomField;

class QTabWidget;
class QLabel;
class QAction;
#include <QMenu>
class QActionGroup;
class DocumentView;
class ToolsPanel;
class PropertyInspector;
class CollapsiblePanel;
class PanelStack;
class LayersPanel;
class FramesPanel;
class HistoryPanel;
class OptimizePanel;
class AlignPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    DocumentView *addDocument(Document *doc);
    bool openFile(const QString &path);
    DocumentView *currentView() const;
    Document *currentDoc() const;
    // Modify > Transform > Numeric Transform: mode 0 Scale (a, b in %), 1 Resize (a, b = new
    // width / height in px), 2 Rotate (a = degrees clockwise), 3 Skew (a, b = horizontal /
    // vertical skew angles in degrees). About the centre of the selection; one undo step.
    bool applyNumericTransform(int mode, double a, double b = 0, bool scaleAttrs = true);
    void numericTransform();
    bool removeTransformations();
    // Modify > Canvas > Rotate Canvas: any angle (clockwise); the canvas grows to hold the result
    bool rotateCanvas(double degrees);
    bool saveDocument(Document *doc, bool saveAs);
    bool exportTo(Document *doc, const QString &path, const ExportOptions &o);
    QAction *action(const QString &id) const { return m_actions.value(id); }
    // [ / ] (dir -1 / +1): tip size of the current tool, or the stroke of the selected vectors
    // with the Pointer; softness = Shift ({ / }) changes the tip edge instead. Returns false if
    // nothing applies (no tip, or a text field has focus).
    bool adjustTipSize(int dir, bool softness = false);
    static double stepTipSize(double v, int dir);
    QList<QString> actionIds() const { return m_actions.keys(); }
    CollapsiblePanel *panel(const QString &title) const;
    class FindPanel *findPanel() const { return m_find; }   // Window > Find & Replace
    QVector<Document *> documents() const;                  // open documents in tab order
    QStringList recentFiles() const;
    void editPreferences();
    // the canvas right-click menu (Fireworks MX 2004 style): built for the document's selection;
    // property "context" = vector / bitmap / text / symbol / multiple / pixels / canvas
    QMenu *buildCanvasContextMenu(Document *d);
    void showCanvasContextMenu(const QPoint &globalPos);
    QMenu *lastContextMenu() const { return m_ctxMenu; }
    bool pasteAttributes();   // Edit > Paste Attributes
public slots:
    void refreshToolShortcutLabels(); // tips / flyout text from the shortcut registry

protected:
    bool event(QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    QAction *act(const QString &id, const QString &text, const QKeySequence &key, std::function<void()> fn, const QString &icon = {});
    void buildMenus();
    void addRecent(const QString &path);
    void rebuildRecent();
    QStringList m_recent;
    QMenu *m_recentMenu = nullptr;
    QPointer<QMenu> m_ctxMenu;
    void buildDocks();
    void currentChanged();
    void syncToolEnvironmentTo(Document *d); // write AppState → d->toolEnv
    void applyToolEnvironmentFrom(Document *d); // d->toolEnv → AppState (defaults if empty)
    void updateActions();
    void updateTitle();
    bool closeTab(int index);
    bool maybeSave(Document *doc);

    // commands
    void fileNew();
    void fileOpen();
    void fileImport();
    void fileExport(const QRect &area = QRect());          // area: Export Area / Export Selection Area (document px)
    void fileExportPreview(const QRect &area = QRect());
    void fileExportSelectionArea();
    void fileExportSelected();                        // File > Export Selection Area...
    void fileExportHtml();
    void insertWebObjects(ObjType type);
    void syncWebFeatureUi();
    void exportFrames();
    void editCopy(bool cut);
    void editPaste(bool inPlace = false);
    // Pixel-selection copy (Fireworks): the selected pixels of the selected bitmaps (or, `merged`,
    // of everything visible), masked by the selection (feather included) and cropped to its
    // bounds. Returns false when there is no pixel selection or nothing under it.
    bool copyPixels(Document *d, bool merged, VObject *out, QVector<VObject *> *sources = nullptr);
    void clearPixels(Document *d, const QVector<VObject *> &targets);
    void bitmapVia(bool cut);
    void editDuplicate(bool clone);
    void canvasSize();
    void imageSize();
    void fitCanvas();
    void fitCanvasToWindow();
    void trimCanvas();
    void transform(int how);
    void convertToBitmap();
public:
    // v0.3.2 groups and masks (Modify > Group / Ungroup / Mask, Convert to Bitmap); commit with a label
    enum class GroupCmd { Group, Ungroup, PasteAsMask, PasteInside, GroupAsMask, RevealAll, HideAll,
                          DisableMask, EnableMask, ToggleLink, DeleteMask, ConvertToBitmap };
    bool groupCommand(GroupCmd cmd);
private:
    QVector<VObject> clipboardObjects(Document *d);
    void mergeDown();
    void flattenLayers();
    void effectFilter(int effectType);
    QVector<VObject *> filterTargets(Document *d);
public:
    // Symbols (Modify > Symbol). editSymbol opens (or switches to) the symbol's editor tab.
    Document *editSymbol(Document *parent, quint64 symId);
    bool isSymbolEditor(Document *d) const { return m_symbolEdits.contains(d); }
    void convertToSymbol();
    void tweenInstancesDialog();
    void animateSelectionDialog();
    void alterPathDialog(int which); // 0 simplify, 1 expand stroke, 2 inset path
    bool applyEffectFilter(const LiveEffect &e); // destructive, no dialog (also used by tests)
    // v0.3.0 -------------------------------------------------------------------------------------
    // The filter dialog (live preview) for `e` as given (e.g. a preset); false = cancelled / no target.
    bool effectFilterWith(const LiveEffect &e, const QString &title = QString());
    // Filters > Distort > Liquify: `script` (tests) drives the dialog instead of exec() and accepts it.
    bool liquifyFilter(const std::function<void(class LiquifyDialog &)> &script = {});
    // Filters > Other > Content-Aware Fill: fills the pixel selection of the selected bitmaps (PatchMatch).
    bool contentAwareFill(quint32 seed = 1);
    // Modify > Trace Bitmap... (v0.3.1): the single selected bitmap -> vector paths, one undo step.
    // `script` (tests) drives the dialog instead of exec(); returning false cancels.
    bool traceBitmap(const std::function<bool(class TraceDialog &)> &script = {});
    // Filters > Pixel Art > Upscale... (v0.3.1): the selected bitmaps enlarged by a whole factor with
    // a pixel-art scaler, one undo step. `script` (tests) drives the dialog instead of exec().
    bool pixelUpscale(const std::function<bool(class PixelUpscaleDialog &)> &script = {});
    // Modify > Indexed Color
    bool convertToIndexed();
    // `palette` non-null: no dialog (scripts / tests)
    bool editPalette(const QVector<QRgb> *palette = nullptr);
    // File > Export Slices / Export Image Map; empty path = ask
    bool exportSlices(const QString &dir = QString());
    bool exportImageMap(const QString &htmlPath = QString());
    // File > Batch Process...
    void batchProcess();
    void makeSeamlessTile(double blendWidth = -1); // < 0: ask (dialog with live preview)
    // Filters > Render > Texture: render a procedural texture into the selected bitmaps (within the
    // pixel selection) or into a new bitmap; `paramsJson` non-empty = no dialog (scripts / tests)
    bool renderTexture(const QString &paramsJson = {});
    // ---- Select menu (v0.2.3). amount < 0: ask in a dialog with a live preview.
    enum SelOp { SelFeather, SelExpand, SelContract, SelBorder, SelSmooth };
    bool modifySelection(int op, double amount = -1);
    bool selectSimilar();
    bool saveSelection(const QString &name = QString());          // empty: ask for a name
    bool restoreSelection(int index, Qt::KeyboardModifiers mods = {});
    void manageSelections();                                       // the saved selections list
    int convertMarqueeToPath();
    bool quickMaskActive(Document *d = nullptr) const;
    void toggleQuickMask();
    void selectInverse();
    void updateOptimizeTitle();
    void fileExportFavicons();                                     // File > Export > Favicon / App Icons...
private:
    void showNotes(const QString &title, const QString &intro, const QStringList &notes);
    QPointer<Document> m_sliderMergeDoc;   // a pop-up slider drag folds its updates into one undo step
    void textStyle(int what);

    QTabWidget *m_tabs;
    ToolsPanel *m_toolsPanel;
    Document *m_envDoc = nullptr; // document whose toolEnv is currently mirrored in AppState
    PropertyInspector *m_pi;
    LayersPanel *m_layers;
    FramesPanel *m_frames;
    HistoryPanel *m_history;
    OptimizePanel *m_optimize;
    AlignPanel *m_align;
    class StylesPanel *m_styles = nullptr;
    class LibraryPanel *m_library = nullptr;
    class FindPanel *m_find = nullptr;
    // symbol editors: a tab whose document holds the symbol's frames; every change is pushed back
    struct SymbolEdit { QPointer<Document> parent; quint64 symId = 0; int pushedIndex = -1; };
    QHash<Document *, SymbolEdit> m_symbolEdits;
    void pushSymbolEdit(Document *editor);
    QList<CollapsiblePanel *> m_panels;
    PanelStack *m_panelStack = nullptr;
    QHash<QString, QAction *> m_actions;
    QActionGroup *m_viewModes;
    QLabel *m_statusPos, *m_statusSize;
    ZoomField *m_statusZoom;
public:
    ZoomField *zoomField() const { return m_statusZoom; }
private:
    QString m_lastDir;
    QStringList m_lastNotes;
    int m_untitled = 0;
};
