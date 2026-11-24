#include "EditStrokeDialog.h"
#include "core/Fills.h"
#include "Dialogs.h"
#include "PopupSlider.h"
#include "AppState.h"
#include <QToolButton>
#include <QPen>
#include <QPixmap>
#include <QIcon>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QAbstractButton>
#include <QLabel>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QLineEdit>
#include "FileDialogs.h"
#include "WidgetEnable.h"
#include <QFileDialog>
#include <QResizeEvent>
#include <functional>


namespace {
const QString kTipCellSampleUnder = QStringLiteral(
    "Each cell takes its colour from the target bitmap under the stroke, blended with the brush colour. "
    "Where the pixels underneath are transparent, the usual self-generated Cellular pattern is used, "
    "weighted by that alpha.");
const QString kTipCellFollowEdges = QStringLiteral(
    "Cell seeds and boundaries adapt to edges and colour regions already in the bitmap. "
    "Transparent areas keep the usual self-generated pattern, weighted by the alpha underneath.");
}

class PreviewStrokeWidget : public QWidget {
public:
    QImage image;
    std::function<void()> onResize;
    PreviewStrokeWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("editStrokePreview"));
        setMinimumHeight(72);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setImage(const QImage &im) { image = im; update(); }
protected:
    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        if (image.size() != size() && onResize)
            onResize();
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        if (image.isNull() || image.size() != size())
            p.fillRect(rect(), QColor(250, 250, 248));
        else
            p.drawImage(0, 0, image);
    }
};

static QSpinBox *spin(int lo, int hi, const QString &tip, QWidget *parent)
{
    auto *s = new PopupSliderField(parent);
    s->setRange(lo, hi);
    s->setToolTip(tip);
    return s;
}

static QDoubleSpinBox *dspin(double lo, double hi, int dec, const QString &tip, QWidget *parent)
{
    auto *s = new PopupSliderDoubleField(parent);
    s->setRange(lo, hi);
    s->setDecimals(dec);
    s->setSingleStep(dec >= 2 ? 0.1 : 1.0);
    s->setToolTip(tip);
    return s;
}

