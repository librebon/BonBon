#include "core/Render.h"
#include <QMap>
#include "PropertyInspector.h"
#include "DragReorder.h"
#include "EditStrokeDialog.h"
#include "core/AutoShape.h"
#include <QGridLayout>
#include "PopupSlider.h"
#include <QScrollArea>
#include <QScrollBar>
#include "Dialogs.h"
#include "WidgetEnable.h"
#include "GradientEditor.h"
#include "ProceduralDialog.h"
#include "core/Procedural.h"
#include "core/Fills.h"
#include "core/PixelScale.h"
#include <QFileDialog>
#include "FileDialogs.h"
#include "io/ImageIO.h"
#include "Icons.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include "WideFontCombo.h"
#include <QFontComboBox>
#include <QScreen>
#include <QListView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QPointer>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include "core/BrushEngine.h"
#include <QPainter>
#include <QTimer>
#include <QEvent>
#include <memory>


namespace {
QDoubleSpinBox *dspin(double mn, double mx, int dec = 0, const QString &suffix = {}) {
    auto *s = new PopupSliderDoubleField;
    s->setRange(mn, mx);
    s->setDecimals(dec);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setFixedWidth(62);
    return s;
}
// Fireworks pop-up slider field (value + unit + down-triangle slider)
QDoubleSpinBox *sdspin(double mn, double mx, int dec, const QString &suffix, double slo, double shi, int width = 62) {
    auto *s = new PopupSliderDoubleField(mn, mx, dec, suffix);
    s->setSliderRange(slo, shi);
    s->setFixedWidth(width + SliderArrow::kWidth);
    return s;
}
QSpinBox *sspin(int mn, int mx, const QString &suffix, double slo, double shi, int width = 52) {
    auto *s = new PopupSliderField(mn, mx, suffix);
    s->setSliderRange(slo, shi);
    s->setFixedWidth(width + SliderArrow::kWidth);
    return s;
}
QLabel *small(const QString &t) {
    auto *l = new QLabel(t);
    l->setProperty("lwSmall", true);   // themed (QLabel[lwSmall])
    return l;
}
QToolButton *tbtn(const QString &text, const QString &tip, bool checkable = false) {
    auto *b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(checkable);
    b->setFixedSize(24, 22);
    return b;
}
// Font size field: a pop-up slider field (slider 1-200 pt, typed values up to 2000) plus the
// Fireworks preset list (8, 9, 10, 12 ... 72 ...) on a small button next to it.
const QList<int> kFontSizePresets = {8, 9, 10, 12, 14, 18, 24, 36, 48, 72, 96, 120, 144, 200};
QDoubleSpinBox *fontSizeField(const QString &objectName) {
    auto *s = new PopupSliderDoubleField(1, 2000, 0, QString());
    s->setSliderRange(1, 200);
    s->setObjectName(objectName);
    s->setToolTip(QStringLiteral("Font size (pt). Drag the triangle for a slider; type any size up to 2000."));
    s->setFixedWidth(46 + SliderArrow::kWidth);
    return s;
}
QWidget *withPresets(QDoubleSpinBox *field) {
    auto *box = new QWidget;
    auto *l = new QHBoxLayout(box);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    l->addWidget(field);
    auto *b = new QToolButton;
    b->setObjectName(field->objectName() + QStringLiteral("Presets"));
    b->setToolTip(QStringLiteral("Font size presets"));
    b->setText(QStringLiteral("\u2261"));
    b->setFixedSize(16, 22);
    b->setPopupMode(QToolButton::InstantPopup);
    auto *m = new QMenu(b);
    for (int v : kFontSizePresets) {
        QAction *a = m->addAction(QString::number(v));
        a->setData(v);
        QObject::connect(a, &QAction::triggered, field, [field, v] { field->setValue(v); });
    }
    b->setMenu(m);
    l->addWidget(b);
    return box;
}
}

PropertyInspector::PropertyInspector(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("PropertyInspector"));
    auto *l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget;
    m_stack->addWidget(buildDocPage());
    m_stack->addWidget(buildObjectPage());
    m_stack->addWidget(buildToolPage());
    // A QStackedWidget sizes itself to its largest page, which made the
    // scroll area show a horizontal bar even when the visible page fits.
    // Ignore the size of hidden pages.
    auto fitPage = [this](int idx) {
        for (int i = 0; i < m_stack->count(); ++i)
            m_stack->widget(i)->setSizePolicy(i == idx ? QSizePolicy::Preferred : QSizePolicy::Ignored,
                                              i == idx ? QSizePolicy::Preferred : QSizePolicy::Ignored);
        m_stack->updateGeometry();
    };
    fitPage(0);
    connect(m_stack, &QStackedWidget::currentChanged, this, fitPage);
    auto *scroll = new QScrollArea;
    m_scroll = scroll;
    scroll->setWidget(m_stack);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    l->addWidget(scroll);
    setMinimumHeight(118);
    updateMinimumHeight();
    connect(&appState(), &AppState::toolChanged, this, &PropertyInspector::refresh);
    connect(&appState(), &AppState::prefsChanged, this, [this] {
        m_toolOptsFor = -1; // Slice Link/Alt/Target and similar depend on Enable web features
        refresh();
        if (!m_doc) refreshTool();
    });
    connect(&appState(), &AppState::cropChanged, this, [this] { if (m_stack->currentIndex() == 2) refreshTool(); });
    connect(&appState(), &AppState::exportAreaChanged, this, [this] { if (m_stack->currentIndex() == 2) refreshTool(); });
    connect(&appState(), &AppState::optionsChanged, this, &PropertyInspector::refreshTool);
    connect(&appState(), &AppState::colorsChanged, this, &PropertyInspector::refreshTool);
}

QWidget *PropertyInspector::buildDocPage() {
    auto *w = new QWidget;
    auto *g = new QGridLayout(w);
    g->setContentsMargins(8, 6, 8, 6);
    m_docTitle = new QLabel(QStringLiteral("Document"));
    m_docTitle->setObjectName(QStringLiteral("PITitle"));
    g->addWidget(m_docTitle, 0, 0, 1, 2);
    m_docInfo = small({});
    g->addWidget(m_docInfo, 1, 0, 1, 2);
    g->addWidget(small(QStringLiteral("Canvas:")), 0, 2);
    m_canvasColor = new ColorButton(this, false);
    g->addWidget(m_canvasColor, 0, 3);
    m_canvasTransparent = new QCheckBox(QStringLiteral("Transparent"));
    g->addWidget(m_canvasTransparent, 0, 4);
    auto *cs = new QPushButton(QStringLiteral("Canvas Size..."));
    auto *is = new QPushButton(QStringLiteral("Image Size..."));
    auto *fc = new QPushButton(QStringLiteral("Fit Canvas"));
    auto *fw = new QPushButton(QStringLiteral("Fit to Window"));
    cs->setObjectName(QStringLiteral("docCanvasSize"));
    is->setObjectName(QStringLiteral("docImageSize"));
    fc->setObjectName(QStringLiteral("docFitCanvas"));
    fw->setObjectName(QStringLiteral("docFitToWindow"));
    fw->setToolTip(QStringLiteral("Resize the canvas to fill the current window at the current zoom, with no margin."));
    g->addWidget(cs, 1, 2, 1, 2);
    g->addWidget(is, 1, 4);
    g->addWidget(fc, 1, 5);
    g->addWidget(fw, 2, 2, 1, 2);
    g->setColumnStretch(6, 1);
    g->setRowStretch(3, 1);
    connect(cs, &QPushButton::clicked, this, &PropertyInspector::canvasSizeRequested);
    connect(is, &QPushButton::clicked, this, &PropertyInspector::imageSizeRequested);
    connect(fc, &QPushButton::clicked, this, &PropertyInspector::fitCanvasRequested);
    connect(fw, &QPushButton::clicked, this, &PropertyInspector::fitCanvasToWindowRequested);
    auto setCanvas = [this](QColor c) {
        if (!m_doc || m_updating) return;
        m_doc->s.canvasColor = c;
        m_doc->commit(QStringLiteral("Canvas Color"));
    };
    connect(m_canvasColor, &ColorButton::colorPicked, this, [this, setCanvas](const QColor &c) { m_canvasTransparent->setChecked(false); setCanvas(c); });
    connect(m_canvasTransparent, &QCheckBox::toggled, this, [this, setCanvas](bool on) {
        if (m_updating) return;
        QColor c = m_canvasColor->color();
        c.setAlpha(on ? 0 : 255);
        setCanvas(c);
    });
    return w;
}

