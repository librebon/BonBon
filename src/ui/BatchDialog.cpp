#include "Brand.h"
#include "PopupSlider.h"
#include "core/PixelScale.h"
#include "BatchDialog.h"
#include "AppState.h"
#include "Dialogs.h"
#include "core/Document.h"
#include "io/ExportPresets.h"
#include "io/LwdFormat.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include "FileDialogs.h"
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

using namespace batch;

static bool testing() { return qApp->property("lw_testing").toBool(); }

BatchDialog::BatchDialog(const QVector<QPair<QString, Document *>> &openDocs, const QString &startDir, QWidget *parent)
    : QWizard(parent), m_dir(startDir) {
    setObjectName(QStringLiteral("batchDialog"));
    setWindowTitle(QStringLiteral("Batch Process"));
    setWizardStyle(QWizard::ClassicStyle);
    setOption(QWizard::NoBackButtonOnStartPage);
    setOption(QWizard::HaveCustomButton1);
    setOption(QWizard::HaveCustomButton2);
    setButtonText(QWizard::CustomButton1, QStringLiteral("Load Script..."));
    setButtonText(QWizard::CustomButton2, QStringLiteral("Save Script..."));
    setButtonText(QWizard::FinishButton, QStringLiteral("&Batch"));
    for (const auto &p : openDocs) m_open << qMakePair(p.first, QPointer<Document>(p.second));
    // sensible defaults: export with the document's settings to a folder
    Step ex;
    ex.type = Export;
    ex.preset = exportpresets::names().value(0);
    m_s.steps << ex;
    buildInputPage();
    buildStepsPage();
    buildOutputPage();
    connect(this, &QWizard::customButtonClicked, this, [this](int which) {
        QString err;
        if (which == QWizard::CustomButton1) {
            const QString f = filedialogs::getOpenFileName(this, QStringLiteral("Load Batch Script"), m_dir, QStringLiteral("Batch scripts (*.json)"));
            if (!f.isEmpty() && !loadScript(f, &err)) QMessageBox::warning(this, QStringLiteral("Batch Process"), err);
        } else {
            QString f = filedialogs::getSaveFileName(this, QStringLiteral("Save Batch Script"), QDir(m_dir).filePath(QStringLiteral("batch.json")), QStringLiteral("Batch scripts (*.json)"));
            if (f.isEmpty()) return;
            if (QFileInfo(f).suffix().isEmpty()) f += QStringLiteral(".json");
            if (!saveScript(f, &err)) QMessageBox::warning(this, QStringLiteral("Batch Process"), err);
        }
    });
    resize(640, 520);
    syncUi();
}

QStringList BatchDialog::openDocNames() const {
    QStringList n;
    for (const auto &p : m_open) if (p.second) n << p.first;
    return n;
}

