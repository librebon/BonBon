#include "ProceduralDialog.h"
#include "Dialogs.h"
#include "GradientEditor.h"
#include "PopupSlider.h"
#include "core/Fills.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {
QSpinBox *ispin(const char *name, int lo, int hi, const QString &tip, const QString &suffix = {}) {
    auto *s = new PopupSliderField(lo, hi, suffix);
    s->setObjectName(QString::fromLatin1(name));
    s->setToolTip(tip);
    return s;
}
QDoubleSpinBox *dspin(const char *name, double lo, double hi, int dec, const QString &tip, const QString &suffix = {}) {
    auto *s = new PopupSliderDoubleField(lo, hi, dec, suffix);
    s->setObjectName(QString::fromLatin1(name));
    s->setSingleStep(dec >= 3 ? 0.001 : dec == 2 ? 0.05 : 1);
    s->setToolTip(tip);
    return s;
}
QComboBox *combo(const char *name, const QStringList &items, const QString &tip) {
    auto *c = new QComboBox;
    c->setObjectName(QString::fromLatin1(name));
    c->addItems(items);
    c->setToolTip(tip);
    return c;
}
}

ProceduralDialog::ProceduralDialog(const proc::Params &initial, Mode mode, QSize target, QWidget *parent)
    : QDialog(parent), m_mode(mode), m_target(target), m_p(initial) {
    setObjectName(QStringLiteral("proceduralDialog"));
    setWindowTitle(mode == Mode::Render ? QStringLiteral("Render Texture") : mode == Mode::Pattern ? QStringLiteral("Procedural Pattern") : QStringLiteral("Procedural Texture"));
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(40);
    connect(m_debounce, &QTimer::timeout, this, &ProceduralDialog::startPreview);
    connect(&m_watch, &QFutureWatcher<QImage>::finished, this, &ProceduralDialog::showPreview);
    build();
    syncWidgets();
    startPreview();
}

ProceduralDialog::~ProceduralDialog() {
    if (m_cancel) m_cancel->store(true);
    m_watch.waitForFinished();
}