QWidget *PropertyInspector::buildObjectPage() {
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(6, 4, 6, 4);
    h->setSpacing(8);
    // --- column 1: type, name, geometry
    {
        auto *g = new QGridLayout;
        g->setSpacing(3);
        m_objType = new QLabel;
        m_objType->setObjectName(QStringLiteral("PITitle"));
        g->addWidget(m_objType, 0, 0, 1, 4);
        m_name = new QLineEdit;
        m_name->setFixedWidth(150);
        g->addWidget(m_name, 1, 0, 1, 4);
        m_w = dspin(1, 32000); m_h = dspin(1, 32000); m_x = dspin(-32000, 32000); m_y = dspin(-32000, 32000);
        g->addWidget(small(QStringLiteral("W:")), 2, 0); g->addWidget(m_w, 2, 1);
        g->addWidget(small(QStringLiteral("X:")), 2, 2); g->addWidget(m_x, 2, 3);
        g->addWidget(small(QStringLiteral("H:")), 3, 0); g->addWidget(m_h, 3, 1);
        g->addWidget(small(QStringLiteral("Y:")), 3, 2); g->addWidget(m_y, 3, 3);
        h->addLayout(g);
        connect(m_name, &QLineEdit::editingFinished, this, [this] {
            // v0.3.4: trimmed; an empty name is ignored (the field shows the name again)
            const QString n = m_name->text().trimmed();
            if (n.isEmpty()) { QTimer::singleShot(0, this, [this] { refresh(); }); return; }
            applyToSelection(QStringLiteral("Rename Object"), [n](VObject &o) { if (o.name == n || (o.name.isEmpty() && o.displayName() == n)) return false; o.name = n; return true; });
        });
        auto geo = [this](int which) {
            const double v = (which == 0 ? m_w : which == 1 ? m_h : which == 2 ? m_x : m_y)->value();
            applyToSelection(which < 2 ? QStringLiteral("Resize") : QStringLiteral("Move"), [which, v](VObject &o) {
                QRectF b = o.bounds();
                if (which == 0) { if (qFuzzyCompare(b.width(), v)) return false; b.setWidth(v); }
                else if (which == 1) { if (qFuzzyCompare(b.height(), v)) return false; b.setHeight(v); }
                else if (which == 2) { if (qFuzzyCompare(b.x(), v)) return false; o.moveBy({v - b.x(), 0}); return true; }
                else { if (qFuzzyCompare(b.y(), v)) return false; o.moveBy({0, v - b.y()}); return true; }
                o.setBounds(b);
                return true;
            });
        };
        connect(m_w, &QDoubleSpinBox::valueChanged, this, [geo] { geo(0); });
        connect(m_h, &QDoubleSpinBox::valueChanged, this, [geo] { geo(1); });
        connect(m_x, &QDoubleSpinBox::valueChanged, this, [geo] { geo(2); });
        connect(m_y, &QDoubleSpinBox::valueChanged, this, [geo] { geo(3); });
        // 0.3.16: per-bitmap Scaling (same choices as Preferences → Interpolation)
        m_bmpScaleRow = new QWidget;
        auto *sr = new QHBoxLayout(m_bmpScaleRow);
        sr->setContentsMargins(0, 2, 0, 0);
        sr->setSpacing(4);
        m_bmpScale = new QComboBox;
        m_bmpScale->setObjectName(QStringLiteral("bitmapScaling"));
        m_bmpScale->addItems(pixelscale::resampleNames());
        m_bmpScale->setMaximumWidth(150);
        m_bmpScale->setToolTip(QStringLiteral("How this bitmap is resampled when displayed scaled or transformed.\n"
            "New bitmaps start with the Preferences Interpolation default."));
        sr->addWidget(small(QStringLiteral("Scaling:")));
        sr->addWidget(m_bmpScale, 1);
        g->addWidget(m_bmpScaleRow, 4, 0, 1, 4);
        connect(m_bmpScale, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (m_updating || i < 0) return;
            applyToSelection(QStringLiteral("Bitmap Scaling"), [i](VObject &o) {
                if (o.type != ObjType::Bitmap) return false;
                if (o.interpolation == i) return false;
                o.interpolation = i;
                return true;
            });
        });
    }
    // columns 2-3 are for artwork; slices and hotspots show the web column instead
    QHBoxLayout *outer = h;
    m_artBox = new QWidget;
    h = new QHBoxLayout(m_artBox);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(14);
    auto sep = [&] { auto *f = new QFrame; f->setFrameShape(QFrame::VLine); f->setFrameShadow(QFrame::Sunken); h->addWidget(f); };
    sep();
    // --- column 2: fill, stroke, text
    {
        auto *v = new QVBoxLayout;
        v->setSpacing(3);
        m_fillRow = new QWidget;
        auto *fr = new QHBoxLayout(m_fillRow);
        fr->setContentsMargins(0, 0, 0, 0);
        m_fill = new ColorButton(this, true);
        m_fill->setIconHint(icons::tool(Tool::PaintBucket));
        m_fill->setFixedSize(44, 22);
        m_fillOn = new QCheckBox(QStringLiteral("Fill"));
        m_roundLabel = small(QStringLiteral("Roundness:"));
        m_round = sdspin(0, 100, 0, QStringLiteral("%"), 0, 100);
        fr->addWidget(m_fill); fr->addWidget(m_fillOn); fr->addSpacing(8); fr->addWidget(m_roundLabel); fr->addWidget(m_round); fr->addStretch();
        v->addWidget(m_fillRow);
        m_strokeRow = new QWidget;
        auto *sr = new QHBoxLayout(m_strokeRow);
        sr->setContentsMargins(0, 0, 0, 0);
        m_stroke = new ColorButton(this, true);
        m_stroke->setIconHint(icons::tool(Tool::Pencil));
        m_stroke->setFixedSize(44, 22);
        m_strokeOn = new QCheckBox(QStringLiteral("Stroke"));
        m_strokeW = sdspin(0, 1000, 1, QStringLiteral(" px"), 0, 100);
        m_strokeW->setObjectName(QStringLiteral("strokeWidth"));
        m_aa = new QCheckBox(QStringLiteral("Anti-alias"));
        sr->addWidget(m_stroke); sr->addWidget(m_strokeOn); sr->addWidget(small(QStringLiteral("Tip size:"))); sr->addWidget(m_strokeW); sr->addWidget(m_aa); sr->addStretch();
        v->addWidget(m_strokeRow);
        m_textRow = new QWidget;
        auto *tr = new QHBoxLayout(m_textRow);
        tr->setContentsMargins(0, 0, 0, 0);
        tr->setSpacing(2);
        m_font = new WideFontComboBox;
        m_font->setObjectName(QStringLiteral("piFont"));
        if (auto *lv = qobject_cast<QListView *>(m_font->view())) lv->setUniformItemSizes(true);   // v0.3.2: theme-switch cost
        m_font->setFixedWidth(150);
                m_fontSize = fontSizeField(QStringLiteral("fontSize"));
        m_bold = tbtn(QStringLiteral("B"), QStringLiteral("Bold"), true);
        m_italic = tbtn(QStringLiteral("I"), QStringLiteral("Italic"), true);
        m_underline = tbtn(QStringLiteral("U"), QStringLiteral("Underline"), true);
        m_underline->setObjectName(QStringLiteral("textUnderline"));
        { QFont uf = m_underline->font(); uf.setUnderline(true); m_underline->setFont(uf); }
        m_alignL = tbtn(QStringLiteral("L"), QStringLiteral("Align left"), true);
        m_alignC = tbtn(QStringLiteral("C"), QStringLiteral("Align centre"), true);
        m_alignR = tbtn(QStringLiteral("R"), QStringLiteral("Align right"), true);
        m_alignJ = tbtn(QStringLiteral("J"), QStringLiteral("Justify"), true);
        m_alignJ->setObjectName(QStringLiteral("textAlignJustify"));
        QFont bf = m_bold->font(); bf.setBold(true); m_bold->setFont(bf);
        QFont itf = m_italic->font(); itf.setItalic(true); m_italic->setFont(itf);
        m_textAA = new QComboBox;
        m_textAA->setObjectName(QStringLiteral("TextAA"));
        m_textAA->addItems(textAANames());
        m_textAA->setToolTip(QStringLiteral("Text anti-aliasing"));
        for (QWidget *x : std::initializer_list<QWidget *>{m_font, withPresets(m_fontSize), m_bold, m_italic, m_underline, m_alignL, m_alignC, m_alignR, m_alignJ, m_textAA}) tr->addWidget(x);
        tr->addStretch();
        v->addWidget(m_textRow);
        // kerning, leading, horizontal scale and baseline shift: pop-up slider fields as in Fireworks
        m_textRow2 = new QWidget;
        m_textRow2->setObjectName(QStringLiteral("PITextRow2"));
        auto *tr2 = new QHBoxLayout(m_textRow2);
        tr2->setContentsMargins(0, 0, 0, 0);
        tr2->setSpacing(2);
        m_kerning = sdspin(-100, 100, 0, QStringLiteral("%"), -50, 50, 44);
        m_kerning->setObjectName(QStringLiteral("textKerning"));
        m_leading = sdspin(0, 1000, 0, QStringLiteral("%"), 50, 300, 48);
        m_leading->setObjectName(QStringLiteral("textLeading"));
        m_hScale = sdspin(1, 1000, 0, QStringLiteral("%"), 25, 400, 48);
        m_hScale->setObjectName(QStringLiteral("textHScale"));
        m_baseline = sdspin(-1000, 1000, 0, QStringLiteral(" px"), -50, 50, 48);
        m_baseline->setObjectName(QStringLiteral("textBaseline"));
        tr2->addWidget(small(QStringLiteral("Kerning:"))); tr2->addWidget(m_kerning);
        tr2->addWidget(small(QStringLiteral("Leading:"))); tr2->addWidget(m_leading);
        tr2->addWidget(small(QStringLiteral("H scale:"))); tr2->addWidget(m_hScale);
        tr2->addWidget(small(QStringLiteral("Baseline:"))); tr2->addWidget(m_baseline);
        tr2->addStretch();
        v->addWidget(m_textRow2);
        m_textRow3 = new QWidget;
        m_textRow3->setObjectName(QStringLiteral("PITextRow3"));
        auto *tr3 = new QHBoxLayout(m_textRow3);
        tr3->setContentsMargins(0, 0, 0, 0);
        tr3->setSpacing(2);
        m_textOrient = new QComboBox;
        m_textOrient->setObjectName(QStringLiteral("textOrientation"));
        m_textOrient->addItems({QStringLiteral("Horizontal"), QStringLiteral("Vertical")});
        m_textOrient->setToolTip(QStringLiteral("Text orientation"));
        m_textOrient->setMaximumWidth(110);
        m_textDir = new QComboBox;
        m_textDir->setObjectName(QStringLiteral("textDirection"));
        m_textDir->addItems({QStringLiteral("LTR"), QStringLiteral("RTL")});
        m_textDir->setToolTip(QStringLiteral("Writing direction"));
        m_textDir->setMaximumWidth(70);
        m_paraIndent = sdspin(0, 10000, 0, QStringLiteral(" px"), 0, 200, 48);
        m_paraIndent->setObjectName(QStringLiteral("textIndent"));
        m_paraBefore = sdspin(0, 10000, 0, QStringLiteral(" px"), 0, 200, 48);
        m_paraBefore->setObjectName(QStringLiteral("textSpaceBefore"));
        m_paraAfter = sdspin(0, 10000, 0, QStringLiteral(" px"), 0, 200, 48);
        m_paraAfter->setObjectName(QStringLiteral("textSpaceAfter"));
        tr3->addWidget(small(QStringLiteral("Orient:"))); tr3->addWidget(m_textOrient);
        tr3->addWidget(small(QStringLiteral("Dir:"))); tr3->addWidget(m_textDir);
        tr3->addWidget(small(QStringLiteral("Indent:"))); tr3->addWidget(m_paraIndent);
        tr3->addWidget(small(QStringLiteral("Before:"))); tr3->addWidget(m_paraBefore);
        tr3->addWidget(small(QStringLiteral("After:"))); tr3->addWidget(m_paraAfter);
        tr3->addStretch();
        v->addWidget(m_textRow3);
        m_textPathRow = new QWidget;
        m_textPathRow->setObjectName(QStringLiteral("PITextPathRow"));
        auto *tpr = new QHBoxLayout(m_textPathRow);
        tpr->setContentsMargins(0, 0, 0, 0);
        tpr->setSpacing(2);
        m_textPathOffset = sdspin(-100000, 100000, 1, QStringLiteral(" px"), -400, 400, 56);
        m_textPathOffset->setObjectName(QStringLiteral("textPathOffset"));
        m_textPathOffset->setToolTip(QStringLiteral("Text Offset: starting point along the path"));
        tpr->addWidget(small(QStringLiteral("Text Offset:"))); tpr->addWidget(m_textPathOffset);
        tpr->addStretch();
        v->addWidget(m_textPathRow);
        // symbol instances: symbol name / kind, the shown symbol frame, button link + alt
        m_instRow = new QWidget;
        m_instRow->setObjectName(QStringLiteral("PIInstance"));
        auto *ir = new QGridLayout(m_instRow);
        ir->setContentsMargins(0, 0, 0, 0);
        ir->setSpacing(3);
        m_instInfo = small(QString());
        m_instInfo->setObjectName(QStringLiteral("InstInfo"));
        m_instFrame = new PopupSliderField;
        m_instFrame->setObjectName(QStringLiteral("InstFrame"));
        m_instFrame->setKeyboardTracking(false);
        m_instFrame->setToolTip(QStringLiteral("Symbol frame / button state shown by this instance"));
        m_instLink = new QLineEdit;
        m_instLink->setObjectName(QStringLiteral("InstLink"));
        m_instLink->setPlaceholderText(QStringLiteral("http://"));
        m_instLink->setFixedWidth(160);
        m_instAlt = new QLineEdit;
        m_instAlt->setObjectName(QStringLiteral("InstAlt"));
        m_instAlt->setFixedWidth(160);
        ir->addWidget(m_instInfo, 0, 0, 1, 2);
        ir->addWidget(m_instFrame, 0, 2);
        m_instWebBox = new QWidget;
        m_instWebBox->setObjectName(QStringLiteral("PIInstWeb"));
        auto *iw = new QGridLayout(m_instWebBox);
        iw->setContentsMargins(0, 0, 0, 0);
        iw->setSpacing(3);
        iw->addWidget(small(QStringLiteral("Link:")), 0, 0); iw->addWidget(m_instLink, 0, 1, 1, 2);
        iw->addWidget(small(QStringLiteral("Alt:")), 1, 0); iw->addWidget(m_instAlt, 1, 1, 1, 2);
        ir->addWidget(m_instWebBox, 1, 0, 1, 3);
        v->addWidget(m_instRow);
        // groups (v0.3.2): member count; for a masked group the mask mode (Fireworks: "Mask: Alpha channel /
        // Grayscale" for bitmaps, "Path outline / Grayscale" for vectors), enabled and linked
        m_maskRow = new QWidget;
        m_maskRow->setObjectName(QStringLiteral("PIMask"));
        auto *mr = new QHBoxLayout(m_maskRow);
        mr->setContentsMargins(0, 0, 0, 0);
        mr->setSpacing(4);
        m_groupInfo = small(QString());
        m_groupInfo->setObjectName(QStringLiteral("GroupInfo"));
        m_maskMode = new QComboBox;
        m_maskMode->setObjectName(QStringLiteral("MaskMode"));
        m_maskMode->addItems({QStringLiteral("Alpha / path outline"), QStringLiteral("Grayscale")});
        m_maskMode->setToolTip(QStringLiteral("How the mask reveals the group: its shape (alpha channel or path outline) or its brightness"));
        m_maskOn = new QCheckBox(QStringLiteral("Enabled"));
        m_maskOn->setObjectName(QStringLiteral("MaskEnabled"));
        m_maskLink = new QCheckBox(QStringLiteral("Linked"));
        m_maskLink->setObjectName(QStringLiteral("MaskLinked"));
        m_maskLink->setToolTip(QStringLiteral("The mask moves and transforms with the group"));
        mr->addWidget(m_groupInfo);
        mr->addWidget(small(QStringLiteral("Mask:")));
        mr->addWidget(m_maskMode);
        mr->addWidget(m_maskOn);
        mr->addWidget(m_maskLink);
        mr->addStretch();
        v->addWidget(m_maskRow);
        connect(m_maskMode, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (m_updating) return;
            applyToSelection(QStringLiteral("Mask Mode"), [i](VObject &o) { if (!o.hasMask() || o.maskMode == i) return false; o.maskMode = i; return true; });
        });
        connect(m_maskOn, &QCheckBox::toggled, this, [this](bool on) {
            if (m_updating) return;
            applyToSelection(on ? QStringLiteral("Enable Mask") : QStringLiteral("Disable Mask"),
                             [on](VObject &o) { if (!o.hasMask() || o.maskEnabled == on) return false; o.maskEnabled = on; return true; });
        });
        connect(m_maskLink, &QCheckBox::toggled, this, [this](bool on) {
            if (m_updating) return;
            applyToSelection(on ? QStringLiteral("Link Mask") : QStringLiteral("Unlink Mask"),
                             [on](VObject &o) { if (!o.hasMask() || o.maskLinked == on) return false; o.maskLinked = on; return true; });
        });
        // auto shapes: the shape's parameters (rebuilt when the selected shape kind changes)
        m_shapeRow = new QWidget;
        m_shapeRow->setObjectName(QStringLiteral("PIAutoShape"));
        auto *shg = new QGridLayout(m_shapeRow);
        shg->setContentsMargins(0, 0, 0, 0);
        shg->setHorizontalSpacing(4);
        shg->setVerticalSpacing(3);
        v->addWidget(m_shapeRow);
        v->addStretch();
        h->addLayout(v);
        connect(m_instFrame, &QSpinBox::valueChanged, this, [this](int f) {
            if (m_updating) return;
            applyToSelection(QStringLiteral("Instance Frame"), [f](VObject &o) { if (!o.isInstance() || o.symFrame == f - 1) return false; o.symFrame = f - 1; return true; });
        });
        connect(m_instLink, &QLineEdit::editingFinished, this, [this] {
            const QString link = m_instLink->text().trimmed();
            applyToSelection(QStringLiteral("Link"), [link](VObject &o) { if (!o.isInstance() || o.link == link) return false; o.link = link; return true; });
        });
        connect(m_instAlt, &QLineEdit::editingFinished, this, [this] {
            const QString alt = m_instAlt->text();
            applyToSelection(QStringLiteral("Alt Text"), [alt](VObject &o) { if (!o.isInstance() || o.alt == alt) return false; o.alt = alt; return true; });
        });

        connect(m_fill, &ColorButton::colorPicked, this, [this](const QColor &c) {
            applyToSelection(QStringLiteral("Fill Color"), [c](VObject &o) {
                if (o.type == ObjType::Text) { o.fill = c; o.fillOn = true; return true; }
                if (o.type == ObjType::Bitmap || o.type == ObjType::Line) return false;
                o.fill = c; o.fillOn = true; return true;
            });
        });
        connect(m_fillOn, &QCheckBox::toggled, this, [this](bool on) {
            applyToSelection(QStringLiteral("Fill"), [on](VObject &o) { if (!o.isVector() || o.fillOn == on) return false; o.fillOn = on; return true; });
        });
        connect(m_round, &QDoubleSpinBox::valueChanged, this, [this](double r) {
            applyToSelection(QStringLiteral("Roundness"), [r](VObject &o) { if (o.type != ObjType::Rect || o.roundness == r) return false; o.roundness = r; return true; });
        });
        connect(m_stroke, &ColorButton::colorPicked, this, [this](const QColor &c) {
            applyToSelection(QStringLiteral("Stroke Color"), [c](VObject &o) { if (!o.isVector()) return false; o.stroke = c; o.strokeOn = true; return true; });
        });
        connect(m_strokeOn, &QCheckBox::toggled, this, [this](bool on) {
            applyToSelection(QStringLiteral("Stroke"), [on](VObject &o) { if (!o.isVector() || o.strokeOn == on) return false; o.strokeOn = on; return true; });
        });
        connect(m_strokeW, &QDoubleSpinBox::valueChanged, this, [this](double wv) {
            applyToSelection(QStringLiteral("Stroke Width"), [wv](VObject &o) { if (!o.isVector() || o.strokeWidth == wv) return false; o.strokeWidth = wv; return true; });
        });
        connect(m_aa, &QCheckBox::toggled, this, [this](bool on) {
            applyToSelection(QStringLiteral("Anti-alias"), [on](VObject &o) { if (o.antialias == on) return false; o.antialias = on; return true; });
        });
        connect(m_textAA, &QComboBox::currentIndexChanged, this, [this](int i) {
            applyToSelection(QStringLiteral("Text Anti-alias"), [i](VObject &o) {
                if (o.type != ObjType::Text || int(o.textAA) == i) return false;
                o.textAA = TextAA(i);
                o.antialias = o.textAA != TextAA::None;
                return true;
            });
        });
        auto textProp = [this](const QString &label, std::function<void(VObject &)> f) {
            applyToSelection(label, [f](VObject &o) { if (o.type != ObjType::Text) return false; f(o); o.fitTextBox(); return true; });
        };
        connect(m_font, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, textProp](int) {
            if (m_updating) return;
            const QString fam = m_font->currentFamily();
            if (fam.isEmpty()) return;
            textProp(QStringLiteral("Font"), [fam](VObject &o) { o.fontFamily = fam; });
        });
        connect(m_fontSize, &QDoubleSpinBox::valueChanged, this, [textProp](double s) { textProp(QStringLiteral("Font Size"), [s](VObject &o) { o.fontSize = s; }); });
        connect(m_bold, &QToolButton::toggled, this, [textProp](bool b) { textProp(QStringLiteral("Bold"), [b](VObject &o) { o.bold = b; }); });
        connect(m_italic, &QToolButton::toggled, this, [textProp](bool b) { textProp(QStringLiteral("Italic"), [b](VObject &o) { o.italic = b; }); });
        connect(m_underline, &QToolButton::toggled, this, [textProp](bool b) { textProp(QStringLiteral("Underline"), [b](VObject &o) { o.underline = b; }); });
        connect(m_kerning, &QDoubleSpinBox::valueChanged, this, [textProp](double x) { textProp(QStringLiteral("Kerning"), [x](VObject &o) { o.kerning = x; }); });
        connect(m_leading, &QDoubleSpinBox::valueChanged, this, [textProp](double x) { textProp(QStringLiteral("Leading"), [x](VObject &o) { o.leading = x; }); });
        connect(m_hScale, &QDoubleSpinBox::valueChanged, this, [textProp](double x) { textProp(QStringLiteral("Horizontal Scale"), [x](VObject &o) { o.hScale = x; }); });
        connect(m_baseline, &QDoubleSpinBox::valueChanged, this, [textProp](double x) { textProp(QStringLiteral("Baseline Shift"), [x](VObject &o) { o.baselineShift = x; }); });
        int ai = 0;
        for (QToolButton *b : {m_alignL, m_alignC, m_alignR, m_alignJ}) {
            connect(b, &QToolButton::clicked, this, [textProp, ai] { textProp(QStringLiteral("Text Align"), [ai](VObject &o) { o.textAlign = ai; }); });
            ++ai;
        }
        connect(m_textOrient, &QComboBox::currentIndexChanged, this, [textProp](int i) { textProp(QStringLiteral("Text Orientation"), [i](VObject &o) { o.textOrientation = i; }); });
        connect(m_textDir, &QComboBox::currentIndexChanged, this, [textProp](int i) { textProp(QStringLiteral("Text Direction"), [i](VObject &o) { o.textDirection = i; }); });
        connect(m_paraIndent, &QDoubleSpinBox::valueChanged, this, [textProp](double x) { textProp(QStringLiteral("Paragraph Indent"), [x](VObject &o) { o.paraIndent = x; }); });
        connect(m_paraBefore, &QDoubleSpinBox::valueChanged, this, [textProp](double x) { textProp(QStringLiteral("Space Before"), [x](VObject &o) { o.paraSpaceBefore = x; }); });
        connect(m_paraAfter, &QDoubleSpinBox::valueChanged, this, [textProp](double x) { textProp(QStringLiteral("Space After"), [x](VObject &o) { o.paraSpaceAfter = x; }); });
        connect(m_textPathOffset, &QDoubleSpinBox::valueChanged, this, [this](double x) {
            applyToSelection(QStringLiteral("Text Offset"), [x](VObject &o) {
                if (o.type != ObjType::Text || !o.textOnPath || o.textPathOffset == x) return false;
                o.textPathOffset = x;
                return true;
            });
        });
    }
    sep();
    // --- column 2b: fill category / edge / texture, stroke category / placement
    {
        m_fillOptBox = new QWidget;
        m_fillOptBox->setObjectName(QStringLiteral("PIFillOptions"));
        auto *g = new QGridLayout(m_fillOptBox);
        g->setContentsMargins(0, 0, 0, 0);
        g->setSpacing(3);
        m_fillKind = new QComboBox;
        m_fillKind->setObjectName(QStringLiteral("fillKind"));
        m_fillKind->addItems(fills::fillKindLabels());
        m_fillKind->insertSeparator(3);
        m_fillKind->setToolTip(QStringLiteral("Fill category (gradients: Linear ... Rectangle)"));
        m_gradEdit = new QPushButton(QStringLiteral("Edit..."));
        m_gradEdit->setObjectName(QStringLiteral("gradEdit"));
        m_gradEdit->setToolTip(QStringLiteral("Edit gradient colours"));
        m_pattern = new QComboBox;
        m_pattern->setObjectName(QStringLiteral("fillPattern"));
        m_pattern->addItems(fills::patternNames());
        m_pattern->addItem(QStringLiteral("Procedural..."));
        m_pattern->setItemData(m_pattern->count() - 1, QStringLiteral("Generate a seamless pattern (noise, Voronoi, reaction-diffusion, cellular automata, geometric)"), Qt::ToolTipRole);
        m_pattern->addItem(QStringLiteral("Other..."));
        m_edge = new QComboBox;
        m_edge->setObjectName(QStringLiteral("fillEdge"));
        m_edge->addItems({QStringLiteral("Hard"), QStringLiteral("Anti-Alias"), QStringLiteral("Feather")});
        m_feather = sdspin(0, 1000, 0, QStringLiteral(" px"), 0, 100);
        m_feather->setObjectName(QStringLiteral("fillFeather"));
        m_texture = new QComboBox;
        m_texture->setObjectName(QStringLiteral("fillTexture"));
        m_texture->addItem(QStringLiteral("No texture"));
        m_texture->addItems(fills::textureNames());
        m_texture->addItem(QStringLiteral("Procedural..."));
        m_texture->setItemData(m_texture->count() - 1, QStringLiteral("Generate a seamless texture with the procedural generators"), Qt::ToolTipRole);
        m_strokeTexture = new QComboBox;
        m_strokeTexture->setObjectName(QStringLiteral("strokeTexture"));
        m_strokeTexture->setToolTip(QStringLiteral("Stroke texture"));
        m_strokeTexture->addItem(QStringLiteral("No texture"));
        m_strokeTexture->addItems(fills::textureNames());
        m_strokeTexture->addItem(QStringLiteral("Procedural..."));
        m_strokeTexture->setMaximumWidth(118);
        m_strokeTexAmount = sspin(0, 100, QStringLiteral("%"), 0, 100);
        m_strokeTexAmount->setObjectName(QStringLiteral("strokeTextureAmount"));
        m_strokeTexAmount->setToolTip(QStringLiteral("Stroke texture amount"));
        m_texAmount = sspin(0, 100, QStringLiteral("%"), 0, 100);
        m_texAmount->setObjectName(QStringLiteral("textureAmount"));
        m_strokeKind = new QComboBox(this);   // kept for findData / legacy; hidden, mirrors cat+name
        m_strokeKind->setObjectName(QStringLiteral("strokeKind"));
        m_strokeKind->hide();
        for (const auto &sp : fills::strokePresets()) m_strokeKind->addItem(sp.category + QStringLiteral(": ") + sp.name, sp.key);
        m_strokeCat = new QComboBox;
        m_strokeCat->setObjectName(QStringLiteral("strokeCategory"));
        m_strokeCat->addItems(fills::strokeCategories());
        m_strokeCat->setToolTip(QStringLiteral("Stroke category"));
        m_strokeName = new QComboBox;
        m_strokeName->setObjectName(QStringLiteral("strokeName"));
        m_strokeName->setToolTip(QStringLiteral("Stroke type within the category"));
        m_strokeAlign = new QComboBox;
        m_strokeAlign->setObjectName(QStringLiteral("strokeAlign"));
        m_strokeAlign->addItems({QStringLiteral("Centered"), QStringLiteral("Inside"), QStringLiteral("Outside")});
        m_strokeAlign->setToolTip(QStringLiteral("Stroke placement on the path"));
        // Longest item names ("Viscous Alien Paint", "Libreworks") must not drive the
        // page's minimum width or a 1400px window gets a horizontal scrollbar.
        for (QComboBox *c : {m_fillKind, m_pattern, m_edge, m_texture, m_strokeAlign,
                             m_strokeCat, m_strokeName, m_strokeTexture}) {
            c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            c->setMinimumContentsLength(6);
            c->setMinimumSize(0, 0);
            c->setMaximumWidth(c == m_strokeCat || c == m_strokeName ? 80 : 100);
            QSizePolicy sp = c->sizePolicy();
            sp.setHorizontalPolicy(QSizePolicy::Preferred);
            c->setSizePolicy(sp);
        }
        m_fillOptBox->setMaximumWidth(240);
        auto *kr = new QHBoxLayout;
        kr->setSpacing(3);
        kr->addWidget(m_fillKind); kr->addWidget(m_gradEdit); kr->addWidget(m_pattern);
        g->addWidget(small(QStringLiteral("Fill:")), 0, 0); g->addLayout(kr, 0, 1, 1, 2);
        g->addWidget(small(QStringLiteral("Edge:")), 1, 0); g->addWidget(m_edge, 1, 1); g->addWidget(m_feather, 1, 2);
        g->addWidget(small(QStringLiteral("Texture:")), 2, 0); g->addWidget(m_texture, 2, 1); g->addWidget(m_texAmount, 2, 2);
        {
            auto *sr = new QHBoxLayout;
            sr->setSpacing(2);
            sr->setContentsMargins(0, 0, 0, 0);
            sr->addWidget(m_strokeCat, 1);
            sr->addWidget(m_strokeName, 1);
            g->addWidget(small(QStringLiteral("Stroke:")), 3, 0);
            g->addLayout(sr, 3, 1, 1, 2);
            m_editStroke = new QPushButton(QStringLiteral("Edit Stroke…"));
            m_editStroke->setObjectName(QStringLiteral("editStroke"));
            m_editStroke->setToolTip(QStringLiteral("Edit ink, tip shape and sensitivity (Fireworks Edit Stroke)."));
            m_editStroke->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            g->addWidget(m_editStroke, 4, 1);
            g->addWidget(m_strokeAlign, 4, 2);
        }
        { QLabel *stl = small(QStringLiteral("Texture:")); stl->setToolTip(QStringLiteral("Stroke texture and amount")); g->addWidget(stl, 5, 0); } g->addWidget(m_strokeTexture, 5, 1); g->addWidget(m_strokeTexAmount, 5, 2);
        g->setRowStretch(6, 1);
        h->addWidget(m_fillOptBox);
        connect(m_fillKind, &QComboBox::currentIndexChanged, this, [this](int idx) {
            if (m_updating) return;
            const int k = idx > 3 ? idx - 1 : idx; // separator
            applyToSelection(QStringLiteral("Fill Category"), [k](VObject &o) {
                if (!o.isVector() || (o.fillOn && int(o.fillKind) == k)) return false;
                fills::setFillKind(o, FillKind(k));
                return true;
            });
        });
        connect(m_gradEdit, &QPushButton::clicked, this, &PropertyInspector::editGradient);
        connect(m_pattern, &QComboBox::activated, this, [this](int i) {
            QString name = m_pattern->itemText(i);
            QImage custom;
            if (name == QLatin1String("Procedural...")) { editProcedural(1); return; }
            if (i == m_pattern->count() - 1) {
                const QString f = filedialogs::getOpenFileName(this, QStringLiteral("Pattern Image"), QString(), QStringLiteral("Images (*.png *.jpg *.jpeg *.gif *.bmp *.webp)"));
                if (f.isEmpty()) { refresh(); return; }
                custom = QImage(f).convertToFormat(QImage::Format_ARGB32_Premultiplied);
                if (custom.isNull()) { refresh(); return; }
                name = QStringLiteral("Custom");
            }
            applyToSelection(QStringLiteral("Fill Pattern"), [name, custom](VObject &o) {
                if (!o.isVector()) return false;
                o.fillOn = true;
                o.fillKind = FillKind::Pattern;
                o.pattern = name;
                o.patternImage = custom;
                return true;
            });
        });
        connect(m_edge, &QComboBox::currentIndexChanged, this, [this](int e) {
            const double fv = m_feather->value() > 0 ? m_feather->value() : 4;
            applyToSelection(QStringLiteral("Fill Edge"), [e, fv](VObject &o) {
                if (!o.isVector()) return false;
                const int cur = o.feather > 0 ? 2 : (o.antialias ? 1 : 0);
                if (cur == e) return false;
                o.antialias = e != 0;
                o.feather = e == 2 ? fv : 0;
                return true;
            });
        });
        connect(m_feather, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            applyToSelection(QStringLiteral("Feather"), [v](VObject &o) {
                if (!o.isVector() || o.feather == v) return false;
                o.feather = v;
                if (v > 0) o.antialias = true;
                return true;
            });
        });
        connect(m_texture, &QComboBox::activated, this, [this](int i) { if (i == m_texture->count() - 1) editProcedural(0); });
        connect(m_strokeTexture, &QComboBox::activated, this, [this](int i) { if (i == m_strokeTexture->count() - 1) editProcedural(2); });
        connect(m_strokeTexture, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i == m_strokeTexture->count() - 1) return;   // Procedural...: handled on activation
            const QString t = i <= 0 ? QString() : m_strokeTexture->itemText(i);
            applyToSelection(QStringLiteral("Stroke Texture"), [t](VObject &o) {
                if (!o.isVector() || o.strokeTexture == t) return false;
                o.strokeTexture = t;
                if (!t.isEmpty() && o.strokeTextureAmount == 0) o.strokeTextureAmount = 50;
                return true;
            });
        });
        connect(m_strokeTexAmount, &QSpinBox::valueChanged, this, [this](int a) {
            applyToSelection(QStringLiteral("Stroke Texture Amount"), [a](VObject &o) { if (!o.isVector() || o.strokeTextureAmount == a) return false; o.strokeTextureAmount = a; return true; });
        });
        connect(m_texture, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i == m_texture->count() - 1) return;   // Procedural...: handled on activation
            const QString t = i <= 0 ? QString() : m_texture->itemText(i);
            applyToSelection(QStringLiteral("Texture"), [t](VObject &o) {
                if (!o.isVector() || o.texture == t) return false;
                o.texture = t;
                if (!t.isEmpty() && o.textureAmount == 0) o.textureAmount = 50;
                return true;
            });
        });
        connect(m_texAmount, &QSpinBox::valueChanged, this, [this](int a) {
            applyToSelection(QStringLiteral("Texture Amount"), [a](VObject &o) { if (!o.isVector() || o.textureAmount == a) return false; o.textureAmount = a; return true; });
        });
        auto refillStrokeNames = [this](const QString &keepKey = {}) {
            const QString cat = m_strokeCat->currentText();
            const QStringList names = fills::strokeNamesInCategory(cat);
            const QString prefer = keepKey.isEmpty() ? QString() : keepKey;
            QString curName;
            for (const auto &sp : fills::strokePresets()) if (sp.key == prefer) curName = sp.name;
            m_strokeName->blockSignals(true);
            m_strokeName->clear();
            for (const QString &n : names) m_strokeName->addItem(n, fills::strokeKey(cat, n));
            int idx = prefer.isEmpty() ? 0 : m_strokeName->findData(prefer);
            if (idx < 0) idx = curName.isEmpty() ? 0 : m_strokeName->findText(curName);
            m_strokeName->setCurrentIndex(qMax(0, idx));
            m_strokeName->blockSignals(false);
        };
        auto applyStrokeKey = [this](const QString &k) {
            applyToSelection(QStringLiteral("Stroke"), [k](VObject &o) {
                if (!o.isVector() || (o.strokeKind == k && o.strokeOn && !o.strokeCustom)) return false;
                o.strokeKind = k;
                o.strokeOn = true;
                o.strokeCustom = false;
                o.strokeParams = stroke::paramsFor(k);
                if (o.strokeWidth <= 0) o.strokeWidth = 1;
                return true;
            });
        };
        connect(m_strokeCat, &QComboBox::currentIndexChanged, this, [this, refillStrokeNames, applyStrokeKey](int) {
            refillStrokeNames();
            applyStrokeKey(m_strokeName->currentData().toString());
        });
        connect(m_strokeName, &QComboBox::currentIndexChanged, this, [this, applyStrokeKey](int) {
            applyStrokeKey(m_strokeName->currentData().toString());
        });

        connect(m_editStroke, &QPushButton::clicked, this, [this] { editStroke(); });
        refillStrokeNames(QStringLiteral("basic.hard"));
        connect(m_strokeAlign, &QComboBox::currentIndexChanged, this, [this](int a) {
            applyToSelection(QStringLiteral("Stroke Placement"), [a](VObject &o) { if (!o.isVector() || o.strokeAlign == a) return false; o.strokeAlign = a; return true; });
        });
    }
    sep();
    // --- column 3: opacity, blend, effects
    {
        auto *g = new QGridLayout;
        g->setSpacing(3);
        m_opacity = sspin(0, 100, QStringLiteral("%"), 0, 100);
        m_opacity->setObjectName(QStringLiteral("objOpacity"));
        m_blend = new QComboBox;
        m_blend->addItems(blendModeNames());
        g->addWidget(small(QStringLiteral("Opacity:")), 0, 0);
        g->addWidget(m_opacity, 0, 1);
        g->addWidget(m_blend, 0, 2);
        g->addWidget(small(QStringLiteral("Effects:")), 1, 0);
        m_fxAdd = tbtn(QStringLiteral("+"), QStringLiteral("Add Live Effect"));
        m_fxDel = tbtn(QStringLiteral("-"), QStringLiteral("Remove effect"));
        m_fxEdit = tbtn(QString(), QStringLiteral("Edit effect"));
        m_fxEdit->setIcon(icons::named(QStringLiteral("fx")));
        auto *menu = new QMenu(m_fxAdd);
        menu->setObjectName(QStringLiteral("fxAddMenu"));
        {
            // Fireworks-style Effects menu: one submenu per category, alphabetical
            QMap<QString, QMenu *> subs;
            for (EffectType t : allEffectTypes()) {
                const QString cat = effectCategory(t);
                if (!subs.contains(cat)) subs[cat] = nullptr;
            }
            for (auto it = subs.begin(); it != subs.end(); ++it) it.value() = menu->addMenu(it.key());
            for (EffectType t : allEffectTypes()) {
                QAction *a = subs[effectCategory(t)]->addAction(effectName(t), this, [this, t] { addEffect(t); });
                a->setObjectName(QStringLiteral("fx_add_") + effectName(t));
            }
        }
        m_fxAdd->setObjectName(QStringLiteral("fxAdd"));
        m_fxAdd->setMenu(menu);
        m_fxAdd->setPopupMode(QToolButton::InstantPopup);
        // hide the menu-indicator arrow; InstantPopup still opens the menu on click
        m_fxAdd->setStyleSheet(QStringLiteral(
            "QToolButton#fxAdd::menu-indicator { image: none; width: 0px; }"));
        auto *br = new QHBoxLayout;
        br->setSpacing(1);
        m_fxUp = tbtn(QStringLiteral("▲"), QStringLiteral("Move effect up"));
        m_fxDown = tbtn(QStringLiteral("▼"), QStringLiteral("Move effect down"));
        m_fxUp->setObjectName(QStringLiteral("fxUp"));
        m_fxDown->setObjectName(QStringLiteral("fxDown"));
        br->addWidget(m_fxAdd); br->addWidget(m_fxDel); br->addWidget(m_fxEdit); br->addWidget(m_fxUp); br->addWidget(m_fxDown); br->addStretch();
        g->addLayout(br, 1, 1, 1, 2);
        m_effects = new QListWidget;
        m_effects->setObjectName(QStringLiteral("fxList"));
        m_fxDel->setObjectName(QStringLiteral("fxDel"));
        m_fxEdit->setObjectName(QStringLiteral("fxEdit"));
        m_effects->setFixedSize(210, 58);
        m_effects->setDragEnabled(true);
        m_effects->setAcceptDrops(true);
        m_effects->setDropIndicatorShown(false);
        m_effects->setMovement(QListView::Static);
        m_effects->setDragDropMode(QAbstractItemView::InternalMove);
        m_effects->setDefaultDropAction(Qt::MoveAction);
        m_effects->setToolTip(QStringLiteral("Drag to reorder effects"));
        auto *fxInd = new DragInsertIndicator(m_effects->viewport());
        class FxDragFilter : public QObject {
        public:
            PropertyInspector *pi = nullptr;
            QListWidget *list = nullptr;
            DragInsertIndicator *ind = nullptr;
            bool eventFilter(QObject *, QEvent *e) override {
                if (!pi || !list || !ind) return false;
                auto rowAt = [&](const QPoint &p) -> int {
                    if (QListWidgetItem *it = list->itemAt(p)) return list->row(it);
                    return list->count() > 0 ? list->count() - 1 : -1;
                };
                auto destOf = [&](const QPoint &p, bool *intoLine) {
                    const int n = list->count();
                    int r = rowAt(p);
                    if (r < 0) { if (intoLine) *intoLine = false; return n; }
                    const QRect vr = list->visualItemRect(list->item(r));
                    const bool above = p.y() < vr.center().y();
                    if (intoLine) *intoLine = false;
                    return above ? r : r + 1;
                };
                auto showAt = [&](const QPoint &p) {
                    const int n = list->count();
                    if (n == 0) { ind->clear(); return; }
                    const int dest = destOf(p, nullptr);
                    const int vw = list->viewport()->width();
                    if (dest >= n) {
                        const QRect vr = list->visualItemRect(list->item(n - 1));
                        ind->showLine(QRect(0, vr.bottom() - 1, vw, 2));
                    } else {
                        const QRect vr = list->visualItemRect(list->item(dest));
                        ind->showLine(QRect(0, vr.top() - 1, vw, 2));
                    }
                };
                if (e->type() == QEvent::DragEnter) {
                    static_cast<QDragEnterEvent *>(e)->acceptProposedAction();
                    return true;
                }
                if (e->type() == QEvent::DragMove) {
                    auto *de = static_cast<QDragMoveEvent *>(e);
                    showAt(de->position().toPoint());
                    de->acceptProposedAction();
                    return true;
                }
                if (e->type() == QEvent::DragLeave) { ind->clear(); return true; }
                if (e->type() == QEvent::Drop) {
                    auto *de = static_cast<QDropEvent *>(e);
                    const int from = list->currentRow();
                    int dest = destOf(de->position().toPoint(), nullptr);
                    ind->clear();
                    if (from < 0 || from >= list->count()) { de->ignore(); return true; }
                    if (dest > from) --dest;
                    dest = qBound(0, dest, list->count() - 1);
                    if (dest == from) { de->ignore(); return true; }
                    bool moved = false;
                    pi->reorderEffects(from, dest, &moved);
                    if (moved) list->setCurrentRow(dest);
                    de->acceptProposedAction();
                    return true;
                }
                return false;
            }
        };
        auto *ff = new FxDragFilter;
        ff->setParent(m_effects);
        ff->pi = this;
        ff->list = m_effects;
        ff->ind = fxInd;
        m_effects->viewport()->installEventFilter(ff);
        g->addWidget(m_effects, 2, 0, 1, 3);
        h->addLayout(g);
        h->addStretch();

        connect(m_opacity, &QSpinBox::valueChanged, this, [this](int v) {
            applyToSelection(QStringLiteral("Opacity"), [v](VObject &o) { if (o.opacity == v) return false; o.opacity = v; return true; });
        });
        connect(m_blend, &QComboBox::currentIndexChanged, this, [this](int i) {
            applyToSelection(QStringLiteral("Blend Mode"), [i](VObject &o) { if (int(o.blend) == i) return false; o.blend = BlendMode(i); return true; });
        });
        connect(m_fxDel, &QToolButton::clicked, this, [this] {
            int r = m_effects->currentRow();
            if (r < 0) return;
            applyToSelection(QStringLiteral("Remove Effect"), [r](VObject &o) { if (r >= o.effects.size()) return false; o.effects.remove(r); return true; });
        });
        connect(m_fxEdit, &QToolButton::clicked, this, [this] { editEffect(m_effects->currentRow()); });
        auto move = [this](int delta) {
            const int r = m_effects->currentRow(), to = r + delta;
            if (r < 0) return;
            bool moved = false;
            applyToSelection(QStringLiteral("Reorder Effects"), [r, to, &moved](VObject &o) {
                if (r >= o.effects.size() || to < 0 || to >= o.effects.size()) return false;
                o.effects.move(r, to);
                moved = true;
                return true;
            });
            if (moved) m_effects->setCurrentRow(to);
        };
        connect(m_fxUp, &QToolButton::clicked, this, [move] { move(-1); });
        connect(m_fxDown, &QToolButton::clicked, this, [move] { move(1); });
        connect(m_effects, &QListWidget::itemDoubleClicked, this, [this] { editEffect(m_effects->currentRow()); });
        connect(m_effects, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
            if (m_updating) return;
            int r = m_effects->row(it);
            bool on = it->checkState() == Qt::Checked;
            applyToSelection(on ? QStringLiteral("Enable Effect") : QStringLiteral("Disable Effect"), [r, on](VObject &o) {
                if (r >= o.effects.size() || o.effects[r].enabled == on) return false;
                o.effects[r].enabled = on;
                return true;
            });
        });
    }
    outer->addWidget(m_artBox);
    outer->addWidget(buildWebBox());
    outer->addStretch();
    return w;
}

