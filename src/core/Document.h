#pragma once
#include "Object.h"
#include "PixelEdit.h"
#include <QHash>
#include <QJsonObject>
#include "io/ImageIO.h"
#include <QObject>
#include <QSet>

struct LayerInfo {
    quint64 id = 0;
    QString name;
    bool visible = true;
    bool locked = false;
    bool shared = false;   // same objects on every frame ("Share Layer Across Frames")
    bool web = false;      // the Web Layer: slices and hotspots, never rendered into pixels
};

struct Frame {
    QString name;
    int delayMs = 100;
    QVector<QVector<VObject>> layers; // parallel to DocState::layers, bottom layer first
};

// Per-document view aids, saved in .lwd but not part of the undo history (as in Fireworks).
struct GridSettings {
    int x = 20, y = 20;                    // horizontal / vertical spacing in px (>= 1)
    QColor color = QColor(0, 120, 255, 60);
    bool show = false, snap = false;
    bool operator==(const GridSettings &o) const { return x == o.x && y == o.y && color == o.color && show == o.show && snap == o.snap; }
};
struct GuideSettings {
    QVector<double> v;                     // vertical guides: x positions (dragged from the left ruler)
    QVector<double> h;                     // horizontal guides: y positions (dragged from the top ruler)
    QColor color = QColor(0x22, 0xCC, 0x44);
    bool show = true, lock = false, snap = true;
    bool operator==(const GuideSettings &o) const { return v == o.v && h == o.h && color == o.color && show == o.show && lock == o.lock && snap == o.snap; }
};

struct DocState {
    int width = 500, height = 500;
    QColor canvasColor = Qt::white;   // alpha 0 = transparent canvas
    QVector<LayerInfo> layers;
    QVector<Frame> frames;
    int loops = 0;                    // 0 = forever
    QVector<Symbol> symbols;          // the document Library
    double dpi = 72;                  // v0.3.1: resolution, pixels/inch (72 = Fireworks default; not written to exports)
};