void ProceduralDialog::build() {
    auto *root = new QHBoxLayout(this);
    auto *formHost = new QWidget;
    auto *g = new QGridLayout(formHost);
    g->setContentsMargins(0, 0, 4, 0);
    g->setVerticalSpacing(4);
    int row = 0;
    auto add = [&](const QString &label, QWidget *w, const QString &key = {}) {
        auto *l = new QLabel(label);
        l->setBuddy(w);
        g->addWidget(l, row, 0);
        g->addWidget(w, row, 1);
        ++row;
        if (!key.isEmpty()) m_groups[key] << l << w;
    };
    auto hbox = [](std::initializer_list<QWidget *> ws) {
        auto *w = new QWidget;
        auto *h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        for (QWidget *x : ws) h->addWidget(x);
        h->addStretch();
        return w;
    };

    m_gen = new QComboBox;
    m_gen->setObjectName(QStringLiteral("procGen"));
    m_gen->setToolTip(QStringLiteral("Generator"));
    for (const QString &k : proc::generatorKeys()) m_gen->addItem(proc::generatorLabel(k), k);
    m_gen->setMaxVisibleItems(20);
    add(QStringLiteral("Generator:"), m_gen);
    m_preset = combo("procPreset", {}, QStringLiteral("Preset parameters for this generator"));
    add(QStringLiteral("Preset:"), m_preset, QStringLiteral("preset"));
    m_seed = ispin("procSeed", 0, 999999999, QStringLiteral("Random seed: the same seed always gives the same texture"));
    auto *rnd = new QPushButton(QStringLiteral("Randomize"));
    rnd->setObjectName(QStringLiteral("procRandomize"));
    rnd->setToolTip(QStringLiteral("Pick a new random seed"));
    add(QStringLiteral("Seed:"), hbox({m_seed, rnd}));
    m_w = ispin("procWidth", 8, 1024, QStringLiteral("Tile width"), QStringLiteral(" px"));
    m_h = ispin("procHeight", 8, 1024, QStringLiteral("Tile height"), QStringLiteral(" px"));
    add(QStringLiteral("Tile size:"), hbox({m_w, new QLabel(QStringLiteral("x")), m_h}), QStringLiteral("size"));
    m_tile = new QCheckBox(QStringLiteral("Tileable (seamless)"));
    m_tile->setObjectName(QStringLiteral("procTileable"));
    m_invert = new QCheckBox(QStringLiteral("Invert"));
    m_invert->setObjectName(QStringLiteral("procInvert"));
    add(QString(), hbox({m_tile, m_invert}));
    m_scale = dspin("procScale", 0.25, 64, 2, QStringLiteral("Feature scale: noise periods / cells / repeats across the tile"));
    add(QStringLiteral("Scale:"), m_scale, QStringLiteral("scale"));
    m_contrast = dspin("procContrast", -100, 100, 0, QStringLiteral("Contrast"));
    m_bright = dspin("procBrightness", -100, 100, 0, QStringLiteral("Brightness"));
    add(QStringLiteral("Contrast:"), m_contrast);
    add(QStringLiteral("Brightness:"), m_bright);
    // noise
    m_basis = combo("procBasis", {QStringLiteral("Perlin"), QStringLiteral("Simplex")}, QStringLiteral("Noise basis"));
    add(QStringLiteral("Basis:"), m_basis, QStringLiteral("noise"));
    m_octaves = ispin("procOctaves", 1, 10, QStringLiteral("Octaves of detail"));
    m_pers = dspin("procPersistence", 0.05, 1, 2, QStringLiteral("Persistence (gain): amplitude kept per octave"));
    m_lac = dspin("procLacunarity", 1.5, 4, 2, QStringLiteral("Lacunarity: frequency multiplier per octave"));
    add(QStringLiteral("Octaves:"), m_octaves, QStringLiteral("octaves"));
    add(QStringLiteral("Persistence:"), m_pers, QStringLiteral("octaves"));
    add(QStringLiteral("Lacunarity:"), m_lac, QStringLiteral("octaves"));
    m_warp = dspin("procWarp", 0, 4, 2, QStringLiteral("Warp / turbulence strength"));
    add(QStringLiteral("Warp:"), m_warp, QStringLiteral("warp"));
    // Worley
    m_variant = combo("procWorleyVariant", proc::worleyVariants(), QStringLiteral("Worley feature"));
    m_metric = combo("procMetric", {QStringLiteral("Euclidean"), QStringLiteral("Manhattan"), QStringLiteral("Chebyshev")}, QStringLiteral("Distance metric"));
    add(QStringLiteral("Feature:"), m_variant, QStringLiteral("worley"));
    add(QStringLiteral("Distance:"), m_metric, QStringLiteral("worley"));
    // patterns
    m_pattern = combo("procPattern", proc::patternNames(), QStringLiteral("Pattern"));
    m_width2 = dspin("procLineWidth", 0.02, 0.98, 2, QStringLiteral("Line / dot / stripe width"));
    add(QStringLiteral("Pattern:"), m_pattern, QStringLiteral("pattern"));
    add(QStringLiteral("Width:"), m_width2, QStringLiteral("pattern"));
    // Gray-Scott
    m_feed = dspin("procFeed", 0.001, 0.12, 4, QStringLiteral("Feed rate F"));
    m_kill = dspin("procKill", 0.001, 0.12, 4, QStringLiteral("Kill rate k"));
    add(QStringLiteral("Feed:"), m_feed, QStringLiteral("rd"));
    add(QStringLiteral("Kill:"), m_kill, QStringLiteral("rd"));
    // elementary
    m_rule = ispin("procRule", 0, 255, QStringLiteral("Wolfram rule number"));
    m_single = new QCheckBox(QStringLiteral("Single cell start"));
    m_single->setObjectName(QStringLiteral("procSingleCell"));
    m_single->setToolTip(QStringLiteral("Start from one live cell instead of a random row"));
    add(QStringLiteral("Rule:"), hbox({m_rule, m_single}), QStringLiteral("rule"));
    // life-like
    m_lifePreset = combo("procLifePreset", proc::lifePresets(), QStringLiteral("Known Life-like rules"));
    m_life = new QLineEdit;
    m_life->setObjectName(QStringLiteral("procLifeRule"));
    m_life->setToolTip(QStringLiteral("Birth/Survival rule, e.g. B3/S23"));
    m_life->setMaximumWidth(110);
    add(QStringLiteral("Rule:"), hbox({m_life, m_lifePreset}), QStringLiteral("life"));
    // cyclic
    m_states = ispin("procStates", 2, 32, QStringLiteral("Number of states"));
    m_threshold = ispin("procThreshold", 1, 20, QStringLiteral("Neighbours in the next state needed to advance"));
    m_range = ispin("procRange", 1, 5, QStringLiteral("Neighbourhood range"));
    m_moore = new QCheckBox(QStringLiteral("Moore neighbourhood"));
    m_moore->setObjectName(QStringLiteral("procMoore"));
    m_moore->setToolTip(QStringLiteral("Moore (square) or von Neumann (diamond) neighbourhood"));
    add(QStringLiteral("States:"), m_states, QStringLiteral("cyclic"));
    add(QStringLiteral("Threshold:"), m_threshold, QStringLiteral("cyclic"));
    add(QStringLiteral("Range:"), hbox({m_range, m_moore}), QStringLiteral("cyclic"));
    // Brian's Brain state colours
    const char *names[3] = {"procStateOff", "procStateFiring", "procStateDying"};
    const char *tips[3] = {"Off", "Firing", "Refractory (dying)"};
    for (int i = 0; i < 3; ++i) {
        m_stateCol[i] = new ColorButton(this, true);
        m_stateCol[i]->setObjectName(QString::fromLatin1(names[i]));
        m_stateCol[i]->setToolTip(QString::fromLatin1(tips[i]));
        m_stateCol[i]->setFixedSize(36, 20);
        connect(m_stateCol[i], &ColorButton::colorPicked, this, &ProceduralDialog::changed);
    }
    add(QStringLiteral("State colors:"), hbox({m_stateCol[0], m_stateCol[1], m_stateCol[2]}), QStringLiteral("states"));
    // cellular common
    m_cell = ispin("procCell", 1, 32, QStringLiteral("Cell size"), QStringLiteral(" px"));
    m_gens = ispin("procGenerations", 1, 20000, QStringLiteral("Generations / iterations to run"));
    m_density = dspin("procDensity", 0.01, 0.99, 2, QStringLiteral("Initial live-cell density"));
    add(QStringLiteral("Cell size:"), m_cell, QStringLiteral("cell"));
    add(QStringLiteral("Generations:"), m_gens, QStringLiteral("generations"));
    add(QStringLiteral("Density:"), m_density, QStringLiteral("density"));
    // colour ramp
    m_rampPreset = combo("procRampPreset", QStringList{QStringLiteral("Ramp preset..."), QStringLiteral("Black, White")} + fills::gradientPresetNames(), QStringLiteral("Load a gradient as the colour ramp"));
    auto *editRamp = new QPushButton(QStringLiteral("Edit..."));
    editRamp->setObjectName(QStringLiteral("procRampEdit"));
    editRamp->setToolTip(QStringLiteral("Edit the colour ramp in the gradient editor"));
    add(QStringLiteral("Color ramp:"), hbox({m_rampPreset, editRamp}), QStringLiteral("ramp"));
    m_ramp = new GradientBar;
    m_ramp->setObjectName(QStringLiteral("procRamp"));
    m_ramp->setToolTip(QStringLiteral("Values map through this ramp: click below it to add a stop, double-click a stop to pick its colour"));
    g->addWidget(m_ramp, row++, 0, 1, 2);
    m_groups[QStringLiteral("ramp")] << m_ramp;
    g->setRowStretch(row, 1);

    auto *scroll = new QScrollArea;
    scroll->setWidget(formHost);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(formHost->sizeHint().width() + 24);
    root->addWidget(scroll, 1);

    auto *right = new QVBoxLayout;
    m_previewLabel = new QLabel;
    m_previewLabel->setObjectName(QStringLiteral("procPreview"));
    m_previewLabel->setFixedSize(kPreviewCap, kPreviewCap);
    m_previewLabel->setFrameShape(QFrame::Box);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setToolTip(m_mode == Mode::Render ? QStringLiteral("Preview of the rendered texture")
                                                      : QStringLiteral("Preview: the tile repeated, so seams would show"));
    right->addWidget(m_previewLabel);
    m_info = new QLabel;
    m_info->setObjectName(QStringLiteral("procInfo"));
    m_info->setWordWrap(true);
    m_info->setMaximumWidth(kPreviewCap);
    right->addWidget(m_info);
    right->addStretch();
    if (m_mode == Mode::Render) {
        m_canvasPv = new QCheckBox(QStringLiteral("Preview on canvas"));
        m_canvasPv->setObjectName(QStringLiteral("fx_preview"));
        m_canvasPv->setChecked(true);
        m_canvasPv->setToolTip(QStringLiteral("Show the texture in the selected bitmaps while the dialog is open"));
        connect(m_canvasPv, &QCheckBox::toggled, this, &ProceduralDialog::canvasPreviewToggled);
        right->addWidget(m_canvasPv);
    }
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    right->addWidget(bb);
    root->addLayout(right);
    resize(sizeHint().width(), 600);

    // wiring
    connect(m_gen, &QComboBox::currentIndexChanged, this, [this] {
        if (m_sync) return;
        const QString k = m_gen->currentData().toString();
        if (k == m_p.gen) return;
        proc::Params np;                         // fresh defaults, keep seed / size / ramp / tone
        np.gen = k; np.seed = m_p.seed; np.width = m_p.width; np.height = m_p.height; np.tileable = m_p.tileable;
        np.ramp = m_p.ramp; np.contrast = m_p.contrast; np.brightness = m_p.brightness; np.invert = m_p.invert;
        const QStringList ps = proc::presetNames(k);
        if (!ps.isEmpty()) proc::applyPreset(np, ps.first());
        if (k == QLatin1String("cyclic") || k == QLatin1String("grayscott")) np.cell = 1;
        m_p = np;
        syncWidgets();
        changed();
    });
    connect(m_preset, &QComboBox::activated, this, [this](int i) {
        if (m_sync || i <= 0) return;
        readWidgets();
        proc::applyPreset(m_p, m_preset->itemText(i));
        syncWidgets();
        changed();
    });
    connect(rnd, &QPushButton::clicked, this, [this] { m_seed->setValue(QRandomGenerator::global()->bounded(1, 999999999)); });
    connect(m_lifePreset, &QComboBox::activated, this, [this](int i) { m_life->setText(proc::lifePresetRule(m_lifePreset->itemText(i))); });
    connect(m_rampPreset, &QComboBox::activated, this, [this](int i) {
        if (i <= 0) return;
        m_ramp->setStops(i == 1 ? QVector<GradStop>{{0, Qt::black}, {1, Qt::white}} : fills::gradientPreset(m_rampPreset->itemText(i)));
        m_rampPreset->setCurrentIndex(0);
        changed();
    });
    connect(editRamp, &QPushButton::clicked, this, [this] {
        GradientDialog gd(m_ramp->stops(), this);
        if (gd.exec() == QDialog::Accepted) { m_ramp->setStops(gd.stops()); changed(); }
    });
    connect(m_ramp, &GradientBar::changed, this, &ProceduralDialog::changed);
    for (QSpinBox *s : {m_seed, m_w, m_h, m_octaves, m_cell, m_gens, m_rule, m_states, m_threshold, m_range})
        connect(s, &QSpinBox::valueChanged, this, &ProceduralDialog::changed);
    for (QDoubleSpinBox *s : {m_scale, m_contrast, m_bright, m_pers, m_lac, m_warp, m_width2, m_density, m_feed, m_kill})
        connect(s, &QDoubleSpinBox::valueChanged, this, &ProceduralDialog::changed);
    for (QCheckBox *c : {m_tile, m_invert, m_single, m_moore}) connect(c, &QCheckBox::toggled, this, &ProceduralDialog::changed);
    for (QComboBox *c : {m_basis, m_variant, m_metric, m_pattern}) connect(c, &QComboBox::currentIndexChanged, this, &ProceduralDialog::changed);
    connect(m_life, &QLineEdit::textChanged, this, &ProceduralDialog::changed);
}

