#pragma once
#include "core/Document.h"
#include "core/Styles.h"
#include <QPointer>
#include <QTimer>
#include <QScrollArea>
#include <QWidget>

class QResizeEvent;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class DragInsertIndicator;
class QListWidget;
class QSpinBox;
class QCheckBox;
class QLabel;
class OptimizeWidget;
struct EncodeResult;
template <typename T> class QFutureWatcher;

// A Fireworks style panel: a grey title bar with a disclosure arrow over a body.
class CollapsiblePanel : public QWidget {
    Q_OBJECT
public:
    CollapsiblePanel(const QString &title, QWidget *body, QWidget *parent = nullptr);
    void setCollapsed(bool c);
    bool isCollapsed() const { return m_collapsed; }
    QWidget *body() const { return m_body; }
    QString title() const { return m_title; }
    // the text shown in the title bar (e.g. "Optimize: pane 3"); title() stays the panel's name
    void setHeaderText(const QString &t);
    QString headerText() const;
    int headerHeight() const;
    // Content-sized panels (Optimize, Align, Auto Shapes) follow their sizeHint
    // and are not resized by a splitter handle.
    void setContentSized(bool on);
    bool isContentSized() const { return m_contentSized; }
signals:
    void collapsedChanged(bool c);
private:
    QToolButton *m_header;
    QWidget *m_body;
    QString m_title;
    bool m_collapsed = false;
    bool m_contentSized = false;
};

class QSettings;
class PanelSplitterHandle;

// Right-dock scroll area: the stack's width always follows the viewport, so a
// vertical scrollbar never steals space from the panels (no horizontal bar).
class PanelScrollArea : public QScrollArea {
    Q_OBJECT
public:
    explicit PanelScrollArea(QWidget *parent = nullptr);
protected:
    void resizeEvent(QResizeEvent *e) override;
    bool event(QEvent *e) override;
    bool viewportEvent(QEvent *e) override;
private:
    void fitContentsToViewport();
};


// Vertical stack of CollapsiblePanels with splitter handles between expanded neighbours.
class PanelStack : public QWidget {
    Q_OBJECT
public:
    static constexpr int kHandleHeight = 5;
    static constexpr int kMinBody = 120;
    explicit PanelStack(QWidget *parent = nullptr);
    void addPanel(CollapsiblePanel *cp, int defaultStretch);
    QList<CollapsiblePanel *> panels() const { return m_panels; }
    QList<PanelSplitterHandle *> handles() const { return m_handles; }
    int visibleHandleCount() const;
    void saveLayout(QSettings &st) const;
    void restoreLayout(QSettings &st);
    // Grow (or shrink) the expanded resizable panel `above` by dy pixels and push
    // everything below down (or up), keeping those panels' heights. For tests and the handle.
    bool dragBetween(CollapsiblePanel *above, int dy);
    int storedHeight(CollapsiblePanel *cp) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void relayout();
private:
    void refreshHandles();
    void applyHeights();
    int minExpandedHeight(CollapsiblePanel *cp) const;
    int contentHeight(CollapsiblePanel *cp) const;
    int panelHeight(int i) const;
    int totalHeight() const;
    CollapsiblePanel *nextExpanded(int after) const;
    bool event(QEvent *e) override;
    QList<CollapsiblePanel *> m_panels;
    QList<int> m_defaultStretch;
    QList<int> m_heights; // stored content height for each panel (including header)
    QList<PanelSplitterHandle *> m_handles;
    class QVBoxLayout *m_layout = nullptr;
};

class DocPanel : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
    virtual void setDocument(Document *doc);
protected:
    virtual void rebuild() = 0;
    // A selection change alone (P3): panels whose content does not show the selection do nothing;
    // the Layers panel updates its row selection in place instead of rebuilding the tree.
    virtual void syncSelection() {}
    void scheduleRebuild();
    void scheduleSyncSelection();
    virtual void onHistoryChanged() { scheduleRebuild(); }
    QPointer<Document> m_doc;
private:
    bool m_rebuildPending = false;
    bool m_syncPending = false;
};