EditStrokeDialog::EditStrokeDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Edit Stroke"));
    setObjectName(QStringLiteral("editStrokeDialog"));
    uiutil::installEnableLabelFilter();
    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;
    // ---- Options
    auto *opt = new QWidget;
    auto *of = new QFormLayout(opt);
    m_ink = spin(0, 100, QStringLiteral("How much ink each dab lays down."), this);
    m_ink->setObjectName(QStringLiteral("editStrokeInk"));
    m_spacing = spin(0, 400, QStringLiteral("Spacing between dabs, as a percentage of tip size."), this);
    m_spacing->setObjectName(QStringLiteral("editStrokeSpacing"));
    m_flow = spin(0, 100, QStringLiteral("Higher flow rates create strokes that flow over time, as with an airbrush."), this);
    m_flow->setObjectName(QStringLiteral("editStrokeFlow"));
    m_buildUp = new QCheckBox(QStringLiteral("Build-up"));
    m_buildUp->setObjectName(QStringLiteral("editStrokeBuildUp"));
    m_buildUp->setToolTip(QStringLiteral("Overlap dabs for denser strokes."));
    m_texAmt = spin(0, 100, QStringLiteral("The higher the number, the more apparent the texture becomes."), this);
    m_texture = new QComboBox;
    m_texture->addItem(QStringLiteral("(None)"), QString());
    for (const QString &n : fills::textureNames()) m_texture->addItem(n, n);
    m_edgeTex = spin(0, 100, QStringLiteral("Amount of texture applied only to the stroke edge."), this);
    m_tips = spin(1, 16, QStringLiteral("Number of tips the stroke is painted with."), this);
    m_tipSpacing = spin(0, 400, QStringLiteral("Spacing between multiple tips."), this);
    m_colorVar = new QComboBox;
    m_colorVar->addItems({QStringLiteral("None"), QStringLiteral("Random"), QStringLiteral("Uniform"),
                          QStringLiteral("Complementary"), QStringLiteral("Hue"), QStringLiteral("Shadow")});
    m_colorVar->setToolTip(QStringLiteral("Colour variation across multiple tips or along the stroke."));
    of->addRow(QStringLiteral("Ink amount:"), m_ink);
    of->addRow(QStringLiteral("Spacing:"), m_spacing);
    of->addRow(QStringLiteral("Flow rate:"), m_flow);
    of->addRow(m_buildUp);
    of->addRow(QStringLiteral("Texture:"), m_texture);
    of->addRow(QStringLiteral("Texture amount:"), m_texAmt);
    of->addRow(QStringLiteral("Edge texture:"), m_edgeTex);
    of->addRow(QStringLiteral("Tips:"), m_tips);
    of->addRow(QStringLiteral("Tip spacing:"), m_tipSpacing);
    of->addRow(QStringLiteral("Color variation:"), m_colorVar);
    auto *dash = new QWidget;
    dash->setObjectName(QStringLiteral("editStrokeDash"));
    auto *dh = new QHBoxLayout(dash);
    dh->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 3; ++i) {
        m_dashOn[i] = spin(0, 100, QStringLiteral("Dash %1 on length.").arg(i + 1), this);
        m_dashOn[i]->setObjectName(QStringLiteral("editStrokeDashOn%1").arg(i + 1));
        m_dashOff[i] = spin(0, 100, QStringLiteral("Dash %1 off length.").arg(i + 1), this);
        m_dashOff[i]->setObjectName(QStringLiteral("editStrokeDashOff%1").arg(i + 1));
        auto *onLab = new QLabel(QStringLiteral("On"));
        onLab->setBuddy(m_dashOn[i]);
        auto *offLab = new QLabel(QStringLiteral("Off"));
        offLab->setBuddy(m_dashOff[i]);
        dh->addWidget(onLab);
        dh->addWidget(m_dashOn[i]);
        dh->addWidget(offLab);
        dh->addWidget(m_dashOff[i]);
    }
    of->addRow(QStringLiteral("Dash:"), dash);
    m_dashRow = dash;
    m_sprayDensity = spin(0, 100, QStringLiteral("How densely the spray emits droplets."), this);
    m_sprayDensity->setObjectName(QStringLiteral("editStrokeSprayDensity"));
    m_spraySpread = spin(0, 100, QStringLiteral("How far the spray mist spreads from the centre."), this);
    m_spraySpread->setObjectName(QStringLiteral("editStrokeSpraySpread"));
    m_sprayDrips = new QCheckBox(QStringLiteral("Drips"));
    m_sprayDrips->setObjectName(QStringLiteral("editStrokeSprayDrips"));
    m_sprayDrips->setToolTip(QStringLiteral("When on, slow or hovering strokes can run drips of paint."));
    of->addRow(QStringLiteral("Density:"), m_sprayDensity);
    of->addRow(QStringLiteral("Spread:"), m_spraySpread);
    of->addRow(m_sprayDrips);
    m_stampRow = new QWidget;
    auto *sh = new QHBoxLayout(m_stampRow);
    sh->setContentsMargins(0, 0, 0, 0);
    m_stampEdit = new QLineEdit;
    m_stampEdit->setPlaceholderText(QStringLiteral("Bitmap file or Library symbol"));
    m_stampEdit->setToolTip(QStringLiteral("Image Stamp source: a file path, or symbol:<id> from the document Library."));
    m_stampSymbol = new QComboBox;
    m_stampSymbol->setToolTip(QStringLiteral("Pick a Library symbol to stamp along the path."));
    auto *browse = new QPushButton(QStringLiteral("Browse…"));
    browse->setObjectName(QStringLiteral("stampBrowse"));
    sh->addWidget(m_stampEdit, 1);
    sh->addWidget(m_stampSymbol);
    sh->addWidget(browse);
    of->addRow(QStringLiteral("Stamp source:"), m_stampRow);
    m_cellBox = new QWidget;
    auto *cf = new QFormLayout(m_cellBox);
    cf->setContentsMargins(0, 0, 0, 0);
    m_cellRule = new QComboBox;
    m_cellRule->setToolTip(QStringLiteral("Cellular automaton rule. Custom accepts a B/S string such as B3/S23."));
    const struct { const char *label; const char *val; } kRules[] = {
        {"Life B3/S23", "B3/S23"},
        {"HighLife B36/S23", "B36/S23"},
        {"Seeds B2/S", "B2/S"},
        {"Day & Night B3678/S34678", "B3678/S34678"},
        {"Brian's Brain (3-state)", "BB"},
        {"Wolfram 1D 30", "W30"},
        {"Wolfram 1D 90", "W90"},
        {"Wolfram 1D 110", "W110"},
        {"Custom B/S…", "custom"},
    };
    for (const auto &r : kRules) m_cellRule->addItem(QString::fromUtf8(r.label), QString::fromLatin1(r.val));
    m_cellCustom = new QLineEdit;
    m_cellCustom->setPlaceholderText(QStringLiteral("B3/S23"));
    m_cellCustom->setToolTip(QStringLiteral("Custom birth/survive rule, e.g. B36/S23. Ignored for Brian's Brain and Wolfram rules."));
    m_cellSize = spin(1, 32, QStringLiteral("Size of each automaton cell, in pixels."), this);
    m_cellSteps = spin(0, 16, QStringLiteral("Generations to run inside the stroke mask after each dab."), this);
    m_cellDensity = spin(0, 100, QStringLiteral("Probability that a newly covered cell is seeded alive (plus inheritance from neighbours)."), this);
    m_cellAgeColor = new ColorButton(this);
    m_cellAgeColor->setToolTip(QStringLiteral("Colour by age: cells lerp from the stroke colour to this colour as they live."));
    cf->addRow(QStringLiteral("Rule:"), m_cellRule);
    cf->addRow(QStringLiteral("Custom B/S:"), m_cellCustom);
    cf->addRow(QStringLiteral("Cell size:"), m_cellSize);
    cf->addRow(QStringLiteral("Steps per dab:"), m_cellSteps);
    cf->addRow(QStringLiteral("Seed density:"), m_cellDensity);
    cf->addRow(QStringLiteral("Colour by age:"), m_cellAgeColor);
    m_cellSampleUnder = new QCheckBox(QStringLiteral("Sample colours from underneath"));
    m_cellSampleUnder->setToolTip(kTipCellSampleUnder);
    m_cellFollowEdges = new QCheckBox(QStringLiteral("Follow existing edges"));
    m_cellFollowEdges->setToolTip(kTipCellFollowEdges);
    cf->addRow(m_cellSampleUnder);
    cf->addRow(m_cellFollowEdges);
    of->addRow(m_cellBox);
    tabs->addTab(opt, QStringLiteral("Options"));
    // ---- Shape
    auto *shp = new QWidget;
    auto *sf = new QFormLayout(shp);
    m_square = new QCheckBox(QStringLiteral("Square tip"));
    m_square->setToolTip(QStringLiteral("A square tip; leave unchecked for a round tip."));
    m_size = dspin(0.1, 1000.0, 1, QStringLiteral("Tip or stroke width, in pixels. Accepts fractional widths and sizes up to 1000."), this);
    m_size->setObjectName(QStringLiteral("editStrokeSize"));
    m_edge = spin(0, 100, QStringLiteral("Edge softness: 0 is hard, 100 is soft."), this);
    m_edge->setObjectName(QStringLiteral("editStrokeEdge"));
    m_aspect = spin(1, 100, QStringLiteral("Tip aspect: 100 is round, lower values make a flatter calligraphy nib."), this);
    m_aspect->setObjectName(QStringLiteral("editStrokeAspect"));
    m_angle = spin(-180, 180, QStringLiteral("Tip angle in degrees. Calligraphy presets default to −45."), this);
    m_angle->setObjectName(QStringLiteral("editStrokeAngle"));
    sf->addRow(m_square);
    sf->addRow(QStringLiteral("Size:"), m_size);
    sf->addRow(QStringLiteral("Edge:"), m_edge);
    sf->addRow(QStringLiteral("Aspect:"), m_aspect);
    sf->addRow(QStringLiteral("Angle:"), m_angle);
    tabs->addTab(shp, QStringLiteral("Shape"));
    // ---- Sensitivity
    auto *sen = new QWidget;
    auto *sn = new QFormLayout(sen);
    m_prop = new QComboBox;
    m_prop->addItems({QStringLiteral("Size"), QStringLiteral("Ink Amount"), QStringLiteral("Saturation"),
                      QStringLiteral("Hue"), QStringLiteral("Angle")});
    m_prop->setToolTip(QStringLiteral("The stroke property affected by pressure, speed and the other inputs."));
    m_pressure = spin(0, 100, QStringLiteral("How much tablet pressure affects the selected property."), this);
    m_pressure->setObjectName(QStringLiteral("editStrokeSensPressure"));
    m_speed = spin(0, 100, QStringLiteral("How much stroke speed (or mouse speed) affects the selected property."), this);
    m_speed->setObjectName(QStringLiteral("editStrokeSensSpeed"));
    m_horizontal = spin(0, 100, QStringLiteral("How much horizontal position affects the selected property."), this);
    m_horizontal->setObjectName(QStringLiteral("editStrokeSensHorizontal"));
    m_vertical = spin(0, 100, QStringLiteral("How much vertical position affects the selected property."), this);
    m_random = spin(0, 100, QStringLiteral("How much randomness affects the selected property."), this);
    sn->addRow(QStringLiteral("Stroke property:"), m_prop);
    sn->addRow(QStringLiteral("Pressure:"), m_pressure);
    sn->addRow(QStringLiteral("Speed:"), m_speed);
    sn->addRow(QStringLiteral("Horizontal:"), m_horizontal);
    sn->addRow(QStringLiteral("Vertical:"), m_vertical);
    sn->addRow(QStringLiteral("Random:"), m_random);
    auto *note = new QLabel(QStringLiteral("Horizontal and Vertical are included to fill Fireworks’ five Affected By slots; "
                                           "only Pressure, Speed and Random were confirmed in sources used for 0.3.20."));
    note->setWordWrap(true);
    sn->addRow(note);
    tabs->addTab(sen, QStringLiteral("Sensitivity"));
    root->addWidget(tabs);
    auto *prevRow = new QWidget;
    auto *ph = new QHBoxLayout(prevRow);
    ph->setContentsMargins(0, 0, 0, 0);
    ph->setSpacing(4);
    m_preview = new PreviewStrokeWidget;
    m_preview->onResize = [this] { refreshPreview(); };
    ph->addWidget(m_preview, 1);
    m_previewBgBtn = new QToolButton;
    m_previewBgBtn->setObjectName(QStringLiteral("editStrokePreviewBg"));
    m_previewBgBtn->setAutoRaise(true);
    m_previewBgBtn->setFocusPolicy(Qt::NoFocus);
    m_previewBgBtn->setFixedSize(28, 28);
    m_previewBgBtn->setToolTip(QStringLiteral("Preview background: Auto (default), White, or Black (click to change)."));
    connect(m_previewBgBtn, &QToolButton::clicked, this, [this] {
        setPreviewBackgroundMode((m_previewBg + 1) % 3);
    });
    ph->addWidget(m_previewBgBtn, 0, Qt::AlignBottom);
    root->addWidget(prevRow);
    m_previewBg = qBound(0, appState().prefs.strokePreviewBg, 2);
    syncPreviewBgButton();
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    connect(bb, &QDialogButtonBox::accepted, this, [this] { syncToParams(); accept(); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
        syncToParams();
        emit applied(m_p);
    });
    root->addWidget(bb);

    auto changed = [this] {
        if (m_updating) return;
        if (sender() == m_prop) { m_updating = true; syncFromParams(); m_updating = false; return; }
        syncToParams();
        refreshPreview();
    };
    for (QSpinBox *s : {m_ink, m_spacing, m_flow, m_texAmt, m_edgeTex, m_tips, m_tipSpacing,
                        m_dashOn[0], m_dashOff[0], m_dashOn[1], m_dashOff[1], m_dashOn[2], m_dashOff[2],
                        m_sprayDensity, m_spraySpread,
                        m_edge, m_aspect, m_angle, m_pressure, m_speed, m_horizontal, m_vertical, m_random})
        connect(s, &QSpinBox::valueChanged, this, changed);
    connect(m_size, &QDoubleSpinBox::valueChanged, this, changed);
    connect(m_buildUp, &QCheckBox::toggled, this, changed);
    connect(m_square, &QCheckBox::toggled, this, changed);
    connect(m_sprayDrips, &QCheckBox::toggled, this, changed);
    connect(m_colorVar, &QComboBox::currentIndexChanged, this, changed);
    connect(m_texture, &QComboBox::currentIndexChanged, this, changed);
    connect(m_prop, &QComboBox::currentIndexChanged, this, changed);
    connect(m_stampEdit, &QLineEdit::editingFinished, this, changed);
    connect(m_cellRule, &QComboBox::currentIndexChanged, this, changed);
    connect(m_cellCustom, &QLineEdit::editingFinished, this, changed);
    connect(m_cellSize, &QSpinBox::valueChanged, this, changed);
    connect(m_cellSteps, &QSpinBox::valueChanged, this, changed);
    connect(m_cellDensity, &QSpinBox::valueChanged, this, changed);
    connect(m_cellAgeColor, &ColorButton::colorPicked, this, changed);
    connect(m_cellAgeColor, &ColorButton::colorPreview, this, changed);
    connect(m_cellSampleUnder, &QCheckBox::toggled, this, changed);
    connect(m_cellFollowEdges, &QCheckBox::toggled, this, changed);
    connect(m_stampSymbol, &QComboBox::currentIndexChanged, this, [this, changed] {
        const QVariant v = m_stampSymbol->currentData();
        if (m_updating) return;
        if (v.isValid() && !v.toString().isEmpty()) {
            m_updating = true;
            m_stampEdit->setText(v.toString());
            m_updating = false;
        }
        changed();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = filedialogs::getOpenFileName(this, QStringLiteral("Choose stamp image"),
                                                       m_stampEdit->text(),
                                                       QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif);;All files (*)"));
        if (f.isEmpty()) return;
        m_stampEdit->setText(f);
        syncToParams();
        refreshPreview();
    });
}