void ProceduralDialog::setParams(const proc::Params &p) {
    m_p = p;
    syncWidgets();
    changed();
}

void ProceduralDialog::syncWidgets() {
    m_sync = true;
    const proc::Params &p = m_p;
    m_gen->setCurrentIndex(qMax(0, m_gen->findData(p.gen)));
    m_preset->clear();
    m_preset->addItem(QStringLiteral("Choose..."));
    m_preset->addItems(proc::presetNames(p.gen));
    m_seed->setValue(int(qMin<quint32>(p.seed, 999999999u)));
    m_w->setValue(p.width); m_h->setValue(p.height);
    m_tile->setChecked(p.tileable); m_invert->setChecked(p.invert);
    m_scale->setValue(p.scale); m_contrast->setValue(p.contrast); m_bright->setValue(p.brightness);
    m_basis->setCurrentIndex(p.basis); m_octaves->setValue(p.octaves); m_pers->setValue(p.persistence); m_lac->setValue(p.lacunarity);
    m_warp->setValue(p.warp);
    m_variant->setCurrentIndex(qBound(0, p.variant, m_variant->count() - 1));
    m_pattern->setCurrentIndex(qBound(0, p.variant, m_pattern->count() - 1));
    m_metric->setCurrentIndex(p.metric); m_width2->setValue(p.width2);
    m_feed->setValue(p.feed); m_kill->setValue(p.kill);
    m_rule->setValue(p.rule); m_single->setChecked(p.singleCell);
    m_life->setText(p.lifeRule);
    m_states->setValue(p.states); m_threshold->setValue(p.threshold); m_range->setValue(p.range); m_moore->setChecked(p.moore);
    m_cell->setValue(p.cell); m_gens->setValue(p.generations); m_density->setValue(p.density);
    m_ramp->setStops(p.ramp.isEmpty() ? QVector<GradStop>{{0, Qt::black}, {1, Qt::white}} : p.ramp);
    const QVector<QColor> defStates{QColor(0x10, 0x10, 0x20), Qt::white, QColor(0x40, 0x70, 0xff)};
    for (int i = 0; i < 3; ++i) m_stateCol[i]->setColor(i < p.stateColors.size() ? p.stateColors[i] : defStates[i]);
    // show only what the generator uses
    QStringList used = proc::usedParams(p.gen);
    if (!used.contains(QStringLiteral("cell"))) used << QStringLiteral("scale");
    if (!proc::presetNames(p.gen).isEmpty() && p.gen != QLatin1String("pattern")) used << QStringLiteral("preset");
    if (m_mode != Mode::Render) used << QStringLiteral("size");
    if (p.gen != QLatin1String("brain")) used << QStringLiteral("ramp");
    for (auto it = m_groups.cbegin(); it != m_groups.cend(); ++it)
        for (QWidget *w : it.value()) w->setVisible(used.contains(it.key()));
    m_sync = false;
}

