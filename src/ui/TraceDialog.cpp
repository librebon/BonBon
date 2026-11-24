#include "PopupSlider.h"
#include "TraceDialog.h"
#include "core/FxExtra.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {
constexpr int kPreview = 320;

bool sameOptions(const trace::Options &a, const trace::Options &b) {
    return a.mode == b.mode && a.threshold == b.threshold && a.invert == b.invert && a.turdSize == b.turdSize &&
           qFuzzyCompare(a.alphaMax + 1, b.alphaMax + 1) && a.optimize == b.optimize &&
           qFuzzyCompare(a.optTolerance + 1, b.optTolerance + 1) && a.colors == b.colors && a.palette == b.palette &&
           a.stacked == b.stacked && a.smoothing == b.smoothing && a.ignoreTransparent == b.ignoreTransparent &&
           a.removeBackground == b.removeBackground;
}

QImage checker(QSize s) {
    QImage c(s, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&c);
    for (int y = 0; y < s.height(); y += 8)
        for (int x = 0; x < s.width(); x += 8)
            p.fillRect(x, y, 8, 8, ((x / 8 + y / 8) & 1) ? QColor(0xcc, 0xcc, 0xcc) : Qt::white);
    return c;
}
}

TraceDialog::TraceDialog(const QImage &src, QWidget *parent) : QDialog(parent), m_src(src.convertToFormat(QImage::Format_ARGB32)) {
    setObjectName(QStringLiteral("traceDialog"));
    setWindowTitle(QStringLiteral("Trace Bitmap"));
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(150);
    connect(m_debounce, &QTimer::timeout, this, &TraceDialog::startPreview);
    connect(&m_watch, &QFutureWatcher<trace::Result>::finished, this, &TraceDialog::showPreview);
    build();
    syncVisibility();
    startPreview();
}

TraceDialog::~TraceDialog() {
    if (m_cancel) m_cancel->store(true);
    m_watch.waitForFinished();
}