void EditStrokeDialog::setLibrarySymbols(const QVector<QPair<quint64, QString>> &syms)
{
    m_library = syms;
    if (!m_stampSymbol) return;
    const bool up = m_updating;
    m_updating = true;
    m_stampSymbol->clear();
    m_stampSymbol->addItem(QStringLiteral("(None)"), QString());
    for (const auto &s : m_library)
        m_stampSymbol->addItem(s.second, QStringLiteral("symbol:%1").arg(s.first));
    m_updating = up;
}

void EditStrokeDialog::setBrushSource(const brush::Params &bp)
{
    m_brush = bp;
    m_brushMode = true;
    m_vectorStroke = false;
}

void EditStrokeDialog::setParams(const stroke::Params &p, const QColor &color, double width, quint32 seed)
{
    m_p = p;
    m_color = color;
    m_width = width > 0 ? width : (p.size > 0 ? p.size : 1);
    m_seed = seed;
    // The Size spinner shows the live Brush size / object stroke width, which the
    // preview already uses as tipWidth. Factory Params.size is only a fallback.
    if (m_width > 0)
        m_p.size = m_width;
    m_updating = true;
    syncFromParams();
    m_updating = false;
    refreshPreview();
}

stroke::Sensitivity &EditStrokeDialog::sensFor(int prop)
{
    switch (prop) {
    case 1: return m_p.inkSens;
    case 2: return m_p.satSens;
    case 3: return m_p.hueSens;
    case 4: return m_p.angleSens;
    default: return m_p.sizeSens;
    }
}