void ProceduralDialog::readWidgets() {
    proc::Params &p = m_p;
    p.gen = m_gen->currentData().toString();
    p.seed = quint32(m_seed->value());
    p.width = m_w->value(); p.height = m_h->value();
    p.tileable = m_tile->isChecked(); p.invert = m_invert->isChecked();
    p.scale = m_scale->value(); p.contrast = m_contrast->value(); p.brightness = m_bright->value();
    p.basis = m_basis->currentIndex(); p.octaves = m_octaves->value(); p.persistence = m_pers->value(); p.lacunarity = m_lac->value();
    p.warp = m_warp->value();
    p.variant = p.gen == QLatin1String("pattern") ? m_pattern->currentIndex() : m_variant->currentIndex();
    p.metric = m_metric->currentIndex(); p.width2 = m_width2->value();
    p.feed = m_feed->value(); p.kill = m_kill->value();
    p.rule = m_rule->value(); p.singleCell = m_single->isChecked();
    if (proc::parseLifeRule(m_life->text()).valid) p.lifeRule = m_life->text().trimmed().toUpper();
    p.states = m_states->value(); p.threshold = m_threshold->value(); p.range = m_range->value(); p.moore = m_moore->isChecked();
    p.cell = m_cell->value(); p.generations = m_gens->value(); p.density = m_density->value();
    const QVector<GradStop> st = m_ramp->stops();
    const bool plain = st.size() == 2 && st[0].pos <= 0 && st[1].pos >= 1 && st[0].color == QColor(Qt::black) && st[1].color == QColor(Qt::white);
    p.ramp = plain ? QVector<GradStop>{} : st;
    p.stateColors.clear();
    if (p.gen == QLatin1String("brain")) for (ColorButton *b : m_stateCol) p.stateColors << b->color();
}