QWidget *PropertyInspector::buildWebBox() {
    m_webBox = new QWidget;
    m_webBox->setObjectName(QStringLiteral("PIWeb"));
    auto *h = new QHBoxLayout(m_webBox);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(14);
    auto sep = [&] { auto *f = new QFrame; f->setFrameShape(QFrame::VLine); f->setFrameShadow(QFrame::Sunken); h->addWidget(f); };
    m_webPageBox = new QWidget;
    m_webPageBox->setObjectName(QStringLiteral("PIWebPage"));
    auto *wh = new QHBoxLayout(m_webPageBox);
    wh->setContentsMargins(0, 0, 0, 0);
    wh->setSpacing(14);
    auto webSep = [&] { auto *f = new QFrame; f->setFrameShape(QFrame::VLine); f->setFrameShadow(QFrame::Sunken); wh->addWidget(f); };
    webSep();
    // link / alt / target
    {
        auto *g = new QGridLayout;
        g->setSpacing(3);
        m_link = new QLineEdit;
        m_link->setObjectName(QStringLiteral("WebLink"));
        m_link->setPlaceholderText(QStringLiteral("http://"));
        m_link->setFixedWidth(200);
        m_alt = new QLineEdit;
        m_alt->setObjectName(QStringLiteral("WebAlt"));
        m_alt->setFixedWidth(200);
        m_target = new QComboBox;
        m_target->setObjectName(QStringLiteral("WebTarget"));
        m_target->setEditable(true);
        m_target->addItems({QString(), QStringLiteral("_blank"), QStringLiteral("_self"), QStringLiteral("_parent"), QStringLiteral("_top")});
        m_target->setFixedWidth(120);
        g->addWidget(small(QStringLiteral("Link:")), 0, 0); g->addWidget(m_link, 0, 1);
        g->addWidget(small(QStringLiteral("Alt:")), 1, 0); g->addWidget(m_alt, 1, 1);
        g->addWidget(small(QStringLiteral("Target:")), 2, 0); g->addWidget(m_target, 2, 1, Qt::AlignLeft);
        auto text = [this](const QString &label, QString VObject::*field, const QString &v) {
            applyToSelection(label, [field, v](VObject &o) { if (!o.isWeb() || o.*field == v) return false; o.*field = v; return true; });
        };
        connect(m_link, &QLineEdit::editingFinished, this, [this, text] { text(QStringLiteral("Link"), &VObject::link, m_link->text().trimmed()); });
        connect(m_alt, &QLineEdit::editingFinished, this, [this, text] { text(QStringLiteral("Alt Text"), &VObject::alt, m_alt->text()); });
        connect(m_target, &QComboBox::currentTextChanged, this, [text](const QString &t) { text(QStringLiteral("Target"), &VObject::target, t.trimmed()); });
        wh->addLayout(g);
    }
    webSep();
    // behaviours
    {
        auto *g = new QGridLayout;
        g->setSpacing(3);
        m_behavior = new QComboBox;
        m_behavior->setObjectName(QStringLiteral("WebBehavior"));
        m_behavior->addItems({QStringLiteral("No behavior"), QStringLiteral("Simple Rollover"), QStringLiteral("Swap Image")});
        m_swapTarget = new QComboBox;
        m_swapTarget->setObjectName(QStringLiteral("WebSwapTarget"));
        m_swapTarget->setToolTip(QStringLiteral("Slice whose image is swapped on mouse over"));
        m_swapFrame = new PopupSliderField;
        m_swapFrame->setObjectName(QStringLiteral("WebSwapFrame"));
        m_swapFrame->setPrefix(QStringLiteral("Frame "));
        m_swapFrame->setKeyboardTracking(false);
        g->addWidget(small(QStringLiteral("Behavior:")), 0, 0); g->addWidget(m_behavior, 0, 1);
        g->addWidget(small(QStringLiteral("Swap:")), 1, 0); g->addWidget(m_swapTarget, 1, 1);
        g->addWidget(small(QStringLiteral("Show:")), 2, 0); g->addWidget(m_swapFrame, 2, 1);
        connect(m_behavior, &QComboBox::currentIndexChanged, this, [this](int i) {
            applyToSelection(QStringLiteral("Behavior"), [i](VObject &o) { if (!o.isWeb() || int(o.behavior) == i) return false; o.behavior = WebBehavior(i); return true; });
        });
        connect(m_swapTarget, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i < 0) return;
            const quint64 id = m_swapTarget->itemData(i).toULongLong();
            applyToSelection(QStringLiteral("Swap Image Target"), [id](VObject &o) { if (!o.isWeb() || o.swapTarget == id || o.id == id) return false; o.swapTarget = id; return true; });
        });
        connect(m_swapFrame, &QSpinBox::valueChanged, this, [this](int f) {
            applyToSelection(QStringLiteral("Swap Image Frame"), [f](VObject &o) { if (!o.isWeb() || o.swapFrame == f - 1) return false; o.swapFrame = f - 1; return true; });
        });
        wh->addLayout(g);
    }
    h->addWidget(m_webPageBox);
    sep();
    // per-slice optimize settings
    {
        m_sliceOptBox = new QWidget;
        auto *g = new QGridLayout(m_sliceOptBox);
        g->setContentsMargins(0, 0, 0, 0);
        g->setSpacing(3);
        m_sliceCustom = new QCheckBox(QStringLiteral("Custom export settings"));
        m_sliceCustom->setObjectName(QStringLiteral("SliceCustomOpt"));
        m_sliceFormat = new QComboBox;
        m_sliceFormat->setObjectName(QStringLiteral("SliceFormat"));
        for (Fmt f : {Fmt::PNG, Fmt::JPEG, Fmt::GIF, Fmt::WEBP, Fmt::AVIF, Fmt::JXL})
            if (ImageIO::available(f)) m_sliceFormat->addItem(ImageIO::formatName(f), int(f));
        m_sliceQuality = sspin(0, 100, QString(), 0, 100, 80);
        m_sliceQuality->setPrefix(QStringLiteral("Quality "));
        m_sliceQuality->setKeyboardTracking(false);
        g->addWidget(m_sliceCustom, 0, 0, 1, 2);
        g->addWidget(m_sliceFormat, 1, 0);
        g->addWidget(m_sliceQuality, 1, 1);
        g->setRowStretch(2, 1);
        h->addWidget(m_sliceOptBox);
        auto apply = [this] {
            if (m_updating) return;
            const bool on = m_sliceCustom->isChecked();
            const Fmt f = Fmt(m_sliceFormat->currentData().toInt());
            const int q = m_sliceQuality->value();
            const ExportOptions base = m_doc ? m_doc->exportOptions : ExportOptions{};
            applyToSelection(QStringLiteral("Slice Export Settings"), [on, f, q, base](VObject &o) {
                if (o.type != ObjType::Slice) return false;
                ExportOptions n = o.sliceCustomOpt ? o.sliceOpt : base;
                n.format = f;
                n.quality = q;
                if (o.sliceCustomOpt == on && (!on || n == o.sliceOpt)) return false;
                o.sliceCustomOpt = on;
                o.sliceOpt = n;
                return true;
            });
        };
        connect(m_sliceCustom, &QCheckBox::toggled, this, apply);
        connect(m_sliceFormat, &QComboBox::currentIndexChanged, this, apply);
        connect(m_sliceQuality, &QSpinBox::valueChanged, this, apply);
    }
    return m_webBox;
}