void EditStrokeDialog::setVectorStroke(bool on)
{
    m_vectorStroke = on;
    if (on) m_brushMode = false;
    const char *kDisabled =
        "This option samples the target bitmap and applies only to the bitmap Brush tool, not to vector strokes.";
    auto apply = [&](QCheckBox *box, const QString &enabledTip) {
        if (!box) return;
        box->setEnabled(!on);
        box->setToolTip(on ? QString::fromUtf8(kDisabled) : enabledTip);
        if (on) box->setChecked(false);
    };
    apply(m_cellSampleUnder, kTipCellSampleUnder);
    apply(m_cellFollowEdges, kTipCellFollowEdges);
}

void EditStrokeDialog::syncFromParams()
{
    m_ink->setValue(m_p.inkAmount);
    m_spacing->setValue(m_p.spacing);
    m_flow->setValue(m_p.flowRate);
    m_buildUp->setChecked(m_p.buildUp);
    m_texAmt->setValue(m_p.textureAmount);
    int ti = m_texture->findData(m_p.texture);
    m_texture->setCurrentIndex(qMax(0, ti));
    m_edgeTex->setValue(m_p.edgeTexture);
    m_tips->setValue(m_p.tips);
    m_tipSpacing->setValue(m_p.tipSpacing);
    m_colorVar->setCurrentIndex(int(m_p.colorVar));
    for (int i = 0; i < 3; ++i) { m_dashOn[i]->setValue(m_p.dashOn[i]); m_dashOff[i]->setValue(m_p.dashOff[i]); }
    if (m_sprayDensity) m_sprayDensity->setValue(qBound(0, m_p.sprayDensity, 100));
    if (m_spraySpread) m_spraySpread->setValue(qBound(0, m_p.spraySpread, 100));
    if (m_sprayDrips) m_sprayDrips->setChecked(m_p.sprayDrips);
    m_square->setChecked(m_p.square);
    m_size->setValue(qBound(0.1, m_p.size, 1000.0));
    m_edge->setValue(m_p.edge);
    m_aspect->setValue(int(qBound(1.0, m_p.aspect * 100, 100.0)));
    m_angle->setValue(qRound(m_p.angle));
    const auto s = sensFor(m_prop->currentIndex());
    m_pressure->setValue(s.pressure);
    m_speed->setValue(s.speed);
    m_horizontal->setValue(s.horizontal);
    m_vertical->setValue(s.vertical);
    m_random->setValue(s.random);
    if (m_stampEdit) m_stampEdit->setText(m_p.stampSource);
    if (m_stampSymbol) {
        const int si = m_stampSymbol->findData(m_p.stampSource);
        m_stampSymbol->setCurrentIndex(si >= 0 ? si : 0);
    }
    if (m_stampRow) m_stampRow->setVisible(m_p.look == stroke::Look::ImageStamp || m_p.key == QLatin1String("lw.imageStamp"));
    if (auto *form = qobject_cast<QFormLayout *>(m_stampRow ? m_stampRow->parentWidget()->layout() : nullptr)) {
        if (QWidget *lab = form->labelForField(m_stampRow))
            lab->setVisible(m_stampRow->isVisible());
    }
    const bool cell = m_p.look == stroke::Look::Cellular || m_p.key.startsWith(QLatin1String("lw.cellular"));
    if (m_cellBox) m_cellBox->setVisible(cell);
    if (cell && m_cellRule) {
        const QString rule = m_p.cellRule.isEmpty() ? QStringLiteral("B3/S23") : m_p.cellRule;
        int ri = m_cellRule->findData(rule);
        if (ri < 0) {
            // treat as custom unless it is a known alias
            ri = m_cellRule->findData(QStringLiteral("custom"));
            m_cellCustom->setText(rule);
        } else {
            m_cellCustom->setText(rule == QLatin1String("custom") ? QString() : rule);
        }
        m_cellRule->setCurrentIndex(qMax(0, ri));
        m_cellCustom->setEnabled(m_cellRule->currentData().toString() == QLatin1String("custom"));
        m_cellSize->setValue(qBound(1, m_p.cellSize, 32));
        m_cellSteps->setValue(qBound(0, m_p.cellSteps, 16));
        m_cellDensity->setValue(qBound(0, m_p.seedDensity, 100));
        m_cellAgeColor->setColor(m_p.cellAgeColor.isValid() ? m_p.cellAgeColor : QColor(255, 210, 80));
        if (m_cellSampleUnder) m_cellSampleUnder->setChecked(m_p.cellSampleUnder && !m_vectorStroke);
        if (m_cellFollowEdges) m_cellFollowEdges->setChecked(m_p.cellFollowEdges && !m_vectorStroke);
    }
    const bool edgeOn = stroke::usesEdgeSoftness(m_p.look);
    m_edge->setToolTip(edgeOn ? QStringLiteral("Edge softness: 0 is hard, 100 is soft.")
                              : QStringLiteral("Edge softness does not apply to this tip."));
    auto en = [this](QWidget *w, bool on, const QString &offTip) {
        if (!w) return;
        uiutil::setEnabledWithLabel(w, on);
        if (!on && !offTip.isEmpty()) w->setToolTip(offTip);
    };
    auto enOpt = [&](QWidget *w, stroke::Option opt, const QString &offTip) {
        en(w, stroke::optionApplies(m_p, opt), offTip);
    };
    const QString na = QStringLiteral("This option does not apply to this tip.");
    enOpt(m_ink, stroke::Option::InkAmount, na);
    enOpt(m_spacing, stroke::Option::Spacing, na);
    enOpt(m_flow, stroke::Option::Flow, na);
    enOpt(m_buildUp, stroke::Option::BuildUp, na);
    enOpt(m_texture, stroke::Option::Texture, na);
    enOpt(m_texAmt, stroke::Option::Texture, na);
    enOpt(m_edgeTex, stroke::Option::EdgeTexture, na);
    enOpt(m_tips, stroke::Option::Tips, na);
    enOpt(m_tipSpacing, stroke::Option::TipSpacing, na);
    enOpt(m_colorVar, stroke::Option::ColorVariation, na);
    const bool dashOn = stroke::optionApplies(m_p, stroke::Option::Dash);
    for (int i = 0; i < 3; ++i) {
        en(m_dashOn[i], dashOn, na);
        en(m_dashOff[i], dashOn, na);
    }
    en(m_dashRow, dashOn, na);
    enOpt(m_square, stroke::Option::Square, na);
    enOpt(m_size, stroke::Option::Size, na);
    en(m_edge, edgeOn, QStringLiteral("Edge softness does not apply to this tip."));
    enOpt(m_aspect, stroke::Option::Aspect, na);
    enOpt(m_angle, stroke::Option::Angle, na);
    enOpt(m_sprayDensity, stroke::Option::SprayDensity, na);
    enOpt(m_spraySpread, stroke::Option::SpraySpread, na);
    enOpt(m_sprayDrips, stroke::Option::SprayDrips, na);
    const bool sensOn = stroke::optionApplies(m_p, stroke::Option::SizePressure)
        || stroke::optionApplies(m_p, stroke::Option::InkPressure);
    for (QWidget *w : {static_cast<QWidget *>(m_prop), static_cast<QWidget *>(m_pressure),
                       static_cast<QWidget *>(m_speed), static_cast<QWidget *>(m_horizontal),
                       static_cast<QWidget *>(m_vertical), static_cast<QWidget *>(m_random)}) {
        if (!w) continue;
        en(w, sensOn, na);
    }
    uiutil::syncSectionLabels(this);
}