void TraceDialog::build() {
    auto *top = new QHBoxLayout(this);
    auto *left = new QVBoxLayout;
    top->addLayout(left);

    auto *modeForm = new QFormLayout;
    m_mode = new QComboBox;
    m_mode->setObjectName(QStringLiteral("traceMode"));
    m_mode->addItems({QStringLiteral("Black and white"), QStringLiteral("Colors"), QStringLiteral("Pixel art (exact)"), QStringLiteral("Centerline")});
    modeForm->addRow(QStringLiteral("&Mode:"), m_mode);
    left->addLayout(modeForm);

    m_pages = new QStackedWidget;
    // page 0: threshold (B&W and centreline)
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->setContentsMargins(0, 0, 0, 0);
        m_threshold = new PopupSliderField;
        m_threshold->setObjectName(QStringLiteral("traceThreshold"));
        m_threshold->setRange(0, 255);
        m_threshold->setValue(128);
        m_threshold->setToolTip(QStringLiteral("Pixels darker than this (luminance 0-255) are traced"));
        f->addRow(QStringLiteral("&Threshold:"), m_threshold);
        m_invert = new QCheckBox(QStringLiteral("&Invert (trace the light pixels)"));
        m_invert->setObjectName(QStringLiteral("traceInvert"));
        f->addRow(m_invert);
        m_pages->addWidget(w);
    }
    // page 1: colours
    {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->setContentsMargins(0, 0, 0, 0);
        m_palette = new QComboBox;
        m_palette->setObjectName(QStringLiteral("tracePalette"));
        m_palette->addItems({QStringLiteral("Adaptive"), QStringLiteral("Image palette (indexed)"), QStringLiteral("Web 216"), QStringLiteral("Grayscale")});
        f->addRow(QStringLiteral("&Palette:"), m_palette);
        m_colors = new PopupSliderField;
        m_colors->setObjectName(QStringLiteral("traceColors"));
        m_colors->setRange(2, 256);
        m_colors->setValue(8);
        f->addRow(QStringLiteral("&Colors:"), m_colors);
        m_layout = new QComboBox;
        m_layout->setObjectName(QStringLiteral("traceStacking"));
        m_layout->addItems({QStringLiteral("Stacked (no gaps)"), QStringLiteral("Cut-out (no overlap)")});
        f->addRow(QStringLiteral("&Shapes:"), m_layout);
        m_smoothing = new PopupSliderField;
        m_smoothing->setObjectName(QStringLiteral("traceSmoothing"));
        m_smoothing->setRange(0, 10);
        m_smoothing->setToolTip(QStringLiteral("Majority-filter passes over the colour map: fewer specks and nodes"));
        f->addRow(QStringLiteral("S&moothing:"), m_smoothing);
        m_pages->addWidget(w);
    }
    // page 2: pixel art
    {
        auto *l = new QLabel(QStringLiteral("Every colour becomes exact pixel-edge polygons; the paths re-rasterise to the original pixels."));
        l->setWordWrap(true);
        m_pages->addWidget(l);
    }
    left->addWidget(m_pages);

    auto *curves = new QGroupBox(QStringLiteral("Curves"));
    m_curveBox = curves;
    {
        auto *f = new QFormLayout(curves);
        m_turd = new PopupSliderField;
        m_turd->setObjectName(QStringLiteral("traceTurd"));
        m_turd->setRange(0, 1000);
        m_turd->setValue(2);
        m_turd->setSuffix(QStringLiteral(" px"));
        m_turd->setToolTip(QStringLiteral("Ignore specks and holes of at most this many pixels"));
        f->addRow(QStringLiteral("I&gnore specks up to:"), m_turd);
        m_alpha = new PopupSliderDoubleField;
        m_alpha->setObjectName(QStringLiteral("traceAlphaMax"));
        m_alpha->setRange(0, 1.334);
        m_alpha->setDecimals(3);
        m_alpha->setSingleStep(0.05);
        m_alpha->setValue(1.0);
        m_alpha->setToolTip(QStringLiteral("Corner threshold: 0 = all corners (polygons), 1.334 = no corners"));
        f->addRow(QStringLiteral("Smooth c&orners:"), m_alpha);
        m_optimize = new QCheckBox(QStringLiteral("Optimi&ze curves"));
        m_optimize->setObjectName(QStringLiteral("traceOptimize"));
        m_optimize->setChecked(true);
        f->addRow(m_optimize);
        m_tol = new PopupSliderDoubleField;
        m_tol->setObjectName(QStringLiteral("traceTolerance"));
        m_tol->setRange(0, 5);
        m_tol->setDecimals(2);
        m_tol->setSingleStep(0.05);
        m_tol->setValue(0.2);
        m_tol->setSuffix(QStringLiteral(" px"));
        f->addRow(QStringLiteral("Tolera&nce:"), m_tol);
    }
    left->addWidget(curves);

    auto *opts = new QGroupBox(QStringLiteral("Options"));
    {
        auto *v = new QVBoxLayout(opts);
        m_ignoreAlpha = new QCheckBox(QStringLiteral("Transparent pixels are &background"));
        m_ignoreAlpha->setObjectName(QStringLiteral("traceIgnoreAlpha"));
        m_ignoreAlpha->setChecked(true);
        m_ignoreAlpha->setToolTip(QStringLiteral("Off: transparent areas are traced as if on white"));
        m_removeBg = new QCheckBox(QStringLiteral("&Remove background color"));
        m_removeBg->setObjectName(QStringLiteral("traceRemoveBg"));
        m_removeBg->setToolTip(QStringLiteral("Drop the most common border colour"));
        m_group = new QCheckBox(QStringLiteral("Put paths on a new &layer (group)"));
        m_group->setObjectName(QStringLiteral("traceGroup"));
        m_group->setChecked(true);
        m_asGroup = new QCheckBox(QStringLiteral("Output as a g&roup"));
        m_asGroup->setObjectName(QStringLiteral("traceAsGroup"));
        m_asGroup->setToolTip(QStringLiteral("Put the traced paths in one group (Modify > Ungroup splits it)"));
        m_keep = new QCheckBox(QStringLiteral("&Keep original bitmap"));
        m_keep->setObjectName(QStringLiteral("traceKeep"));
        m_keep->setChecked(true);
        for (QCheckBox *c : {m_ignoreAlpha, m_removeBg, m_group, m_asGroup, m_keep}) v->addWidget(c);
    }
    left->addWidget(opts);
    left->addStretch(1);

    auto *right = new QVBoxLayout;
    top->addLayout(right);
    m_preview = new QLabel;
    m_preview->setObjectName(QStringLiteral("tracePreview"));
    m_preview->setFixedSize(kPreview, kPreview);
    m_preview->setFrameShape(QFrame::Box);
    m_preview->setAlignment(Qt::AlignCenter);
    right->addWidget(m_preview);
    m_stats = new QLabel(QStringLiteral("Tracing..."));
    m_stats->setObjectName(QStringLiteral("traceStats"));
    right->addWidget(m_stats);
    auto *row = new QHBoxLayout;
    m_live = new QCheckBox(QStringLiteral("Li&ve preview"));
    m_live->setObjectName(QStringLiteral("traceLive"));
    m_live->setChecked(true);
    m_showSrc = new QCheckBox(QStringLiteral("Show &bitmap"));
    m_showSrc->setObjectName(QStringLiteral("traceShowSource"));
    m_showSrc->setToolTip(QStringLiteral("Draw the bitmap faintly under the paths"));
    row->addWidget(m_live);
    row->addWidget(m_showSrc);
    row->addStretch(1);
    right->addLayout(row);
    right->addStretch(1);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    right->addWidget(bb);

    auto ch = [this] { changed(); };
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] { syncVisibility(); changed(); });
    for (QComboBox *c : {m_palette, m_layout}) connect(c, &QComboBox::currentIndexChanged, this, ch);
    for (QSpinBox *s : {m_threshold, m_colors, m_smoothing, m_turd}) connect(s, &QSpinBox::valueChanged, this, ch);
    for (QDoubleSpinBox *s : {m_alpha, m_tol}) connect(s, &QDoubleSpinBox::valueChanged, this, ch);
    for (QCheckBox *c : {m_invert, m_optimize, m_ignoreAlpha, m_removeBg}) connect(c, &QCheckBox::toggled, this, ch);
    connect(m_optimize, &QCheckBox::toggled, m_tol, &QWidget::setEnabled);
    connect(m_live, &QCheckBox::toggled, this, [this](bool on) { if (on) changed(); });
    connect(m_showSrc, &QCheckBox::toggled, this, &TraceDialog::paintPreview);
    connect(m_palette, &QComboBox::currentIndexChanged, this, [this](int i) { m_colors->setEnabled(i == PalAdaptive); });
}