QWidget *PropertyInspector::buildToolPage() {
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(8, 6, 8, 6);
    h->setSpacing(14);
    m_toolTitle = new QLabel;
    m_toolTitle->setObjectName(QStringLiteral("PITitle"));
    m_toolTitle->setMinimumWidth(110);
    m_toolTitle->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    h->addWidget(m_toolTitle, 0, Qt::AlignTop);
    m_toolHost = new QWidget;
    m_toolHost->setObjectName(QStringLiteral("PIToolOptions"));
    auto *hl = new QHBoxLayout(m_toolHost);
    hl->setContentsMargins(0, 0, 0, 0);
    h->addWidget(m_toolHost, 0, Qt::AlignTop);
    h->addStretch();
    return w;
}

bool PropertyInspector::toolUsesDocPage(Tool t) {
    return t == Tool::Pointer || t == Tool::Subselect || toolIsTransform(t) || t == Tool::Hand || t == Tool::Zoom;
}

namespace {
// Lays tool options out the way Fireworks MX 2004 does: label / field pairs in columns of
// three rows, left to right. Every control writes straight into AppState and registers a
// sync function that reads the value back (tool switches, [ ] keys, the canvas...).
// auto shape ratio parameters (0..1-ish, 2 decimals) are edited as percentages
bool asPercent(const autoshape::Param &p) { return p.decimals >= 2 && p.max <= 2 && p.suffix.isEmpty(); }

class OptGrid {
public:
    // `host` is required: it becomes the parent of the QGridLayout (not an unused parameter).
    OptGrid(QWidget *host, QList<std::function<void()>> *sync, const bool *updating)
        : m_sync(sync), m_updating(updating) {
        m_g = new QGridLayout(host);
        m_g->setContentsMargins(0, 0, 0, 0);
        m_g->setHorizontalSpacing(4);
        m_g->setVerticalSpacing(3);
    }
    static void changed() { appState().notifyOptions(); }
    void sync(std::function<void()> f) { m_sync->append(std::move(f)); }
    // place a label (may be empty) and a field
    void add(const QString &label, QWidget *field, int span = 1) {
        if (m_row >= 3) newColumn();
        if (!label.isEmpty()) {
            QLabel *lab = small(label);
            if (field) lab->setBuddy(field);
            m_g->addWidget(lab, m_row, m_col * 3, Qt::AlignRight | Qt::AlignVCenter);
        }
        m_g->addWidget(field, m_row, m_col * 3 + 1, 1, span, Qt::AlignLeft | Qt::AlignVCenter);
        ++m_row;
    }
    void newColumn() {
        if (m_row == 0) return;
        m_g->setColumnMinimumWidth(m_col * 3 + 2, 12);
        m_row = 0;
        ++m_col;
    }
    QSpinBox *intSlider(const QString &label, const QString &name, std::function<int()> get, std::function<void(int)> set,
                        int mn, int mx, const QString &suffix, double slo, double shi, int width = 52) {
        QSpinBox *s = sspin(mn, mx, suffix, slo, shi, width);
        s->setObjectName(name);
        s->setKeyboardTracking(false);
        QObject::connect(s, &QSpinBox::valueChanged, s, [u = m_updating, set](int v) { if (!*u) { set(v); changed(); } });
        sync([s, get] { s->setValue(get()); });
        add(label, s);
        return s;
    }
    QSpinBox *intSlider(const QString &label, const QString &name, int AppState::*m, int mn, int mx, const QString &suffix,
                        double slo, double shi, int width = 52) {
        return intSlider(label, name, [m] { return appState().*m; }, [m](int v) { appState().*m = v; }, mn, mx, suffix, slo, shi, width);
    }
    QDoubleSpinBox *dblSlider(const QString &label, const QString &name, double AppState::*m, double mn, double mx, int dec,
                              const QString &suffix, double slo, double shi, int width = 56) {
        QDoubleSpinBox *s = sdspin(mn, mx, dec, suffix, slo, shi, width);
        s->setObjectName(name);
        s->setKeyboardTracking(false);
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [u = m_updating, m](double v) { if (!*u) { appState().*m = v; changed(); } });
        sync([s, m] { s->setValue(appState().*m); });
        add(label, s);
        return s;
    }
    QDoubleSpinBox *dblSlider(const QString &label, const QString &name, std::function<double()> get, std::function<void(double)> set,
                              double mn, double mx, int dec, const QString &suffix, int width = 56) {
        QDoubleSpinBox *s = sdspin(mn, mx, dec, suffix, mn, mx, width);
        s->setObjectName(name);
        s->setKeyboardTracking(false);
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [u = m_updating, set](double v) { if (!*u) { set(v); changed(); } });
        sync([s, get] { s->setValue(get()); });
        add(label, s);
        return s;
    }
    QComboBox *combo(const QString &label, const QString &name, const QStringList &items, std::function<int()> get,
                     std::function<void(int)> set) {
        auto *c = new QComboBox;
        c->setObjectName(name);
        for (int i = 0; i < items.size(); ++i) c->addItem(items[i], i);   // data = the stored value
        QObject::connect(c, &QComboBox::currentIndexChanged, c, [u = m_updating, set](int i) { if (!*u && i >= 0) { set(i); changed(); } });
        sync([c, get] { c->setCurrentIndex(qBound(0, get(), c->count() - 1)); });
        add(label, c);
        return c;
    }
    QComboBox *combo(const QString &label, const QString &name, const QStringList &items, int AppState::*m) {
        return combo(label, name, items, [m] { return appState().*m; }, [m](int v) { appState().*m = v; });
    }
    QCheckBox *check(const QString &text, const QString &name, bool AppState::*m) {
        auto *c = new QCheckBox(text);
        c->setObjectName(name);
        QObject::connect(c, &QCheckBox::toggled, c, [u = m_updating, m](bool v) { if (!*u) { appState().*m = v; changed(); } });
        sync([c, m] { c->setChecked(appState().*m); });
        add(QString(), c, 2);
        return c;
    }
    QLineEdit *text(const QString &label, const QString &name, QString AppState::*m, const QString &placeholder = {}) {
        auto *e = new QLineEdit;
        e->setObjectName(name);
        e->setFixedWidth(170);
        e->setPlaceholderText(placeholder);
        QObject::connect(e, &QLineEdit::textEdited, e, [u = m_updating, m](const QString &v) { if (!*u) { appState().*m = v.trimmed(); changed(); } });
        sync([e, m] { if (e->text().trimmed() != appState().*m) e->setText(appState().*m); });
        add(label, e);
        return e;
    }
    // texture combo + amount slider (per-tool brushTexture / brushTextureAmount)
    void texture() {
        QStringList names{QStringLiteral("None")};
        names += fills::textureNames();
        combo(QStringLiteral("Texture:"), QStringLiteral("opt_texture"), names,
              [names] { return qMax(0, int(names.indexOf(appState().brushTexture))); },
              [names](int i) { appState().brushTexture = i <= 0 ? QString() : names.value(i); });
        intSlider(QStringLiteral("Amount:"), QStringLiteral("opt_textureAmount"), &AppState::brushTextureAmount, 0, 100,
                  QStringLiteral("%"), 0, 100);
    }
    void opacityBlend() {
        intSlider(QStringLiteral("Opacity:"), QStringLiteral("opt_opacity"), &AppState::toolOpacity, 1, 100, QStringLiteral("%"), 1, 100);
        combo(QStringLiteral("Blend:"), QStringLiteral("opt_blend"), blendModeNames(), &AppState::toolBlend);
    }
    void tip(bool shape = true) {
        intSlider(QStringLiteral("Size:"), QStringLiteral("opt_size"), &AppState::brushSize, 1, 1000, QStringLiteral(" px"), 1, 100);
        intSlider(QStringLiteral("Edge:"), QStringLiteral("opt_edge"), &AppState::brushEdge, 0, 100, QStringLiteral("%"), 0, 100);
        if (shape) combo(QStringLiteral("Shape:"), QStringLiteral("opt_tipShape"), {QStringLiteral("Round"), QStringLiteral("Square")},
                         &AppState::tipShape);
    }
    // fill edge: Hard / Anti-Alias / Feather + feather amount
    void fillEdge() {
        combo(QStringLiteral("Edge:"), QStringLiteral("opt_fillEdge"),
              {QStringLiteral("Hard"), QStringLiteral("Anti-Alias"), QStringLiteral("Feather")}, &AppState::fillEdge);
        QSpinBox *f = intSlider(QStringLiteral("Feather:"), QStringLiteral("opt_fillFeather"), &AppState::fillFeather, 0, 250,
                                QStringLiteral(" px"), 0, 100);
        sync([f] { f->setEnabled(appState().fillEdge == 2); });
    }
private:
    QGridLayout *m_g;
    QList<std::function<void()>> *m_sync;
    const bool *m_updating;
    int m_row = 0, m_col = 0;
};
} // OptGrid

namespace {
QString currentBrushTipKey()
{
    const auto cat = brush::Category(qBound(0, appState().brushCategory, int(brush::Category::Libreworks)));
    return brush::tipStrokeKey(cat, appState().brushTip);
}

void writePiDabFieldsToBrushCustom()
{
    const QString key = currentBrushTipKey();
    auto it = appState().brushStrokeByTip.find(key);
    if (it == appState().brushStrokeByTip.end()) return;
    it->size = appState().brushSize;
    it->edge = appState().brushEdge;
    it->square = appState().tipShape == 1;
    it->texture = appState().brushTexture;
    it->textureAmount = appState().brushTextureAmount;
}

void applyTipAndRestoreBrushCustom(brush::Category cat, int tip)
{
    brush::Params p;
    brush::applyTip(p, cat, tip);
    appState().brushEdge = p.edge;
    appState().tipShape = p.square ? 1 : 0;
    if (!p.texture.isEmpty()) {
        appState().brushTexture = p.texture;
        appState().brushTextureAmount = p.textureAmount;
    }
    const QString key = stroke::canonicalKey(p.strokeKey);
    auto it = appState().brushStrokeByTip.constFind(key);
    if (it == appState().brushStrokeByTip.cend()) return;
    const stroke::Params &sp = it.value();
    if (sp.size > 0)
        appState().brushSize = qBound(1, int(qRound(sp.size)), 1000);
    appState().brushEdge = sp.edge;
    appState().tipShape = sp.square ? 1 : 0;
    appState().brushTexture = sp.texture;
    appState().brushTextureAmount = sp.textureAmount;
}

brush::Params liveBrushParams()
{
    brush::Params bp;
    const auto cat = brush::Category(qBound(0, appState().brushCategory, int(brush::Category::Libreworks)));
    brush::applyTip(bp, cat, appState().brushTip);
    const AppState &as = appState();
    bp.size = as.brushSize > 0 ? as.brushSize : bp.size;
    bp.edge = as.brushEdge;
    bp.square = as.tipShape == 1;
    if (!as.brushTexture.isEmpty()) {
        bp.texture = as.brushTexture;
        bp.textureAmount = as.brushTextureAmount;
    } else if (bp.textureAmount == 0) {
        bp.textureAmount = as.brushTextureAmount;
    }
    return bp;
}
}