void EditStrokeDialog::syncToParams()
{
    m_p.inkAmount = m_ink->value();
    m_p.spacing = m_spacing->value();
    m_p.flowRate = m_flow->value();
    m_p.buildUp = m_buildUp->isChecked();
    m_p.textureAmount = m_texAmt->value();
    m_p.texture = m_texture->currentData().toString();
    m_p.edgeTexture = m_edgeTex->value();
    m_p.tips = m_tips->value();
    m_p.tipSpacing = m_tipSpacing->value();
    m_p.colorVar = stroke::ColorVariation(m_colorVar->currentIndex());
    for (int i = 0; i < 3; ++i) { m_p.dashOn[i] = m_dashOn[i]->value(); m_p.dashOff[i] = m_dashOff[i]->value(); }
    if (m_sprayDensity) m_p.sprayDensity = m_sprayDensity->value();
    if (m_spraySpread) m_p.spraySpread = m_spraySpread->value();
    if (m_sprayDrips) m_p.sprayDrips = m_sprayDrips->isChecked();
    m_p.square = m_square->isChecked();
    m_p.size = m_size->value();
    m_width = m_p.size;
    m_p.edge = m_edge->value();
    m_p.aspect = m_aspect->value() / 100.0;
    m_p.angle = m_angle->value();
    auto &s = sensFor(m_prop->currentIndex());
    s.pressure = m_pressure->value();
    s.speed = m_speed->value();
    s.horizontal = m_horizontal->value();
    s.vertical = m_vertical->value();
    s.random = m_random->value();
    if (m_stampEdit) m_p.stampSource = m_stampEdit->text().trimmed();
    if (m_cellRule && (m_p.look == stroke::Look::Cellular || m_p.key.startsWith(QLatin1String("lw.cellular")))) {
        const QString sel = m_cellRule->currentData().toString();
        if (sel == QLatin1String("custom")) {
            const QString ctm = m_cellCustom->text().trimmed();
            m_p.cellRule = ctm.isEmpty() ? QStringLiteral("B3/S23") : ctm;
        } else {
            m_p.cellRule = sel;
        }
        m_p.cellSize = m_cellSize->value();
        m_p.cellSteps = m_cellSteps->value();
        m_p.seedDensity = m_cellDensity->value();
        m_p.cellAgeColor = m_cellAgeColor->color();
        m_p.cellSampleUnder = !m_vectorStroke && m_cellSampleUnder && m_cellSampleUnder->isChecked();
        m_p.cellFollowEdges = !m_vectorStroke && m_cellFollowEdges && m_cellFollowEdges->isChecked();
        m_cellCustom->setEnabled(sel == QLatin1String("custom"));
    }
}