void BatchDialog::buildInputPage() {
    auto *page = new QWizardPage;
    page->setTitle(QStringLiteral("Files to process"));
    auto *v = new QVBoxLayout(page);
    m_inFiles = new QRadioButton(QStringLiteral("&Files:"));
    m_inFiles->setObjectName(QStringLiteral("batchInFiles"));
    m_inFolder = new QRadioButton(QStringLiteral("All files in a f&older:"));
    m_inFolder->setObjectName(QStringLiteral("batchInFolder"));
    m_inOpen = new QRadioButton(QStringLiteral("Current &open documents (%1)").arg(openDocNames().size()));
    m_inOpen->setObjectName(QStringLiteral("batchInOpen"));
    m_inOpen->setEnabled(!openDocNames().isEmpty());
    auto *g = new QButtonGroup(page);
    g->addButton(m_inFiles, Script::Files);
    g->addButton(m_inFolder, Script::Folder);
    g->addButton(m_inOpen, Script::OpenDocuments);
    v->addWidget(m_inFiles);
    m_files = new QListWidget;
    m_files->setObjectName(QStringLiteral("batchFiles"));
    m_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto *fr = new QHBoxLayout;
    fr->addWidget(m_files, 1);
    auto *fb = new QVBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add..."));
    auto *rem = new QPushButton(QStringLiteral("Remove"));
    fb->addWidget(add); fb->addWidget(rem); fb->addStretch();
    fr->addLayout(fb);
    v->addLayout(fr, 1);
    v->addWidget(m_inFolder);
    auto *fl = new QHBoxLayout;
    m_folder = new QLineEdit;
    m_folder->setObjectName(QStringLiteral("batchFolder"));
    auto *browse = new QPushButton(QStringLiteral("Browse..."));
    fl->addWidget(m_folder, 1); fl->addWidget(browse);
    v->addLayout(fl);
    auto *pf = new QFormLayout;
    m_patterns = new QLineEdit;
    m_patterns->setObjectName(QStringLiteral("batchPatterns"));
    m_recursive = new QCheckBox(QStringLiteral("Include sub&folders"));
    m_recursive->setObjectName(QStringLiteral("batchRecursive"));
    pf->addRow(QStringLiteral("File types:"), m_patterns);
    pf->addRow(QString(), m_recursive);
    v->addLayout(pf);
    v->addWidget(m_inOpen);
    m_inCount = new QLabel;
    m_inCount->setObjectName(QStringLiteral("batchInCount"));
    v->addWidget(m_inCount);
    addPage(page);

    auto upd = [this] {
        if (m_syncing) return;
        m_s.input = m_inFolder->isChecked() ? Script::Folder : m_inOpen->isChecked() ? Script::OpenDocuments : Script::Files;
        m_s.files.clear();
        for (int i = 0; i < m_files->count(); ++i) m_s.files << m_files->item(i)->data(Qt::UserRole).toString();
        m_s.folder = m_folder->text();
        m_s.patterns = m_patterns->text();
        m_s.recursive = m_recursive->isChecked();
        const int n = m_s.input == Script::OpenDocuments ? int(openDocNames().size()) : int(inputFiles(m_s).size());
        m_inCount->setText(QStringLiteral("%1 file(s) will be processed.").arg(n));
        m_files->setEnabled(m_s.input == Script::Files);
        m_folder->setEnabled(m_s.input == Script::Folder);
        m_patterns->setEnabled(m_s.input == Script::Folder);
        m_recursive->setEnabled(m_s.input == Script::Folder);
    };
    connect(g, &QButtonGroup::idToggled, this, upd);
    connect(m_folder, &QLineEdit::textChanged, this, upd);
    connect(m_patterns, &QLineEdit::textChanged, this, upd);
    connect(m_recursive, &QCheckBox::toggled, this, upd);
    connect(add, &QPushButton::clicked, this, [this, upd] {
        const QStringList fs = filedialogs::getOpenFileNames(this, QStringLiteral("Add Files"), m_dir,
                                                             QStringLiteral("Images (%1);;All files (*)").arg(Script().patterns));
        for (const QString &f : fs) {
            auto *it = new QListWidgetItem(QFileInfo(f).fileName(), m_files);
            it->setData(Qt::UserRole, f);
            it->setToolTip(f);
        }
        if (!fs.isEmpty()) { m_dir = QFileInfo(fs.first()).absolutePath(); m_inFiles->setChecked(true); }
        upd();
    });
    connect(rem, &QPushButton::clicked, this, [this, upd] { qDeleteAll(m_files->selectedItems()); upd(); });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = filedialogs::getExistingDirectory(this, QStringLiteral("Folder to Process"), m_folder->text().isEmpty() ? m_dir : m_folder->text());
        if (!d.isEmpty()) { m_folder->setText(d); m_inFolder->setChecked(true); }
    });
}