void TraceDialog::syncVisibility() {
    const auto m = trace::Mode(m_mode->currentIndex());
    m_pages->setCurrentIndex(m == trace::Mode::Colors ? 1 : m == trace::Mode::PixelArt ? 2 : 0);
    m_curveBox->setEnabled(m != trace::Mode::PixelArt);
}

QVector<QRgb> TraceDialog::paletteFor(int choice, const QImage &img, int colors) {
    switch (choice) {
    case PalImage: {
        const QVector<QRgb> u = fx2::uniqueColors(img, 257);
        if (u.size() <= 256) return u;
        return fx2::targetPalette(fx2::PalAdaptive, 256, img, {});
    }
    case PalWeb: return fx2::targetPalette(fx2::PalWeb, 216, img, {});
    case PalGray: return fx2::targetPalette(fx2::PalGray, qBound(2, colors, 256), img, {});
    default: return {};
    }
}

int TraceDialog::paletteChoice() const { return m_palette->currentIndex(); }

trace::Options TraceDialog::options() const {
    trace::Options o;
    o.mode = trace::Mode(m_mode->currentIndex());
    o.threshold = m_threshold->value();
    o.invert = m_invert->isChecked();
    o.turdSize = m_turd->value();
    o.alphaMax = m_alpha->value();
    o.optimize = m_optimize->isChecked();
    o.optTolerance = m_tol->value();
    o.colors = m_colors->value();
    o.stacked = m_layout->currentIndex() == 0;
    o.smoothing = m_smoothing->value();
    o.ignoreTransparent = m_ignoreAlpha->isChecked();
    o.removeBackground = m_removeBg->isChecked();
    if (o.mode == trace::Mode::Colors) o.palette = paletteFor(m_palette->currentIndex(), m_src, o.colors);
    return o;
}

void TraceDialog::setOptions(const trace::Options &o, int paletteChoice) {
    m_sync = true;
    m_mode->setCurrentIndex(int(o.mode));
    m_threshold->setValue(o.threshold);
    m_invert->setChecked(o.invert);
    m_turd->setValue(o.turdSize);
    m_alpha->setValue(o.alphaMax);
    m_optimize->setChecked(o.optimize);
    m_tol->setValue(o.optTolerance);
    m_colors->setValue(o.colors);
    m_palette->setCurrentIndex(qBound(0, paletteChoice, m_palette->count() - 1));
    m_layout->setCurrentIndex(o.stacked ? 0 : 1);
    m_smoothing->setValue(o.smoothing);
    m_ignoreAlpha->setChecked(o.ignoreTransparent);
    m_removeBg->setChecked(o.removeBackground);
    m_sync = false;
    syncVisibility();
    changed();
}