void EditStrokeDialog::refreshPreview()
{
    if (!m_preview) return;
    const QSize sz = m_preview->size().isEmpty()
        ? QSize(qMax(200, m_preview->minimumWidth()), qMax(72, m_preview->minimumHeight()))
        : m_preview->size();
    QImage raw;
    if (m_brushMode) {
        brush::Params bp = m_brush;
        bp.size = m_width;
        bp.color = m_color;
        bp.edge = m_p.edge;
        bp.square = m_p.square;
        bp.texture = m_p.texture;
        bp.textureAmount = m_p.textureAmount;
        bp.nibAngle = m_p.angle;
        if (m_p.aspect > 0.0 && m_p.aspect < 0.999)
            bp.nibRatio = qBound(0.05, m_p.aspect, 1.0);
        if (!m_p.key.isEmpty()) bp.strokeKey = m_p.key;
        bp.strokeCustom = true;
        bp.strokeParams = m_p;
        raw = brush::renderPreviewRaw(bp, sz, m_seed);
    } else {
        stroke::PaintInput in;
        in.params = m_p;
        in.width = qMax(0.5, m_width);
        in.color = m_color;
        in.seed = m_seed ? m_seed : stroke::kPreviewSeed;
        in.pressure = 0.85;
        in.speed = 0.7;
        raw = stroke::renderPreviewRaw(in, sz);
    }
    const QColor bg = previewBackgroundFor(m_previewBg, m_color, raw);
    if (m_previewBg == 2) {
        m_autoPreviewBg = bg;
        m_autoPreviewBgSet = true;
    }
    m_preview->setImage(stroke::fitPreview(raw, sz, bg));
}