class LayersPanel final : public DocPanel {
    Q_OBJECT
public:
    explicit LayersPanel(QWidget *parent = nullptr);
    // In-place rename (v0.3.4): opens the inline editor on the current row (or the current layer)
    void renameCurrent();
    bool startRename(QTreeWidgetItem *it);
    void moveCurrent(int dir); // +1 visually up (layer up / bring forward)
    bool applyLayersDrop(const QPoint &viewportPos); // also used by tests
protected:
    void rebuild() override;
    void syncSelection() override;
    void onHistoryChanged() override;
    void resizeEvent(QResizeEvent *e) override;
private:
    int m_builtLayer = -1, m_builtFrame = -1;   // what the tree was built for (syncSelection rebuilds
    quint64 m_builtGroup = 0;                    // when they changed along with the selection)
    void onClicked(QTreeWidgetItem *it, int col);
    void applyRename(int kind, quint64 id, const QString &text);
    void deleteFromLayers(); // right-click: the clicked row, or the selection if that row is in it
    class QMenu *m_menu = nullptr;
    int m_ctxKind = -1;
    quint64 m_ctxId = 0;
    int m_ctxLayer = -1;
    bool m_ctxMenu = false;
    bool m_rebuildDeferred = false;   // a rebuild arrived while the inline editor was open
    bool eventFilter(QObject *o, QEvent *e) override;
    void fitNameColumn();
    static constexpr int kIndent = 12, kIconCol = 22, kThumbW = 20, kMinNameText = 64;
    QTreeWidget *m_tree;
    class QToolButton *m_share = nullptr;
    QSet<quint64> m_collapsedLayers;
    QSet<quint64> m_openGroups;   // expanded group rows (v0.3.2); groups holding the selection open too
    void addObjectItem(QTreeWidgetItem *parent, const VObject &o, int layer);
    bool refreshInPlace();   // history-driven: update existing rows/thumbs when the tree shape is unchanged
    void updateLayerInsertIndicator(const QPoint &viewportPos);
    DragInsertIndicator *m_insert = nullptr;
    bool m_building = false;
};