trace::Insert TraceDialog::insertOptions() const {
    trace::Insert i;
    i.group = m_group->isChecked();
    i.keepOriginal = m_keep->isChecked();
    i.asGroup = m_asGroup->isChecked();
    i.hardEdges = m_mode->currentIndex() == int(trace::Mode::PixelArt);
    return i;
}

void TraceDialog::setInsertOptions(const trace::Insert &i) {
    m_group->setChecked(i.group);
    m_keep->setChecked(i.keepOriginal);
    m_asGroup->setChecked(i.asGroup);
}


void TraceDialog::changed() {
    if (m_sync) return;
    if (!m_live->isChecked()) {
        m_stats->setText(QStringLiteral("Preview off"));
        return;
    }
    m_debounce->start();
}

void TraceDialog::startPreview() {
    if (m_watch.isRunning()) {   // restart when the current one stops
        if (m_cancel) m_cancel->store(true);
        m_pending = true;
        return;
    }
    m_pending = false;
    m_unshown = true;
    m_running = options();
    m_cancel = std::make_shared<std::atomic_bool>(false);
    auto cancel = m_cancel;
    const QImage img = m_src;
    const trace::Options o = m_running;
    m_stats->setText(QStringLiteral("Tracing..."));
    m_watch.setFuture(QtConcurrent::run([img, o, cancel]() { return trace::trace(img, o, cancel.get()); }));
}

void TraceDialog::showPreview() {
    const bool cancelled = m_cancel && m_cancel->load();
    if (m_pending || cancelled) { startPreview(); return; }
    trace::Result r = m_watch.result();
    if (r.cancelled) { startPreview(); return; }
    m_result = std::move(r);
    m_resultOpts = m_running;
    m_hasResult = true;
    m_unshown = false;
    ++m_rendered;
    paintPreview();
}

void TraceDialog::paintPreview() {
    if (!m_hasResult) return;
    // picture: checkerboard, optional faint bitmap, the traced shapes fitted into the box
    QImage pic = checker(QSize(kPreview, kPreview));
    {
        QPainter p(&pic);
        p.setRenderHint(QPainter::Antialiasing, m_resultOpts.mode != trace::Mode::PixelArt);
        const QSize s = m_src.size().scaled(kPreview, kPreview, Qt::KeepAspectRatio);
        const QRectF box((kPreview - s.width()) / 2.0, (kPreview - s.height()) / 2.0, s.width(), s.height());
        if (m_showSrc->isChecked()) {
            p.setOpacity(0.35);
            p.drawImage(box, m_src);
            p.setOpacity(1);
        }
        p.translate(box.topLeft());
        if (!m_src.isNull()) p.scale(box.width() / m_src.width(), box.height() / m_src.height());
        const double k = m_src.isNull() ? 1 : box.width() / m_src.width();
        for (const trace::Shape &sh : m_result.shapes) {
            if (sh.stroke) {
                QPen pen(sh.color, sh.strokeWidth);
                pen.setCapStyle(Qt::RoundCap);
                pen.setJoinStyle(Qt::RoundJoin);
                pen.setCosmetic(sh.strokeWidth * k < 1);
                p.strokePath(sh.path, pen);
            } else {
                QPainterPath path = sh.path;
                path.setFillRule(Qt::OddEvenFill);
                p.fillPath(path, sh.color);
            }
        }
    }
    m_shown = pic;
    m_preview->setPixmap(QPixmap::fromImage(pic));
    m_stats->setText(statsText());
    emit previewUpdated();
}

QString TraceDialog::statsText() const {
    if (!m_hasResult) return QString();
    return QStringLiteral("%1 paths, %2 nodes").arg(m_result.shapes.size()).arg(m_result.nodes);
}

bool TraceDialog::previewBusy() const { return m_watch.isRunning() || m_pending || m_unshown || m_debounce->isActive(); }

bool TraceDialog::waitForPreview(int ms) {
    QElapsedTimer t;
    t.start();
    while (previewBusy() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return !previewBusy();
}

trace::Result TraceDialog::result() {
    const trace::Options o = options();
    if (m_hasResult && sameOptions(o, m_resultOpts)) return m_result;
    if (m_cancel) m_cancel->store(true);
    m_watch.waitForFinished();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    trace::Result r = trace::trace(m_src, o);
    QApplication::restoreOverrideCursor();
    return r;
}