void ProceduralDialog::changed() {
    if (m_sync) return;
    readWidgets();
    m_life->setStyleSheet(proc::parseLifeRule(m_life->text()).valid ? QString() : QStringLiteral("color: #c00000"));
    m_debounce->start();
}

QSize ProceduralDialog::renderSize() const {
    return m_mode == Mode::Render && m_target.isValid() ? m_target : QSize(m_p.width, m_p.height);
}

proc::Params ProceduralDialog::previewParams(const proc::Params &p, QSize size, int cap) {
    proc::Params q = p;
    q.width = size.width(); q.height = size.height();
    const int longest = qMax(size.width(), size.height());
    if (longest > cap) {
        const double k = double(cap) / longest;
        q.width = qMax(1, qRound(size.width() * k));
        q.height = qMax(1, qRound(size.height() * k));
        q.cell = qMax(1, qRound(p.cell * k));
    }
    return q;
}

void ProceduralDialog::startPreview() {
    if (m_watch.isRunning()) {           // restart once the current one stops
        if (m_cancel) m_cancel->store(true);
        m_pending = true;
        return;
    }
    m_pending = false;
    m_unshown = true;
    const proc::Params q = previewParams(m_p, renderSize());
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_cancel;
    m_info->setText(QStringLiteral("Rendering..."));
    m_watch.setFuture(QtConcurrent::run([q, cancel]() { return proc::render(q, q.width, q.height, cancel.get()); }));
}