class FramesPanel final : public DocPanel {
    Q_OBJECT
public:
    explicit FramesPanel(QWidget *parent = nullptr);
    void setDocument(Document *doc) override;
    void renameCurrent();   // inline editor on the current frame (double-click / F2 do the same)
    bool isPlaying() const { return m_timer.isActive(); }
    void play(bool on);
    int renderedThumbCount() const { return m_thumbRenders; }   // thumbnails rendered so far (tests, bench)
protected:
    void rebuild() override;
    void showEvent(QShowEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
private:
    // Frame thumbnails are cached (P2) by lwd::contentKey(): a frame is rendered again only when what
    // it draws changed (selection changes, frame switches and playback render nothing)
    struct Thumb { QByteArray key; QPixmap pix; };
    void fillThumbs();
    QVector<Thumb> m_thumbs;
    QTimer m_thumbTimer;
    int m_thumbRenders = 0;
    QListWidget *m_list;
    QSpinBox *m_delay, *m_loops;
    QToolButton *m_play;
    QToolButton *m_onion;
    QTimer m_timer;
    bool m_building = false;
    bool m_rebuildDeferred = false;
    bool eventFilter(QObject *o, QEvent *e) override;
};

class HistoryPanel final : public DocPanel {
    Q_OBJECT
public:
    explicit HistoryPanel(QWidget *parent = nullptr);
protected:
    void rebuild() override;
    void resizeEvent(QResizeEvent *e) override;
private:
    QListWidget *m_list;
    bool m_building = false;
};

class OptimizePanel final : public DocPanel {
    Q_OBJECT
public:
    explicit OptimizePanel(QWidget *parent = nullptr);
    void setDocument(Document *doc) override;
    OptimizeWidget *widget() const { return m_w; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;
signals:
    void optionsChanged();
    void exportRequested();
protected:
    void rebuild() override;
    void showEvent(QShowEvent *e) override;
public:
    // The size estimate encodes a snapshot of the document on a worker thread (P1), debounced and
    // only while the panel is visible; these let tests wait for it.
    bool estimatePending() const;
    int estimatesRun() const { return m_estimatesRun; }
private:
    void estimate();
    void estimateDone();
    OptimizeWidget *m_w;
    QTimer m_estimate;
    QFutureWatcher<EncodeResult> *m_watch;
    QPointer<Document> m_estimateDoc;          // the document the running estimate belongs to
    bool m_estimateAgain = false;               // changed while an estimate ran: run another one
    bool m_estimateStale = false;               // skipped while hidden: run one when shown
    int m_estimatesRun = 0;
};

class AlignPanel final : public DocPanel {
    Q_OBJECT
public:
    explicit AlignPanel(QWidget *parent = nullptr);
protected:
    void rebuild() override {}
private:
    void align(int how);
    QToolButton *m_toCanvas;
};

namespace align {
// how: 0 left, 1 h-centre, 2 right, 3 top, 4 v-centre, 5 bottom, 6 distribute h, 7 distribute v, 8 same width, 9 same height
bool apply(Document *doc, int how, bool toCanvas);
}

// Styles panel: built-in presets plus user styles, kept in panel order in
// ~/.config/libreworks/styles.json. Click selects (Ctrl/Shift for several), double-click or Enter
// applies to the selected objects, Delete removes. Every style -- built-in or not -- can be renamed,
// reordered (drag, or Move Earlier/Later) and deleted; Reset Styles brings the built-ins back.
class StylesPanel final : public DocPanel {
    Q_OBJECT
public:
    explicit StylesPanel(QWidget *parent = nullptr);
    const QVector<Style> &styles() const { return m_styles; }
    int indexOf(const QString &name) const;
    QList<int> selectedRows() const;   // sorted
    bool applyStyle(int index);
    // New Style from the first selected object (or the current fill/stroke colours when nothing is
    // selected); `fill` etc. pick the attributes (Fireworks' New Style dialog)
    bool addStyle(const QString &name, bool fill, bool stroke, bool effects, bool text);
    int deleteStyles(QList<int> rows);   // returns how many went
    bool renameStyle(int index, const QString &name);
    bool moveStyle(int from, int to);
    bool duplicateStyle(int index);
    bool resetStyles();                  // the built-ins restored (in their original order), user styles kept after them
    int importStyles(const QString &path, QString *err = nullptr);   // appended, names made unique
    bool exportStyles(const QString &path, QList<int> rows = {}, QString *err = nullptr);   // empty = all
    void newStyleDialog();
    void deleteSelectedAsk();            // Delete key / trash button (asks unless testing)
    void renameDialog(int index);
    void reload();                       // re-read the library file
    QMenu *optionsMenu() const { return m_menu; }
protected:
    void rebuild() override;
    bool eventFilter(QObject *o, QEvent *e) override;
private:
    void fillList();
    void syncOrderFromList();
    bool saveUser();
    QString uniqueName(const QString &base, int except = -1) const;
    QListWidget *m_list;
    QToolButton *m_new, *m_del, *m_opts;
    QMenu *m_menu;
    QVector<Style> m_styles;
    bool m_filling = false;
};

// Symbol Properties / Convert to Symbol dialog (name + Graphic / Animation / Button).
bool symbolPropertiesDialog(QWidget *parent, const QString &title, QString &name, SymbolKind &kind);

// Library panel (F11): the document's symbols. Double-click edits, drag to the canvas inserts.
class LibraryPanel final : public DocPanel {
    Q_OBJECT
public:
    explicit LibraryPanel(QWidget *parent = nullptr);
    quint64 currentSymbol() const;
    void setCurrentSymbol(quint64 id);
    bool insertCurrent();               // instance centred on the canvas
    bool deleteCurrent(bool ask = true);
    quint64 duplicateCurrent();
    bool propertiesCurrent();           // dialog
    quint64 newSymbol();                // dialog, then emits editSymbol
    static QIcon preview(const Document &doc, const Symbol &sym, int size = 32);
signals:
    void editSymbol(quint64 id);
protected:
    void rebuild() override;
private:
    QListWidget *m_list;
    QLabel *m_count;
    QToolButton *m_newB, *m_dupB, *m_editB, *m_insB, *m_delB, *m_propB;
};

// Auto Shapes (v0.3.0): a grid of the parametric shapes; a click picks the shape's tool,
// drag on the canvas to draw it, then edit it with the yellow diamonds or the Property Inspector.
class AutoShapesPanel : public QWidget {
    Q_OBJECT
public:
    explicit AutoShapesPanel(QWidget *parent = nullptr);
private:
    void sync();
    QList<class QToolButton *> m_buttons;
};

class StubPanel : public QWidget {
    Q_OBJECT
public:
    StubPanel(const QString &what, QWidget *parent = nullptr);
};