void BatchDialog::buildStepsPage() {
    auto *page = new QWizardPage;
    page->setTitle(QStringLiteral("Steps"));
    page->setSubTitle(QStringLiteral("Run in order on every file. Uncheck a step to skip it."));
    auto *v = new QVBoxLayout(page);
    auto *h = new QHBoxLayout;
    m_steps = new QListWidget;
    m_steps->setObjectName(QStringLiteral("batchSteps"));
    h->addWidget(m_steps, 1);
    auto *bv = new QVBoxLayout;
    auto *addB = new QToolButton;
    addB->setObjectName(QStringLiteral("batchAddStep"));
    addB->setText(QStringLiteral("Add Step"));
    addB->setPopupMode(QToolButton::InstantPopup);
    addB->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto *menu = new QMenu(addB);
    const QStringList names = stepTypeNames();
    for (int t = 0; t < kStepTypes; ++t) menu->addAction(names.value(t), this, [this, t] { addStep(t); });
    addB->setMenu(menu);
    auto *rem = new QPushButton(QStringLiteral("Remove"));
    auto *up = new QPushButton(QStringLiteral("Move Up"));
    auto *dn = new QPushButton(QStringLiteral("Move Down"));
    for (QWidget *w : {static_cast<QWidget *>(addB), static_cast<QWidget *>(rem), static_cast<QWidget *>(up), static_cast<QWidget *>(dn)}) {
        w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        bv->addWidget(w);
    }
    bv->addStretch();
    h->addLayout(bv);
    v->addLayout(h, 1);
    m_editorHost = new QWidget;
    m_editorHost->setObjectName(QStringLiteral("batchStepEditor"));
    m_editorLay = new QVBoxLayout(m_editorHost);
    m_editorLay->setContentsMargins(0, 0, 0, 0);
    v->addWidget(m_editorHost);
    addPage(page);
    connect(rem, &QPushButton::clicked, this, [this] {
        const int r = m_steps->currentRow();
        if (r < 0 || r >= m_s.steps.size()) return;
        m_s.steps.remove(r);
        refreshSteps(qMin(r, int(m_s.steps.size()) - 1));
    });
    connect(up, &QPushButton::clicked, this, [this] { moveStep(-1); });
    connect(dn, &QPushButton::clicked, this, [this] { moveStep(1); });
    connect(m_steps, &QListWidget::currentRowChanged, this, [this](int r) { editStep(r); });
    connect(m_steps, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_syncing) return;
        const int r = m_steps->row(it);
        if (r >= 0 && r < m_s.steps.size()) m_s.steps[r].enabled = it->checkState() == Qt::Checked;
    });
}

void BatchDialog::buildOutputPage() {
    auto *page = new QWizardPage;
    page->setTitle(QStringLiteral("Output"));
    auto *v = new QVBoxLayout(page);
    m_outFolder = new QRadioButton(QStringLiteral("Save in a &folder:"));
    m_outFolder->setObjectName(QStringLiteral("batchOutFolder"));
    m_outOverwrite = new QRadioButton(QStringLiteral("&Overwrite the originals (same folder)"));
    m_outOverwrite->setObjectName(QStringLiteral("batchOutOverwrite"));
    m_outBackup = new QRadioButton(QStringLiteral("Same folder, &backing up the originals into:"));
    m_outBackup->setObjectName(QStringLiteral("batchOutBackup"));
    auto *g = new QButtonGroup(page);
    g->addButton(m_outFolder, Script::ToFolder);
    g->addButton(m_outOverwrite, Script::Overwrite);
    g->addButton(m_outBackup, Script::Backup);
    v->addWidget(m_outFolder);
    auto *h = new QHBoxLayout;
    m_outDir = new QLineEdit;
    m_outDir->setObjectName(QStringLiteral("batchOutDir"));
    auto *browse = new QPushButton(QStringLiteral("Browse..."));
    h->addWidget(m_outDir, 1); h->addWidget(browse);
    v->addLayout(h);
    v->addWidget(m_outOverwrite);
    v->addWidget(m_outBackup);
    m_backupDir = new QLineEdit;
    m_backupDir->setObjectName(QStringLiteral("batchBackupDir"));
    v->addWidget(m_backupDir);
    auto *note = new QLabel(QStringLiteral("Without an Export or Convert step each file is saved in its own format. "
                                           "Use File > Batch Process > Save Script to reuse these settings, or run them "
                                           "from a terminal: ") + brand::nameLower() + QStringLiteral(" --batch script.json files..."));
    note->setWordWrap(true);
    v->addWidget(note);
    v->addStretch();
    addPage(page);
    auto upd = [this] {
        if (m_syncing) return;
        m_s.output = m_outBackup->isChecked() ? Script::Backup : m_outOverwrite->isChecked() ? Script::Overwrite : Script::ToFolder;
        m_s.outDir = m_outDir->text();
        m_s.backupDir = m_backupDir->text();
        m_outDir->setEnabled(m_s.output == Script::ToFolder);
        m_backupDir->setEnabled(m_s.output == Script::Backup);
    };
    connect(g, &QButtonGroup::idToggled, this, upd);
    connect(m_outDir, &QLineEdit::textChanged, this, upd);
    connect(m_backupDir, &QLineEdit::textChanged, this, upd);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = filedialogs::getExistingDirectory(this, QStringLiteral("Output Folder"), m_outDir->text().isEmpty() ? m_dir : m_outDir->text());
        if (!d.isEmpty()) { m_outDir->setText(d); m_outFolder->setChecked(true); }
    });
}