QImage EditStrokeDialog::previewImage() const
{
    return m_preview ? m_preview->image : QImage();
}

QColor EditStrokeDialog::previewBackgroundFor(int mode, const QColor &stroke)
{
    return previewBackgroundFor(mode, stroke, QImage());
}

QColor EditStrokeDialog::previewBackgroundFor(int mode, const QColor &stroke, const QImage &rendered)
{
    mode = qBound(0, mode, 2);
    if (mode == 1) return QColor(20, 20, 20);
    if (mode != 2) return QColor(250, 250, 248);

    // Auto: decide from the painted pixels (luminance weighted by alpha), not
    // only the palette colour. A thin bright core (Lightning) can sit in a
    // darker glow, so a dark mean must not hide a light 90th-percentile mark.
    if (!rendered.isNull()) {
        const QImage src = rendered.convertToFormat(QImage::Format_ARGB32);
        double wLuma = 0.0, wA = 0.0;
        double hist[256] = {};
        for (int y = 0; y < src.height(); ++y) {
            const QRgb *sl = reinterpret_cast<const QRgb *>(src.constScanLine(y));
            for (int x = 0; x < src.width(); ++x) {
                const int a = qAlpha(sl[x]);
                if (a <= 10) continue;
                const double af = a / 255.0;
                const int li = qBound(0, int(qRound(0.299 * qRed(sl[x]) + 0.587 * qGreen(sl[x]) + 0.114 * qBlue(sl[x]))), 255);
                wLuma += (li / 255.0) * af;
                wA += af;
                hist[li] += af;
            }
        }
        if (wA > 1e-6) {
            double acc = 0.0, p90 = 0.0;
            for (int i = 0; i < 256; ++i) {
                acc += hist[i];
                if (acc >= 0.90 * wA) { p90 = i / 255.0; break; }
            }
            const double mean = wLuma / wA;
            if (mean > 0.62 || p90 > 0.62) return QColor(20, 20, 20);
            return QColor(250, 250, 248);
        }
    }

    const double luma = (0.299 * stroke.redF() + 0.587 * stroke.greenF() + 0.114 * stroke.blueF()) * stroke.alphaF()
                      + (1.0 - stroke.alphaF());
    if (luma > 0.62) return QColor(20, 20, 20);
    return QColor(250, 250, 248);
}