void PropertyInspector::buildToolOptions(Tool t) {
    if (m_toolOpts) {   // detach now so lookups never find the old controls; delete once idle
        m_toolOpts->hide();
        m_toolOpts->setParent(nullptr);
        m_toolOpts->deleteLater();
    }
    m_toolSync.clear();
    m_toolOpts = new QWidget;
    m_toolHost->layout()->addWidget(m_toolOpts);
    m_toolOptsFor = int(t);
    OptGrid g(m_toolOpts, &m_toolSync, &m_updating);
    AppState *as = &appState();
    auto selEdge = [&](int AppState::*edge, double AppState::*feather) {
        g.combo(QStringLiteral("Edge:"), QStringLiteral("opt_selEdge"),
                {QStringLiteral("Hard"), QStringLiteral("Anti-alias"), QStringLiteral("Feather")}, edge);
        auto *f = g.intSlider(QStringLiteral("Feather:"), QStringLiteral("opt_selFeather"),
                              [feather] { return qRound(appState().*feather); }, [feather](int v) { appState().*feather = v; },
                              1, 250, QStringLiteral(" px"), 1, 100);
        g.sync([f, edge] { f->setEnabled(appState().*edge == 2); });
    };
    auto strokeTarget = [&] {
        g.combo(QStringLiteral("Target:"), QStringLiteral("opt_strokeTarget"),
                {QStringLiteral("New bitmap for each stroke"), QStringLiteral("Expand selected bitmap"),
                 QStringLiteral("Paint into selected bitmap only (clip)")},
                &AppState::strokeTarget)->setToolTip(QStringLiteral("Where a stroke goes when a bitmap object is selected"));
    };
    auto strokeFill = [&](bool fill) {
        auto *sw = g.dblSlider(QStringLiteral("Stroke:"), QStringLiteral("opt_strokeWidth"), &AppState::strokeWidth, 0, 1000, 0,
                               QStringLiteral(" px"), 0, 100);
        sw->setToolTip(QStringLiteral("Stroke width of new shapes"));
        {
            const QStringList cats = fills::strokeCategories();
            auto *catBox = g.combo(QStringLiteral("Category:"), QStringLiteral("opt_strokeCat"), cats,
                [cats]() -> int {
                    const QString key = fills::resolveStrokeKind(appState().strokeKind);
                    for (const auto &sp : fills::strokePresets()) if (sp.key == key) return int(qMax(0, cats.indexOf(sp.category)));
                    return 0;
                },
                [cats](int i) {
                    const QStringList names = fills::strokeNamesInCategory(cats.value(i));
                    appState().strokeKind = fills::strokeKey(cats.value(i), names.value(0));
                    appState().notifyOptions();
                });
            auto namesNow = [] {
                const QString key = fills::resolveStrokeKind(appState().strokeKind);
                QString cat;
                for (const auto &sp : fills::strokePresets()) if (sp.key == key) { cat = sp.category; break; }
                if (cat.isEmpty()) cat = fills::strokeCategories().value(0);
                return fills::strokeNamesInCategory(cat);
            };
            auto *typeBox = g.combo(QStringLiteral("Type:"), QStringLiteral("opt_strokeName"), namesNow(),
                [namesNow]() -> int {
                    const QString key = fills::resolveStrokeKind(appState().strokeKind);
                    const QStringList names = namesNow();
                    QString cat;
                    for (const auto &sp : fills::strokePresets()) if (sp.key == key) { cat = sp.category; break; }
                    for (int i = 0; i < names.size(); ++i) if (fills::strokeKey(cat, names[i]) == key) return i;
                    return 0;
                },
                [](int i) {
                    const QString key = fills::resolveStrokeKind(appState().strokeKind);
                    QString cat;
                    for (const auto &sp : fills::strokePresets()) if (sp.key == key) { cat = sp.category; break; }
                    const QStringList names = fills::strokeNamesInCategory(cat);
                    appState().strokeKind = fills::strokeKey(cat, names.value(i));
                });
            g.sync([typeBox, namesNow, catBox] {
                const QStringList names = namesNow();
                QStringList cur;
                for (int i = 0; i < typeBox->count(); ++i) cur << typeBox->itemText(i);
                if (cur != names) {
                    const QString key = fills::resolveStrokeKind(appState().strokeKind);
                    typeBox->blockSignals(true);
                    typeBox->clear();
                    typeBox->addItems(names);
                    QString cat = catBox->currentText();
                    int idx = 0;
                    for (int i = 0; i < names.size(); ++i) if (fills::strokeKey(cat, names[i]) == key) idx = i;
                    typeBox->setCurrentIndex(idx);
                    typeBox->blockSignals(false);
                }
            });
            Q_UNUSED(catBox);
        }
        g.check(QStringLiteral("Stroke on"), QStringLiteral("opt_strokeOn"), &AppState::strokeOn);
        if (fill) {
            g.newColumn();
            g.check(QStringLiteral("Fill on"), QStringLiteral("opt_fillOn"), &AppState::fillOn);
            g.fillEdge();
        }
    };
    auto pathTool = [&](bool strength) {
        g.intSlider(QStringLiteral("Size:"), QStringLiteral("pathSize"), &AppState::pathToolSize, 1, 1000, QStringLiteral(" px"), 1, 200);
        if (strength) g.intSlider(QStringLiteral("Strength:"), QStringLiteral("pathStrength"), &AppState::reshapeStrength, 1, 100,
                                  QStringLiteral("%"), 1, 100);
        g.check(QStringLiteral("Pressure"), QStringLiteral("opt_pathPressure"), &AppState::pathPressure)
            ->setToolTip(QStringLiteral("A tablet pen's pressure scales the size"));
    };
    auto link = [&] {
        g.newColumn();
        g.text(QStringLiteral("Link:"), QStringLiteral("opt_linkUrl"), &AppState::linkUrl, QStringLiteral("http://"));
        g.text(QStringLiteral("Alt:"), QStringLiteral("opt_linkAlt"), &AppState::linkAlt);
        auto *tg = g.text(QStringLiteral("Target:"), QStringLiteral("opt_linkTarget"), &AppState::linkTarget, QStringLiteral("_blank"));
        tg->setFixedWidth(90);
    };
    // Tablet (v0.3.1): "Pressure: [Size] [Opacity] [Flow]" for the painting tools (per tool), and Tilt for
    // the Brush (a calligraphic nib follows the pen's tilt / rotation). opacityLabel names what the pressure
    // scales for the retouch tools (Intensity, Exposure, Strength...); empty hides it.
    auto pressureRow = [&](bool size, const QString &opacityLabel, bool flow, bool tilt) {
        auto *box = new QWidget;
        box->setObjectName(QStringLiteral("opt_pressure"));
        box->setToolTip(QStringLiteral("What a tablet pen's pressure changes (curve and minimums in Preferences \u25B8 Tablet).\n"
                                       "A mouse always presses at 100%."));
        auto *bl = new QHBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(6);
        auto add = [&](const QString &text, const QString &name, bool AppState::*m) {
            auto *c = new QCheckBox(text);
            c->setObjectName(name);
            bl->addWidget(c);
            connect(c, &QCheckBox::toggled, this, [this, m](bool v) { if (!m_updating) { appState().*m = v; appState().notifyOptions(); } });
            g.sync([c, m] { c->setChecked(appState().*m); });
        };
        if (size) add(QStringLiteral("Size"), QStringLiteral("opt_pressureSize"), &AppState::pressureSize);
        if (!opacityLabel.isEmpty()) add(opacityLabel, QStringLiteral("opt_pressureOpacity"), &AppState::pressureOpacity);
        if (flow) add(QStringLiteral("Flow"), QStringLiteral("opt_pressureFlow"), &AppState::pressureFlow);
        bl->addStretch();
        g.add(QStringLiteral("Pressure:"), box, 2);
        if (tilt)
            g.check(QStringLiteral("Tilt steers the nib"), QStringLiteral("opt_tiltAngle"), &AppState::tiltAngle)
                ->setToolTip(QStringLiteral("Calligraphy: the nib angle follows the pen's tilt direction or barrel rotation"));
    };
    auto note = [&](const QString &text) {
        auto *l = small(text);
        l->setObjectName(QStringLiteral("opt_note"));
        g.newColumn();
        g.add(QString(), l, 2);
    };
    switch (t) {
    case Tool::Brush: {
        // Category + Type share one OptGrid cell so the tool page still fits at 1400px
        auto *box = new QWidget;
        auto *bl = new QHBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(3);
        auto *catBox = new QComboBox;
        catBox->setObjectName(QStringLiteral("opt_brushCategory"));
        catBox->addItems(brush::categoryNames());
        auto *tipBox = new QComboBox;
        tipBox->setObjectName(QStringLiteral("opt_brushTip"));
        for (QComboBox *c : {catBox, tipBox}) {
            c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            c->setMinimumContentsLength(6);
            c->setMinimumSize(0, 0);
            c->setMaximumWidth(90);
        }
        bl->addWidget(catBox);
        bl->addWidget(tipBox);
        g.add(QStringLiteral("Brush:"), box, 2);
        auto tipLabels = [] {
            return brush::tipNames(brush::Category(qBound(0, appState().brushCategory, int(brush::Category::Libreworks))));
        };
        auto refillTips = [tipBox, tipLabels] {
            const QStringList names = tipLabels();
            tipBox->blockSignals(true);
            tipBox->clear(); tipBox->addItems(names);
            tipBox->setCurrentIndex(qBound(0, appState().brushTip, names.size() - 1));
            tipBox->blockSignals(false);
        };
        refillTips();
        QObject::connect(catBox, &QComboBox::currentIndexChanged, box, [this, refillTips](int i) {
            if (m_updating) return;   // PI rebuild / tool switch must not reset the tip
            appState().brushCategory = i;
            appState().brushTip = 0;
            applyTipAndRestoreBrushCustom(brush::Category(i), 0);
            refillTips();
            OptGrid::changed();
        });
        QObject::connect(tipBox, &QComboBox::currentIndexChanged, box, [](int i) {
            appState().brushTip = i;
            applyTipAndRestoreBrushCustom(brush::Category(appState().brushCategory), i);
            OptGrid::changed();
        });
        g.sync([catBox, refillTips] {
            catBox->blockSignals(true);
            catBox->setCurrentIndex(qBound(0, appState().brushCategory, catBox->count() - 1));
            catBox->blockSignals(false);
            refillTips();
        });
        // Size, with Edit Stroke… to its right, sits on the row under the category / tip dropdowns.
        {
            auto *row = new QWidget;
            auto *hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 0, 0, 0);
            hl->setSpacing(6);
            QSpinBox *sz = sspin(1, 1000, QStringLiteral(" px"), 1, 100);
            sz->setObjectName(QStringLiteral("opt_size"));
            sz->setKeyboardTracking(false);
            connect(sz, &QSpinBox::valueChanged, this, [this](int v) {
                if (m_updating) return;
                appState().brushSize = v;
                writePiDabFieldsToBrushCustom();
                appState().notifyOptions();
            });
            g.sync([sz] { sz->setValue(appState().brushSize); });
            auto *btn = new QPushButton(QStringLiteral("Edit Stroke…"));
            btn->setObjectName(QStringLiteral("opt_editStroke"));
            btn->setToolTip(QStringLiteral("Edit this brush tip's stroke options, including Cellular."));
            btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            connect(btn, &QPushButton::clicked, this, [this] { editStroke(); });
            hl->addWidget(sz);
            hl->addWidget(btn);
            hl->addStretch();
            g.add(QStringLiteral("Size:"), row, 2);
        }
        g.intSlider(QStringLiteral("Edge:"), QStringLiteral("opt_edge"),
                    [] { return appState().brushEdge; },
                    [](int v) { appState().brushEdge = v; writePiDabFieldsToBrushCustom(); },
                    0, 100, QStringLiteral("%"), 0, 100);
        g.combo(QStringLiteral("Shape:"), QStringLiteral("opt_tipShape"), {QStringLiteral("Round"), QStringLiteral("Square")},
                [] { return appState().tipShape; },
                [](int v) { appState().tipShape = v; writePiDabFieldsToBrushCustom(); });
        {
            QStringList names{QStringLiteral("None")};
            names += fills::textureNames();
            g.combo(QStringLiteral("Texture:"), QStringLiteral("opt_texture"), names,
                    [names] { return qMax(0, int(names.indexOf(appState().brushTexture))); },
                    [names](int i) {
                        appState().brushTexture = i <= 0 ? QString() : names.value(i);
                        writePiDabFieldsToBrushCustom();
                    });
            g.intSlider(QStringLiteral("Amount:"), QStringLiteral("opt_textureAmount"),
                        [] { return appState().brushTextureAmount; },
                        [](int v) { appState().brushTextureAmount = v; writePiDabFieldsToBrushCustom(); },
                        0, 100, QStringLiteral("%"), 0, 100);
        }
        g.sync([this] {
            const auto cat = brush::Category(qBound(0, appState().brushCategory, int(brush::Category::Libreworks)));
            const stroke::Params sp = stroke::paramsFor(brush::tipStrokeKey(cat, appState().brushTip));
            auto setOn = [this](const char *name, bool on) {
                if (QWidget *w = findChild<QWidget *>(QString::fromLatin1(name)))
                    uiutil::setEnabledWithLabel(w, on);
            };
            setOn("opt_edge", stroke::optionApplies(sp.look, stroke::Option::Edge));
            setOn("opt_tipShape", stroke::optionApplies(sp.look, stroke::Option::Square));
        });
        g.opacityBlend();
        g.check(QStringLiteral("Preserve transparency"), QStringLiteral("opt_preserveTransparency"), &AppState::preserveTransparency);
        g.newColumn();
        strokeTarget();
        pressureRow(true, QStringLiteral("Opacity"), true, true);
        }
        break;
    case Tool::Pencil:
        g.intSlider(QStringLiteral("Size:"), QStringLiteral("opt_size"), &AppState::brushSize, 1, 1000, QStringLiteral(" px"), 1, 100);
        g.check(QStringLiteral("Anti-aliased"), QStringLiteral("opt_antialias"), &AppState::antialias);
        g.check(QStringLiteral("Auto erase"), QStringLiteral("opt_autoErase"), &AppState::autoErase)
            ->setToolTip(QStringLiteral("Starting on the stroke colour paints with the fill colour"));
        g.check(QStringLiteral("Preserve transparency"), QStringLiteral("opt_preserveTransparency"), &AppState::preserveTransparency);
        g.check(QStringLiteral("Pixel perfect"), QStringLiteral("opt_pixelPerfect"), &AppState::pixelPerfect)
            ->setToolTip(QStringLiteral("1 px aliased strokes: remove the extra pixels of L-shaped corners (pixel art)"));
        {
            // two-colour pixel art: left drag paints one colour, right drag the other (Transparent erases)
            auto *two = g.check(QStringLiteral("Left/Right colours"), QStringLiteral("opt_pencilTwoColor"), &AppState::pencilTwoColor);
            two->setToolTip(QStringLiteral("Left drag paints the left colour, right drag the right colour (no canvas context menu with the Pencil).\n"
                                           "Transparent erases to alpha 0. Shift-click draws a line from the end of the last stroke."));
            auto *box = new QWidget;
            box->setObjectName(QStringLiteral("opt_pencilColors"));
            auto *bl = new QHBoxLayout(box);
            bl->setContentsMargins(0, 0, 0, 0);
            bl->setSpacing(4);
            auto swatch = [&](bool right) {
                auto *sw = new ColorButton(nullptr, false);
                sw->setObjectName(right ? QStringLiteral("opt_pencilRight") : QStringLiteral("opt_pencilLeft"));
                sw->setTransparentChoice(true);
                sw->setFixedSize(32, 20);
                connect(sw, &ColorButton::colorPicked, this, [right](const QColor &col) {
                    (right ? appState().pencilRight : appState().pencilLeft) = col;
                    appState().notifyOptions();
                });
                auto *l = small(right ? QStringLiteral("Right click") : QStringLiteral("Left click"));
                bl->addWidget(l);
                bl->addWidget(sw);
                return sw;
            };
            ColorButton *left = swatch(false);
            bl->addSpacing(6);
            ColorButton *right = swatch(true);
            bl->addStretch();
            g.add(QString(), box, 2);
            g.sync([box, left, right] {
                const AppState &st = appState();
                left->setColor(st.pencilColor(false));
                right->setColor(st.pencilColor(true));
                box->setEnabled(st.pencilTwoColor);
            });
        }
        g.opacityBlend();
        strokeTarget();
        pressureRow(true, QString(), false, false);   // the Pencil: size only
        break;
    case Tool::ContentAware:
        g.intSlider(QStringLiteral("Size:"), QStringLiteral("opt_size"), &AppState::brushSize, 1, 1000, QStringLiteral(" px"), 1, 200);
        note(QStringLiteral("Paint over what to remove; on release it is filled from the surroundings (PatchMatch).\n"
                            "For a selection, use Filters \u25B8 Other \u25B8 Content-Aware Fill."));
        break;
    case Tool::ReplaceColor: {
        // Fireworks' Replace Color: From (swatch or image) -> To, Tolerance, Range (Solid / Color), Strength, Size, Edge
        g.tip();
        auto swatch = [&](const QString &name, QColor AppState::*m) {
            auto *sw = new ColorButton(nullptr, false);
            sw->setObjectName(name);
            sw->setFixedSize(32, 20);
            connect(sw, &ColorButton::colorPicked, this, [m](const QColor &col) { appState().*m = col; appState().notifyOptions(); });
            g.sync([sw, m] { sw->setColor(appState().*m); });
            return sw;
        };
        auto *fromBox = new QWidget;
        auto *fl = new QHBoxLayout(fromBox);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(4);
        auto *src = new QComboBox;
        src->setObjectName(QStringLiteral("opt_rcFromSource"));
        src->addItems({QStringLiteral("Swatch"), QStringLiteral("Image")});
        src->setToolTip(QStringLiteral("Swatch: replace the From colour. Image: the colour under the pointer when the stroke starts.\nAlt-click the image to pick the From colour."));
        connect(src, &QComboBox::currentIndexChanged, this, [this](int i) { if (!m_updating) { appState().rcFromImage = i == 1; appState().notifyOptions(); } });
        g.sync([src] { src->setCurrentIndex(appState().rcFromImage ? 1 : 0); });
        fl->addWidget(src);
        fl->addWidget(swatch(QStringLiteral("opt_rcFrom"), &AppState::rcFrom));
        g.add(QStringLiteral("From:"), fromBox);
        g.add(QStringLiteral("To:"), swatch(QStringLiteral("opt_rcTo"), &AppState::rcTo));
        g.newColumn();
        g.intSlider(QStringLiteral("Tolerance:"), QStringLiteral("opt_rcTolerance"), &AppState::rcTolerance, 0, 100, QStringLiteral("%"), 1, 100);
        g.combo(QStringLiteral("Range:"), QStringLiteral("opt_rcRange"), {QStringLiteral("Solid"), QStringLiteral("Color")}, &AppState::rcRange);
        g.intSlider(QStringLiteral("Strength:"), QStringLiteral("opt_opacity"), &AppState::toolOpacity, 1, 100, QStringLiteral("%"), 1, 100);
        g.check(QStringLiteral("Pixel perfect"), QStringLiteral("opt_rcPixelPerfect"), &AppState::rcPixelPerfect)
            ->setToolTip(QStringLiteral("Aliased tip, every pixel fully replaced or kept (pixel art)"));
        pressureRow(true, QString(), true, false);
        break;
    }
    case Tool::RedEye:
        // Red Eye Removal: a brush that desaturates and darkens red-dominant pixels, keeping the catchlight
        g.tip();
        g.intSlider(QStringLiteral("Tolerance:"), QStringLiteral("opt_reTolerance"), &AppState::reTolerance, 0, 100, QStringLiteral("%"), 0, 100)
            ->setToolTip(QStringLiteral("How little red is enough: higher also fixes orange-ish and pinkish pupils"));
        g.intSlider(QStringLiteral("Darken:"), QStringLiteral("opt_reDarken"), &AppState::reDarken, 0, 100, QStringLiteral("%"), 0, 100)
            ->setToolTip(QStringLiteral("How much darker the corrected pupil becomes"));
        g.newColumn();
        g.intSlider(QStringLiteral("Strength:"), QStringLiteral("opt_opacity"), &AppState::toolOpacity, 1, 100, QStringLiteral("%"), 1, 100);
        pressureRow(true, QStringLiteral("Strength"), false, false);
        break;
    case Tool::Eraser:
        g.tip();
        g.intSlider(QStringLiteral("Opacity:"), QStringLiteral("opt_opacity"), &AppState::toolOpacity, 1, 100, QStringLiteral("%"), 1, 100);
        pressureRow(true, QStringLiteral("Opacity"), true, false);
        break;
    case Tool::Blur: case Tool::Sharpen:
        g.tip();
        g.intSlider(QStringLiteral("Intensity:"), QStringLiteral("toolIntensity"), &AppState::toolIntensity, 1, 100, QStringLiteral("%"), 1, 100);
        pressureRow(true, QStringLiteral("Intensity"), true, false);
        break;
    case Tool::Dodge: case Tool::Burn:
        g.tip();
        g.combo(QStringLiteral("Range:"), QStringLiteral("toolRange"),
                {QStringLiteral("Shadows"), QStringLiteral("Midtones"), QStringLiteral("Highlights")}, &AppState::toneRange);
        g.intSlider(QStringLiteral("Exposure:"), QStringLiteral("toolIntensity"), &AppState::toolIntensity, 1, 100, QStringLiteral("%"), 1, 100);
        pressureRow(true, QStringLiteral("Exposure"), true, false);
        break;
    case Tool::Smudge: {
        g.tip();
        g.intSlider(QStringLiteral("Pressure:"), QStringLiteral("toolIntensity"), &AppState::toolIntensity, 1, 100, QStringLiteral("%"), 1, 100);
        // Smudge Color: checkbox + swatch on one row
        auto *box = new QWidget;
        auto *bl = new QHBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(4);
        auto *c = new QCheckBox(QStringLiteral("Smudge color"));
        c->setObjectName(QStringLiteral("opt_smudgeUseColor"));
        auto *sw = new ColorButton(nullptr, false);
        sw->setObjectName(QStringLiteral("opt_smudgeColor"));
        sw->setFixedSize(32, 20);
        bl->addWidget(c);
        bl->addWidget(sw);
        connect(c, &QCheckBox::toggled, this, [this](bool v) { if (!m_updating) { appState().smudgeUseColor = v; appState().notifyOptions(); } });
        connect(sw, &ColorButton::colorPicked, this, [](const QColor &col) { appState().smudgeColor = col; appState().notifyOptions(); });
        g.sync([c, sw] { c->setChecked(appState().smudgeUseColor); sw->setColor(appState().smudgeColor); sw->setEnabled(appState().smudgeUseColor); });
        g.add(QString(), box, 2);
        g.check(QStringLiteral("Use entire document"), QStringLiteral("opt_useEntireDoc"), &AppState::useEntireDocument);
        pressureRow(false, QStringLiteral("Strength"), true, false);   // the smudge tip keeps the size it starts with
        break;
    }
    case Tool::RubberStamp:
        g.tip();
        g.check(QStringLiteral("Source aligned"), QStringLiteral("opt_stampAligned"), &AppState::stampAligned);
        g.check(QStringLiteral("Use entire document"), QStringLiteral("opt_useEntireDoc"), &AppState::useEntireDocument);
        g.opacityBlend();
        pressureRow(true, QStringLiteral("Opacity"), true, false);
        break;
    case Tool::Healing: case Tool::SpotHealing:
        g.tip();
        g.combo(QStringLiteral("Mode:"), QStringLiteral("opt_healMode"),
                {QStringLiteral("Normal"), QStringLiteral("Replace"), QStringLiteral("Lighten"), QStringLiteral("Darken")}, &AppState::healMode);
        if (t == Tool::Healing) {
            g.check(QStringLiteral("Source aligned"), QStringLiteral("opt_stampAligned"), &AppState::stampAligned);
            g.check(QStringLiteral("Use entire document"), QStringLiteral("opt_useEntireDoc"), &AppState::useEntireDocument);
        }
        pressureRow(true, QString(), true, false);
        break;
    case Tool::Marquee: case Tool::OvalMarquee: {
        g.combo(QStringLiteral("Style:"), QStringLiteral("opt_selStyle"),
                {QStringLiteral("Normal"), QStringLiteral("Fixed Ratio"), QStringLiteral("Fixed Size")}, &AppState::marqueeStyle);
        auto *wf = sdspin(0.01, 30000, 2, {}, 1, 100, 56);
        auto *hf = sdspin(0.01, 30000, 2, {}, 1, 100, 56);
        wf->setObjectName(QStringLiteral("opt_selW"));
        hf->setObjectName(QStringLiteral("opt_selH"));
        for (auto *f : {wf, hf}) f->setKeyboardTracking(false);
        connect(wf, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (m_updating) return;
            if (appState().marqueeStyle == AppState::StyleFixedSize) appState().marqueeFixedW = qMax(1, qRound(v)); else appState().marqueeRatioW = v;
            appState().notifyOptions();
        });
        connect(hf, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (m_updating) return;
            if (appState().marqueeStyle == AppState::StyleFixedSize) appState().marqueeFixedH = qMax(1, qRound(v)); else appState().marqueeRatioH = v;
            appState().notifyOptions();
        });
        g.sync([wf, hf] {
            const AppState &a = appState();
            const bool fixed = a.marqueeStyle == AppState::StyleFixedSize;
            for (auto *f : {wf, hf}) { f->setDecimals(fixed ? 0 : 2); f->setEnabled(a.marqueeStyle != AppState::StyleNormal); }
            wf->setValue(fixed ? a.marqueeFixedW : a.marqueeRatioW);
            hf->setValue(fixed ? a.marqueeFixedH : a.marqueeRatioH);
        });
        g.add(QStringLiteral("W:"), wf);
        g.add(QStringLiteral("H:"), hf);
        selEdge(&AppState::marqueeEdge, &AppState::marqueeFeather);
        break;
    }
    case Tool::Lasso: case Tool::PolyLasso:
        selEdge(&AppState::lassoEdge, &AppState::lassoFeather);
        break;
    case Tool::MagicWand:
        g.intSlider(QStringLiteral("Tolerance:"), QStringLiteral("opt_tolerance"), &AppState::tolerance, 0, 255, {}, 0, 255);
        selEdge(&AppState::wandEdge, &AppState::wandFeather);
        g.check(QStringLiteral("Contiguous"), QStringLiteral("opt_contiguous"), &AppState::contiguous);
        g.check(QStringLiteral("Sample all layers"), QStringLiteral("opt_wandSampleAll"), &AppState::wandSampleAllLayers);
        break;
    case Tool::PaintBucket:
        g.intSlider(QStringLiteral("Tolerance:"), QStringLiteral("opt_tolerance"), &AppState::tolerance, 0, 255, {}, 0, 255);
        g.check(QStringLiteral("Fill selection only"), QStringLiteral("opt_fillSelectionOnly"), &AppState::fillSelectionOnly);
        g.check(QStringLiteral("Fill selected object"), QStringLiteral("opt_fillSelectedObject"), &AppState::fillSelectedObject);
        g.fillEdge();
        g.texture();
        g.opacityBlend();
        break;
    case Tool::Gradient: {
        QStringList kinds;
        const QStringList labels = fills::fillKindLabels();
        for (int k = int(FillKind::Linear); k < labels.size(); ++k) kinds << labels[k];
        g.combo(QStringLiteral("Type:"), QStringLiteral("toolGradKind"), kinds,
                [] { return appState().gradientKind - int(FillKind::Linear); },
                [](int i) { appState().gradientKind = i + int(FillKind::Linear); });
        // Edit...: the tool's own colours; the swatch shows them
        auto *edit = new QPushButton(QStringLiteral("Edit\u2026"));
        edit->setObjectName(QStringLiteral("opt_gradEdit"));
        edit->setProperty("lwAction", true);
        edit->setToolTip(QStringLiteral("Edit the gradient colours the tool applies (default: fill colour to white)"));
        auto *ramp = new QLabel;
        ramp->setObjectName(QStringLiteral("opt_gradRamp"));
        ramp->setFixedSize(64, 16);
        ramp->setFrameShape(QFrame::Box);
        auto stops = [] {
            if (appState().gradientStops.size() >= 2) return appState().gradientStops;
            return QVector<GradStop>{GradStop{0, appState().fill}, GradStop{1, Qt::white}};
        };
        connect(edit, &QPushButton::clicked, this, [this, stops] {
            GradientDialog dlg(stops(), this);
            if (dlg.exec() != QDialog::Accepted) return;
            appState().gradientStops = dlg.stops();
            appState().notifyOptions();
        });
        g.sync([ramp, stops] {
            QPixmap pm(ramp->size());
            QPainter p(&pm);
            QLinearGradient lg(0, 0, pm.width(), 0);
            for (const GradStop &s : stops()) lg.setColorAt(s.pos, s.color);
            p.fillRect(pm.rect(), lg);
            p.end();
            ramp->setPixmap(pm);
        });
        auto *box = new QWidget;
        auto *bl = new QHBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(4);
        bl->addWidget(ramp);
        bl->addWidget(edit);
        g.add(QStringLiteral("Colors:"), box, 2);
        g.check(QStringLiteral("Fill selected object"), QStringLiteral("opt_fillSelectedObject"), &AppState::fillSelectedObject);
        g.fillEdge();
        g.texture();
        break;
    }
    case Tool::Eyedropper:
        g.combo(QStringLiteral("Sample:"), QStringLiteral("opt_eyedropperSample"),
                {QStringLiteral("1 Pixel"), QStringLiteral("3x3 Average"), QStringLiteral("5x5 Average")}, &AppState::eyedropperSample);
        break;
    case Tool::Line: case Tool::Pen: case Tool::VectorPath: case Tool::RedrawPath:
        strokeFill(t != Tool::Line);
        if (t == Tool::VectorPath || t == Tool::RedrawPath) {
            g.newColumn();
            g.intSlider(QStringLiteral("Precision:"), QStringLiteral("opt_pathPrecision"), &AppState::pathPrecision, 1, 10, {}, 1, 10);
            g.check(QStringLiteral("Pressure"), QStringLiteral("opt_pathPressure"), &AppState::pathPressure)
                ->setToolTip(QStringLiteral("A tablet pen's pressure scales the stroke width"));
        }
        break;
    case Tool::Rect: case Tool::Ellipse:
        strokeFill(true);
        if (t == Tool::Rect) g.dblSlider(QStringLiteral("Roundness:"), QStringLiteral("opt_roundness"), &AppState::roundness, 0, 100, 0,
                                         QStringLiteral("%"), 0, 100);
        break;
    case Tool::Polygon: {
        strokeFill(true);
        g.newColumn();
        g.combo(QStringLiteral("Shape:"), QStringLiteral("opt_polygonShape"), {QStringLiteral("Polygon"), QStringLiteral("Star")},
                [] { return appState().polygonStar ? 1 : 0; }, [](int i) { appState().polygonStar = i == 1; });
        g.intSlider(QStringLiteral("Sides:"), QStringLiteral("opt_polygonSides"), &AppState::polygonSides, 3, 360, {}, 3, 25);
        auto *box = new QWidget;
        auto *bl = new QHBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(4);
        auto *ang = sdspin(1, 179, 0, QStringLiteral("\u00B0"), 1, 179, 44);
        ang->setObjectName(QStringLiteral("opt_polygonAngle"));
        ang->setKeyboardTracking(false);
        auto *aut = new QCheckBox(QStringLiteral("Automatic"));
        aut->setObjectName(QStringLiteral("opt_polygonAutoAngle"));
        bl->addWidget(ang);
        bl->addWidget(aut);
        connect(ang, &QDoubleSpinBox::valueChanged, this, [this](double v) { if (!m_updating) { appState().polygonAngle = v; appState().notifyOptions(); } });
        connect(aut, &QCheckBox::toggled, this, [this](bool v) { if (!m_updating) { appState().polygonAutoAngle = v; appState().notifyOptions(); } });
        g.sync([ang, aut] {
            const AppState &a = appState();
            ang->setValue(a.polygonAngle);
            aut->setChecked(a.polygonAutoAngle);
            ang->setEnabled(a.polygonStar && !a.polygonAutoAngle);
            aut->setEnabled(a.polygonStar);
        });
        g.add(QStringLiteral("Angle:"), box, 2);
        break;
    }
    case Tool::Text: {
        auto *font = new WideFontComboBox;
        if (auto *lv = qobject_cast<QListView *>(font->view())) lv->setUniformItemSizes(true);
        font->setObjectName(QStringLiteral("toolFont"));
        font->setFixedWidth(150);
                connect(font, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, font](int) {
                    if (!m_updating) { appState().fontFamily = font->currentFamily(); appState().notifyOptions(); }
                });
        g.sync([font] { if (font->currentFamily() != appState().fontFamily) font->setCurrentFamily(appState().fontFamily); });
        g.add(QString(), font, 2);
        // size + presets, colour
        auto *row = new QWidget;
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(4);
        QDoubleSpinBox *size = fontSizeField(QStringLiteral("toolFontSize"));
        size->setKeyboardTracking(false);
        connect(size, &QDoubleSpinBox::valueChanged, this, [this](double v) { if (!m_updating) { appState().fontSize = v; appState().notifyOptions(); } });
        auto *col = new ColorButton(nullptr, true);
        col->setObjectName(QStringLiteral("toolTextColor"));
        col->setFixedSize(32, 20);
        connect(col, &ColorButton::colorPicked, this, [](const QColor &c) { appState().textColor = c; appState().notifyOptions(); });
        rl->addWidget(withPresets(size));
        rl->addWidget(col);
        g.sync([size, col] { size->setValue(appState().fontSize); col->setColor(appState().textColor); });
        g.add(QString(), row, 2);
        // B I U  | L C R
        auto *styles = new QWidget;
        auto *sl = new QHBoxLayout(styles);
        sl->setContentsMargins(0, 0, 0, 0);
        sl->setSpacing(1);
        auto toggle = [&](const QString &txt, const QString &tip, const QString &name, bool AppState::*m) {
            QToolButton *b = tbtn(txt, tip, true);
            b->setObjectName(name);
            connect(b, &QToolButton::toggled, this, [this, m](bool v) { if (!m_updating) { appState().*m = v; appState().notifyOptions(); } });
            g.sync([b, m] { b->setChecked(appState().*m); });
            sl->addWidget(b);
            return b;
        };
        QFont bf; bf.setBold(true);
        toggle(QStringLiteral("B"), QStringLiteral("Bold"), QStringLiteral("toolBold"), &AppState::bold)->setFont(bf);
        QFont itf; itf.setItalic(true);
        toggle(QStringLiteral("I"), QStringLiteral("Italic"), QStringLiteral("toolItalic"), &AppState::italic)->setFont(itf);
        QFont uf; uf.setUnderline(true);
        toggle(QStringLiteral("U"), QStringLiteral("Underline"), QStringLiteral("toolUnderline"), &AppState::underline)->setFont(uf);
        sl->addSpacing(6);
        const QStringList alignNames{QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right"), QStringLiteral("Justify")};
        for (int i = 0; i < 4; ++i) {
            QToolButton *b = tbtn(alignNames[i].left(1), QStringLiteral("Align %1").arg(alignNames[i].toLower()), true);
            b->setObjectName(QStringLiteral("toolAlign") + alignNames[i]);
            b->setAutoExclusive(true);
            connect(b, &QToolButton::clicked, this, [this, i] { if (!m_updating) { appState().textAlign = i; appState().notifyOptions(); } });
            g.sync([b, i] { b->setChecked(appState().textAlign == i); });
            sl->addWidget(b);
        }
        g.add(QString(), styles, 2);
        g.newColumn();
        g.combo(QStringLiteral("Anti-alias:"), QStringLiteral("toolTextAA"), textAANames(),
                [] { return int(appState().textAA); }, [](int i) { appState().textAA = TextAA(i); });
        g.dblSlider(QStringLiteral("Kern:"), QStringLiteral("toolKerning"), &AppState::kerning, -100, 100, 0, QStringLiteral("%"), -50, 50, 44);
        g.dblSlider(QStringLiteral("Lead:"), QStringLiteral("toolLeading"), &AppState::leading, 0, 1000, 0, QStringLiteral("%"), 50, 300, 48);
        g.newColumn();
        g.dblSlider(QStringLiteral("Scale:"), QStringLiteral("toolHScale"), &AppState::hScale, 1, 1000, 0, QStringLiteral("%"), 25, 400, 48);
        g.dblSlider(QStringLiteral("Shift:"), QStringLiteral("toolBaseline"), &AppState::baselineShift, -1000, 1000, 0, QStringLiteral(" px"), -50, 50, 48);
        g.combo(QStringLiteral("Orient:"), QStringLiteral("toolTextOrient"), {QStringLiteral("Horizontal"), QStringLiteral("Vertical")},
                &AppState::textOrientation);
        g.combo(QStringLiteral("Dir:"), QStringLiteral("toolTextDir"), {QStringLiteral("LTR"), QStringLiteral("RTL")},
                &AppState::textDirection);
        g.dblSlider(QStringLiteral("Indent:"), QStringLiteral("toolIndent"), &AppState::paraIndent, 0, 10000, 0, QStringLiteral(" px"), 0, 200, 48);
        g.dblSlider(QStringLiteral("Before:"), QStringLiteral("toolSpaceBefore"), &AppState::paraSpaceBefore, 0, 10000, 0, QStringLiteral(" px"), 0, 200, 48);
        g.dblSlider(QStringLiteral("After:"), QStringLiteral("toolSpaceAfter"), &AppState::paraSpaceAfter, 0, 10000, 0, QStringLiteral(" px"), 0, 200, 48);
        break;
    }
    case Tool::Knife:
        note(QStringLiteral("The Knife tool has no options: drag across a selected path to cut it."));
        break;
    case Tool::Freeform:
        pathTool(false);
        break;
    case Tool::ReshapeArea:
        pathTool(true);
        break;
    case Tool::PathScrubber:
        // Fireworks' Path Scrubber: raises or lowers the pressure data of the paths under the brush
        g.combo(QStringLiteral("Mode:"), QStringLiteral("opt_psMode"), {QStringLiteral("Increase (+)"), QStringLiteral("Decrease (\u2212)")},
                [] { return appState().psMode; }, [](int i) { appState().psMode = i; })
            ->setToolTip(QStringLiteral("Hold Alt while dragging for the opposite; X swaps the mode"));
        g.check(QStringLiteral("Size"), QStringLiteral("opt_psAffectSize"), &AppState::psAffectSize)
            ->setToolTip(QStringLiteral("The pressure data scales the stroke's width"));
        g.check(QStringLiteral("Opacity"), QStringLiteral("opt_psAffectOpacity"), &AppState::psAffectOpacity)
            ->setToolTip(QStringLiteral("The pressure data fades the stroke"));
        g.newColumn();
        g.intSlider(QStringLiteral("Strength:"), QStringLiteral("opt_psStrength"), &AppState::psStrength, 1, 100, QStringLiteral("%"), 1, 100);
        g.intSlider(QStringLiteral("Size:"), QStringLiteral("opt_psSize"), &AppState::psSize, 1, 1000, QStringLiteral(" px"), 1, 200);
        g.check(QStringLiteral("Pressure"), QStringLiteral("opt_pathPressure"), &AppState::pathPressure)
            ->setToolTip(QStringLiteral("A tablet pen's pressure scales the size"));
        break;
    case Tool::Crop: {
        // W / H / X / Y of the crop box (document px) plus Crop / Cancel, like Fireworks
        auto field = [&](const QString &label, const QString &name, int which) {
            auto *f = sdspin(-30000, 30000, 0, QStringLiteral(" px"), 0, 2000, 56);
            f->setObjectName(name);
            f->setKeyboardTracking(false);
            f->setProperty("lwState", QStringLiteral("crop"));
            connect(f, &QDoubleSpinBox::valueChanged, this, [this, which](double v) {
                if (m_updating) return;
                QRectF r = appState().cropRect;
                if (which == 0) r.setWidth(qMax(1.0, v)); else if (which == 1) r.setHeight(qMax(1.0, v));
                else if (which == 2) r.moveLeft(v); else r.moveTop(v);
                appState().cropRect = r;
                emit appState().cropEdited(r);
            });
            g.sync([f, which] {
                const QRectF r = appState().cropRect;
                f->setValue(which == 0 ? r.width() : which == 1 ? r.height() : which == 2 ? r.x() : r.y());
            });
            g.add(label, f);
        };
        field(QStringLiteral("W:"), QStringLiteral("opt_cropW"), 0);
        field(QStringLiteral("H:"), QStringLiteral("opt_cropH"), 1);
        g.newColumn();
        field(QStringLiteral("X:"), QStringLiteral("opt_cropX"), 2);
        field(QStringLiteral("Y:"), QStringLiteral("opt_cropY"), 3);
        g.newColumn();
        auto *crop = new QPushButton(QStringLiteral("Crop"));
        crop->setObjectName(QStringLiteral("opt_cropApply"));
        crop->setProperty("lwAction", true);
        auto *cancel = new QPushButton(QStringLiteral("Cancel"));
        cancel->setObjectName(QStringLiteral("opt_cropCancel"));
        cancel->setProperty("lwAction", true);
        connect(crop, &QPushButton::clicked, this, [] { emit appState().cropCommand(true); });
        connect(cancel, &QPushButton::clicked, this, [] { emit appState().cropCommand(false); });
        g.sync([crop, cancel] { crop->setEnabled(appState().cropActive); cancel->setEnabled(appState().cropActive); });
        g.add(QString(), crop);
        g.add(QString(), cancel);
        break;
    }
    case Tool::ExportArea: {
        // X / Y / W / H of the export area (whole document px) plus Export... / Cancel, like Fireworks
        auto field = [&](const QString &label, const QString &name, int which) {
            auto *f = sdspin(0, 30000, 0, QStringLiteral(" px"), 0, 2000, 56);
            f->setObjectName(name);
            f->setKeyboardTracking(false);
            f->setProperty("lwState", QStringLiteral("exportarea"));
            connect(f, &QDoubleSpinBox::valueChanged, this, [this, which](double v) {
                if (m_updating) return;
                QRect r = appState().exportAreaRect;
                if (r.isEmpty()) r = QRect(0, 0, 100, 100);
                const int iv = qRound(v);
                if (which == 0) r.moveLeft(iv); else if (which == 1) r.moveTop(iv);
                else if (which == 2) r.setWidth(qMax(1, iv)); else r.setHeight(qMax(1, iv));
                appState().exportAreaRect = r;
                emit appState().exportAreaEdited(r);
            });
            g.sync([f, which] {
                const QRect r = appState().exportAreaRect;
                f->setValue(which == 0 ? r.x() : which == 1 ? r.y() : which == 2 ? r.width() : r.height());
            });
            g.add(label, f);
        };
        field(QStringLiteral("X:"), QStringLiteral("opt_areaX"), 0);
        field(QStringLiteral("Y:"), QStringLiteral("opt_areaY"), 1);
        g.newColumn();
        field(QStringLiteral("W:"), QStringLiteral("opt_areaW"), 2);
        field(QStringLiteral("H:"), QStringLiteral("opt_areaH"), 3);
        g.newColumn();
        auto *exp = new QPushButton(QStringLiteral("Export..."));
        exp->setObjectName(QStringLiteral("opt_areaExport"));
        exp->setProperty("lwAction", true);
        auto *cancel = new QPushButton(QStringLiteral("Cancel"));
        cancel->setObjectName(QStringLiteral("opt_areaCancel"));
        cancel->setProperty("lwAction", true);
        connect(exp, &QPushButton::clicked, this, [] { emit appState().exportAreaCommand(true); });
        connect(cancel, &QPushButton::clicked, this, [] { emit appState().exportAreaCommand(false); });
        g.sync([exp, cancel] { exp->setEnabled(appState().exportAreaActive); cancel->setEnabled(appState().exportAreaActive); });
        g.add(QString(), exp);
        g.add(QString(), cancel);
        break;
    }
    case Tool::Slice: case Tool::PolySlice: {
        {
            auto *shape = new QComboBox;
            shape->setObjectName(QStringLiteral("opt_sliceShape"));
            shape->setProperty("lwState", QStringLiteral("tool"));
            shape->addItems({QStringLiteral("Rectangle"), QStringLiteral("Polygon")});
            connect(shape, &QComboBox::currentIndexChanged, this, [this](int i) {
                if (m_updating || i < 0) return;
                const Tool nt = i == 1 ? Tool::PolySlice : Tool::Slice;
                QTimer::singleShot(0, &appState(), [nt] { appState().setTool(nt); });
            });
            g.sync([shape] {
                shape->setCurrentIndex(appState().tool() == Tool::PolySlice ? 1 : 0);
            });
            g.add(QStringLiteral("Shape:"), shape);
        }
        if (webFeaturesEnabled()) link();
        break;
    }
    case Tool::Hotspot: case Tool::CircleHotspot: case Tool::PolyHotspot: {
        auto *shape = new QComboBox;
        shape->setObjectName(QStringLiteral("opt_hotspotShape"));
        shape->setProperty("lwState", QStringLiteral("tool"));
        shape->addItems({QStringLiteral("Rectangle"), QStringLiteral("Circle"), QStringLiteral("Polygon")});
        connect(shape, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (m_updating || i < 0) return;
            const Tool nt = i == 1 ? Tool::CircleHotspot : i == 2 ? Tool::PolyHotspot : Tool::Hotspot;
            QTimer::singleShot(0, &appState(), [nt] { appState().setTool(nt); });   // rebuilds this page
        });
        g.sync([shape] {
            const Tool cur = appState().tool();
            shape->setCurrentIndex(cur == Tool::CircleHotspot ? 1 : cur == Tool::PolyHotspot ? 2 : 0);
        });
        g.add(QStringLiteral("Shape:"), shape);
        link();
        break;
    }
    default:
        if (toolIsAutoShape(t)) {
            const QString kind = toolAutoShapeKind(t);
            strokeFill(!autoshape::isLineKind(kind));
            g.newColumn();
            for (const autoshape::Param &pr : autoshape::params(kind)) {
                const QString key = pr.key, name = QStringLiteral("opt_as_") + pr.key;
                auto get = [kind, key] { return appState().autoShapeParamsFor(kind).value(key); };
                if (!pr.choices.isEmpty())
                    g.combo(pr.label + QLatin1Char(':'), name, pr.choices, [get] { return qRound(get()); },
                            [kind, key](int i) { appState().autoShapeParams[kind][key] = i; });
                else if (asPercent(pr))   // ratios are shown as percentages
                    g.dblSlider(pr.label + QLatin1Char(':'), name, [get] { return get() * 100; },
                                [kind, key](double v) { appState().autoShapeParams[kind][key] = v / 100; },
                                pr.min * 100, pr.max * 100, 0, QStringLiteral("%"));
                else
                    g.dblSlider(pr.label + QLatin1Char(':'), name, get, [kind, key](double v) { appState().autoShapeParams[kind][key] = v; },
                                pr.min, pr.max, pr.decimals, pr.suffix);
            }
            break;
        }
        note(QStringLiteral("No options."));
        break;
    }
    Q_UNUSED(as);
    m_toolOpts->show();
}