void ProceduralDialog::showPreview() {
    const bool cancelled = m_cancel && m_cancel->load();
    if (m_pending || cancelled) { startPreview(); return; }
    m_preview = m_watch.result();
    m_unshown = false;
    ++m_rendered;
    QPixmap pm(kPreviewCap, kPreviewCap);
    pm.fill(Qt::white);
    {
        QPainter pt(&pm);
        if (m_mode == Mode::Render) {
            const QSize s = m_preview.size().scaled(kPreviewCap, kPreviewCap, Qt::KeepAspectRatio);
            pt.setRenderHint(QPainter::SmoothPixmapTransform);
            pt.drawImage(QRect(QPoint((kPreviewCap - s.width()) / 2, (kPreviewCap - s.height()) / 2), s), m_preview);
        } else {
            // the tile at up to half the box, repeated 2x2+ so seams show
            QImage t = m_preview;
            const int half = kPreviewCap / 2;
            if (t.width() > half || t.height() > half) t = t.scaled(half, half, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            pt.fillRect(pm.rect(), QBrush(t));
        }
    }
    m_previewLabel->setPixmap(pm);
    const QSize rs = renderSize();
    m_info->setText(QStringLiteral("%1 — %2 x %3 px%4").arg(proc::generatorLabel(m_p.gen)).arg(rs.width()).arg(rs.height())
                        .arg(m_preview.size() != rs ? QStringLiteral(" (preview %1 x %2)").arg(m_preview.width()).arg(m_preview.height()) : QString()));
    emit previewUpdated();
}

// m_unshown covers the gap between the worker finishing and the queued finished() signal arriving
// (wide under ASan), so waitForPreview() never returns before the image is shown.
bool ProceduralDialog::previewBusy() const { return m_watch.isRunning() || m_pending || m_unshown || m_debounce->isActive(); }

bool ProceduralDialog::waitForPreview(int ms) {
    QElapsedTimer t;
    t.start();
    while (previewBusy() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !previewBusy();
}

bool ProceduralDialog::canvasPreview() const { return m_canvasPv && m_canvasPv->isChecked(); }