class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(QObject *parent = nullptr);
    ~Document() override = default;
    static Document *createNew(int w, int h, const QColor &canvas, QObject *parent = nullptr, double dpi = 72);
    static Document *createSample(QObject *parent = nullptr);
    // Build a document from imported image frames (one bitmap object per frame).
    static Document *fromImageSeq(const ImageSeq &seq, QObject *parent = nullptr);
    // A detached copy of what rendering and exporting read (s, the current frame/layer, file name,
    // export settings) for read-only work on another thread (the Optimize estimate): no parent, no
    // thread affinity, no history, no selection. The bitmaps are shared (implicit sharing), not copied.
    Document *snapshot() const;

    DocState s;
    int curFrame = 0;
    int curLayer = 0;
    QSet<quint64> selection;
    QPainterPath pixelSelection;      // marquee/lasso selection in canvas coordinates
    // Edge of the pixel selection (marquee / lasso / wand "Edge" option of the last operation)
    enum SelEdge { SelHard = 0, SelAntiAlias = 1, SelFeather = 2 };
    int selectionEdge = SelHard;
    double selectionFeather = 0;      // px (SelFeather)
    // Combine `p` with the current pixel selection, Fireworks style: Shift adds, Alt subtracts,
    // Shift+Alt intersects, no modifier replaces. Emits selectionChanged.
    void combinePixelSelection(const QPainterPath &p, Qt::KeyboardModifiers mods, int edge = SelHard, double feather = 0);
    // The selection as a coverage mask (Alpha8, `size`), document -> image via `docToImg`; hard,
    // anti-aliased or feathered per selectionEdge. Null image when there is no pixel selection.
    QImage selectionMask(const QTransform &docToImg, QSize size) const;
    // ---- bitmap (raster) selections, v0.2.3. Feather / Expand / Quick Mask etc. turn the selection
    // into an 8-bit mask in canvas pixels; pixelSelection then holds its outline (marching ants).
    // The mask applies while pixelSelection is still that outline: anything that replaces
    // pixelSelection (a new marquee, Deselect) drops it.
    QImage selMask;                   // Alpha8, 1 px per canvas px
    QPoint selMaskOrigin;             // canvas position of selMask(0,0)
    QPainterPath selMaskOutline;
    bool hasMaskSelection() const { return !selMask.isNull() && !pixelSelection.isEmpty() && pixelSelection == selMaskOutline; }
    // canvas-pixel rectangle that holds every (even partly) selected pixel
    QRect pixelSelectionBounds() const;
    // the selection as a canvas-space mask covering `area` (canvas px)
    QImage canvasSelectionMask(const QRect &area) const;
    // replace the selection by a mask (canvas px at `origin`); trims it, updates the outline, emits selectionChanged
    void setSelectionMask(const QImage &mask, QPoint origin);
    void clearPixelSelection();
    // Is canvas point `p` selected (>= 50 %)?
    bool selectionContains(const QPointF &p) const;
    void moveSelection(const QPointF &by);
    // Save / Restore Bitmap Selection: named masks kept in the .lwd (not part of undo)
    struct SavedSelection { QString name; QImage mask; QPoint origin; };
    QVector<SavedSelection> savedSelections;
    void markViewDirty() { m_viewDirty = true; emit historyChanged(); }
    QString filePath;
    // Onion skinning (Frames panel): 0 off, 1 next frame, 2 before and after, 3 all frames.
    // View state only: not saved and not part of the undo history.
    int onionMode = 0;
    int onionOpacity = 40;              // % for the nearest frame (further frames fade out)
    QVector<int> onionFrames() const;   // frames drawn dimmed behind the current one
    void setOnion(int mode, int opacity = -1) { onionMode = mode; if (opacity >= 0) onionOpacity = opacity; emit changed(); }
    GridSettings grid;                  // initialised from the Preferences grid defaults
    GuideSettings guides;
    // v0.3.7 / .lwd v5: per-document tool options, active tool, colours.
    // Not part of undo; changing them does not mark the document dirty.
    QJsonObject toolEnv;
    // change grid or guides: repaint, and the document needs saving
    void setGrid(const GridSettings &g) { grid = g; grid.x = qMax(1, grid.x); grid.y = qMax(1, grid.y); m_viewDirty = true; emit viewSettingsChanged(); emit historyChanged(); }
    void setGuides(const GuideSettings &g) { guides = g; m_viewDirty = true; emit viewSettingsChanged(); emit historyChanged(); }
    QString untitledName;             // display name while filePath is empty
    QString title() const;
    QRect exportArea;             // Export Area tool: the last rectangle drawn (session only, never saved)
    ExportOptions exportOptions;  // settings of the selected preview pane (what File > Export uses)
    // 2-Up / 4-Up: each optimised preview pane owns its settings (as in Fireworks). Pane i (0..2)
    // is the i-th optimised pane; the selected pane's settings live in exportOptions, so
    // paneOptions[selectedPane] may be stale -- use paneOption(). Empty until a multi-pane
    // view is first opened, then derived once from exportOptions.
    QVector<ExportOptions> paneOptions;
    int selectedPane = 0;
    static constexpr int kPanes = 3;
    static QVector<ExportOptions> defaultPaneOptions(const ExportOptions &base);
    void ensurePaneOptions();
    void resetPaneOptions();               // derive the variants again from the selected settings
    ExportOptions paneOption(int i) const;
    void selectPane(int i);                // stores the current settings, makes pane i current

    // ---- history (snapshot based; QImage is implicitly shared so snapshots are cheap)
    struct HistEntry {
        DocState st; QString label;
        QVector<QPair<qint64, qint64>> images;   // (QImage::cacheKey, bytes) of the bitmaps this step holds
        bool counted = false;
        QByteArray digest;                       // historyDigest(st); filled on commit, compared next time
        QVector<QByteArray> frameKeys;           // per-frame layer keys, reused when QVectors still share
    };
    const QVector<HistEntry> &history() const { return m_hist; }
    int historyIndex() const { return m_histIndex; }
    // MaybeNoop (default) drops a commit whose document state matches the tip.
    // Always records a new step and leaves the digest lazy (slider-free, known edits).
    enum class CommitMode { MaybeNoop, Always };
    void commit(const QString &label, CommitMode mode = CommitMode::MaybeNoop);
    // While a merge is open, repeated commits with the same label replace the step that the
    // first one added (a slider drag or a held key becomes one undo step). Nestable.
    void beginMerge();
    // KeepIfChanged drops the merge tip only when the containers still share with the
    // base (a cheap no-op, no history digest). Cancel always drops the tip (the slider
    // has already restored the original value).
    enum class MergeEnd { KeepIfChanged, Cancel };
    void endMerge(MergeEnd how = MergeEnd::KeepIfChanged);
    void resetHistory(const QString &label = QStringLiteral("Open"));
    // Stamp every bitmap with interpolation < 0 from Preferences (Open/import paths).
    void stampUnsetBitmaps();
    static void stampUnsetBitmaps(DocState &st);
    bool canUndo() const { return m_histIndex > 0; }
    bool canRedo() const { return m_histIndex + 1 < m_hist.size(); }
    void undo();
    void redo();
    void jumpTo(int index);
    void revert();                          // drop uncommitted edits
    const DocState &committed() const { return m_hist[m_histIndex].st; }
    bool isDirty() const { return m_histIndex != m_cleanIndex || m_viewDirty; }
    void markClean() { m_cleanIndex = m_histIndex; m_viewDirty = false; emit historyChanged(); }

    // ---- helpers
    Frame &frame() { return s.frames[curFrame]; }
    QVector<VObject> &objects(int layer) { return s.frames[curFrame].layers[layer]; }
    VObject *find(quint64 id, int *layerOut = nullptr, int *indexOut = nullptr);
    QVector<VObject *> selectedObjects();
    QVector<VObject *> selectedEditable();
    VObject *singleSelected();
    quint64 newId() { return ++m_nextId; }
    quint64 lastId() const { return m_nextId; }
    void reserveIds(quint64 upTo) { if (m_nextId < upTo) m_nextId = upTo; }
    void addObject(VObject o, int layer = -1, bool select = true);
    void deleteSelection();
    void selectOnly(quint64 id);
    void setSelection(const QSet<quint64> &sel);
    void clearSelection();
    void selectAll(bool includeHotspots = true);
    void validateSelection();
    QRectF selectionBounds();

    int addLayer(const QString &name = QString());
    void deleteLayer(int index);
    void moveLayer(int from, int to);             // the Web Layer stays on top: it does not move, nothing moves above it
    // Move several layers as one block to destIndex (insertion index before removal;
    // 0 = back / visually bottom). Relative order is kept. The Web Layer is skipped
    // and nothing is placed at or above it. Returns true if anything moved.
    bool moveLayers(const QVector<int> &from, int destIndex);
    int duplicateLayer(int src);                  // a copy above src (fresh ids, nested ones too); -1 for the Web Layer
    QString uniqueLayerName() const;
    int webLayer() const;       // index of the Web Layer or -1
    int ensureWebLayer();       // create the (shared, topmost) Web Layer if needed
    int drawingLayer();         // layer for new artwork: the current one, or a normal layer if that is the Web Layer
    void setLayerShared(int index, bool shared);
    void syncShared();          // copy shared layers of the current frame to all frames

    void addFrame(bool duplicateCurrent);
    void deleteFrame(int index);
    void setCurrentFrame(int f);
    // Copy the current selection onto other frames (same layer, new ids). Shared layers are skipped.
    // `frames` are 0-based; the current frame is ignored.
    void copyToFrames(const QVector<int> &frames);
    // Place each selected object on its own consecutive frame starting at the current one,
    // adding empty frames as needed. The first (bottom-most) object stays put; the rest move.
    // Shared-layer objects are left as they are (they already appear on every frame).
    void distributeToFrames();

    void resizeCanvas(int w, int h, QPoint offset);  // offset applied to all objects
    // resample: a pixelscale Resample list entry (0-3 Bicubic..Nearest, 4+ pixel-art scalers); -1 = Preferences
    void scaleImage(int w, int h, int resample = -1);
    QRect contentPixelBounds() const;   // exact: the rendered, non-transparent pixels of every frame (Fit Canvas)

    void arrange(int how);
    // Move objects (by id, relative order kept) into destLayer, destGroupId (0 = the
    // layer's object list) at destIndex in storage (0 = back). One undo step after commit.
    // Refuses to drop a group into itself or a locked group, to move masks, or to place objects on the Web Layer.
    // Returns true if the stacking or parent actually changed.
    bool relocateObjects(const QVector<quint64> &ids, int destLayer, quint64 destGroupId, int destIndex);

    // ---- groups and masks (v0.3.2, Fireworks MX 2004 style). None of these commit.
    // find() above also finds objects nested in groups (children and masks); its index is the
    // position inside the object's own container. containerOf() returns that container (a layer's
    // object list, a group's children, or a group's mask list) and the owning group, if any.
    QVector<VObject> *containerOf(quint64 id, int *layerOut = nullptr, int *indexOut = nullptr, VObject **parentOut = nullptr);
    VObject *parentGroup(quint64 id);             // the group holding `id` (as a member or as its mask)
    VObject *topLevelOf(quint64 id, int *layerOut = nullptr, int *indexOut = nullptr); // the layer-level object containing `id`
    bool isMaskObject(quint64 id);                // `id` is some group's mask
    bool isNestedIn(quint64 id, quint64 groupId); // `id` sits (at any depth) inside group `groupId`
    // The group the Pointer works inside after double-click / Subselection (0 = layer level).
    // Session state only: not saved, not undone; cleared when it no longer exists.
    quint64 enteredGroup = 0;
    void enterGroup(quint64 groupId);
    void exitGroup();                             // one level up
    void reassignIds(VObject &o);                 // fresh ids for o and everything nested in it
    QVector<VObject *> selectedOutermost();       // selected editable objects without selected ancestors, in stacking order
    quint64 groupSelection();                     // Ctrl+G: the selection -> one group (in place of its top object)
    QVector<quint64> ungroupSelection();          // Ctrl+Shift+G: selected groups -> their members (a mask becomes an object)
    // Masks. Each returns the masked group's id (0 = nothing done).
    quint64 pasteAsMask(const VObject &mask);     // `mask` masks the selection (replaces an existing mask)
    quint64 groupAsMask();                        // the top selected object masks the others
    quint64 pasteInside(const QVector<VObject> &content); // the selected object becomes the mask of `content`
    quint64 revealHideAll(bool reveal);           // white / black grayscale bitmap mask over the selection
    int setMaskEnabled(bool on);                  // selected masked groups; returns how many changed
    int setMaskLinked(bool on);
    int deleteMask();                             // removes the mask; the members stay grouped
    quint64 maskOwnerOfSelection();               // the single masked group the selection is (or is in)
    // Convert to Bitmap: each selected object -> a bitmap of how it looks (effects baked; opacity
    // and blend stay object settings). Unlike Flatten Selection, objects are not merged.
    QVector<quint64> convertToBitmap();

    // ---- symbols (Library). None of these commit; callers commit with a label.
    Symbol *symbol(quint64 id);
    const Symbol *symbol(quint64 id) const;
    QString uniqueSymbolName(const QString &base) const;
    quint64 addSymbol(Symbol sym);                       // assigns an id and a unique name
    quint64 convertToSymbol(const QString &name, SymbolKind kind); // selection -> one instance; 0 if nothing
    VObject makeInstance(quint64 symId) const;           // at the symbol's own position, natural size
    // Places an instance centred on `center` (animation symbols span one document frame per
    // symbol frame, adding frames as needed). Returns the new object ids, selects the first.
    QVector<quint64> insertInstance(quint64 symId, const QPointF &center);
    void setSymbolFrames(quint64 symId, const QVector<QVector<VObject>> &frames); // instances follow
    bool deleteSymbol(quint64 symId);                    // removes its instances too
    quint64 duplicateSymbol(quint64 symId);
    int instanceCount(quint64 symId) const;              // over all frames
    QVector<quint64> breakApart();                       // selected instances -> their objects
    // Tween Instances: two or more selected instances of one symbol; `steps` in-betweens per pair.
    bool tweenInstances(int steps, bool distributeToFrames);
    // Animate Selection: converts the selection into an animation symbol of `frames` frames.
    quint64 animateSelection(int frames, const QPointF &move, double scalePct, int opacityFrom, int opacityTo);
    int appendFrame();                                   // empty frame at the end (shared layers kept)  // 0 front, 1 forward, 2 backward, 3 back

    QImage render(int frame = -1, bool bg = true) const;
    ImageSeq toImageSeq(bool allFrames) const;

    void notifyChanged() { emit changed(); }
    // a change confined to `dirty` (document pixels, old and new extents of what changed): views may
    // re-render just that region (P4). Listeners read it with partialChange() while changed() is emitted
    void notifyChanged(const QRectF &dirty) { m_changeRect = dirty; m_changePartial = true; emit changed(); m_changePartial = false; }
    bool partialChange(QRectF *r) const { if (m_changePartial && r) *r = m_changeRect; return m_changePartial; }
    void notifyStructure() { emit structureChanged(); emit changed(); }

    // ---- pixel edits (v0.3.4). Every operation that changes a bitmap's pixels calls
    // beginPixelEdit() on it first: it bakes a transform, refuses bitmaps whose live effects move
    // pixels irreversibly (Lens Correction, Make Seamless; returns false) and switches bitmaps with a
    // live Offset into display geometry (see PixelEdit.h), so canvas -> image is the plain rect
    // mapping. commit() ends the edits: offsets are shifted back losslessly and every touched
    // bitmap is trimmed to the tight bounds of its pixels (Fireworks bitmaps never keep
    // transparent margins), all in the same undo step. revert()/undo drop them.
    bool beginPixelEdit(VObject &o);
    void endPixelEdits(bool trim = true);   // also called by commit()
    void dropPixelEdits();
    bool pixelEditOpen() const { return !m_pixelTouched.isEmpty(); }
    QString takePixelEditNote() { QString n = m_pixelNote; m_pixelNote.clear(); return n; }

signals:
    void changed();            // pixels changed (repaint)
    void structureChanged();   // layers/frames/objects/properties changed (refresh panels)
    void selectionChanged();
    void historyChanged();
    void frameChanged();
    void viewSettingsChanged(); // grid or guides

private:
    QRectF m_changeRect;
    bool m_changePartial = false;
    int m_mergeDepth = 0;
    int m_mergeBase = -1;
    QVector<HistEntry> m_hist;
    void trimHistory();                         // undo step count and memory limits (coreprefs)
public:
    qint64 historyBytes() const;                // bitmap memory held by the history (shared images counted once)
private:
    int m_histIndex = 0;
    int m_cleanIndex = 0;
    bool m_viewDirty = false;
    quint64 m_nextId = 0;
    QHash<quint64, pxedit::Session> m_pixelEdits;
    QVector<quint64> m_pixelTouched;
    QString m_pixelNote;
    void restore(int index);
};