void PropertyInspector::setDocument(Document *doc) {
    if (m_doc) disconnect(m_doc, nullptr, this, nullptr);
    m_doc = doc;
    if (doc) {
        connect(doc, &Document::selectionChanged, this, &PropertyInspector::refresh);
        connect(doc, &Document::structureChanged, this, &PropertyInspector::refresh);
        connect(doc, &Document::historyChanged, this, &PropertyInspector::refresh);
        connect(doc, &Document::frameChanged, this, &PropertyInspector::refresh);
    }
    refresh();
}

void PropertyInspector::reorderEffects(int from, int to, bool *moved) {
    if (moved) *moved = false;
    if (from < 0 || to < 0 || from == to) return;
    bool did = false;
    applyToSelection(QStringLiteral("Reorder Effects"), [from, to, &did](VObject &o) {
        if (from >= o.effects.size() || to < 0 || to >= o.effects.size()) return false;
        o.effects.move(from, to);
        did = true;
        return true;
    });
    if (moved) *moved = did;
}

template <class F> void PropertyInspector::applyToSelection(const QString &label, F f) {
    if (m_updating || !m_doc) return;
    bool any = false;
    for (VObject *o : m_doc->selectedEditable()) any |= f(*o);
    if (any) m_doc->commit(label);
}

