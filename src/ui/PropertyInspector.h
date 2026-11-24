#pragma once
#include "AppState.h"
#include "core/Document.h"
#include <QPointer>
#include <QWidget>
#include <functional>

class QStackedWidget;
class QLabel;
class QLineEdit;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QFontComboBox;
class WideFontComboBox;
class QListWidget;
class QToolButton;
class QPushButton;
class ColorButton;

// Context-sensitive Property Inspector, docked at the bottom like Fireworks.
class PropertyInspector : public QWidget {
    Q_OBJECT
public:
    explicit PropertyInspector(QWidget *parent = nullptr);
    void setDocument(Document *doc);
    void refresh();
    // Reorder the selected objects' effects from `from` to `to` as one undo step.
    void reorderEffects(int from, int to, bool *moved = nullptr);
    Document *document() const { return m_doc; }
    // 0.3.4: the height the panel needs to show every row of its tallest page: the document page,
    // the object page for each kind of selection (bitmap, vector shapes and fills, text, group,
    // auto shape, slice, hotspot, several objects) and the current tool's options. Measured from
    // the pages' size hints in the current style and font; the inspector's minimum height follows it.
    int contentHeight();
    void updateMinimumHeight();
protected:
    void changeEvent(QEvent *e) override;
signals:
    void canvasSizeRequested();
    void imageSizeRequested();
    void fitCanvasRequested();
    void fitCanvasToWindowRequested();
private:
    QWidget *buildDocPage();
    QWidget *buildObjectPage();
    QWidget *buildToolPage();
    QWidget *buildWebBox();
    void refreshObject();
    void refreshTool();
    template <class F> void applyToSelection(const QString &label, F f);

    QPointer<Document> m_doc;
    bool m_updating = false;
    QStackedWidget *m_stack;
    class QScrollArea *m_scroll = nullptr;
    bool m_measuring = false;
    // doc page
    QLabel *m_docTitle, *m_docInfo;
    ColorButton *m_canvasColor;
    QCheckBox *m_canvasTransparent;
    // object page
    QLabel *m_objType;
    QLineEdit *m_name;
    QDoubleSpinBox *m_w, *m_h, *m_x, *m_y;
    QWidget *m_bmpScaleRow = nullptr;  // 0.3.16: Scaling (interpolation) for bitmaps
    QComboBox *m_bmpScale = nullptr;
    ColorButton *m_fill, *m_stroke;
    QCheckBox *m_fillOn, *m_strokeOn, *m_aa;
    QDoubleSpinBox *m_strokeW, *m_round;
    QLabel *m_roundLabel;
    QWidget *m_shapeRow = nullptr;
    QString m_shapeRowKind;
    QList<std::function<void(const VObject &)>> m_shapeSync;
    void refreshShapeParams(const VObject *o);
    QWidget *m_textRow, *m_instRow;
    QWidget *m_maskRow = nullptr;   // v0.3.2: a masked group's mask mode / enabled / linked
    QLabel *m_groupInfo = nullptr;
    QComboBox *m_maskMode = nullptr;
    QCheckBox *m_maskOn = nullptr, *m_maskLink = nullptr;
    QLabel *m_instInfo;
    QLineEdit *m_instLink, *m_instAlt;
    QSpinBox *m_instFrame;
    WideFontComboBox *m_font;
    QDoubleSpinBox *m_fontSize;
    QDoubleSpinBox *m_kerning, *m_leading, *m_hScale, *m_baseline;
    QWidget *m_textRow2;
    QToolButton *m_bold, *m_italic, *m_underline, *m_alignL, *m_alignC, *m_alignR, *m_alignJ = nullptr;
    QComboBox *m_textAA;
    QWidget *m_textRow3 = nullptr, *m_textPathRow = nullptr;
    QComboBox *m_textOrient = nullptr, *m_textDir = nullptr;
    QDoubleSpinBox *m_paraIndent = nullptr, *m_paraBefore = nullptr, *m_paraAfter = nullptr;
    QDoubleSpinBox *m_textPathOffset = nullptr;
    QSpinBox *m_opacity;
    QComboBox *m_blend;
    QListWidget *m_effects;
    QToolButton *m_fxAdd, *m_fxDel, *m_fxEdit, *m_fxUp, *m_fxDown;
public: // fill/stroke option widgets are driven by the UI tests
    QWidget *m_fillRow, *m_strokeRow;
    QWidget *m_fillOptBox;
    QComboBox *m_fillKind, *m_pattern, *m_edge, *m_texture, *m_strokeKind, *m_strokeCat, *m_strokeName, *m_strokeAlign;
    QPushButton *m_editStroke = nullptr;
    QPushButton *m_gradEdit;
    QDoubleSpinBox *m_feather;
    QSpinBox *m_texAmount, *m_strokeTexAmount;
    QComboBox *m_strokeTexture;
    // Procedural... entries: 0 fill texture, 1 pattern fill, 2 stroke texture
    void editProcedural(int which);
public:
    void editGradient(); // opens the Edit Gradient dialog for the selection
    void editStroke();   // Edit Stroke…; no selection edits the tool defaults
    enum class EditStrokeTarget { None, BrushTip, SelectedVector, VectorDefault };
    EditStrokeTarget editStrokeTarget() const; // shared by the menu item and the dialog
    void applyStrokeParams(const stroke::Params &sp);
    QWidget *m_artBox, *m_webBox, *m_webPageBox = nullptr, *m_sliceOptBox, *m_instWebBox = nullptr;
    QLineEdit *m_link, *m_alt;
    QComboBox *m_target, *m_behavior, *m_swapTarget, *m_sliceFormat;
    QSpinBox *m_swapFrame, *m_sliceQuality;
    QCheckBox *m_sliceCustom;
    // tool page: rebuilt for each tool (buildToolOptions), values re-synced on every option change
    QLabel *m_toolTitle = nullptr;
    QWidget *m_toolHost = nullptr;
    QWidget *m_toolOpts = nullptr;
    int m_toolOptsFor = -1;
    QList<std::function<void()>> m_toolSync;
    void buildToolOptions(Tool t);
public:
    // adds a Live Effect to the selection; one with settings opens its window first and is only added on OK
    void addEffect(EffectType t);
    bool m_openEditorOnAdd = true; // tests turn this off
    bool m_fxPending = false;      // an Add Effect settings window is open (previewing, not yet in the list)
    void editEffect(int index);   // opens the effect's editor with live preview
    // Tools whose Property Inspector shows the document / object page instead of tool options
    static bool toolUsesDocPage(Tool t);
private:
};