void BatchDialog::syncUi() {
    m_syncing = true;
    m_files->clear();
    for (const QString &f : m_s.files) {
        auto *it = new QListWidgetItem(QFileInfo(f).fileName(), m_files);
        it->setData(Qt::UserRole, f);
        it->setToolTip(f);
    }
    m_folder->setText(m_s.folder);
    m_patterns->setText(m_s.patterns);
    m_recursive->setChecked(m_s.recursive);
    (m_s.input == Script::Folder ? m_inFolder : m_s.input == Script::OpenDocuments && m_inOpen->isEnabled() ? m_inOpen : m_inFiles)->setChecked(true);
    m_outDir->setText(m_s.outDir);
    m_backupDir->setText(m_s.backupDir);
    (m_s.output == Script::Overwrite ? m_outOverwrite : m_s.output == Script::Backup ? m_outBackup : m_outFolder)->setChecked(true);
    m_syncing = false;
    // re-derive the enabled states / counts
    const Script keep = m_s;
    m_folder->setText(QString()); m_folder->setText(keep.folder);
    m_outDir->setText(QString()); m_outDir->setText(keep.outDir);
    m_s = keep;
    refreshSteps(m_s.steps.isEmpty() ? -1 : 0);
}

void BatchDialog::refreshSteps(int select) {
    m_syncing = true;
    m_steps->clear();
    for (int i = 0; i < m_s.steps.size(); ++i) {
        auto *it = new QListWidgetItem(QStringLiteral("%1. %2").arg(i + 1).arg(m_s.steps[i].summary()), m_steps);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(m_s.steps[i].enabled ? Qt::Checked : Qt::Unchecked);
    }
    m_syncing = false;
    m_steps->setCurrentRow(select);
    if (select < 0) editStep(-1);
}

void BatchDialog::addStep(int type) {
    Step s;
    s.type = type;
    if (type == Export) s.preset = exportpresets::names().value(0);
    if (type == Convert) s.format = QStringLiteral("png");
    if (type == Command) s.command = commandKeys().value(0);
    if (type == Rename) s.suffix = QStringLiteral("_new");
    const int at = m_steps->currentRow() < 0 ? int(m_s.steps.size()) : m_steps->currentRow() + 1;
    m_s.steps.insert(at, s);
    refreshSteps(at);
}

void BatchDialog::moveStep(int delta) {
    const int r = m_steps->currentRow(), to = r + delta;
    if (r < 0 || to < 0 || to >= m_s.steps.size()) return;
    m_s.steps.move(r, to);
    refreshSteps(to);
}