void PropertyInspector::editGradient() {
    if (!m_doc) return;
    auto sel = m_doc->selectedEditable();
    VObject *first = nullptr;
    for (VObject *o : sel) if (o->isVector()) { first = o; break; }
    if (!first) return;
    GradientDialog dlg(fills::effectiveGradient(*first), this);
    if (dlg.exec() != QDialog::Accepted) return;
    const QVector<GradStop> st = dlg.stops();
    applyToSelection(QStringLiteral("Edit Gradient"), [st](VObject &o) {
        if (!o.isVector()) return false;
        if (!fills::isGradient(o.fillKind)) fills::setFillKind(o, FillKind::Linear);
        o.gradient = st;
        o.fillOn = true;
        return true;
    });
}



void PropertyInspector::applyStrokeParams(const stroke::Params &sp) {
    auto applyObj = [sp](VObject &o) -> bool {
        if (!o.isVector()) return false;
        const QString key = stroke::canonicalKey(sp.key.isEmpty() ? o.strokeKind : sp.key);
        if (o.strokeCustom && o.strokeParams == sp && o.strokeKind == key && o.strokeOn) return false;
        o.strokeKind = key;
        o.strokeParams = sp;
        o.strokeCustom = true;
        o.strokeOn = true;
        if (sp.size > 0)
            o.strokeWidth = sp.size;
        return true;
    };
    // With the bitmap Brush tool active, Advanced… / Edit Stroke always write the tip's
    // stroke params (keyed to the current tip so Canvas::beginBrushStroke applies them).
    const bool brushTip = appState().tool() == Tool::Brush;
    bool anySel = false;
    if (!brushTip && m_doc) {
        for (VObject *o : m_doc->selectedEditable()) if (o->isVector()) { anySel = true; break; }
    }
    if (anySel) {
        applyToSelection(QStringLiteral("Edit Stroke"), applyObj);
        return;
    }
    stroke::Params out = sp;
    if (brushTip) {
        const QString key = currentBrushTipKey();
        out.key = key;
        auto it = appState().brushStrokeByTip.constFind(key);
        if (it != appState().brushStrokeByTip.cend() && *it == out
            && appState().brushSize == qBound(1, int(qRound(out.size > 0 ? out.size : appState().brushSize)), 1000)
            && appState().brushEdge == out.edge
            && (appState().tipShape == 1) == out.square
            && appState().brushTexture == out.texture
            && appState().brushTextureAmount == out.textureAmount)
            return;
        appState().brushStrokeByTip.insert(key, out);
        if (out.size > 0)
            appState().brushSize = qBound(1, int(qRound(out.size)), 1000);
        appState().brushEdge = out.edge;
        appState().tipShape = out.square ? 1 : 0;
        appState().brushTexture = out.texture;
        appState().brushTextureAmount = out.textureAmount;
        appState().notifyOptions();
        return;
    }
    QString key = stroke::canonicalKey(sp.key.isEmpty() ? appState().strokeKind : sp.key);
    out.key = key;
    if (appState().strokeCustom && appState().strokeParams == out && appState().strokeKind == key && appState().strokeOn)
        return;
    appState().strokeKind = key;
    appState().strokeParams = out;
    appState().strokeCustom = true;
    appState().strokeOn = true;
    appState().notifyOptions();
}

PropertyInspector::EditStrokeTarget PropertyInspector::editStrokeTarget() const
{
    if (appState().tool() == Tool::Brush)
        return EditStrokeTarget::BrushTip;
    if (m_doc) {
        for (VObject *o : m_doc->selectedEditable())
            if (o->isVector()) return EditStrokeTarget::SelectedVector;
    }
    const Tool t = appState().tool();
    // Pointer with no selection still edits the vector default used by new shapes.
    // Bitmap tools (Pencil, stamp, retouch) have no stroke dialog target.
    const bool strokeTool = t == Tool::Pointer || t == Tool::Pen || t == Tool::VectorPath
        || t == Tool::RedrawPath || t == Tool::Line || t == Tool::Rect || t == Tool::Ellipse
        || t == Tool::Polygon || t == Tool::Text || toolIsAutoShape(t);
    if (strokeTool)
        return EditStrokeTarget::VectorDefault;
    return EditStrokeTarget::None;
}

void PropertyInspector::editStroke() {
    const EditStrokeTarget target = editStrokeTarget();
    if (target == EditStrokeTarget::None) return;
    const bool brushTip = target == EditStrokeTarget::BrushTip;
    VObject *cur = nullptr;
    if (target == EditStrokeTarget::SelectedVector && m_doc) {
        for (VObject *o : m_doc->selectedEditable()) if (o->isVector()) { cur = o; break; }
    }
    stroke::Params p;
    QColor col;
    double w;
    brush::Params brushSrc;
    quint32 seed = 0;
    if (brushTip) {
        brushSrc = liveBrushParams();
        const QString tipKey = stroke::canonicalKey(brushSrc.strokeKey);
        const AppState &as = appState();
        auto it = as.brushStrokeByTip.constFind(tipKey);
        if (it != as.brushStrokeByTip.cend())
            p = it.value();
        else
            p = brush::liveEditStrokeParams(brushSrc);
        p.key = tipKey;
        // Live PI fields always win so the dialog matches what the next stroke will paint.
        p.size = as.brushSize > 0 ? as.brushSize : p.size;
        p.edge = as.brushEdge;
        p.square = as.tipShape == 1;
        p.texture = as.brushTexture;
        p.textureAmount = as.brushTextureAmount;
        col = as.stroke;
        w = p.size > 0 ? p.size : 1;
        brushSrc.size = w;
        brushSrc.color = col;
        brushSrc.edge = p.edge;
        brushSrc.square = p.square;
        brushSrc.texture = p.texture;
        brushSrc.textureAmount = p.textureAmount;
        brushSrc.strokeKey = tipKey;
        brushSrc.strokeCustom = true;
        brushSrc.strokeParams = p;
    } else if (cur) {
        p = cur->strokeCustom ? cur->strokeParams : stroke::paramsFor(cur->strokeKind);
        col = cur->stroke;
        w = cur->strokeWidth > 0 ? cur->strokeWidth : p.size;
        seed = cur->strokeSeed ? cur->strokeSeed
              : (quint32(cur->id * 2654435761ull) ^ quint32(qHash(cur->strokeKind)));
    } else {
        p = appState().strokeCustom ? appState().strokeParams : stroke::paramsFor(appState().strokeKind);
        col = appState().stroke;
        w = appState().strokeWidth > 0 ? appState().strokeWidth : p.size;
    }
    EditStrokeDialog dlg(this);
    if (m_doc) {
        QVector<QPair<quint64, QString>> syms;
        for (const Symbol &sy : m_doc->s.symbols)
            syms.append({sy.id, sy.name.isEmpty() ? QString::number(sy.id) : sy.name});
        dlg.setLibrarySymbols(syms);
    }
    if (brushTip) dlg.setBrushSource(brushSrc);
    dlg.setParams(p, col, w, seed);
    // Sample-under options apply only to the bitmap Brush, not to vector strokes or tool defaults.
    dlg.setVectorStroke(!brushTip);
    connect(&dlg, &EditStrokeDialog::applied, this, [this](const stroke::Params &sp) { applyStrokeParams(sp); });
    if (dlg.exec() == QDialog::Accepted) applyStrokeParams(dlg.params());
}

void PropertyInspector::editProcedural(int which) {
    if (!m_doc) { refresh(); return; }
    VObject *first = nullptr;
    for (VObject *o : m_doc->selectedEditable()) if (o->isVector()) { first = o; break; }
    if (!first) { refresh(); return; }
    const QString cur = which == 0 ? first->textureProc : which == 1 ? first->patternProc : first->strokeTextureProc;
    proc::Params p;
    if (!cur.isEmpty()) p = proc::fromString(cur);
    else if (which == 1) { p.gen = QStringLiteral("marble"); p.ramp = fills::gradientPreset(fills::gradientPresetNames().value(0)); }
    else p.gen = QStringLiteral("fbm");
    ProceduralDialog dlg(p, which == 1 ? ProceduralDialog::Mode::Pattern : ProceduralDialog::Mode::Texture, {}, this);
    if (dlg.exec() != QDialog::Accepted) { refresh(); return; }
    const QString json = proc::toString(dlg.params());
    applyToSelection(which == 1 ? QStringLiteral("Fill Pattern") : which == 0 ? QStringLiteral("Texture") : QStringLiteral("Stroke Texture"), [which, json](VObject &o) {
        if (!o.isVector()) return false;
        if (which == 1) {
            o.fillOn = true;
            o.fillKind = FillKind::Pattern;
            o.pattern = QStringLiteral("Procedural");
            o.patternProc = json;
        } else if (which == 0) {
            o.texture = QStringLiteral("Procedural");
            o.textureProc = json;
            if (o.textureAmount == 0) o.textureAmount = 50;
        } else {
            o.strokeTexture = QStringLiteral("Procedural");
            o.strokeTextureProc = json;
            if (o.strokeTextureAmount == 0) o.strokeTextureAmount = 50;
        }
        return true;
    });
    refresh();
}