QColor EditStrokeDialog::previewBackground() const
{
    if (m_previewBg == 2 && m_autoPreviewBgSet)
        return m_autoPreviewBg;
    return previewBackgroundFor(m_previewBg, m_color);
}

void EditStrokeDialog::syncPreviewBgButton()
{
    if (!m_previewBgBtn) return;
    const char *labels[] = {"White", "Black", "Auto"};
    const int mode = qBound(0, m_previewBg, 2);
    m_previewBgBtn->setText(QString());
    QPixmap px(18, 18);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor fill = mode == 2 ? QColor(180, 180, 180) : previewBackgroundFor(mode, Qt::black);
    p.setPen(QPen(QColor(80, 80, 80), 1));
    p.setBrush(fill);
    p.drawRoundedRect(1, 1, 16, 16, 3, 3);
    if (mode == 2) {
        QPainterPath tri;
        tri.moveTo(17, 1);
        tri.lineTo(17, 17);
        tri.lineTo(1, 17);
        tri.closeSubpath();
        p.setBrush(QColor(20, 20, 20));
        p.setPen(Qt::NoPen);
        p.drawPath(tri);
        p.setPen(QPen(QColor(80, 80, 80), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(1, 1, 16, 16, 3, 3);
    }
    p.end();
    m_previewBgBtn->setIcon(QIcon(px));
    m_previewBgBtn->setIconSize(QSize(18, 18));
    m_previewBgBtn->setToolTip(QStringLiteral("Preview background: %1 (click to change). Auto (default), White, or Black.")
                                   .arg(QLatin1String(labels[mode])));
}

void EditStrokeDialog::setPreviewBackgroundMode(int mode)
{
    mode = qBound(0, mode, 2);
    m_previewBg = mode;
    appState().prefs.strokePreviewBg = mode;
    appState().savePrefs();
    syncPreviewBgButton();
    refreshPreview();
}