void BatchDialog::editStep(int row) {
    while (QLayoutItem *li = m_editorLay->takeAt(0)) { delete li->widget(); delete li; }
    if (row < 0 || row >= m_s.steps.size()) return;
    auto *w = new QWidget;
    auto *f = new QFormLayout(w);
    f->setContentsMargins(0, 6, 0, 0);
    m_editorLay->addWidget(w);
    Step &st = m_s.steps[row];
    auto touch = [this, row] {
        if (QListWidgetItem *it = m_steps->item(row)) {
            m_syncing = true;
            it->setText(QStringLiteral("%1. %2").arg(row + 1).arg(m_s.steps[row].summary()));
            m_syncing = false;
        }
    };
    auto spin = [&](int lo, int hi, int val, const QString &suffix, const std::function<void(int)> &set) {
        auto *s = new PopupSliderField;
        s->setRange(lo, hi); s->setValue(val); s->setSuffix(suffix);
        connect(s, &QSpinBox::valueChanged, this, [set, touch](int x) { set(x); touch(); });
        return s;
    };
    auto line = [&](const QString &val, const std::function<void(const QString &)> &set) {
        auto *e = new QLineEdit(val);
        connect(e, &QLineEdit::textChanged, this, [set, touch](const QString &x) { set(x); touch(); });
        return e;
    };
    auto check = [&](const QString &label, bool val, const std::function<void(bool)> &set) {
        auto *c = new QCheckBox(label);
        c->setChecked(val);
        connect(c, &QCheckBox::toggled, this, [set, touch](bool x) { set(x); touch(); });
        return c;
    };
    Step *sp = &st;   // m_s.steps is not resized while this editor is shown (add/remove rebuild it)
    // Export / Convert: "Export area" -- only a rectangle of each canvas (the Export Area tool's saved one)
    auto areaRows = [&] {
        auto *use = check(QStringLiteral("Export area only"), st.useArea, [sp](bool x) { sp->useArea = x; });
        use->setObjectName(QStringLiteral("batchUseArea"));
        auto *x = spin(0, 100000, st.area.x(), QStringLiteral(" px"), [sp](int v) { sp->area.moveLeft(v); });
        auto *y = spin(0, 100000, st.area.y(), QStringLiteral(" px"), [sp](int v) { sp->area.moveTop(v); });
        auto *wd = spin(0, 100000, st.area.width(), QStringLiteral(" px"), [sp](int v) { sp->area.setWidth(v); });
        auto *h = spin(0, 100000, st.area.height(), QStringLiteral(" px"), [sp](int v) { sp->area.setHeight(v); });
        x->setObjectName(QStringLiteral("batchAreaX")); y->setObjectName(QStringLiteral("batchAreaY"));
        wd->setObjectName(QStringLiteral("batchAreaW")); h->setObjectName(QStringLiteral("batchAreaH"));
        auto *saved = new QPushButton(QStringLiteral("Use saved area"));
        saved->setObjectName(QStringLiteral("batchAreaSaved"));
        saved->setToolTip(QStringLiteral("The rectangle last used with the Export Area tool or Export Selection Area this session"));
        saved->setEnabled(!appState().lastExportArea.isEmpty());
        connect(saved, &QPushButton::clicked, this, [sp, use, x, y, wd, h, touch] {
            const QRect a = appState().lastExportArea;
            if (a.isEmpty()) return;
            sp->area = a;
            for (QSpinBox *b : {x, y, wd, h}) b->blockSignals(true);
            x->setValue(a.x()); y->setValue(a.y()); wd->setValue(a.width()); h->setValue(a.height());
            for (QSpinBox *b : {x, y, wd, h}) b->blockSignals(false);
            use->setChecked(true);
            touch();
        });
        auto en = [x, y, wd, h](bool on) { for (QSpinBox *b : {x, y, wd, h}) b->setEnabled(on); };
        connect(use, &QCheckBox::toggled, this, en);
        en(st.useArea);
        auto *r1 = new QHBoxLayout;
        r1->addWidget(use); r1->addWidget(saved); r1->addStretch();
        auto *r2 = new QHBoxLayout;
        r2->addWidget(new QLabel(QStringLiteral("X:"))); r2->addWidget(x);
        r2->addWidget(new QLabel(QStringLiteral("Y:"))); r2->addWidget(y);
        r2->addWidget(new QLabel(QStringLiteral("W:"))); r2->addWidget(wd);
        r2->addWidget(new QLabel(QStringLiteral("H:"))); r2->addWidget(h);
        f->addRow(QStringLiteral("Export area:"), r1);
        f->addRow(QString(), r2);
    };
    switch (st.type) {
    case Export: {
        auto *c = new QComboBox;
        c->setObjectName(QStringLiteral("batchPreset"));
        c->addItem(QStringLiteral("(Custom settings)"));
        c->addItems(exportpresets::names());
        const int pi = exportpresets::indexOf(st.preset);
        c->setCurrentIndex(st.preset.isEmpty() ? 0 : pi + 1);
        auto *custom = new QPushButton(QStringLiteral("Settings..."));
        custom->setEnabled(st.preset.isEmpty());
        connect(c, &QComboBox::currentIndexChanged, this, [sp, c, custom, touch](int i) {
            sp->preset = i <= 0 ? QString() : c->itemText(i);
            if (i <= 0 && sp->options.isEmpty()) sp->options = lwd::exportOptionsToJson(ExportOptions());
            custom->setEnabled(i <= 0);
            touch();
        });
        connect(custom, &QPushButton::clicked, this, [this, sp, touch] {
            QDialog d(this);
            d.setWindowTitle(QStringLiteral("Batch Export Settings"));
            auto *v = new QVBoxLayout(&d);
            auto *ow = new OptimizeWidget(&d);
            ow->setOptions(lwd::exportOptionsFromJson(sp->options));
            v->addWidget(ow);
            auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
            connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
            v->addWidget(bb);
            if (d.exec() == QDialog::Accepted) { sp->options = lwd::exportOptionsToJson(ow->options()); touch(); }
        });
        auto *h = new QHBoxLayout;
        h->addWidget(c, 1); h->addWidget(custom);
        f->addRow(QStringLiteral("Export preset:"), h);
        areaRows();
        break;
    }
    case Convert: {
        auto *c = new QComboBox;
        c->setObjectName(QStringLiteral("batchFormat"));
        c->addItem(QStringLiteral("%1 (%2)").arg(brand::docFilterName(), brand::docExt()), brand::docExtNoDot());
        for (Fmt fm : ImageIO::exportFormats())
            if (ImageIO::canWrite(fm)) c->addItem(QStringLiteral("%1 (.%2)").arg(ImageIO::formatName(fm), ImageIO::extension(fm)), ImageIO::formatKey(fm));
        c->setCurrentIndex(qMax(0, c->findData(st.format)));
        connect(c, &QComboBox::currentIndexChanged, this, [sp, c, touch](int) { sp->format = c->currentData().toString(); touch(); });
        f->addRow(QStringLiteral("Format:"), c);
        f->addRow(new QLabel(QStringLiteral("Uses each document's own optimize settings for that format.")));
        areaRows();
        break;
    }
    case Scale: {
        auto *mode = new QComboBox;
        mode->setObjectName(QStringLiteral("batchScaleMode"));
        mode->addItems({QStringLiteral("Scale to size"), QStringLiteral("Scale to percentage"), QStringLiteral("Scale to fit area")});
        mode->setCurrentIndex(st.scaleMode);
        auto *pct = new PopupSliderDoubleField;
        pct->setRange(1, 10000); pct->setSuffix(QStringLiteral(" %")); pct->setValue(st.percent);
        connect(pct, &QDoubleSpinBox::valueChanged, this, [sp, touch](double x) { sp->percent = x; touch(); });
        auto *wd = spin(0, 100000, st.width, QStringLiteral(" px"), [sp](int x) { sp->width = x; });
        auto *ht = spin(0, 100000, st.height, QStringLiteral(" px"), [sp](int x) { sp->height = x; });
        wd->setSpecialValueText(QStringLiteral("auto")); ht->setSpecialValueText(QStringLiteral("auto"));
        auto *shrink = check(QStringLiteral("Only shrink"), st.onlyShrink, [sp](bool x) { sp->onlyShrink = x; });
        auto en = [pct, wd, ht, shrink](int m) { pct->setEnabled(m == 1); wd->setEnabled(m != 1); ht->setEnabled(m != 1); shrink->setEnabled(m == 2); };
        connect(mode, &QComboBox::currentIndexChanged, this, [sp, en, touch](int m) { sp->scaleMode = m; en(m); touch(); });
        en(st.scaleMode);
        f->addRow(QStringLiteral("Mode:"), mode);
        f->addRow(QStringLiteral("Percent:"), pct);
        f->addRow(QStringLiteral("Width:"), wd);
        f->addRow(QStringLiteral("Height:"), ht);
        f->addRow(QString(), shrink);
        auto *rs = new QComboBox;
        rs->setObjectName(QStringLiteral("batchScaleResample"));
        rs->addItem(QStringLiteral("Preferences default"));
        rs->addItems(pixelscale::resampleNames());
        rs->setCurrentIndex(qBound(-1, st.resample, pixelscale::kResampleCount - 1) + 1);
        connect(rs, &QComboBox::currentIndexChanged, this, [sp, touch](int i) { sp->resample = i - 1; touch(); });
        f->addRow(QStringLiteral("Resample:"), rs);
        break;
    }
    case ReplaceColor: {
        auto *from = new ColorButton(nullptr, true);
        from->setColor(st.from);
        connect(from, &ColorButton::colorPicked, this, [sp, from, touch](const QColor &c) { sp->from = c; from->setColor(c); touch(); });
        auto *to = new ColorButton(nullptr, true);
        to->setColor(st.to);
        connect(to, &ColorButton::colorPicked, this, [sp, to, touch](const QColor &c) { sp->to = c; to->setColor(c); touch(); });
        f->addRow(QStringLiteral("Find color:"), from);
        f->addRow(QStringLiteral("Replace with:"), to);
        f->addRow(QStringLiteral("Tolerance:"), spin(0, 255, st.tolerance, QString(), [sp](int x) { sp->tolerance = x; }));
        f->addRow(new QLabel(QStringLiteral("Changes vector fills, strokes and gradient stops, bitmap pixels and the canvas color.")));
        break;
    }
    case ReplaceText:
        f->addRow(QStringLiteral("Find:"), line(st.find, [sp](const QString &x) { sp->find = x; }));
        f->addRow(QStringLiteral("Replace:"), line(st.replace, [sp](const QString &x) { sp->replace = x; }));
        f->addRow(QString(), check(QStringLiteral("Match case"), st.caseSensitive, [sp](bool x) { sp->caseSensitive = x; }));
        break;
    case Rename:
        f->addRow(QStringLiteral("Prefix:"), line(st.prefix, [sp](const QString &x) { sp->prefix = x; }));
        f->addRow(QStringLiteral("Name:"), [&] { auto *e = line(st.baseName, [sp](const QString &x) { sp->baseName = x; }); e->setPlaceholderText(QStringLiteral("(original name)")); return e; }());
        f->addRow(QStringLiteral("Suffix:"), line(st.suffix, [sp](const QString &x) { sp->suffix = x; }));
        f->addRow(QString(), check(QStringLiteral("Add a number"), st.numbering, [sp](bool x) { sp->numbering = x; }));
        f->addRow(QStringLiteral("Start at:"), spin(0, 1000000, st.start, QString(), [sp](int x) { sp->start = x; }));
        f->addRow(QStringLiteral("Digits:"), spin(1, 9, st.digits, QString(), [sp](int x) { sp->digits = x; }));
        break;
    case Command: {
        auto *c = new QComboBox;
        c->setObjectName(QStringLiteral("batchCommand"));
        const QStringList keys = commandKeys(), labels = commandLabels();
        for (int i = 0; i < keys.size(); ++i) c->addItem(labels.value(i, keys[i]), keys[i]);
        c->setCurrentIndex(qMax(0, c->findData(st.command)));
        connect(c, &QComboBox::currentIndexChanged, this, [sp, c, touch](int) { sp->command = c->currentData().toString(); touch(); });
        f->addRow(QStringLiteral("Command:"), c);
        break;
    }
    case Filters: {
        auto *list = new QListWidget;
        list->setObjectName(QStringLiteral("batchFilterList"));
        list->setMaximumHeight(110);
        auto fill = [list, sp] {
            list->clear();
            for (const LiveEffect &e : sp->effects) list->addItem(effectName(e.type));
        };
        fill();
        auto *addB = new QToolButton;
        addB->setText(QStringLiteral("Add Filter"));
        addB->setPopupMode(QToolButton::InstantPopup);
        addB->setToolButtonStyle(Qt::ToolButtonTextOnly);
        auto *menu = new QMenu(addB);
        QMap<QString, QMenu *> cats;
        for (EffectType t : allEffectTypes()) {
            const QString cat = effectCategory(t);
            if (!cats.contains(cat)) cats[cat] = menu->addMenu(cat);
            cats[cat]->addAction(effectName(t), this, [this, sp, t, fill, touch] {
                LiveEffect e = LiveEffect::make(t);
                if (effectHasSettings(t)) {
                    EffectDialog d(e, this);
                    d.setWindowTitle(effectName(t));
                    if (d.exec() != QDialog::Accepted) return;
                    e = d.effect();
                }
                sp->effects << e;
                fill(); touch();
            });
        }
        addB->setMenu(menu);
        auto *edit = new QPushButton(QStringLiteral("Edit..."));
        connect(edit, &QPushButton::clicked, this, [this, sp, list, fill] {
            const int r = list->currentRow();
            if (r < 0 || r >= sp->effects.size() || !effectHasSettings(sp->effects[r].type)) return;
            EffectDialog d(sp->effects[r], this);
            if (d.exec() == QDialog::Accepted) { sp->effects[r] = d.effect(); fill(); }
        });
        auto *rem = new QPushButton(QStringLiteral("Remove"));
        connect(rem, &QPushButton::clicked, this, [sp, list, fill, touch] {
            const int r = list->currentRow();
            if (r >= 0 && r < sp->effects.size()) { sp->effects.remove(r); fill(); touch(); }
        });
        auto *h = new QHBoxLayout;
        h->addWidget(addB); h->addWidget(edit); h->addWidget(rem); h->addStretch();
        f->addRow(list);
        f->addRow(h);
        f->addRow(new QLabel(QStringLiteral("Applied in order. Color adjustments are baked into bitmaps; other filters "
                                            "(and anything on vectors) are added as live effects.")));
        break;
    }
    default: break;
    }
}