int PropertyInspector::contentHeight() {
    m_measuring = true;
    QPointer<Document> keep = m_doc;
    const int keepIdx = m_stack->currentIndex();
    m_stack->ensurePolished();
    int h = 0;
    auto measure = [&] {
        QWidget *pg = m_stack->currentWidget();
        pg->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        if (pg->layout()) { pg->layout()->invalidate(); pg->layout()->activate(); }
        h = qMax(h, pg->sizeHint().height());
    };
    // a scratch document with one of each kind of object
    std::unique_ptr<Document> scratch(Document::createNew(400, 300, Qt::white));
    QVector<QVector<quint64>> sels;
    auto add = [&](VObject o, int layer = 0) {
        o.id = scratch->newId();
        if (o.rect.isNull()) o.rect = QRectF(10, 10, 120, 80);
        scratch->addObject(o, layer, false);
        sels.append(QVector<quint64>{o.id});
        return o.id;
    };
    { VObject o; o.type = ObjType::Bitmap; o.image = QImage(20, 20, QImage::Format_ARGB32_Premultiplied); o.image.fill(Qt::red); add(o); }
    for (ObjType t : {ObjType::Rect, ObjType::Ellipse, ObjType::Line, ObjType::Path}) {
        VObject o; o.type = t; o.strokeOn = true;
        if (t == ObjType::Path) { o.points = {{10, 10}, {100, 20}, {60, 90}}; o.closed = true; }
        add(o);
    }
    for (FillKind k : {FillKind::Linear, FillKind::Pattern}) {
        VObject o; o.type = ObjType::Rect; o.fillOn = true;
        fills::setFillKind(o, k);
        if (k == FillKind::Pattern) o.pattern = fills::patternNames().value(0);
        add(o);
    }
    { VObject o; o.type = ObjType::Text; o.text = QStringLiteral("Aa"); add(o); }
    const quint64 a = sels.value(1).value(0), b = sels.value(2).value(0);
    { VObject g; g.type = ObjType::Group; for (quint64 id : {a, b}) if (VObject *x = scratch->find(id)) { VObject c = *x; c.id = scratch->newId(); g.children << c; } add(g); }
    for (const QString &k : autoshape::kinds()) { VObject o; o.type = ObjType::AutoShape; o.rect = QRectF(10, 10, 120, 80); autoshape::init(o, k); add(o); }
    const int wl = scratch->ensureWebLayer();
    { VObject o; o.type = ObjType::Slice; add(o, wl); }
    { VObject o; o.type = ObjType::Slice; o.webShape = WebShape::Polygon; o.points = {{10, 10}, {100, 20}, {60, 90}}; add(o, wl); }
    for (WebShape ws : {WebShape::Rect, WebShape::Circle, WebShape::Polygon}) {
        VObject o; o.type = ObjType::Hotspot; o.webShape = ws;
        if (ws == WebShape::Polygon) o.points = {{10, 10}, {100, 20}, {60, 90}};
        add(o, wl);
    }
    sels.append(QVector<quint64>{a, b});   // several objects
    m_doc = scratch.get();
    m_stack->setCurrentIndex(0);
    measure();
    for (const auto &ids : sels) {
        scratch->selection.clear();
        for (quint64 id : ids) scratch->selection.insert(id);
        m_stack->setCurrentIndex(1);
        refreshObject();
        measure();
    }
    // the current tool's options
    m_stack->setCurrentIndex(2);
    refreshTool();
    measure();
    m_doc = keep;
    m_stack->setCurrentIndex(keepIdx);
    m_measuring = false;
    if (m_doc) refresh();
    else if (keepIdx >= 0) for (int i = 0; i < m_stack->count(); ++i)
        m_stack->widget(i)->setSizePolicy(i == keepIdx ? QSizePolicy::Preferred : QSizePolicy::Ignored, i == keepIdx ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    return h;
}

void PropertyInspector::updateMinimumHeight() {
    if (!m_scroll || m_measuring) return;
    // every row in view, with room for the horizontal scroll bar a narrow window brings
    const int need = contentHeight() + m_scroll->horizontalScrollBar()->sizeHint().height() + 2;
    m_scroll->setMinimumHeight(need);
    updateGeometry();
}

void PropertyInspector::changeEvent(QEvent *e) {
    QWidget::changeEvent(e);
    // a theme or font change can make rows taller: measure again once things settle
    if ((e->type() == QEvent::StyleChange || e->type() == QEvent::FontChange) && !m_measuring) {
        static const char *pending = "lw_piMeasurePending";
        if (!property(pending).toBool()) {
            setProperty(pending, true);
            QTimer::singleShot(0, this, [this] { setProperty(pending, false); updateMinimumHeight(); });
        }
    }
}

void PropertyInspector::refresh() {
    if (!m_doc) {
        m_stack->setEnabled(false);
        return;
    }
    m_stack->setEnabled(true);
    const Tool t = appState().tool();
    // painting / selection tools always show their options; vector tools show the object
    // page while something is selected (Pointer-like tools show the document page otherwise)
    const bool paintTool = toolIsBitmap(t) || t == Tool::Crop;
    const bool toolPage = !toolUsesDocPage(t);
    // Freeform / Reshape Area act on the selection, so their options stay in view like the paint tools'
    const bool toolFirst = paintTool || t == Tool::Freeform || t == Tool::ReshapeArea || t == Tool::PathScrubber;
    if (!toolFirst && !m_doc->selection.isEmpty()) {
        m_stack->setCurrentIndex(1);
        refreshObject();
    } else if (toolPage) {
        m_stack->setCurrentIndex(2);
        refreshTool();
    } else {
        m_stack->setCurrentIndex(0);
        m_updating = true;
        m_docTitle->setText(QStringLiteral("Document: %1").arg(m_doc->title()));
        int nObj = 0;
        for (const auto &l : m_doc->frame().layers) nObj += l.size();
        m_docInfo->setText(QStringLiteral("%1 x %2 pixels   %3 layer(s), %4 frame(s), %5 object(s) in this frame")
                               .arg(m_doc->s.width).arg(m_doc->s.height).arg(m_doc->s.layers.size()).arg(m_doc->s.frames.size()).arg(nObj));
        QColor c = m_doc->s.canvasColor;
        m_canvasTransparent->setChecked(c.alpha() == 0);
        c.setAlpha(255);
        m_canvasColor->setColor(c);
        m_updating = false;
    }
}

void PropertyInspector::refreshShapeParams(const VObject *o) {
    m_shapeRow->setVisible(o);
    if (!o) return;
    auto *grid = static_cast<QGridLayout *>(m_shapeRow->layout());
    if (m_shapeRowKind != o->shapeKind) {
        m_shapeRowKind = o->shapeKind;
        while (QLayoutItem *it = grid->takeAt(0)) { delete it->widget(); delete it; }
        m_shapeSync.clear();
        auto *title = small(autoshape::label(o->shapeKind) + QStringLiteral(" (drag the yellow diamonds)"));
        title->setToolTip(autoshape::tip(o->shapeKind));
        grid->addWidget(title, 0, 0, 1, 8);
        int i = 0;
        for (const autoshape::Param &pr : autoshape::params(o->shapeKind)) {
            const int row = 1 + i % 2, col = (i / 2) * 2;
            ++i;
            const QString key = pr.key;
            grid->addWidget(small(pr.label + QLatin1Char(':')), row, col, Qt::AlignRight | Qt::AlignVCenter);
            auto apply = [this, key](double v) {
                if (m_updating) return;
                applyToSelection(QStringLiteral("Edit Auto Shape"), [key, v](VObject &x) {
                    if (x.type != ObjType::AutoShape || autoshape::param(x, key) == v) return false;
                    autoshape::setParam(x, key, v);
                    return true;
                });
            };
            if (!pr.choices.isEmpty()) {
                auto *c = new QComboBox;
                c->setObjectName(QStringLiteral("shape_") + key);
                c->addItems(pr.choices);
                connect(c, &QComboBox::currentIndexChanged, this, [apply](int k) { if (k >= 0) apply(k); });
                m_shapeSync.append([c, key](const VObject &x) { c->setCurrentIndex(qBound(0, qRound(autoshape::param(x, key)), c->count() - 1)); });
                grid->addWidget(c, row, col + 1);
            } else {
                const double f = asPercent(pr) ? 100 : 1;
                auto *sp = f > 1 ? sdspin(pr.min * f, pr.max * f, 0, QStringLiteral("%"), pr.min * f, pr.max * f, 58)
                                 : sdspin(pr.min, pr.max, pr.decimals, pr.suffix, pr.min, pr.max, 58);
                sp->setObjectName(QStringLiteral("shape_") + key);
                sp->setKeyboardTracking(false);
                connect(sp, &QDoubleSpinBox::valueChanged, this, [apply, f](double v) { apply(v / f); });
                m_shapeSync.append([sp, key, f](const VObject &x) { sp->setValue(autoshape::param(x, key) * f); });
                grid->addWidget(sp, row, col + 1);
            }
        }
        grid->setColumnStretch(grid->columnCount(), 1);
    }
    for (const auto &f : m_shapeSync) f(*o);
}

void PropertyInspector::refreshObject() {
    auto sel = m_doc->selectedObjects();
    if (sel.isEmpty()) return;
    m_updating = true;
    const VObject &o = *sel.first();
    m_objType->setText(sel.size() > 1 ? QStringLiteral("%1 objects").arg(sel.size()) : objTypeLabel(o.type));
    m_name->setText(sel.size() > 1 ? QString() : o.name);
    m_name->setEnabled(sel.size() == 1);
    QRectF b = sel.size() > 1 ? m_doc->selectionBounds() : o.bounds();
    m_w->setValue(b.width()); m_h->setValue(b.height()); m_x->setValue(b.x()); m_y->setValue(b.y());
    for (auto *s : {m_w, m_h, m_x, m_y}) s->setEnabled(sel.size() == 1);
    {
        int bmpCount = 0, firstMethod = -1;
        bool mixed = false;
        for (const VObject *x : sel) {
            if (x->type != ObjType::Bitmap) continue;
            ++bmpCount;
            const int m = x->effectiveInterpolation();
            if (firstMethod < 0) firstMethod = m;
            else if (m != firstMethod) mixed = true;
        }
        m_bmpScaleRow->setVisible(bmpCount > 0 && !o.isWeb());
        if (bmpCount > 0) {
            const bool block = m_bmpScale->blockSignals(true);
            if (mixed) m_bmpScale->setCurrentIndex(-1);
            else m_bmpScale->setCurrentIndex(qBound(0, firstMethod, m_bmpScale->count() - 1));
            m_bmpScale->blockSignals(block);
        }
    }
    const bool web = o.isWeb();
    m_artBox->setVisible(!web);
    m_webBox->setVisible(web);
    if (web) {
        m_objType->setText(sel.size() > 1 ? QStringLiteral("%1 objects").arg(sel.size())
                           : o.type == ObjType::Slice ? (o.webShape == WebShape::Polygon ? QStringLiteral("Polygon Slice") : QStringLiteral("Slice"))
                           : o.webShape == WebShape::Circle ? QStringLiteral("Circle Hotspot")
                           : o.webShape == WebShape::Polygon ? QStringLiteral("Polygon Hotspot") : QStringLiteral("Hotspot"));
        m_link->setText(o.link);
        m_alt->setText(o.alt);
        m_target->setCurrentText(o.target);
        m_behavior->setCurrentIndex(int(o.behavior));
        m_swapTarget->clear();
        m_swapTarget->addItem(QStringLiteral("(this slice)"), QVariant::fromValue<qulonglong>(0));
        if (int wl = m_doc->webLayer(); wl >= 0)
            for (const VObject &x : m_doc->objects(wl))
                if (x.type == ObjType::Slice && x.id != o.id) m_swapTarget->addItem(x.name, QVariant::fromValue<qulonglong>(x.id));
        m_swapTarget->setCurrentIndex(qMax(0, m_swapTarget->findData(QVariant::fromValue<qulonglong>(o.swapTarget))));
        m_swapFrame->setRange(1, qMax(1, int(m_doc->s.frames.size())));
        m_swapFrame->setValue(o.swapFrame + 1);
        const bool swap = o.behavior == WebBehavior::SwapImage;
        m_swapTarget->setEnabled(swap);
        m_swapFrame->setEnabled(swap);
        const bool webOn = webFeaturesEnabled();
        m_webPageBox->setVisible(webOn);
        m_sliceOptBox->setVisible(o.type == ObjType::Slice);
        const ExportOptions eo = o.sliceCustomOpt ? o.sliceOpt : m_doc->exportOptions;
        m_sliceCustom->setChecked(o.sliceCustomOpt);
        m_sliceFormat->setCurrentIndex(qMax(0, m_sliceFormat->findData(int(eo.format))));
        m_sliceQuality->setValue(eo.quality);
        m_sliceFormat->setEnabled(o.sliceCustomOpt);
        m_sliceQuality->setEnabled(o.sliceCustomOpt);
        m_updating = false;
        return;
    }
    const bool vec = o.isVector();
    m_fillRow->setVisible(vec && o.type != ObjType::Line);
    m_strokeRow->setVisible(vec);
    m_textRow->setVisible(o.type == ObjType::Text);
    m_textRow2->setVisible(o.type == ObjType::Text);
    m_textRow3->setVisible(o.type == ObjType::Text);
    m_textPathRow->setVisible(o.type == ObjType::Text && o.textOnPath);
    m_instRow->setVisible(o.isInstance());
    m_maskRow->setVisible(sel.size() == 1 && o.type == ObjType::Group);
    if (sel.size() == 1 && o.type == ObjType::Group) {
        m_groupInfo->setText(QStringLiteral("%1 object%2%3").arg(o.children.size()).arg(o.children.size() == 1 ? QString() : QStringLiteral("s")).arg(o.hasMask() ? QStringLiteral(";") : QString()));
        for (QWidget *w : std::initializer_list<QWidget *>{m_maskMode, m_maskOn, m_maskLink}) w->setVisible(o.hasMask());
        if (QLayoutItem *li = m_maskRow->layout()->itemAt(1)) if (li->widget()) li->widget()->setVisible(o.hasMask());
        m_maskMode->setCurrentIndex(qBound(0, o.maskMode, 1));
        m_maskOn->setChecked(o.maskEnabled);
        m_maskLink->setChecked(o.maskLinked);
    }
    refreshShapeParams(sel.size() == 1 && o.type == ObjType::AutoShape ? &o : nullptr);
    if (o.isInstance()) {
        const Symbol *sym = m_doc->symbol(o.symbolId);
        const bool button = sym && sym->kind == SymbolKind::Button;
        m_instInfo->setText(sym ? QStringLiteral("Symbol: %1 (%2)").arg(sym->name, symbolKindLabel(sym->kind)) : QStringLiteral("Missing symbol"));
        m_instFrame->setRange(1, sym ? qMax(1, sym->frameCount()) : 1);
        m_instFrame->setPrefix(button ? QString() : QStringLiteral("Frame "));
        m_instFrame->setValue(o.symFrame + 1);
        m_instFrame->setEnabled(sym && sym->frameCount() > 1);
        if (button) m_instFrame->setToolTip(QStringLiteral("Button state: %1").arg(buttonStateNames().value(o.symFrame)));
        m_instLink->setText(o.link);
        m_instAlt->setText(o.alt);
        const bool webOn = webFeaturesEnabled();
        m_instWebBox->setVisible(webOn);
        m_instLink->setEnabled(button && webOn);
        m_instAlt->setEnabled(button && webOn);
        m_instFrame->setVisible(!button || webOn);
        if (button && webOn) {
            m_instFrame->setEnabled(sym && sym->frameCount() > 1);
            m_instFrame->setToolTip(QStringLiteral("Button state: %1").arg(buttonStateNames().value(o.symFrame)));
        } else if (!button) {
            m_instFrame->setEnabled(sym && sym->frameCount() > 1);
            m_instFrame->setToolTip(QStringLiteral("Symbol frame / button state shown by this instance"));
        }
    }
    m_fill->setColor(o.fill);
    m_fill->setNone(!o.fillOn);
    m_fillOn->setChecked(o.fillOn);
    m_roundLabel->setVisible(o.type == ObjType::Rect);
    m_round->setVisible(o.type == ObjType::Rect);
    m_round->setValue(o.roundness);
    m_stroke->setColor(o.stroke);
    m_stroke->setNone(!o.strokeOn);
    m_strokeOn->setChecked(o.strokeOn);
    m_strokeW->setValue(o.strokeWidth);
    m_aa->setChecked(o.antialias);
    m_aa->setVisible(o.type != ObjType::Text); // text uses the anti-alias dropdown instead
    {
        m_fillOptBox->setVisible(vec);
        const int k = int(o.fillKind);
        m_fillKind->setCurrentIndex(k >= 3 ? k + 1 : k);
        const bool grad = fills::isGradient(o.fillKind);
        m_gradEdit->setVisible(grad);
        m_pattern->setVisible(o.fillKind == FillKind::Pattern);
        const int pi = m_pattern->findText(o.pattern);
        if (pi >= 0) m_pattern->setCurrentIndex(pi);
        else if (o.pattern == QLatin1String("Procedural")) m_pattern->setCurrentIndex(m_pattern->count() - 2);
        else if (o.pattern == QLatin1String("Custom")) m_pattern->setCurrentIndex(m_pattern->count() - 1);
        m_edge->setCurrentIndex(o.feather > 0 ? 2 : (o.antialias ? 1 : 0));
        m_feather->setEnabled(o.feather > 0);
        m_feather->setValue(o.feather);
        auto texIndex = [](QComboBox *c, const QString &t) {
            if (t.isEmpty()) return 0;
            if (t == QLatin1String("Procedural")) return c->count() - 1;
            return qMax(0, c->findText(t));
        };
        const int ti = texIndex(m_texture, o.texture);
        m_texture->setCurrentIndex(ti);
        m_texture->setItemText(m_texture->count() - 1, o.texture == QLatin1String("Procedural") ? QStringLiteral("Procedural (edit...)") : QStringLiteral("Procedural..."));
        m_texAmount->setEnabled(ti > 0);
        m_texAmount->setValue(o.textureAmount);
        const int sti = texIndex(m_strokeTexture, o.strokeTexture);
        m_strokeTexture->setCurrentIndex(sti);
        m_strokeTexture->setItemText(m_strokeTexture->count() - 1, o.strokeTexture == QLatin1String("Procedural") ? QStringLiteral("Procedural (edit...)") : QStringLiteral("Procedural..."));
        m_strokeTexture->setEnabled(o.strokeOn);
        m_strokeTexAmount->setEnabled(o.strokeOn && sti > 0);
        m_strokeTexAmount->setValue(o.strokeTextureAmount);
        {
            const QString key = fills::resolveStrokeKind(o.strokeKind);
            m_strokeKind->setCurrentIndex(qMax(0, m_strokeKind->findData(key)));
            QString cat = QStringLiteral("Basic"), name = QStringLiteral("Hard Line");
            for (const auto &sp : fills::strokePresets()) if (sp.key == key) { cat = sp.category; name = sp.name; break; }
            const bool block = m_strokeCat->blockSignals(true);
            m_strokeCat->setCurrentIndex(qMax(0, m_strokeCat->findText(cat)));
            m_strokeCat->blockSignals(block);
            m_strokeName->blockSignals(true);
            m_strokeName->clear();
            for (const QString &n : fills::strokeNamesInCategory(cat)) m_strokeName->addItem(n, fills::strokeKey(cat, n));
            m_strokeName->setCurrentIndex(qMax(0, m_strokeName->findData(key)));
            m_strokeName->blockSignals(false);
        }
        m_strokeAlign->setCurrentIndex(o.strokeAlign);
        m_fillKind->setToolTip(grad ? QStringLiteral("Gradient fill: drag the handles on the canvas (or use the Gradient tool) to move it")
                                    : QStringLiteral("Fill category"));
    }
    m_textAA->setCurrentIndex(int(o.textAA));
    // contentHeight() builds a scratch document (text defaults to "Sans Serif")
    // and must not insert those families into the live combo or overwrite sticky.
    if (!m_measuring)
        m_font->setCurrentFamily(o.fontFamily);
    m_fontSize->setValue(o.fontSize);
    m_bold->setChecked(o.bold);
    m_italic->setChecked(o.italic);
    m_underline->setChecked(o.underline);
    m_kerning->setValue(o.kerning);
    m_leading->setValue(o.leading);
    m_hScale->setValue(o.hScale);
    m_baseline->setValue(o.baselineShift);
    m_alignL->setChecked(o.textAlign == 0); m_alignC->setChecked(o.textAlign == 1); m_alignR->setChecked(o.textAlign == 2); m_alignJ->setChecked(o.textAlign == 3);
    m_textOrient->setCurrentIndex(qBound(0, o.textOrientation, 1));
    m_textDir->setCurrentIndex(qBound(0, o.textDirection, 1));
    m_paraIndent->setValue(o.paraIndent);
    m_paraBefore->setValue(o.paraSpaceBefore);
    m_paraAfter->setValue(o.paraSpaceAfter);
    m_textPathOffset->setValue(o.textPathOffset);
    m_opacity->setValue(o.opacity);
    m_blend->setCurrentIndex(int(o.blend));
    if (m_fxPending) { m_updating = false; return; }   // an Add Effect window is previewing: the list waits for OK
    int row = m_effects->currentRow();
    m_effects->clear();
    for (const LiveEffect &e : o.effects) {
        auto *it = new QListWidgetItem(effectName(e.type), m_effects);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(e.enabled ? Qt::Checked : Qt::Unchecked);
    }
    if (m_effects->count()) m_effects->setCurrentRow(qBound(0, row, m_effects->count() - 1));
    m_updating = false;
}

void PropertyInspector::refreshTool() {
    if (m_stack->currentIndex() != 2) return;
    const Tool t = appState().tool();
    if (m_toolOptsFor != int(t) || !m_toolOpts) buildToolOptions(t);
    m_updating = true;
    m_toolTitle->setText(toolName(t));
    for (const auto &f : m_toolSync) f();
    if (t == Tool::Brush && m_toolOpts) {
        const auto cat = brush::Category(qBound(0, appState().brushCategory, int(brush::Category::Libreworks)));
        stroke::Look look = stroke::paramsFor(brush::tipStrokeKey(cat, appState().brushTip)).look;
        const QString key = stroke::canonicalKey(brush::tipStrokeKey(cat, appState().brushTip));
        auto it = appState().brushStrokeByTip.constFind(key);
        if (it != appState().brushStrokeByTip.cend()) look = it->look;
        if (QWidget *edge = m_toolOpts->findChild<QWidget *>(QStringLiteral("opt_edge"))) {
            const bool on = stroke::usesEdgeSoftness(look);
            uiutil::setEnabledWithLabel(edge, on);
            edge->setToolTip(on ? QStringLiteral("Edge softness: 0 is hard, 100 is soft.")
                                : QStringLiteral("Edge softness does not apply to this tip."));
        }
    }
    m_updating = false;
}

void PropertyInspector::addEffect(EffectType t) {
    if (m_updating || !m_doc) return;
    QVector<quint64> ids;
    for (VObject *o : m_doc->selectedEditable()) ids << o->id;
    if (ids.isEmpty()) return;
    const QString label = QStringLiteral("Add %1").arg(effectName(t));
    auto selectLast = [this] { if (m_effects->count()) m_effects->setCurrentRow(m_effects->count() - 1); };
    // effects without settings (or when scripted / testing) go straight in, one undo step
    if (!m_openEditorOnAdd || !effectHasSettings(t)) {
        for (quint64 id : ids) if (VObject *o = m_doc->find(id)) o->effects.append(LiveEffect::make(t));
        m_doc->commit(label);
        selectLast();
        return;
    }
    // Fireworks: an effect with a settings window is previewed while the window is open but only
    // inserted into the Effects list when OK is clicked; Cancel / Esc / close leave no trace (no list
    // entry, no undo step, the document's modified state untouched)
    QHash<quint64, QVector<LiveEffect>> originals;
    for (quint64 id : ids) if (VObject *o = m_doc->find(id)) originals[id] = o->effects;
    QImage sample;
    {
        // the new effect's input is the object with its current effects
        SymbolScope symScope(&m_doc->s.symbols);
        if (VObject *o = m_doc->find(ids.first())) sample = render::renderObject(*o, nullptr, QRect(-1000000, -1000000, 2000000, 2000000));
    }
    EffectDialog dlg(LiveEffect::make(t), this, sample);
    QPointer<Document> doc(m_doc);
    auto preview = [doc, originals, &dlg](const LiveEffect &e) {
        if (!doc) return;
        for (auto it = originals.begin(); it != originals.end(); ++it)
            if (VObject *x = doc->find(it.key())) {
                x->effects = it.value();
                if (dlg.previewEnabled()) x->effects.append(e);
            }
        doc->notifyChanged();
    };
    connect(&dlg, &EffectDialog::changed, this, preview);
    m_fxPending = true;           // keep the Effects list as it was while previewing
    preview(dlg.effect());
    const bool ok = dlg.exec() == QDialog::Accepted;
    m_fxPending = false;
    if (!doc) return;
    if (ok) {
        const LiveEffect e = dlg.effect();
        for (auto it = originals.begin(); it != originals.end(); ++it)
            if (VObject *x = doc->find(it.key())) { x->effects = it.value(); x->effects.append(e); }
        doc->commit(label);
        refresh();
        selectLast();
    } else {
        doc->revert();
        refresh();
    }
}

void PropertyInspector::editEffect(int index) {
    VObject *o = m_doc ? m_doc->singleSelected() : nullptr;
    if (!o || o->locked || index < 0 || index >= o->effects.size()) return;
    if (!effectHasSettings(o->effects[index].type)) return;
    const quint64 id = o->id;
    // the effect's input (object with the effects before it) is the sample for histograms / eyedroppers
    VObject input = *o;
    input.effects = o->effects.mid(0, index);
    SymbolScope symScope(&m_doc->s.symbols);
    QImage sample = render::renderObject(input, nullptr, QRect(-1000000, -1000000, 2000000, 2000000));
    EffectDialog dlg(o->effects[index], this, sample);
    connect(&dlg, &EffectDialog::changed, this, [this, id, index](const LiveEffect &e) {
        if (VObject *x = m_doc->find(id)) { x->effects[index] = e; m_doc->notifyChanged(); }
    });
    if (dlg.exec() == QDialog::Accepted) {
        if (VObject *x = m_doc->find(id)) x->effects[index] = dlg.effect();
        m_doc->commit(QStringLiteral("Edit %1").arg(effectName(dlg.effect().type)));
    } else {
        m_doc->revert();
    }
}