Script BatchDialog::script() const { return m_s; }

void BatchDialog::setScript(const Script &s) {
    m_s = s;
    syncUi();
}

bool BatchDialog::saveScript(const QString &path, QString *err) const { return m_s.save(path, err); }

bool BatchDialog::loadScript(const QString &path, QString *err) {
    bool ok = false;
    const Script s = Script::load(path, err, &ok);
    if (!ok) return false;
    setScript(s);
    m_dir = QFileInfo(path).absolutePath();
    return true;
}

bool BatchDialog::validateCurrentPage() {
    const int id = currentId();
    QString problem;
    if (id == 0) {
        const int n = m_s.input == Script::OpenDocuments ? int(openDocNames().size()) : int(inputFiles(m_s).size());
        if (n == 0) problem = QStringLiteral("There are no files to process.");
    } else if (id == 1) {
        bool any = false;
        for (const Step &s : m_s.steps) any |= s.enabled;
        if (!any) problem = QStringLiteral("Add at least one step.");
    } else if (id == 2) {
        if (m_s.output == Script::ToFolder && m_s.outDir.trimmed().isEmpty()) problem = QStringLiteral("Choose an output folder.");
        if (m_s.input == Script::OpenDocuments && m_s.output != Script::ToFolder)
            for (const auto &p : m_open)
                if (p.second && p.second->filePath.isEmpty()) { problem = QStringLiteral("\"%1\" has not been saved yet: choose an output folder.").arg(p.first); break; }
    }
    if (problem.isEmpty()) return true;
    if (!testing()) QMessageBox::information(this, QStringLiteral("Batch Process"), problem);
    return false;
}

Result BatchDialog::runBatch() {
    QStringList files;
    std::function<Document *(const QString &)> openDoc;
    QHash<QString, QPointer<Document>> byName;
    if (m_s.input == Script::OpenDocuments) {
        for (const auto &p : m_open) {
            if (!p.second) continue;
            const QString name = p.second->filePath.isEmpty() ? p.first : p.second->filePath;
            files << name;
            byName[name] = p.second;
        }
        openDoc = [byName](const QString &n) -> Document * {
            const QPointer<Document> d = byName.value(n);
            return d ? lwd::deserialize(lwd::serialize(*d)) : nullptr;   // a copy: the open document is untouched
        };
    } else {
        files = inputFiles(m_s);
    }
    QProgressDialog *pd = nullptr;
    if (!testing()) {
        pd = new QProgressDialog(QStringLiteral("Processing..."), QStringLiteral("Cancel"), 0, int(files.size()), this);
        pd->setObjectName(QStringLiteral("batchProgress"));
        pd->setWindowTitle(QStringLiteral("Batch Process"));
        pd->setWindowModality(Qt::WindowModal);
        pd->setMinimumDuration(0);
        pd->setValue(0);
    }
    m_result = run(m_s, files, [pd](int i, int n, const QString &f) {
        if (!pd) return true;
        pd->setMaximum(n);
        pd->setValue(i);
        pd->setLabelText(QStringLiteral("Processing %1 (%2 of %3)...").arg(QFileInfo(f).fileName()).arg(i + 1).arg(n));
        QCoreApplication::processEvents();
        return !pd->wasCanceled();
    }, openDoc);
    delete pd;
    return m_result;
}

void BatchDialog::accept() {
    if (!validateCurrentPage()) return;
    const Result r = runBatch();
    if (!testing()) {
        QString msg = QStringLiteral("%1 file(s) processed, %2 failed%3. %4 file(s) written.")
                          .arg(r.processed).arg(r.failed).arg(r.cancelled ? QStringLiteral(" (cancelled)") : QString()).arg(r.written.size());
        QMessageBox box(r.failed ? QMessageBox::Warning : QMessageBox::Information, QStringLiteral("Batch Process"), msg, QMessageBox::Ok, this);
        box.setObjectName(QStringLiteral("batchDone"));
        QStringList details = r.errors;
        if (!r.log.isEmpty()) details << QString() << r.log;
        if (!details.isEmpty()) box.setDetailedText(details.join(QLatin1Char('\n')));
        box.exec();
    }
    QWizard::accept();
}
